## Test that BOLT builds a CFG for a GCC PPC64 ELFv2 PIC switch and rewrites its
## jump table.
##
## GCC compiles a dense switch into a table of signed 32-bit offsets placed in
## the function body, right after the indirect branch that reads it:
##
##   addis rB, 2, table@toc@ha   # table base, from the TOC pointer
##   addi  rB, rB, table@toc@l
##   rldic rI, rX, 2, 30         # index * 4
##   lwax  rD, rB, rI            # signed 32-bit entry
##   add   rD, rD, rB            # entry + table base = case address
##   mtctr rD
##   bctr
##   .long case0-table, ...      # the table, no relocations
##
## The table words carry no relocations, so BOLT used to give up on the
## function: "failed to post-process indirect branches". Without a CFG the
## function is not optimized and is copied unchanged.
##
## sw is the switch: it has five cases that each return a different value, and
## a default. neg is a negative control: its bctr jumps through a register that
## is not a table entry, so BOLT must keep leaving it alone.
##
## --reorder-blocks=reverse changes the block order without a profile. If the
## rewritten table still held the old offsets, every case would land in the
## wrong block, so _start checks the value of each case.
# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -e _start --emit-relocs
## The input itself must work: this checks the test program, not BOLT.
# RUN: %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt -v=2 -no-threads -reorder-blocks=reverse -print-cfg \
# RUN:   -print-only=sw,neg 2>&1 | FileCheck %s --check-prefix=BOLT
# RUN: llvm-objdump -d --no-show-raw-insn %t.bolt | FileCheck %s --check-prefix=DUMP
# RUN: llvm-objdump -s -j .rodata.cold %t.bolt | FileCheck %s --check-prefix=TABLE
## C4: run the bolted binary. Each case of sw is called and checked.
# RUN: %t.bolt

## C1: sw gets a CFG and is simple. Before the fix BOLT printed
## "failed to post-process indirect branches for sw" here.
# BOLT-NOT: failed to post-process indirect branches for sw
# BOLT: Binary Function "sw" after building cfg {
# BOLT: IsSimple    : 1
## C5: neg is not a jump table and stays non-simple.
# BOLT: Binary Function "neg" after building cfg {
# BOLT: IsSimple    : 0

## C3 and C2 are checked on the bolted binary (see the DUMP lines below).
## C3: the pair builds the address of the new table, not of the old island.
## .TOC. is 0x10028318, so (61 << 16) + 32344 gives 0x10400170, the table
## in .rodata.cold (the old island was at 0x100101ec).
# DUMP: <sw>:
# DUMP: addis 4, 2, 61
# DUMP: addi 4, 4, 32344
# DUMP: lwax 6, 4, 5
# DUMP: bctr

## C2: the new table, at 0x10400170. With the blocks reversed, entry i plus the
## table address is the block that returns 11, 22, 33, 44, 55 for i = 0..4.
# TABLE: 10400170 c4feffff bcfeffff b4feffff acfeffff
# TABLE-NEXT: 10400180 a4feffff

        .text
        .abiversion 2

        .globl sw
        .type  sw, @function
sw:
        .localentry sw, 1
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
.Ldefault:
        li      3, 99
        blr
        .size sw, .-sw

## Negative control: an indirect jump through a register that no table load
## produced. It has two blocks, because BOLT treats the only indirect branch of
## a one-block function as a tail call. It is never called.
        .globl neg
        .type  neg, @function
neg:
        .localentry neg, 1
        cmpdi   3, 0
        beq     0, .Lneg_ret
        mtctr   4
        bctr
.Lneg_ret:
        li      3, 1
        blr
        .size neg, .-neg

## Entry point. The kernel does not set r2, so build the TOC pointer from the
## absolute address of .TOC. before calling sw. Each call checks one result;
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
        bl      sw
        nop
        cmpdi   3, 11
        bne     0, .Lfail1
        li      3, 1
        bl      sw
        nop
        cmpdi   3, 22
        bne     0, .Lfail2
        li      3, 2
        bl      sw
        nop
        cmpdi   3, 33
        bne     0, .Lfail3
        li      3, 3
        bl      sw
        nop
        cmpdi   3, 44
        bne     0, .Lfail4
        li      3, 4
        bl      sw
        nop
        cmpdi   3, 55
        bne     0, .Lfail5
        li      3, 5
        bl      sw
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
