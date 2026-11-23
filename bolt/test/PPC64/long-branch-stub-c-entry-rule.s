## SYNTHETIC test (hand-made stubs, linked with ld.lld): the call is redirected
## only to the entry the stub already reaches, and only if that is the entry the
## ABI gives a caller that shares the TOC: the local entry, or offset 0 when the
## function has none (ELFv2 section 3.4.1, st_other). A stub that reaches the
## global entry of g, which has a local entry, is left alone. A stub that reaches
## z+0, where z has no local entry, is redirected to z.

# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu -I %S/Inputs \
# RUN:   --defsym CASE_G=1 %s -o %t.g.o
# RUN: ld.lld %t.g.o -o %t.g.exe -e _start --emit-relocs
# RUN: %t.g.exe
# RUN: llvm-bolt %t.g.exe -o %t.g.bolt --lite=0 2>&1 | FileCheck %s --check-prefix=G
# RUN: llvm-objdump -d --no-show-raw-insn %t.g.bolt --disassemble-symbols=c_g \
# RUN:   | FileCheck %s --check-prefix=G-OUT
# RUN: %t.g.bolt
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu -I %S/Inputs \
# RUN:   --defsym CASE_Z=1 %s -o %t.z.o
# RUN: ld.lld %t.z.o -o %t.z.exe -e _start --emit-relocs
# RUN: %t.z.exe
# RUN: llvm-bolt %t.z.exe -o %t.z.bolt --lite=0 2>&1 | FileCheck %s --check-prefix=ZERO
# RUN: llvm-nm %t.z.bolt > %t.z.txt
# RUN: llvm-objdump -d --no-show-raw-insn %t.z.bolt --disassemble-symbols=c_z >> %t.z.txt
# RUN: FileCheck %s --check-prefix=ZERO-OUT --input-file=%t.z.txt
# RUN: %t.z.bolt

# G: BOLT-INFO: long_branch stubs: 0 calls redirected, skipped 0 (target not moved), 0 (target ignored), 1 (entry rule), 0 (no-label-at-target), 0 (not a plain b), 0 (stub not recognised), 0 (not a valid stub start)
# G-OUT-LABEL: <c_g>:
# G-OUT: bl 0x{{[0-9a-f]+}} <c_g+0x{{[0-9a-f]+}}>

# ZERO: BOLT-INFO: long_branch stubs: 1 calls redirected, skipped 0 (target not moved), 0 (target ignored), 0 (entry rule), 0 (no-label-at-target), 0 (not a plain b), 0 (stub not recognised), 0 (not a valid stub start)
# ZERO-OUT: [[#%x,ZADDR:]] T z
# ZERO-OUT-LABEL: <c_z>:
# ZERO-OUT: bl 0x[[#%x,ZADDR]] <z>

        .include "long-branch-stub.inc"
.ifdef CASE_G
        CALLEE g
        CALLER c_g, x00000001.long_branch.g
        STUB x00000001.long_branch.g, .Lg_gep
        START c_g
.endif
.ifdef CASE_Z
## z has no .localentry: its entry is offset 0.
        .globl z
        .type  z, @function
z:
.Lz_gep:
        addi  3, 3, 1
        blr
        .size z, .-z
        CALLER c_z, x00000001.long_branch.z
        STUB x00000001.long_branch.z, .Lz_gep
        START c_z
.endif
