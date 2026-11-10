# R_PPC64_ADDR32 (JITLink edge kind ppc64::Pointer32) is "word32*, S + A" in the
# ELFv2 ABI. The field holds either a signed or an unsigned 32-bit value, so
# anything in [INT32_MIN, UINT32_MAX] must link and anything else is an error.
# This is the same range lld accepts (checkIntUInt(..., 32, ...)).
#
# RUN: rm -rf %t && mkdir -p %t
#
# Little endian: values that only fit as unsigned (0xdeadbeef), only fit as
# signed (0xffffffff80000000) and the addend case must all link and be written
# as the low 32 bits of S + A.
# RUN: llvm-mc --triple=powerpc64le-unknown-linux-gnu --filetype=obj \
# RUN:   -o %t/le.o %s
# RUN: llvm-jitlink --noexec \
# RUN:   --abs ext_hi=0xdeadbeef \
# RUN:   --abs ext_neg=0xffffffff80000000 \
# RUN:   --check %s %t/le.o
#
# Big endian: same checks.
# RUN: llvm-mc --triple=powerpc64-unknown-linux-gnu --filetype=obj \
# RUN:   -o %t/be.o %s
# RUN: llvm-jitlink --noexec \
# RUN:   --abs ext_hi=0xdeadbeef \
# RUN:   --abs ext_neg=0xffffffff80000000 \
# RUN:   --check %s %t/be.o
#
# Out of range, above UINT32_MAX (both endians).
# RUN: not llvm-jitlink --noexec \
# RUN:   --abs ext_hi=0x100000000 \
# RUN:   --abs ext_neg=0xffffffff80000000 \
# RUN:   %t/le.o 2>&1 | FileCheck --check-prefix=ERR %s
# RUN: not llvm-jitlink --noexec \
# RUN:   --abs ext_hi=0x100000000 \
# RUN:   --abs ext_neg=0xffffffff80000000 \
# RUN:   %t/be.o 2>&1 | FileCheck --check-prefix=ERR %s
#
# Out of range, below INT32_MIN (both endians).
# RUN: not llvm-jitlink --noexec \
# RUN:   --abs ext_hi=0xdeadbeef \
# RUN:   --abs ext_neg=0xffffffff7fffffff \
# RUN:   %t/le.o 2>&1 | FileCheck --check-prefix=ERR %s
# RUN: not llvm-jitlink --noexec \
# RUN:   --abs ext_hi=0xdeadbeef \
# RUN:   --abs ext_neg=0xffffffff7fffffff \
# RUN:   %t/be.o 2>&1 | FileCheck --check-prefix=ERR %s
#
# ERR: relocation target {{.*}} is out of range of Pointer32 fixup

	.text
	.globl main
	.type main,@function
main:
	blr
	.size main, .-main

	.data
	.globl p32_data
p32_data:
# jitlink-check: *{4}(p32_data) = ext_hi & 0xffffffff
	.long ext_hi
# jitlink-check: *{4}(p32_data + 4) = ext_neg & 0xffffffff
	.long ext_neg
# jitlink-check: *{4}(p32_data + 8) = (ext_hi + 4) & 0xffffffff
	.long ext_hi+4
