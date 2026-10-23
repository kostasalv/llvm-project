## p1-repro-localentry.s: focused reproduction for the 36 unmarked `b FUNCat0x...`
## (rollout P1, after fix C).
##
## Two claims are under test. Both are INFERRED from source until this runs.
##   H-user : a `b` to a LOCAL entry (func+8) is not marked as a tail call.
##   H-recur: a function with a self-recursive `bl` to its own local entry gets an
##            "internal call", becomes non-simple after the CFG stage, and the
##            non-simple arm of LongJmp relaxLocalBranches then writes an unmarked
##            `b FUNCat...` trampoline.
##
## One caller function per case. Targets live in the old text (ignored because
## they contain `mtctr; bctrl`, as llc functions do), the callers are rewritten
## into the new text more than 32 MiB away (see relax-exp-tail-call.s).
##
##   c_r1  b  simp_g        simple target.  POSITIVE CONTROL: mark kept
##   c_r2  b  ign_g         ignored target.
##         NOTE (MEASURED 2026-10-23): lld rewrites `b simp_g` and `b ign_g` to the
##         LOCAL entry (+8), because the caller has .localentry and shares the TOC.
##         So c_r1 and c_r2 branch to +8 too. Only c_r7 and c_r8 reach offset 0.
##   c_r3  b  simp_l        simple target,  local entry (+8)
##   c_r4  b  ign_l         ignored target, local entry (+8)
##   c_r5  b  inside itself NEGATIVE CONTROL: no mark, no warning
##   c_r6a bl own +8, then b ign_g   recursive caller, global-entry tail call
##   c_r6b bl own +8, then b ign_l   recursive caller, local-entry tail call
##   c_r9  bl own +8, no tail call  internal call only: does the JITLink error need the unmarked b?
##   c_r7  b  offset 0 of simp_g    simple target,  TRUE global entry (via local label)
##   c_r8  b  offset 0 of ign_g     ignored target, TRUE global entry (via local label)
##
## ign_g / ign_l are the same shape as llc's non-simple functions.
## simp_g / simp_l are plain simple functions.
        .text
        .abiversion 2

## ---- targets -----------------------------------------------------------
        .globl simp_g
        .type  simp_g, @function
simp_g:
.Lsimp_g_start:
0:      addis 2, 12, 0
        addi  2, 2, 0
        .localentry simp_g, .-simp_g
        blr
        .size simp_g, .-simp_g

        .globl simp_l
        .type  simp_l, @function
simp_l:
0:      addis 2, 12, 0
        addi  2, 2, 0
        .localentry simp_l, .-simp_l
.Lsimp_l_le:
        blr
        .size simp_l, .-simp_l

        .globl ign_g
        .type  ign_g, @function
ign_g:
.Lign_g_start:
0:      addis 2, 12, 0
        addi  2, 2, 0
        .localentry ign_g, .-ign_g
        mflr  0
        std   0, 16(1)
        stdu  1, -32(1)
        cmpdi 3, 0
        beq   1f
        mtctr 4
        bctrl
1:      addi  1, 1, 32
        ld    0, 16(1)
        mtlr  0
        blr
        .size ign_g, .-ign_g

        .globl ign_l
        .type  ign_l, @function
ign_l:
0:      addis 2, 12, 0
        addi  2, 2, 0
        .localentry ign_l, .-ign_l
.Lign_l_le:
        mflr  0
        std   0, 16(1)
        stdu  1, -32(1)
        cmpdi 3, 0
        beq   1f
        mtctr 4
        bctrl
1:      addi  1, 1, 32
        ld    0, 16(1)
        mtlr  0
        blr
        .size ign_l, .-ign_l

## ---- callers -----------------------------------------------------------
        .globl c_r1
        .type  c_r1, @function
c_r1:
0:      addis 2, 12, 0
        addi  2, 2, 0
        .localentry c_r1, .-c_r1
        b     simp_g
        .size c_r1, .-c_r1

        .globl c_r2
        .type  c_r2, @function
c_r2:
0:      addis 2, 12, 0
        addi  2, 2, 0
        .localentry c_r2, .-c_r2
        b     ign_g
        .size c_r2, .-c_r2

        .globl c_r3
        .type  c_r3, @function
c_r3:
0:      addis 2, 12, 0
        addi  2, 2, 0
        .localentry c_r3, .-c_r3
        b     .Lsimp_l_le
        .size c_r3, .-c_r3

        .globl c_r4
        .type  c_r4, @function
c_r4:
0:      addis 2, 12, 0
        addi  2, 2, 0
        .localentry c_r4, .-c_r4
        b     .Lign_l_le
        .size c_r4, .-c_r4

        .globl c_r5
        .type  c_r5, @function
c_r5:
0:      addis 2, 12, 0
        addi  2, 2, 0
        .localentry c_r5, .-c_r5
        cmpdi 3, 0
        beq   1f
        addi  3, 3, 1
        b     2f
1:      addi  3, 3, 2
2:      blr
        .size c_r5, .-c_r5

        .globl c_r6a
        .type  c_r6a, @function
c_r6a:
0:      addis 2, 12, 0
        addi  2, 2, 0
        .localentry c_r6a, .-c_r6a
.Lc_r6a_le:
        mflr  0
        std   0, 16(1)
        stdu  1, -32(1)
        cmpdi 3, 0
        beq   1f
        addi  3, 3, -1
        bl    .Lc_r6a_le
        nop
1:      addi  1, 1, 32
        ld    0, 16(1)
        mtlr  0
        b     ign_g
        .size c_r6a, .-c_r6a

        .globl c_r6b
        .type  c_r6b, @function
c_r6b:
0:      addis 2, 12, 0
        addi  2, 2, 0
        .localentry c_r6b, .-c_r6b
.Lc_r6b_le:
        mflr  0
        std   0, 16(1)
        stdu  1, -32(1)
        cmpdi 3, 0
        beq   1f
        addi  3, 3, -1
        bl    .Lc_r6b_le
        nop
1:      addi  1, 1, 32
        ld    0, 16(1)
        mtlr  0
        b     .Lign_l_le
        .size c_r6b, .-c_r6b

        .globl c_r9
        .type  c_r9, @function
c_r9:
0:      addis 2, 12, 0
        addi  2, 2, 0
        .localentry c_r9, .-c_r9
.Lc_r9_le:
        mflr  0
        std   0, 16(1)
        stdu  1, -32(1)
        cmpdi 3, 0
        beq   1f
        addi  3, 3, -1
        bl    .Lc_r9_le
        nop
1:      addi  1, 1, 32
        ld    0, 16(1)
        mtlr  0
        blr
        .size c_r9, .-c_r9

        .globl c_r7
        .type  c_r7, @function
c_r7:
0:      addis 2, 12, 0
        addi  2, 2, 0
        .localentry c_r7, .-c_r7
        b     .Lsimp_g_start
        .size c_r7, .-c_r7

        .globl c_r8
        .type  c_r8, @function
c_r8:
0:      addis 2, 12, 0
        addi  2, 2, 0
        .localentry c_r8, .-c_r8
        b     .Lign_g_start
        .size c_r8, .-c_r8

        .globl _start
        .type  _start, @function
_start:
0:      addis 2, 12, 0
        addi  2, 2, 0
        .localentry _start, .-_start
        bl    c_r1
        nop
        bl    c_r2
        nop
        bl    c_r3
        nop
        bl    c_r4
        nop
        bl    c_r5
        nop
        bl    c_r6a
        nop
        bl    c_r6b
        nop
        bl    c_r7
        nop
        bl    c_r8
        nop
        bl    c_r9
        nop
        li    0, 1
        li    3, 0
        sc
        .size _start, .-_start

## ---- padding: makes the new text bigger than 32 MiB -----------------------
        .globl pad0
        .type  pad0, @function
pad0:
        .rept 0x200000
        addi  3, 3, 1
        .endr
        blr
        .size pad0, .-pad0

        .globl pad1
        .type  pad1, @function
pad1:
        .rept 0x200000
        addi  3, 3, 1
        .endr
        blr
        .size pad1, .-pad1

        .globl pad2
        .type  pad2, @function
pad2:
        .rept 0x200000
        addi  3, 3, 1
        .endr
        blr
        .size pad2, .-pad2

        .globl pad3
        .type  pad3, @function
pad3:
        .rept 0x200000
        addi  3, 3, 1
        .endr
        blr
        .size pad3, .-pad3

        .globl pad4
        .type  pad4, @function
pad4:
        .rept 0x200000
        addi  3, 3, 1
        .endr
        blr
        .size pad4, .-pad4

        .section .bss,"aw",@nobits
        .p2align 4
pad:
        .zero 0x3000000
