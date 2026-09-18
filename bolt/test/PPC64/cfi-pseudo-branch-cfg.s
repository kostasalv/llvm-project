## Regression test for analyzeBranch() commit a3d8e596be1f ("[BOLT][PowerPC]
## Skip pseudo instructions (CFI, debug annotations) in analyzeBranch()").
##
## PPCMCPlusBuilder::analyzeBranch() found a basic block's terminator by
## decrementing once from the block's end, with no check for trailing
## pseudo instructions such as CFI directives. A block whose real
## terminator (here: `bf 2, .LTargetA; b .LTargetB`) is followed by CFI
## pseudo-ops (`.cfi_restore`, emitted as `!CFI OpRestore` annotations) has
## its last "instruction" be a CFI pseudo-op, not the branch. None of
## analyzeBranch()'s branch-classification checks match a CFI pseudo-op, so
## it silently fell through to the "not a terminator" case and reported
## zero successors, while BinaryFunction::buildCFG() (which correctly skips
## pseudo instructions) had already wired up two real successors for this
## same block. That TBB/FBB-vs-Successors mismatch is exactly what
## BinaryBasicBlock::validateSuccessorInvariants() flags as an invalid CFG,
## which fires the validateCFG() assertion in postProcessBranches() -- this
## is exactly the shape of clang's C++ static-initializer functions, which
## carry dense CFI unwind info after their branches.
##
## Before the fix: llvm-bolt asserts/crashes on `foo` below ("invalid CFG"
## in postProcessBranches()). After the fix: `foo`'s block is correctly
## seen as having 2 successors and BOLT rewrites the binary cleanly.

# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple powerpc64le-unknown-linux-gnu %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -e _start --emit-relocs
# RUN: llvm-bolt %t.exe -o %t.bolt --print-cfg 2>&1 | FileCheck %s
# RUN: %t.bolt

# CHECK: BOLT-INFO: Target architecture: powerpc64le
# CHECK: BOLT-INFO: enabling relocation mode

## `foo`'s entry block ends in a real cond+uncond branch pair followed by
## trailing CFI pseudo-ops, then must still be correctly analyzed as having
## both branch targets as successors (not silently dropped to 0).
# CHECK-LABEL: Binary Function "foo" after building cfg
# CHECK: .LBB00
# CHECK:      bf 2, .Ltmp0
# CHECK-NEXT: b .Ltmp1
# CHECK-NEXT: !CFI{{.*}}OpRestore
# CHECK-NEXT: !CFI{{.*}}OpRestore
# CHECK: Successors: .Ltmp0, .Ltmp1

	.text
	.abiversion 2
	.globl foo
	.type foo, @function
foo:
	.localentry foo, 1
	.cfi_startproc
	cmpwi 3, 0
	bf 2, .LTargetA
	b .LTargetB
	.cfi_restore 31
	.cfi_restore 30
.LTargetB:
	li 3, 0
	blr
.LTargetA:
	li 3, 1
	blr
	.cfi_endproc
	.size foo, .-foo

	.globl _start
	.type _start, @function
_start:
	.localentry _start, 1
	li 3, 0
	bl foo
	nop
	li 0, 1
	sc
	.size _start, .-_start
