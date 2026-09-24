# cfarm135 PPC64 PLT/IFUNC runtime findings

## Status

The root cause was confirmed in `bolt/lib/Core/BinaryFunction.cpp`: PPC64
relocation targets were marked `setNeedsPatch(true)` unconditionally. This defeated
the `PatchThreshold` skip in `PatchEntries.cpp` and forced 28-byte PPC64 entry patches
onto 20-byte ELFv2 `__plt_*` stubs and local-entry functions.

The focused `runtime/iplt.c` evidence showed:

```text
failed to patch entries in __plt_imemcpy/1
failed to patch entries in __plt_ifoo/1
failed to patch entries in main
failed to patch entries in _init/1(*2)
```

The `__plt_*` failures were subsequently addressed by recognizing text-resident
`__plt_*` symbols as pseudo-functions. The remaining valve was then fixed directly.

## Fix pushed

Branch:

```text
bolt-ppc-fix-plt-runtime
```

Latest commit:

```text
a00493c6c2fb [BOLT][PowerPC] Patch only out-of-range relocation targets
```

The initially pushed `a00493c6c2fb` attempted to gate `setNeedsPatch(true)` using
`TargetBF->getAddress()`. That was incorrect: it tested the input address before layout,
so it would effectively disable redirect requests for large rewrites. It was reverted
semantically by `a4d61b3c10aa`.

The final fix keeps the existing optional-relocation and redirect-request pairing, and
moves the range decision to `PatchEntries`, where the output address is available. Each
PPC64 entry now selects its actual patch size:

- four-byte direct `b` when `Function.getOutputAddress()` is within REL24 range;
- the existing 28-byte absolute long-tail-call otherwise.

The local-entry overlap and patch-size checks use that per-entry size, and patch records
store the selected size/direct-branch mode. This preserves the large-binary redirect
mechanism while allowing nearby 20-byte ELFv2 stubs and offset-8 local entries to be
patched safely.

Commits:

```text
48769d987569 [BOLT][PowerPC] Use direct branches for nearby entry patches
a4d61b3c10aa [BOLT][PowerPC] Restore relocation redirect requests
```

This specifically targets the confirmed cause. PatchEntries safety checks remain active;
only the patch encoding/size is selected after layout.

## Earlier durable commits

```text
6abda2dc3d41 [BOLT][PowerPC] Preserve non-branch data relocations
e4dc0e45d1ed [BOLT][PowerPC] Recognize ELFv2 PLT sections
9af9fbd18950 [BOLT][PowerPC] Skip virtual ELFv2 PLT sections
de9650039828 [BOLT][PowerPC] Do not register virtual PLT functions
39a293a2d59c [BOLT][PowerPC] Preserve linker-generated text PLT stubs
a00493c6c2fb [BOLT][PowerPC] Patch only out-of-range relocation targets
```

## Validation required

The focused test must be rerun on cfarm135 from the latest branch:

```bash
cd ~/llvm-project
git fetch origin bolt-ppc-fix-plt-runtime
git checkout bolt-ppc-fix-plt-runtime
git reset --hard origin/bolt-ppc-fix-plt-runtime
cd ~/llvm-build
nice -n 19 ninja -j16 llvm-bolt 2>&1 | tee ~/logs/build-plt-fix-valve.log
./bin/llvm-lit -v ../llvm-project/bolt/test/runtime/iplt.c \\
  2>&1 | tee ~/logs/iplt-fix-valve.log
```

Per the task amendments, do not build a pristine upstream control and do not run
the full optimized-clang workflow in this task. Compare the eventual failure list
against the archived laptop log:

```text
/Users/konstantinosalvertis/Documents/OpenSourceLLVMBolt/latestProgressWithAgents/cfarm135-logs/run-20260922T213748Z.log
```

That archived 24-test failure list already contains the known runtime/PLT failures,
`perf_brstack`, and `perf_test`; these are baseline failures, not newly introduced
regressions.

The shared-code regression requirement also calls for one cfarm14 run of the relevant
x86_64 and AArch64 tests after the PPC fix. That run is not yet executed.
