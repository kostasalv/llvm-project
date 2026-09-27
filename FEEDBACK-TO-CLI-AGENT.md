# Feedback for the Claude CLI agent working on the PPC64 BOLT port

State this is written against: `bolt-ppc-fix-plt-runtime` @ `40af17aa0d47`,
pushed to `origin`. All four definition-of-done criteria for the optimised clang
on cfarm135 pass and are re-measured at that head. Everything below is either a
bug I found and fixed (with the commit), a bug I found and did **not** fix, or a
process lesson that cost real time.

Evidence for every claim is in `cfarm135-plt-runtime-fix-findings.md`; section
numbers are given. Where I say "measured", there is a log line behind it. Where
I was wrong earlier, I say so — the wrong turns are the useful part.

---

## 1. Bugs fixed — please don't reintroduce these patterns

### 1.1 `extractValuePPC64()` returned literal `0` for `ADDR32`/`ADDR64`

**This was the root cause of the BOLTed clang segfaulting 5/5 times.** (§12,
commits `5f9d4365b05a` + `8a4c11d3f08e`.)

`Relocation::extractValue()` feeds `analyzeRelocation()`, which uses the
extracted value to normalize a **section symbol** into
`(section symbol + offset)`. With `0` coming back, that normalization could not
happen, so `analyzeRelocation` fell through to `Addend = -SymbolAddress`, and
every one of the 486 `.init_array` slots resolved to the `.text` base
`0x10780060`. `__libc_csu_init` then called into the middle of a function with
`ctr == r12 == 0x13587f58`.

The lesson that generalizes: **an `extractValue` that returns a placeholder is
not neutral.** It is not "we don't need this value on PPC64" — it silently
changes how `analyzeRelocation` classifies the relocation. If a target genuinely
cannot extract a value for a type, that needs to be an explicit
unreachable/diagnostic, not a `0`.

Second half of the same bug: `Relocation::createExpr` discarded the addend for
`R_PPC64_ADDR64`. Test: `bolt/test/PPC64/addr64-data-addend.s`.

### 1.2 `getSizeForTypePPC64` reported `R_PPC64_REL32` as 8 bytes

(§16, commit `5473a91b5132`.) It is `word32*` — ELFv2 ABI Figure 4-1, value 26,
calculation `S + A - P`. Two consumers read that width and both were wrong:

* `RewriteInstance::analyzeRelocation()` passes it to
  `getUnsignedValueAtAddress()`, so an overlong size pulls four bytes of the
  *following* field into the extracted value and the check against
  `truncateToSize(SymbolAddress + Addend - PCRelOffset, RelSize)` cannot match;
* `BinarySection::emitAsData()` steps over the width of each relocation, so an
  overlong size swallows four bytes of real data.

**But be honest about its impact, because I nearly wasn't.** The fix changed
nothing measurable on clang: `Failed to analyze 10332 relocations` is
byte-for-byte unchanged. Measured why — all 159744 `R_PPC64_REL32` relocations
in `clang-24` live in `.rela.eh_frame` (226031 entries), and
`RewriteInstance::readRelocations()` returns early for `.eh_frame` and
`.gcc_except_table` at `RewriteInstance.cpp:3297`:

```cpp
  const bool SkipRelocs = StringSwitch<bool>(RelocatedSectionName)
                              .Cases({".plt", ".rela.plt", ".got.plt",
                                      ".eh_frame", ".gcc_except_table"}, true)
                              .Default(false);
```

So it is a latent-bug fix. Correct per the ABI, worth having, buys nothing here.
If you are looking at the 10332 figure, **it is not REL32** — start elsewhere.

### 1.3 Stubs shared across functions without relocations

(§9, commit `bc4ce4e128d6`.) A `.LStub2` was reachable from two functions; with
`CheckLargeFunctions` that surfaced as a size assertion. Do not share a stub
across functions when the input has no relocations for them.

### 1.4 The relocation tables are unit-testable with no hardware

`Relocation::Arch` is a **public static** (`Relocation.h:52`, defined
`Relocation.cpp:34`) set by the BinaryContext ctor, and `getSizeForType` /
`isPCRelative` dispatch only on it. So every architecture's relocation table can
be tested with **no target backend and no PowerPC machine** — just save/restore
`Relocation::Arch` in a fixture. `bolt/unittests/Core/Relocation.cpp` does that
in four tests, one of which
(`PCRelative32BitIsFourBytesEverywhere`) pins exactly the cross-architecture
discrepancy that 1.2 was: x86-64 `PC32` == 4, AArch64 `PREL32` == 4, so PPC64
`REL32` == 4. **Please add to this file rather than testing relocation widths
end-to-end on hardware** — it runs in 0 ms and it catches this class of bug on
any developer's laptop.

---

## 2. Found, not fixed — open work, roughly in order of value

1. **`Failed to analyze 10332 relocations`.** Cause unknown, and now known not
   to be REL32 (1.2). This is the largest remaining correctness-shaped unknown.
2. **Six ctors emitted with zero symbols** while a healthy one gets three:
   `X86PostLegalizerCombiner`, `X86PreLegalizerCombiner`, `RegAllocFast`,
   `RegAllocBasic`, `PassTimingInfo`, `ScheduleDAGVLIW`. Suspects are the two
   silent `setIgnored()` calls at `RewriteInstance.cpp:1383-1384` and
   `:4330-4331` and the silent `setSimple(false)` at `:4320`
   (`hasDynamicRelocationAtIsland()`). Missed optimisation rather than
   miscompilation — but **it emits no diagnostic at all**, which is the real
   defect. Make it diagnosable first, then decide.
3. **FDE-derived size inflation for PPC64 linker stubs**
   (`.plt_branch.`/`.plt_call.`/`.long_branch.`) — `RewriteInstance.cpp:1298-1313`
   and `setMaxSize(Function.getSize())` at `:2307`. This produces the only
   `BOLT-ERROR` lines in a clang run, 3 of them
   (`symbol seen in the middle of the function 00000025.plt_branch._ZNK5clang20TextDiagnosticBuffer16FlushDiagnosticsERNS_17DiagnosticsEngineE/1`
   and two more). There are **no assertion failures** — don't read these as such.
4. **`PPCMCSymbolizer.cpp` is dead code** (§11): `PPCDisassembler.cpp` never
   calls `tryAddingSymbolicOperand`. Wire it up or delete it; as it stands it
   tells the next reader that PPC64 in-body symbolization happens there.
5. **Warning volume**, in a single clang run: 5690 `internal call detected in
   function`, 5619 `unable to disassemble instruction at offset`, 299 `failed to
   patch entries in`, 167 `corrupted control flow detected in function`, 20
   `unclaimed data relocation`, plus `ignoring symbol __bss_start ... which lies
   outside .bss`. 5619 undisassemblable instructions is a lot; that is probably
   one or two missing encodings, not 5619 problems.
6. Dead specifier dispatch, and the glibc `<elf.h>` macro collision — both minor,
   both real.

---

## 3. Process lessons that cost hours

### 3.1 Pin *every* driver-derived path when A/B-ing two clang binaries

This one burned most of a day. (§15.) Criterion 3 — "compile a large C++ TU to a
byte-identical object" — failed, and it was **never a BOLT bug**.

Clang's `Generic_GCC::GCCInstallationDetector` derives the GCC installation
prefix from the **driver executable's own directory** when `--gcc-toolchain` is
absent. The baseline at `~/llvm-build/bin/clang-24` searched
`/usr/lib/gcc/ppc64le-redhat-linux/8/...`; the BOLTed copy at
`/tmp/kosta-clang3.bolt` searched `/tmp/../lib/gcc/ppc64le-redhat-linux/8/...`.
The TU is built `-D_GLIBCXX_ASSERTIONS -UNDEBUG`, so libstdc++ `assert()`
embeds those header paths as string literals. `/usr` (4 chars) → `/tmp/..`
(7 chars), +3 bytes on each of four strings = +12, which with padding is the
whole +16 object delta — and with GlobalMerge on, the longer strings shifted
every subsequent merged-global offset, producing 3846 changed `addi` immediates
and a +9 section growth.

**`-resource-dir` does not fix this.** I had pinned it and believed the
comparison was clean. Pin `--gcc-toolchain=/usr` too. With that one flag added
and no `-mllvm` knobs at all, the objects are byte-identical, md5
`b7b5cd0a842e6d6072b97bb2c0535e64` on both sides.

### 3.2 When a diff is dominated by one mechanical pattern, turn that mechanism off and re-take it

The 4723-line assembly diff looked like a codegen difference and I spent real
time reasoning about GlobalMerge partitioning — which I then correctly falsified,
and still drew the wrong conclusion from ("the divergence is upstream of
GlobalMerge", §14). What actually worked, immediately: recompile both with
`-mllvm -enable-global-merge=false` and diff again. 4723 lines → **24**, and all
24 were the four header paths. The cause was in the output the whole time,
buried under layout noise.

The user's instruction mid-investigation was *"put debug statements if they help
for diagnostics, dont speculate"*, and it was the right call: the very next
script **printed both drivers' include search paths** instead of arguing about
them, and that ended it. Prefer a probe that prints the disputed value over a
hypothesis about it.

### 3.3 Don't inherit measurements across a commit that changes behaviour

I nearly reported criteria 1–4 as passing at head using numbers taken with an
`llvm-bolt` built two commits earlier. `5473a91b5132` changes a relocation
width, which affects both extracted values and data emission, so the whole
definition of done had to be re-run. It was, and it passes — but the cheap
version of that report would have been false.

### 3.4 Check your own parsing before reporting a finding

My `llvm-readelf -SW | sed` column extraction produced 40 identical
`.group 000008 -> 00000c` lines. That was my `sed`, not the objects. Nearly
shipped it as a finding.

### 3.5 Environment specifics worth knowing

* cfarm135 has **python 3.6** — no `subprocess.run(..., capture_output=True)`.
  Use `subprocess.check_output`.
* lit's `(N of 472)` progress index is scheduling order, not a result. Diffing
  FAIL lists without stripping it invents regressions — it invented one for me.
  `sed 's/ ([0-9]* of [0-9]*)$//'`.
* `llvm-objdump -r --section=` silently prints nothing on these objects; use
  `llvm-readelf -rW | grep`.
* Remote commands run in an ssh foreground die on disconnect. Redirect to a file
  on the remote and read the file.

---

## 4. The gate that applies to this port

Per `AGENTS.md`: a PowerPC-only change under `bolt/lib/Target/PowerPC/` can land
directly, but **anything outside that directory needs x86_64 and AArch64
regression runs on cfarm14 first**. Four of the five commits on this branch touch
`bolt/lib/Core/Relocation.cpp`, so all four went through it:
`bolt/test/X86` + `bolt/test/AArch64`, two-point run at the previous cleared
commit and at head, 296 passed / 164 failed on both sides, FAIL lists identical
test-for-test, plus `CoreTests` 50/50.

Note the 164. The same tree reportedly showed ~29 failures at some earlier point
and nobody has explained the difference — it is on the open list. The gate is
still meaningful because it is a **two-point diff**, not an absolute count, but
don't quote 164 as healthy.
