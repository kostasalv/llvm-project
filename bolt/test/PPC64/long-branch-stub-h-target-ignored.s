## SYNTHETIC test (hand-made stubs, linked with ld.lld). The program of
## long-branch-stub-g-two-stubs.s, but f and g are 16 bytes, too small for the
## 36-byte redirect PatchEntries must write at their old addresses (the header
## of test g quotes the lines). PatchEntries ignores f and g, so they are not
## moved. The pass must leave both calls on their stubs and count them as
## "target ignored", not as "target not moved". The program must still exit 0.
##
## The OUT-NOT lines below are NOT a fail-before. Two older builds of this pass
## (lbs3 and lbs4, measured) leave both calls on the stubs here too, so the
## OUT-NOT lines pass with them; what those builds fail is only the pass line
## (CHECK). The OUT-NOT lines were run against a hand-edited t.txt in which the
## calls land on <f+0x8> and <g+0x8>, and they fail on it.

# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu -I %S/Inputs \
# RUN:   %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -e _start --emit-relocs
# RUN: %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt --lite=0 2>&1 | FileCheck %s
# RUN: llvm-nm %t.bolt > %t.txt
# RUN: llvm-objdump -d --no-show-raw-insn %t.bolt --disassemble-symbols=c_fg >> %t.txt
# RUN: FileCheck %s --check-prefix=OUT --input-file=%t.txt
# RUN: %t.bolt

# CHECK: BOLT-INFO: long_branch stubs: 0 calls redirected, skipped 0 (target not moved), 2 (target ignored), 0 (entry rule), 0 (no-label-at-target), 0 (not a plain b), 0 (stub not recognised), 0 (not a valid stub start)

## Neither call goes to the local entry of f or g in the new text.
# OUT-LABEL: <c_fg>:
# OUT-NOT: <f+0x8>
# OUT-NOT: <g+0x8>

        .include "long-branch-stub.inc"
        FUNC f
        addi  3, 3, 1
        blr
        .size f, .-f

        FUNC g
        addi  3, 3, 3
        blr
        .size g, .-g

        FUNC c_fg
        mflr  0
        std   0, 16(1)
        stdu  1, -32(1)
        bl    x00000001.long_branch.f
        nop
        bl    x00000002.long_branch.g
        nop
        addi  3, 3, -3
        addi  1, 1, 32
        ld    0, 16(1)
        mtlr  0
        blr
        .size c_fg, .-c_fg

## Two adjacent stubs in one section, covered by one FDE.
        .pushsection .text.stubs,"ax",@progbits
        .cfi_startproc
x00000001.long_branch.f:
        b     .Lf_lep
x00000002.long_branch.g:
        b     .Lg_lep
        .cfi_endproc
        .popsection

        START c_fg
