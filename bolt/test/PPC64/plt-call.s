## Test that BOLT correctly handles PLT call relocations on PPC64 ELFv2.
## When calling an external function (puts), the linker generates a PLT
## stub and bl targets that stub via R_PPC64_REL24. BOLT must correctly
## patch the branch to the updated PLT stub address after rewriting.
# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -e _start --emit-relocs \
# RUN:         -L/usr/lib64 -lc \
# RUN:         --dynamic-linker /lib64/ld64.so.2
# RUN: llvm-bolt %t.exe -o %t.bolt -lite 2>&1 | FileCheck %s
# RUN: %t.bolt
# CHECK: BOLT-INFO: Target architecture: powerpc64le
# CHECK: BOLT-INFO: enabling relocation mode
        .text
        .abiversion 2
        .globl _start
        .type  _start, @function
_start:
        addis   2, 12, .TOC.-_start@ha
        addi    2, 2,  .TOC.-_start@l
        .localentry _start, .-_start
        ## allocate stack frame: puts needs valid 24(r1) for TOC save/restore
        stdu    1, -64(1)           # create 64-byte stack frame
        std     2, 24(1)            # save TOC at ABI-mandated slot
        addis   3, 2, .LC0@toc@ha   # high-adjusted part of TOC entry offset
        addi    3, 3, .LC0@toc@l    # low part of TOC entry offset
        ld      3, 0(3)             # load msg address from the TOC entry
        bl      puts                # R_PPC64_REL24 to PLT stub
        nop                         # TOC restore slot (ELFv2 ABI)
        addi    1, 1, 64            # restore stack pointer
        li      0, 1                # syscall: exit
        li      3, 0                # exit code 0
        sc
        .size _start, .-_start
        .section .toc,"aw"
.LC0:
        .quad   msg                 ## TOC entry containing address of msg
        .section .rodata
        .type   msg, @object
msg:
        .asciz  "hello from BOLT"
        .size   msg, .-msg
