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

`48769d987569` then moved the range decision to `PatchEntries`, selecting a four-byte
direct `b` when `Function.getOutputAddress()` is within REL24 range. That was also
incorrect, and for the same underlying reason: **the output address is not available in
`PatchEntries`.** The pass is registered at `BinaryPassManager.cpp:534`, inside
`runOptimizationPasses()` (called from `RewriteInstance.cpp:869`), whereas
`OutputAddress` is only ever written by `BinaryFunction::updateOutputValues()`
(`BinaryFunction.cpp:4700`/`:4707`), reached from `emitAndLink()` at
`RewriteInstance.cpp:873`. `getOutputAddress()` therefore returns 0 for every function
while this pass runs, the guard never fires, and the commit was a no-op. It failed safe,
so it left the tree no worse — just not better.

The actual remaining failure was the local-entry-point overlap, confirmed on cfarm135
with `-v=1`:

```text
BOLT-INFO: unable to patch entry point in main at offset 0x8 (ELFv2 local entry point overlaps global-entry patch)
BOLT-INFO: unable to patch entry point in _init/1(*2) at offset 0x8 (...)
```

Note the scope: essentially every global ELFv2 function has a local entry point at
offset 8, so a 28-byte patch at offset 0 always collides with it. Refusing to patch on
collision abandons almost every function in the binary, not just PLT stubs.

The fix splits the redirect across both ABI entry points, using only displacements that
are provable without any layout information:

```text
offset 0     b <local entry patch>                 <- global entry
offset 4     left untouched
offset LEP   lis8 r12, ...; mtctr r12; bctr        <- local entry
```

Both entries land on the *global* entry point of the new function, which rebuilds r2
from r12 itself, so neither depends on the TOC base the caller happened to hold;
`createLongTailCall()` materializes the target into r12, which is what the ELFv2 ABI
requires of a caller entering a global entry point. The forwarding branch's displacement
is the local entry offset (at most 64 bytes), so it needs no output address. Leaving
offset 4 untouched is ABI-legal: ELFv2 §2.3.2.1 states that "addresses between the
global and local entry points must not be branch targets, either for function entry or
referenced by program logic of the function".

Total budget is `LEP + 28` = 36 bytes for the usual `LEP == 8`. `_init` has exactly 64
bytes of room (BOLT reports `setting size of function _init/1(*2) to 64`) and `main` has
152, so both fit. Where it does not fit, the function is still ignored, as before.

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
