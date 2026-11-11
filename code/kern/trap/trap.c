#include <assert.h>
#include <clock.h>
#include <console.h>
#include <defs.h>
#include <kdebug.h>
#include <memlayout.h>
#include <mmu.h>
#include <riscv.h>
#include <stdio.h>
#include <trap.h>
#include <vmm.h>

#define TICK_NUM 100

static void print_ticks()
{
    cprintf("%d ticks\n", TICK_NUM);
#ifdef DEBUG_GRADE
    cprintf("End of Test.\n");
    panic("EOT: kernel seems ok.");
#endif
}

/* idt_init - initialize IDT to each of the entry points in kern/trap/vectors.S
 */
void idt_init(void)
{
    extern void __alltraps(void);
    /* Set sscratch register to 0, indicating to exception vector that we are
     * presently executing in the kernel */
    write_csr(sscratch, 0);
    /* Set the exception vector address */
    write_csr(stvec, &__alltraps);
    /* Allow kernel to access user memory */
    set_csr(sstatus, SSTATUS_SUM);
}

void print_trapframe(struct trapframe *tf)
{
    cprintf("trapframe at %p\n", tf);
    print_regs(&tf->gpr);
    cprintf("  status   0x%08x\n", tf->status);
    cprintf("  epc      0x%08x\n", tf->epc);
    cprintf("  badvaddr 0x%08x\n", tf->badvaddr);
    cprintf("  cause    0x%08x\n", tf->cause);
}

void print_regs(struct pushregs *gpr)
{
    cprintf("  zero     0x%08x\n", gpr->zero);
    cprintf("  ra       0x%08x\n", gpr->ra);
    cprintf("  sp       0x%08x\n", gpr->sp);
    cprintf("  gp       0x%08x\n", gpr->gp);
    cprintf("  tp       0x%08x\n", gpr->tp);
    cprintf("  t0       0x%08x\n", gpr->t0);
    cprintf("  t1       0x%08x\n", gpr->t1);
    cprintf("  t2       0x%08x\n", gpr->t2);
    cprintf("  s0       0x%08x\n", gpr->s0);
    cprintf("  s1       0x%08x\n", gpr->s1);
    cprintf("  a0       0x%08x\n", gpr->a0);
    cprintf("  a1       0x%08x\n", gpr->a1);
    cprintf("  a2       0x%08x\n", gpr->a2);
    cprintf("  a3       0x%08x\n", gpr->a3);
    cprintf("  a4       0x%08x\n", gpr->a4);
    cprintf("  a5       0x%08x\n", gpr->a5);
    cprintf("  a6       0x%08x\n", gpr->a6);
    cprintf("  a7       0x%08x\n", gpr->a7);
    cprintf("  s2       0x%08x\n", gpr->s2);
    cprintf("  s3       0x%08x\n", gpr->s3);
    cprintf("  s4       0x%08x\n", gpr->s4);
    cprintf("  s5       0x%08x\n", gpr->s5);
    cprintf("  s6       0x%08x\n", gpr->s6);
    cprintf("  s7       0x%08x\n", gpr->s7);
    cprintf("  s8       0x%08x\n", gpr->s8);
    cprintf("  s9       0x%08x\n", gpr->s9);
    cprintf("  s10      0x%08x\n", gpr->s10);
    cprintf("  s11      0x%08x\n", gpr->s11);
    cprintf("  t3       0x%08x\n", gpr->t3);
    cprintf("  t4       0x%08x\n", gpr->t4);
    cprintf("  t5       0x%08x\n", gpr->t5);
    cprintf("  t6       0x%08x\n", gpr->t6);
}

extern struct mm_struct *check_mm_struct;

//lab3 2412266: migrated interrupt and exception handlers.
void interrupt_handler(struct trapframe *tf) {
    // scause 的最高位是中断标志，其余位保存中断原因编号。
    // 清除最高位，取得用于分类处理中断的编号。
    uintptr_t cause = tf->cause & ~((uintptr_t)1 << (__riscv_xlen - 1));

    switch (cause) {
    case IRQ_U_SOFT:  // 框架定义的用户软件中断编号。
        cprintf("User software interrupt\n");
        break;
    case IRQ_S_SOFT:  // S 态软件中断；此处仅打印，不实现完整的中断源清理。
        cprintf("Supervisor software interrupt\n");
        break;
    case IRQ_H_SOFT:  // 框架保留的旧版虚拟化软件中断编号。
        cprintf("Hypervisor software interrupt\n");
        break;
    case IRQ_M_SOFT:  // 框架定义的机器态软件中断编号。
        cprintf("Machine software interrupt\n");
        break;
    case IRQ_U_TIMER:  // 框架定义的用户时钟中断编号。
        cprintf("User timer interrupt\n");
        break;
    case IRQ_S_TIMER:  // 本实验实际使用的 S 态时钟中断。
        clock_set_next_event();  // 通过 SBI 设置下一次触发时间，使时钟持续工作。
        ticks++;                 // 软件计数器加一，ticks 本身不是 CSR。
        if (ticks % TICK_NUM == 0) {  // 累计到 100、200、300……次时输出。
            print_ticks();
        }
        break;
    case IRQ_H_TIMER:  // 框架保留的旧版虚拟化时钟中断编号。
        cprintf("Hypervisor timer interrupt\n");
        break;
    case IRQ_M_TIMER:  // 框架定义的机器态时钟中断编号。
        cprintf("Machine timer interrupt\n");
        break;
    case IRQ_U_EXT:  // 框架定义的用户外部中断编号。
        cprintf("User external interrupt\n");
        break;
    case IRQ_S_EXT:  // S 态外部中断，此处仅打印类型。
        cprintf("Supervisor external interrupt\n");
        break;
    case IRQ_H_EXT:  // 框架保留的旧版虚拟化外部中断编号。
        cprintf("Hypervisor external interrupt\n");
        break;
    case IRQ_M_EXT:  // 框架定义的机器态外部中断编号。
        cprintf("Machine external interrupt\n");
        break;
    case IRQ_COP:  // 框架保留的协处理器中断编号。
        cprintf("Coprocessor interrupt\n");
        break;
    case IRQ_HOST:  // 框架保留的宿主中断编号。
        cprintf("Host interrupt\n");
        break;
    default:
        // 未识别的编号：打印原因和完整现场，便于定位问题。
        cprintf("Unknown interrupt: cause %d\n", (int)cause);
        print_trapframe(tf);
        break;
    }
}

/* lab3 功能二 2412266 */
// 同步异常处理：对本实验已知的测试指令，打印信息并跳过该指令。
void exception_handler(struct trapframe *tf) {
    switch (tf->cause) {
    case CAUSE_BREAKPOINT:
        cprintf("Breakpoint at %p\n", (void *)tf->epc);
        // 区分压缩指令和普通 32 位指令。
        tf->epc += ((*(const uint16_t *)tf->epc & 3) == 3) ? 4 : 2;
        break;
    case CAUSE_ILLEGAL_INSTRUCTION:
        cprintf("Illegal instruction at %p\n", (void *)tf->epc);
        // 沿用 Lab3 的演示异常跳过策略（16/32 位指令）。
        tf->epc += ((*(const uint16_t *)tf->epc & 3) == 3) ? 4 : 2;
        break;
    default:
        // Lab4 不提供按需分页；未处理异常应停止，避免无限重复。
        print_trapframe(tf);
        panic("unhandled exception");
    }
}






/* *
 * trap - handles or dispatches an exception/interrupt. if and when trap()
 * returns,
 * the code in kern/trap/trapentry.S restores the old CPU state saved in the
 * trapframe and then uses the iret instruction to return from the exception.
 * */
void trap(struct trapframe *tf)
{
    // dispatch based on what type of trap occurred
    if ((intptr_t)tf->cause < 0)
    {
        // interrupts
        interrupt_handler(tf);
    }
    else
    {
        // exceptions
        exception_handler(tf);
    }
}
