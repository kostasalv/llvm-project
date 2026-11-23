## SYNTHETIC test (hand-made stub, linked with ld.lld): a call through a stub is
## redirected only if the function the stub reaches is moved. u is left in place
## with --skip-funcs (an ignored function, counted as "target ignored"), so the
## call keeps its stub (and LongJmp's relay). The same
## program without --skip-funcs is the control: the call is then redirected.

# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu -I %S/Inputs \
# RUN:   %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -e _start --emit-relocs
# RUN: %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt --lite=0 --skip-funcs=u 2>&1 | FileCheck %s
# RUN: llvm-objdump -d --no-show-raw-insn %t.bolt --disassemble-symbols=c_u \
# RUN:   | FileCheck %s --check-prefix=KEPT
# RUN: %t.bolt
# RUN: llvm-bolt %t.exe -o %t.moved --lite=0 2>&1 | FileCheck %s --check-prefix=MOVED
# RUN: llvm-nm %t.moved > %t.txt
# RUN: llvm-objdump -d --no-show-raw-insn %t.moved --disassemble-symbols=c_u >> %t.txt
# RUN: FileCheck %s --check-prefix=MOVED-OUT --input-file=%t.txt
# RUN: %t.moved

# CHECK: BOLT-INFO: long_branch stubs: 0 calls redirected, skipped 0 (target not moved), 1 (target ignored), 0 (entry rule), 0 (no-label-at-target), 0 (not a plain b), 0 (stub not recognised), 0 (not a valid stub start)
## The call goes to a relay inside c_u, not to u.
# KEPT-LABEL: <c_u>:
# KEPT: bl 0x{{[0-9a-f]+}} <c_u+0x{{[0-9a-f]+}}>

# MOVED: BOLT-INFO: long_branch stubs: 1 calls redirected, skipped 0 (target not moved), 0 (target ignored), 0 (entry rule), 0 (no-label-at-target), 0 (not a plain b), 0 (stub not recognised), 0 (not a valid stub start)
# MOVED-OUT: [[#%x,U:]] T u
# MOVED-OUT-LABEL: <c_u>:
# MOVED-OUT: bl 0x[[#%x,U+8]] <u+0x8>

        .include "long-branch-stub.inc"
        CALLEE u
        CALLER c_u, x00000001.long_branch.u
        STUB x00000001.long_branch.u, .Lu_lep
        START c_u
