## Test that BOLT keeps the ELFv2 global entry TOC setup correct when it moves
## a function. The linker fills the immediates of the prologue
##   addis r2, r12, (.TOC. - f)@ha
##   addi  r2, r2,  (.TOC. - f)@l
## through R_PPC64_REL16_HA/LO, which BOLT does not keep. Without the fix the
## old immediates stay in place, so a moved f gets a TOC pointer that is off by
## the distance f moved, and the TOC load below reads the wrong slot.
## The exit code is the loaded value minus 7, so 0 means r2 was right.
## f has no .localentry, so the linker sends the call to its global entry,
## where the prologue runs, and _start sets r12 the way a caller must.
# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -e _start --emit-relocs
# RUN: llvm-bolt %t.exe -o %t.bolt 2>&1 | FileCheck %s
# RUN: %t.bolt
# CHECK: BOLT-INFO: Target architecture: powerpc64le
# CHECK: BOLT-INFO: enabling relocation mode
        .text
        .abiversion 2
        .globl _start
        .type  _start, @function
_start:
        .localentry _start, 1
        addis   12, 0, f@ha          # r12 = address of f, as the global entry expects
        addi    12, 12, f@l
        bl      f
        subi    3, 3, 7              # r3 is the value f loaded; 0 when it is 7
        li      0, 1                 # syscall: exit
        sc
        .size _start, .-_start

        .globl  f
        .type   f, @function
f:
        addis   2, 12, (.TOC.-f)@ha  # global entry: r2 = .TOC. from r12
        addi    2, 2, (.TOC.-f)@l
        ld      3, .LC0@toc(2)       # load through the TOC pointer
        blr
        .size f, .-f

        .section .toc,"aw"
.LC0:
        .quad   7
