//===- bolt/Passes/LongJmp.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_PASSES_LONGJMP_H
#define BOLT_PASSES_LONGJMP_H

#include "bolt/Passes/BinaryPasses.h"
#include "llvm/ADT/SmallVector.h"

namespace llvm {
namespace bolt {

class BranchLivenessInfo;

/// LongJmp is veneer-insertion pass originally written for AArch64 that
/// compensates for its short-range branches, typically done during linking. We
/// pull this pass inside BOLT because here we can do a better job at stub
/// inserting by manipulating the CFG, something linkers can't do.
///
/// We iteratively repeat the following until no modification is done: we do a
/// tentative layout with the current function sizes; then we add stubs for
/// branches that we know are out of range or we expand smaller stubs (28-bit)
/// to a large one if necessary (32 or 64).
///
/// This expansion inserts the equivalent of "linker stubs", small
/// blocks of code that load a 64-bit address into a pre-allocated register and
//  then executes an unconditional indirect branch on this register. By using a
/// 64-bit range, we guarantee it can reach any code location.
///
class LongJmpPass : public BinaryFunctionPass {
  /// Used to implement stub grouping (reusing a stub from one function into
  /// another)
  using StubTy = std::pair<uint64_t, BinaryBasicBlock *>;
  using StubGroupTy = SmallVector<StubTy, 4>;
  using StubGroupsTy = DenseMap<const MCSymbol *, StubGroupTy>;
  StubGroupsTy HotStubGroups;
  StubGroupsTy ColdStubGroups;
  DenseMap<const MCSymbol *, BinaryBasicBlock *> SharedStubs;

  /// Stubs that are local to a function. This will be the primary lookup
  /// before resorting to stubs located in foreign functions.
  using StubMapTy = DenseMap<const BinaryFunction *, StubGroupsTy>;
  /// Used to quickly fetch stubs based on the target they jump to
  StubMapTy HotLocalStubs;
  StubMapTy ColdLocalStubs;

  /// Used to quickly identify whether a BB is a stub, sharded by function
  DenseMap<const BinaryFunction *, std::set<const BinaryBasicBlock *>> Stubs;

  using FuncAddressesMapTy = DenseMap<const BinaryFunction *, uint64_t>;
  /// Hold tentative addresses
  FuncAddressesMapTy HotAddresses;
  FuncAddressesMapTy ColdAddresses;
  DenseMap<const BinaryBasicBlock *, uint64_t> BBAddresses;

  /// Used to identify the stub size
  DenseMap<const BinaryBasicBlock *, int> StubBits;

  /// Stats about number of stubs inserted
  uint32_t NumHotStubs{0};
  uint32_t NumColdStubs{0};
  uint32_t NumSharedStubs{0};

  /// PPC64 ELFv2 diagnostics: why needsStub() said yes.
  ///
  /// The aggregate counters above say how many stubs exist but not which rule
  /// demanded them, and on PPC64 four quite different rules can. Two of them --
  /// the forced .plt_call./.plt_branch. rule and the forced ignored-function
  /// rule -- return true with no distance test at all, so they could in
  /// principle be producing stubs for targets that were comfortably in range.
  /// Whether they are is a measurement nobody has taken.
  ///
  /// These are RUNNING TOTALS across the whole relaxation fixpoint, and the
  /// per-iteration figures are printed as deltas at the end of each iteration.
  /// An earlier version of this reset them at the top of every iteration so
  /// that only the final iteration's values were printed, on the theory that
  /// the two unconditional rules fire every iteration while the distance-based
  /// ones can flip, so running totals would inflate the former. The theory was
  /// wrong in a way that destroyed the measurement: by the final iteration the
  /// calls already point at their stubs, so needsStub() says no everywhere and
  /// all four buckets print zero. The real correction for the uneven weighting
  /// is to print per-iteration deltas and let the reader see iteration 1 --
  /// where the stubs are actually created -- separately. On clang that fixpoint
  /// converges in 2 iterations, so the distortion was never large anyway.
  ///
  /// mutable because needsStub() is const and should stay const; counting is
  /// not a semantic change.
  mutable uint64_t NumStubsPLTForced{0};
  mutable uint64_t NumStubsIgnoredForced{0};
  mutable uint64_t NumStubsMarginOnly{0};
  mutable uint64_t NumStubsGenuinelyFar{0};

  /// Of the stubs the two forced rules demanded, how many were for targets a
  /// plain in-range branch could have reached.
  ///
  /// This is the number the whole exercise is for. "A forced rule fired" is not
  /// the same claim as "the stub was unnecessary": the forced rules skip the
  /// distance test, so counting them says nothing about whether the target was
  /// within a 26-bit +/-32MB `bl`. If these targets are mostly hundreds of MB
  /// away, making the rules conditional buys nothing and the cost of the pass
  /// is somewhere else entirely.
  ///
  /// AddrUnknown is counted separately rather than folded into WouldNotFit,
  /// because the two mean opposite things. A target whose address BinaryContext
  /// cannot resolve is a target the forced rule is *right* about -- that is the
  /// rule's whole justification. Folding those into "too far" would manufacture
  /// agreement with the rule out of an absence of data.
  mutable uint64_t NumForcedWouldFit{0};
  mutable uint64_t NumForcedWouldNotFit{0};
  mutable uint64_t NumForcedAddrUnknown{0};

  /// Resolve \p TgtSym for diagnostics without asserting if it cannot be
  /// resolved. Deliberately not getSymbolAddress(), which asserts
  /// "Unrecognized symbol" -- and the forced rules exist precisely because
  /// these targets are the ones BOLT's layout cannot see, so asking through
  /// the asserting path would abort the measurement instead of answering it.
  /// Returns false when no address is available.
  bool tryResolveForDiag(const BinaryContext &BC, const MCSymbol *TgtSym,
                         uint64_t &Addr) const;

  /// Record whether a stub one of the forced rules demanded was for a target
  /// that an in-range branch could have reached.
  void classifyForcedStub(const BinaryContext &BC, const MCInst &Inst,
                          const MCSymbol *TgtSym, uint64_t DotAddress) const;

  /// The shortest distance for any branch instruction on AArch64.
  static constexpr size_t ShortestJumpBits = 11;
  static constexpr size_t ShortestJumpSpan = 1ULL << (ShortestJumpBits - 1);

  /// The longest single-instruction branch.
  static constexpr size_t LongestJumpBits = 28;
  static constexpr size_t LongestJumpSpan = 1ULL << (LongestJumpBits - 1);

  /// Relax all internal function branches including those between fragments.
  /// Assume that fragments are placed in different sections but are within
  /// 128MB of each other. Return false and report an error if a branch cannot
  /// be relaxed.
  bool relaxLocalBranches(BinaryFunction &BF,
                          const BranchLivenessInfo *BLI = nullptr);

  /// Relax calls and direct unconditional branches using one cluster layout.
  void relaxWithClusters(BinaryContext &BC);

  ///                 -- Layout estimation methods --
  /// Try to do layout before running the emitter, by looking at BinaryFunctions
  /// and MCInsts -- this is an estimation. To be correct for longjmp inserter
  /// purposes, we need to do a size worst-case estimation. Real layout is done
  /// by RewriteInstance::mapFileSections()
  void tentativeLayout(const BinaryContext &BC,
                       BinaryFunctionListType &SortedFunctions);
  uint64_t tentativeLayoutRelocMode(const BinaryContext &BC,
                                    BinaryFunctionListType &SortedFunctions,
                                    uint64_t DotAddress);
  uint64_t tentativeLayoutRelocColdPart(const BinaryContext &BC,
                                        BinaryFunctionListType &SortedFunctions,
                                        uint64_t DotAddress);
  void tentativeBBLayout(const BinaryFunction &Func);

  /// Update stubs addresses with their exact address after a round of stub
  /// insertion and layout estimation is done.
  void updateStubGroups();

  ///              -- Relaxation/stub insertion methods --
  /// Creates a  new stub jumping to \p TgtSym and updates bookkeeping about
  /// this stub using \p AtAddress as its initial location. This location is
  /// an approximation and will be later resolved to the exact location in
  /// a next iteration, in updateStubGroups.
  /// \p IsCall indicates the original instruction being relaxed was a call
  /// (e.g. PPC64 bl), even if \p TgtIsFunc is false because the target
  /// happens to resolve to a local BinaryBasicBlock (e.g. a self-recursive
  /// call to the function's own entry). This matters on PPC64 ELFv2, where
  /// a call and a branch share the same 26-bit ±32MB encoding and range
  /// failure mode, so both need the same long-jump treatment.
  std::pair<std::unique_ptr<BinaryBasicBlock>, MCSymbol *>
  createNewStub(BinaryBasicBlock &SourceBB, const MCSymbol *TgtSym,
                bool TgtIsFunc, uint64_t AtAddress, bool IsCall = false);

  /// Replace the target of call or conditional branch in \p Inst with a
  /// a stub that in turn will branch to the target (perform stub insertion).
  /// If a new stub was created, return it.
  std::unique_ptr<BinaryBasicBlock>
  replaceTargetWithStub(BinaryBasicBlock &BB, MCInst &Inst, uint64_t DotAddress,
                        uint64_t StubCreationAddress);

  /// Helper used to fetch the closest stub to \p Inst at \p DotAddress that
  /// is jumping to \p TgtSym. Returns nullptr if the closest stub is out of
  /// range or if it doesn't exist. The source of truth for stubs will be the
  /// map \p StubGroups, which can be either local stubs for a particular
  /// function that is very large and needs to group stubs, or can be global
  /// stubs if we are sharing stubs across functions.
  BinaryBasicBlock *lookupStubFromGroup(const StubGroupsTy &StubGroups,
                                        const BinaryFunction &Func,
                                        const MCInst &Inst,
                                        const MCSymbol *TgtSym,
                                        uint64_t DotAddress) const;

  /// Lookup closest stub from the global pool, meaning this can return a basic
  /// block from another function.
  BinaryBasicBlock *lookupGlobalStub(const BinaryBasicBlock &SourceBB,
                                     const MCInst &Inst, const MCSymbol *TgtSym,
                                     uint64_t DotAddress) const;

  /// Lookup closest stub local to \p Func.
  BinaryBasicBlock *lookupLocalStub(const BinaryBasicBlock &SourceBB,
                                    const MCInst &Inst, const MCSymbol *TgtSym,
                                    uint64_t DotAddress) const;

  /// Helper to identify whether \p Inst is branching to a stub
  bool usesStub(const BinaryFunction &Func, const MCInst &Inst) const;

  /// True if Inst is a branch that is out of range
  bool needsStub(const BinaryBasicBlock &BB, const MCInst &Inst,
                 uint64_t DotAddress) const;

  /// Expand the range of the stub in StubBB if necessary
  Error relaxStub(BinaryBasicBlock &StubBB, bool &Modified);

  /// Helper to resolve a symbol address according to our tentative layout
  uint64_t getSymbolAddress(const BinaryContext &BC, const MCSymbol *Target,
                            const BinaryBasicBlock *TgtBB) const;

  /// Relax function by adding necessary stubs or relaxing existing stubs
  Error relax(BinaryFunction &BF, bool &Modified);

public:
  /// BinaryPass public interface

  explicit LongJmpPass(const cl::opt<bool> &PrintPass)
      : BinaryFunctionPass(PrintPass) {}

  const char *getName() const override { return "long-jmp"; }

  Error runOnFunctions(BinaryContext &BC) override;
};
} // namespace bolt
} // namespace llvm

#endif
