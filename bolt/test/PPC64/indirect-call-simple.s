## An indirect call (mtctr; bctrl, ELFv2 section 2.3.6, Table 2.30) is a call,
## not an indirect jump. A function that contains one must stay simple, so BOLT
## can move it.
## Before the fix this test FAILS: with_bctrl and single print IsSimple 0 and
## the "failed to post-process indirect branches" warning.
## with_bl has the same shape with a direct call and is the positive control:
## it proves this test can see IsSimple 1.
## A real indirect jump (bctr) with no known target must still make the
## function non-simple; with_bctr is the negative control for that.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -e _start --emit-relocs
# RUN: llvm-bolt %t.exe -o %t.bolt --relocs -v=1 --print-cfg \
# RUN:   --print-only='with_bctrl|with_bl|with_bctr|single|with_bclrl|with_blrl' 2>&1 | FileCheck %s
# RUN: %t.bolt

# CHECK-NOT: failed to post-process indirect branches for with_bctrl
# CHECK-NOT: failed to post-process indirect branches for single
# CHECK-LABEL: Binary Function "with_bctrl" after building cfg
# CHECK:       IsSimple    : 1
# CHECK-LABEL: Binary Function "with_bl" after building cfg
# CHECK:       IsSimple    : 1
# CHECK-LABEL: Binary Function "single" after building cfg
# CHECK:       IsSimple    : 1
# CHECK-LABEL: Binary Function "with_bclrl" after building cfg
# CHECK:       IsSimple    : 1
# CHECK-LABEL: Binary Function "with_blrl" after building cfg
# CHECK:       IsSimple    : 1
# CHECK-LABEL: Binary Function "with_bctr" after building cfg
# CHECK:       IsSimple    : 0

        .text
        .abiversion 2

        .globl helper
        .type  helper, @function
helper:
        blr
        .size helper, .-helper

## Two blocks, an indirect call in the first.
        .globl with_bctrl
        .type  with_bctrl, @function
with_bctrl:
0:      addis 2, 12, .TOC.-0b@ha
        addi  2, 2, .TOC.-0b@l
        .localentry with_bctrl, .-with_bctrl
        mflr  0
        std   0, 16(1)
        stdu  1, -32(1)
        cmpdi 3, 0
        beq   1f
        mr    12, 4
        mtctr 12
        std   2, 24(1)
        bctrl
        ld    2, 24(1)
1:
        addi  1, 1, 32
        ld    0, 16(1)
        mtlr  0
        blr
        .size with_bctrl, .-with_bctrl

## Same shape with a direct call: positive control.
        .globl with_bl
        .type  with_bl, @function
with_bl:
0:      addis 2, 12, .TOC.-0b@ha
        addi  2, 2, .TOC.-0b@l
        .localentry with_bl, .-with_bl
        mflr  0
        std   0, 16(1)
        stdu  1, -32(1)
        cmpdi 3, 0
        beq   1f
        bl    helper
        nop
1:
        addi  1, 1, 32
        ld    0, 16(1)
        mtlr  0
        blr
        .size with_bl, .-with_bl

## No branch, one indirect call.
        .globl single
        .type  single, @function
single:
0:      addis 2, 12, .TOC.-0b@ha
        addi  2, 2, .TOC.-0b@l
        .localentry single, .-single
        mflr  0
        std   0, 16(1)
        stdu  1, -32(1)
        mr    12, 4
        mtctr 12
        std   2, 24(1)
        bctrl
        ld    2, 24(1)
        addi  1, 1, 32
        ld    0, 16(1)
        mtlr  0
        blr
        .size single, .-single

## Conditional indirect call through LR (beqlrl, encoding of bclrl 12,2).
## The bytes decode to gBCLRL, not BCLRL (llvm-mc --disassemble --show-inst; the
## assembler path gives BCCLRL). No PPC predicate lists gBCLRL or BCCLRL, so this
## function is NOT expected to fail before the fix; it is a regression guard that
## isIndirectBranch does not touch it. It cannot exercise the BCLRL case label
## itself.
        .globl with_bclrl
        .type  with_bclrl, @function
with_bclrl:
0:      addis 2, 12, .TOC.-0b@ha
        addi  2, 2, .TOC.-0b@l
        .localentry with_bclrl, .-with_bclrl
        mflr  0
        std   0, 16(1)
        stdu  1, -32(1)
        cmpdi 3, 0
        mr    12, 4
        mtlr  12
        std   2, 24(1)
        beqlrl
        ld    2, 24(1)
        addi  1, 1, 32
        ld    0, 16(1)
        mtlr  0
        blr
        .size with_bclrl, .-with_bclrl

## Unconditional indirect call through LR (blrl). Decodes to BLRL, which only
## isBranch() lists. It must stay simple.
        .globl with_blrl
        .type  with_blrl, @function
with_blrl:
0:      addis 2, 12, .TOC.-0b@ha
        addi  2, 2, .TOC.-0b@l
        .localentry with_blrl, .-with_blrl
        mflr  0
        std   0, 16(1)
        stdu  1, -32(1)
        mr    12, 4
        mtlr  12
        std   2, 24(1)
        blrl
        ld    2, 24(1)
        addi  1, 1, 32
        ld    0, 16(1)
        mtlr  0
        blr
        .size with_blrl, .-with_blrl

## Two blocks, an indirect JUMP with no known target: negative control.
## Never called at run time.
        .globl with_bctr
        .type  with_bctr, @function
with_bctr:
        cmpdi 3, 0
        beq   1f
        mtctr 4
        bctr
1:
        blr
        .size with_bctr, .-with_bctr

        .globl _start
        .type  _start, @function
_start:
0:      addis 2, 12, .TOC.-0b@ha
        addi  2, 2, .TOC.-0b@l
        .localentry _start, .-_start
        mflr  0
        std   0, 16(1)
        stdu  1, -48(1)
        std   2, 24(1)
        li    3, 1
        addis 4, 2, helper@toc@ha
        addi  4, 4, helper@toc@l
        bl    with_bctrl
        nop
        li    3, 1
        bl    with_bl
        nop
        addis 4, 2, helper@toc@ha
        addi  4, 4, helper@toc@l
        bl    single
        nop
        li    0, 1
        li    3, 0
        sc
        .size _start, .-_start
