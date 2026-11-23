## Macros shared by the long-branch-stub-*.s tests (llvm-mc -I %S/Inputs).
## .Lname_gep marks the global entry of a function, .Lname_lep the local entry
## (offset 8). Every callee adds 1 to r3; START removes 1 again, so a program
## exits with 0 only if the call it makes really ran.
        .text
        .abiversion 2

        .macro FUNC name
        .globl \name
        .type  \name, @function
\name:
.L\name\()_gep:
0:      addis 2, 12, 0
        addi  2, 2, 0
        .localentry \name, .-\name
.L\name\()_lep:
        .endm

## A function that adds 1 to r3 and returns, with a local entry.
        .macro CALLEE name
        FUNC \name
        addi  3, 3, 1
        blr
        .size \name, .-\name
        .endm

## A function that calls \target once (a stub or a function) and returns.
        .macro CALLER name, target
        FUNC \name
        mflr  0
        std   0, 16(1)
        stdu  1, -32(1)
        bl    \target
        nop
        addi  1, 1, 32
        ld    0, 16(1)
        mtlr  0
        blr
        .size \name, .-\name
        .endm

## A stub: one `b` to a local label, in a section of its own. The assembler
## cannot resolve the branch, so its relocation is against .text plus the offset
## of the label, the form GNU ld writes (target plus 8 for a local entry).
        .macro STUB name, label
        .pushsection .text.\name,"ax",@progbits
\name:
        b     \label
        .popsection
        .endm

        .macro START caller
        .globl _start
        .type  _start, @function
_start:
0:      addis 2, 12, 0
        addi  2, 2, 0
        .localentry _start, .-_start
        li    3, 0
        bl    \caller
        nop
        addi  3, 3, -1
        li    0, 1
        sc
        .size _start, .-_start
        .endm
