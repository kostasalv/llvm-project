//===- bolt/Passes/PatchEntries.cpp - Pass for patching function entries --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the PatchEntries class that is used for patching the
// original function entry points. This ensures that only the new/optimized code
// executes and that the old code is never used. This is necessary due to
// current BOLT limitations of not being able to duplicate all function's
// associated metadata (e.g., .eh_frame, exception ranges, debug info,
// jump-tables).
//
// NOTE: A successful run of 'scanExternalRefs' can relax this requirement as
// it also ensures that old code is never executed.
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/PatchEntries.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "bolt/Utils/NameResolver.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CommandLine.h"

namespace opts {
extern llvm::cl::OptionCategory BoltCategory;
extern llvm::cl::opt<unsigned> Verbosity;
} // namespace opts

namespace llvm {
namespace bolt {

Error PatchEntries::runOnFunctions(BinaryContext &BC) {
  if (!opts::ForcePatch) {
    // Mark the binary for patching if we did not create external references
    // for original code in any of functions we are not going to emit.
    auto needsPatching = [&](const BinaryFunction &BF) {
      // FIXME: keep compatibility for NFC testing.
      if (BF.isFolded())
        return false;

      // Patching is always needed if explicitly requested.
      if (BF.needsPatch())
        return true;

      return !BC.shouldEmit(BF) && !BF.hasExternalRefRelocations();
    };

    if (!llvm::any_of(llvm::make_second_range(BC.getBinaryFunctions()),
                      needsPatching))
      return Error::success();
  }

  if (opts::Verbosity >= 1)
    BC.outs() << "BOLT-INFO: patching entries in original code\n";

  // PPC64 ELFv2 needs two patch shapes, so its sizes are computed separately
  // (and not cached in a static, which would leak across targets): a full
  // absolute long tail call, and a single unconditional branch used to forward
  // the global entry point to the local entry point.
  size_t LongPatchSize = 0;
  size_t BranchSize = 0;
  if (BC.isPPC64()) {
    InstructionListType Seq;
    BC.MIB->createLongTailCall(Seq, BC.Ctx->createTempSymbol(), BC.Ctx.get());
    LongPatchSize = BC.computeCodeSize(Seq.begin(), Seq.end());

    InstructionListType BranchSeq(1);
    BC.MIB->createUncondBranch(BranchSeq.front(), BC.Ctx->createTempSymbol(),
                               BC.Ctx.get());
    BranchSize = BC.computeCodeSize(BranchSeq.begin(), BranchSeq.end());
  }
  static size_t PatchSize = 0;
  if (!PatchSize && !BC.isPPC64()) {
    InstructionListType Seq;
    BC.MIB->createLongTailCall(Seq, BC.Ctx->createTempSymbol(), BC.Ctx.get());
    PatchSize = BC.computeCodeSize(Seq.begin(), Seq.end());
  }
  static size_t FillerSize = 0;
  if (BC.isX86() && FillerSize == 0) {
    std::array<MCInst, 1> Seq;
    BC.MIB->createBreakpoint(Seq[0]);
    FillerSize = BC.computeCodeSize(Seq.begin(), Seq.end());
  }

  for (auto &BFI : BC.getBinaryFunctions()) {
    BinaryFunction &Function = BFI.second;

    // Patch original code only for functions that will be emitted.
    if (!BC.shouldEmit(Function))
      continue;

    // Check if we can skip patching the function.
    if (!opts::ForcePatch && !Function.hasEHRanges() &&
        !Function.needsPatch() && Function.getSize() < PatchThreshold)
      continue;

    // List of patches for function entries. We either successfully patch
    // all entries or, if we cannot patch one or more, do no patch any and
    // mark the function as ignorable.
    std::vector<Patch> PendingPatches;

    uint64_t NextValidByte = 0; // offset of the byte past the last patch
    bool Success = Function.forEachEntryPoint([&](uint64_t Offset,
                                                  const MCSymbol *Symbol) {
      // PPC64 ELFv2: the local entry point (offset == getPPC64LocalEntryOffset,
      // typically 8) is a distinct, independently-reachable ABI entry — callers
      // that already have r2 set up branch directly to it, skipping the
      // 2-instruction global-entry TOC preamble at offset 0. It is NOT
      // redundant with the offset-0 patch: the global-entry redirect written
      // at offset 0 is a 7-instruction/28-byte absolute long-tail-call (see
      // createLongTailCall / PatchSize below), which physically spans bytes
      // [0, 28) and so overwrites the local entry's original bytes (usually
      // at offset 8) with the *middle* of that instruction sequence — not a
      // valid branch target. A caller that jumps directly to the original
      // local entry then lands mid-stub (e.g. on the `rldicr` that assumes
      // r12's high bits were already loaded by the preceding `lis`/`ori`,
      // which never executed), producing a garbage absolute address and a
      // wild branch at runtime.
      //
      // This can NOT be left to the generic overlap check below (Offset <
      // NextValidByte): that check only fires for offsets that
      // forEachEntryPoint() actually calls back with, i.e. offset 0 plus
      // whatever is registered in Function.Labels/SecondaryEntryPoints via
      // isMultiEntry(). The local entry offset is deliberately never
      // registered there (see RewriteInstance::handleRelocation's
      // IsPPC64LocalEntry handling, which routes func+LEP references to
      // getOrCreateLocalLabel() instead of addEntryPointAtOffset() precisely
      // to avoid it being treated as a CFG/BOLT entry point). So for a
      // function whose only extra entry is its ABI local entry point (i.e.
      // not independently multi-entry), forEachEntryPoint() calls back
      // exactly once, for offset 0 — the check below never sees the
      // conflicting offset and the overlap goes undetected, silently
      // producing the mid-stub wild branch described above. Confirmed via
      // gdb/objdump on a BOLT-rewritten llc crashing with CTR =
      // 0xf7649a5013a72480: the low 32 bits (0x13a72480) are exactly
      // llvm::cl::ValuesClass::apply<...>'s relocated address (the
      // `oris`/`ori` pair at the stub's offsets 12/16 that DID execute), while
      // the high 32 bits are garbage left in r12 from skipping the stub's
      // first `lis`/`ori` (offsets 0/4) — because the caller (a kept-in-place
      // ELFv2 `.long_branch.` thunk with its original R_PPC64_REL24 Func+8
      // relocation) branched straight to Func+8, landing on the `rldicr` at
      // stub offset 8.
      //
      // So the local entry point has to be handled explicitly here rather than
      // left to forEachEntryPoint(). Note that essentially every global ELFv2
      // function has a local entry point, so simply refusing to patch these
      // would abandon almost every function in the binary.
      //
      // Split the redirect in two instead of giving up:
      //
      //   offset 0    b <local entry patch>                  <- global entry
      //   offset 4    (left untouched; never a branch target, see below)
      //   offset LEP  lis8 r12, ...; mtctr r12; bctr         <- local entry
      //
      // Both ABI entries end up at the *global* entry point of the new
      // function, which rebuilds r2 from r12 itself, so neither entry depends
      // on the TOC base the caller happened to hold. createLongTailCall()
      // materializes the target into r12, which is exactly what the ELFv2 ABI
      // requires of a caller entering a global entry point.
      //
      // The forwarding branch's displacement is the local entry offset - at
      // most 64 bytes - so it is encodable without knowing anything about the
      // output layout. That matters: this pass runs inside
      // runOptimizationPasses(), long before emitAndLink() assigns output
      // addresses, so Function.getOutputAddress() is still 0 here and any
      // range decision based on it would be meaningless.
      //
      // Leaving offset 4 alone is safe. Per the ELFv2 ABI (Section 2.3.2.1,
      // Function Prologue): "Addresses between the global and local entry
      // points must not be branch targets, either for function entry or
      // referenced by program logic of the function." Nothing can enter there,
      // and those bytes are dead once offset 0 is overwritten.
      uint64_t EntryPatchSize = BC.isPPC64() ? LongPatchSize : PatchSize;
      const uint8_t LEPOffset =
          BC.isPPC64() ? Function.getPPC64LocalEntryOffset() : 0;
      const bool SplitEntry = LEPOffset && Offset == 0;
      if (SplitEntry)
        EntryPatchSize = LEPOffset + LongPatchSize;

      if (Offset < NextValidByte) {
        if (opts::Verbosity >= 1)
          BC.outs() << "BOLT-INFO: unable to patch entry point in " << Function
                    << " at offset 0x" << Twine::utohexstr(Offset) << '\n';
        return false;
      }

      NextValidByte = Offset + EntryPatchSize;
      if (NextValidByte > Function.getMaxSize()) {
        if (opts::Verbosity >= 1)
          BC.outs() << "BOLT-INFO: function " << Function
                    << " too small to patch its entry point\n";
        return false;
      }

      const uint64_t PatchAddress = Function.getAddress() + Offset;

      if (SplitEntry) {
        // Emit the local entry patch first: the global entry point's forwarding
        // branch resolves its target from the patch function created for it.
        PendingPatches.emplace_back(
            Patch{Symbol, PatchAddress + LEPOffset, LongPatchSize});
        Patch Forward{Symbol, PatchAddress, BranchSize};
        Forward.DirectBranch = true;
        Forward.BranchToPatch = static_cast<int>(PendingPatches.size()) - 1;
        PendingPatches.emplace_back(Forward);
        return true;
      }

      Patch P{Symbol, PatchAddress, EntryPatchSize};

      if (BC.isX86()) {
        uint64_t OverwriteLength =
            Function.getInstructionSequenceLength(Offset, EntryPatchSize);
        P.PaddingAfter = OverwriteLength - EntryPatchSize;
        assert(PendingPatches.empty() ||
               (PendingPatches.back().Address + PendingPatches.back().Size +
                    PendingPatches.back().PaddingAfter <=
                PatchAddress) &&
                   "Entry point cannot overlap with instruction stream of "
                   "previous entrypoint.");
      }

      PendingPatches.emplace_back(P);
      return true;
    });

    if (!Success) {
      // If the original function entries cannot be patched, then we cannot
      // safely emit new function body.
      BC.errs() << "BOLT-WARNING: failed to patch entries in " << Function
                << ". The function will not be optimized\n";
      Function.setIgnored();
      continue;
    }

    // Patch functions in creation order, so that a patch forwarding to another
    // one (PPC64 global -> local entry point) can name its target's symbol.
    std::vector<BinaryFunction *> PatchFunctions(PendingPatches.size(), nullptr);

    for (unsigned Idx = 0; Idx < PendingPatches.size(); ++Idx) {
      Patch &Patch = PendingPatches[Idx];

      const MCSymbol *TargetSymbol = Patch.Symbol;
      std::string PatchName =
          NameResolver::append(Patch.Symbol->getName(), ".org.0");
      if (Patch.BranchToPatch >= 0) {
        assert(PatchFunctions[Patch.BranchToPatch] &&
               "forwarding patch must be created after its target");
        TargetSymbol = PatchFunctions[Patch.BranchToPatch]->getSymbol();
        PatchName = NameResolver::append(Patch.Symbol->getName(), ".org.gep");
      }

      // Add instruction patch to the binary.
      InstructionListType Instructions;
      if (Patch.DirectBranch) {
        BC.MIB->createUncondBranch(Instructions.emplace_back(), TargetSymbol,
                                   BC.Ctx.get());
      } else {
        BC.MIB->createLongTailCall(Instructions, TargetSymbol, BC.Ctx.get());
      }

      if (BC.isX86()) {
        assert(Patch.PaddingAfter % FillerSize == 0 &&
               "Padding must be multiple of filler size.");
        llvm::MCInst Inst;
        BC.MIB->createBreakpoint(Inst);
        Instructions.resize(
            Instructions.size() + Patch.PaddingAfter / FillerSize, Inst);
      }

      BinaryFunction *PatchFunction =
          BC.createInstructionPatch(Patch.Address, Instructions, PatchName);
      PatchFunctions[Idx] = PatchFunction;
      if (BC.usesBTI())
        BC.MIB->applyBTIFixupToSymbol(BC, Patch.Symbol,
                                      *(Instructions.end() - 1));

      // Verify the size requirements.
      uint64_t HotSize, ColdSize;
      std::tie(HotSize, ColdSize) = BC.calculateEmittedSize(*PatchFunction);
      assert(!ColdSize && "unexpected cold code");
      assert(HotSize <= Patch.Size + Patch.PaddingAfter &&
             "max patch size exceeded");
    }
  }
  return Error::success();
}

} // end namespace bolt
} // end namespace llvm
