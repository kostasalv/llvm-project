# RUN: llvm-mc -triple=x86_64-unknown-linux -position-independent \
# RUN:     -filetype=obj -o %t.o %s
# RUN: llvm-jitlink -noexec -abs X=0x12 -check=%s %t.o
# RUN: not llvm-jitlink -noexec -abs X=0x123 %t.o 2>&1 | \
# RUN:   FileCheck -check-prefix=CHECK-ERROR %s
#
# Check success and failure cases of R_X86_64_16 handling.

# jitlink-check: *{8}P = X

# The fixup address must be the address being patched -- that is, the address of
# the block holding P plus the edge's offset -- and not the address of the
# relocation target. Binding FIXUP here and requiring it to reappear as the
# block address pins that down, since the fixup sits at offset 0 within P's
# block. Without this the two addresses can silently become the same value and
# the message stops being usable for diagnosing out-of-range branches.
# CHECK-ERROR: relocation target 0x123 (X) is out of range of Pointer8 fixup at address [[FIXUP:0x[0-9a-f]+]] (P, [[FIXUP]] +

	.text
	.section	.text.main,"ax",@progbits
	.globl	main
	.p2align	4, 0x90
	.type	main,@function
main:
	xorl	%eax, %eax
	retq
.Lfunc_end0:
	.size	main, .Lfunc_end0-main

	.type	P,@object
	.data
	.globl	P
P:
	.byte	X    # Using byte here generates R_X86_64_8.
	.byte   0
	.byte   0
	.byte   0
	.byte   0
	.byte   0
	.byte   0
	.byte   0
	.size	P, 8
