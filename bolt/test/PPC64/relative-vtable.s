## Test that a relative vtable is handled on PPC64 as it is on x86 and AArch64.
## A relative vtable slot is a 32-bit PC-relative word (R_PPC64_REL32) whose
## addend is the slot's distance from the first virtual function slot, so the
## second slot below is "f + 4". That addend is not an offset into f. If BOLT
## treats it as one, it registers an internal reference into f that no jump
## table claims, warns about an unclaimed data relocation and marks f
## non-simple, which blocks every optimisation of f (and identical code folding).
# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -e _start --emit-relocs
## Guard: the input must really carry the relocation under test, with the addend.
# RUN: llvm-readelf -rW %t.exe | FileCheck %s --check-prefix=INPUT
# INPUT: R_PPC64_REL32 {{.*}} f + 4
# RUN: %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt --lite=0 -print-cfg -print-only=f 2>&1 \
# RUN:   | FileCheck %s --implicit-check-not="unclaimed data relocation"
# CHECK: Binary Function "f"
# CHECK: IsSimple{{ *}}: 1
# RUN: %t.bolt
        .text
        .abiversion 2
        .globl _start
        .type  _start, @function
_start:
        addis   2, 12, (.TOC.-_start)@ha
        addi    2, 2, (.TOC.-_start)@l
        .localentry _start, .-_start
        bl      f
        nop                  # call slot (ELFv2)
        subi    3, 3, 6      # f returns 6, so the exit code is 0
        li      0, 1         # syscall: exit
        sc
        .size _start, .-_start

        .globl  f
        .type   f, @function
f:
        li      3, 1
        addi    3, 3, 2
        addi    3, 3, 3
        blr
        .size f, .-f

## Offset-to-top and RTTI, then two slots. A slot's addend is its offset from
## the address point, so the second slot's "f + 4" lands inside f.
        .section .rodata,"a",@progbits
        .globl  _ZTV1D
        .type   _ZTV1D, @object
        .p2align 2
_ZTV1D:
        .long   0
        .long   0
        .long   f - .
        .long   f + 4 - .
        .size _ZTV1D, .-_ZTV1D
