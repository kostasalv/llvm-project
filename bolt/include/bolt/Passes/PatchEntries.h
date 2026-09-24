//===- bolt/Passes/PatchEntries.h - Patch function entries ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Pass for patching original function entry points.
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_PASSES_PATCH_ENTRIES_H
#define BOLT_PASSES_PATCH_ENTRIES_H

#include "bolt/Passes/BinaryPasses.h"

namespace llvm {
namespace bolt {

/// Pass for patching original function entry points.
class PatchEntries : public BinaryFunctionPass {
  // If the function size is below the threshold, attempt to skip patching it.
  static constexpr uint64_t PatchThreshold = 128;

  struct Patch {
    const MCSymbol *Symbol;
    uint64_t Address;
    uint64_t Size;
    /// Emit a single unconditional branch instead of an absolute long tail
    /// call. Only used on PPC64, and only when the branch displacement is
    /// known to be encodable without any knowledge of the output layout.
    bool DirectBranch = false;
    /// PPC64 ELFv2: index into the function's pending patch list of the patch
    /// this one branches to. -1 means the redirect targets \p Symbol directly.
    /// Used to send the global entry point to the patch installed at the local
    /// entry point, whose address is a fixed, small offset away.
    int BranchToPatch = -1;
    uint32_t PaddingAfter = 0;
  };

public:
  explicit PatchEntries() : BinaryFunctionPass(false) {}

  const char *getName() const override { return "patch-entries"; }
  Error runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif
