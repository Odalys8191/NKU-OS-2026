# Lab 3 成员 C 交接文档

## 0. 文档定位与证据边界

本文件面向成员 C，主责 `exception_handler(struct trapframe *tf)`、集成测试和报告统稿。本文不提供可直接提交的代码。

实际测试已确认：

- 断点由 `tools/test_lab3_context.S` 中的 `ebreak` 触发，并在返回后检查大部分通用寄存器。
- 非法指令由 `.word 0x0000007b` 触发，测试源明确说明它是 4 字节。
- Test 2 还直接构造 cause 20，要求异常兜底打印 Trap frame，评分检查 `cause    0x00000014`。
- 评分要求输出中出现 `Breakpoint at` 与 `Illegal instruction at`。

断点指令长度必须从实际构建产物或汇编设置确认，不能凭经验猜测；若是普通 32 位 `ebreak`，EPC 前进 4 字节。不要为了“通用”而在运行时做可能再次触发异常的取指探测。

## 1. 全项目工作及逻辑顺序

| 阶段 | 工作 | 负责人 | 依赖 | 工作量/难度 |
|---|---|---|---|---|
| 1 | 统一规范、扫描标记、保存证据 | 三人 | 无 | 小 / 中 |
| 2 | 迁移 Lab 2 缺失逻辑并验证 | A | 已完成 Lab 2 | 中 / 中高 |
| 3 | 完成 Trap 入口和上下文恢复 | A | 阶段 2 | 中 / 高 |
| 4 | 完成中断 handler 和持续时钟 | B | A 稳定 | 中 / 中高 |
| 5 | 完成异常 handler 和 EPC 语义 | C | B 提交最新 `trap.c` | 中 / 中高 |
| 6 | 运行三组评分及普通回归 | C 组织，三人复核 | 全部实现 | 中 / 高 |
| 7 | 汇总三人的实验记录和两个思考题 | C 统稿，A/B 提供原稿 | 测试完成 | 中 / 中高 |
| 8 | 最终 marker 扫描、报告复核和提交 | 三人 | 全部完成 | 小 / 中 |

## 2. 三份任务边界

| 成员 | 主责 | 主要交付物 | 评分关联 |
|---|---|---|---|
| A | Lab 2 迁移、`RESTORE_ALL`、`__alltraps`、`stvec` 思考题 | Trap 入口/返回基线 | Test 1 及全部测试基础 |
| B | `interrupt_handler()` | 分类、时钟重装、ticks 和中断兜底 | Test 3 40 分；Test 2 部分 |
| C | `exception_handler()`、宏思考题、集成测试、统稿 | 精确异常返回、最终报告 | Test 1 断点；Test 2 25 分 |

## 3. C 的具体任务

### 3.1 `exception_handler()` 推荐实现逻辑

课程原理对应：异常是由当前指令同步触发的精确事件；保存的 EPC 指向责任指令。若异常已被软件处理并准备继续执行，必须把 EPC 推进到下一条指令，否则会重复陷入。

建议逻辑：

1. 直接按 `tf->cause` 与 `CAUSE_ILLEGAL_INSTRUCTION`、`CAUSE_BREAKPOINT` 比较。`trap_dispatch()` 已经排除了中断，不需要再次处理最高位。
2. 非法指令分支输出包含 `Illegal instruction at` 和当前 EPC 的信息。
3. 非法测试指令在 `tools/test_lab3.c` 中明确为 4 字节，因此该分支按 4 字节推进 EPC。
4. 断点分支输出包含 `Breakpoint at` 和当前 EPC 的信息。
5. 先确认实际 `ebreak` 的机器码长度，再设定断点 EPC 增量。当前测试写的是普通 `ebreak`；推荐通过构建后的 `obj/kernel.asm` 或目标文件反汇编确认。若编码为标准 32 位 `ebreak`，增量为 4；若构建实际生成 16 位 `c.ebreak`，才使用 2。
6. 其他异常只调用 `print_trapframe(tf)`，不修改 EPC。Test 2 的 cause 20 应打印十六进制 `0x00000014`。
7. 函数返回后由 A 的 `RESTORE_ALL` 把更新后的 `tf->epc` 写回 `sepc`，再由 `sret` 继续执行。

### 3.2 精确异常与“更先进原理”的取舍

现代内核强调精确异常：体系结构状态对应一条清晰的指令边界，错误指令之前的效果完成、之后的效果尚未发生。这里应利用测试已知的指令长度保持精确恢复。

RISC-V 通用指令长度可以从低位编码判断，但在 handler 中读取 `tf->epc` 指向的指令可能再次取指失败。本实验已经给出固定测试指令，且要求禁用防御性编程，所以推荐使用已确认的固定长度，不实现通用安全取指器、页错误恢复或递归异常保护。

### 3.3 禁用防御性编程

- 不检查 `tf == NULL`。
- 不对未知异常猜测 EPC 增量。
- 不对 unknown exception 执行 panic、关机或重试。
- 不在运行时试读未知 EPC 来“自动识别一切指令长度”。
- 不修改 `status`、`cause` 或通用寄存器来掩盖问题。
- 不重复实现 A 的返回逻辑或 B 的中断逻辑。

指导书要求的 `print_trapframe()` 兜底属于规定行为。

### 3.4 `do { ... } while (0)` 思考题

推荐表述：该写法把多语句宏包装成一条语法语句，使调用者可以像调用普通函数一样在末尾写分号，并可安全用于没有大括号的 `if/else`；常量条件为假，所以循环体只执行一次；同时宏内部形成独立作用域。它解决的是宏的语法组合问题，不是并发或循环控制机制。

## 4. 全部必做实现点的推荐边界

| 实现点 | 推荐职责 | C 的验收关注点 |
|---|---|---|
| `RESTORE_ALL` | 恢复 CSR/GPR，最后恢复 `sp` | 异常返回后寄存器未被破坏，更新后的 EPC 生效 |
| `__alltraps` | 将 Trap frame 传入 `trap()`，统一返回 | `tf` 地址正确，C handler 修改可传递到 `sret` |
| `interrupt_handler()` | IRQ 分类、时钟重装、计数和规定输出 | C 合并后 Test 3 不回退 |
| `exception_handler()` | 非法/断点输出和精确 EPC 推进，其他异常打印 frame | C 主责 |

## 5. 三人统一规范

1. C 必须从 B 最新提交开始修改 `trap.c`，不粘贴旧版整文件。
2. B/C 各自只改自己的 handler；公共 include 或辅助函数变更需先沟通。
3. 输出字符串至少包含评分 sentinel：`Breakpoint at`、`Illegal instruction at`、`Supervisor software interrupt`、`Unknown interrupt: cause 15`、`100 ticks`。
4. 保留 `lab3` 注释，`YOUR CODE` 按小组约定替换为学号。
5. 不自行定义当前源码不存在的 `SHUTDOWN_AFTER`；在报告中记录指导书与框架差异。
6. Challenge 不列为必做。指导书示例出现 `lab3_switch_test()`，但当前实际源码没有该入口。
7. 高级原理用于解释设计，不扩展代码：不做嵌套中断、向量表、多核 per-hart 状态、调度、用户栈切换或通用异常修复。
8. 每个人必须自行提交 spec、prompt、实现思路、测试和 bug 记录，C 只统一格式和逻辑，不代写事实。

## 6. C 的开始、结束与交接

开始条件：A 的入口/恢复已合并；B 的中断 handler 和 `trap.c` 提交已合并；C 更新到该基点；确认断点指令实际长度。

结束条件：

- 断点输出符合 sentinel，EPC 正确推进，Test 1 返回后寄存器检查通过。
- 非法指令输出符合 sentinel，EPC 按已知 4 字节推进，Test 2 能继续执行。
- cause 20 走 `print_trapframe()` 且不擅自推进 EPC。
- Test 1/2/3 全部回归。
- 报告包含两个思考题、三人的独立记录、指导书/代码差异。
- 最终扫描无未处理的必做 marker。

交回全组：最终测试矩阵、报告草稿、所有差异说明、待确认事项和最终提交基点。

## 7. 集成测试矩阵

| 测试 | 主要负责人 | 必须出现 | 主要验证 |
|---|---|---|---|
| Test 1，35 分 | A+C | `Breakpoint at`、Test 1 PASS | Trap 入口、寄存器恢复、断点 EPC |
| Test 2，25 分 | B+C | `Illegal instruction at`、软件中断、未知中断、cause 0x14、Test 2 PASS | 非法 EPC、两类兜底 |
| Test 3，40 分 | B，C 回归 | `100 ticks`、`ticks=200`、Test 3 PASS | 定时器重装和重复 Trap 返回 |
| 普通 QEMU | 三人 | 初始化完成、持续 tick、无异常风暴 | 非评分环境行为 |

当前 Windows 环境未发现 RISC-V GCC/QEMU 命令，正式测试可能需要 WSL、课程 VM 或已经配置好的 Linux 环境；报告必须注明实际运行环境。

## 8. 报告统稿清单

- [ ] “保存 Trap 上下文 → C 分发 → 恢复 → `sret`”整体逻辑线。
- [ ] A 对 Trap frame、ABI、`stvec` 兼容性的说明。
- [ ] B 对 one-shot timer、重装、ticks 和 IRQ 分类的说明。
- [ ] C 对精确异常、EPC 和 `do { } while (0)` 的说明。
- [ ] 三人各自的 spec、原始 prompt、bug/修复、测试证据。
- [ ] 教材原理与实验映射：中断控制流、上下文切换、原子临界区。
- [ ] 高级但本实验未实现：嵌套中断、多核 per-hart 状态、向量入口、无滴答定时器、通用安全取指。
- [ ] 指导书/代码差异：无 `SHUTDOWN_AFTER`；实际评分要求 ticks 到 200；实际源码无 challenge 入口。
- [ ] 最终提交前保留 `lab3` 注释并替换 `YOUR CODE`。

## 9. 记录模板

- Spec：cause、输出、EPC 前后值、默认分支、不变量、验收测试。
- Prompt：提供的源码/测试上下文、禁止改动、禁止防御性编程、输出格式要求。
- Bug：重复异常、错误增量、寄存器损坏、输出不匹配、合并回退。
- 测试：命令、环境、构建宏、预期 sentinel、实际输出、结论。

