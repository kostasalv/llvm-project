## Test the PPC64 ELFv2 call slot (ELFv2 ABI, section 2.2.1.1): every call that
## may be routed through a linker stub must be followed by one slot
## instruction. BOLT must add the nop where it is missing, must not add a second
## one where a nop is already present, and must never replace an existing TOC
## restore, `ld r2,24(r1)`, with a nop.
# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -e _start --emit-relocs
# RUN: %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt 2>&1 | FileCheck %s
# RUN: llvm-objdump -d --no-show-raw-insn --section=.text %t.bolt | FileCheck %s --check-prefix=DIS
# RUN: %t.bolt
# CHECK: BOLT-INFO: Target architecture: powerpc64le
# CHECK: BOLT-INFO: enabling relocation mode
## _start: the first call had no slot and gets exactly one nop; the second
## already had a nop and must not get another one.
# DIS:      <_start>:
# DIS-NEXT: bl {{.*}}
# DIS-NEXT: nop
# DIS-NEXT: bl {{.*}}
# DIS-NEXT: nop
# DIS-NEXT: li 0, 1
## g is never called. Its call is followed by the TOC restore, which has to
## survive untouched, with no nop added before or after it.
# DIS:      <g>:
# DIS-NEXT: bl {{.*}}
# DIS-NEXT: ld 2, 24(1)
# DIS-NEXT: blr
        .text
        .abiversion 2
        .globl _start
        .type  _start, @function
_start:
        bl      f               # no call slot in the input
        bl      f               # explicit call slot
        nop
        li      0, 1            # exit(r3), r3 = 0 from f
        sc
        .size  _start, .-_start

        .globl f
        .type  f, @function
f:
        li      3, 0
        blr
        .size  f, .-f

        .globl g
        .type  g, @function
g:
        bl      f
        ld      2, 24(1)        # TOC restore in the call slot
        blr
        .size  g, .-g
