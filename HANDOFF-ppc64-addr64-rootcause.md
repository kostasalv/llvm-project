# PPC64 BOLT: the definition of done is met — state as of 2026-09-26 19:30 UTC

Nothing is left running on cfarm135 or cfarm14: every remote command ran in the
foreground of an `ssh` session, so they died with the disconnect.

## All four criteria pass at branch head `5473a91b5132`

Re-measured from scratch at the head commit, not carried over from the earlier
pass — `5473a91b5132` changes a relocation width, so every criterion had to be
re-run against an `llvm-bolt` built at head and a freshly BOLTed clang
(`/tmp/kosta-clang4.bolt`, 337991184 bytes, `rc=0`).

| # | criterion | result |
|---|---|---|
| 1 | optimised clang starts cleanly 5/5 | **PASS** — 5/5 `rc=0` (was 5/5 `rc=139`) |
| 2 | no increase in the `R_PPC64_REL24` clobber pattern | **PASS** — `ps_msub`/`vpmsumh` 0 baseline, 0 BOLTed |
| 3 | byte-identical object for a large C++ TU | **PASS** — md5 `b7b5cd0a842e6d6072b97bb2c0535e64` on both |
| 4 | `check-bolt` has no PowerPC-specific failures | **PASS** — PPC64 lit 10/10 |

`.init_array` in the BOLTed clang: **486 entries, 486 distinct, 0** at the old
`.text` base `0x10780060`, **0** below `0x1000`. Before the fix all 486 slots
held `0x10780060`.

## Commits on `bolt-ppc-fix-plt-runtime`

| commit | what |
|---|---|
| `bc4ce4e128d6` | do not share stubs across functions without relocations |
| `5f9d4365b05a` | keep the addend of `R_PPC64_ADDR64` in `Relocation::createExpr` + test `bolt/test/PPC64/addr64-data-addend.s` |
| `8a4c11d3f08e` | `extractValuePPC64()` returns `Contents` for `R_PPC64_ADDR32`/`ADDR64` instead of `0` — **the `.init_array` root cause** |
| `549f8b34c891` | findings: the ADDR64 root cause and the criterion-3 state |
| `5473a91b5132` | report `R_PPC64_REL32` as 4 bytes + `bolt/unittests/Core/Relocation.cpp` |

Every one of these touches files outside `bolt/lib/Target/PowerPC/`, so all of
them have been through the AGENTS.md cross-target gate on cfarm14:
`bolt/test/X86` + `bolt/test/AArch64` at `549f8b34c891` vs `5473a91b5132` —
296 passed / 164 failed on both sides, FAIL lists identical test-for-test,
`DIFF_RC=0`. `CoreTests` 50/50 including the 4 new `RelocationTester` cases.
(The raw `diff` looks dirty unless lit's `(N of 472)` progress index is
stripped; `cfarm14-gate.sh` does that.)

## Criterion 3 was the harness, not the compiler

Full write-up in §15 of `cfarm135-plt-runtime-fix-findings.md`. In short:
clang derives the **GCC installation prefix from the driver executable's own
directory** when `--gcc-toolchain` is absent, so the baseline in
`~/llvm-build/bin` searched `/usr/lib/gcc/ppc64le-redhat-linux/8/...` while the
BOLTed copy in `/tmp` searched `/tmp/../lib/gcc/ppc64le-redhat-linux/8/...`.
The TU is built with `-D_GLIBCXX_ASSERTIONS`, so `assert()` embeds those header
paths as string literals; `/usr` → `/tmp/..` is +3 bytes on each of four
strings, which with padding is the whole +16 object delta, and with GlobalMerge
on it shifted 3846 merged-global immediates.

**Pinning `-resource-dir` is not enough — it does not touch the GCC prefix.**
Any future A/B of two clang binaries at different paths must pin
`--gcc-toolchain=/usr` as well. With that one flag added and no `-mllvm` knobs
at all, the objects are byte-identical.

Two lessons worth keeping:

* Pin *every* driver-derived path, not just the one you thought of.
* When a diff is dominated by one mechanical pattern, re-take it with that
  mechanism disabled before drawing a conclusion. Disabling GlobalMerge cut the
  `-S` diff from 4723 lines to 24, and those 24 named the cause outright.

## The REL32 fix is latent on this workload

`5473a91b5132` is correct against the ELFv2 ABI (`R_PPC64_REL32` is `word32*`,
Figure 4-1) and fixes two real consumers, but it changed nothing measurable
here: `Failed to analyze 10332 relocations` is byte-for-byte unchanged. Reason,
measured: all 159744 `R_PPC64_REL32` relocations in `clang-24` live in
`.rela.eh_frame` (226031 entries), and `RewriteInstance::readRelocations()`
returns early for `.eh_frame` at `RewriteInstance.cpp:3297`. The 10332
unanalyzed relocations therefore have a different cause, still to be found.

## Still open

* `Failed to analyze 10332 relocations` — **not** REL32-related (above).
* Why six ctors (`X86PostLegalizerCombiner`, `X86PreLegalizerCombiner`,
  `RegAllocFast`, `RegAllocBasic`, `PassTimingInfo`, `ScheduleDAGVLIW`) are
  emitted with **zero** symbols while a healthy one gets three. Suspects: the
  two silent `setIgnored()` calls at `RewriteInstance.cpp:1383-1384` and
  `:4330-4331`, plus the silent `setSimple(false)` at `:4320`. Missed
  optimisation, not correctness — but it emits no diagnostic at all.
* FDE-derived size inflation for `.plt_branch.`/`.plt_call.`/`.long_branch.`
  stub symbols (`RewriteInstance.cpp:1298-1313`, `setMaxSize` at `:2307`) — the
  3 `symbol seen in the middle` errors, which are the only `BOLT-ERROR` lines
  in the log. There are no assertion failures.
* The warning volume: 5690 `internal call detected`, 5619 `unable to disassemble
  instruction at offset`, 299 `failed to patch entries in`, 167 `corrupted
  control flow detected`, 20 `unclaimed data relocation`, and
  `ignoring symbol __bss_start ... which lies outside .bss`.
* `PPCMCSymbolizer.cpp` is dead code (§11 of the findings doc).
* Why cfarm14 shows 164 X86+AArch64 failures where the same tree once showed
  ~29; and how the cfarm135 FAIL list compares with cfarm120's 34 pre-existing
  non-PPC64 failures.
* Feedback owed to the CLI agent on all of the above, plus the `--gcc-toolchain`
  harness lesson, the dead specifier dispatch, the glibc `<elf.h>` macro
  collision, and the `.LStub2` cross-function shared-stub regression.
* Next per the user's roadmap: rebase onto current upstream `main`, then look at
  a call-relaxation/clustering pass (`--compact-code-model` first, with a ±32 MB
  PPC64 branch budget). `--relax-exp` / CallRelaxation is not in this tree.

## Scripts (local, `/Users/konstantinosalvertis/LLVM/`)

| script | deployed as | does |
|---|---|---|
| `build-lit-ppc64.sh` | `/tmp/kosta-build-lit.sh` | pull, `ninja -j16 llvm-bolt`, PPC64 lit |
| `cfarm14-gate.sh` | `/tmp/kosta-c14-gate.sh` | the x86_64 + AArch64 two-point FAIL-list gate, plus `CoreTests` |
| `validate-head-dod.sh` | `/tmp/kosta-dod.sh` | **all four criteria in one run** — use this one |
| `criterion23.sh` | `/tmp/kosta-crit23.sh` | `.init_array` scan + criteria 2 and 3; writes `/tmp/kosta-crit23/cmd.sh` |
| `criterion3-final.sh` | `/tmp/kosta-crit3f.sh` | prints both drivers' include search paths, then the `--gcc-toolchain` A/B |
| `criterion3-nogm-asm.sh` | `/tmp/kosta-crit3n.sh` | the probe that cracked criterion 3 (`-S` with GlobalMerge off). Its `sed`-based readelf column extraction is crude — it emitted 40 bogus `.group 000008 -> 00000c` lines; ignore that part |

Several scripts source `/tmp/kosta-crit23/cmd.sh` for the TU and its flags, so
run `criterion23.sh` first on a fresh machine. `validate-addr64-fix.sh` still
has py3.6-incompatible inline python (`capture_output=`); the working version is
in `validate-head-dod.sh`.

## Scratch to clean up on cfarm135 when convenient

`/tmp/kosta-*` (scripts, `crit23/`, `crit3d/`, `crit3f/`, `crit3l/`, `crit3n/`,
`crit3s/`, `dod/`, `a64/`, `clang{,2,3,4}.bolt`, `v{1..5}.txt`,
`clangbolt*.log`, `sym*.txt`, `obj-*.txt`, `ia-rel.txt`, `ctors-*.txt`),
`/tmp/output-ad9848.o`, `/tmp/output-8d6f96.o`, `~/llvm-build/bin/clang-bolted`,
and any core dumps. Criterion 3 is settled, so `/tmp/kosta-clang3.bolt` is no
longer needed; `/tmp/kosta-clang4.bolt` (338 MB) is the artefact all four
criteria were measured against — keep it only if you want to re-check without
re-BOLTing (~20 min). On cfarm14: untracked `CMakeFiles/`, `CPackConfig.cmake`,
`CPackSourceConfig.cmake`, `update-pr-branch.sh` in the build dir.
