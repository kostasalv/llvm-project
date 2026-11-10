## Test that identical code folding can compare the instructions BOLT builds for
## TOC-relative references. R_PPC64_TOC16_HA and R_PPC64_TOC16_LO_DS are kept as
## specifier expressions (@toc@ha, @toc@l), and the folding pass compares them
## operand by operand. Without PPCMCPlusBuilder::equals for those expressions
## the comparison reached llvm_unreachable and llvm-bolt aborted.
## f and g are byte for byte the same and load the same TOC entry, so they must
## fold. h loads another TOC entry, so it must not fold with them.
# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -e _start --emit-relocs
## Guard: the input must really carry the relocations under test.
# RUN: llvm-readelf -rW %t.exe | FileCheck %s --check-prefix=INPUT
# INPUT-DAG: R_PPC64_TOC16_HA
# INPUT-DAG: R_PPC64_TOC16_LO_DS
# RUN: %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt --icf 2>&1 | FileCheck %s
# CHECK: BOLT-INFO: Target architecture: powerpc64le
# CHECK: BOLT-INFO: ICF folded {{[0-9]+}} out of {{[0-9]+}} functions
## Folded functions must keep working: the exit code is 0 when f, g and h return
## 5, 5 and 6, so folding g into f (or f into g) must not change any result.
# RUN: %t.bolt
## The linker's own stubs (__toc_save_f/g) may fold too, so the number of folds
## is not checked. f and g must end up at one address, and h must not.
# RUN: llvm-nm %t.bolt | FileCheck %s --check-prefix=SYM
# SYM-DAG: [[#%x,F:]] T f
# SYM-DAG: [[#F]] T g
# SYM-DAG: {{[0-9a-f]+}} T h
## h loads another TOC entry, so it must not be folded into f. The pattern below
## only matches when h has the address of f, so FileCheck has to fail on it.
# RUN: llvm-nm %t.bolt | not FileCheck %s --check-prefix=HFOLDED
# HFOLDED-DAG: [[#%x,HF:]] T f
# HFOLDED-DAG: [[#HF]] T h
        .text
        .abiversion 2
        .globl _start
        .type  _start, @function
_start:
        addis   2, 12, (.TOC.-_start)@ha   # the kernel passes the entry in r12,
        addi    2, 2, (.TOC.-_start)@l     # so r2 is set up from r12 (ELFv2)
        .localentry _start, .-_start
        bl      f
        nop                  # call slot (ELFv2): the linker may put ld r2,24(r1) here
        mr      14, 3                # r14 = f(), nonvolatile across calls
        bl      g
        nop
        subf    14, 3, 14            # r14 = f() - g(), 0 when equal
        bl      h
        nop
        subi    3, 3, 6              # r3 = h() - 6, 0 when h returns 6
        add     3, 3, 14             # both parts must be 0
        li      0, 1                 # syscall: exit
        sc
        .size _start, .-_start

        .globl  f
        .type   f, @function
f:
        .localentry f, 1
        addis   3, 2, .LC5@toc@ha
        ld      3, .LC5@toc@l(3)
        blr
        .size f, .-f

        .globl  g
        .type   g, @function
g:
        .localentry g, 1
        addis   3, 2, .LC5@toc@ha
        ld      3, .LC5@toc@l(3)
        blr
        .size g, .-g

        .globl  h
        .type   h, @function
h:
        .localentry h, 1
        addis   3, 2, .LC6@toc@ha
        ld      3, .LC6@toc@l(3)
        blr
        .size h, .-h

        .section .toc,"aw"
.LC5:
        .quad   5
.LC6:
        .quad   6
