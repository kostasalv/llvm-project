# RUN: llvm-mc -triple powerpc64le-unknown-unknown -filetype=obj \
# RUN:   --defsym LIMIT=1 %s -o /dev/null
# RUN: llvm-mc -triple powerpc64-unknown-unknown -filetype=obj \
# RUN:   --defsym LIMIT=1 %s -o /dev/null
# RUN: not llvm-mc -triple powerpc64le-unknown-unknown -filetype=obj \
# RUN:   --defsym PAST=1 %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=PAST
# RUN: not llvm-mc -triple powerpc64-unknown-unknown -filetype=obj \
# RUN:   --defsym PAST=1 %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=PAST
# RUN: llvm-mc -triple powerpc64le-unknown-unknown -filetype=obj \
# RUN:   --defsym EXTERNAL=1 %s | llvm-readobj -r - | FileCheck %s --check-prefix=EXT
# RUN: llvm-mc -triple powerpc64le-unknown-unknown -filetype=obj \
# RUN:   --defsym NOTOC=1 %s | llvm-readobj -r - | FileCheck %s --check-prefix=NOTOC

## The displacement of b and bl is limited by the width of its field, which is
## encoded without the two low bits: 24 bits give -32 MiB to 32 MiB - 4. The
## cases here sit exactly on those limits. A branch that lands on the limit must
## assemble, and one that lands one instruction past it must be rejected and
## name the branch. The conditional branch is covered in
## fixup-range-boundary-bc.s, which needs only 32 KiB.
##
## Every case shares one 32 MiB span, so the test assembles about 32 MiB of
## zeroes once and discards the object.

.ifdef LIMIT
## Farthest reachable target in each direction. None of these may fail.
##
##   offset 0x0         b  to 0x1FFFFFC    forward by 32 MiB - 4
##   offset 0x4         bl to 0x2000000    forward by 32 MiB - 4
##   offset 0x2000000   b  to 0x0          backward by 32 MiB
##   offset 0x2000004   bl to 0x4          backward by 32 MiB
limit_b_bwd_target:
    b limit_b_fwd_target
limit_bl_bwd_target:
    bl limit_bl_fwd_target
    .space 0x1FFFFF4
limit_b_fwd_target:
    blr
limit_bl_fwd_target:
    b limit_b_bwd_target
    bl limit_bl_bwd_target
.endif

.ifdef PAST
## One instruction past the limit, in both directions, plus a temporary label
## as generated code uses (it must still be readable in the message).
##
##   offset 0x0         b  to 0x2000000    forward by 32 MiB
##   offset 0x4         b  to 0x2000008    forward by 32 MiB, through .Ltmp0
##   offset 0x2000004   b  to 0x0          backward by 32 MiB + 4
# PAST: error: branch target out of range (33554432 not between -33554432 and 33554428) to 'past_fwd_target' at .text+0x0
# PAST: error: branch target out of range (33554436 not between -33554432 and 33554428) to '.Ltmp0' at .text+0x4
# PAST: error: branch target out of range (-33554436 not between -33554432 and 33554428) to 'past_bwd_target' at .text+0x2000004
past_bwd_target:
    b past_fwd_target
    b .Ltmp0
    .space 0x1FFFFF8
past_fwd_target:
    blr
    b past_bwd_target
.Ltmp0:
    blr
.endif

.ifdef EXTERNAL
## A target the assembler cannot resolve is left to the linker, so a branch to
## it must not be diagnosed here. This is the control for the cases above: it
## shows the check fires only when the displacement is known. A @notoc branch is
## always left to the linker, even to a label in the same section, so it is
## never range checked by the assembler and belongs in this block too.
    bl external_func
    bl external_func@notoc
    bl near_target@notoc
    beq 0, external_label
    .section .other,"ax",@progbits
cross_section_target:
    blr
    .text
    b cross_section_target
near_target:
    blr
.endif

.ifdef NOTOC
## A @notoc branch to a label in the same section is left to the linker even
## when the displacement is known to be out of range, so the assembler must not
## diagnose it. Both directions are further than a b or bl can reach.
##
##   offset 0x0         bl to 0x2000008    forward by 32 MiB + 8
##   offset 0x2000004   bl to 0x0          backward by 32 MiB + 4
notoc_bwd_target:
    bl notoc_fwd_target@notoc
    .space 0x2000000
    bl notoc_bwd_target@notoc
notoc_fwd_target:
    blr
.endif

# EXT:      R_PPC64_REL24 external_func 0x0
# EXT-NEXT: R_PPC64_REL24_NOTOC external_func 0x0
# EXT-NEXT: R_PPC64_REL24_NOTOC .text 0x{{[0-9A-F]+}}
# EXT-NEXT: R_PPC64_REL14 external_label 0x0
# EXT-NEXT: R_PPC64_REL24 .other 0x0

# NOTOC:      R_PPC64_REL24_NOTOC .text 0x2000008
# NOTOC-NEXT: R_PPC64_REL24_NOTOC .text 0x0
