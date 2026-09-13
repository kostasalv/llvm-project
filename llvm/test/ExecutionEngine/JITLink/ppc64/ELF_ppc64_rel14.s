# REQUIRES: system-linux
# RUN: rm -rf %t && mkdir -p %t
# RUN: llvm-mc --triple=powerpc64le-unknown-linux-gnu --filetype=obj -o \
# RUN:   %t/elf_rel14.o %s
# RUN: llvm-jitlink --noexec --check %s %t/elf_rel14.o
# RUN: llvm-mc --triple=powerpc64-unknown-linux-gnu --filetype=obj -o \
# RUN:   %t/elf_rel14.o %s
# RUN: llvm-jitlink --noexec --check %s %t/elf_rel14.o
#
# Regression test for R_PPC64_REL14 / R_PPC64_REL14_BRTAKEN /
# R_PPC64_REL14_BRNTAKEN support in JITLink's ppc64 backend (Delta14 edge
# kind).
#
# PPC64 14-bit conditional branches (bc/bdnz/...) that target a symbol in a
# *different* section (so the assembler cannot resolve the branch displacement
# itself and must emit a relocation) previously hit:
#
#   JITLink failed: Unsupported ppc64 relocation type R_PPC64_REL14
#
# because ppc64::EdgeKind_ppc64 had an absolute Pointer14 (R_PPC64_ADDR14)
# edge kind but no PC-relative counterpart. This test forces the assembler to
# emit a real R_PPC64_REL14 relocation (by branching to a symbol defined in a
# later, separately-emitted section so the offset can't be folded at
# assembly time) and checks that JITLink resolves it to the correct
# PC-relative displacement.

  .text
  .abiversion 2
  .globl main
  .p2align 4
  .type main,@function
main:
  li 3, 0
  blr
  .size main, .-main

# Conditional branch (bc, AA=0) to a symbol in another section: the
# assembler cannot compute BD locally, so it must emit R_PPC64_REL14.
# jitlink-check: (decode_operand(test_rel14, 2) << 2)[15:0] = \
# jitlink-check:   (far_target - test_rel14)[15:0]
  .globl test_rel14
  .p2align 4
  .type test_rel14,@function
test_rel14:
  bc 12, 2, far_target
  blr
  .size test_rel14, .-test_rel14

# Put the target far enough away (a distinct, later section) that the
# assembler must leave the displacement for the linker to fill in via a
# relocation rather than resolving it as a local fixed offset.
  .section .far_text,"ax",@progbits
  .globl far_target
  .p2align 4
  .type far_target,@function
far_target:
  li 3, 1
  blr
  .size far_target, .-far_target
