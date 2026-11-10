## Check that -relax-exp gives a tail call (a plain `b`) to a function left at
## its old address a long thunk, as it does for a call (`bl`), and that the
## tail call stays a `b`.
##
## far_call and far_tail are skipped, so they stay in the old text. hot is
## rewritten into the new text, which starts past 48 MiB of .bss, so neither
## callee is within the +-32 MiB reach of `bl`/`b` from hot. The padding
## functions after hot make the new text larger than 32 MiB: without a thunk,
## the far `b` is sent to JITLink's stub table at the end of the new text, and
## the link fails with an out-of-range fixup. The padding uses `addi`, because
## BOLT drops nops. _start sits before the padding so that `bl hot` needs no
## linker thunk.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -e _start --emit-relocs
# RUN: llvm-bolt %t.exe -o %t.bolt --relax-exp --lite=0 \
# RUN:   --max-cluster-size=16777216 --skip-funcs=far_tail,far_call 2>&1 \
# RUN:   | FileCheck %s --implicit-check-not=BOLT-ERROR
# RUN: llvm-nm %t.exe > %t.txt
# RUN: llvm-objdump -d --no-show-raw-insn %t.bolt \
# RUN:   --disassemble-symbols=hot,__AArch64_forward_ADRPThunk_FUNCat0x1001016c_0,__AArch64_forward_ADRPThunk_FUNCat0x10010160_1 \
# RUN:   >> %t.txt
# RUN: FileCheck %s --check-prefix=CHECK-OUTPUT --input-file=%t.txt
# RUN: %t.bolt

# CHECK: BOLT-INFO: starting experimental relaxation pass
# CHECK: BOLT-INFO: relaxed 2 calls with long thunks
# CHECK: BOLT-INFO: 2 long thunks created

## The callees keep their old addresses. The thunks are named after the
## local entry (+8), which is what the calls in hot point at.
# CHECK-OUTPUT: [[#%x,FAR_CALL:]] T far_call
# CHECK-OUTPUT: [[#%x,FAR_TAIL:]] T far_tail

## The call stays a `bl` and the tail call stays a `b` (no link gained); each
## goes to a thunk for its callee's local entry.
# CHECK-OUTPUT-LABEL: <hot>:
# CHECK-OUTPUT:      {{:[[:space:]]+}}bl 0x[[#%x,CALL_THUNK:]] <__AArch64_forward_ADRPThunk_FUNCat0x[[#%x,FAR_CALL+8]]_0>
# CHECK-OUTPUT:      {{:[[:space:]]+}}b 0x[[#%x,TAIL_THUNK:]] <__AArch64_forward_ADRPThunk_FUNCat0x[[#%x,FAR_TAIL+8]]_1>

## Each thunk loads the callee's local entry into r12 and jumps; nothing in it
## touches r2 (every line is checked with -NEXT).
# CHECK-OUTPUT:      [[#%.16x,CALL_THUNK]] <__AArch64_forward_ADRPThunk_FUNCat0x[[#%x,FAR_CALL+8]]_0>:
# CHECK-OUTPUT-NEXT: lis 12, 0
# CHECK-OUTPUT-NEXT: ori 12, 12, 0
# CHECK-OUTPUT-NEXT: sldi 12, 12, 32
# CHECK-OUTPUT-NEXT: oris 12, 12, [[#%u,div(FAR_CALL+8,65536)]]
# CHECK-OUTPUT-NEXT: ori 12, 12, [[#%u,FAR_CALL+8-mul(div(FAR_CALL+8,65536),65536)]]
# CHECK-OUTPUT-NEXT: mtctr 12
# CHECK-OUTPUT-NEXT: bctr
# CHECK-OUTPUT:      [[#%.16x,TAIL_THUNK]] <__AArch64_forward_ADRPThunk_FUNCat0x[[#%x,FAR_TAIL+8]]_1>:
# CHECK-OUTPUT-NEXT: lis 12, 0
# CHECK-OUTPUT-NEXT: ori 12, 12, 0
# CHECK-OUTPUT-NEXT: sldi 12, 12, 32
# CHECK-OUTPUT-NEXT: oris 12, 12, [[#%u,div(FAR_TAIL+8,65536)]]
# CHECK-OUTPUT-NEXT: ori 12, 12, [[#%u,FAR_TAIL+8-mul(div(FAR_TAIL+8,65536),65536)]]
# CHECK-OUTPUT-NEXT: mtctr 12
# CHECK-OUTPUT-NEXT: bctr

        .text
        .abiversion 2

        .globl far_tail
        .type  far_tail, @function
far_tail:
0:      addis 2, 12, 0
        addi  2, 2, 0
        .localentry far_tail, .-far_tail
        blr
        .size far_tail, .-far_tail

        .globl far_call
        .type  far_call, @function
far_call:
0:      addis 2, 12, 0
        addi  2, 2, 0
        .localentry far_call, .-far_call
        blr
        .size far_call, .-far_call

        .globl hot
        .type  hot, @function
hot:
0:      addis 2, 12, 0
        addi  2, 2, 0
        .localentry hot, .-hot
        mflr  0
        std   0, 16(1)
        stdu  1, -32(1)
        bl    far_call
        nop
        addi  1, 1, 32
        ld    0, 16(1)
        mtlr  0
        b     far_tail
        .size hot, .-hot

        .globl _start
        .type  _start, @function
_start:
0:      addis 2, 12, 0
        addi  2, 2, 0
        .localentry _start, .-_start
        bl    hot
        nop
        li    0, 1
        li    3, 0
        sc
        .size _start, .-_start

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
