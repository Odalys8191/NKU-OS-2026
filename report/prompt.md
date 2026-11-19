# OS Lab4 Prompt 汇总

## Prompt 1：Sv39 虚拟内存页操作

```text
[PROMPT]
你是一名熟悉 RISC-V Sv39 分页机制和 ucore 内核的操作系统开发助手。完成 Lab4 虚拟内存页操作模块，在现有框架内实现 page_insert()、page_remove() 和 page_remove_pte()，并核对 get_pte() 创建非叶子表项的方式，使页表内容、数据页引用计数、物理页回收和 TLB 同步保持一致。

[RELY]
主要文件为 code/kern/mm/pmm.c，并结合 pmm.h、mmu.h、memlayout.h 和现有 PMM 接口理解 Page、PTE、地址转换及引用计数。当前使用 Sv39 三级页表和 4 KiB 基础页；pgdir 是页表根的内核虚拟地址，不能与页表物理地址混用。使用已有的 get_pte()、page_ref_inc()、page_ref_dec()、pte2page()、page2ppn()、pte_create()、ptd_create()、free_page() 和 tlb_invalidate()。

[GUARANTEE]
保持现有函数签名、结构体布局和模块边界，不修改无关模块，不伪造测试输出。实现后说明首次映射、同页重映射、异页替换、重复删除和分配失败时的状态变化，并执行真实编译和测试。

[SPECIFICATION]
1. page_insert() 获取目标 PTE 失败时返回 -E_NO_MEM，且不得增加数据页引用。
2. 首次映射增加一次引用；同页重映射只更新权限，引用计数净变化为 0；异页替换先持有新页，再撤销旧映射。
3. 重建目标表项时权限必须与 perm 一致，不保留旧权限；修改后同步 TLB。
4. page_remove() 使用 create=0 查找，不能因删除而新建页表。
5. 有效映射按清除表项、同步 TLB、递减引用的顺序处理；引用归零后才能回收数据页。
6. 重复删除不能导致引用下溢或重复释放；本接口只撤销数据页映射，不自动释放中间页表。
7. get_pte() 创建非叶子表项时使用 ptd_create()，不得设置叶子权限及 U、A、D 位。
8. 若多级页表创建过程中后续分配失败，已接入页表树的前级页表可能保留，不得把 NULL 描述成全部分配已回滚。
9. 使用官方检查和独立边界测试验证首次插入、同页更新、异页替换、重复删除、共享页引用、TLB 同步及分配失败路径。
```

## Prompt 2：进程创建与初始化

```text
[PROMPT]
修改 code/kern/process/proc.c，完成 Lab4 进程创建与初始化模块，实现 PCB 初始化、内核线程创建、进程资源管理、进程登记及首次执行现场构造。

[RELY]
结合 proc.h 中的 proc_state、proc_struct、context、MAX_PROCESS、MAX_PID、KSTACKPAGE 和创建标志，使用现有 alloc_proc()、setup_kstack()、put_kstack()、copy_mm()、copy_thread()、get_pid()、do_fork()、kernel_thread()、forkret() 及物理页分配接口。Lab2/Lab3 的物理内存、虚拟内存和 Trap 前置功能已经完成。

[GUARANTEE]
直接修改实际源码，保留课程 LAB4 标记和公共接口，不改变结构体布局或无关逻辑。不得修改官方测试、跳过功能或伪造成功输出。完成后执行编译、官方测试和独立边界测试，并报告真实结果与未验证部分。

[SPECIFICATION]
1. alloc_proc() 初始化所有 PCB 字段，状态为 PROC_UNINIT，PID 为未分配值，链表节点和上下文处于可安全使用的初始状态。
2. setup_kstack() 分配 KSTACKPAGE 个连续页并记录栈底；失败时返回 -E_NO_MEM。put_kstack() 必须按相同页数释放并清空记录。
3. copy_mm() 只处理本阶段的内核线程：proc->mm 为 NULL，proc->pgdir 使用 boot_pgdir_pa；遇到不支持的用户地址空间时明确失败，不虚构用户页表复制或写时复制。
4. copy_thread() 将 Trapframe 放到新线程内核栈顶部，复制父现场后设置返回值和入口参数；context.ra 指向 forkret，context.sp 指向该 Trapframe，并满足 RISC-V 16 字节栈对齐。
5. get_pid() 在合法范围内生成未占用 PID，正确处理回绕和接近上限的情况。
6. do_fork() 按“分配 PCB—分配内核栈—复制地址空间—构造现场—分配 PID—加入全局链表—发布 RUNNABLE”顺序执行。只有完整初始化后才能发布为可调度状态。
7. 任一步失败时按资源获取的逆序回收，不能遗留 PCB、内核栈、PID 或链表节点。
8. kernel_thread() 正确构造内核线程入口，确保函数地址和参数能通过首次 Trap 返回链路传递。
9. 测试正常创建、连续 PID、PID 回绕、内核栈分配失败、地址空间复制失败、资源回收、Trapframe 位置、context 初值和栈对齐。
```

## Prompt 3：RV64 上下文切换

```text
[PROMPT]
在 code/kern/process/switch.S 实现 RV64 内核线程的上下文切换入口。先确认 C 结构体布局与汇编寄存器宽度，直接交付实现，并使用交叉汇编和反汇编核对保存、恢复与返回行为。

[RELY]
使用 proc.h 的 struct context、proc.c 中 switch_to() 的声明和 copy_thread() 对 ra、sp 的初始化，以及 libs/riscv.h 中 RV64 的 LOAD、STORE、REGBYTES 定义。

[GUARANTEE]
导出 switch_to(struct context *from, struct context *to) 全局符号，保持 struct context 布局不变，交付槽位对应关系及反汇编证据。

[SPECIFICATION]
a0 指向 from，a1 指向 to。将离开线程的 ra、sp、s0-s11 保存到 from 的对应字段，再从 to 恢复同一组 64 位字段，最后沿恢复的 ra 在目标栈上继续执行。恢复旧线程应回到其此前暂停的 switch_to() 调用点；新线程应通过 copy_thread() 准备的 forkret 和内核栈进入首次运行链路。运行 make obj/kern/process/switch.o 和 riscv64-unknown-elf-objdump -dr，核对全局符号、14 个槽位、8 字节读写、0～104 字节偏移及最终控制转移。
```

## Prompt 4：当前线程与页表交接

```text
[PROMPT]
在 code/kern/process/proc.c 补齐 proc_run()，完成不同 PCB 之间的切换，使目标进程成为 current、硬件使用目标进程的 Sv39 地址空间，并在目标寄存器现场继续执行。

[RELY]
使用 proc_struct、context、current、switch_to()、copy_thread()、copy_mm()、forkret()、local_intr_save()/local_intr_restore()、flush_tlb()、CSR 宏、SATP_MODE_SV39 和 RISCV_PGSHIFT。当前 PCB 的 pgdir 保存页表根物理地址，内核线程共享 boot_pgdir_pa。

[GUARANTEE]
实现 void proc_run(struct proc_struct *proc)，说明同线程调用结果、跨线程交接不变量，以及被切出线程恢复后对应的中断状态。

[SPECIFICATION]
1. 目标等于 current 时直接返回，不改变上下文、satp 和中断状态。
2. 跨线程时，即使共享页表根，也必须交接 current 和执行现场。
3. 在本地中断保护下完成 PCB、地址空间和栈的交接，使目标线程接手时三者一致。
4. 按 RV64 Sv39 编码 satp：模式写入 63:60 位，pgdir 物理地址右移 12 位得到根页 PPN，ASID 使用当前项目约定。
5. 写入 satp 后调用 flush_tlb()，再执行 switch_to(&prev->context, &proc->context)。
6. 已运行线程恢复后，应在自己的栈和调用点继续，并使用自己保存的标志结束临界区。
7. 编译 proc.o，核对 MODE、PPN、CSR 写入和 sfence.vma；区分普通恢复与新线程首次进入 forkret 的路径，并按实际测试范围报告结果。
```

## Prompt 5：调度选择与 idle 入口

```text
[PROMPT]
完成 Lab4 调度选择和启动触发入口：在 code/kern/schedule/sched.c 实现 schedule()，在 code/kern/process/proc.c 实现 cpu_idle()，沿用实际进程链表和状态约定。

[RELY]
使用 proc_list、current、idleproc、list_link、need_resched、runs、PROC_RUNNABLE、le2proc、环形链表接口、local_intr_save()/local_intr_restore()、proc_run()、proc_init()、do_fork() 及 kern_init() 的启动顺序。当前版本把 idleproc 加入 proc_list，新线程通过 list_add() 插入表头之后。

[GUARANTEE]
实现 void schedule(void) 和 void cpu_idle(void)。schedule() 完成一次有界选择，cpu_idle() 持续响应重调度请求。保留函数声明，给出边界场景的候选、计数及真实验证证据。

[SPECIFICATION]
1. schedule() 在本地中断保护下消费当前线程的 need_resched。
2. 当前为 idle 时从首个实际节点开始；当前为普通线程时从其后继开始，越过表头后环形继续。
3. 一轮内选择第一个 PROC_RUNNABLE 的普通线程，跳过表头、idle 和不可运行 PCB。
4. 只有当前普通线程可运行时，允许绕回选中自身；没有可运行普通线程时回退 idle。
5. 按项目约定给每次调度选中的 PCB 的 runs 加一，包括选中自身和回退 idle；仅在目标不同于 current 时调用 proc_run()。
6. cpu_idle() 保持循环，仅在观察到 current->need_resched 时调用 schedule()。
7. 覆盖 idle 出发、普通线程跨表头、不可运行节点、仅当前可运行、无普通线程五类边界，记录候选、runs、need_resched 和是否切换。
8. 完整构建后用 QEMU 验证启动确实从 idle 进入 init，并运行官方调度测试。
```

## Prompt 6：中断开关与首次 Trap 返回分析

```text
[PROMPT]
从当前 Lab4 源码推导中断开关 Challenge 的状态表，串联普通临界区、schedule()/proc_run() 的嵌套保护，以及新线程首次 Trap 返回过程，输出源码分析、自检问题和可复验证据。

[RELY]
结合 kern/sync/sync.h、kern/driver/intr.c、libs/riscv.h、当前 schedule()/proc_run()、kernel_thread()、copy_thread()、forkret() 和 kern/trap/trapentry.S 中的 __trapret。

[GUARANTEE]
交付带文件和函数位置的调用链、两组嵌套临界区状态表，以及首次运行与已切出线程恢复的对照。所有描述以实际源码和观察记录为依据。

[SPECIFICATION]
1. 沿 local_intr_save/restore、内联函数、intr_enable/disable 和 sstatus.SIE 逐层追踪。
2. 分别以外层进入前 SIE=1 和 SIE=0 为起点，记录外层与内层保存值、保护区内 SIE、内层返回后 SIE 和外层返回后 SIE。
3. 核对 restore(0) 的真实行为及嵌套临界区正确配对的条件。
4. 说明线程切出后如何在恢复自己的栈与调用点后，继续结束属于自己的 proc_run() 和 schedule() 临界区。
5. 沿 forkret → forkrets → __trapret → sret 分析新线程首次运行时 SIE、SPIE、SPP 的状态变化。
6. 说明屏蔽本 hart 的 S 态中断能够保护的范围，并区分同步异常、其他 hart 并发和多核互斥问题。
7. 使用源码、串口探针或 GDB 分别复核初始 SIE 开启、初始 SIE 关闭、新线程初始 Trapframe 和线程入口实时 SIE。
```

## Prompt 7：完整集成与评分验收

```text
[PROMPT]
在虚拟内存、进程创建和调度切换功能已经集成的同一版本上，对 lab4/code 执行完整验收。先验证正常启动，再运行课程原版 make grade，收集真实串口和评分证据，并按首个失败位置定位问题；修复后重跑受影响检查和完整评分。

[RELY]
使用 code/Makefile、tools/inject.py、tools/grade.sh、tools/test_lab4.c、当前集成源码，以及项目已有的 RV64 GCC、QEMU、交叉 GDB 和 Python 3 环境。

[GUARANTEE]
记录验证所用版本、工作树差异范围、环境、命令、退出码和关键输出。分别汇总 PMM、Process Create、Schedule 和 Total Score，并使用 PASS、FAIL 或 NOT VERIFIED 表示真实状态。

[SPECIFICATION]
1. 先执行完整构建，再运行 make qemu 并保存日志，核对 initproc 的 PID、名称、Hello world!! 和 Bye, Bye 输出。
2. 结合 kernel_thread_entry 和 do_exit() 解释 init_main() 返回后的 process exit!! 或现有占位 panic，不把预期退出误判为启动失败。
3. 在隔离副本中串行执行原版 make grade，不修改注入器、断言或评分逻辑；日志保存到构建副本之外，并记录命令真实退出码。
4. 验收目标为 [Test PMM] PASS、[Test Process Create] PASS、[Test Schedule] PASS，三组各 10 分，Total Score: 30/30，同时核对评分要求的 init 输出。
5. 超时时保留已有日志并结束遗留 QEMU/GDB 会话，不因 timeout 状态单独推断功能失败。
6. 阅读测试线程的 schedule() 调用和通过条件，用真实 QEMU/GDB 至少核对一次已运行线程恢复到自己的 sp、返回点和 current。
7. 对官方断言未覆盖的“仅当前可运行、回退 idle、中断状态”等边界，以独立探针或 GDB 补充证据；未实际到达的场景标记 NOT VERIFIED。
8. 失败时保留首个错误位置和最小复现证据：页表/Trap 问题检查内存模块，PCB/初始栈问题检查创建模块，候选选择/上下文恢复问题检查调度切换模块。
9. 所有“通过”结论必须来自同一集成版本的真实运行结果，不能由源码中的 PASS 字符串推断。
```

