## Regression test for BinaryEmitter.cpp commit c0e56a33c3cb ("Fix
## BinaryEmitter hardcoding LEP offset 8 for all PPC64 functions").
##
## BinaryEmitter::emitFunction used to unconditionally stamp *every*
## BOLT-rewritten PPC64 function's symbol (in the intermediate object file
## BOLT emits and links via JITLink) with an ELFv2 local entry point (LEP)
## offset of 8 (encoded in st_other's STO_PPC64_LOCAL bits), on the
## assumption that BOLT always emits a 2-instruction global-entry-point
## (GEP) TOC-recompute preamble before a function's real body.
##
## That assumption is false for a leaf function with no TOC/r2 dependency
## (no ".localentry sym, N" with N>1 in the original assembly): such a
## function has no GEP preamble, so its LEP offset is 0, not 8. The
## st_other encoding matters because JITLink's ELF_ppc64 backend adds
## ELF::decodePPC64LocalEntryOffset(st_other) to the addend of every
## R_PPC64_REL24 relocation (see llvm/lib/ExecutionEngine/JITLink/
## ELF_ppc64.cpp) when resolving a direct call -- i.e. it is what
## determines where a `bl` to that symbol actually lands. Note this
## st_other is on the *intermediate* object BOLT emits/links internally,
## NOT the final rewritten ELF's .symtab (which BOLT populates by copying
## the original input symbols and only patching st_value/st_size/st_shndx
## -- so a static `llvm-readelf -sW` diff of the final binary's .symtab
## would NOT show this bug; the effect is only observable in where calls
## actually land).
##
## Before the fix, a direct call (`bl`) to a callee with no local entry
## (e.g. `leaf` here) incorrectly got the ABI-mandated +8 byte adjustment
## added to its resolved target address, landing the call 8 bytes into
## the function -- i.e. skipping `leaf` entirely and, in this test's
## layout, landing on the start of the next function, `marker`, instead.
##
## This test builds a caller (`_start`) that calls `leaf` (no local
## entry), checks the return value is what `leaf` (not some other
## function placed 8 bytes later) produces, and also directly verifies
## via disassembly of the BOLT output that the `bl` still targets `leaf`
## and not some other symbol/offset.
# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -e _start --emit-relocs
# RUN: llvm-bolt %t.exe -o %t.bolt --relocs 2>&1 | FileCheck %s --check-prefix=BOLT
# RUN: llvm-objdump -d --no-show-raw-insn %t.bolt | FileCheck %s
# RUN: %t.bolt

# BOLT: BOLT-INFO: enabling relocation mode

## The call in _start must still target `leaf` by symbol -- not `marker`
## (which, before the fix, is exactly what an incorrect +8 local-entry
## addend resolved to in this function layout: leaf is 8 bytes and has no
## local entry, so leaf+8 == marker's own entry address).
# CHECK: <_start>:
# CHECK-NEXT: bl {{.*}} <leaf>

        .text
        .abiversion 2

## Leaf function: no TOC dependency, no .localentry directive at all, so
## its ELFv2 local entry point offset is implicitly 0 (no GEP preamble).
## Exactly 8 bytes (2 instructions), so leaf+8 lands exactly on the next
## function's entry if (and only if) a caller's resolved call target is
## incorrectly adjusted by the ABI local-entry offset.
        .globl leaf
        .type  leaf, @function
leaf:
        li 3, 42
        blr
        .size leaf, .-leaf

## Placed immediately after `leaf` so that leaf's address + 8 (the
## erroneous adjustment this test guards against) lands exactly here.
        .globl marker
        .type  marker, @function
marker:
        li 3, 99
        blr
        .size marker, .-marker

        .globl _start
        .type  _start, @function
_start:
        .localentry _start, 1
        bl      leaf
        nop
        cmpwi   3, 42
        beq     ok
        li      3, 1
        b       done
ok:
        li      3, 0
done:
        li      0, 1
        sc
        .size _start, .-_start
