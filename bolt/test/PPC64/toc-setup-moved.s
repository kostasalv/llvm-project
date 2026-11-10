## Test that BOLT keeps the ELFv2 global entry TOC setup correct when it moves
## a function. The linker fills the immediates of the prologue
##   addis r2, r12, (.TOC. - f)@ha
##   addi  r2, r2,  (.TOC. - f)@l
## through R_PPC64_REL16_HA/LO, which BOLT does not keep. Without the fix the
## old immediates stay in place, so a moved function gets a TOC pointer that is
## off by the distance it moved, and the TOC load reads the wrong slot.
## Both _start and f have a global entry. The kernel passes the entry address in
## r12, and _start passes the address of f in r12 after loading it from the TOC,
## where BOLT updates it. The exit code is the loaded value minus 7, so 0 means
## r2 was right in both functions.
# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -e _start --emit-relocs
# RUN: %t.exe
## Guard: the input must really carry the relocations under test, otherwise the
## warning check below could pass for the wrong reason.
# RUN: llvm-readelf -rW %t.exe | FileCheck %s --check-prefix=INPUT
# INPUT-DAG: R_PPC64_REL16_HA {{.*}} .TOC. + 0
# INPUT-DAG: R_PPC64_REL16_LO {{.*}} .TOC. + 4
## The TOC setup relocations are handled by symbolizeTOCSetup, so they must not
## be reported as relocations BOLT failed to analyze.
# RUN: llvm-bolt %t.exe -o %t.bolt 2>&1 | FileCheck %s --implicit-check-not="Failed to analyze"
# RUN: %t.bolt
# CHECK: BOLT-INFO: Target architecture: powerpc64le
# CHECK: BOLT-INFO: enabling relocation mode
        .text
        .abiversion 2
        .globl _start
        .type  _start, @function
_start:
        addis   2, 12, (.TOC.-_start)@ha
        addi    2, 2, (.TOC.-_start)@l
        .localentry _start, .-_start
        ld      12, .LCf@toc(2)      # r12 = address of f
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
.LCf:
        .quad   f
