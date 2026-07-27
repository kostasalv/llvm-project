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
    Config.PrePrunePasses.push_back(markSectionsLive);
    Config.PostAllocationPasses.push_back([this](auto &G) {
      MapSections([&G](const BinarySection &Section, uint64_t Address) {
        reassignSectionAddress(G, Section, Address);
      });
      return Error::success();
    });

    // PPC64 ELFv2: before fixups are applied, demote any
    // CallBranchDeltaRestoreTOC edge whose post-call slot is not a NOP to
    // CallBranchDelta.  This prevents applyFixup from overwriting live code
    // in tail calls and non-simple functions emitted as raw bytes by BOLT.
    if (G.getTargetTriple().isPPC64()) {
      llvm::errs() << "BOLT-DIAG: isPPC64=true PPC64TOCBase=0x"
                   << Twine::utohexstr(Linker.BC.PPC64TOCBase) << "\n";
      bool IsLE = G.getTargetTriple().getArch() == Triple::ppc64le;
      if (IsLE)
        Config.PostPrunePasses.push_back(
            ppc64DowngradeRestoreTOCIfNoNOP<llvm::endianness::little>);
      else
        Config.PostPrunePasses.push_back(
            ppc64DowngradeRestoreTOCIfNoNOP<llvm::endianness::big>);

      // PPC64 ELFv2: JITLink's ELFJITLinker_ppc64 sets .TOC. to
      // $__GOT + 0x8000 (added as a PostAllocationPass in its constructor).
      // BOLT-rewritten functions hardcode r2 = original_binary_TOC_base, so
      // the JITLink-synthesised stubs must also use that same TOC base.
      // Override .TOC. in a PreFixupPass (which runs after all
      // PostAllocationPasses including defineTOCBase) to force it back to the
      // original binary's TOC base.  TOCDelta fixups in the synthesized stubs
      // are then computed relative to the correct r2 value.
      if (Linker.BC.PPC64TOCBase != 0) {
        uint64_t OrigTOC = Linker.BC.PPC64TOCBase;
        Config.PreFixupPasses.push_back([OrigTOC](jitlink::LinkGraph &G) {
          constexpr StringRef TOCName = ".TOC.";
          constexpr StringRef TOCAliasName = "__TOC__";
          // After defineTOCBase runs (a PostAllocationPass), .TOC. and
          // __TOC__ are absolute symbols.  Just walk absolute_symbols().
          for (auto *Sym : G.absolute_symbols()) {
            if (Sym->hasName() &&
                (*Sym->getName() == TOCName ||
                 *Sym->getName() == TOCAliasName)) {
              LLVM_DEBUG(dbgs()
                         << "BOLT PPC64: overriding TOC symbol "
                         << *Sym->getName() << " from 0x"
                         << Twine::utohexstr(Sym->getAddress().getValue())
                         << " to original TOC 0x"
                         << Twine::utohexstr(OrigTOC) << "\n");
              Sym->getAddressable().setAddress(orc::ExecutorAddr(OrigTOC));
            }
          }
          return Error::success();
        });
      }
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

      if (auto SymInfo = Linker.lookupSymbolInfo(SymName)) {
        LLVM_DEBUG(dbgs() << "Resolved to address 0x"
                          << Twine::utohexstr(SymInfo->Address) << "\n");
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
          AllResults[Symbol.first] = orc::ExecutorSymbolDef(
              orc::ExecutorAddr(Address), JITSymbolFlags());
          continue;
        }
      }

      LLVM_DEBUG(dbgs() << "Resolved to address 0x0\n");
      AllResults[Symbol.first] =
          orc::ExecutorSymbolDef(orc::ExecutorAddr(0), JITSymbolFlags());
    }

    LC->run(std::move(AllResults));
  }

  Error notifyResolved(jitlink::LinkGraph &G) override {
    for (auto *Symbol : G.defined_symbols()) {
      SymbolInfo Info{Symbol->getAddress().getValue(), Symbol->getSize()};
      auto Name =
          Symbol->hasName() ? (*Symbol->getName()).str() : std::string();
      Linker.Symtab.insert({std::move(Name), Info});
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

JITLinkLinker::~JITLinkLinker() { cantFail(MM->deallocate(std::move(Allocs))); }

void JITLinkLinker::loadObject(MemoryBufferRef Obj,
                               SectionsMapper MapSections) {
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
