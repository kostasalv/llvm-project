## Two empty functions, 16 MiB and 17 MiB, so that the caller and the callee are
## more than 32 MiB apart and GNU ld has to build a `.long_branch.` stub.
        .section .text.pad1,"ax",@progbits
        .globl pad1
        .type  pad1, @function
pad1:   .space 0x1000000
        .size  pad1, .-pad1
        .section .text.pad2,"ax",@progbits
        .globl pad2
        .type  pad2, @function
pad2:   .space 0x1100000
        .size  pad2, .-pad2
