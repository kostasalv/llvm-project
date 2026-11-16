## Test that BOLT's branch relaxation (--relax-exp) handles a GCC PPC64 ELFv2
## PIC switch in a function that is too big for relaxation to skip.
##
## LongJmpPass::relaxLocalBranches() returns early for a simple, unsplit
## function whose code is smaller than 1024 bytes, so a small switch never
## reaches the loop that looks at the last instruction of every block. big is
## over 1 KiB of code: its default case is padded with 300 instructions.
##
## big ends its first block with a bctr. The loop that splits unconditional
## branches asked the target for isUnconditionalBranch(), which is true for
## bctr on PowerPC, then looked up a target symbol that a bctr does not have
## and dereferenced the null result: BOLT died with SIGSEGV in
## relaxLocalBranches(). The bctr only reaches this loop since BOLT builds a
## CFG for the PIC switch (see pic-jump-table-switch.s); before that the
## function was not simple and the loop skipped it.
##
## --lite=0 is needed because there is no profile. Each case is called from
## _start and checked, so a wrongly rewritten table also fails.
# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -e _start --emit-relocs
## The input itself must work: this checks the test program, not BOLT.
# RUN: %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt --relax-exp --lite=0 -v=2 -no-threads \
# RUN:   -print-cfg -print-only=big 2>&1 | FileCheck %s
## Run the bolted binary: each case of big is called and checked.
# RUN: %t.bolt

# CHECK-NOT: failed to post-process indirect branches for big
# CHECK: Binary Function "big" after building cfg {
# CHECK: IsSimple    : 1
# CHECK: BOLT-INFO: relaxing branches for compact code model
# CHECK: BOLT-INFO: starting experimental relaxation pass

        .text
        .abiversion 2

        .globl big
        .type  big, @function
big:
        .localentry big, 1
        cmplwi  3, 4
        bgt     0, .Ldefault
        addis   4, 2, .Ltable@toc@ha
        addi    4, 4, .Ltable@toc@l
        rldic   5, 3, 2, 30
        lwax    6, 4, 5
        add     6, 6, 4
        mtctr   6
        bctr
.Ltable:
        .long   .Lcase0-.Ltable
        .long   .Lcase1-.Ltable
        .long   .Lcase2-.Ltable
        .long   .Lcase3-.Ltable
        .long   .Lcase4-.Ltable
.Lcase0:
        li      3, 11
        blr
.Lcase1:
        li      3, 22
        blr
.Lcase2:
        li      3, 33
        blr
.Lcase3:
        li      3, 44
        blr
.Lcase4:
        li      3, 55
        blr
## 300 instructions (1200 bytes) that leave r3 unchanged. addi is used because
## BOLT drops nops.
.Ldefault:
        li      3, 99
        .rept   300
        addi    3, 3, 0
        .endr
        blr
        .size big, .-big

## Entry point. The kernel does not set r2, so build the TOC pointer from the
## absolute address of .TOC. before calling big. Each call checks one result;
## the exit status says which check failed (0 = all passed).
        .globl _start
        .type  _start, @function
_start:
        .localentry _start, 1
        lis     2, .TOC.@highest
        ori     2, 2, .TOC.@higher
        rldicr  2, 2, 32, 31
        oris    2, 2, .TOC.@h
        ori     2, 2, .TOC.@l
        li      3, 0
        bl      big
        nop
        cmpdi   3, 11
        bne     0, .Lfail1
        li      3, 1
        bl      big
        nop
        cmpdi   3, 22
        bne     0, .Lfail2
        li      3, 2
        bl      big
        nop
        cmpdi   3, 33
        bne     0, .Lfail3
        li      3, 3
        bl      big
        nop
        cmpdi   3, 44
        bne     0, .Lfail4
        li      3, 4
        bl      big
        nop
        cmpdi   3, 55
        bne     0, .Lfail5
        li      3, 5
        bl      big
        nop
        cmpdi   3, 99
        bne     0, .Lfail6
        li      3, 0
        b       .Lexit
.Lfail1:
        li      3, 1
        b       .Lexit
.Lfail2:
        li      3, 2
        b       .Lexit
.Lfail3:
        li      3, 3
        b       .Lexit
.Lfail4:
        li      3, 4
        b       .Lexit
.Lfail5:
        li      3, 5
        b       .Lexit
.Lfail6:
        li      3, 6
.Lexit:
        li      0, 1
        sc
        .size _start, .-_start
