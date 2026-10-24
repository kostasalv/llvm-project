# RUN: llvm-mc -triple powerpc64le-unknown-unknown -filetype=obj \
# RUN:   --defsym LIMIT=1 %s -o /dev/null
# RUN: llvm-mc -triple powerpc64-unknown-unknown -filetype=obj \
# RUN:   --defsym LIMIT=1 %s -o /dev/null
# RUN: not llvm-mc -triple powerpc64le-unknown-unknown -filetype=obj \
# RUN:   --defsym PAST=1 %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=PAST
# RUN: not llvm-mc -triple powerpc64-unknown-unknown -filetype=obj \
# RUN:   --defsym PAST=1 %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=PAST

## The conditional branch has a 14 bit field, encoded without the two low bits,
## so it reaches -32 KiB to 32 KiB - 4. A branch that lands on the limit must
## assemble, and one that lands one instruction past it must be rejected and
## name the branch.

.ifdef LIMIT
##   offset 0x0      beq to 0x7FFC    forward by 32 KiB - 4
##   offset 0x8000   beq to 0x0       backward by 32 KiB
limit_bwd_target:
    beq 0, limit_fwd_target
    .space 0x7FF8
limit_fwd_target:
    blr
    beq 0, limit_bwd_target
.endif

.ifdef PAST
##   offset 0x0      beq to 0x8000    forward by 32 KiB
##   offset 0x8004   beq to 0x0       backward by 32 KiB + 4
# PAST: error: branch target out of range (32768 not between -32768 and 32764) to 'past_fwd_target' at .text+0x0
# PAST: error: branch target out of range (-32772 not between -32768 and 32764) to 'past_bwd_target' at .text+0x8004
past_bwd_target:
    beq 0, past_fwd_target
    .space 0x7FFC
past_fwd_target:
    blr
    beq 0, past_bwd_target
.endif
