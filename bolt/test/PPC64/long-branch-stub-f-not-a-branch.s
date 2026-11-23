## SYNTHETIC test (hand-made stubs, linked with ld.lld). A symbol named
## `.long_branch.` is treated as a stub only if its one instruction is a plain
## `b` (opcode 18, AA=0, LK=0). Here one such symbol holds `blr` and another a
## `bl`: a call to either is left alone, and the pass counts it as "not a plain
## b" and redirects nothing. Without that check the pass would read the
## displacement field of an instruction that is not a branch and rewrite the
## call to a place the stub never reaches.
## The third case is the control: the same program with a plain `b` as the stub
## is redirected, so the counter line differs between the cases.

# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu -I %S/Inputs \
# RUN:   --defsym CASE_BLR=1 %s -o %t.r.o
# RUN: ld.lld %t.r.o -o %t.r.exe -e _start --emit-relocs
# RUN: %t.r.exe
# RUN: llvm-bolt %t.r.exe -o %t.r.bolt --lite=0 2>&1 | FileCheck %s --check-prefix=BLR
# RUN: %t.r.bolt
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu -I %S/Inputs \
# RUN:   --defsym CASE_BL=1 %s -o %t.l.o
# RUN: ld.lld %t.l.o -o %t.l.exe -e _start --emit-relocs
# RUN: %t.l.exe
# RUN: llvm-bolt %t.l.exe -o %t.l.bolt --lite=0 2>&1 | FileCheck %s --check-prefix=BL
# RUN: %t.l.bolt
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu -I %S/Inputs \
# RUN:   --defsym CASE_B=1 %s -o %t.b.o
# RUN: ld.lld %t.b.o -o %t.b.exe -e _start --emit-relocs
# RUN: %t.b.exe
# RUN: llvm-bolt %t.b.exe -o %t.b.bolt --lite=0 2>&1 | FileCheck %s --check-prefix=PLAIN
# RUN: %t.b.bolt

# BLR: BOLT-INFO: long_branch stubs: 0 calls redirected, skipped 0 (target not moved), 0 (target ignored), 0 (entry rule), 0 (no-label-at-target), 1 (not a plain b), 0 (stub not recognised), 0 (not a valid stub start)
# BL: BOLT-INFO: long_branch stubs: 0 calls redirected, skipped 0 (target not moved), 0 (target ignored), 0 (entry rule), 0 (no-label-at-target), 1 (not a plain b), 0 (stub not recognised), 0 (not a valid stub start)
# PLAIN: BOLT-INFO: long_branch stubs: 1 calls redirected, skipped 0 (target not moved), 0 (target ignored), 0 (entry rule), 0 (no-label-at-target), 0 (not a plain b), 0 (stub not recognised), 0 (not a valid stub start)

        .include "long-branch-stub.inc"
.ifdef CASE_BLR
## The "stub" is a function that adds 1 to r3 and returns.
        CALLER c_r, x00000001.long_branch.r
        .pushsection .text.x00000001.long_branch.r,"ax",@progbits
x00000001.long_branch.r:
        addi  3, 3, 1
        blr
        .popsection
        START c_r
.endif
.ifdef CASE_BL
## The "stub" is a function that calls k (a `bl`, LK=1) and returns.
        CALLEE k
        CALLER c_l, x00000001.long_branch.l
        .pushsection .text.x00000001.long_branch.l,"ax",@progbits
x00000001.long_branch.l:
        mflr  0
        std   0, 16(1)
        stdu  1, -32(1)
        bl    k
        nop
        addi  1, 1, 32
        ld    0, 16(1)
        mtlr  0
        blr
        .popsection
        START c_l
.endif
.ifdef CASE_B
## The control: a plain `b` to the local entry.
        CALLEE k
        CALLER c_b, x00000001.long_branch.b
        STUB x00000001.long_branch.b, .Lk_lep
        START c_b
.endif
