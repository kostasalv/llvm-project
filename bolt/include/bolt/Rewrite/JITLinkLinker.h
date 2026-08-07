//===- bolt/Rewrite/JITLinkLinker.h - Linker using JITLink ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// BOLTLinker using JITLink.
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_REWRITE_JITLINK_LINKER_H
#define BOLT_REWRITE_JITLINK_LINKER_H

#include "bolt/Core/Linker.h"
#include "bolt/Rewrite/ExecutableFileMemoryManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ExecutionEngine/JITLink/JITLinkDylib.h"

#include <memory>
#include <vector>

namespace llvm {
namespace bolt {

class BinaryContext;

class JITLinkLinker : public BOLTLinker {
private:
  struct Context;
  friend struct Context;

  BinaryContext &BC;
  std::unique_ptr<ExecutableFileMemoryManager> MM;
  jitlink::JITLinkDylib Dylib{"main"};
  std::vector<ExecutableFileMemoryManager::FinalizedAlloc> Allocs;
  StringMap<SymbolInfo> Symtab;
  /// PPC64 ELFv2: maps PLT thunk address → BOLT safe-stub address.
  /// Pre-populated by buildPLTThunkToStubMap() before the first loadObject()
  /// call, and updated by notifyResolved() as new stubs are finalized.
  /// Used in lookup() to redirect bare symbol resolutions that accidentally
  /// land on a PLT thunk.
  DenseMap<uint64_t, uint64_t> PLTThunkToStub;
  bool PLTThunkToStubBuilt = false;

  /// PPC64 ELFv2: scan BC for all __bolt_ppc_abs_call_stub.* injected
  /// functions and build PLTThunkToStub before any lookup() calls.
  void buildPLTThunkToStubMap();

public:
  JITLinkLinker(BinaryContext &BC,
                std::unique_ptr<ExecutableFileMemoryManager> MM);
  ~JITLinkLinker();

  void loadObject(MemoryBufferRef Obj, SectionsMapper MapSections) override;
  std::optional<SymbolInfo> lookupSymbolInfo(StringRef Name) const override;

  static SmallVector<jitlink::Block *, 2>
  orderedBlocks(const jitlink::Section &Section);
  static size_t sectionSize(const jitlink::Section &Section);
};

} // namespace bolt
} // namespace llvm

#endif // BOLT_REWRITE_JITLINK_LINKER_H
