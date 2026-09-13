## Regression test for PatchEntries.cpp commits 96098e1d607e ("Fix
## PatchEntries silently corrupting PPC64 local entry points") and
## 45e9ff25018a ("Fix PatchEntries missing PPC64 local-entry overlap
## check").
##
## In non-relocation ("patch") mode, BOLT leaves a function's original
## code in place and instead overwrites its entry point(s) with a
## long-tail-call redirect to the relocated copy. For ELFv2 PPC64
## functions that have a TOC-recompute global-entry preamble, this
## created a genuine conflict that PatchEntries did not originally
## detect:
##
##  - The redirect written at offset 0 (the function's global entry
##    point, GEP) is a 7-instruction/28-byte absolute long-tail-call
##    (see PPCMCPlusBuilder::createLongTailCall) that spans bytes [0, 28).
##  - The function's ELFv2 *local* entry point (LEP, the address ABI
##    callers that already have r2 set up branch to directly, skipping
##    the GEP's TOC recompute) sits at a small fixed offset from the GEP
##    -- typically offset 8 -- which is *inside* that [0, 28) span.
##
## Patching the GEP therefore unavoidably overwrites the LEP's original
## bytes with the middle of the long-tail-call sequence. A direct branch
## to the LEP (as ELFv2 callers that already have r2 are meant to use)
## then lands mid-stub on an instruction that assumes earlier stub
## instructions already ran -- producing a garbage absolute address and
## a wild branch at runtime. BOLT must detect this overlap up front and
## mark the function Ignored (leave the original bytes untouched, do not
## optimize it) rather than silently emit the corrupting redirect.
##
## This is only reachable in non-relocation mode, which BOLT does not
## select by default when it can use relocations (as in the other PPC64
## lit tests in this directory). We force it here with -force-patch,
## which routes every function through the exact overlap-check code path
## this test targets, without needing a real profile-less non-relocation
## binary or a huge function to cross PatchThreshold.
# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -e _start --emit-relocs
# RUN: llvm-bolt %t.exe -o %t.bolt -lite -force-patch -v=1 2>&1 | FileCheck %s
# RUN: %t.bolt
# RUN: llvm-readelf -sW %t.bolt | FileCheck %s --check-prefix=SYMTAB

# CHECK: BOLT-INFO: Target architecture: powerpc64le

## `with_toc`'s local entry point (offset 8) overlaps the 28-byte
## global-entry redirect patch (offset 0..27), so it must be reported as
## unpatchable and the function must be marked ignored -- not silently
## corrupted.
# CHECK: BOLT-INFO: unable to patch entry point in with_toc at offset 0x8
# CHECK-SAME: local entry point overlaps global-entry patch
# CHECK-NEXT: BOLT-WARNING: failed to patch entries in with_toc

## Since with_toc was left Ignored, its original bytes (and address) must
## be unchanged in the output -- i.e. its local-entry-point annotation
## (offset 8, st_other bits 0x60) must still be present and correct,
## exactly as in the unmodified input, rather than showing a
## createLongTailCall stub's clobbered/incorrect encoding.
# SYMTAB: FUNC{{.*}}DEFAULT [<other: 0x60>]{{ +}}{{[0-9]+}} with_toc

        .text
        .abiversion 2

## TOC-dependent function with a genuine 2-instruction GEP preamble
## (local entry point at offset 8) -- the overlap case this test targets.
        .globl with_toc
        .type  with_toc, @function
with_toc:
        .localentry with_toc, 8
        addis   2, 12, .TOC.-with_toc@ha
        addi    2, 2, .TOC.-with_toc@l
        li      3, 7
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
