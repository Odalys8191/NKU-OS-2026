# Lab 3 成员 B 交接文档

## 0. 文档定位与证据边界

本文件面向成员 B，主责 `interrupt_handler(struct trapframe *tf)`。本文只描述逻辑，不提供可直接提交的代码。

依据为本地 `lab3.md`、实际 `lab3_new` 框架、`tools/grade.sh`/测试源码，以及课程教材的中断与 I/O 处理原理。实际框架中 `IRQ_S_TIMER` 为 5，`clock_set_next_event()` 使用一次性 SBI 定时事件，`ticks` 是 `volatile size_t`。

重要差异：指导书提到 `SHUTDOWN_AFTER`，但实际源码没有定义；评分测试要求持续到 `ticks=200`。因此不得自行添加一个会令评分提前退出的关机阈值。

## 1. 全项目工作及逻辑顺序

| 阶段 | 工作 | 负责人 | 依赖 | 工作量/难度 |
|---|---|---|---|---|
| 1 | 统一接口、输出、提交和记录规范 | 三人 | 无 | 小 / 中 |
| 2 | 迁移 Lab 2 两个缺失区并验证内存管理 | A | 已完成 Lab 2 | 中 / 中高 |
| 3 | 实现 Trap 恢复与统一入口 | A | 阶段 2 | 中 / 高 |
| 4 | 实现中断分类、持续时钟和兜底 | B | A 的接口稳定 | 中 / 中高 |
| 5 | 实现异常处理和 EPC 更新 | C | B 提交最新 `trap.c` | 中 / 中高 |
| 6 | 运行 Test 1/2/3 与普通 QEMU 回归 | 三人 | 所有代码完成 | 中 / 高 |
| 7 | 各自提供实验记录，C 统稿 | 三人 | 测试完成 | 中 / 中 |
| 8 | 最终 marker 扫描、复核、提交 | 三人 | 全部完成 | 小 / 中 |

## 2. 三份任务边界

| 成员 | 主责 | 主要交付物 | 评分关联 |
|---|---|---|---|
| A | Lab 2 迁移、`RESTORE_ALL`、`__alltraps`、`stvec` 思考题 | 稳定 Trap 入口与返回基线 | 所有测试底座，Test 1 核心 |
| B | `interrupt_handler()` | 中断分类、时钟重装、计数、输出和兜底 | Test 3 40 分，Test 2 部分输出 |
| C | `exception_handler()`、异常测试、集成和报告 | 精确异常返回与最终报告 | Test 1 断点部分，Test 2 25 分 |

## 3. B 的具体任务

### 3.1 `interrupt_handler()` 的推荐逻辑

课程原理对应：设备/定时器发出中断，CPU 保存当前状态、转入处理程序，处理完再返回原执行流。当前 SBI 定时器是 one-shot，而非自动周期定时器，所以每次时钟中断都必须显式安排下一次事件。

建议逻辑顺序：

1. 从 `tf->cause` 去掉最高中断标志位，得到实际 IRQ 编号。使用框架的 XLEN/整型语义，不把完整负值直接与 `IRQ_*` 比较。
2. 按 `libs/riscv.h` 已有 IRQ 宏分类，不写魔法编号。
3. `IRQ_S_TIMER` 分支先调用 `clock_set_next_event()`，再推进 `ticks`。这样当前 one-shot 事件被续上，测试才能到 200 次。
4. 当新 `ticks` 是 `TICK_NUM` 的整数倍时调用现有 `print_ticks()`，精确产生 `100 ticks`。
5. 关机只在框架出现明确阈值时实现；当前版本没有 `SHUTDOWN_AFTER`，不能自造阈值。普通运行若需要演示关机，应先由三人统一并确保不破坏 Test 3。
6. `IRQ_S_SOFT` 必须输出包含 `Supervisor software interrupt` 的消息，评分 Test 2 会直接调用 handler 检查。
7. 其余已知 IRQ 按类型输出简洁固定标签；不做设备确认、清除或调度，因为框架没有提供相应契约。
8. 默认分支输出 `Unknown interrupt: cause N`，其中测试传入的归一化 cause 为 15；再按指导书调用 `print_trapframe(tf)`。

### 3.2 为什么这个顺序合理

- 中断与异常由 `trap_dispatch()` 依据 `scause` 最高位分开，handler 内只需处理 cause 编号。
- 时钟事件是一次性的；若不重装，只会产生一次中断。
- `ticks` 在中断上下文更新。当前单 hart、进入 Trap 后 S 态中断关闭的模型下，不需要再加锁或原子操作。
- 中断不会消费当前指令，因此中断 handler 不应修改 `tf->epc`。
- 先进系统会考虑 per-hart tick、无滴答内核、deadline timer、嵌套中断和中断下半部，但本实验没有这些接口，不能扩张实现。

### 3.3 禁用防御性编程

本函数假设 `tf` 非空且来自合法 Trap 入口：

- 不加空指针检查。
- 不对 cause 做范围钳制。
- 不把未知 cause 强行映射到某个已知 IRQ。
- 不在未知中断中 panic 或关机。
- 不重复开关全局中断，不实现嵌套中断。
- 不为 `ticks` 加溢出重置逻辑；框架未要求。

指导书明确要求的 unknown 输出和 `print_trapframe()` 是功能要求，不视为额外防御。

## 4. 全部必做实现点的推荐边界

| 实现点 | 推荐职责 | 与 B 的关系 |
|---|---|---|
| `RESTORE_ALL` | 严格按 Trap frame 恢复 CSR/GPR，最后恢复原 `sp` | A 完成；若寄存器被破坏，B 的症状可能是假故障 |
| `__alltraps` | 传 `sp` 作为 `tf` 调用 `trap()`，随后统一返回 | A 完成；B 依赖 `tf->cause` 正确 |
| `interrupt_handler()` | IRQ 分类、时钟重装、ticks/输出、规定兜底 | B 主责 |
| `exception_handler()` | 异常输出和已处理异常的 EPC 推进 | C 主责；不应被 B 的提交覆盖 |

## 5. 三人统一规范

1. B 只编辑 `trap.c` 的 Lab 3 占位区，并把中断与异常写成两个彼此独立的函数。
2. B 先提交 `trap.c`；C 必须从 B 的提交继续，不能复制旧文件覆盖。
3. 不对整个 `trap.c` 自动格式化，减少冲突。
4. 输出字符串以评分脚本为准：`Supervisor software interrupt`、`Unknown interrupt: cause 15`、`100 ticks`。
5. `TICK_NUM` 使用现有定义 100；不重复定义。
6. 当前无 `SHUTDOWN_AFTER`；该差异记录进报告，不假定已完成关机要求。
7. 保留 `lab3` 注释，`YOUR CODE` 按三人约定替换学号。
8. 每人分别写 spec、最终 prompt、实现思路、AI bug/解决、测试结果。
9. Challenge 不纳入必做；实际源码也没有 `lab3_switch_test()` 标记。

## 6. B 的开始、结束与交接

开始条件：A 的 Lab 2 迁移和 Trap 入口至少可编译；A 给出 Trap frame/调用约定；B 已基于最新提交工作。

结束条件：

- 所有 `IRQ_*` 分支按框架需求分类。
- S 态软件中断输出满足 Test 2。
- 未知中断 cause 15 输出满足 Test 2。
- 时钟反复重装，`ticks` 可到 200。
- 每 100 次产生一次 `100 ticks`。
- 不修改 `tf->epc`。
- 已提交 spec、prompt、bug 和测试记录。

交给 C：B 的提交哈希、修改范围、cause 归一化方式、所有输出字符串、Test 2 中断兜底结果、Test 3 完整结果、当前无关机阈值的差异说明。

## 7. B 的测试清单

- [ ] 静态确认中断号来自 `libs/riscv.h`。
- [ ] 直接 handler 测试能看到 `Supervisor software interrupt`。
- [ ] cause 15 能看到 `Unknown interrupt: cause 15`。
- [ ] `clock_init()` 后、开中断前 `ticks == 0`。
- [ ] 开中断后 ticks 连续增长，而不是停在 1。
- [ ] 输出至少包含 `100 ticks`。
- [ ] 评分 Test 3 到达 `ticks=200`。
- [ ] C 合并异常 handler 后重跑 Test 3。
- [ ] 普通 `make qemu` 下没有异常风暴或提前退出。

## 8. B 的 spec 建议

- 前置条件：`tf` 是中断 Trap frame；A 的保存/恢复正确；时钟已初始化。
- 后置条件：已知 IRQ 执行规定动作；未知 IRQ 输出并打印 Trap frame；函数返回统一恢复路径。
- 时钟不变量：每次 S timer 中断恰好重装一次、`ticks` 恰好增加一次、EPC 不变。
- 禁止修改：函数签名、`trap_dispatch()`、`clock_set_next_event()`、`TICK_NUM`、A/C 负责的逻辑。

## 9. 记录模板

- Prompt：输入文件、功能约束、输出字符串、禁止防御性编程、禁止改动范围。
- Bug：是否只触发一次、ticks 是否错位、是否提前关机、cause 是否未去最高位、是否改坏 EPC。
- 测试：命令、预期 sentinel、实际输出、次数/时间、结论。

