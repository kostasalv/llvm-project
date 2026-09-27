## Regression test: a '.branch_lt' slot pointing at a function's ELFv2 local
## entry point must not be registered as an internal data reference.
##
## '.branch_lt' is the linker's branch lookup table -- 8-byte absolute function
## addresses used by '.plt_branch.' trampolines for calls beyond the 26-bit `bl`
## range. Each slot carries an R_PPC64_RELATIVE entry in '.rela.branch_lt'.
##
## A slot may hold either entry point of its target: Func+0 (the global entry,
## which recomputes r2 from r12) or Func+LocalEntryOffset, normally Func+8 (the
## local entry, used when the caller already holds the right TOC base). The
## second case used to be misfiled:
##
##   readBranchLTRelocations()
##     -> handleRelativeDynamicRelocation()
##          ReferenceOffset = ReferencedAddress - Func->getAddress()   // == 8
##          -> registerInternalRefDataRelocation()                     // wrong
##
## which means "data references the interior of this function at an offset I
## cannot explain" -- BOLT's signal for a computed branch whose jump table it
## does not control. validateInternalRefDataRelocations() then only clears
## offsets covered by a recognized jump table, so a local entry was never
## claimed. It warned and returned false, and postProcessCFG() responded with
## setSimple(false): the function stayed correct but was excluded from every
## optimization pass.
##
## On a `clang` binary 7067 of 7902 '.branch_lt' entries point at Func+8, which
## silently de-optimized 5708 functions. Nothing there was a genuine interior
## reference: the other 835 slots point at Func+0, and all 146133 code
## references in '.rela.data.rel.ro' target Func+0 as the ABI requires.
##
## The local entry needs no such bookkeeping. patchELFBranchLT() resolves
## through getNewFunctionAddress(), which matches exact function starts only, so
## a Func+8 slot is deliberately left pointing at the original address -- which
## stays reachable because PatchEntries installs a split global/local entry
## redirect there.
##
## This test builds both slot kinds by hand. '.rela.branch_lt' is written as raw
## Elf64_Rela entries because that is exactly how readBranchLTRelocations()
## reads it: object::SectionRef::relocations() does not associate
## '.rela.branch_lt' with '.branch_lt' on the toolchain that produces it, so
## BOLT parses the bytes directly.
# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -e _start --emit-relocs
# RUN: llvm-bolt %t.exe -o %t.bolt 2>&1 | FileCheck %s

# CHECK: BOLT-INFO: Target architecture: powerpc64le

## The Func+8 slot must not be reported as an unexplained interior reference,
## and must not cost the function its optimizations.
# CHECK-NOT: unclaimed data relocation

## The rewritten binary still runs: both slots reach the right function.
# RUN: %t.bolt

        .text
        .abiversion 2

## has_lep gets a 2-instruction global-entry TOC preamble, so ".localentry
## has_lep, 8" -- the assembler encodes st_other such that
## decodePPC64LocalEntryOffset() returns 8, which is what
## BinaryFunction::getPPC64LocalEntryOffset() records at discovery time.
        .globl  has_lep
        .type   has_lep, @function
has_lep:
        addis   2, 12, .TOC.-has_lep@ha         # global entry: recompute r2
        addi    2, 2, .TOC.-has_lep@l
        .localentry has_lep, .-has_lep         # local entry starts here (+8)
        li      3, 42
        blr
        .size has_lep, .-has_lep

        .globl  _start
        .type   _start, @function
_start:
        .localentry _start, 1
## Call through the global-entry slot. r12 must hold the target, per the ABI.
        lis     30, branch_lt@ha
        addi    30, 30, branch_lt@l
        ld      12, 0(30)                       # slot 0: has_lep+0
        mtctr   12
        bctrl
        cmpdi   3, 42
        bne     .Lfail
## Call through the local-entry slot. It skips the preamble, so r2 must already
## be correct -- it is, since we never clobbered it.
        ld      12, 8(30)                       # slot 1: has_lep+8
        mtctr   12
        bctrl
        cmpdi   3, 42
        bne     .Lfail
        li      0, 1                            # syscall: exit
        li      3, 0
        sc
.Lfail:
        li      0, 1
        li      3, 1
        sc
        .size _start, .-_start

## The branch lookup table: one slot per entry point kind.
        .section .branch_lt,"aw",@progbits
        .align 3
branch_lt:
        .quad   has_lep                         # global entry
        .quad   has_lep+8                       # local entry -- the case at issue

## Hand-built Elf64_Rela entries: { r_offset, r_info, r_addend }, 24 bytes each.
## r_info's low 32 bits are the type; R_PPC64_RELATIVE is 22 and carries no
## symbol, so the target address lives entirely in r_addend.
        .section .rela.branch_lt,"a",@progbits
        .align 3
        .quad   branch_lt                       # r_offset: slot 0
        .quad   22                              # R_PPC64_RELATIVE
        .quad   has_lep                         # r_addend: Func+0
        .quad   branch_lt+8                     # r_offset: slot 1
        .quad   22                              # R_PPC64_RELATIVE
        .quad   has_lep+8                       # r_addend: Func+LEP
