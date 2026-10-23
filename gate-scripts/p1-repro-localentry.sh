#!/bin/bash
# p1-repro-localentry.sh: run p1-repro-localentry.s on cfarm135, through bolt-run.sh.
# Usage: p1-repro-localentry.sh   (expects ~/rollout/p1-repro-localentry.s and
#        ~/rollout/bolt-run.sh with the BOLT_INPUT mode)
# llvm-bolt is started only by bolt-run.sh, which holds the lock.
# Controls (the verdict line says INVALID unless both hold):
#   R1  c_r1 shows "# TAILCALL" on its b after disassembly (the harness can see a mark)
#   R5  c_r5 shows no "# TAILCALL" and no "cannot relax" warning
# Data per case: the mark after disassembly, the first printed pass where the
# mark is gone, the "cannot relax" warning, the "internal call" warning.
set -u
BIN=$HOME/llvm-build/bin
W=$HOME/rollout/p1/repro-le
mkdir -p "$W"
for T in llvm-mc ld.lld llvm-bolt; do
  [ -x "$BIN/$T" ] || { echo "ABORT: $BIN/$T missing"; exit 1; }
done
"$BIN/llvm-mc" -filetype=obj -triple powerpc64le-unknown-linux-gnu \
  "$HOME/rollout/${SRC:-p1-repro-localentry.s}" -o "$W/t.o" || exit 1
"$BIN/ld.lld" "$W/t.o" -o "$W/t.exe" -e _start --emit-relocs || exit 1
"$BIN/llvm-objdump" -d --no-show-raw-insn \
  --disassemble-symbols=c_r1,c_r2,c_r3,c_r4,c_r5,c_r6a,c_r6b,c_r7,c_r8,c_r9 "$W/t.exe" > "$W/orig.dis"
"$BIN/llvm-nm" "$W/t.exe" | grep -E ' (simp_|ign_|c_r)' > "$W/nm.txt"

# SRC=<file in ~/rollout> picks another assembly input (default p1-repro-localentry.s).
# EXTRA_BOLT_ARGS="..." adds llvm-bolt flags (unquoted, split on spaces).
# SKIP_EXTRA=c_r6a,c_r6b (optional) leaves those callers untouched: the JITLink experiment.
SKIP=simp_g,simp_l${SKIP_EXTRA:+,$SKIP_EXTRA}
BOLT_INPUT="$W/t.exe" "$HOME/rollout/bolt-run.sh" "${RUN_TAG:-repro-le}" 16 \
  --relocs --relax-exp --lite=0 -v=1 --skip-funcs=$SKIP \
  -print-disasm -print-longjmp -print-all -print-only='c_r.*' ${EXTRA_BOLT_ARGS:-} > "$W/run${RUN_TAG:+-$RUN_TAG}.txt" 2>&1
RC=$?
tail -3 "$W/run${RUN_TAG:+-$RUN_TAG}.txt"
DIR=$(sed -n 's/^run dir: //p' "$W/run${RUN_TAG:+-$RUN_TAG}.txt")
[ -n "$DIR" ] && [ -f "$DIR/bolt.out" ] || { echo "ABORT: no run dir (bolt-run exit $RC)"; cat "$W/run${RUN_TAG:+-$RUN_TAG}.txt"; exit 1; }
O=$DIR/bolt.out
echo "run dir: $DIR  bolt-run exit: $RC"
echo "== nm"; cat "$W/nm.txt"
echo "== original branches"; grep -E '<c_r|:[[:space:]]+(b|bl) ' "$W/orig.dis"

# For one case: walk the "Binary Function "<f>" after <pass>" dumps in order,
# and print, per pass, how many b/bl lines and how many carry # TAILCALL.
walk() {
  awk -v f="$1" '
    /^Binary Function "/ { cur = ($0 ~ "^Binary Function \"" f "(/[0-9]+)?(\\(\\*[0-9]+\\))?\"") ? $0 : ""; if (cur!="") { p=$0; sub(/.*" after /,"",p); sub(/ *$/,"",p); n++; pass[n]=p; b[n]=0; t[n]=0 } next }
    /^End of Function/ { cur=""; next }
    cur!="" && /^[[:space:]]+[0-9a-f]+:[[:space:]]+b(l)?[[:space:]]/ { b[n]++; if ($0 ~ /# TAILCALL/) t[n]++ }
    END { for (i=1;i<=n;i++) printf "    %-40s branches=%d tailcall=%d\n", pass[i], b[i], t[i] }' "$O"
}
for F in c_r1 c_r2 c_r3 c_r4 c_r5 c_r6a c_r6b c_r7 c_r8 c_r9; do
  echo "== $F"
  walk "$F"
  echo "    cannot-relax: $(grep -a 'cannot relax' "$O" | grep -ac "from $F[/ (]")"
  echo "    internal-call: $(grep -a 'internal call detected' "$O" | grep -ac "function $F\(\$\|[/ (]\)")"
  echo "    last dump of $F:"
  awk -v f="$F" '/^Binary Function "/{g=($0 ~ "^Binary Function \"" f "(/[0-9]+)?(\\(\\*[0-9]+\\))?\""); if(g){buf=""}} g{buf=buf $0 "\n"} /^End of Function/{if(g)last=buf; g=0} END{printf "%s", last}' "$O" \
    | grep -E 'after|IsSimple|:[[:space:]]+b(l)?[[:space:]]' | sed 's/^/      /'
done
echo "== all cannot-relax lines"; grep -a 'cannot relax' "$O" | cut -c1-200
echo "== LongJmp lines"; grep -a 'LongJmp\|thunk\|relaxation' "$O" | cut -c1-200

first_tc() { walk "$1" | awk 'NR==1{print ($0 ~ /tailcall=[1-9]/) ? 1 : 0}'; }
R1=$(first_tc c_r1)
R5T=$(walk c_r5 | grep -c 'tailcall=[1-9]')
R5W=$(grep -a 'cannot relax' "$O" | grep -ac 'from c_r5[/ (]')
if [ "$R1" = 1 ] && [ "$R5T" = 0 ] && [ "$R5W" = 0 ]; then
  echo "CONTROLS OK: R1 marked after disassembly; R5 no mark, no warning"
else
  echo "VERDICT: INVALID - R1 first-dump tailcall=$R1, R5 tailcall dumps=$R5T, R5 warnings=$R5W"
fi
