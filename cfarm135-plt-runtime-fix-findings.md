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

### 14. Criterion 3 is a separate, still-open bug

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
