# Lab5 普通 fork 收尾内存验证

2026-11-26（Asia/Shanghai）。**本轮通过，原 `make grade` 得分 130/130，退出 0。**

- **Goal** 让普通 fork 通过原有收尾内存检查，交给成员 A 接入 COW。
- **Owner** 课程分工中普通 fork 归成员 C。本轮按用户授权执行本地修复，远端 Owner、Issue 和认领无可核实记录。
- **Branch** `lab5`。验证副本基于 `bbf5636ce877950e2b37ec280fdc0090c6492e59`，并包含普通 fork 实现和 SLOB 整页回收。
- **Acceptance** 修复前复现与核算、分配器专项、四个课程程序真实收尾、普通 fork 契约回归、原评分以及 `git diff --check` 全部通过相应检查。COW 和完整双重 GDB 属于其他验收。

## 根因与修复

两次生产入口复现均到达用户进程全部退出标记，随后在 `kern/process/proc.c:994` 报 `kernel panic`，触发 `nr_free_pages() == free_pages_before`。原评分为 123/130，`make` 退出 2，`grade.sh` 退出 1。

独立探针通过链接器包装真实 `kmalloc/kfree`，分别检查活动对象、SLOB 可用字节和 PMM 页数。每行只比较同一镜像、相同采样位置的前后值。旧探针的 SLOB 字节包含固定的哨兵大小，新探针只统计真实空闲块。

| 内核与程序 | PMM 空闲页 | SLOB 空闲字节 | 活动对象 |
| --- | ---: | ---: | ---: |
| 修复前 forktest | 31553 → 31550 | 7632 → 19920 | 2 → 2 |
| 修复前 forktree | 31553 → 31551 | 7632 → 15824 | 2 → 2 |
| 修复后 forktest | 31554 → 31554 | 3520 → 3520 | 2 → 2 |
| 修复后 forktree | 31554 → 31554 | 3520 → 3520 | 2 → 2 |

修复前 SLOB 空闲区增加的 12288、8192 字节恰好对应 PMM 缺少的三页、两页，两个初始活动对象地址均保留。旧分配器只把释放的小块交回 SLOB，合并后的完整空闲页仍归 SLOB 持有。

生产改动集中在 [`kern/mm/kmalloc.c`](../code/kern/mm/kmalloc.c)。释放路径合并相邻空闲块，再将区间首地址向上、结束地址向下对齐，只交还两者之间完整覆盖的页。小块头部和有效载荷，以及大块描述符所在区间，均须先被释放才能进入空闲区。边界碎片和链表先重连，游标指向有效前驱，随后在同一关中断区间将整页交还 PMM。补页只执行入链与合并，并在同一临界区继续分配。原 `init_main` 采样及断言、课程用户程序、评分文件和普通 fork 生产代码的 SHA-256 在修复前后相同。

测试保留两个原始活动对象，拒绝释放它们，并检查本轮新增活动对象全部消失。分配器专项进一步验证 PMM 重新分到原页，填充该页的所有字节后，SLOB 后续分配仍使用自己的边缘碎片，且存活对象内容保持完整。现有 `kallocated()` 返回常量，本轮回收证据来自真实 PMM、SLOB 链表和活动对象探针。精确回收策略为 C 类工程决策，见课程工作区 `docs/decisions/0010-lab5-slob-page-reclamation.md`。课程要求见 `docs/lab5/lab5.md:70–86` 的模块三。

## 执行与证据

复用 `os-project-lab-1`，Ubuntu 22.04.5 LTS / x86_64，RV64 目标。GCC 10.2.0（SiFive 2020.12.8）、QEMU 4.1.1（os-project-signalfd4-1）、OpenSBI v0.4。修复前后分别在 `.artifacts/lab5-slob-reclaim.uhJzeh/{baseline,fixed}/lab5/code` 运行。以下为容器内执行的命令与外层静态检查。

```sh
python3 tools/test_fork_contract.py all
python3 tools/test_fork_accounting.py
git -C lab5 diff --check
git diff --check
```

契约驱动对每个课程程序执行 `make -j2 build-<程序>`，随后运行 `qemu-system-riscv64 -machine virt -nographic -bios default -kernel bin/ucore.img`。评分调用 `make grade GRADE_TMP=<本轮副本的 .artifacts/lab5-p4/grade-original-tmp> SH=<同目录的 grade-shell.py>`。包装器原样执行 `/bin/sh tools/grade.sh` 并传回其退出码，清理前保存原始日志和最后的镜像。专用内核在测试副本中运行，分配器专项和资源探针在通过标记后经 OpenSBI 关机。

| 检查 | 状态与实际退出码 |
| --- | --- |
| 修复前课程复现 | forktest、forktree FAIL，QEMU 各 0，均有原 PMM 断言 panic |
| 修复前诊断 | 计数探针驱动 0 / 诊断 PASS。原断言仍 FAIL |
| 修复前原评分 | 123/130，`make` 2，`grade.sh` 1，契约总驱动 1 |
| 修复后 SLOB 专项 | 构建、QEMU 各 0 / PASS。覆盖共享页、末次释放、跨页合并与非对齐两端、链表游标、PMM 复用、大块、八轮重复、真实内存耗尽与回滚 |
| 修复后生产入口 | forktest、forktree、exit、waitkill 构建与 QEMU 均 0 / PASS。全部打印 `all user-mode processes have quit.` 和 `init check memory pass.`，无 kernel/user panic。检测到收尾标记后驱动受控停止 |
| 修复后计数探针 | 两个程序构建与 QEMU 均 0，计数驱动 0 / PASS，原内存断言通过 |
| 修复后普通 fork 契约 | 构建、QEMU 0 / PASS。父子返回、独立页表与双向隔离、用户态只读保护、内核线程、CLONE_VM 引用与末次释放、14 个普通失败注入与 1 个共享发布失败均通过 |
| 修复后原评分 | 130/130，`make` 0，`grade.sh` 0，契约总驱动 0 |
| 差异检查 | 内外层 `git diff --check` 0，既存 tracked 补丁 `cmp` 0 |

契约探针直接在第一次复制前采样，逐次检查退出和失败后的资源恢复。初始、建立源 mm 后、测试全部结束时的 PMM 计数分别为 31555、31549、31555，最终活动对象恢复至 3。原 15 个失败用例全部保留。

原评分日志末尾摘录分别为 `Total Score: 123/130` 与 `Total Score: 130/130`。完整日志在课程工作区 `.artifacts/lab5-slob-reclaim.uhJzeh/{baseline,fixed}/lab5/report/logs/`，包括 `p4-grade-original.log`、`p4-grade-script-exit.log`、`p4-results.json` 和 QEMU 串口输出。同一产物目录保存 `baseline-source.sha256`、`fixed-source.sha256` 和 `allocator-only.patch`。用户 Prompt 归档于课程工作区 `prompts/experiments/lab05/10-fix-slob-reclamation.md`。

验证入口为 [`test_fork_contract.py`](../code/tools/test_fork_contract.py) 和 [`test_fork_accounting.py`](../code/tools/test_fork_accounting.py)。构建副本和临时产物集中在课程工作区 `.artifacts/lab5-slob-reclaim.uhJzeh/`，保留所需日志后可整体删除该目录。本次复用现有容器，无宿主工具安装或升级。
