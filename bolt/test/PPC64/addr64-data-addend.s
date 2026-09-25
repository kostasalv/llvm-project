## Test that BOLT keeps the addend of an R_PPC64_ADDR64 relocation living in a
## data section.  A pointer to a local function is assembled as "section symbol
## + offset", so dropping the addend while re-emitting the section collapses
## every pointer in it onto the start of .text.  That is what used to corrupt
## .init_array and make a BOLTed clang crash during static initialisation.
# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -e _start --emit-relocs
# RUN: llvm-bolt %t.exe -o %t.bolt 2>&1 | FileCheck %s
# CHECK: BOLT-INFO: Target architecture: powerpc64le
## The two function pointers must still differ and must still reach their own
## function: %t.bolt exits 0 only if both hold.
# RUN: %t.bolt
        .text
        .abiversion 2

## Two distinct local functions.  Because they are local, the .quad directives
## below become R_PPC64_ADDR64 against the .text section symbol plus an offset.
        .type  f1, @function
f1:
        .localentry f1, 1
        li      3, 11
        blr
        .size f1, .-f1

        .type  f2, @function
f2:
        .localentry f2, 1
        li      3, 22
        blr
        .size f2, .-f2

        .globl _start
        .type  _start, @function
_start:
        .localentry _start, 1
        lis     30, ptrs@ha             # R_PPC64_ADDR16_HA
        addi    30, 30, ptrs@l          # R_PPC64_ADDR16_LO
        ld      31, 0(30)
        ld      29, 8(30)
        cmpd    31, 29                  # a dropped addend makes these equal
        beq     .Lfail
        mtctr   31
        bctrl                           # must land in f1
        cmpdi   3, 11
        bne     .Lfail
        mtctr   29
        bctrl                           # must land in f2
        cmpdi   3, 22
        bne     .Lfail
        li      0, 1                    # syscall: exit
        li      3, 0                    # exit code 0
        sc
.Lfail:
        li      0, 1                    # syscall: exit
        li      3, 1                    # exit code 1
        sc
        .size _start, .-_start

        .section .mydata,"aw",@progbits
        .align 3
ptrs:
        .quad   f1                      # R_PPC64_ADDR64 .text + offset(f1)
        .quad   f2                      # R_PPC64_ADDR64 .text + offset(f2)
