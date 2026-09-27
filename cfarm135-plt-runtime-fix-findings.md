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

---

## Update — PatchEntries verified on cfarm135, and the real root cause found

### 1. Both PatchEntries bugs are fixed and verified (commit f1dd0a40c258)

cfarm135, `NINJA_RC=0`, `readelf -Ws` on the rewritten `iplt.c.tmp.bolt.exe`:

```
78: 0000000010010868  28 FUNC LOCAL DEFAULT 13 main.org.0
79: 0000000010010860   4 FUNC LOCAL DEFAULT 13 main.org.gep
80: 0000000010010ad8  28 FUNC LOCAL DEFAULT 14 _init.org.0
81: 0000000010010ad0   4 FUNC LOCAL DEFAULT 14 _init.org.gep

0000000010010860 <main.org.gep>:
10010860: 08 00 00 48   b 0x10010868 <main.org.0>
```

- `main.org.gep` size 4 (was 0) — Bug B (forwarding branch dropped by
  `fixBranches()` because it was not marked a tail call) is fixed.
- `_init` is patched at all — Bug A (my own overlap check rejecting `_init`)
  is fixed by tracking `SplitLEPOffset` per function.
- `-v=1` prints no "unable to patch" / "too small" / "failed to patch".

### 2. The surviving 5/5 `runtime/iplt.c` SIGSEGV is a different, deeper bug

GDB:

```
0x000000001040024c in resolver_memcpy ()
   0x10400244 <+0>: addis r2,r12,2
   0x10400248 <+4>: addi  r2,r2,-31760
=> 0x1040024c <+8>: ld    r3,-32728(r2)
r12 0x10400244   r2 0x10418634   ctr 0x10400244
#1 resolve_ifunc  #2 elf_machine_rela  #4 _dl_relocate_object
```

The immediates `2` / `-31760` are verbatim copies from the *original*
address:

- old: `0x10010970 + 0x20000 - 0x7C10 = 0x10028D60` = `.TOC.`  ✓
- new: `0x10400244 + 0x20000 - 0x7C10 = 0x10418634`            ✗ (off by 0x3EF8D4)

The IRELATIVE redirects were correct (`0x10030dc8 -> 0x10400244`,
`0x10030dd0 -> 0x104001c0`), so only the TOC setup is wrong. The entry
patch was never the cause of this one.

### 3. Root cause: the GEP TOC preamble was never re-relocated

`R_PPC64_REL16_HA` / `_LO` were missing from `isSupportedPPC64`
(`bolt/lib/Core/Relocation.cpp`), so `analyzeRelocation` returned false and
`handleRelocation` dropped them with only an `LLVM_DEBUG` message plus
`++NumFailedRelocations` — invisible in a release build.

Independently, nothing would have consumed them even if recorded:

- the relocation-symbolization loop in `BinaryFunction::disassemble` is
  gated on `BC.isRISCV()`;
- `bolt/lib/Target/PowerPC/PPCMCSymbolizer.cpp` is **dead code** —
  `grep -rn tryAddingSymbolicOperand llvm/lib/Target/PowerPC/Disassembler/
  bolt/lib/Target/PowerPC/` hits only `PPCMCSymbolizer.{cpp,h}` itself;
  `PPCDisassembler.cpp` never calls it.

So before this change the PPC port had **no in-body relocation
symbolization at all**, through either mechanism.

### 4. Fix (local commit a3579d32034c, 220 insertions)

| File | Change |
|---|---|
| `bolt/lib/Core/Relocation.cpp` | REL16 family added to `isSupportedPPC64`, `getSizeForTypePPC64` (2), `isPCRelativePPC64` (true), `extractValuePPC64` (0) |
| `bolt/lib/Rewrite/RewriteInstance.cpp` | REL16 family added to the PPC64 `SkipVerification` switch |
| `bolt/lib/Core/BinaryFunction.cpp` | new `else if (BC.isPPC64())` arm routing in-body relocations to `MIB->replaceImmWithSymbolRef` |
| `bolt/lib/Target/PowerPC/PPCMCPlusBuilder.{h,cpp}` | `replaceImmWithSymbolRef` building `(Symbol + Addend - L)@ha/@hi/@lo` via `getOrCreateInstLabel` + `MCSpecifierExpr` |
| `bolt/test/PPC64/gep-toc-recompute.{test,c}` | lit test calling through a volatile function pointer so the GEP preamble actually runs |

MC-layer validation (local Homebrew `llvm-mc`) — the constructed expression
assembles to relocations byte-identical to the compiler's:

```
addis 2, 12, (.TOC.+0-.Lp0)@ha  ->  R_PPC64_REL16_HA .TOC. + 0  @ 0
addi  2, 2,  (.TOC.+4-.Lp1)@l   ->  R_PPC64_REL16_LO .TOC. + 4  @ 4
```

Getting the addend into the expression is essential: without it the `addi`
computes `lo(.TOC. - (func+4))` while the `addis` computes
`ha(.TOC. - func)`, leaving r2 off by 4.

Downstream links, each verified by reading the code:
`PPCELFObjectWriter.cpp:127-140` maps PC-relative `fixup_ppc_half16` +
`S_HA`/`S_LO` to `R_PPC64_REL16_HA`/`_LO`; JITLink
`ELF_ppc64.cpp:341-353` resolves those as `Delta16HA`/`Delta16LO`
(`ppc64.h:126` literally encodes the preamble pair);
`BinaryEmitter.cpp:513-533` emits `getInstLabel` before the instruction for
all targets; `RewriteInstance.cpp:942-947` already resolves `.TOC.` at
`.got + 0x8000`.

### 5. That commit alone did not fix the segfault: a third gate

Built on cfarm135 at `a3579d32034c`, `runtime/iplt.c` still exited -11 and
the rewritten resolver still held `addis 2,12,2 / addi 2,2,-31760`, even
though `readelf -rW` on the input showed 24 correct `R_PPC64_REL16*`
relocations.

A PPC64 in-body relocation has to pass **three** independent gates to reach
the emitter, and only two had been opened:

1. `Relocation.cpp::isSupportedPPC64` — otherwise `analyzeRelocation`
   returns false and `handleRelocation` drops the relocation with an
   `LLVM_DEBUG` + `++NumFailedRelocations` (silent in release builds).
2. `MCPlusBuilder::shouldRecordCodeRelocation` — `BinaryFunction::addRelocation`
   (`bolt/lib/Core/BinaryFunction.cpp:5132`) is
   `if (BC.MIB->shouldRecordCodeRelocation(RelType)) Rels[Offset] = ...`.
   The PPC override whitelisted **only `ELF::R_PPC64_REL14`**, so everything
   recorded past gate 1 was thrown away here.
3. Consumption in `BinaryFunction::disassemble` — the new `BC.isPPC64()` arm.

Gate 2 was the live one. Fixed in `845972e9c98b` by extending the switch to
the REL16 family. `R_PPC64_REL24` is deliberately still excluded: recording
it makes JITLink create `CallBranchDeltaRestoreTOC` edges that expect a NOP
at call+4, which PLT stubs do not have (they have `ld r2,24(r1)`), and the
link asserts.

Verified safe: the only consumer that re-emits `BinaryFunction::Relocations`
is `Islands.Relocations` in `BinaryEmitter.cpp:625-626` (AArch64 constant
islands). Otherwise the map is read only through `getRelocationAt()` /
`getRelocationsInRange()`.

### 6. Verified fixed on cfarm135

`PASS: BOLT :: runtime/iplt.c`. The resolver preamble is now recomputed at
the relocated address:

```
0000000010400244 <resolver_memcpy>:
10400244: addis 2, 12, -61      r2 = 0x10400244 - 0x3D0000 = 0x10030244
10400248: addi  2, 2, -29924    r2 = 0x10030244 -   0x74E4 = 0x10028D60
1040024c: ld    3, -32728(2)
10400250: blr
```

`0x10028D60` is exactly `.TOC.` (`readelf -sW`:
`65: 0000000010028d60 0 NOTYPE LOCAL HIDDEN 20 .TOC.`). Before the fix the
same two instructions computed `0x10418634`, off by `0x3EF8D4` — the
distance the function moved.

Full PPC64 suite on cfarm135 at `dcf99e29dc78`: **10/10 pass** (8 existing
PPC64 tests, the new `gep-toc-recompute.test`, and `runtime/iplt.c`).

### 7. Two test-harness findings, unrelated to BOLT

**lit does not use the login shell's compiler.** `PPC64/plt-call.test` had
been failing on `gcc: error: unrecognized command line option
'-mno-prefixed'` even though the login shell's GCC 14.2.1 accepts it: inside
lit, `gcc --version` is `gcc (GCC) 8.5.0 20210514 (Red Hat 8.5.0-28)`, the
AlmaLinux 8 system compiler. `-mcpu=power8` already excludes Power10
prefixed/pcrel instructions, so both flags were redundant; dropping them
(`845972e9c98b`) made the test pass. Not a BOLT bug.

**Two things suppress the relocations under test.** For `gep-toc-recompute`
the input has to actually contain `R_PPC64_REL16_HA/_LO`:

- `-fno-pic` — and `-fPIE -no-pie`, which gcc reads as cancelling PIE
  codegen — makes the compiler materialize `.TOC.` as a link-time constant
  (`lis 2, 4098 / addi 2, 2, 32512`, `R_PPC64_ADDR16_HA/_LO`).
- **`ld.bfd` rewrites the r12-relative sequence into that same absolute
  form**, the optimization ELFv2 ABI §2.3.2.1 explicitly permits ("a linker
  may rewrite the code sequence establishing addressability to a different,
  more optimized form"). **lld does not.**

Measured REL16 counts on cfarm135: every bfd-linked variant (`-fPIC -no-pie`,
`-fPIC -c` then link, `-fPIE` default link) = **0**;
`gcc -fPIC -fuse-ld=lld -no-pie` = **20**, likewise with the default link and
with clang. Hence `-fPIC -fuse-ld=lld` in the RUN line (`dcf99e29dc78`), and
hence the `llvm-readelf | FileCheck --check-prefix=INPUT` guard in the test —
without it the test would have passed vacuously.

### 8. Shared-code debt

This change touches `Relocation.cpp`, `RewriteInstance.cpp` and
`BinaryFunction.cpp`, all outside `bolt/lib/Target/PowerPC/`, so per
AGENTS.md it needs x86_64 **and** AArch64 regression runs on cfarm14
before it goes anywhere.

### 9. Cross-function shared stubs vs CheckLargeFunctions (commit bc4ce4e128d6)

`check-bolt` on cfarm135 at `9019ba62ce25` showed one *new* failure against the
archived baseline: `shared-object.test`, with

```text
<unknown>:0: error: Undefined temporary symbol .LStub2
BOLT-ERROR: Emission failed.
```

Provenance first: reverting both files of `9019ba62ce25` to its parent
reproduces it identically, and neither the test nor the lit config was touched
by any of the 22 commits since the baseline. So it is a real regression from one
of the 21 earlier commits, not from the `createRelocation` work.

`--group-stubs=0` makes it pass, which points at cross-function stub sharing.
`--print-large-functions` gives the mechanism outright:

```text
BOLT-INFO: inc size of 364 bytes exceeds allocated space by 12 bytes
<unknown>:0: error: Undefined temporary symbol .LStub2
```

The pass ordering, all in `BinaryPassManager::runAllPasses`:

| line | pass | effect |
|---|---|---|
| 487 | `PopulateOutputFunctions` | snapshots the emit list while everything is still simple |
| 551 | `LongJmpPass` (PPC64) | `inc_dup` reuses, 6×, a stub block living inside `inc` |
| 578 | `CheckLargeFunctions` (`!HasRelocations` only) | `inc` is 364 B vs a 352 B allocation → `setSimple(false)` |

In non-relocation mode `BinaryContext::shouldEmit()` reduces to `isSimple()`, so
`BinaryEmitter::emitFunctions` drops `inc` and every block inside it, including
`.LStub2` — while `inc_dup`, which grew by nothing *precisely because* it
borrowed the stub, is still emitted and still references the label.

The failure mode is self-selecting: the stub's 28 bytes fall entirely on the
function that is then dropped, and the borrowers pay none of it. Note also that
`--print-finalized` still shows `.LStub2` inside `inc`'s layout with its
`__ENTRY_.LStub2` secondary entry point, so this is *not* block erasure — the
block is alive right up to emission.

**Not PowerPC-specific.** AArch64 runs the same pass with the same
`--group-stubs` default (`cl::init(true)`, LongJmp.cpp:28) and
`CheckLargeFunctions` is architecture-neutral. PPC64 just reaches it easily
because an ELFv2 stub is a 28-byte `bctr` sequence rather than a 4-byte `b`.

Fix: gate registration into `HotStubGroups`/`ColdStubGroups` on
`BC.HasRelocations`. `lookupGlobalStub()` then finds nothing and `SharedStubs`
is never populated from the cross-function path; function-local stubs and
relocation mode are untouched.

Validation, both machines at the commit with clean trees:

* cfarm135: `check-bolt` **462 passed / 20 failed** (from 461 / 21), and
  `shared-object.test` + `bolt/test/PPC64` + `runtime/iplt.c` = 11/11.
* cfarm14: `check-bolt` FAIL list **byte-identical** before and after (189
  lines, `diff` clean), and the 13 tests exercising `LongJmpPass`
  (`AArch64/veneer*`, `bti-long-jmp*`, `long-jmp-offset-boundary`,
  `compact-code-model`, `lite-mode`, `plt-got`) = 12 passed, 1 unsupported.

Caveat on that gate: cfarm14 carries 188 pre-existing failures, almost all
DWARF and instrumentation, while an earlier log on the same tree
(`/tmp/kosta-bolt-noleak.log`) shows ~29. The degradation is identical on both
sides of the comparison, so the diff is still a valid gate, but that machine is
a weaker safety net than the numbers suggest and deserves its own look.

### 10. Machine inventory

| host | CPU | ISA | distro | CPUs |
|---|---|---|---|---|
| cfarm135 | POWER9 | ppc64le | AlmaLinux 8.10 | 128 |
| cfarm29 | POWER9 | ppc64le | Debian 13 (trixie) | 32 |
| cfarm120 | POWER10 | ppc64le | (unreachable) | — |
| cfarm14 | x86_64 | — | — | cross-target gate |

Only cfarm120 was POWER10. The divergences that have actually cost time here
are toolchain, not ISA: lit's gcc 8.5.0 vs the login shell's 14.2.1 on
cfarm135 (§7), `ld.bfd` rewriting the r12-relative TOC preamble (§7), and
cfarm135's 6-subfield `perf` brstack format that `perf2bolt` rejects.

Where P9 vs P10 genuinely matters is the *shape of the input*: ISA 3.1
prefixed/pc-relative code (`paddi`, `pld`, `R_PPC64_PCREL34`,
`R_PPC64_REL24_NOTOC`, `@notoc`) exists only on POWER10, and with
`-mcpu=power10` the compiler can omit the TOC-based global-entry preamble
entirely — so the REL16_HA/_LO recompute path of §3–§6 is not even exercised
there, while a PCREL34 path P9 never emits is. A green P9 run does not cover
prefixed/pcrel; a P10 run at default flags may not cover the TOC preamble.

### 11. Open: `PPCMCSymbolizer.cpp` is dead code

`bolt/lib/Target/PowerPC/PPCMCSymbolizer.cpp` is never reached —
`PPCDisassembler.cpp` does not call `tryAddingSymbolicOperand`. Either wire
it up or delete it; leaving it in place invites the next reader to assume
PPC64 in-body symbolization happens there.

### 12. Root cause of the BOLTed-clang segfault: `extractValuePPC64` returned 0

The crash (`rc=139`, 5/5, `#0 clang::TextDiagnosticBuffer::FlushDiagnostics`,
`#1 __libc_csu_init`, `ctr == r12 == 0x13587f58`) is a corrupted `.init_array`.
Two independent defects were tangled together here; only the first is a
correctness bug.

**(A) The corruption.** `extractValuePPC64()` listed `R_PPC64_ADDR64` and
`R_PPC64_ADDR32` in the group that ends `return 0;`. Both are plain data words
that store `S + A` directly, so returning zero throws away the only copy of
the value. The zero then propagates into `analyzeRelocation()`
(`RewriteInstance.cpp:2834-2846`): the "section symbol + offset"
normalisation only rewrites the relocation to point at the referenced symbol
when `Section->containsAddress(ExtractedValue)`, so with a zero it took the
other branch and computed

```
Addend = ExtractedValue - (SymbolAddress - PCRelOffset) = -SymbolAddress
```

leaving the symbol as the section base. `-debug-only=bolt` prints it plainly:

```
BOLT-DEBUG: Relocation: offset = 0x10010168; type = R_PPC64_ADDR16_HA;
  value = 0x0; symbol = section .mydata (.mydata);
  symbol address = 0x100201b8; addend = 0xffffffffeffdfe48; address = 0x0
```

`Address = SymbolAddress + Addend == 0`, so `handleRelocation()` finds no
`ReferencedBF` and falls into the `IsSectionRelocation` branch
(`:3690-3691`), binding the entry to `getOrCreateGlobalSymbol(SymbolAddress)`
— in clang, the NOTYPE/size-0 `.plt_branch._ZNK5clang20TextDiagnosticBuffer
16FlushDiagnostics…` stub that happens to sit exactly at the `.text` base
`0x10780060`.

There were therefore *two* wrong values to fix, in this order:

1. `Relocation::createExpr` also had `R_PPC64_ADDR64` in its
   "handled natively by the PPC64 backend, ignore the addend" switch. Both
   `createExpr` overloads are private and reachable only through
   `Relocation::emit`, whose only caller is `BinarySection::emitAsData` — so
   that switch affects **data emission only**, and the double-encoding
   rationale it cites does not apply to a full-width data word at all. With
   the addend discarded, all 486 `.init_array` slots came out as
   `0x10780060`. Fixed in `5f9d4365b05a`.
2. With the addend kept but still `-SymbolAddress`, the slots became
   `new_address - old_text_base` instead — equally garbage. Fixed in
   `8a4c11d3f08e` by returning `Contents` for `ADDR32`/`ADDR64`.

The half16 forms (`ADDR16_*`, `REL16_*`, TOC/GOT/TLS) must keep returning 0:
`ha()`/`lo()` of a value cannot be reconstructed from one instruction, and
those pairs are re-symbolized from the relocation's symbol and addend in
`PPCMCPlusBuilder::replaceImmWithSymbolRef`.

`bolt/test/PPC64/addr64-data-addend.s` is the end-to-end reproducer: two local
functions whose addresses are taken in a data section, so the `.quad`s
assemble as `.text + offset`; the BOLTed binary exits 0 only if the two
pointers still differ *and* each still reaches its own function. It failed on
both intermediate states and passes now (PPC64 lit: 10/10).

Expected side effect worth measuring: the verifier at
`RewriteInstance.cpp:2889` compares
`truncateToSize(SymbolAddress + Addend - PCRelOffset, RelSize)` against
`ExtractedValue`, so `BOLT-WARNING: Failed to analyze 10332 relocations`
should drop substantially.

**(B) Six ctors are silently not emitted.** Independent of (A), six of the 486
constructors — `X86PostLegalizerCombiner`, `X86PreLegalizerCombiner`,
`RegAllocFast`, `RegAllocBasic`, `PassTimingInfo`, `ScheduleDAGVLIW` — have
**zero** entries in the emitted object's symbol table, where a healthy ctor
(`PPCMCAsmInfo`) has three (`NAME`, `NAME.org.0`, `NAME.org.gep`). They are
the ones `patchELFFuncArraysPPC64` (`:6491-6553`) cannot repair, because its
`if (NewGEP == Entry || NewGEP == 0) continue;` skips anything that did not
move — which is why exactly those six stayed corrupt while the other 480 were
patched back. This is a missed optimisation, not a correctness bug, but it is
what exposed (A). Remaining suspects are the two PPC64 `setIgnored()` calls
that emit no diagnostic at all, `RewriteInstance.cpp:1383-1384` and
`:4330-4331`, plus the silent `setSimple(false)` for
`hasDynamicRelocationAtIsland()` at `:4320`.

**Unrelated bug noticed in passing:** `getSizeForTypePPC64` reports
`R_PPC64_REL32` as 8 bytes; it is a 4-byte relocation.

### 13. Definition-of-done status after the `extractValuePPC64` fix

Measured on cfarm135 at `8a4c11d3f08e`, 2026-09-26.

| # | criterion | result |
|---|---|---|
| 1 | optimised clang starts cleanly 5/5 | **PASS** — 5/5 `rc=0`, was 5/5 `rc=139` |
| 2 | no increase in the `R_PPC64_REL24` clobber pattern | **PASS** — `ps_msub`/`vpmsumh` count 0 baseline, 0 BOLTed |
| 3 | byte-identical object for a large C++ TU | **FAIL** — see below |
| 4 | `check-bolt` has no PowerPC-specific failures | PASS (462 passed / 20 failed, 4 fixed, 0 regressions) |

`.init_array` of the BOLTed clang: **486 entries, 486 distinct, 0** at the old
`.text` base `0x10780060`, **0** below `0x1000`. Pre-fix: all 486 were
`0x10780060`. PPC64 lit on cfarm135: 10/10.

cfarm14 cross-target gate for the two `bolt/lib/Core/Relocation.cpp` commits:
`bolt/test/X86` + `bolt/test/AArch64` at `bc4ce4e128d6` vs `8a4c11d3f08e`, 296
passed / 164 failed on both sides, FAIL lists identical test-for-test. Note for
whoever runs that gate next: lit's `(N of 472)` suffix is scheduling order and
must be stripped before diffing, or a clean run looks like 162 regressions.

### 14. Criterion 3 while it was still open — superseded by §15

Kept for the record of what was measured and what was ruled out. The conclusion
here ("the divergence is upstream of GlobalMerge") was right about GlobalMerge
and wrong about LLVM: the divergence was in the harness. See §15.

`llvm/lib/Target/X86/X86ISelLowering.cpp` compiled with `-resource-dir` pinned
identically for both binaries:

* both compilers are **deterministic** (two runs of each byte-identical), so
  this is not an ASLR or pointer-order artefact;
* 4091248 vs 4091256 bytes (+8), same section count (3283), same `FUNC` symbol
  count (1392);
* exactly one section changes size, `.data.rel.ro..L_MergedGlobals.3020`
  `0x7fc6` → `0x7fcf`; first differing byte is offset 40, `e_shoff`;
* the assembly diff is 4723 lines and is entirely merged-global layout: 3846
  `addi rX, rY, <imm>`, 10 `.size`, 8 `.asciz`, 4 `lhz`, 2 `ld`, plus the
  `.L.str.N = .L_MergedGlobals.M+offset` aliases. Nothing is dropped —
  `.L.str.512` just moves from `.L_MergedGlobals.3019+20564` to `+24022`.

The obvious conclusion, that `GlobalMerge` partitions differently, is **wrong**:
with `-mllvm -enable-global-merge=false` the two objects still differ
(4243728 vs 4243744, +16). The divergence is upstream of GlobalMerge and the
merged-global immediates were only where it became visible. Resume from the `-S`
diff with the pass disabled — see `HANDOFF-ppc64-addr64-rootcause.md`.

This is independent of the `.init_array` corruption: criteria 1, 2 and 4 hold.

Resolved in §15: redoing that `-S` diff with GlobalMerge off cut it from 4723
lines to 24, all four libstdc++ header paths, and named the cause — the driver's
own directory determines the GCC prefix.

## 15. Criterion 3 resolved: it was the test harness, not BOLT

Redoing the `-S` comparison with `-mllvm -enable-global-merge=false` — so that
the 3846 merged-global `addi` immediates stop drowning everything out — cut the
diff from 4723 lines to **24**, and all 24 are the same four strings:

```
< 	.asciz	"/usr/lib/gcc/ppc64le-redhat-linux/8/../../../../include/c++/8/optional"
< 	.size	.L.str.512, 71
---
> 	.asciz	"/tmp/../lib/gcc/ppc64le-redhat-linux/8/../../../../include/c++/8/optional"
> 	.size	.L.str.512, 74
```

The other three are `stl_iterator_base_funcs.h`, `stl_vector.h` and
`unique_ptr.h`. Clang's `Generic_GCC::GCCInstallationDetector` derives the GCC
installation prefix from the **driver's own directory** when no
`--gcc-toolchain` is given. The baseline is `~/llvm-build/bin/clang-24`, so it
computes `/usr/lib/gcc/...`; the BOLTed copy is `/tmp/kosta-clang3.bolt`, so it
computes `/tmp/../lib/gcc/...`. Confirmed straight from the driver:

```
base search:  /usr/lib/gcc/ppc64le-redhat-linux/8/../../../../include/c++/8
bolt search:  /tmp/../lib/gcc/ppc64le-redhat-linux/8/../../../../include/c++/8
```

The TU is compiled with `-D_GLIBCXX_ASSERTIONS -UNDEBUG`, so libstdc++'s
`assert()` calls embed those header paths as string literals in the object.
`/usr` → `/tmp/..` is +3 bytes on each of four strings = +12, which with
padding is the +16 delta seen with GlobalMerge off. With the pass on, the
longer strings shift every subsequent merged-global offset, which is where the
3846 changed `addi` immediates and the +9 growth of
`.data.rel.ro..L_MergedGlobals.3020` came from.

Pinning `-resource-dir` (which the earlier runs did) fixes the *resource*
directory only; it does not touch the GCC prefix. Adding
`--gcc-toolchain=/usr` to both compilations, with **no `-mllvm` knobs at all**,
gives:

```
RESULT[gcctc]=IDENTICAL
b7b5cd0a842e6d6072b97bb2c0535e64  /tmp/kosta-crit3f/gcctc-base.o
b7b5cd0a842e6d6072b97bb2c0535e64  /tmp/kosta-crit3f/gcctc-bolt.o
```

**Criterion 3 passes.** The BOLTed clang compiles a 2.6 MB C++ translation unit
to a byte-identical object. Nothing in LLVM was miscompiled; the measurement was
sensitive to where the binary was placed.

Two lessons for the harness, both now baked into `validate-head-dod.sh`:

* comparing two builds of a compiler requires pinning **every** path the driver
  derives from its own location, not just `-resource-dir`;
* a diff dominated by one mechanical pattern (here, thousands of layout
  immediates) should be re-taken with that mechanism disabled before any
  conclusion is drawn from it. Doing so turned an open question into four lines.

## 16. `R_PPC64_REL32` was reported as 8 bytes

`getSizeForTypePPC64()` grouped `R_PPC64_REL32` with the `doubleword64` types.
Figure 4-1 of the ELFv2 ABI gives it field `word32*`, calculation `S + A - P` —
four bytes. It is the relocation `.eh_frame` uses for a
`DW_EH_PE_pcrel|DW_EH_PE_sdata4` pointer, so it appears in real inputs.

Both consumers of the width were wrong as a result:

* `RewriteInstance::analyzeRelocation()` feeds it to
  `getUnsignedValueAtAddress()`, so four bytes of the *following* field landed
  in the extracted value and the check against
  `truncateToSize(SymbolAddress + Addend - PCRelOffset, RelSize)` could not
  match — each such relocation counted toward `Failed to analyze N relocations`.
* `BinarySection::emitAsData()` steps over the width of each relocation, so it
  skipped four bytes of real data after every `R_PPC64_REL32`.

Fixed in `5473a91b5132`, with `bolt/unittests/Core/Relocation.cpp` covering the
PPC64 width table. `getSizeForType()` dispatches only on the static
`Relocation::Arch`, so the test needs neither a PowerPC backend nor PowerPC
hardware — it runs anywhere, and it caught the discrepancy against x86's
`R_X86_64_PC32` and AArch64's `R_AARCH64_PREL32`.

### 16.1 The REL32 fix is latent on this binary — measured, not assumed

Re-BOLTing clang at `5473a91b5132` left `Failed to analyze 10332 relocations`
**exactly unchanged**. That is not a sign the fix is wrong; it is where the
relocations live. The baseline `clang-24` (linked with `--emit-relocs`, as BOLT
requires) contains 159744 `R_PPC64_REL32` relocations — the fifth most common
type — and `.rela.eh_frame` accounts for 226031 entries:

Two independent histograms, printed side by side — relocation types on the left,
containing sections on the right. The columns are **not** pairs: `.rela.branch_lt`
holds `R_PPC64_RELATIVE` (type 22), confirmed directly with
`readelf -rW bin/clang-24`, not the `R_PPC64_REL32` that happens to share its row.

```
1858839 R_PPC64_REL24        .rela.text            4561096
1202794 R_PPC64_TOC16_HA     .rela.eh_frame         226031
1158198 R_PPC64_TOC16_LO     .rela.data.rel.ro      173792
 187307 R_PPC64_ADDR64       .rela.data               9723
 159744 R_PPC64_REL32        .rela.branch_lt          7903
```

`RewriteInstance::readRelocations()` (RewriteInstance.cpp:3297) returns before
`handleRelocation()` for a fixed set of section names:

```cpp
  const bool SkipRelocs = StringSwitch<bool>(RelocatedSectionName)
                              .Cases({".plt", ".rela.plt", ".got.plt",
                                      ".eh_frame", ".gcc_except_table"},
                                     true)
                              .Default(false);
```

So every `R_PPC64_REL32` in this binary is filtered out before the width is ever
consulted, and neither the extracted-value check nor `emitAsData()` sees one.
The fix is correct against the ABI and against both consumers' contracts, and it
removes a trap for any input that does carry a `R_PPC64_REL32` outside
`.eh_frame` — but it buys nothing measurable here, and the 10332 unanalyzed
relocations have some other cause still to be found.

## 17. All four definition-of-done criteria pass at `5473a91b5132`

Full re-validation on cfarm135 with an `llvm-bolt` built at the branch head and
a freshly BOLTed clang (`/tmp/kosta-clang4.bolt`), 18:42–19:21 UTC:

| # | criterion | result |
|---|---|---|
| 1 | optimised clang starts cleanly 5/5 | **PASS** — 5/5 `rc=0` |
| 2 | no increase in the `R_PPC64_REL24` clobber pattern | **PASS** — `ps_msub`/`vpmsumh` 0 baseline, 0 BOLTed |
| 3 | byte-identical object for a large C++ TU | **PASS** — `b7b5cd0a842e6d6072b97bb2c0535e64` both |
| 4 | `check-bolt` has no PowerPC-specific failures | **PASS** — PPC64 lit 10/10 |

`.init_array`: 486 entries, 486 distinct, 0 at the old `.text` base, 0 near
null. The criterion-3 MD5 is the same value the `--gcc-toolchain`-pinned run
produced against the previous binary, so the REL32 change did not perturb the
output either.

Cross-target gate on cfarm14 for `5473a91b5132` (it touches
`bolt/lib/Core/Relocation.cpp`, outside `bolt/lib/Target/PowerPC/`):
`bolt/test/X86` + `bolt/test/AArch64` at `549f8b34c891` vs `5473a91b5132` —
296 passed / 164 failed on both sides, FAIL lists identical, `DIFF_RC=0`.
`CoreTests` builds and passes 50/50, including the 4 new `RelocationTester`
cases.

### Still not clean, and not claimed to be

The BOLT log still carries, all pre-existing and all on the open list:

* 3 `BOLT-ERROR: symbol seen in the middle of the function ...plt_branch...` —
  the FDE-derived size inflation for linker stubs. These are the only
  `BOLT-ERROR` lines; there are no assertion failures.
* `Failed to analyze 10332 relocations` (see §16.1).
* 5690 `internal call detected`, 5619 `unable to disassemble instruction at
  offset`, 299 `failed to patch entries in`, 167 `corrupted control flow
  detected`, 5708 `unclaimed data relocation` (§19 — this said 20 until the
  count was redone; the old figure was a grep artefact).

## 18. Criterion 3 widened to 51 translation units: 51/51 identical

One byte-identical object could be one lucky file. `criterion3-wide.sh` re-runs
the comparison over a sample drawn evenly across the source-size range from
`compile_commands.json` — 4925 candidate C++ TUs, 51 selected from 400 bytes up
to the 2.5 MB `X86ISelLowering.cpp` — with `-resource-dir` and
`--gcc-toolchain=/usr` pinned for both binaries, 8 compiles at a time, each
object deleted as soon as it compares equal.

```
  candidate C++ TUs: 4925  (0 KB .. 2.5 MB)
  selected: 51 TUs
  [  1/ 51] IDENTICAL       0KB  clang/tools/clang-shlib/clang-shlib.cpp
  ...
  [ 49/ 51] IDENTICAL     118KB  clang/lib/Sema/Sema.cpp
  [ 50/ 51] IDENTICAL     183KB  llvm/lib/Transforms/Vectorize/VPlanRecipes.cpp
  [ 51/ 51] IDENTICAL    2586KB  llvm/lib/Target/X86/X86ISelLowering.cpp
  IDENTICAL    51
  VERDICT=CLEAN  (51/51 identical)
```

11 minutes wall clock at `JOBS=8`, load average 1.15 before the run, nothing
left on disk afterwards.

The size spread is deliberate: small TUs exercise the driver, the preprocessor
and the trivial paths, and large ones are where the optimiser, the register
allocator and the deep template machinery live. A sample of only large files
would miss driver-level bugs; a sample of only small ones would miss everything
that matters. Four orders of magnitude, no differences.

Methodology, including why byte-identity is the right bar and the two flags that
have to be pinned, is written up separately in `TESTING-BOLTED-CLANG.md`.

**What this still does not cover.** The validated binary had
`0 out of 159758 functions (0.0%) have non-empty execution profile` — so no
function reordering, no ext-TSP block layout, no splitting, no ICF ran. This is
the correctness of BOLT's rewrite path, not of its optimisation passes.

## 19. The 5708 "unclaimed data relocation" warnings: the ELFv2 local entry point

### 19.1 The count was wrong first, and the wrong count hid the problem

This class was carried on the open list as "20 unclaimed data relocations" for
several sections of this document. That number came from histogramming the log
with `grep -oE "BOLT-WARNING: [a-z ]+"`. The character class stops at the first
digit or capital letter, so thousands of distinct warnings collapsed into a
handful of alphabetic prefixes and what got counted was the prefixes. The real
count is **5708**, and it is the largest single class of skipped function in the
binary — not a residue worth deferring.

Histogram BOLT warnings by normalising digits out of the whole line:

```bash
grep -oE "BOLT-WARNING: .*" log | sed -E 's/[0-9]+/N/g' | sort | uniq -c | sort -rn
```

Never by grepping a fixed alphabetic prefix. This is the third time in this port
that a crude local extraction produced a confidently-reported wrong number (after
this one: the `.group 000008 -> 00000c` non-difference). Extraction output needs
a sanity check before it is quoted.

### 19.2 The failure chain

```
readBranchLTRelocations()                     RewriteInstance.cpp:3110
  -> handleRelativeDynamicRelocation()                          :3229
       ReferenceOffset = ReferencedAddress - Func->getAddress()   // == 8
       -> registerInternalRefDataRelocation()                    :3246
            -> validateInternalRefDataRelocations()   // can only clear offsets
                                                     // covered by a recognised
                                                     // jump table -> warns,
                                                     // returns false
                 -> postProcessCFG() -> setSimple(false)
```

`.branch_lt` is the linker's branch lookup table: 8-byte absolute function
addresses used by `.plt_branch.`/`.plt_call.` trampolines for calls beyond the
26-bit ±32 MB `bl` range. A slot may hold either of a function's two ELFv2 entry
points — `Func+0`, the global entry whose 2-instruction preamble recomputes r2
from r12, or `Func+LocalEntryOffset` (conventionally `Func+8`), used when the
caller already holds the right TOC base. The second kind was being read as "data
references the interior of this function at an offset I cannot explain", which is
BOLT's signal for a computed branch whose jump table it does not control.

`setSimple(false)` is a conservative bail-out, not a corruption: the function
stays correct. The cost is that it is excluded from every optimisation pass. So
this was never a silent-wrong-data hazard — it was ~5708 functions silently
opted out of the thing BOLT exists to do.

### 19.3 Measured, not argued: every data→code relocation in the binary

`registerInternalRefDataRelocation()` has two call sites, and the first probe
only covered one of them. Site `:3246` is reached only from
`readBranchLTRelocations` (R_PPC64_RELATIVE only). Site `:3637` is reached from
`processRelocations()`, which skips allocatable sections (`:3038`) — but this
clang is linked `--emit-relocs`, so `.rela.data.rel.ro` (146133 code refs),
`.rela.data`, `.rela.init_array` and friends are non-allocatable and *are*
iterated. A third probe (`probe-datarefs.sh`) therefore histogrammed the
offset-into-containing-function of **every** relocation in **every** data
section:

```
  section                         total    off=0    off=8    other  funcs w/ off!=0
  .rela.branch_lt                  7902      835     7067        0  7062
  .rela.data                          8        8        0        0  0
  .rela.data.rel.ro              146133   146133        0        0  0
  .rela.gnu.build.attributes         21       18        0        3  3
  .rela.got                           3        3        0        0  0
  .rela.init_array                  485      485        0        0  0
  .rela.rodata                        3        3        0        0  0

  relocations at a NON-ZERO offset into a function: 7070
  of those, at an offset OTHER than 8:              3
  distinct functions with a non-zero-offset data ref: 7063
```

So: 154555 data→code relocations, 7070 at a non-zero offset, **7067 of them at
exactly the local entry**, and 3 anywhere else. Every one of the 146133
`.data.rel.ro` code references — vtables, function pointers — targets `Func+0`,
as the ABI requires. Across the whole binary there is not one genuine interior
data reference. The warning had a ~100% false-positive rate.

The 7063→5708 gap is functions BOLT never reaches validation for: already
ignored, already non-simple, or failed disassembly.

### 19.4 Why the local entry needs no bookkeeping to stay correct

Established by reading the code, not inferred. `patchELFBranchLT()` resolves
through `getNewFunctionAddress()` → `getBinaryFunctionAtAddress()`, which matches
**exact function starts only**. A `Func+8` slot therefore returns 0 and is
deliberately left pointing at the original address — which stays reachable
because `PatchEntries` installs a split global/local entry redirect there
(`PatchEntries.cpp:179-193`). References that do resolve symbolically go through
`getNewFunctionOrDataAddress()`, which has its own local-entry case at
`:6811-6814` returning `OutputAddress + getPPC64LocalEntryOffset()`.

Three sites already special-cased `Func+LEP` before this fix — `handleRelocation`'s
`IsPPC64LocalEntry` branch (`:3619-3628`), that `PatchEntries` redirect, and
`getNewFunctionOrDataAddress`. `handleRelativeDynamicRelocation` was the fourth
and last, and did not. The comment on `getNewFunctionOrDataAddress` even asserts
the local entry "is never registered as a BB start or an internal-reference
offset" — which is exactly what `:3246` was violating.

Phase ordering holds: the symbol loop that decodes `PPC64LocalEntryOffset` from
`st_other` (`:1385-1393`) closes at `:1417`, and `processDynamicRelocations()` is
called at `:1422`. The offset is fully populated before `.branch_lt` is read, and
is 0 for functions with no local entry, so the guard can never match a genuine
interior reference.

### 19.5 The fix, and why it needs full re-validation

`bfbaa5f3021c`, in `handleRelativeDynamicRelocation`:

```cpp
if (BC->isPPC64() && ReferenceOffset == Func->getPPC64LocalEntryOffset())
  return;
```

Plus `bolt/test/PPC64/branch-lt-local-entry.s`, which hand-builds both slot
kinds — including raw `Elf64_Rela` bytes in `.rela.branch_lt`, because that is
exactly how `readBranchLTRelocations()` reads it — calls through both slots, and
checks `CHECK-NOT: unclaimed data relocation`.

This removes a false de-optimisation, so ~5708 more functions now go through
BOLT's optimisation passes. That is a real behaviour change on code BOLT
previously left alone, which makes criterion 3 (byte-identical objects) the
load-bearing check for this commit, not a formality. The result in §18 must not
be inherited across it.

It also touches `bolt/lib/Rewrite/RewriteInstance.cpp`, outside
`bolt/lib/Target/PowerPC/`, so AGENTS.md requires the cfarm14 x86_64 + AArch64
two-point FAIL-list gate before it goes anywhere.

### 19.6 Two smaller things this probe turned up

* **3 relocations in `.rela.gnu.build.attributes` target `Func+4`** —
  `__libc_csu_init`, `stat`, `lstat`, statically linked glibc pieces referenced
  from GNU build-attribute notes. These reach site `:3637` and are genuinely
  unclaimable. Candidate fix: skip `.rela.gnu.build.attributes` in
  `readRelocations()` the way `.eh_frame` and `.gcc_except_table` already are —
  build-attribute notes are metadata and never need relocating.
* **`BinaryEmitter.cpp:386-398` re-encodes `st_other` as
  `getPPC64LocalEntryOffset() ? 3u : 0u`** — i.e. always "8" for any non-zero
  LEP — while `getNewFunctionOrDataAddress()` uses the true recorded byte offset.
  Latent only, and measured so: this binary has 27206 symbols with LEP=0 and
  230866 with LEP=8, and **zero** with any other value. Wrong the moment a
  function has LEP=16.

### 19.7 Validation of the fix, and the test that nearly proved nothing

**The four criteria, re-run at `bfbaa5f3021c` rather than inherited:**

| | result |
|---|---|
| unclaimed data relocations | **5708 → 0** |
| 1. five clean starts | 5/5 `rc=0` |
| 2. REL24 clobber pattern | 0 base, 0 bolt (unchanged) |
| 3. byte-identical objects | **52/52 identical**, 0 KB … 2.5 MB |
| 4. `check-bolt` PPC64 lit | 11/11 after the test was corrected |
| `BOLT-ERROR` / assertions | 3 / 0 — unchanged |

**The fix demonstrably changed what BOLT optimises**, which is the point of
measuring pass statistics rather than just the warning count:

| BOLT-INFO | before | after |
|---|---|---|
| UCE removed | 209287 blocks, 837148 bytes | **221485 blocks, 885940 bytes** |
| merged duplicate CFG edges | 1689 | **1810** |
| inserted stubs | 58448, shared 810584× | **60302, shared 852318×** |
| `_end` | `0x2109ee64` | `0x214b2564` |
| Failed to analyze relocations | 10332 | 10332 (unchanged) |
| corrupted control flow | 167 | 167 (unchanged) |

+12198 blocks eliminated and +1854 stubs is thousands of functions entering the
optimisation pipeline for the first time — and 52/52 byte-identical objects
across four orders of magnitude of TU size says the code they now produce is
still correct. Had the warning count dropped with these numbers unchanged, the
fix would have silenced a message without fixing anything.

`internal call detected` rose 5690 → 5863. Expected direction: those are reported
per analysed function, and there are now more of them.

**x86_64 + AArch64 gate** (required: `RewriteInstance.cpp` is outside
`bolt/lib/Target/PowerPC/`). `5473a91b5132` vs `bfbaa5f3021c` — the three commits
in between are documentation only, so this two-point run isolates exactly this
change. `BASE_FAILS=166 NEW_FAILS=164`, and the diff is deletions only:

```
165,166d164
< TIMEOUT: BOLT :: AArch64/compare-and-branch-split-functions.S
< TIMEOUT: BOLT :: AArch64/compare-and-branch-unsupported.S
```

Two load-dependent timeouts present at BASE and absent at NEW. Nothing newly
fails, which is what the gate asks.

#### The test was wrong twice, in opposite directions

First it **failed**, and the fix was not at fault. With `--emit-relocs` on the
link, `ld.lld` generates its *own* `SHT_RELA` section also named
`.rela.branch_lt` — holding `R_PPC64_ADDR64`, not `R_PPC64_RELATIVE` — for the
`.branch_lt` data, plus a `.rela.rela.branch_lt` for the hand-built section
itself:

```
  [ 1] .rela.branch_lt   PROGBITS  00000000100001c8  ...  A    <- hand-built
  [ 7] .rela.branch_lt   RELA      0000000000000000  ...  I 10 5   <- ld.lld's
  [ 8] .rela.rela.branch_lt RELA   0000000000000000  ...  I 10 1
```

Two sections with one name. BOLT's by-name lookup took the wrong one and aborted
in `ExecutableFileMemoryManager::updateSection` on `"Original section must exist
and be allocatable"`, and the warnings that appeared were `RType:26`
(`R_PPC64_ADDR64`) arriving at the *other* call site — never the guard. Dropping
`--emit-relocs` leaves exactly one `.rela.branch_lt` and the test passes.

Then it passed, and BOLT reported `relocation mode: 0` for that input — so a
green result was ambiguous: guard fired, or path never reached? A test that
cannot fail is worse than no test, so this was settled by building `llvm-bolt` at
`5473a91b5132` and running the identical inputs through both:

| llvm-bolt | slot | `unclaimed data relocation` |
|---|---|---|
| pre-fix `5473a91b5132` | `has_lep+8` | 1 |
| pre-fix | `has_lep+4` | 1 |
| fixed `bfbaa5f3021c` | `has_lep+8` | **0** |
| fixed | `has_lep+4` | **1** |

The path runs with or without relocation mode; the guard skips exactly
`getPPC64LocalEntryOffset()` and nothing adjacent. The `has_lep+4` case is now
committed as a positive control in the same test file, reassembled via
`--defsym CONTROL=1`, asserting the warning is *still* produced — so the test
cannot quietly go vacuous if this code moves.

Generalised: a passing negative-only test on a path you have not proved is
reachable is not evidence. Either show the test failing without the fix, or pair
it with a positive control.

## 20. The llc layout retest: block reordering works, function reordering does not

Re-run on cfarm135 at `792372f4dca4`, against `llc` (167,150,728 bytes, 79,199
functions). Corpus: `llvm/test/CodeGen/PowerPC/*.ll`, deterministically shuffled
with `shuf --random-source=<(yes 42)` and split 200 training / 80 held-out, zero
overlap by `comm -12`.

The profiling pipeline works at this scale. `perf record -e cycles:u -j any,u`
over 200 files x 25 repetitions captured 611,739 branch-stack samples into an
86,692,852-byte `perf.data`; `perf2bolt` converted it (rc=0) into a 22,282,131-byte
`llc.fdata` giving 9035/79199 functions (11.4%) a profile.

Five cumulative configurations, same binary, same profile, all with
`-thread-count=16`:

| Config | Flags added | Stubs hot/cold | Result |
|---|---|---|---|
| A | `-reorder-blocks=ext-tsp` | 33,734 / 0 | **rc=0**, 80/80 held-out files byte-identical |
| B | `+ -reorder-functions=cdsort` | 33,953 / 0 | rc=1, out of range, no binary |
| C | `+ -split-functions -split-all-cold` | 48,081 / 10,232 | rc=1, out of range, no binary |
| D | `+ -icf=1` | 47,963 / 10,225 | rc=1, out of range, no binary |
| E | `-reorder-blocks=ext-tsp -reorder-functions=hfsort` | 33,798 / 0 | rc=1, out of range, no binary |

In September every `-reorder-blocks=` algorithm produced an `llc` that crashed or
hung on a one-line `.ll`. Configuration A now produces one that runs and is
byte-exact on 80 of 80 held-out files. The boundary has moved to function
reordering.

Configuration E is the control that matters: `hfsort` is a different algorithm
producing a different layout, and it fails the same way at a comparable distance.
**The trigger is moving functions at all, not any one ordering pass**, so the
remedy belongs in stub insertion, not in a reordering pass.

Corrected distances (the printed fixup address is wrong -- see section 21):

| Config | target | real fixup address | distance | over +/-32 MiB by |
|---|---|---|---|---|
| B | `0x184d9208` | `0x160082b0` | 36.82 MiB | 4.82 MiB |
| C | `0x1851b5b8` | `0x160dd6f0` | 36.24 MiB | 4.24 MiB |
| D | `0x18464478` | `0x160db0d4` | 35.54 MiB | 3.54 MiB |
| E | `0x184d77c8` | `0x16025950` | 36.70 MiB | 4.70 MiB |

Every overshoot lands in 3.5-4.8 MiB. These are calls that just barely failed to
reach, which is the case a relaxation pass handles.

Two leads. The target is an `<anonymous symbol>` -- a block with no name, which
stub insertion may simply not see. And the addend is enormous in both
configurations that print one: B is `0x1656b440 + 0x1f6ddc8`, E is
`0x1655e300 + 0x1f794c8`; both addends are ~32.9 MB and the two bases are 53 KB
apart under unrelated ordering algorithms, which suggests the same unnamed block
reached two ways.

Note on machine courtesy: the first matrix run drove the 1-minute load average to
44.76, because `llvm-bolt` defaults `-thread-count` to hardware concurrency and
cfarm135 has 128 CPUs. Re-running configuration D under `-thread-count=16`
reproduced the identical failing address, so the cap changes only how long BOLT
takes, not what it emits.

## 21. The out-of-range diagnostic prints the target address twice

The failure above reports the relocation target and the fixup address as the same
value, which cannot be right -- a call to itself has distance zero.

`llvm/lib/ExecutionEngine/JITLink/JITLink.cpp` prints
`E.getTarget().getAddress()` where it claims to print the fixup address. The code
before `0c33799e374a` ("[JITLink] Include target addend in out-of-range error
(#145423)", 2025-06-23) printed `B.getFixupAddress(E)`, so this is a regression.

It cost real time here: the first reading of the message implied distance zero,
the second implied 31.4 MiB, which is *inside* the limit and would have meant a
broken range check. Neither was true.

No test caught it because all four existing tests stop checking at the word
`fixup`:

```text
# CHECK-ERROR: relocation target {{.*}} (X) is out of range of Pointer8 fixup
```

The fix is one line, plus a test that binds the printed fixup address and requires
it to reappear as the block address in the parenthetical. This touches `llvm/`,
not `bolt/lib/Target/PowerPC/`, so per `AGENTS.md` it needs the x86_64 and AArch64
gate on cfarm14, and it should go upstream as its own patch -- it is useful to
anyone debugging out-of-range relocations on any target.

## 22. The stub tax is the plain rewrite, not the layout passes

The natural assumption about the PPC64 slowdown is that it comes from BOLT moving
code around and lengthening call distances. Measured, that is wrong.

A control run with no profile and no layout passes at all --
`llvm-bolt llc -o llc.plain -thread-count=16` -- completed rc=0 in 364 s,
produced 180,213,024 bytes, inserted 33,668 hot stubs shared 428,391 times over
2 iterations, emitted only the 2 known FDE-size `BOLT-ERROR`s and zero
assertions, and compiled all 80 held-out files byte-identically to baseline.

`perf stat -r 5 -e cycles:u,instructions:u`, compiling the 10 largest held-out
files:

| | cycles | instructions | elapsed | IPC |
|---|---|---|---|---|
| baseline | 1,387,148,963 (+/-0.40%) | 679,415,077 (+/-0.01%) | 0.47158 s | 0.49 |
| `llc.plain` | 1,932,358,546 (+/-0.30%) | 772,585,461 (+/-0.01%) | 0.65259 s | 0.40 |
| change | **+39.3%** | **+13.7%** | **+38.4%** | worse |

Against configuration A (profile + block reordering): +38.5% cycles, +14.4%
instructions. So of the 14.4 points of extra instructions, **13.7 are already
present before any optimisation pass runs.** Layout contributes ~0.7 points, and
the stub counts say why: 33,668 versus 33,734, a difference of 0.2%. The number of
stubs is decided by the rewrite, not by layout.

Also worth recording: BOLT's dyno-stats claimed an instruction *reduction* for
both llc and clang. It excludes stub cost, so it points the wrong way. Treat it as
"what layout would save if calls were free", not as a prediction.

### The mechanism is already in our own code

Two PPC64 rules in `bolt/lib/Passes/LongJmp.cpp` together produce exactly this,
and both are currently unconditional.

`createNewStub` selects the stub form with no distance test at all:

```cpp
bool UseLongJmp = BC.isPPC64() && (TgtIsFunc || IsCall);
```

Every stub for a call target is therefore the full 7-instruction indirect
sequence (`lis/ori/rldicr/oris/ori r12, mtctr, bctr`) -- 28 bytes and 7
instructions where a reachable target needs 4 bytes and 1. `StubBits` is then set
to 64, which makes `relaxStub()` return early, so a stub that turns out to be in
range is never shortened.

`needsStub` returns true *before* any range arithmetic for calls whose target
name contains `.plt_call.`/`.plt_branch.` and for calls to any ignored function,
and subtracts a further 1 MB safety margin from the +/-32 MiB budget for all
calls.

So a large share of the 428,391 stub-routed call sites are likely not distant
calls; they are categorically stubbed, given the most expensive stub form, with
relaxation disabled. Seven instructions instead of one on hot paths is the right
order of magnitude for +13.7%.

### Sections 20 and 22 are the same bug

The comment justifying the unconditional long jump says why it exists:

> A single `b target` (26-bit +/-32MB) stub created in the final LongJmpPass
> iteration has BBAddresses set to the hot SOURCE address (stale), so relaxStub()
> sees it as within range and skips long-jump conversion. At JITLink time the
> CallBranchDelta fixup finds the true 34MB displacement and rejects it with
> "out of range".

The same staleness is noted again around `LongJmp.cpp:1257`. The 28-byte
always-stub is a workaround for addresses the relaxation loop cannot trust -- and
it costs 13.7% **and still does not prevent the failure in section 20**, because
function reordering produces a call that misses the stub path entirely.

That sets the order of work. Making the addresses read by `needsStub()` and
`relaxStub()` accurate in the final iteration is the prerequisite; porting the
AArch64 call relaxation pass (#173952, `--relax-exp`) on top of stale addresses
would likely reproduce the same class of bug. The cheap confirming measurement,
which needs no build, is to bucket the 33,668 stubs under `-debug-only=longjmp`
into: forced by the PLT rule, forced by the ignored-function rule, forced by the
1 MB margin, and genuinely beyond +/-32 MiB. If the last bucket is a small
minority -- and the plain rewrite does not move functions relative to one another
at all -- the tax is self-inflicted and recoverable.

### A harness lesson, again

The first byte-compare of `llc.plain` reported `identical: 0, differ: 80`. Before
reporting that, one file was diffed by hand: `diff` said
`base/addc.s: No such file or directory`. The baseline files are named
`addc.ll.s` -- the baseline loop used `basename "$f"` and kept the `.ll`, the new
script used `basename "$f" .ll` and dropped it. Every `cmp` compared against a
file that did not exist and therefore reported a difference. The BOLT run was
fine; the check was broken.

Same shape as the `/dev/null` near-miss in section 15 and the vacuous-test problem
in section 19. When a result is uniformly catastrophic -- 0 of 80 rather than 3 of
80 -- suspect the harness before the code.
