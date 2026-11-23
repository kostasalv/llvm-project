## SYNTHETIC test (hand-made stub, linked with ld.lld): a call through a
## `.long_branch.` stub that reaches the local entry of a moved function goes
## straight to that local entry. GNU ld makes the real stubs; see
## long-branch-stub-gnu-ld.test.
##
## The stub is one `b` to f's local entry (f+8). Its relocation, against
## .text + 8, is what gives f+8 a label in BOLT (RewriteInstance::
## handleRelocation); nothing else refers to f+8, so no other function is needed
## to make the label.

# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu -I %S/Inputs \
# RUN:   %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -e _start --emit-relocs
# RUN: %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt --lite=0 2>&1 | FileCheck %s
# RUN: llvm-nm %t.bolt > %t.txt
# RUN: llvm-objdump -d --no-show-raw-insn %t.bolt --disassemble-symbols=c_f >> %t.txt
# RUN: FileCheck %s --check-prefix=OUT --input-file=%t.txt
# RUN: %t.bolt

# CHECK: BOLT-INFO: long_branch stubs: 1 calls redirected, skipped 0 (target not moved), 0 (target ignored), 0 (entry rule), 0 (no-label-at-target), 0 (not a plain b), 0 (stub not recognised), 0 (not a valid stub start)

## The call goes to f's local entry: no stub, no relay in c_f.
# OUT: [[#%x,F:]] T f
# OUT-LABEL: <c_f>:
# OUT: bl 0x[[#%x,F+8]] <f+0x8>
# OUT-NOT: <c_f+

        .include "long-branch-stub.inc"
        CALLEE f
        CALLER c_f, x00000001.long_branch.f
        STUB x00000001.long_branch.f, .Lf_lep
        START c_f
