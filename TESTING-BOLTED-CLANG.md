# How we test a BOLT-optimised clang on PowerPC64

This is the methodology, separate from the findings. It explains what each test
establishes, what it deliberately does not, and how to re-run it. Written for
whoever picks this up next — including the version of me that has forgotten.

## The problem this has to solve

BOLT does not patch a binary in place. In relocation mode it disassembles every
function, rebuilds each control-flow graph, discards unreachable code, inserts
branch stubs where a displacement no longer fits, and re-emits `.text` wholesale.
On a clang binary that is 159758 functions, 837 KB of code removed, and 58448
stubs inserted.

The dangerous failure mode for a port to a new architecture is **not** a crash.
A crash is a good day: you get a core file and a stack trace. The failure that
costs you months is the one where the rewritten compiler **runs perfectly and is
quietly wrong** — a comparator that orders two elements differently, a hash that
collides where it didn't, a size or alignment computed one byte off. That clang
still compiles your code. It just emits something subtly different, and you have
no way to know which of your builds to trust.

So the test suite has to be able to detect "behaves differently in a way nobody
would notice", not just "fell over".

## What does not count as evidence

`clang --version` was ruled out explicitly at the start of this work, and it is
worth saying why, because it is the tempting first check.

`--version` proves the ELF headers are sane, the entry point is reachable, the
dynamic linker resolved its libraries, and the string table survived. It exercises
almost none of the compiler. A binary whose `.init_array` is entirely corrupt can
still print its version — and in fact ours did, for a while, in between crashing
on every real compile. Any check that passes on a binary that cannot compile
anything is not a correctness check.

## The four criteria

All four must hold. They are ordered cheapest-first, which is also
weakest-first.

### 1. It starts cleanly, five times out of five

```bash
for i in 1 2 3 4 5; do /tmp/clang.bolt --version; echo "rc=$?"; done
```

Catches gross breakage: a corrupt `.init_array`, a bad entry point, a stub that
branches into nowhere. Five runs rather than one because a static-initialiser bug
can be order- or address-dependent.

This is the criterion that was failing 5/5 with `rc=139` (SIGSEGV) before the
`extractValuePPC64` fix. Repeat-count matters: it told us the crash was
deterministic, not a race, which is what pointed at static initialisers.

Supporting check worth keeping — scan `.init_array` directly:

```
486 entries, 486 distinct, 0 at the old .text base 0x10780060, 0 below 0x1000
```

Before the fix all 486 slots held the same value, `0x10780060`. "All N pointers
identical" is a much louder signal than a segfault, and it is one `readelf` away.

### 2. No increase in the known-bad instruction pattern

```bash
objdump -d "$BIN" | grep -cE 'ps_msub|vpmsumh'
```

`ps_msub` and `vpmsumh` are rare-to-absent in compiler code. They show up when a
`R_PPC64_REL24` branch displacement has been written over an adjacent instruction
word — a corrupted branch decodes as a plausible but nonsensical VMX/paired-single
op. Counting them is a cheap proxy for relocation corruption across the whole
text section. Baseline 0, BOLTed 0.

This is a **canary, not a proof**: zero hits means we didn't find that specific
corruption signature, not that no corruption exists.

### 3. Byte-identical object files — the one that actually matters

Compile real C++ source with both binaries and compare the output bit for bit:

```bash
"$BASE" -resource-dir "$RESDIR" --gcc-toolchain=/usr "${ARGS[@]}" -o base.o
"$BOLT" -resource-dir "$RESDIR" --gcc-toolchain=/usr "${ARGS[@]}" -o bolt.o
cmp base.o bolt.o
```

**Why byte-identity is the right bar.** A compiler is deterministic: the same
clang, same input, same flags must produce the same object every time. So if two
objects match to the byte, then every one of the millions of decisions clang made
along the way came out the same — every hash lookup, every sort comparison, every
size and alignment computation, every register allocation, every symbol ordering.
One pass of `cmp` covers all of it at once.

Nothing else available to us has anything like that density. "The output links and
runs" would pass a compiler that is wrong in a hundred places that happen not to
matter for that program. Byte-identity gives a hard yes/no over the entire
pipeline, and it costs one `cmp`.

The flip side is worth knowing: byte-identity is **brittle by design**. It will
flag things that are not bugs. That is a feature — you would rather investigate a
false alarm than miss a real one — but it means every difference has to be chased
to its actual cause before you conclude anything. See the trap below, which cost
about a day.

**Both `-resource-dir` and `--gcc-toolchain` must be pinned.** This is not
optional and it is not obvious.

Clang locates its own headers *and* the system GCC installation relative to
**the driver executable's own path on disk**. Two copies of the identical binary
at different paths therefore behave differently:

| binary | searches |
|---|---|
| `~/llvm-build/bin/clang-24` | `/usr/lib/gcc/ppc64le-redhat-linux/8/...` |
| `/tmp/clang.bolt` | `/tmp/../lib/gcc/ppc64le-redhat-linux/8/...` |

The TU we compare is built with `-D_GLIBCXX_ASSERTIONS -UNDEBUG`, so libstdc++
`assert()` calls embed those header paths into the object **as string literals**.
`/usr` → `/tmp/..` is three characters longer, across four libstdc++ headers:
twelve bytes, sixteen after padding. That was the entire object-size delta that
had this criterion reading as FAILED. With GlobalMerge on, the longer strings
also shifted every subsequent merged-global offset, which turned twelve bytes of
string into 3846 changed `addi` immediates and a 4723-line assembly diff that
looked exactly like a codegen bug.

`-resource-dir` alone does **not** fix this — it pins clang's own headers and
does not touch the GCC prefix. Pin both, and prefer copying the BOLTed binary
next to the baseline over reasoning about which paths leak.

### 4. No PowerPC-specific regressions in BOLT's own test suite

```bash
./bin/llvm-lit -s "$SRC/bolt/test/PPC64"      # 10/10
```

And for anything touching shared code, the two-point gate on x86_64/AArch64
hardware, per `AGENTS.md`: run `bolt/test/X86` + `bolt/test/AArch64` at the last
cleared commit *and* at the new one, and diff the FAIL lists. Compare the two
lists, never an absolute count — this tree has 164 pre-existing failures and
nobody has yet explained why it once had ~29.

Strip lit's progress index before diffing or you will invent regressions:

```bash
sed 's/ ([0-9]* of [0-9]*)$//'
```

## Widening criterion 3 from one TU to fifty

One identical object could be one lucky file. `criterion3-wide.sh` runs the same
comparison over a sample drawn evenly across the source-size range from
`compile_commands.json`, from 400-byte files up to the 2.5 MB
`X86ISelLowering.cpp`, 8 compiles at a time, deleting each object as soon as it
compares equal.

Spreading across sizes is deliberate. Small TUs exercise the driver, the
preprocessor and the trivial paths; large ones are where the optimiser, the
register allocator and the deep template machinery live. A sample of only large
files would miss driver-level bugs, and a sample of only small ones would miss
everything that matters.

Result at `b5ee5590c148`: **51/51 identical.**

## What all four together do and do not establish

They establish that a fully rewritten clang — every function through BOLT's
disassemble/rebuild/emit path — produces bit-for-bit identical code on 51 real
translation units spanning four orders of magnitude of size, and starts reliably.

They do **not** establish:

* **Anything about the profile-driven passes.** The validated run had
  `0 out of 159758 functions (0.0%) have non-empty execution profile`. No
  function reordering, no ext-TSP block layout, no splitting, no ICF ran. Those
  are the passes BOLT exists for and their PPC64 correctness is untested. Turning
  them on is new risk, not a continuation of this result.
* **That clang is correct on inputs unlike these 51.** No bootstrap, and clang's
  own test suite has never been run *using* the BOLTed clang as the compiler
  under test. Criterion 4 tests BOLT, not clang.
* **That the functions BOLT skipped are fine.** A clang run logs 5619
  undisassemblable instructions, 167 corrupted CFGs, 10332 unanalyzed
  relocations, 20 unclaimed data relocations and 3 hard errors. Those functions
  get marked non-simple and left alone. Part of the correctness above is the
  correctness of *declining to transform* them — safe for now, and a hazard the
  moment a change makes BOLT act on them.

## Re-running

| script | does |
|---|---|
| `validate-head-dod.sh` | all four criteria in one run: ff-merge, build `llvm-bolt`, PPC64 lit, re-BOLT clang, 5 starts, `.init_array` scan, criteria 2 and 3 |
| `criterion3-wide.sh` | criterion 3 across N TUs (`N=50 JOBS=8`) |
| `cfarm14-gate.sh` | the x86_64 + AArch64 two-point FAIL-list gate, plus `CoreTests` |

They live in `/Users/konstantinosalvertis/LLVM/`, are copied to `/tmp/kosta-*.sh`
on the server, and run over ssh in the foreground with output redirected to a
local log — so they die on disconnect and leave nothing running.

Two environment facts that will bite otherwise: cfarm135 has **python 3.6**
(no `subprocess.run(..., capture_output=True)`), and re-BOLTing clang takes about
20 minutes, so keep the artefact if you intend to measure it twice.

## The rule behind all of it

Never inherit a measurement across a commit that changes behaviour. The REL32
width fix altered both extracted relocation values and data emission, so the
entire definition of done was re-measured against a freshly BOLTed binary rather
than carried over from two commits earlier. The cheap version of that report
would have been false.
