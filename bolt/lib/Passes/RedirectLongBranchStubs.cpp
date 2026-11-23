//===- bolt/Passes/RedirectLongBranchStubs.cpp ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the RedirectLongBranchStubs class.
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/RedirectLongBranchStubs.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinaryFunction.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include <optional>

#define DEBUG_TYPE "redirect-long-branch-stubs"

using namespace llvm;
using namespace bolt;

namespace {

bool isLongBranchStub(StringRef Name) { return Name.contains(".long_branch."); }

/// The stub is one plain `b` (opcode 18, AA=0, LK=0) and its symbol has size
/// 0, so the instruction is read from the input at the address of the stub
/// symbol the call names. Return the address it branches to, or nothing if the
/// word is not a plain `b`.
std::optional<uint64_t> getStubTarget(const BinaryContext &BC,
                                      uint64_t StubAddress) {
  ErrorOr<uint64_t> Word = BC.getUnsignedValueAtAddress(StubAddress, 4);
  if (!Word || (*Word >> 26) != 18 || (*Word & 3))
    return std::nullopt;
  return StubAddress + SignExtend64<26>(*Word & 0x3fffffc);
}

} // namespace

Error RedirectLongBranchStubs::runOnFunctions(BinaryContext &BC) {
  uint64_t NumRedirected = 0, NumNotMoved = 0, NumIgnored = 0, NumEntryRule = 0,
           NumNoLabel = 0, NumNotBranch = 0, NumNotRecognised = 0,
           NumBadStart = 0;

  for (auto &BFI : BC.getBinaryFunctions()) {
    BinaryFunction &Caller = BFI.second;
    if (!shouldOptimize(Caller))
      continue;

    for (BinaryBasicBlock &BB : Caller) {
      for (MCInst &Inst : BB) {
        if (!BC.MIB->isCall(Inst))
          continue;
        const MCSymbol *CalleeSym = BC.MIB->getTargetSymbol(Inst);
        // GNU ld puts many stubs in one group that is covered by one FDE, and
        // BOLT makes one function of the group. Only the first stub symbol is
        // the start of that function, so the call's own symbol is what names
        // the stub: its name says it is a `.long_branch.` stub, and its own
        // address is where the stub's branch is read.
        const bool StubName =
            CalleeSym && isLongBranchStub(CalleeSym->getName());
        ErrorOr<uint64_t> StubAddr =
            StubName ? BC.getSymbolValue(*CalleeSym)
                     : ErrorOr<uint64_t>(std::errc::bad_address);
        const BinaryFunction *Stub =
            StubAddr
                ? BC.getBinaryFunctionContainingAddress(*StubAddr,
                                                        /*CheckPastEnd=*/false,
                                                        /*UseMaxSize=*/true)
                : nullptr;
        // A valid stub start is 4-byte aligned and the whole instruction word
        // is inside the stub's function (the function may be a group of stubs).
        const bool ValidStart =
            Stub && !(*StubAddr & 3) &&
            Stub->containsAddress(*StubAddr + 3, /*UseMaxSize=*/true);
        LLVM_DEBUG({
          dbgs() << "BOLT-DEBUG: long_branch: call in " << Caller << " to "
                 << (CalleeSym ? CalleeSym->getName() : "<no symbol>")
                 << ", callee " << (Stub ? Stub->getPrintName() : "-")
                 << ", stub "
                 << (!StubName     ? "no"
                     : !StubAddr   ? "unresolved"
                     : !ValidStart ? "bad-start"
                                   : "yes")
                 << ", insn ";
          BC.printInstruction(dbgs(), Inst);
        });
        if (!StubName)
          continue;
        if (!StubAddr) {
          // The call names a `.long_branch.` symbol that has no address.
          ++NumNotRecognised;
          continue;
        }
        if (!ValidStart) {
          ++NumBadStart;
          continue;
        }
        std::optional<uint64_t> Final = getStubTarget(BC, *StubAddr);
        if (!Final) {
          ++NumNotBranch;
          continue;
        }

        BinaryFunction *Target = BC.getBinaryFunctionContainingAddress(*Final);
        // An ignored function keeps its old body and is not emitted into the
        // new text. Whoever ignored it (--skip-funcs, or PatchEntries when the
        // function is too small to hold the redirect it must leave at its old
        // address), the call has nowhere to go in the new text, so it is
        // counted on its own. Moved: emitted into the new text, with a CFG.
        const bool Ignored = Target && Target->isIgnored();
        const bool Moved =
            Target && !Ignored && BC.shouldEmit(*Target) && Target->hasCFG();
        LLVM_DEBUG(dbgs() << "BOLT-DEBUG: long_branch:   stub at 0x"
                          << Twine::utohexstr(*StubAddr) << " target 0x"
                          << Twine::utohexstr(*Final) << " in "
                          << (Target ? Target->getPrintName() : "-")
                          << ", moved "
                          << (Moved     ? "yes"
                              : Ignored ? "ignored"
                                        : "no")
                          << '\n');
        if (Ignored) {
          ++NumIgnored;
          continue;
        }
        if (!Moved) {
          ++NumNotMoved;
          continue;
        }

        // Keep the entry the stub reaches, and only if it is the one the ABI
        // gives callers that share the TOC: offset 0 when the function has no
        // local entry, else the local entry.
        const uint64_t Offset = *Final - Target->getAddress();
        if (Offset != Target->getPPC64LocalEntryOffset()) {
          ++NumEntryRule;
          continue;
        }

        // Offset 0 is the function symbol. For the local entry, call the
        // existing label through a secondary entry symbol: a call is resolved
        // by symbol (LongJmp, the emitter and the linker), and a function-local
        // label of another function is not such a symbol. addEntryPoint() only
        // names an existing block, so no new label is made.
        const MCSymbol *Label = Target->getSymbol();
        if (Offset) {
          const MCSymbol *Existing = Target->getLabelAtOffset(Offset);
          BinaryBasicBlock *TargetBB = Target->getBasicBlockAtOffset(Offset);
          if (!Existing || !TargetBB || TargetBB->getLabel() != Existing) {
            ++NumNoLabel;
            continue;
          }
          Label = Target->addEntryPoint(*TargetBB);
        }

        BC.MIB->replaceBranchTarget(Inst, Label, BC.Ctx.get());
        ++NumRedirected;
      }
    }
  }

  if (NumRedirected || NumNotMoved || NumIgnored || NumEntryRule ||
      NumNoLabel || NumNotBranch || NumNotRecognised || NumBadStart)
    BC.outs() << "BOLT-INFO: long_branch stubs: " << NumRedirected
              << " calls redirected, skipped " << NumNotMoved
              << " (target not moved), " << NumIgnored << " (target ignored), "
              << NumEntryRule << " (entry rule), " << NumNoLabel
              << " (no-label-at-target), " << NumNotBranch
              << " (not a plain b), " << NumNotRecognised
              << " (stub not recognised), " << NumBadStart
              << " (not a valid stub start)\n";
  return Error::success();
}
