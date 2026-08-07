//===- bolt/Rewrite/JITLinkLinker.cpp - BOLTLinker using JITLink ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Rewrite/JITLinkLinker.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinaryData.h"
#include "bolt/Core/BinaryFunction.h"
#include "bolt/Core/BinarySection.h"
#include "llvm/ADT/Twine.h"
#include "llvm/ExecutionEngine/JITLink/ELF_riscv.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/JITLink/ppc64.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorSymbolDef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"

#define DEBUG_TYPE "bolt"

namespace llvm {
namespace bolt {

namespace {

bool hasSymbols(const jitlink::Block &B) {
  return llvm::any_of(B.getSection().symbols(),
                      [&B](const auto &S) { return &S->getBlock() == &B; });
}

/// Liveness in JITLink is based on symbols so sections that do not contain
/// any symbols will always be pruned. This pass adds anonymous symbols to
/// needed sections to prevent pruning.
Error markSectionsLive(jitlink::LinkGraph &G) {
  for (auto &Section : G.sections()) {
    // We only need allocatable sections.
    if (Section.getMemLifetime() == orc::MemLifetime::NoAlloc)
      continue;

    // Skip empty sections.
    if (JITLinkLinker::sectionSize(Section) == 0)
      continue;

    for (auto *Block : Section.blocks()) {
      // No need to add symbols if it already has some.
      if (hasSymbols(*Block))
        continue;

      G.addAnonymousSymbol(*Block, /*Offset=*/0, /*Size=*/0,
                           /*IsCallable=*/false, /*IsLive=*/true);
    }
  }

  return jitlink::markAllSymbolsLive(G);
}

void reassignSectionAddress(jitlink::LinkGraph &LG,
                            const BinarySection &BinSection, uint64_t Address) {
  auto *JLSection = LG.findSectionByName(BinSection.getSectionID());
  assert(JLSection && "cannot find section in LinkGraph");

  auto BlockAddress = Address;
  for (auto *Block : JITLinkLinker::orderedBlocks(*JLSection)) {
    // FIXME it would seem to make sense to align here. However, in
    // non-relocation mode, we simply use the original address of functions
    // which might not be aligned with the minimum alignment used by
    // BinaryFunction (2). Example failing test when aligning:
    // bolt/test/X86/addr32.s
    Block->setAddress(orc::ExecutorAddr(BlockAddress));
    BlockAddress += Block->getSize();
  }
}

/// PPC64 ELFv2: downgrade CallBranchDeltaRestoreTOC edges to CallBranchDelta
/// when the post-call slot does not contain a NOP (0x60000000).
///
/// JITLink's PLTTableManager unconditionally transforms every external
/// RequestCall edge to CallBranchDeltaRestoreTOC, which instructs applyFixup
/// to overwrite the 4 bytes after the `bl` with `ld r2, 24(r1)` (TOC
/// restore).  This is only valid when BOLT (or the original linker) left a
/// NOP placeholder in that slot.  For tail calls, local calls or non-simple
/// functions emitted as raw bytes there is no such placeholder, so the
/// rewrite would corrupt live code.  Demoting the edge to CallBranchDelta
/// patches only the branch offset and leaves the post-call slot intact.
template <llvm::endianness Endianness>
Error ppc64DowngradeRestoreTOCIfNoNOP(jitlink::LinkGraph &G) {
  constexpr uint32_t NOP = 0x60000000u;
  for (auto *Block : G.blocks()) {
    for (auto &Edge : Block->edges()) {
      if (Edge.getKind() != jitlink::ppc64::CallBranchDeltaRestoreTOC)
        continue;
      // The block's content is mutable at this point (pre-fixup).
      auto Content = Block->getContent();
      size_t Off = Edge.getOffset();
      // Guard: need at least 8 bytes (bl + slot).
      if (Off + 8 > Content.size())
        continue;
      uint32_t Slot =
          support::endian::read32<Endianness>(Content.data() + Off + 4);
      if (Slot != NOP) {
        LLVM_DEBUG(dbgs() << "BOLT PPC64: demoting CallBranchDeltaRestoreTOC"
                          << " to CallBranchDelta at offset 0x"
                          << Twine::utohexstr(Off) << " in block 0x"
                          << Twine::utohexstr(Block->getAddress().getValue())
                          << " (slot=0x" << Twine::utohexstr(Slot) << ")\n");
        Edge.setKind(jitlink::ppc64::CallBranchDelta);
      }
    }
  }
  return Error::success();
}

} // anonymous namespace

struct JITLinkLinker::Context : jitlink::JITLinkContext {
  JITLinkLinker &Linker;
  JITLinkLinker::SectionsMapper MapSections;
  // Set in modifyPassConfig; used in lookup() for PPC64-specific diagnostics.
  bool IsPPC64 = false;

  Context(JITLinkLinker &Linker, JITLinkLinker::SectionsMapper MapSections)
      : JITLinkContext(&Linker.Dylib), Linker(Linker),
        MapSections(MapSections) {}

  jitlink::JITLinkMemoryManager &getMemoryManager() override {
    return *Linker.MM;
  }

  bool shouldAddDefaultTargetPasses(const Triple &TT) const override {
    // The default passes manipulate DWARF sections in a way incompatible with
    // BOLT.
    // TODO check if we can actually use these passes to remove some of the
    // DWARF manipulation done in BOLT.
    return false;
  }

  Error modifyPassConfig(jitlink::LinkGraph &G,
                         jitlink::PassConfiguration &Config) override {
    // Record the target so lookup() can emit PPC64-specific diagnostics.
    IsPPC64 = G.getTargetTriple().isPPC64();

    Config.PrePrunePasses.push_back(markSectionsLive);
    Config.PostAllocationPasses.push_back([this](auto &G) {
      MapSections([&G](const BinarySection &Section, uint64_t Address) {
        reassignSectionAddress(G, Section, Address);
      });
      return Error::success();
    });

    // PPC64 ELFv2: BOLT-rewritten functions hardcode r2 to the original
    // binary's TOC base.  JITLink's default PLT stubs (LongBranchSaveR2) use
    // TOC-relative (r2-relative) addressing to load the callee address from
    // $__GOT, but $__GOT is allocated far from the original r2, so the
    // TOCDelta fixups would land outside $__GOT and read garbage / zero.
    //
    // Fix: use LongBranchNoTOC stubs instead.  These stubs are fully
    // PC-relative (they use bcl+mflr to get their own address, then
    // addis/ld relative to that), so they work correctly regardless of r2.
    // We achieve this by converting every RequestCall edge on an external
    // target to RequestCallNoTOC before buildTables_ELF_ppc64 runs.
    //
    // We also demote CallBranchDeltaRestoreTOC → CallBranchDelta when the
    // post-call slot is not a NOP, to prevent applyFixup from overwriting
    // live code in tail calls and non-simple functions emitted as raw bytes.
    if (G.getTargetTriple().isPPC64()) {
      bool IsLE = G.getTargetTriple().getArch() == Triple::ppc64le;

      // Pass 1 (PrePrune, runs before buildTables_ELF_ppc64 which is a
      // PostPrunePass): Convert all external RequestCall edges to
      // RequestCallNoTOC so buildTables_ELF_ppc64 generates PC-relative
      // LongBranchNoTOC stubs instead of TOC-relative LongBranchSaveR2 stubs.
      //
      // $__GOT is allocated far from the original TOC base (r2), so
      // LongBranchSaveR2 stubs would compute wrong TOCDelta offsets and crash.
      // LongBranchNoTOC stubs are fully PC-relative and always correct.
      //
      // For BOLT's plt_call/plt_branch stubs (original binary's PLT stubs),
      // lookup() resolves their names to the correct stub addresses, and the
      // LongBranchNoTOC wrapper bctr's into them with r2 = original TOC base,
      // so they execute correctly.
      Config.PrePrunePasses.push_back([](jitlink::LinkGraph &G) -> Error {
        for (auto *Block : G.blocks()) {
          for (auto &Edge : Block->edges()) {
            if (Edge.getKind() != jitlink::ppc64::RequestCall)
              continue;

            // Classify the edge target to decide which stub kind to use.
            //
            // NOTE: ELF_ppc64.cpp defers externality determination to
            // PostPrunePass ("Determining a target is external or not is
            // deferred in PostPrunePass").  We therefore check by NAME for
            // BOLT's plt_call/plt_branch stubs rather than relying on
            // isExternal(), which may return false at PrePrune time for these
            // symbols even though buildTables_ELF_ppc64 will later treat them
            // as external.
            bool IsPltStub = false;
            if (Edge.getTarget().hasName()) {
              StringRef TgtName = *Edge.getTarget().getName();
              IsPltStub = TgtName.contains(".plt_call.") ||
                          TgtName.contains(".plt_branch.");
            }

            if (!IsPltStub && !Edge.getTarget().isExternal()) {
              // Local, non-plt target: leave as RequestCall.
              // buildTables_ELF_ppc64 will convert it to CallBranchDelta
              // (direct branch, no stub) — correct and efficient.
              continue;
            }

            // Either a plt_call/plt_branch stub, or a genuinely external
            // target.  Convert to RequestCallNoTOC so buildTables_ELF_ppc64
            // generates a LongBranchNoTOC stub (PC-relative, works regardless
            // of r2).  LongBranchSaveR2 stubs are broken here because $__GOT
            // is allocated far from the original binary's TOC base.
            //
            // For plt_call/plt_branch stubs, lookup() will redirect the name
            // to the real symbol address (or the stub's own address as a
            // fallback).  LongBranchNoTOC then bctr's to the target with
            // r12 = target address, satisfying the ELFv2 calling convention.
            LLVM_DEBUG({
              if (Edge.getTarget().hasName())
                dbgs() << "BOLT PPC64: converting RequestCall → "
                          "RequestCallNoTOC for "
                       << (IsPltStub ? "plt stub " : "external ")
                       << *Edge.getTarget().getName() << "\n";
            });
            Edge.setKind(jitlink::ppc64::RequestCallNoTOC);
          }
        }
        return Error::success();
      });

      // Pass 2 (PostPrune, runs before buildTables_ELF_ppc64):
      // Demote CallBranchDeltaRestoreTOC → CallBranchDelta when the
      // post-call slot is not a NOP.
      if (IsLE)
        Config.PostPrunePasses.push_back(
            ppc64DowngradeRestoreTOCIfNoNOP<llvm::endianness::little>);
      else
        Config.PostPrunePasses.push_back(
            ppc64DowngradeRestoreTOCIfNoNOP<llvm::endianness::big>);
    }

    if (G.getTargetTriple().isRISCV()) {
      Config.PostAllocationPasses.push_back(
          jitlink::createRelaxationPass_ELF_riscv());
    }

    return Error::success();
  }

  void notifyFailed(Error Err) override {
    errs() << "BOLT-ERROR: JITLink failed: " << Err << '\n';
    exit(1);
  }

  void
  lookup(const LookupMap &Symbols,
         std::unique_ptr<jitlink::JITLinkAsyncLookupContinuation> LC) override {
    jitlink::AsyncLookupResult AllResults;

    for (const auto &Symbol : Symbols) {
      std::string SymName = (*Symbol.first).str();
      LLVM_DEBUG(dbgs() << "BOLT: looking for " << SymName << "\n");

      // PPC64 ELFv2: BOLT's object files reference the original binary's PLT
      // call/branch stubs by their BOLT-internal names, e.g.:
      //   "0000ba05.plt_call.sqrt@@GLIBC_2.17/1"
      //   "0000ba05.plt_branch.2c8e7:13/1"
      // The PrePrune pass converts their RequestCall edges to RequestCallNoTOC
      // so JITLink builds LongBranchNoTOC stubs for them.  Here we make
      // LongBranchNoTOC as useful as possible by resolving the stub name to
      // the actual target symbol address (when known), bypassing the original
      // PLT stub entirely.  This avoids any TOC-relative load in the stub.
      //
      // For anonymous stubs (numeric names) or names we can't resolve, we fall
      // through to the normal lookup path which finds the original stub's
      // address via getBinaryDataByName.  The LongBranchNoTOC wrapper then
      // bctr's into the original stub with r2 = original TOC base, and the
      // stub's own TOC-relative load (ld r12,N(r2)) works correctly.
      if (IsPPC64) {
        StringRef SN(SymName);
        for (StringRef Marker : {StringRef(".plt_call."), StringRef(".plt_branch.")}) {
          auto Pos = SN.find(Marker);
          if (Pos == StringRef::npos)
            continue;
          // Extract the real symbol name after the marker.
          StringRef RealName = SN.drop_front(Pos + Marker.size());
          // Strip trailing "/N" version suffix if present.
          if (auto Slash = RealName.rfind('/'); Slash != StringRef::npos)
            RealName = RealName.take_front(Slash);
          // Skip anonymous numeric stubs (no real symbol name).
          bool IsAnonymous = RealName.empty() ||
                             (RealName[0] >= '0' && RealName[0] <= '9');
          if (!IsAnonymous) {
            std::string RealNameStr = RealName.str();
            LLVM_DEBUG(dbgs() << "BOLT PPC64: redirecting PLT stub lookup "
                               << SymName << " -> " << RealNameStr << "\n");
            // Try to resolve the real symbol name (includes local symbols that
            // getBinaryDataByName misses because it only covers global symbols).
            if (auto SymInfo = Linker.lookupSymbolInfo(RealNameStr)) {
              uint64_t Addr = SymInfo->Address;
              // If this resolved to a PLT thunk that has a safe BOLT stub,
              // redirect to the stub instead.
              auto ThunkIt = Linker.PLTThunkToStub.find(Addr);
              if (ThunkIt != Linker.PLTThunkToStub.end()) {
                errs() << "BOLT-PPC64: " << SymName
                       << " -> PLT thunk 0x" << Twine::utohexstr(Addr)
                       << " redirected to BOLT stub 0x"
                       << Twine::utohexstr(ThunkIt->second) << "\n";
                AllResults[Symbol.first] = orc::ExecutorSymbolDef(
                    orc::ExecutorAddr(ThunkIt->second), JITSymbolFlags());
              } else {
                LLVM_DEBUG(dbgs() << "BOLT-PPC64-LOOKUP: " << SymName
                                  << " -> (plt-redirect) symtab 0x"
                                  << Twine::utohexstr(Addr) << "\n");
                AllResults[Symbol.first] = orc::ExecutorSymbolDef(
                    orc::ExecutorAddr(Addr), JITSymbolFlags());
              }
              goto next_symbol;
            }
            if (const BinaryData *I =
                    Linker.BC.getBinaryDataByName(RealNameStr)) {
              uint64_t Address = I->isMoved() && !I->isJumpTable()
                                     ? I->getOutputAddress()
                                     : I->getAddress();
              LLVM_DEBUG(dbgs() << "BOLT-PPC64-LOOKUP: " << SymName
                                << " -> (plt-redirect) BinaryData 0x"
                                << Twine::utohexstr(Address) << "\n");
              AllResults[Symbol.first] = orc::ExecutorSymbolDef(
                  orc::ExecutorAddr(Address), JITSymbolFlags());
              goto next_symbol;
            }
            // For versioned symbols like "realloc@@GLIBC_2.17", also try
            // stripping the "@@VERSION" suffix and looking up the bare name.
            if (auto AtAt = RealName.find("@@"); AtAt != StringRef::npos) {
              std::string BareNameStr = RealName.take_front(AtAt).str();
              LLVM_DEBUG(dbgs() << "BOLT PPC64: trying bare name "
                                 << BareNameStr << "\n");
              if (auto SymInfo = Linker.lookupSymbolInfo(BareNameStr)) {
                uint64_t Addr = SymInfo->Address;
                auto ThunkIt = Linker.PLTThunkToStub.find(Addr);
                if (ThunkIt != Linker.PLTThunkToStub.end()) {
                  errs() << "BOLT-PPC64: " << SymName
                         << " -> PLT thunk 0x" << Twine::utohexstr(Addr)
                         << " redirected to BOLT stub 0x"
                         << Twine::utohexstr(ThunkIt->second) << "\n";
                  AllResults[Symbol.first] = orc::ExecutorSymbolDef(
                      orc::ExecutorAddr(ThunkIt->second), JITSymbolFlags());
                } else {
                  LLVM_DEBUG(dbgs() << "BOLT-PPC64-LOOKUP: " << SymName
                                    << " -> (plt-redirect-bare) symtab 0x"
                                    << Twine::utohexstr(Addr) << "\n");
                  AllResults[Symbol.first] = orc::ExecutorSymbolDef(
                      orc::ExecutorAddr(Addr), JITSymbolFlags());
                }
                goto next_symbol;
              }
              if (const BinaryData *I =
                      Linker.BC.getBinaryDataByName(BareNameStr)) {
                uint64_t Address = I->isMoved() && !I->isJumpTable()
                                       ? I->getOutputAddress()
                                       : I->getAddress();
                LLVM_DEBUG(dbgs() << "BOLT-PPC64-LOOKUP: " << SymName
                                  << " -> (plt-redirect-bare) BinaryData 0x"
                                  << Twine::utohexstr(Address) << "\n");
                AllResults[Symbol.first] = orc::ExecutorSymbolDef(
                    orc::ExecutorAddr(Address), JITSymbolFlags());
                goto next_symbol;
              }
            }
          }
          // Could not resolve the real symbol name — fall through to the
          // normal lookup paths below.
          LLVM_DEBUG(dbgs() << "BOLT-PPC64-LOOKUP: " << SymName
                            << " -> (plt-stub fallthrough)\n");
          break;
        }
      }

      if (auto SymInfo = Linker.lookupSymbolInfo(SymName)) {
        LLVM_DEBUG(dbgs() << "Resolved to address 0x"
                          << Twine::utohexstr(SymInfo->Address) << "\n");
        if (IsPPC64) {
          // Check if the resolved address is a PLT thunk that has a safe BOLT
          // stub.  If so, redirect to the stub.  This handles the case where a
          // bare external symbol name (e.g. "getenv@@GLIBC_2.17") was
          // registered in Symtab pointing at the original binary's PLT thunk
          // by notifyResolved(), and is now being requested by a JITLink
          // $__STUBS PIC stub to populate its $__GOT slot.  Without this
          // redirect, the $__GOT slot gets the PLT thunk address, which uses
          // "ld r12,N(r2)" with the original TOC base — crashing when called
          // from BOLT-rewritten functions that set r2 = new BOLT TOC.
          auto ThunkIt = Linker.PLTThunkToStub.find(SymInfo->Address);
          if (ThunkIt != Linker.PLTThunkToStub.end()) {
            uint64_t StubAddr = ThunkIt->second;
            errs() << "BOLT-PPC64: " << SymName << " -> PLT thunk 0x"
                   << Twine::utohexstr(SymInfo->Address)
                   << " redirected to BOLT stub 0x"
                   << Twine::utohexstr(StubAddr) << "\n";
            AllResults[Symbol.first] = orc::ExecutorSymbolDef(
                orc::ExecutorAddr(StubAddr), JITSymbolFlags());
            continue;
          }
          LLVM_DEBUG(dbgs() << "BOLT-PPC64-LOOKUP: " << SymName
                            << " -> symtab 0x"
                            << Twine::utohexstr(SymInfo->Address) << "\n");
        }
        AllResults[Symbol.first] = orc::ExecutorSymbolDef(
            orc::ExecutorAddr(SymInfo->Address), JITSymbolFlags());
        continue;
      }

      if (const BinaryData *I = Linker.BC.getBinaryDataByName(SymName)) {
        uint64_t Address = I->isMoved() && !I->isJumpTable()
                               ? I->getOutputAddress()
                               : I->getAddress();
        LLVM_DEBUG(dbgs() << "Resolved to address 0x"
                          << Twine::utohexstr(Address) << "\n");
        LLVM_DEBUG(if (IsPPC64) dbgs()
                   << "BOLT-PPC64-LOOKUP: " << SymName << " -> BinaryData 0x"
                   << Twine::utohexstr(Address) << "\n");
        AllResults[Symbol.first] = orc::ExecutorSymbolDef(
            orc::ExecutorAddr(Address), JITSymbolFlags());
        continue;
      }

      if (Linker.BC.isGOTSymbol(SymName)) {
        if (const BinaryData *I = Linker.BC.getGOTSymbol()) {
          uint64_t Address =
              I->isMoved() ? I->getOutputAddress() : I->getAddress();
          LLVM_DEBUG(dbgs() << "Resolved to address 0x"
                            << Twine::utohexstr(Address) << "\n");
          LLVM_DEBUG(if (IsPPC64) dbgs()
                     << "BOLT-PPC64-LOOKUP: " << SymName << " -> GOTSymbol 0x"
                     << Twine::utohexstr(Address) << "\n");
          AllResults[Symbol.first] = orc::ExecutorSymbolDef(
              orc::ExecutorAddr(Address), JITSymbolFlags());
          continue;
        }
      }

      LLVM_DEBUG(dbgs() << "Resolved to address 0x0\n");
      LLVM_DEBUG(if (IsPPC64) dbgs()
                 << "BOLT-PPC64-LOOKUP: " << SymName << " -> UNRESOLVED (0x0)\n");
      AllResults[Symbol.first] =
          orc::ExecutorSymbolDef(orc::ExecutorAddr(0), JITSymbolFlags());
    next_symbol:;
    }

    LC->run(std::move(AllResults));
  }

  Error notifyResolved(jitlink::LinkGraph &G) override {
    for (auto *Symbol : G.defined_symbols()) {
      SymbolInfo Info{Symbol->getAddress().getValue(), Symbol->getSize()};
      auto Name =
          Symbol->hasName() ? (*Symbol->getName()).str() : std::string();
      Linker.Symtab.insert({Name, Info});

      // PPC64 ELFv2: build PLTThunkToStub reverse map.
      // BOLT names its safe call stubs:
      //   __bolt_ppc_abs_call_stub.XXXX.plt_call.REALNAME
      //   __bolt_ppc_abs_call_stub.XXXX.plt_branch.REALNAME
      // For each such stub, find the original PLT thunk entry in Symtab
      // (keyed by "XXXX.plt_call.REALNAME" or "XXXX.plt_branch.REALNAME")
      // and record: PLTThunkAddr → StubAddr.
      if (IsPPC64) {
        StringRef SN(Name);
        const StringRef Prefix = "__bolt_ppc_abs_call_stub.";
        if (SN.starts_with(Prefix)) {
          // ThunkName = "XXXX.plt_call.REALNAME" (after the prefix)
          StringRef ThunkName = SN.drop_front(Prefix.size());
          uint64_t StubAddr = Info.Address;
          if (auto ThunkInfo = Linker.lookupSymbolInfo(ThunkName)) {
            uint64_t ThunkAddr = ThunkInfo->Address;
            Linker.PLTThunkToStub.insert({ThunkAddr, StubAddr});
            LLVM_DEBUG(dbgs() << "BOLT-PPC64: PLTThunkToStub[0x"
                              << Twine::utohexstr(ThunkAddr) << "] = 0x"
                              << Twine::utohexstr(StubAddr) << " (stub=" << Name
                              << ", thunk=" << ThunkName << ")\n");
          }
        }
      }
    }

    return Error::success();
  }

  void notifyFinalized(
      jitlink::JITLinkMemoryManager::FinalizedAlloc Alloc) override {
    if (Alloc)
      Linker.Allocs.push_back(std::move(Alloc));
    ++Linker.MM->ObjectsLoaded;
  }
};

JITLinkLinker::JITLinkLinker(BinaryContext &BC,
                             std::unique_ptr<ExecutableFileMemoryManager> MM)
    : BC(BC), MM(std::move(MM)) {}

void JITLinkLinker::buildPLTThunkToStubMap() {
  if (!BC.TheTriple->isPPC64())
    return;
  // Scan all BinaryFunctions for __bolt_ppc_abs_call_stub.* injected stubs.
  // For each stub named "__bolt_ppc_abs_call_stub.XXXX.plt_call.NAME" (or
  // plt_branch), find the corresponding original PLT thunk by looking up
  // "XXXX.plt_call.NAME" in BinaryData, and record PLTThunkAddr → StubAddr.
  // This is called before the first lookup() so that PLTThunkToStub is ready
  // when JITLink asks for symbol addresses to populate $__GOT slots.
  const StringRef Prefix = "__bolt_ppc_abs_call_stub.";
  for (auto &[Addr, BF] : BC.getBinaryFunctions()) {
    StringRef Name = BF.getOneName();
    if (!Name.starts_with(Prefix))
      continue;
    StringRef ThunkName = Name.drop_front(Prefix.size());
    uint64_t StubAddr = BF.getOutputAddress();
    if (StubAddr == 0)
      continue;
    // Look up the thunk by name in BinaryData.
    if (const BinaryData *ThunkBD = BC.getBinaryDataByName(ThunkName)) {
      uint64_t ThunkAddr = ThunkBD->getAddress();
      PLTThunkToStub.insert({ThunkAddr, StubAddr});
      LLVM_DEBUG(dbgs() << "BOLT-PPC64: PLTThunkToStub (pre-link) [0x"
                        << Twine::utohexstr(ThunkAddr) << "] = 0x"
                        << Twine::utohexstr(StubAddr)
                        << " stub=" << Name << "\n");
    }
  }
  errs() << "BOLT-PPC64: PLTThunkToStub pre-populated with "
         << PLTThunkToStub.size() << " entries\n";
}

JITLinkLinker::~JITLinkLinker() { cantFail(MM->deallocate(std::move(Allocs))); }

void JITLinkLinker::loadObject(MemoryBufferRef Obj,
                               SectionsMapper MapSections) {
  // On first object load for PPC64, pre-populate PLTThunkToStub from BC so
  // that lookup() can redirect bare symbol names to BOLT stubs immediately,
  // before notifyResolved() has a chance to register them.
  if (!PLTThunkToStubBuilt) {
    PLTThunkToStubBuilt = true;
    buildPLTThunkToStubMap();
  }
  auto LG = jitlink::createLinkGraphFromObject(Obj, BC.getSymbolStringPool());
  if (auto E = LG.takeError()) {
    errs() << "BOLT-ERROR: JITLink failed: " << E << '\n';
    exit(1);
  }

  if ((*LG)->getTargetTriple().getArch() != BC.TheTriple->getArch()) {
    errs() << "BOLT-ERROR: linking object with arch "
           << (*LG)->getTargetTriple().getArchName()
           << " into context with arch " << BC.TheTriple->getArchName() << "\n";
    exit(1);
  }

  auto Ctx = std::make_unique<Context>(*this, MapSections);
  jitlink::link(std::move(*LG), std::move(Ctx));
}

std::optional<JITLinkLinker::SymbolInfo>
JITLinkLinker::lookupSymbolInfo(StringRef Name) const {
  auto It = Symtab.find(Name.data());
  if (It == Symtab.end())
    return std::nullopt;

  return It->second;
}

SmallVector<jitlink::Block *, 2>
JITLinkLinker::orderedBlocks(const jitlink::Section &Section) {
  SmallVector<jitlink::Block *, 2> Blocks(Section.blocks());
  llvm::sort(Blocks, [](const auto *LHS, const auto *RHS) {
    return LHS->getAddress() < RHS->getAddress();
  });
  return Blocks;
}

size_t JITLinkLinker::sectionSize(const jitlink::Section &Section) {
  size_t Size = 0;

  for (const auto *Block : orderedBlocks(Section)) {
    Size = jitlink::alignToBlock(Size, *Block);
    Size += Block->getSize();
  }

  return Size;
}

} // namespace bolt
} // namespace llvm
