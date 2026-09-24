## Test that PatchEntries redirects BOTH PPC64 ELFv2 entry points of a
## function it patches.
##
## In non-relocation ("patch") mode BOLT leaves a function's original code
## in place and overwrites its entry point with a redirect to the relocated
## copy. The redirect is a 28-byte absolute long-tail-call
## (PPCMCPlusBuilder::createLongTailCall), which spans bytes [0, 28) and so
## covers the ELFv2 local entry point at offset 8. A caller that already
## has r2 set up branches straight to func+8 and would land in the middle
## of that sequence.
##
## PatchEntries must therefore split the redirect:
##
##   offset 0     b <local entry patch>          <- global entry
##   offset 4     left untouched
##   offset 8     lis8 r12, ...; mtctr r12; bctr <- local entry
##
## Both entries reach the *global* entry point of the new function, which
## rebuilds r2 from r12 itself. Leaving offset 4 untouched is ABI-legal:
## ELFv2 2.3.2.1 states that addresses between the global and local entry
## points must not be branch targets.
##
## `with_toc` is padded to 64 bytes so that the 8 + 28 = 36 bytes the split
## needs fit; the too-small case is covered by
## patch-entries-local-entry-overlap.s.
##
## As in that test, -force-patch routes every function through the patching
## path without needing a real non-relocation binary.
# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -e _start --emit-relocs
# RUN: llvm-bolt %t.exe -o %t.bolt -lite -force-patch -v=1 2>&1 | FileCheck %s
# RUN: %t.bolt
# RUN: llvm-readelf -sW %t.bolt | FileCheck %s --check-prefix=SYMTAB
# RUN: llvm-objdump -d --disassemble-symbols=with_toc.org.gep,with_toc.org.0 \
# RUN:   %t.bolt | FileCheck %s --check-prefix=PATCH

# CHECK: BOLT-INFO: Target architecture: powerpc64le

## with_toc must now be patched, not abandoned.
# CHECK-NOT: failed to patch entries in with_toc

## Both halves of the split redirect must exist as patch functions: the
## forwarding branch at the global entry point and the long-tail-call at
## the local entry point.
# SYMTAB-DAG: with_toc.org.gep
# SYMTAB-DAG: with_toc.org.0

## The global-entry patch is a single unconditional branch to the
## local-entry patch, which is the absolute CTR-dispatch sequence. r12 is
## loaded with the target address, as the ELFv2 ABI requires of a caller
## entering a global entry point.
# PATCH-LABEL: <with_toc.org.gep>:
# PATCH-NEXT:    b
# PATCH-LABEL: <with_toc.org.0>:
# PATCH-NEXT:    lis 12,
# PATCH:         mtctr 12
# PATCH-NEXT:    bctr

        .text
        .abiversion 2

## TOC-dependent function with a genuine 2-instruction GEP preamble, so its
## local entry point is at offset 8. Padded past 36 bytes so the split
## redirect fits.
        .globl with_toc
        .type  with_toc, @function
with_toc:
        .localentry with_toc, 8
        addis   2, 12, .TOC.-with_toc@ha
        addi    2, 2, .TOC.-with_toc@l
        li      3, 7
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        blr
        .size with_toc, .-with_toc

        .globl _start
        .type  _start, @function
_start:
        .localentry _start, 1
        bl      with_toc
        nop
        li      0, 1
        li      3, 0
        sc
        .size _start, .-_start
