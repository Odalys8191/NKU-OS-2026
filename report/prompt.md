# OS Lab3 Prompt 汇总

本文件汇总 Lab3“中断处理”实验中使用的主要提示词，涵盖 Trap 入口与现场恢复、中断处理、异常接口梳理及异常处理实现。提示词统一按 `[PROMPT]`、`[RELY]`、`[GUARANTEE]`、`[SPECIFICATION]` 四段式整理。所有实现和结论均应以当前工程源码、真实构建产物、QEMU 输出及评分日志为依据。

## Prompt 1：Trap 入口与上下文恢复

```text
[PROMPT]
完成 Lab3 成员 A 负责的前置迁移和 Trap 汇编功能：迁移 Lab2 的 page_init() 与 Best-Fit 主体，在 trapentry.S 中实现 RESTORE_ALL 和 __alltraps。保留 Lab2/Lab3 原有注释，将本人负责位置的 YOUR CODE 替换为真实学号。

[RELY]
先阅读当前工程中真实的 SAVE_ALL、trap.h、riscv.h、内存布局、Lab2 已完成代码及 Lab3 评分源码。以已有接口和宏定义为准，不编造宏，不改变 trap() 的函数签名。

[GUARANTEE]
1. RESTORE_ALL 的栈帧布局必须与 SAVE_ALL 完全一致。
2. 正确恢复通用寄存器、sstatus 和 sepc，sp 必须最后恢复。
3. __alltraps 使用 a0 传递当前 Trapframe 地址，trap() 返回后执行 RESTORE_ALL 和 sret。
4. 保留其他成员的处理函数、公共分发逻辑及框架检查函数，不修改评分断言。

[SPECIFICATION]
1. 实现前列出 Trapframe 的 36 个槽位、字节偏移和状态变化。
2. SAVE_ALL 后执行 mv a0, sp，将当前 Trapframe 指针作为 trap() 的第一个参数。
3. 调用 trap() 会覆盖 ra 和参数寄存器，但这些寄存器的原值必须已经保存在 Trapframe 中。
4. 恢复 CSR 时可使用临时寄存器，但必须先完成临时用途，再恢复该寄存器的原值。
5. 从槽位恢复 sstatus 和 sepc；stval、scause 仅用于诊断，不写回 CSR。
6. 在恢复所有仍需依赖 Trapframe 基址的字段后，最后恢复 x2/sp；不得再额外增加 Trapframe 大小。
7. 结束时执行 sret，根据 sepc 和 sstatus 恢复异常前的执行位置、特权级与中断状态；普通 ret 不能替代 sret。
8. 若迁移代码所需宏缺失，先核实现有接口的等价关系，不能直接照搬旧版本。
9. 执行编译、反汇编、普通启动和教师评分测试，区分编译失败、启动失败及功能失败，并记录真实结果。
```

## Prompt 2：中断分类与时钟中断处理

```text
[PROMPT]
在当前 NKU-OS Lab3 工程中完成中断处理功能。先阅读当前分支的真实代码、实验框架和已有注释，重点检查 code/kern/trap/trap.c、相关头文件、时钟初始化代码及 SBI 接口，然后仅在规定区域实现 interrupt_handler(struct trapframe *tf)。直接修改实际源文件，不只提供示例或建议。

[RELY]
以当前仓库代码为唯一实现依据，核实 struct trapframe、tf->cause、中断原因宏、ticks、TICK_NUM、SHUTDOWN_AFTER、clock_set_next_event()、print_trapframe() 和 libs/sbi.h 中关机接口的真实定义。结合 trap_dispatch() 的分发逻辑，确认 tf->cause 是否仍保留最高位中断标志。使用项目现有 Makefile、QEMU 启动方式和测试脚本。

[GUARANTEE]
1. 保持 interrupt_handler(struct trapframe *tf) 的签名不变。
2. 不修改 SAVE_ALL、RESTORE_ALL、__alltraps、__trapret、struct trapframe、trap()、trap_dispatch() 或 exception_handler()。
3. 正确识别 Supervisor Timer Interrupt，每次只重装一次时钟事件并将 ticks 增加一次。
4. ticks 达到 TICK_NUM 的整数倍时，使用框架已有接口输出统计信息。
5. 按仓库实际定义实现关机阈值和关机逻辑。
6. 对实验要求的其他中断分类输出提示；未知中断打印完整 Trapframe。
7. 不修改评分脚本，不屏蔽错误，不通过伪造输出来使测试通过。

[SPECIFICATION]
1. 先核对宏、变量、函数及输出格式，再编写实现。
2. RISC-V scause 的最高位表示中断，其余位表示原因编号；应按照当前 XLEN 屏蔽最高位，不能盲目使用完整 cause 与 IRQ_* 宏比较。
3. 时钟中断依次完成 clock_set_next_event()、ticks++ 和周期判断，避免漏设下一次事件、重复计数或提前关机。
4. 其他已知中断只完成实验要求的分类与提示，不额外引入复杂设备处理或嵌套中断机制。
5. 未知中断输出原因编号并调用 print_trapframe(tf) 兜底。
6. interrupt_handler() 不修改 tf->epc，现场恢复和返回由统一 Trap 返回路径负责。
7. 修改后检查代码差异，确保仅修改指定区域；使用现有构建和测试流程验证。
8. 失败时依据真实错误定位，只修复本模块范围内的问题。完成后报告修改文件、实现逻辑、测试命令、真实结果及未验证内容。
```

## Prompt 3：异常处理接口与基线梳理

```text
[PROMPT]
为 Lab3 异常处理模块准备实现规格。核对前置成员已经交付的功能、当前 trap.c 基线及异常返回路径，输出简明的接口说明、行为表和验收清单。

[RELY]
参考 Lab3 实验指导书、成员交接记录、code/kern/trap/trap.c、trap.h、trapentry.S、code/libs/riscv.h、tools/test_lab3.c、test_lab3_context.S、grade.sh 和 Makefile。

[GUARANTEE]
交付以下内容：
1. exception_handler() 的输入、输出及允许修改的状态范围。
2. 断点、非法指令和其他异常三类行为表。
3. 当前测试中断点指令长度的真实构建证据。
4. Trap 入口、中断处理等前置交付状态，以及异常模块可使用的验证入口。

[SPECIFICATION]
1. 沿 trap_dispatch → exception_handler → __trapret → sret 解释控制流，说明 tf->epc 的修改如何成为最终返回地址。
2. 核对已经提交的内存初始化、Trap 入口、现场恢复和 interrupt_handler()，以当前文件及交接记录为依据，区分“代码已存在”和“功能已验证”。
3. 使用项目工具链及当前构建参数，在独立构建目录汇编测试源，或检查对应构建产物。
4. 通过反汇编确认测试中的断点指令实际占用 2 字节还是 4 字节，记录所用命令和指令；若无法获得证据，明确标记待验证。
5. 确认非法指令测试使用的 .word 0x0000007b 占用 4 字节。
6. 核对其他异常的现有测试场景，包括 cause=20 的直接调用。
7. 列出各项测试对异常处理和集成回归的要求，区分课程要求、框架事实与项目选择。
```

## Prompt 4：异常分类与精确返回

```text
[PROMPT]
在 code/kern/trap/trap.c 中完成 Lab3 异常处理组件，实现 exception_handler(struct trapframe *tf)，支持断点异常、非法指令异常及其他异常的规定输出，使已处理异常能够返回后续执行流。基于当前最新 trap.c，在 interrupt_handler() 之后、trap_dispatch() 之前添加函数，保留已有中断处理和公共分发逻辑。

[RELY]
使用 Lab3 实验指导书、成员交接记录、前一阶段确认的断点指令长度及构建证据、struct trapframe、CAUSE_BREAKPOINT、CAUSE_ILLEGAL_INSTRUCTION、cprintf()、print_trapframe()、trap_dispatch()、Trap 返回路径、tools/test_lab3.c、test_lab3_context.S 和 grade.sh。

[GUARANTEE]
1. 实现接口 void exception_handler(struct trapframe *tf)。
2. 已处理异常正常返回时，tf->epc 指向异常指令之后；其他异常保持原 EPC。
3. 函数签名和 struct trapframe 布局保持不变。
4. 状态修改仅限断点和非法指令分支中的 tf->epc，其余 Trapframe 字段保持原值。
5. 保留 interrupt_handler()、trap_dispatch() 和 Trap 汇编，不修改无关代码或评分逻辑。

[SPECIFICATION]
Pre-Condition：tf 是框架提供的有效异常上下文；trap_dispatch() 已完成中断与异常分流；断点指令长度已由当前构建产物确认。

Case 1：tf->cause == CAUSE_BREAKPOINT
- 输出包含“Breakpoint at”和异常发生时 EPC 的信息。
- 将 tf->epc 增加当前构建中断点指令的真实长度，使断点测试能够继续执行后续寄存器检查。

Case 2：tf->cause == CAUSE_ILLEGAL_INSTRUCTION
- 输出包含“Illegal instruction at”和异常发生时 EPC 的信息。
- 按测试中已知非法指令的 4 字节长度推进 tf->epc，使执行流继续到下一条指令。

Case 3：其他异常
- 调用现有 print_trapframe(tf) 输出完整现场。
- 保持 tf->epc 不变；cause=20 的测试应由现有打印函数产生评分所需输出。

Requirements：
1. 使用现有 CAUSE_* 宏分类，地址输出采用适合当前指针宽度的格式。
2. 不进行无依据的防御性跳转，不擅自推进未知异常的 EPC。
3. 完成后编译并运行能够验证异常返回的真实 QEMU 测试，记录命令、输出及前置依赖造成的验证缺口。
```

## Prompt 5：Lab3 完整构建与评分验收

```text
[PROMPT]
对当前 Lab3 集成版本执行完整验收，验证 Trap 入口与恢复、中断处理和异常处理能够协同工作。运行普通启动与课程原版评分，保存真实日志，并按首个失败位置区分构建、启动、Trap 入口、中断处理、异常返回或测试判定问题。

[RELY]
使用当前 code/ 源码、Makefile、tools/inject.py、tools/grade.sh、tools/test_lab3.c、test_lab3_context.S、verification-normal.log、verification-grade.log，以及项目配置的 RISC-V 交叉编译器、QEMU 和 GDB。

[GUARANTEE]
记录实际命令、退出码、关键串口输出和评分结果。不得修改官方断言、注入逻辑或输出字符串来迁就测试。未实际运行的项目标记为 NOT VERIFIED，不能根据源码中的 PASS 字符串推断测试通过。

[SPECIFICATION]
1. 清理旧构建产物后完整编译，保留首个编译或链接错误。
2. 反汇编 Trap 入口和测试上下文，核对保存/恢复槽位、CSR 访问、sret 及测试断点的真实长度。
3. 普通启动时确认内核成功进入中断初始化，并持续收到时钟中断；每达到 TICK_NUM 次输出一次统计信息。
4. 运行课程原版 make grade，分别检查 Trap 上下文、时钟中断、断点异常、非法指令异常、其他异常及总体评分。
5. 核对断点和非法指令处理后确实执行到下一条指令，不能只检查打印字符串。
6. 对未知中断或其他异常，确认完整 Trapframe 输出及 EPC 保持行为符合规格。
7. 若出现超时，结合已收到的串口内容判断内核是否在正常等待中断，不能仅凭 timeout 退出码认定失败。
8. 最终按 PASS、FAIL、NOT VERIFIED 汇总，并说明证据对应的源码版本。
```

## 使用说明

- 推荐按“前置代码迁移 → Trap 入口与恢复 → 中断处理 → 异常规格确认 → 异常实现 → 集成评分”的顺序使用。
- 汇编实现前应先固定 Trapframe 布局和寄存器恢复次序，避免恢复过程破坏尚未读取的现场。
- EPC 推进长度必须来自当前测试指令的真实反汇编，不应把所有异常统一按 4 字节处理。
- 报告中应区分源码分析、预期结果和实际运行证据；证据不足时明确写 `NOT VERIFIED`。
