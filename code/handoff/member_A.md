# Lab 3 成员 A 交接文档

## 0. 文档定位与证据边界

本文件面向成员 A，负责 Lab 2 前置迁移以及 Lab 3 功能模块一。本文只给出设计、实现顺序和检查标准，不提供可直接提交的代码。

当前源码已确认有四个 Lab 3 实现点：`RESTORE_ALL`、`__alltraps`、`interrupt_handler()`、`exception_handler()`；另有两个必须迁移的 Lab 2 标记区：`kern/mm/pmm.c` 和 `kern/mm/best_fit_pmm.c`。源码中未定义指导书提到的 `SHUTDOWN_AFTER`。

## 1. 全项目工作及逻辑顺序

| 阶段 | 工作 | 负责人 | 依赖 | 工作量/难度 |
|---|---|---|---|---|
| 1 | 固化统一规范、保存基线扫描结果 | 三人 | 无 | 小 / 中 |
| 2 | 从现有 Lab 2 成品迁移 `pmm.c` 与 `best_fit_pmm.c` 缺失区，保留 Lab 3 的中断保护 | A | Lab 2 成品 | 中 / 中高 |
| 3 | 完成 `RESTORE_ALL` 与 `__alltraps` | A | 阶段 2 能编译 | 中 / 高 |
| 4 | 完成 `interrupt_handler()` | B | A 的 Trap 入口/返回接口稳定 | 中 / 中高 |
| 5 | 完成 `exception_handler()` | C | B 交出最新 `trap.c` | 中 / 中高 |
| 6 | 分别运行评分三组测试并做整体回归 | A、B、C | 阶段 2-5 | 中 / 高 |
| 7 | 各自提交 spec、prompt、实现思路、bug 与测试记录；C 统稿 | 三人，C 汇总 | 测试完成 | 中 / 中 |
| 8 | 扫描占位、复核报告、提交仓库 | 三人 | 前述全部完成 | 小 / 中 |

主提交顺序为 A → B → C；阅读、spec、prompt 和测试设计可并行。

## 2. 三份任务边界

| 成员 | 主责 | 主要文件 | 评分关联 |
|---|---|---|---|
| A | Lab 2 前置迁移；`RESTORE_ALL`；`__alltraps`；`stvec` 思考题 | `kern/mm/pmm.c`、`kern/mm/best_fit_pmm.c`、`kern/trap/trapentry.S` | Test 1 的上下文恢复基础，且是全部测试的底座 |
| B | `interrupt_handler()`；持续时钟；中断分类 | `kern/trap/trap.c` | Test 3 40 分；Test 2 的中断兜底输出 |
| C | `exception_handler()`；EPC 更新；集成测试与统稿；宏思考题 | `kern/trap/trap.c`、报告 | Test 1 的断点返回；Test 2 25 分 |

A 的汇编风险最高，但 Lab 2 部分已有可迁移成品，不应重新实现，因此总体工作量仍可接受。

## 3. A 的具体任务

### 3.1 Lab 2 前置迁移

实际框架中的两个标记：

1. `kern/mm/pmm.c` 的 `//lab2 请补充你在lab2中的代码`。
2. `kern/mm/best_fit_pmm.c` 的同名标记。

推荐做法：

1. 以已完成的 Lab 2 工作目录为唯一迁移源，不重新让 AI 生成另一套内存管理实现。
2. 对 `pmm.c` 做语义迁移，不整文件覆盖：移入缺失的内存探测、`Page` 数组初始化和可用页登记逻辑；保留 Lab 3 已加入的 `local_intr_save/local_intr_restore` 包装。
3. 对 `best_fit_pmm.c` 迁移已经通过 Lab 2 的管理器主体和管理器描述对象；框架自带的 `basic_check()`、`best_fit_check()` 不应被改写。
4. 不把 Lab 2 的额外挑战、额外测试或与 Lab 3 无关的 allocator 选择机制一并扩张进来，除非小组确认它们就是既有提交的一部分。
5. 先编译和运行物理内存自检，再开始 Trap 汇编；否则后续崩溃难以归因。

“禁用防御性编程”适用于本次新增 Lab 3 逻辑，不代表删除已经验证过的 Lab 2 合法性检查。迁移以行为保真为优先。

### 3.2 `RESTORE_ALL` 推荐实现逻辑

课程原理对应：中断处理前保存 CPU 状态，处理结束后恢复被中断执行流。现代实现上应把 Trap frame 当作严格 ABI，而不是普通临时结构体。

建议顺序：

1. 当前 `sp` 是 Trap frame 基址，36 个寄存器槽位必须与 `SAVE_ALL` 一一对应。
2. 从槽位 32、33 取回 `sstatus` 和 `sepc`，写回对应 CSR。`badvaddr` 和 `cause` 是诊断输入，不需要为返回流程恢复。
3. 恢复除 `x0`、`x2` 外的通用寄存器；`x0` 恒为零，不需要恢复。
4. 用作 CSR 临时值和 Trap frame 基址的寄存器必须在其用途结束后才恢复。
5. 最后恢复原始 `x2/sp`。一旦覆盖 `sp`，就不能再从 Trap frame 取值。
6. 结束时不得遗留额外栈帧，随后由 `sret` 使用恢复后的 `sepc`、`sstatus` 返回。

禁止的做法：

- 不对 `sp` 做额外范围检查或对齐修正；框架契约保证 Trap frame 合法。
- 不重写 `SAVE_ALL`、`struct trapframe` 或函数签名。
- 不增加“若 CSR 异常则替换默认值”之类的容错分支。
- 不恢复 `scause/sbadaddr` 来制造与硬件语义不一致的状态。

关键不变量：`RESTORE_ALL` 结束后，除框架明确允许变化的 EPC 外，测试在 `ebreak` 前写入的寄存器值必须保持不变。`tools/test_lab3_context.S` 会检查 `t0-t6`、`s0-s11`、`a1-a7`。

### 3.3 `__alltraps` 推荐实现逻辑

1. `SAVE_ALL` 返回后，当前 `sp` 正好指向 `struct trapframe`。
2. 按 RISC-V 调用约定，把该地址作为第一个参数交给 `trap(struct trapframe *tf)`。
3. 调用 `trap()`；调用前后的 `sp` 必须相同。
4. `trap()` 返回后直接进入统一的 `__trapret`，由 `RESTORE_ALL` 和 `sret` 完成返回。
5. Trap frame 大小为 36 × 8 = 288 字节，满足 16 字节 ABI 栈对齐；不要另造无必要的对齐垫片。

禁止的做法：不复制 Trap frame，不为 C 调用另建一份上下文，不在入口中修改异常 EPC，也不在这里区分中断和异常。

### 3.4 `stvec` 思考题

建议论证：旧规范要求 Trap 入口四字节对齐，合法地址最低两位必为 `00`；新规范复用原本恒为零的低两位编码模式。旧代码写入的对齐地址自然仍表示 Direct 模式，因此保留旧行为；高位继续保存四字节对齐的基址。这是利用既有对齐冗余扩展编码空间的兼容设计。

## 4. 全部必做实现点的接口认知

| 实现点 | 输入 | 输出/副作用 | 不变量 |
|---|---|---|---|
| `RESTORE_ALL` | `sp` 指向合法 Trap frame | 恢复 CSR、GPR 和原 `sp` | 布局与 `SAVE_ALL` 完全对称 |
| `__alltraps` | 硬件跳入统一入口 | 把 Trap frame 传给 `trap()` | 调用前后 frame 基址不丢失 |
| `interrupt_handler()` | 中断型 `tf->cause` | 分类、时钟重装、计数和规定输出 | 不修改 EPC |
| `exception_handler()` | 异常型 `tf->cause` | 规定输出并推进已处理异常的 EPC | 只对明确处理的异常修改 EPC |

## 5. 三人统一规范（在写代码前确认）

1. 只修改分配给自己的函数或 Lab 2 迁移区，不全文件格式化。
2. 保留 `lab3` 注释，并把 `YOUR CODE` 替换为实际学号；具体学号归属先由三人确认。
3. 精确沿用评分要求字符串：`Breakpoint at`、`Illegal instruction at`、`Supervisor software interrupt`、`Unknown interrupt: cause 15`、`100 ticks`。
4. 不自行定义 `SHUTDOWN_AFTER`。当前评分要求时钟至少运行到 `ticks=200`；若后来教师补发常量，再共同调整。
5. 新增 Lab 3 代码禁用防御性编程：不检查 `tf == NULL`，不钳制 cause，不猜测坏栈，不增加 panic、重试或默认关机；指导书明确要求的 unknown fallback 不属于额外防御。
6. 不把“先进 OS 原理”直接扩展成实验功能：不实现嵌套中断、向量模式、每 hart 定时器、调度器、锁或用户/内核栈切换。
7. 每人保留四类记录：spec、最终 prompt、AI bug/修复、测试命令及完整结果。
8. B 先提交 `interrupt_handler()`，C 基于 B 的提交再改 `exception_handler()`，避免并行覆盖 `trap.c`。

## 6. A 的开始、结束与交接

开始条件：已确认 Lab 2 迁移源；三人完成统一规范确认；保存初始 marker 扫描。

结束条件：

- 两个 Lab 2 标记区已迁移且内存自检通过。
- `RESTORE_ALL` 和 `__alltraps` 可编译。
- 对 36 个槽位完成书面偏移复核。
- Test 1 至少交给 C 做最终断点 EPC 后可运行；A 自己先完成汇编静态检查。
- 已写好 `stvec` 思考题、spec、prompt 和 bug 记录。

交给 B/C：提交基点、Trap frame 偏移表、`sp` 最后恢复的理由、调用前后 `sp` 不变的证据、当前测试结果和未解决问题。

## 7. A 的测试清单

- [ ] 全局扫描两个 Lab 2 标记均已处理。
- [ ] `pmm.c` 仍保留中断保存/恢复包装。
- [ ] 物理内存初始化与 allocator 自检通过。
- [ ] 36 个 Trap frame 槽位与结构体顺序一致。
- [ ] Test 1 能进入 handler 并在 C 完成断点 EPC 后返回。
- [ ] Test 1 返回后所有被检查寄存器保持原值。
- [ ] 后续 Test 2/3 的失败不是由入口或恢复破坏上下文引起。

## 8. 记录模板

- Spec：输入、输出、副作用、不变量、禁止修改项、验收条件。
- Prompt：完整原文、提供给 AI 的上下文文件、约束、期望输出范围。
- Bug：现象、复现步骤、根因、修复、回归结果。
- 测试：命令、环境、预期、实际输出、结论。

