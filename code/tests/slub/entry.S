#include <memlayout.h>
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
    # Keep the current low PC mapped while installing the high direct map.
    la t0, test_root
    srli t0, t0, 12
    li t1, 8 << 60
    or t0, t0, t1
    csrw satp, t0
    sfence.vma
    li t1, PHYSICAL_MEMORY_OFFSET
    add sp, sp, t1
    add gp, gp, t1
    la t0, test_main
    add t0, t0, t1
    jr t0
.section .data
.balign 4096
test_root:
    .zero 2 * 8
    .quad (0x80000 << 10) | 0xcf
    .zero 508 * 8
    .quad (0x80000 << 10) | 0xcf
.section .bss.stack,"aw",@nobits
.balign 16
    .space 8192
stack_top:
