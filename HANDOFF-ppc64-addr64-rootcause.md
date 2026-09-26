# PPC64 BOLT: `.init_array` root cause fixed — resume point

Paused 2026-09-26 ~15:50 UTC for a laptop restart. Nothing is left running on
cfarm135 or cfarm14: every remote command ran in the foreground of an `ssh`
session, so they died with the disconnect. Nothing needs cleaning up before the
restart.

## Landed and verified

Branch `bolt-ppc-fix-plt-runtime`, pushed to `origin`:

| commit | what |
|---|---|
| `5f9d4365b05a` | stop discarding the addend of `R_PPC64_ADDR64` in `Relocation::createExpr` + new test `bolt/test/PPC64/addr64-data-addend.s` |
| `8a4c11d3f08e` | `extractValuePPC64()` returns `Contents` for `R_PPC64_ADDR32`/`ADDR64` instead of `0` — **the actual root cause** |

Full write-up: §12 of `cfarm135-plt-runtime-fix-findings.md`.

## Definition-of-done status

| # | criterion | result |
|---|---|---|
| 1 | optimised clang starts cleanly 5/5 | **PASS** — 5/5 `rc=0` (was 5/5 `rc=139`) |
| 2 | no increase in the `R_PPC64_REL24` clobber pattern | **PASS** — `ps_msub`/`vpmsumh` count 0 baseline, 0 BOLTed |
| 3 | byte-identical object for a large C++ TU | **FAIL** — under diagnosis, see below |
| 4 | `check-bolt` has no PowerPC-specific failures | PASS (earlier run: 462 passed / 20 failed, 4 fixed, 0 regressions) |

Supporting evidence for 1: the BOLTed clang's `.init_array` now holds
**486 entries, 486 distinct, 0** pointing at the old `.text` base `0x10780060`
and **0** below `0x1000`. Before the fix all 486 slots were `0x10780060`.

PPC64 lit tests on cfarm135: **10/10**, including the new reproducer.

AGENTS.md cross-target gate on cfarm14 (both commits touch
`bolt/lib/Core/Relocation.cpp`, i.e. outside `bolt/lib/Target/PowerPC/`):
`bolt/test/X86` + `bolt/test/AArch64` at `bc4ce4e128d6` vs `8a4c11d3f08e` —
296 passed / 164 failed on both sides, and the two FAIL lists are **identical
test-for-test**. The raw `diff` looked dirty only because lit's `(N of 472)`
progress index is scheduling order; `cfarm14-gate.sh` now strips it.

## Criterion 3: where the diagnosis stands

`llvm/lib/Target/X86/X86ISelLowering.cpp` (2.6 MB, picked automatically as the
largest C++ TU in `compile_commands.json`), compiled with `-resource-dir`
pinned to the same value for both binaries so that only the optimised code
differs.

Established:

* Both compilers are **deterministic** — two runs of each are byte-identical,
  so this is not an ASLR/pointer-order artefact.
* Object sizes 4091248 (base) vs 4091256 (BOLTed), +8 bytes.
* Same section count (3283) and same `FUNC` symbol count (1392).
* Exactly one section changes size: `.data.rel.ro..L_MergedGlobals.3020`,
  `0x7fc6` → `0x7fcf` (+9). The first differing byte is file offset 40, which
  is `e_shoff` — pure downstream shift.
* The assembly diff is 4723 changed lines and is **entirely** merged-global
  layout: 3846 `addi rX, rY, <imm>`, 10 `.size`, 8 `.asciz`, 4 `lhz`, 2 `ld`,
  and the `.L.str.N = .L_MergedGlobals.M+offset` aliases.
* Nothing is dropped. `.L.str.512` is present in both; it simply moves from
  `.L_MergedGlobals.3019+20564` to `+24022`.

The obvious reading — that `llvm/lib/CodeGen/GlobalMerge.cpp` picks a different
partition — was **tested and falsified** as the last thing before the pause.
Recompiling both with `-mllvm -enable-global-merge=false`:

```
nogm-base.o  4243728 bytes
nogm-bolt.o  4243744 bytes   (+16)
RESULT[nogm]=DIFFER  1160073 differing bytes, first at offset 40 (e_shoff)
```

So the divergence is **upstream of GlobalMerge**; the merged-global immediates
were only where it surfaced. The `-S` diff needs redoing with the pass off, so
that the layout noise is gone and the real difference is visible.

Resume here:

1. `-S` from both with `-mllvm -enable-global-merge=false`, then diff. With
   3846 `addi` immediates removed from the picture the residual diff should be
   small and should name a function.
2. Then narrow by section: `llvm-readelf -SW` on `nogm-base.o` vs
   `nogm-bolt.o`, sorted, to find which section grew by 16 this time. The two
   size deltas (+9 with the pass on, +16 with it off) are the cheapest handle
   on what is actually changing.
3. The cfarm135 build has assertions, so `-mllvm -debug-only=...` and
   `-mllvm -print-after-all` both work under either binary. Diffing a
   `-print-after-all` trace localizes the first pass whose output differs, and
   that names the miscompiled code. It is large but decisive; pipe to a file in
   `/tmp` and diff there rather than pulling it back.
4. Worth keeping in mind: both binaries are deterministic and produce
   semantically equivalent code, so whatever is wrong is a *value* that is
   computed slightly differently, not a crash — most likely a comparator, a
   size/alignment computation, or a hash.

## Scripts (local, `/Users/konstantinosalvertis/LLVM/`)

| script | deployed as | does |
|---|---|---|
| `build-lit-ppc64.sh` | `/tmp/kosta-build-lit.sh` | pull, `ninja -j16 llvm-bolt`, PPC64 lit |
| `cfarm14-gate.sh` | `/tmp/kosta-c14-gate.sh` | the x86_64 + AArch64 two-point FAIL-list gate |
| `validate-addr64-fix.sh` | `/tmp/kosta-validate-addr64.sh` | re-BOLT clang, 5 starts, `.init_array` scan (its inline python needs py3.6-safe `subprocess`; the fixed copy is in `criterion23.sh`) |
| `criterion23.sh` | `/tmp/kosta-crit23.sh` | `.init_array` scan + criteria 2 and 3 |
| `criterion3-diagnose.sh` | `/tmp/kosta-crit3d.sh` | determinism control + which section changed size |
| `criterion3-asm-diff.sh` | `/tmp/kosta-crit3s.sh` | `-S` from both, diff the assembly |
| `criterion3-localize.sh` | `/tmp/kosta-crit3l.sh` | classify the diff, then disable GlobalMerge |

`criterion3-localize.sh` sources `/tmp/kosta-crit23/cmd.sh` for the TU and its
flags, so run `criterion23.sh` first on a fresh machine.

## Still open (unchanged by this work)

* Why six ctors (`X86PostLegalizerCombiner`, `X86PreLegalizerCombiner`,
  `RegAllocFast`, `RegAllocBasic`, `PassTimingInfo`, `ScheduleDAGVLIW`) are
  emitted with **zero** symbols while a healthy one gets three. Suspects: the
  two silent `setIgnored()` calls at `RewriteInstance.cpp:1383-1384` and
  `:4330-4331`, plus the silent `setSimple(false)` at `:4320`. Missed
  optimisation, not correctness — but it emits no diagnostic at all.
* FDE-derived size inflation for `.plt_branch.`/`.plt_call.`/`.long_branch.`
  stub symbols (`RewriteInstance.cpp:1298-1313`, `setMaxSize` at `:2307`) — 3
  `symbol seen in the middle` errors.
* `getSizeForTypePPC64` reports `R_PPC64_REL32` as 8 bytes; it is 4.
* `BOLT-WARNING: Failed to analyze 10332 relocations` — expected to drop now
  that `ExtractedValue` is right for `ADDR32`/`ADDR64`; re-measure.
* `PPCMCSymbolizer.cpp` is dead code (§11 of the findings doc).
* Feedback owed to the CLI agent on all of the above.

## Scratch to clean up on cfarm135 when convenient

`/tmp/kosta-*` (scripts, `crit23/`, `crit3d/`, `crit3l/`, `crit3s/`, `a64/`,
`clang{,2,3}.bolt`, `v{1..5}.txt`, `clangbolt*.log`, `sym*.txt`, `obj-*.txt`,
`ia-rel.txt`, `ctors-*.txt`), `/tmp/output-ad9848.o`, `/tmp/output-8d6f96.o`,
`~/llvm-build/bin/clang-bolted`, and any core dumps. `/tmp/kosta-clang3.bolt`
is 338 MB and is the artefact criterion 3 is being diagnosed against — keep it
until that is settled.

Local terminal tabs to close: c123–c132.
