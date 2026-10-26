.section .text.entry,"ax"
.globl _start
_start:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    la sp, stack_top
    la t0, __bss_start
    la t1, __bss_end
1:
    bgeu t0, t1, 2f
    sd zero, 0(t0)
    addi t0, t0, 8
    j 1b
2:
    call test_main
3:
    wfi
    j 3b
.section .bss.stack,"aw",@nobits
.balign 16
    .space 65536
stack_top:
