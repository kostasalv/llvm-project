## Test that -trap-old-code fills the old body of a moved function with the
## PowerPC unconditional trap, tw 31,0,0 (Power ISA v3.1C, Appendix C.6).
## Without a PowerPC getTrapFillValue, BOLT hits llvm_unreachable in the base
## class. The old copies are in .bolt.org.text; the new copies must still run.
# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -e _start --emit-relocs
# RUN: %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt -trap-old-code 2>&1 | FileCheck %s
# RUN: llvm-objdump -d --section=.bolt.org.text %t.bolt | FileCheck %s --check-prefix=OLD
# RUN: %t.bolt
# CHECK: BOLT-INFO: Target architecture: powerpc64le
# CHECK: BOLT-INFO: enabling relocation mode
## Both functions moved, so all six old instruction words are now the trap,
## little-endian 08 00 e0 7f, and none of the original code is left.
# OLD:          Disassembly of section .bolt.org.text:
# OLD-COUNT-6:  08 00 e0 7f trap
# OLD-NOT:      {{li|blr|sc|bl }}
        .text
        .abiversion 2
        .globl _start
        .type  _start, @function
_start:
        bl      f
        li      0, 1                 # exit(r3), r3 = 0 from f
        sc
        .size  _start, .-_start

        .globl f
        .type  f, @function
f:
        li      3, 0
        nop
        blr
        .size  f, .-f
