## Test that BOLT correctly handles a minimal PPC64 ELFv2 static binary.
## Verifies BOLT can process the binary in lite mode without crashing
## and the output binary executes correctly.
# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -e _start --emit-relocs
# RUN: llvm-bolt %t.exe -o %t.bolt -lite 2>&1 | FileCheck %s
# RUN: %t.bolt
# CHECK: BOLT-INFO: Target architecture: powerpc64le
# CHECK: BOLT-INFO: enabling lite mode
        .text
        .abiversion 2
        .globl _start
        .type  _start, @function
_start:
        .localentry _start, 1
        li      0, 1            # syscall: exit
        li      3, 0            # exit code 0
        sc
        .size _start, .-_start
