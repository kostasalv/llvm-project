## SYNTHETIC test (hand-made stubs, linked with ld.lld). GNU ld puts the stubs of
## a group next to each other, and one FDE covers the whole group. BOLT makes
## one function of the group, and only the first stub symbol is the start of
## that function; every later stub symbol is an extra name inside it.
##
## Here two `.long_branch.` stubs with different targets share one FDE
## (.cfi_startproc), and c_fg calls both: the first stub reaches f, the second
## reaches g. Each call must go to the local entry of its own stub's target.
## A pass that reads the branch of the function's first stub for every call
## sends both calls to f: the pass line then says 2 calls redirected as well,
## so the check is the destination of each call and the exit value (f adds 1,
## g adds 3, c_fg takes 3 off again, _start takes 1 off).

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

# CHECK: BOLT-INFO: long_branch stubs: 2 calls redirected, skipped 0 (target not moved), 0 (target ignored), 0 (entry rule), 0 (no-label-at-target), 0 (not a plain b), 0 (stub not recognised), 0 (not a valid stub start)

## Each call goes to the local entry of its own stub's target, in order.
# OUT: [[#%x,F:]] T f
# OUT: [[#%x,G:]] T g
# OUT-LABEL: <c_fg>:
# OUT: bl 0x[[#%x,F+8]] <f+0x8>
# OUT: bl 0x[[#%x,G+8]] <g+0x8>
# OUT-NOT: <c_fg+

## Why f and g are padded. The stubs' relocations (R_PPC64_REL24 against f+8 and
## g+8) make BOLT ask PatchEntries to leave a redirect at the old f and g
## (BinaryFunction.cpp:2052-2056, setNeedsPatch(true) for PPC64). The redirect
## needs room (bolt/lib/Passes/PatchEntries.cpp, READ):
##   179-184  EntryPatchSize = LongPatchSize, and for a function with a local
##            entry patched at offset 0 (SplitEntry = LEPOffset && Offset == 0)
##            EntryPatchSize = LEPOffset + LongPatchSize
##   202-208  if (NextValidByte > Function.getMaxSize()) the patch fails;
##            "too small to patch its entry point" is printed with -v=1
##   244-250  "BOLT-WARNING: failed to patch entries in <f>. The function will
##            not be optimized", then Function.setIgnored()
## An ignored function is not moved, so the pass cannot redirect a call to it
## (long-branch-stub-h-target-ignored.s has f and g unpadded and checks that).
## LEPOffset is 8 and the long branch is 7 instructions, 28 bytes
## (PPCMCPlusBuilder::createLongTailCall, 28 bytes INFERRED from its 7
## instructions), so 36 bytes are needed. An unpadded f is 16 bytes (READ).
## Eight nops make f and g 48 bytes.
        .include "long-branch-stub.inc"
        FUNC f
        addi  3, 3, 1
        blr
        .rept 8
        nop
        .endr
        .size f, .-f

        FUNC g
        addi  3, 3, 3
        blr
        .rept 8
        nop
        .endr
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
