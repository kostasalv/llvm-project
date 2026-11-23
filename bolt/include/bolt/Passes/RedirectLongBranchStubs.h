//===- bolt/Passes/RedirectLongBranchStubs.h - PPC64 stub bypass ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// PPC64 ELFv2: send a call that goes through a GNU ld `.long_branch.` stub
// straight to the function the stub branches to, when that function was moved.
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_PASSES_REDIRECT_LONG_BRANCH_STUBS_H
#define BOLT_PASSES_REDIRECT_LONG_BRANCH_STUBS_H

#include "bolt/Passes/BinaryPasses.h"

namespace llvm {
namespace bolt {

/// A `.long_branch.` stub is one `b` that stays at its original address, so a
/// call from moved code to it can be out of `bl` range even when the moved
/// callee is close, and LongJmp then builds a long thunk for the stub. Calling
/// the moved callee directly removes the stub and the thunk.
///
/// The call is redirected only to the entry the stub already reaches: the
/// global entry (offset 0) if the callee has no local entry, or the local entry
/// (ELFv2 section 3.4.1, st_other) if the stub branches to it. The caller's r2
/// and r12 are unchanged, exactly as with the stub. The target must already
/// have a label at that offset, so that no new entry point is created.
class RedirectLongBranchStubs : public BinaryFunctionPass {
public:
  explicit RedirectLongBranchStubs(const cl::opt<bool> &PrintPass)
      : BinaryFunctionPass(PrintPass) {}

  const char *getName() const override { return "redirect-long-branch-stubs"; }
  Error runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif
