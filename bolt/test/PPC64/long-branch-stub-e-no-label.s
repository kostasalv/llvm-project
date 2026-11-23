## SYNTHETIC test (hand-made stub, linked with ld.lld). Not reachable with the
## stubs GNU ld writes: every one of them carries a relocation, which gives its
## target offset a label (3,269 of 3,269 stubs in llc, MEASURED).
##
## Here the stub is a `b` to a label in the same section as the stub, so the
## assembler resolves it and the stub has no relocation. Nothing then gives e+8
## a label (a stub is never disassembled), the pass finds no label at the entry
## the stub reaches, counts it as no-label-at-target and leaves the call alone.
## With h, which calls e directly (ld.lld sends that call to e+8, which makes the
## label), the same stub is redirected.

# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu -I %S/Inputs \
# RUN:   --defsym CASE_NOLABEL=1 %s -o %t.n.o
# RUN: ld.lld %t.n.o -o %t.n.exe -e _start --emit-relocs
# RUN: %t.n.exe
# RUN: llvm-bolt %t.n.exe -o %t.n.bolt --lite=0 2>&1 | FileCheck %s --check-prefix=NOLABEL
# RUN: llvm-objdump -d --no-show-raw-insn %t.n.bolt --disassemble-symbols=c_e \
# RUN:   | FileCheck %s --check-prefix=NOLABEL-OUT
# RUN: %t.n.bolt
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu -I %S/Inputs \
# RUN:   --defsym CASE_LABEL=1 %s -o %t.l.o
# RUN: ld.lld %t.l.o -o %t.l.exe -e _start --emit-relocs
# RUN: %t.l.exe
# RUN: llvm-bolt %t.l.exe -o %t.l.bolt --lite=0 2>&1 | FileCheck %s --check-prefix=LABEL
# RUN: llvm-nm %t.l.bolt > %t.l.txt
# RUN: llvm-objdump -d --no-show-raw-insn %t.l.bolt --disassemble-symbols=c_e >> %t.l.txt
# RUN: FileCheck %s --check-prefix=LABEL-OUT --input-file=%t.l.txt
# RUN: %t.l.bolt

# NOLABEL: BOLT-INFO: long_branch stubs: 0 calls redirected, skipped 0 (target not moved), 0 (target ignored), 0 (entry rule), 1 (no-label-at-target), 0 (not a plain b), 0 (stub not recognised), 0 (not a valid stub start)
# NOLABEL-OUT-LABEL: <c_e>:
# NOLABEL-OUT: bl 0x{{[0-9a-f]+}} <c_e+0x{{[0-9a-f]+}}>

# LABEL: BOLT-INFO: long_branch stubs: 1 calls redirected, skipped 0 (target not moved), 0 (target ignored), 0 (entry rule), 0 (no-label-at-target), 0 (not a plain b), 0 (stub not recognised), 0 (not a valid stub start)
# LABEL-OUT: [[#%x,E:]] T e
# LABEL-OUT-LABEL: <c_e>:
# LABEL-OUT: bl 0x[[#%x,E+8]] <e+0x8>

        .include "long-branch-stub.inc"
        CALLEE e
.ifdef CASE_LABEL
        CALLER h, e
.endif
        CALLER c_e, x00000001.long_branch.e
x00000001.long_branch.e:
        b     .Le_lep
        START c_e
