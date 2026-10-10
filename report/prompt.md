# Lab 1：最小可执行内核 Prompt 汇总

本文件汇总本实验中用于辅助代码阅读、构建分析、问题排查和 GDB 调试的全部主要 Prompt。实验结论均以项目源码、实际命令输出和调试记录为依据。

## Prompt 1：内核编译、链接、镜像生成与启动验证

```text
[ROLE]
你是一名熟悉 RISC-V、GNU 交叉编译工具链、QEMU、OpenSBI 和 ucore 的操作系统实验助教。

[CONTEXT]
当前实验为 ucore Lab 1“最小可执行内核”。实验代码包括 Makefile、tools/function.mk、tools/kernel.ld、kern/init/entry.S、kern/init/init.c 及 SBI 控制台输出相关代码。实验环境为 WSL2 中的 Ubuntu，使用 riscv64-unknown-elf 工具链和 qemu-system-riscv64。

[TASK]
1. 阅读实验文档和项目代码，分析内核源代码的编译、链接和镜像生成过程。
2. 说明 Makefile、function.mk 和 kernel.ld 在构建过程中的作用。
3. 说明 bin/kernel 与 bin/ucore.img 的格式、用途及区别。
4. 验证 ELF 文件的目标架构和入口地址是否正确。
5. 分析 QEMU、OpenSBI 和 ucore 内核之间的启动关系。
6. 根据终端输出判断内核是否成功启动。
7. 如果 OpenSBI 显示 Domain0 Next Address 为 0 且内核没有输出，分析问题原因，并给出适用于当前 QEMU 版本的验证方法。
8. 不修改内核功能代码，所有结论均以实验文档、项目代码和实际终端输出为依据。

[OUTPUT]
使用本科操作系统实验报告的书面表达方式输出，内容应包括：编译流程、链接过程、内存布局、镜像生成、启动过程、问题分析、解决方法和最终验证结果。
```

## Prompt 2：理解内核启动中的程序入口操作

```text
[PROMPT]
阅读 kern/init/entry.S 和 kern/init/init.c，完成 Lab 1 练习 1。

重点说明：
1. la sp, bootstacktop 完成了什么操作，目的是什么；
2. tail kern_init 完成了什么操作，目的是什么。

结合实际代码和编译结果进行分析，不修改现有代码。

[RELY]
可以依赖以下已有内容：
- kern/init/entry.S 中的 kern_entry、bootstack、bootstacktop；
- kern/init/init.c 中的 kern_init()；
- bin/kernel 的符号表和反汇编结果；
- 内核入口地址为 0x80200000。

[GUARANTEE]
本练习不需要新增或修改函数。

需要完成：
- 对 la sp, bootstacktop 的作用进行说明；
- 对 tail kern_init 的作用进行说明；
- 结合实际代码说明内核从 kern_entry 进入 kern_init() 的过程。

[SPECIFICATION]
## kern_entry

Pre-Condition：
- OpenSBI 已将控制权交给内核入口；
- 启动栈空间已经预留。

Post-Condition：
- sp 指向 bootstacktop；
- CPU 执行流程进入 kern_init()。

la sp, bootstacktop 用于初始化内核启动栈，为后续 C 语言函数执行提供栈环境。

tail kern_init 用于将执行流程直接转移到 kern_init()。由于 kern_init() 最终进入无限循环，不需要返回 kern_entry。
```

## Prompt 3：使用 GDB 验证启动流程

```text
[PROMPT]
我是 NKU OS Lab 1 小组成员，负责练习 2“使用 GDB 验证启动流程”。请作为操作系统实验助教，辅助我学习 GDB，并理解实验项目中 QEMU、OpenSBI 和内核入口之间的关系。我会自行输入命令、观察输出和保存截图，请结合每一步说明为什么这样操作，以及如何判断 CPU 当前执行到哪里。

[RELY]
课程指导书：lab1.md。
项目代码：lab1/Makefile、tools/kernel.ld、kern/init/entry.S、kern/init/init.c 及控制台输出相关文件。
调试环境：Docker 中的 RISC-V 交叉工具链、QEMU 和 GDB。
分析时结合我提供的终端输出和真实截图。

[GUARANTEE]
- 明确命令应在宿主终端、容器 Shell 还是 GDB 中输入。
- 解释命令的作用，将源码、机器指令和寄存器变化联系起来。
- 以课程资料、当前代码和实际输出为依据；预期结果与实测结果分开说明。
- 遇到异常先分析现象，不虚构运行结果，也不代替我完成截图。
- 本练习采用手动单步和断点观察，不需要自动化测试或样例判分。

[SPECIFICATION]
请按以下顺序指导我操作，并说明每一步需要观察什么：
1. 构建内核，启动等待 GDB 连接的 QEMU。
2. 连接调试目标，查询初始 PC 并反汇编最初几条指令。
3. 在 CPU 仍停于复位位置时，检查内核入口处是否已有代码。
4. 逐条执行复位指令，观察启动参数和跳转目标，确认进入 OpenSBI。
5. 设置内核入口断点，核对 PC、kern_entry 符号和入口反汇编。
6. 单步执行首条内核指令，检查 SP，再观察进入 kern_init() 及启动输出。
7. 最后回答最初几条指令的地址和功能，并根据真实记录整理实验报告。
```

## Prompt 4：实验结果复核与报告整理

```text
[ROLE]
你是一名严谨的操作系统实验报告审阅者，熟悉 RISC-V、QEMU、OpenSBI、GDB 和 ucore Lab 1。

[TASK]
根据实验文档、项目源码、终端输出和调试截图，复核本实验报告中的技术结论，并协助整理“测试与验证”和“实验总结与收获”。

重点检查：
1. 是否区分镜像生成、镜像装载、CPU 到达内核入口和内核功能实际执行；
2. ELF 入口、符号地址、反汇编结果和 GDB 寄存器值是否相互对应；
3. 是否区分 WSL2 与 Docker 两组实验环境，不将不同版本的结果写成同一次运行；
4. 是否将预期结果与实际观察结果分开表述；
5. 对未单独验证的内容，应明确说明验证范围，不得虚构结果；
6. 检查报告是否覆盖实验整体逻辑、核心模块、OS 原理对应知识点、未涉及的重要 OS 知识点以及 AI 协作经验。

[OUTPUT]
使用准确、简洁的本科实验报告语言。所有结论必须能够由源码、命令输出或截图支持；发现证据不足时，应指出缺少的验证，不得将推测写成已验证事实。
```
