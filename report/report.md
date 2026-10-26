# OS\-lab2

# 实验基本信息

|项目|内容|
|---|---|
|**实验名称**|Lab 2: 物理内存和页表|
|**小组成员**|2412103\-唐健、2412566\-曹子衿、2412266\-杨滢|
|**完成日期**|2026\-10\-18|

## 小组分工

### （1）练习分工

|成员|负责的练习/模块|
|---|---|
|2412103\-唐健|物理内存初始化功能、 First\-Fit|
|2412566\-曹子衿|Best\-Fit 、slub|
|2412266\-杨滢|Buddy System|

### （2）实验报告撰写分工

|成员|负责的练习/模块|
|---|---|
|2412103\-唐健|4\.1、4\.2\.1、5\.1、5\.2\.1|
|2412566\-曹子衿|4\.2\.2、5\.2\.2、5\.4|
|2412266\-杨滢|第一、二、三、六章、4\.2\.3、5\.3|

---

# 一、实验目的

1. 理解页表的建立和使用方法

2. 理解物理内存的管理方法

3. 理解页面分配算法

---

# 二、实验环境

## 2\.1你们使用的 AI 工具

|成员|AI 编程工具|底层模型|备注|
|---|---|---|---|
|2412103\-唐健|OpenAI Codex|GPT\-6\.1 Sol|无|
|2412566\-曹子衿|OpenAI Codex|GPT\-5\.6|无|
|2412266\-杨滢|OpenAI Codex|GPT\-5\.6|无|

**说明：**

- **AI 编程工具**：指具体使用的终端工具、编辑器插件、桌面应用或浏览器界面

- **底层模型**：指该工具使用的大语言模型及版本

---

## 2\.2实验环境

- 宿主系统：Windows \+ WSL2 Ubuntu / macOS

- 目标架构：RISC\-V 64

- 交叉编译器：riscv64\-linux\-gnu\-gcc 15\.2

- 模拟器：QEMU 10\.2\.1

- 固件：OpenSBI 1\.8

- 构建工具：GNU Make

- 命令行环境：Bash（WSL2）与 PowerShell

- 主要测试方式：RISC\-V 交叉编译后在 QEMU `virt` 机器上运行，使用内核断言和 `PASS` 标志判断结果

![image\.png](图片和附件/image%201.png)

# 三、实验整体逻辑分析

## **3\.1 本章节的逻辑主线**

本实验围绕“如何让操作系统安全、正确且可替换地管理物理内存”展开。内核首先必须知道机器上有哪些物理内存以及哪些区域已经被内核占用，然后为每个可管理物理页建立 `Page` 描述符；在此基础上，再通过 `pmm_manager` 对外提供统一的连续页分配与回收接口。上层代码只调用 `alloc_pages`、`free_pages` 和 `nr_free_pages`，不直接依赖某一种具体算法。

整个实验形成了下面这条主线：

```Markdown
硬件信息获取 → 页描述符初始化 → 页级分配算法 → 小对象分配 → 分层测试验证
```

First\-Fit 和 Best\-Fit 使用连续空闲区描述可用内存，Buddy System 则按照二次幂大小建立多级空闲链表。SLUB 继续复用底层页分配接口，把整页划分成对象，从而说明物理页分配器既是内核内存管理的基础模块，也是更高层对象分配器的后端。

不同算法均实现同一个 `struct pmm_manager` 接口，这体现了策略与机制分离：`pmm.c` 负责统一调度和物理内存初始化，具体管理器负责决定空闲页如何组织、选择、拆分和合并。编译期宏负责选择算法，使各实现既能独立测试，也能放回完整内核中进行集成验证。

## **3\.2 功能的逐步实现**

1. **探测物理内存并初始化页描述符。** dtb\_init从设备树读取物理内存基址和大小，page\_init计算内核镜像、页描述符数组及真正空闲区域的边界。这一步先确定“哪些页可以管理”，是后续所有分配算法的前提。

2. **理解并保留 First\-Fit 基线实现。** First\-Fit 展示了 pmm\_manager的基本工作方式：用链表记录连续空闲块，找到第一个足够大的块后切分，释放时合并相邻空闲块。它既是接口使用示例，也是其他算法对比的基准。

3. **实现 Best\-Fit。** 在相同接口上把选择策略改为“寻找能够满足请求的最小空闲块”，同时继续维护切分、地址有序插入和相邻合并，验证统一接口下算法可以被替换。

4. **实现 Buddy System。** 将空闲内存改为按阶组织的多条链表。申请时向上寻找并逐级二分，释放时利用伙伴地址逐级合并，重点解决二次幂对齐、非二次幂请求取整和多段初始内存注册问题。

5. **实现并接入 SLUB。** 在稳定的页级分配接口之上建立对象大小类别与 slab 页，处理小对象复用和大对象退化为多页分配，形成从页级管理到对象级管理的层次结构。

6. **分层验证。** 先用各算法的自检函数验证核心不变量，再用 tests文件夹下的独立模块测试覆盖边界条件，最后在完整内核和官方评分脚本中验证编译、启动及模块集成，减少只在单一场景下正确的风险。

---

# 四、实验内容与实现

## 4\.1 功能模块一：物理内存初始化功能

#### 模块功能描述

**需要实现/修改的函数：**

```Plain Text
static void init_pmm_manager(void);
static void page_init(void);
static void init_memmap(struct Page *base, size_t n);
void pmm_init(void);
```

其中，`page_init()` 是本模块的核心实现任务；`init_pmm_manager()` 负责选择并初始化物理内存管理器，`init_memmap()` 和 `pmm_init()` 使用已有接口完成初始化流程。

**功能说明：**

物理内存初始化是内核建立内存管理能力的第一步。系统启动时，虽然已经能够访问物理内存，但还没有建立用于记录页面状态的管理结构，因此无法安全地进行动态页面分配。

本模块利用设备树提供的物理内存信息，确定 RAM 的起始地址与容量，为可管理的物理页建立 `Page` 描述符数组，并排除 OpenSBI、内核镜像及描述符数组占用的空间。剩余的完整物理页通过 `init_memmap()` 交给当前物理内存管理器。

整体初始化流程如下：

```Plain Text
pmm_init()
    ├── init_pmm_manager()
    │       └── pmm_manager->init()
    ├── page_init()
    │       ├── get_memory_base()
    │       ├── get_memory_size()
    │       ├── 初始化 Page 描述符
    │       └── init_memmap()
    └── check_alloc_page()
```

其中，设备树负责提供物理内存布局，`page_init()` 负责确定可以分配的页面集合，具体的页面组织和分配策略由 `pmm_manager` 实现。这种划分使物理内存初始化能够与 First\-Fit、Best\-Fit 等分配算法配合使用。

---

### 4\.1\.1 任务一：实现物理内存探测与页描述符初始化功能

**负责人：** 2412103\-唐健

#### 最终提示词

以下是用于完成 `page_init()` 的最终提示词。提示词明确了真实文件修改范围、已有工程接口、地址转换关系及各类内存边界条件。

```Plain Text
[PROMPT]

**任务**：完成 NKU OS Lab2 的物理内存初始化功能，修改 code/kern/mm/pmm.c，实现 page_init()，使内核能够根据设备树提供的物理内存信息建立 Page 描述符数组，并将可用物理页交给当前物理内存管理器。

**操作要求**：必须直接修改项目中的真实文件，保留现有代码，只补全 page_init() 对应的实验代码。先阅读相关头文件和现有实现，确认地址转换、内存布局及初始化流程，不得凭空定义工程中不存在的接口。

**输出要求**：以实际工程代码为准，按照以下规格完成实现。修改范围仅限 code/kern/mm/pmm.c，不修改其他组员负责的 Best-Fit、Buddy、SLUB 模块，不改动已有测试函数，不执行 Git 提交。完成后简要说明修改内容、关键地址计算及验证结果。

[RELY]

相关工程文件：
- kern/mm/pmm.c：物理内存初始化流程及全局变量
- kern/mm/pmm.h：Page 结构、地址转换和管理器接口
- kern/mm/memlayout.h：物理内存布局相关常量
- kern/driver/dtb.h：设备树内存信息接口
- kern/mm/default_pmm.c：已有物理页管理器实现
- kern/init/init.c、tools/kernel.ld：启动顺序与内核结束符号

现有接口与变量：
- get_memory_base()、get_memory_size()：获取设备树中的物理内存范围
- init_memmap(base, n)：将连续空闲物理页交给当前管理器
- pa2page(pa)：物理地址转换为 Page 描述符
- panic()、cprintf()：错误处理与信息输出
- pages、npage、nbase、va_pa_offset、end
- struct Page 及其页面状态操作接口
- PGSIZE、DRAM_BASE、PHYSICAL_MEMORY_END 等内存布局定义

关键地址关系：
- VA = PA + va_pa_offset
- PFN = PA / PGSIZE
- nbase = DRAM_BASE / PGSIZE
- npage 表示可管理物理页号的上界，不是物理页数量
- pages[i] 描述物理页号 nbase + i
- Page 描述符数组数量为 npage - nbase
- end 是内核结束位置的虚拟地址符号
- 内核、描述符数组及对齐空隙占用的页面不能分配

以上名称和关系须与当前源码核对；具体宏、类型及状态操作以工程实际定义为准。

[GUARANTEE]

必须实现的接口：

static void page_init(void);

该函数由 pmm_init() 调用，完成物理内存探测、Page 描述符初始化和初始空闲区注册。

允许在必要时增加本文件内部的 static 辅助函数，但不修改公共接口及其他模块。

[SPECIFICATION]

## page_init(void)

**Pre-Condition**:
- 设备树初始化已经完成，可以获取物理内存信息。
- 当前 pmm_manager 已经完成初始化。
- 内核能够访问其已建立映射的物理内存区域。

**Post-Condition**:
- 正确设置 va_pa_offset、npage 和 pages。
- 每个可管理物理页都有对应的 Page 描述符。
- 尚未登记为空闲的页面均处于保留状态。
- OpenSBI、内核镜像及描述符数组占用的页面不会进入空闲区。
- 所有可用的完整物理页均正确注册到当前物理内存管理器。
- 输出实际探测到的物理内存范围。

**Case 1：正常初始化**
- 获取设备树提供的物理内存起点和大小，结合框架支持的内存上界，确定实际可管理范围。
- 设置虚拟地址与物理地址之间的转换偏移。
- 在内核结束地址之后按页对齐放置 Page 描述符数组。
- 根据可管理物理页数计算描述符数组所需空间，初始化所有描述符。
- 将描述符数组末尾向上按页对齐，将可管理内存末尾向下按页对齐，得到可分配的完整物理页区间。
- 将该区间交给 init_memmap() 管理。

**Case 2：设备树信息无效**
- 当内存信息为空、范围溢出或与当前框架支持的内存布局不兼容时，终止初始化并报告错误。
- 不得使用猜测的默认内存范围继续执行。

**Case 3：没有可用物理页**
- 若有效内存不足以容纳内核和 Page 描述符数组，应终止初始化，避免越界写入。
- 若描述符能够安全建立，但剩余空间不足一个完整物理页，则保持空闲区为空，不调用 init_memmap()。

**Requirements**:
- 严格区分虚拟地址、物理地址、物理页号和 Page 描述符指针。
- 正确处理页面对齐、地址边界和描述符数组大小。
- 不得在 va_pa_offset 初始化前使用依赖它的地址转换。
- 不得将未初始化的 Page flags 直接视为有效状态。
- 不修改现有分配器算法和测试逻辑。
- 完成后检查相关初始化不变量，尝试编译验证；如因其他组员尚未完成的模块导致失败，应明确说明，不得擅自修改其代码。
```

#### 实现迭代过程

本任务采用一份完整的功能实现提示词完成核心代码，之后通过编译、模块集成和 GDB 检查进行验证。以下按照实际问题出现的顺序记录，编译环境与团队模块问题不计作 `page_init()` 的算法迭代。

##### 第一阶段：实现物理内存初始化

**实现思路：**

首先分析设备树内存信息、内核虚拟地址与物理地址的关系，随后明确 `npage`、`nbase` 和 `pages` 的含义，最后实现页面描述符初始化及空闲区注册。

实现过程中需要特别注意两个边界：

1. `npage` 表示物理页号上界，描述符数组长度应为 `npage - nbase`。

2. 描述符数组本身占用物理内存，必须在完成其空间计算和页面对齐后，才能确定第一个可分配物理页。

**遇到的问题：**

- **问题一：地址类型容易混淆。** `end` 和 `pages` 使用内核虚拟地址，而设备树返回物理地址。如果直接混用，容易造成描述符数组位置和可分配区域边界计算错误。

- **问题二：初期工程尚未完成全部模块。** `page_init()` 已实现时，其他组员负责的 Best\-Fit 模块尚未全部接入，因此暂时无法依靠完整构建验证整个系统。

**问题解决策略：**

针对地址问题，在提示词的 `[RELY]` 中明确 VA、PA、PFN 和 `Page` 描述符的对应关系，并在 `[SPECIFICATION]` 中要求先设置 `va_pa_offset`，再执行依赖该偏移量的地址转换。

针对模块依赖问题，将 `page_init()` 的实现与其他分配器模块分开验证，不修改组员负责的文件，等待 Best\-Fit 模块完成后再开展整体构建。

##### 第二阶段：编译与集成验证

其他组员完成相关模块后，进行了完整编译和 QEMU 启动测试。

**遇到的问题：**

当前 RISC\-V 工具链对原 Makefile 中的指令集参数：

```Plain Text
-march=rv64imac_zicsr_zifencei
```

出现汇编兼容性错误，导致构建无法正常完成。

**解决方法：**

确认使用的 SiFive GCC 工具链版本后，将该参数调整为：

```Plain Text
-march=rv64imac
```

随后重新编译，完整构建成功。该问题属于工具链与构建参数的兼容性问题，未改变 `page_init()` 的内存布局算法。

#### 最终结果

在 QEMU 的 128 MB RAM 配置下，内核成功读取设备树中的物理内存范围：

```Plain Text
Base: 0x0000000080000000
Size: 0x0000000008000000 (128 MB)
End:  0x0000000087ffffff
```

随后完成物理页初始化，并通过分配器基础检查：

```Plain Text
Managed physical memory:
[0x0000000080000000, 0x0000000088000000)

check_alloc_page() succeeded!
```

进一步使用 GDB 检查得到：

```Plain Text
npage        = 0x88000
pages        = 0xffffffffc0208000
va_pa_offset = 0xffffffff40000000
```

上述结果符合预期的内存地址关系。正常配置下，物理内存初始化功能已能够支持后续页面分配。

#### 关键改进点总结

1. **明确地址域与页号关系。** 通过 `npage`、`nbase`、`pages` 的定义，避免将页号上界误认为物理页数量。

2. **明确描述符自身的内存开销。** 在计算空闲区之前先为全部 `Page` 描述符预留空间，保证分配器不会覆盖内核数据。

3. **明确模块边界。** 将物理内存初始化与具体分配算法解耦，使同一初始化流程能够用于 First\-Fit 和 Best\-Fit。

4. **结合运行时状态验证。** 除了观察 `check_alloc_page()`，还通过 GDB 检查了关键全局变量和虚拟地址映射关系。

---

### 4\.1\.2 Challenge：硬件的可用物理内存范围的获取方法

**负责人：** 2412103\-唐健

#### 最终提示词

以下为根据本次思考题讨论整理的结构化提示词版本，用于归纳分析任务和验证过程。

```Plain Text
[PROMPT]

分析 NKU OS Lab2 的硬件物理内存探测 Challenge：如果操作系统无法提前知道当前硬件的可用物理内存范围，应如何获取这些信息？

结合当前 RISC-V QEMU virt 平台和实验中的 DTB 解析过程，给出能够直接写入实验报告的解释，并使用实际设备树数据验证。

[RELY]

当前实验运行在 QEMU RISC-V virt 平台。
内核通过 dtb_init() 解析设备树，并使用 get_memory_base()、get_memory_size() 获取 RAM 起始地址与大小。
物理内存初始化由 page_init() 完成。

[GUARANTEE]

给出硬件物理内存发现方法的分析。
解释设备树如何描述 RAM。
区分硬件存在的 RAM 与实际可分配的物理页。
提供设备树验证方法，不修改内核源码。

[SPECIFICATION]

Pre-Condition:
- 已有能够正常启动的 QEMU 实验环境。
- 可以读取当前平台的设备树。

Post-Condition:
- 解释固件、引导程序与操作系统之间的内存信息传递。
- 说明 BIOS/UEFI 内存映射和设备树等常见方法。
- 结合本实验给出实际内存起点与容量。
- 说明内核、固件和其他保留区域为何需要排除。

Requirements:
- 不将全部 RAM 直接视为可分配内存。
- 使用真实设备树数据支撑实验分析。
- 不执行可能破坏内存内容的盲目读写探测。
```

#### 实现迭代过程

本任务是原理分析和实验验证题，无须新增内核函数。完成过程分为以下两个阶段。

##### 第一阶段：分析物理内存发现方法

当操作系统无法预先知道物理内存布局时，可以由固件或引导程序提供内存描述信息。

传统 x86 平台可以通过 BIOS E820 接口获取物理内存映射；UEFI 平台可以使用 `GetMemoryMap()` 获取不同类型内存区域的地址及大小。在 RISC\-V 和部分 ARM 平台上，设备树则是常见的硬件信息描述方式。

本实验采用 QEMU virt 平台。QEMU 构造虚拟硬件的设备树，启动固件将设备树地址传递给内核。内核解析 `/memory` 节点，从中获得 RAM 的起始地址和长度。

获得物理内存范围后，还需要排除已占用区域，包括启动固件、内核代码和数据、页表以及 `Page` 描述符数组。剩余内存经过页面对齐和初始化后，才能交给物理页分配器。

##### 第二阶段：使用设备树验证实际内存布局

为了验证设备树的内容，使用 QEMU 的 `dumpdtb` 参数导出设备树，并通过 `dtc` 转换为可读的 DTS 文本：

```Plain Text
qemu-system-riscv64 \
  -machine virt,dumpdtb=/tmp/lab2.dtb \
  -m 128M \
  -nographic

dtc -I dtb -O dts /tmp/lab2.dtb -o /tmp/lab2.dts

grep -A8 -B2 'memory@80000000' /tmp/lab2.dts
```

![dtb\_memory\.png](图片和附件/dtb_memory.png)



实际得到的内存节点为：

```Plain Text
memory@80000000 {
    device_type = "memory";
    reg = <0x00 0x80000000 0x00 0x8000000>;
};
```

其中，`reg` 使用两个 32 位单元表示地址、两个 32 位单元表示长度，对应：

- 物理内存起始地址：`0x80000000`

- 物理内存大小：`0x08000000`，即 128 MiB

- 物理内存范围：`[0x80000000, 0x88000000)`

这与内核启动时 `dtb_init()` 输出的物理内存信息一致，验证了设备树中的 RAM 描述能够被内核正确获取。



**最终结论：**

操作系统可以通过固件提供的内存映射或设备树获取硬件内存范围，再结合内核和固件的保留区域确定可分配空间。本实验使用设备树完成 RAM 发现，并由 `page_init()` 将剩余完整物理页交给内存管理器。

对于缺少可靠固件描述的特殊早期启动环境，也可以考虑有限范围的硬件探测，但必须避开 MMIO、保留区域及正在使用的内存，避免破坏系统状态。

---

## 4\.2 功能模块二：物理内存分配功能

#### 模块功能描述

**需要实现/修改的函数：**

```Plain Text
static void default_init(void);
static void default_init_memmap(struct Page *base, size_t n);
static struct Page *default_alloc_pages(size_t n);
static void default_free_pages(struct Page *base, size_t n);
static size_t default_nr_free_pages(void);

struct Page *alloc_pages(size_t n);
void free_pages(struct Page *base, size_t n);
size_t nr_free_pages(void);
```

以上为 First\-Fit 管理器及现有公共分配接口。Best\-Fit、Buddy System 和 SLUB 的新增函数在各自任务小节中说明。

**功能说明：**

物理内存初始化完成后，系统已经知道哪些物理页可以使用，但还需要根据实际申请大小选择合适的空闲区域，并支持后续释放与合并。

本模块通过统一的 `pmm_manager` 接口提供页面分配、释放和空闲页数量统计。不同分配算法通过各自的数据结构组织空闲页，公共调用方不需要了解管理器内部的具体实现。

First\-Fit 按空闲链表顺序寻找第一个足够大的内存块；Best\-Fit 在满足请求的块中选择更接近申请大小的块；Buddy System 将空闲内存组织为不同阶数的 2 的幂大小块；SLUB 则在页分配基础上进一步管理小型内存对象。

---

### 4\.2\.1 任务一：理解 First\-Fit 连续物理内存分配算法

**负责人：** 2412103\-唐健

#### 最终提示词

以下为根据本次 First\-Fit 源码分析与测试目标整理的提示词版本，用于说明本任务的分析方法。

```Plain Text
[PROMPT]

阅读 NKU OS Lab2 中现有的 First-Fit 物理内存分配器代码，分析其初始化、页面分配、页面释放和空闲块合并过程，并讨论算法的改进空间。

保持 kern/mm/default_pmm.c 的原有实现和测试代码不变。说明核心函数的作用及数据结构之间的关系，结合已有自检结果完成实验分析。

[RELY]

kern/mm/default_pmm.c 提供 First-Fit 的完整实现。
kern/mm/pmm.h 定义公共物理内存管理接口。
kern/mm/memlayout.h 定义 Page 描述符。
libs/list.h 提供双向链表操作。

管理器通过 free_list 维护空闲块，nr_free 记录空闲页总数。
Page 的 property 字段记录空闲块大小，PG_property 标记空闲块头页。

[GUARANTEE]

分析以下已有函数：

static void default_init(void);
static void default_init_memmap(struct Page *base, size_t n);
static struct Page *default_alloc_pages(size_t n);
static void default_free_pages(struct Page *base, size_t n);

说明 First-Fit 的分配策略、分裂逻辑和释放合并过程，并回答是否存在进一步改进空间。

[SPECIFICATION]

Pre-Condition:
- 已完成物理内存初始化。
- 已有完整的 First-Fit 代码。

Post-Condition:
- 能解释各核心函数的作用及调用关系。
- 能说明申请 n 个连续物理页时如何查找和拆分空闲块。
- 能说明释放页面后如何合并相邻空闲块。
- 能分析顺序搜索和外部碎片问题。
- 给出真实运行验证结果。

Requirements:
- 以现有源码为依据，不重新实现 First-Fit。
- 不修改测试逻辑。
- 将源码分析与运行验证区分开。
```

#### 实现迭代过程

First\-Fit 是本实验提供的完整参考算法，因此本任务重点是理解已有实现，并验证其基本分配行为。

##### 第一阶段：阅读核心函数

**（1）****`default_init()`****：初始化管理器**

初始化空闲链表头 `free_list`，并将空闲页总数 `nr_free` 设为零，为后续登记可用物理页做准备。

**（2）****`default_init_memmap()`****：登记初始空闲页**

该函数接收一段连续物理页，清除相应页面的保留状态，将首个 `Page` 描述符标记为空闲块头，并通过 `property` 记录块内的页数。

随后把该空闲块加入链表，并更新空闲页总数。内存初始化模块通过 `init_memmap()` 调用该函数，从而完成可用页面向分配器的移交。

**（3）****`default_alloc_pages()`****：分配连续物理页**

当内核请求 `n` 个连续物理页时，First\-Fit 从链表头开始依次检查空闲块，找到第一个大小不小于 `n` 的内存块。

如果块大小恰好等于请求大小，就将其从空闲链表中移除；如果块更大，则将其拆分，把剩余部分重新登记为空闲块。

分配完成后更新空闲页总数，并返回分配区域的首个 `Page` 描述符。

**（4）****`default_free_pages()`****：释放并合并页面**

释放内存时，函数恢复这段页面的空闲状态，将其重新纳入空闲块管理，并检查物理地址相邻的空闲块。

如果被释放区域与已有空闲块相邻，就将它们合并为更大的连续区域，减少后续大块分配失败的可能性。

通过这四个函数，First\-Fit 完成了从初始空闲内存登记，到动态分配、释放和空间回收的完整过程。

##### 第二阶段：运行测试与问题分析

为了单独验证 First\-Fit，将 `init_pmm_manager()` 中的管理器暂时切换为 `default_pmm_manager`，并执行 QEMU 启动测试。

运行日志显示：

```Plain Text
memory management: default_pmm_manager

DTB physical memory:
[0x0000000080000000, 0x0000000088000000)

Managed physical memory:
[0x0000000080000000, 0x0000000088000000)

check_alloc_page() succeeded!
```

结果表明，First\-Fit 的基础页面分配检查通过。

集成过程中还遇到了一个需要区分的问题：当前内核初始化流程额外执行了 Best\-Fit 专用的扩展检查。当管理器临时切换成 First\-Fit 时，该扩展检查会因为管理器类型不符而触发断言。这与 First\-Fit 自身的 `check_alloc_page()` 通过并不矛盾。

完成测试后恢复 Best\-Fit 配置，避免影响小组的正常集成版本。

#### First\-Fit 算法是否还有改进空间？

First\-Fit 实现简单、额外管理开销较小，但仍然存在进一步优化空间。

首先，算法从链表头顺序查找空闲块。随着空闲块数量增加，查找过程可能需要遍历较长的链表，其时间复杂度在最坏情况下为 O\(k\)，其中 k 是空闲块数量。

其次，反复分配与释放可能产生大量较小的空闲块。虽然算法会合并相邻空闲区域，但不相邻的空闲块无法组成更大的连续空间，因此仍可能出现空闲总页数足够、却无法满足大块连续申请的情况。

可以考虑采用以下改进：

1. 使用大小分类的空闲链表或树形索引，加快合适空闲块的查找。

2. 根据请求大小采用 Best\-Fit 等不同分配策略，提高对特定内存使用场景的适应能力。

3. 使用 Buddy System 等分级组织方式，提高固定阶数内存块的分裂与合并效率。

这些改进需要在查找效率、内存碎片和额外管理成本之间取得平衡。

**最终结果：**

通过阅读 First\-Fit 的完整实现，明确了空闲块链表的组织方式，以及页面分配、拆分、释放与合并的具体流程。独立运行验证中，First\-Fit 的基础分配器自检成功，满足本任务对原理理解和基本行为验证的要求。

---



### 4\.2\.2任务二：实现 Best\-Fit 连续物理内存分配算法

**负责人：** 2412566\-曹子衿

#### 最终提示词

本任务实际分为实现 Prompt（P1）和独立补充测试 Prompt（P2）。以下保留当时交给 AI 的原文；原文中的 lab2/kern 路径是开发副本，同步到小组提交仓库后对应 code/kern，没有改变提示词语义。

**P1：完整 Best\-Fit 模块**



```Plain Text
[PROMPT]
在 lab2/kern/mm/best_fit_pmm.c 实现完整 Best-Fit 连续物理页分配模块，必要声明放在同名头文件。直接修改文件，交付关键不变量、验证结果和 best_fit_pmm_manager 的接入说明。

[RELY]
lab2.md 的 Best-Fit 任务；成员二交接第 4–5 节；ADR 0004；
现有 pmm_manager、Page、free_area_t、状态宏、list.h 和 default_pmm.c。
原测试直接使用 free_area、free_list、nr_free，并会构造“空链表、非零计数”的临时状态；状态组织须与这些测试兼容。

[GUARANTEE]
实现以下函数与注册对象：
- static void best_fit_init(void)
- static void best_fit_init_memmap(struct Page *base, size_t n)
- static struct Page *best_fit_alloc_pages(size_t n)
- static void best_fit_free_pages(struct Page *base, size_t n)
- static size_t best_fit_nr_free_pages(void)
- const struct pmm_manager best_fit_pmm_manager
管理器的 check 绑定原 best_fit_check，内部辅助函数自行组织。

[SPECIFICATION]
稳定状态：使用单一 free_area；空闲块按地址递增、互不重叠、相邻块已合并。仅空闲块头页设置 PageProperty，property 表示块长；nr_free 表示总空闲页数。

best_fit_init：
Pre-Condition：启动初始化。
Post-Condition：空闲链表为空，nr_free 为零。

best_fit_init_memmap：
Pre-Condition：base 指向 n > 0 个连续、reserved、尚未登记的合法描述符。
Post-Condition：这些页的引用数清零，成为可分配区间；nr_free 增加 n，保持地址顺序和相邻合并关系。

best_fit_alloc_pages：
Pre-Condition：n > 0；n = 0 按课程示例断言。
Case 1：有候选时，选择长度至少 n 的最小块，同长取低地址，从块头分配恰好 n 页，nr_free 减少 n。
Case 2：候选更大时，余块具有正确的头地址、长度和标志，并保持链表有序；精确匹配时整块分配。
Case 3：页数不足、连续块不足或原测试的临时空链表，返回 NULL，原状态不变。

best_fit_free_pages：
Pre-Condition：n > 0，区间全部是调用者拥有的已分配页；支持原测试中的合法子区间释放和连续区间组合释放。
Post-Condition：引用数和旧头元数据复位，nr_free 增加 n；四种邻接情况均得到最大连续空闲块，被吸收的旧头清除空闲头标志。

best_fit_nr_free_pages：
Post-Condition：以 O(1) 返回维护的空闲页数。

Requirements：选择和插入可用 O(k)；分裂、合并保持页数守恒。运行原检查函数，并说明结果。
```



**P2：Best\-Fit 补充测试**

```Plain Text
[PROMPT]
为 Best-Fit 增加独立补充测试，代码放在 best_fit_pmm.c，入口声明放在 best_fit_pmm.h。交付“场景—预期—能发现的错误”表及运行结果。

[RELY]
P1 的行为规格、当前 Best-Fit 模块、成员二交接第 8.1 节；
公共 alloc_pages/free_pages/nr_free_pages 和现有链表结构。
测试在 Best-Fit 初始化、原检查完成后运行，入口由成员三接入。

[GUARANTEE]
新增 void best_fit_extra_check(void) 和必要测试 helper。
原管理器 check 继续绑定 best_fit_check；发现算法缺陷时给出最小复现，进入 F 修复。

[SPECIFICATION]
Pre-Condition：测试独占分配活动，物理页足以构造场景；通过真实分配持有隔离页，控制候选和相邻区间。
Post-Condition：全部测试页归还，空闲总数恢复，链表有序且充分合并。
Case 1：精确匹配优先；6/3/5 页候选申请 2 页选择 3 页块；同长取低地址。
Case 2：5 页块申请 2 页，验证余下 3 页的地址、长度、标志和计数。
Case 3：覆盖无相邻、仅前邻、仅后邻、双向合并，核对最终区间与旧头标志。
Case 4：只有不相邻的两个 2 页块可用时，申请 3 页失败且状态不变；另测普通耗尽。
Requirements：预期由独立区间和计数计算，直接观察真实分配结果；测试通过公共接口构造状态，覆盖原检查与新增检查。
```



#### 实现迭代过程

本模块的实现经历了 4 次迭代，过程如下：

**第一阶段：从 First\-Fit 提炼契约。** 我先按实验指导文档阅读 default\_init、default\_init\_memmap、default\_alloc\_pages、default\_free\_pages 和公共 pmm\_manager 借口，确认空闲链表只链接块头，nr\_free 记录空闲物理页总数。课程要求实现完整 Best\-Fit 并保留原 basic\_check、best\_fit\_check；项目设计另外约定地址有序链表、同长选择低地址。这些约定写进 P1 的输入状态和后置条件，避免只改查找循环而遗漏登记、分裂、释放和注册。



**第二阶段：实现并检查状态不变量。** best\_fit\_init 建立空链表；best\_fit\_init\_memmap 清理页描述符后登记连续块；best\_fit\_alloc\_pages 遍历所有候选，选择页数足够且长度最小的块，长度相同保留先遇到的低地址块。分配取块的前 n 页，余块以新头留在原链表位置；best\_fit\_free\_pages 按地址插入并先合并前邻、再用更新后的块头合并后邻。只有空闲块头带 PG\_property，property 表示块长度，nr\_free 等于所有空闲块长度之和。正常失败返回 NULL，链表和计数不变。页面计数是可用总量，不等于最大连续块长度。



**第三阶段：让测试具有独立预期。** 原检查覆盖接口基本行为，但不能单独证明“选择最小足够块”。P2 新增 best\_fit\_extra\_check：先真实分配 24 页测试区，并暂时持有其余空闲页，把候选限制在测试区内；随后构造长度 6、3、5 的三个空闲块，申请 2 页应取 3 页块。同样检查精确匹配、同长低地址、5 页拆成 2＋3 页、无邻/前邻/后邻/双向合并、碎片导致的失败及失败前后状态相同。每轮结束归还所持页面，核对原始空闲页数。



**第四阶段：团队接入和审查。** 成员一的 page\_init 建立可用页后，团队在编译期选中 best\_fit\_pmm\_manager；原管理器 check 仍绑定 best\_fit\_check，团队在 pmm\_init 返回后才运行补充检查。P5 在同步代码上实测独立测试、正式内核和课程评分。最初的 \-march 参数与既有 GCC 10\.2\.0 不兼容，改为该工具链实际支持的 rv64imac 后完成运行；这属于构建适配，不是 Best\-Fit 算法返工。P6/F 随后修复了测试脚本对错误断言、异常退出和旧评分输出的误判，保留分配算法和课程原检查。



#### **实现分析与改进空间**



Best\-Fit 的搜索在 k 个空闲块上最坏为 O\(k\)；当前头部取页后的分裂为 O\(1\)，按地址插入与邻接合并需 O\(k\)，释放 n 页的元数据重置另需 O\(n\)。nr\_free\_pages 直接读计数，为 O\(1\)。与 First\-Fit 相比，它倾向于保留较大块，却不能保证消除外部碎片；例如两块各 2 页空闲时申请连续 3 页仍失败。未来可按长度建立索引降低搜索成本，但必须同步维护地址顺序、合并和课程测试使用的 free\_list/nr\_free 状态，额外复杂度与内存开销并不适合本次规模。

---



### 4\.2\.3 任务三：Buddy System 分配算法

**负责人：** 2412266\-杨滢

#### 伙伴系统原理与核心数据结构设计

Buddy System（伙伴系统）是一种以二次幂大小组织和管理连续物理页的内存分配算法。系统按照阶数维护多条空闲链表，其中第 $k$ 阶链表保存大小为 $2^k$ 页、起始 PFN 按 $2^k$ 对齐的空闲块。每个空闲块只使用首页对应的 `Page` 结构加入链表，并通过 `property` 字段记录其阶数，其余页面作为该块的一部分连续管理。



分配内存时，系统先将请求的 `n` 页向上取整为能够容纳该请求的最小二次幂，即选择满足 $2^k \ge n$ 的最小阶数 $k$。如果第 $k$ 阶链表中存在空闲块，则直接取出；否则继续向更高阶查找，并将找到的大块逐级二分。每次拆分都会产生两个大小相等的伙伴块，其中一个继续拆分或返回给申请者，另一个加入相应低阶空闲链表。例如，申请 3 页时实际分配 4 页；如果当前只有 8 页块，则将其拆分为两个 4 页块，一个用于本次分配，另一个保留为空闲块。回收内存时，系统根据当前块的起始 PFN 和阶数计算其唯一伙伴。



只有当伙伴位于合法管理范围内、处于空闲状态且阶数相同时，两个块才可以合并为高一阶块。合并完成后，系统继续检查新块在更高阶中的伙伴，直到伙伴不满足合并条件或达到最大阶。因此，地址相邻且大小相同的两个块不一定互为伙伴，必须通过异或关系确定它们是否来自同一个父块。



本实现设置 `BUDDY_MAX_ORDER=15`，使用 `buddy_free_area[0..15]` 分别保存大小为 1、2、4、……、32768 页的空闲块。只有空闲块的首页设置 `PG_property`，其 `property` 字段记录该块所属的阶数 $k$；全局变量 `buddy_nr_free` 记录当前实际可用的空闲页总数。

```C
#define BUDDY_MAX_ORDER 15

static list_entry_t buddy_free_area[BUDDY_MAX_ORDER + 1];
static size_t buddy_nr_free;

static void buddy_init(void);
static void buddy_init_memmap(struct Page *base, size_t n);
static struct Page *buddy_alloc_pages(size_t n);
static void buddy_free_pages(struct Page *base, size_t n);
static size_t buddy_nr_free_pages(void);
static void buddy_check(void);
```

对于申请的 `n` 页，分配器首先计算满足 $2^k \ge n$ 的最小阶数：$k=\lceil \log_2 n \rceil$。比如申请 3 页时实际分配 4 页。该策略会产生一定的内部碎片，但能够通过分阶链表、块拆分和伙伴合并提高分配与回收效率。



Buddy System多阶空闲链表结构示意图：

![buddy\_free\_area\.png](图片和附件/buddy_free_area.png)

#### 最终提示词

按照实验要求，Buddy System 分为“算法实现”和“独立测试”两个任务，因此分别保留两个最终 Prompt。

##### P1：实现 Buddy System 物理页分配器

```Plain Text
请基于当前 uCore Lab2 框架实现一个可替换的 Buddy System 物理页分配器，并严格遵守已有的 struct pmm_manager 公共接口。

[CONTEXT]
- 目标架构为 RISC-V 64，物理页大小为 4 KiB。
- 可以复用 struct Page 的 flags、property 和 page_link 字段，但不得修改公共接口。
- pages 对应的合法 PFN 区间为 [nbase, npage)。
- init_memmap() 可能被多次调用，用于注册互不连续、起始地址不对齐且长度不是二次幂的空闲区域。
- 现有 First-Fit 和 Best-Fit 实现可作为 pmm_manager 接口及 Page 元数据使用方式的参考，但不得直接复用其单链表分配策略。

[REQUIREMENTS]
1. 新建 kern/mm/buddy_pmm.c 和 kern/mm/buddy_pmm.h，并定义 buddy_pmm_manager。
2. 设置 BUDDY_MAX_ORDER=15，维护 0～15 阶空闲链表。第 k 阶中的每个块必须包含 2^k 个连续页，并按照 2^k 页对齐。
3. buddy_init_memmap(base, n) 必须能够处理任意合法起点和任意长度的区域。初始化时，从低地址开始将区域拆分成“不超过剩余长度且满足起始 PFN 对齐”的最大二次幂块。
4. 多次调用 init_memmap() 时，如果新加入的空闲块与已有块构成真正的同阶伙伴，应继续向更高阶合并。
5. buddy_alloc_pages(n) 必须把非二次幂请求向上取整到最小的 2^k 页。从不低于目标阶的第一个非空链表取出块，并逐级二分；每次拆分后，将暂时不用的另一半加入下一阶空闲链表。
6. 如果没有足够大的空闲块，或者请求超过 BUDDY_MAX_ORDER 能表示的最大块，必须返回 NULL，而且失败前后的链表、页描述符和空闲页计数必须完全一致。
7. buddy_free_pages(base, n) 必须根据原请求重新计算实际阶数，并检查地址范围、块对齐及分配时保存的阶数。
8. 伙伴 PFN 必须通过 pfn ^ (1 << order) 计算。只有当伙伴处于合法 PFN 范围内、当前为空闲状态且阶数相同时，才允许合并。
9. 仅在空闲块的首页设置 PG_property，property 保存该块的阶数。块被分配后应清除 PG_property，但在首页保留分配阶数，以便释放时校验。
10. buddy_nr_free 必须按照实际占用或释放的 2^k 页更新，而不是按照调用参数 n 更新。
11. 增加 buddy_audit() 或等价审计函数，检查：
    - 空闲链表的双向链接正确；
    - 块没有越过合法 PFN 范围；
    - 第 k 阶块按 2^k 页对齐；
    - 空闲块首页的 PG_property 和 property 正确；
    - 不同空闲块之间不存在重叠；
    - 同阶的真正伙伴不会同时停留在空闲链表中；
    - 所有链表中的空闲页总数等于 buddy_nr_free。
12. 在 Makefile 和 kern/mm/pmm.c 中接入该管理器，使用户能够通过 make PMM=buddy 选择 Buddy System。
13. 在 buddy_check() 中至少检查 1 页、3 页和 5 页申请、拆分与合并、失败返回以及全部释放后的空闲页数恢复。

[OUTPUT]
- 给出新增或修改的文件列表。
- 给出 Buddy System 的关键实现代码。
- 说明初始化拆分、申请拆分、伙伴计算和释放合并的逻辑。
- 不要修改与本任务无关的模块。
- 所有修改必须能够在 -Wall -Werror 条件下编译通过。
```

##### P2：为 Buddy System 编写独立测试

```Plain Text
请在不重新实现 Buddy System 算法的前提下，为现有 Buddy 物理页分配器编写一组可独立运行的测试，并给出自动化运行脚本。

[CONTEXT]
- 被测实现位于 kern/mm/buddy_pmm.c 和 kern/mm/buddy_pmm.h。
- Buddy 分配器实现了 init、init_memmap、alloc_pages、free_pages、nr_free_pages 和 check 等 pmm_manager 回调。
- BUDDY_MAX_ORDER=15，非二次幂请求会向上取整。
- 测试必须验证实际行为和内部不变量，不能只检查源码中是否存在某个字符串。
- 测试代码放入 code/tests/buddy/，主要测试入口为 code/tests/buddy/check.c，运行脚本为 code/tests/buddy/run.sh。

[REQUIREMENTS]
1. 构造独立的 Page 数组及合法 PFN 范围，使测试不依赖真实机器的全部物理内存。
2. 增加独立 audit() 函数，逐页建立参考状态，并检查：
   - 空闲块没有重叠；
   - 空闲块没有覆盖保留页；
   - 每个空闲块位于合法区域内；
   - 块大小与所在阶数一致；
   - 块起点满足对应的二次幂对齐；
   - 链表覆盖的总页数等于 nr_free_pages()；
   - 同阶真正伙伴不会同时保留在空闲链表中。
3. 测试多段且非对齐的初始化区域，例如分别注册 [3,64) 和 [129,224)，验证两段共 156 页均被正确管理。
4. 测试申请 3 页和 5 页，验证实际分别消耗 4 页和 8 页，返回地址分别满足 4 页和 8 页对齐。
5. 在一个 16 页区域中申请并交错释放多个小块，验证：
   - 物理地址相邻但不是伙伴的块不得错误合并；
   - 真正伙伴释放后能够合并；
   - 合并可以逐级进行并最终恢复为一个 16 页块。
6. 测试内存耗尽后的再次申请，要求返回 NULL，并比较失败前后的链表、Page 元数据和空闲页计数，确认状态没有变化。
7. 测试超过 BUDDY_MAX_ORDER 的请求，要求安全返回 NULL，且状态保持不变。
8. 全部释放后，检查 nr_free_pages() 恢复为测试开始前的值。
9. run.sh 必须使用 -Wall -Werror 编译测试程序，在 QEMU 中运行，并保存完整日志。
10. 脚本必须同时检查：
    - 进程能够正常生成测试 ELF；
    - 日志中不存在 FAIL、assertion failed 或 kernel panic；
    - 日志中存在最终标志 PASS BUDDY_MODULE。
11. 各组测试分别输出清晰的 PASS 信息，最终输出格式至少包含：

buddy_check() succeeded!
PASS buddy layout / multiple regions / 3->4 / 5->8
PASS buddy adjacent non-partners / true partners / cascade merge
PASS buddy exhaustion / oversized request / failure unchanged
PASS BUDDY_MODULE

[OUTPUT]
- 给出 check.c、run.sh 及必要辅助文件的完整内容。
- 说明每组测试对应验证的 Buddy System 不变量。
- 如果测试发现实现缺陷，应先报告能够复现缺陷的最小场景，再给出局部修复建议。
- 不得通过删除断言、跳过失败场景或直接打印 PASS 的方式使测试通过。
```



#### 实现迭代过程

##### 第一阶段：完成基本分阶、拆分与合并

第一阶段建立了多阶空闲链表，实现请求阶数计算、从高阶块逐级拆分以及释放时向高阶合并。基本的单页和二次幂页数申请能够正常工作，但进一步测试发现两个容易被简单样例掩盖的问题：

- 对于 3 页、5 页等非二次幂请求，如果空闲页计数仍按照参数 `n` 增减，就会与实际占用的 4 页、8 页不一致。

- “物理地址相邻”并不等于“互为伙伴”。如果仅根据相邻关系合并，可能跨越二次幂父块边界，破坏 Buddy System 的对齐不变量。

##### 第二阶段：明确取整语义并补充不变量审计

第二阶段统一使用 $2^k$ 作为分配、回收和计数的实际页数，并使用$buddy\_pfn=pfn\oplus 2^k$计算当前块唯一的伙伴。初始化阶段不再假设可用区域天然对齐，而是从当前 PFN 开始，每次选择“既不超过剩余长度，又满足起始 PFN 对齐”的最大阶块，从而正确处理多段、非对齐和非二次幂长度的空闲区域。



同时加入 `buddy_audit()`，检查以下不变量：

1. 每个空闲块均位于合法 PFN 范围内；

2. 第 $k$ 阶空闲块的起始 PFN 按 $2^k$ 页对齐；

3. 空闲块首页具有 `PG_property`，且 `property` 与所在链表阶数一致；

4. 不同空闲块之间不存在重叠；

5. 同阶的两个真正伙伴不会同时停留在空闲链表中；

6. 所有空闲链表中块大小之和严格等于 `buddy_nr_free`。

    

##### 第三阶段：增加独立测试并验证失败原子性

第三阶段按照 P2 编写独立测试，不仅验证正常的分配与回收，还覆盖了多段初始化、非二次幂请求、相邻非伙伴、级联合并、内存耗尽和超大请求。测试在每次关键操作后执行 `audit()`，并在失败申请前后比较分配器状态，确保失败路径不会破坏链表、页描述符或空闲页计数。



#### 关键实现分析

**（1）初始化。** `buddy_init()` 初始化 16 条空闲链表并将空闲页数清零。`buddy_init_memmap()` 清理页描述符后，把任意 `[base, base+n)` 区间贪心拆分成满足地址对齐要求的最大二次幂块，再通过 `buddy_add_free_block()` 尝试与已有的真正伙伴合并。

**（2）分配。** `buddy_alloc_pages()` 首先计算目标阶数，然后从目标阶向上寻找第一个非空链表。若取得的块大于目标块，则不断将其二分，把暂时不用的另一半插入低一阶链表，直到得到目标大小。若所有更高阶链表均为空，则直接返回 `NULL`，不会修改分配器状态。

**（3）回收。** `buddy_free_pages()` 根据原请求计算实际阶数，并检查块地址、对齐方式以及首页保存的阶数。随后通过异或运算找到唯一伙伴；只要伙伴位于合法区域内、当前空闲且阶数相同，就将伙伴从链表摘除并合并到更高阶，直到无法继续合并。

**（4）统一接入。** `buddy_pmm_manager` 向框架注册标准回调。Makefile 将 `PMM=buddy` 转换为相应的编译宏，`pmm.c` 根据该宏令全局 `pmm_manager` 指向伙伴分配器，因此上层页分配接口不需要改变。

**最终结果：** Buddy System 已完成统一接口接入，实现了任意区域初始化、非二次幂请求取整、逐级拆分、伙伴合并、失败状态保护和一致性审计。独立测试和完整内核测试结果见第 5\.3 节和第 5\.5 节。

---



### 4\.2\.4 Challenge：任意大小的内存单元 slub 分配算法

**负责人：** 2412566\-曹子衿

#### 最终提示词

课程要求 SLUB Challenge 分别提供实现和测试两份 Prompt。以下为本次真实提交的 P3、P4 原文；其中 lab2/ 是初期开发副本，同步后的正式源码位于小组仓库 code/。

**P3：实现简化 SLUB**



```Plain Text
[PROMPT]
新增 lab2/kern/mm/slub.c 和 slub.h，实现基于公共页分配接口的简化 SLUB，包含小对象、大对象和完整释放路径。交付布局说明、接口、验证结果和初始化接入说明。slub_check 在下一步单独编写。

[RELY]
lab2.md 的 SLUB Challenge；成员二交接第 6–7 节；ADR 0004；
pmm.h 的 alloc_pages/free_pages/nr_free_pages、page2pa/pa2page、PADDR、va_pa_offset，PGSIZE = 4096；现有 list.h、defs.h、string.h。
当前 KADDR 被注释，地址转换使用已有可用接口。
Best-Fit 实占 n 页；Buddy 实占向上取整到二次幂的页数，释放时传回原块首和原申请 n。SLUB 通过公共接口与两者协作。

[GUARANTEE]
提供 void slub_init(void)、void *kmalloc(size_t size)、void kfree(void *ptr)。
内部完成尺寸类管理、slab 创建、对象分配回收、整页归还和大对象管理。

[SPECIFICATION]
布局：
- 尺寸类为 16、32、64、128、256、512、1024、2048，小对象选择最小可容纳类，至少 16 字节对齐。
- 小对象 slab 占一页：页首 header，空闲槽内保存下一空闲对象的位置；每类管理 partial 页，满页退出 partial，空页立即归还。
- 统一 header 长度 H = ROUNDUP(sizeof(header),16)，0 < H < PGSIZE；容量 C = floor((PGSIZE-H)/class_size)，每类 C > 0。
- 存活对象与 header、其他对象互不重叠；用户可写满申请的 size 字节，payload 无需自动清零。

slub_init：
Pre-Condition：PMM 原检查已完成，无活跃对象。
Post-Condition：尺寸类和空链表就绪，物理空闲页数不变。

kmalloc：
Pre-Condition：模块已初始化，size > 0。
Case 1：size <= 2048，优先使用 partial 页，必要时申请一页；返回足够且对齐的对象，页满后更新其状态。
Case 2：size > 2048，连续页申请数 n = ceil((H+size)/PGSIZE)，首页记录原 n，返回页首 + H；2049 字节可能仍只占一页。
Case 3：所需页数无法表示或无法分配，返回 NULL；已有对象和分配状态保持不变。

kfree：
Pre-Condition：ptr 是本模块返回且尚未释放的原始非空指针。
Case 1：小对象所在页仍有存活对象，释放槽可复用；满页回到 partial，已有 partial 保持单次入链。
Case 2：释放最后一个对象，整页归还，cache 清除对该页的引用；覆盖容量为一时 full 直接归还的情况。
Case 3：大对象以原 Page 和 header 中的原 n 调用 free_pages，两种底层均完整恢复实际占用。

Requirements：从返回指针恢复页首 header；对象存活时其 payload 归调用者使用，释放后才用于空闲链。用 header 保存 SLUB 自身信息，保持对底层 Page.property 的独立性。
```



**P4：独立设计并实现测试**



```Plain Text
[PROMPT]
为 SLUB 编写独立测试，在 slub.c 中增加测试实现，在 slub.h 声明入口。交付场景、独立预期和运行结果，发现缺陷后使用 F 修复。

[RELY]
lab2.md 关于 SLUB 实现与测试分别提供 Prompt 的要求；
P3 的公开规格、当前 header 布局、成员二交接第 8.2 节；
kmalloc/kfree、公共页分配接口，以及 Best-Fit/Buddy 的实际页数语义。

[GUARANTEE]
新增 void slub_check(void) 和必要测试 helper，覆盖对象行为、页面状态和最终资源恢复。
测试记账使用独立存储；容量可从实际布局读取，期望结果按规格计算。

[SPECIFICATION]
Pre-Condition：PMM 自检和 slub_init 已完成，SLUB 为空，测试独占分配活动。
Post-Condition：所有对象及耗尽测试持有页归还，空闲页数恢复，partial 链表为空。
Case 1：测试 1、15、16、17 和每个尺寸类边界两侧，直到 2047、2048、2049；验证 16 字节对齐和申请大小内的读写。
Case 2：同类申请 C+1 个对象，验证新增页；释放满页中的槽再申请，验证复用。覆盖交错释放、容量一和每类最后对象归还。
Case 3：多类和大对象交错存活，检查地址区间不重叠；写入不同数据，其他对象分配/释放后仍保持原内容。
Case 4：测试 4095、4096、4097、8192 字节。独立申请 8192 字节时原 n = 3，Best-Fit 实占 3 页、Buddy 实占 4 页，释放后均恢复。
Case 5：真实耗尽底层页，验证新 slab 和大对象申请返回 NULL、已有对象完好；已有 partial 的空槽仍能分配。最后释放全部对象和耗尽阶段持有页。
Requirements：分别在两种底层配置运行；页数预期包含 header 和 Buddy 取整成本。测试控制资源所有权，覆盖从创建到整页归还的全过程。
```



#### **实现迭代过程**



**第一阶段：确定两层分配契约。** 页级仍只通过 alloc\_pages、free\_pages、nr\_free\_pages 访问当前 PMM。小对象采用 16、32、64、128、256、512、1024、2048 字节八个类，向上选最小可容纳类并保证至少 16 字节对齐。每个 slab 占一页，页首放 header，剩余位置划成固定对象槽；空闲槽内部存下一槽指针，存活对象的整个申请范围由调用者使用。本次 RV64 构建中 header 对齐后为 64 字节，容量公式为 C=floor\(\(4096−64\)/class\_size\)。



**第二阶段：完成对象生命周期。** slub\_init 建立各类的 partial 链表；kmalloc 优先从对应 partial 页取槽，类内无可用页时再向底层申请一页。取走最后槽后，该页从 partial 移出；kfree 使满页重新进入 partial，释放最后一个存活对象时立即把整页交还 PMM。容量为 1 的 2048 字节 slab 从满页直接进入整页回收，也在测试中覆盖。大于 2048 字节的请求以 n=ceil\(\(header\_size\+size\)/4096\) 申请连续页；header 保存原 n，kfree 用原 n 归还，避免 Buddy 把 3 页取整成 4 页后发生释放契约不一致。



**第三阶段：将测试与实现分开。** P3 只交付 slub\_init、kmalloc、kfree，P4 再独立交付 slub\_check。测试以自己的存活对象清单和页所有权账本计算预期，不从生产 inuse 或 free\_head 倒推结果。它覆盖 25 个边界尺寸、每类 C＋1 个对象的跨页行为、满页重返 partial、复用、不同对象内容隔离、大对象页数、底层耗尽和全部释放后的页数恢复。独立测试分别链接真实 Best\-Fit 和真实 Buddy 后端，非模拟页分配器。



**第四阶段：同步、组合验证与修复。** P5 在团队代码中确认 pmm\_init 的原管理器自检先完成，再执行 Best\-Fit 补充检查和可选 SLUB 自检；两种底层的独立测试与正式内核配置均有真实 QEMU 结果。P6 未发现已约定合法输入下的 SLUB 分配/释放算法错误，发现的是脚本对非零退出和配置组合的误判；F 修改运行器及 Makefile 后重新执行受影响的组合。F 未改 kern/mm/slub\.c、kfree 路径或课程原检查。



#### **本实现的取舍**

尺寸类可快速定位并复用小对象，但一个对象实际占用其所属 class\_size，最后一页不足一个槽的尾部也不能使用，存在内部碎片。小对象的常规取槽与释放为 O\(1\)，首次建页需 O\(C\) 初始化空闲槽；大对象的页搜索成本由底层 PMM 决定。本项目在单核实验里选择“空 slab 立即归还”，便于验证物理页恢复。

#### **成员二 Prompt 与反馈迭代记录**



|阶段|该阶段解决的问题|
|---|---|
|P0|从已有实现提炼空闲块、Page 标志和公共接口契约|
|P1 / P2|先实现完整管理器，再用独立候选和状态核对检验选择、分裂与合并|
|P3 / P4|先定义对象布局和大对象释放契约，再测试边界、生命周期及两种底层|
|P5|解决既有工具链的 ISA 参数兼容问题，实测四项独立测试、四种正式配置及评分|
|P6 / F|审查发现四项验证与配置问题；修复脚本判定、评分运行器及不支持的组合，并重跑受影响测试|
|P7|把实际 Prompt、实现原理与可复核运行证据写入个人负责章节|



课程要求分别提交 Best\-Fit 实现 Prompt、SLUB 实现和测试 Prompt；P0/P2/P5/P6/F/P7 是本组为理解、验证和修复而组织的阶段。P5 初次完整验证时的原始日志与 F 修复后的回归结果分别保留。

---

### 4\.2\.5 分支任务：gdb 调试页表查询过程

**负责人：** 2412103\-唐健

### （1）实验目的与调试环境

本任务通过双重 GDB 调试，观察 uCore 执行访存指令时，QEMU 如何模拟 TLB 填充和 Sv39 页表地址翻译过程。实验使用独立编译的 QEMU 4\.1\.1 调试版本，并保留调试符号，以便在 QEMU 源码中设置断点、查看函数参数和单步执行。

实验采用三个终端协同完成。终端 1 使用 `make debug QEMU="$QEMU_DEBUG"` 启动 QEMU；终端 2 使用普通 GDB 附加到 QEMU 进程，在 `riscv_cpu_tlb_fill()` 和 `get_physical_address()` 等函数处设置断点；终端 3 使用 `riscv64-unknown-elf-gdb` 连接 QEMU 提供的 GDB 调试接口，对 uCore 内核进行调试。为避免远程调试过程中出现 `Ignoring packet error`，在终端 3 中设置 `set remotetimeout unlimited`。

![111\.png](图片和附件/111.png)



### （2）从实际访存进入 QEMU 的 TLB 填充路径

在终端 3 中运行 uCore，并通过 GDB 观察内核执行位置。终端 2 命中 `riscv_cpu_tlb_fill()` 断点后，查看函数参数和调用栈，可以看到本次访问的关键信息：

- `address = 0xffffffffc0205ff8`

- `size = 8`

- `access_type = MMU_DATA_STORE`

- `mmu_idx = 1`

调用栈显示，本次访问经过 `store_helper()`、`tlb_fill()`，进入 `riscv_cpu_tlb_fill()`。继续执行后，程序调用 `get_physical_address()` 完成地址翻译。

这一调用路径说明，uCore 执行了一次 8 字节的存储操作，QEMU 随后进入 TLB 填充处理流程，并通过页表查询获取映射关系。结合 `tlb_fill()` 的调用位置，可以观察到软件 TLB 未命中后的处理过程。

![222\.png](图片和附件/222.png)



### （3）Sv39 页表查询过程分析

在终端 3 中查看寄存器，可以得到 `satp = 0x8000000000080206`。该值的模式字段为 8，表示当前使用 Sv39 分页模式；其页表物理页号为 `0x80206`，因此根页表的物理基址为 `0x80206000`。

Sv39 使用三级页表结构，每级页表索引占 9 位，页内偏移占 12 位。QEMU 在 `get_physical_address()` 中读取 `satp`，确定根页表地址，并设置 `levels = 3`、`ptidxbits = 9`、`ptesize = 8`。随后通过 `ptshift` 和 `idx` 计算页表索引，再读取相应的页表项并检查其有效位、访问权限等信息。

本次调试还查看了根页表的第 511 项，其值为 `0x200000cf`。该页表项的有效位及读、写、执行权限位均已设置，属于叶子页表项。由于该映射在根级就结束了页表遍历，因此本次访问不需要继续查询第二级和第三级页表。这也说明，Sv39 虽然采用三级页表结构，但实际查询深度取决于叶子页表项所在的层级。

![333\.png](图片和附件/333.png)

### （4）地址翻译结果与访问权限

在 `get_physical_address()` 中，GDB 观察到本次访问的虚拟地址为 `0xffffffffc0205ff8`，访问类型为存储操作。函数返回后，`pa` 的值为 `0x80205000`，`prot` 的值为 `0x7`。

其中，`pa` 是用于建立 TLB 映射的物理页基址，`prot = 0x7` 表示读、写、执行权限均已设置。虚拟地址的低 12 位为页内偏移 `0xff8`，因此本次访问对应的完整物理地址为：

`0x80205000 + 0xff8 = 0x80205ff8`

该结果表明，QEMU 成功将内核虚拟地址 `0xffffffffc0205ff8` 映射到物理地址 `0x80205ff8`。翻译成功后，`riscv_cpu_tlb_fill()` 会根据翻译结果建立相应的 TLB 映射，以便后续访问能够复用该映射信息。



![444\.png](图片和附件/444.png)

### （5）调试过程中的问题与收获

调试过程中，`get_physical_address()` 曾经以 `addr = 0` 命中断点。通过查看调用栈，可以发现这次调用来自 QEMU 的 `gdbstub.c`、`target_memory_rw_debug()` 和 `cpu_memory_rw_debug()` 等函数，是调试器读取目标内存时触发的地址查询，并非 uCore 正在执行的实际访存指令。区分这两种调用后，继续运行并观察 `riscv_cpu_tlb_fill()` 的调用栈，才获得了本次实验所需的实际访存证据。

通过本次调试，可以将虚拟内存的理论知识与 QEMU 的具体实现对应起来：uCore 执行访存指令，QEMU 的软件 TLB 处理流程发现需要补充映射后，调用 RISC\-V 地址翻译函数查询页表，并根据页表项和访问权限确定映射结果。双重 GDB 的方式使我们能够同时观察被模拟系统的执行状态和模拟器本身的实现过程，加深了对 TLB、Sv39 多级页表及虚拟地址到物理地址转换机制的理解。

# 五、测试与验证

## 5\.1 物理内存初始化功能测试

**负责人：** 2412103\-唐健

### 测试目的

本节验证 `page_init()` 能否正确读取设备树中的物理内存信息，建立 `Page` 描述符数组，并将可分配的物理页交给当前内存管理器。同时使用 GDB 检查关键全局变量和页表状态，确认物理地址、虚拟地址和物理页号之间的对应关系。

重点检查以下内容：

1. 能否识别 QEMU 提供的物理内存起始地址和容量。

2. `npage`、`pages` 和 `va_pa_offset` 是否正确初始化。

3. 内核及描述符数组占用的空间是否被排除在初始空闲页之外。

4. 物理内存管理器能否正常完成页面分配与释放检查。

### 测试环境

### 测试过程与结果

**（1）编译与启动测试**

在 `code` 目录下清理旧的构建产物，重新编译并启动 QEMU：

```Plain Text
cd ~/projects/NKU-OS-2026/code

make clean
make -j2
make qemu
```

系统成功启动，并输出设备树探测到的物理内存信息：

```Plain Text
Physical Memory from DTB:
  Base: 0x0000000080000000
  Size: 0x0000000008000000 (128 MB)
  End:  0x0000000087ffffff

memory management: best_fit_pmm_manager

DTB physical memory:
[0x0000000080000000, 0x0000000088000000)

Managed physical memory:
[0x0000000080000000, 0x0000000088000000)

check_alloc_page() succeeded!
```

结果表明，内核识别出的物理内存范围为：\[0x80000000, 0x88000000\)

大小为 `0x08000000` 字节，即 128 MiB，与 QEMU 的内存配置一致。

`check_alloc_page() succeeded!` 表明初始化完成后，当前管理器的基础页面分配与释放检查通过。

在 Best\-Fit 扩展测试结束后，系统进一步输出：

```Plain Text
PASS best_fit_extra_check: restored 31928 free pages
```

这说明扩展测试结束时，分配器恢复了 31928 个空闲物理页。它同时反映出内核将部分物理内存保留给固件、内核和管理数据结构，其余部分交由分配器管理。



**（2）使用 GDB 验证关键初始化变量**

为了进一步核对初始化结果，使用 `make debug` 启动 QEMU，再通过 `make gdb` 连接内核调试器。在 `pmm_init()` 返回后，检查关键全局变量：

```Plain Text
p/x npage
p/x pages
p/x va_pa_offset
p/x satp_virtual
p/x satp_physical
info registers pc satp
x/gx satp_virtual + 511
```

GDB 实际输出的主要结果如下：

![gdb\.png](图片和附件/gdb.png)



首先，RAM 的物理页号范围为：

nbase==0x80000

npage==0x88000

因此需要覆盖的物理页数量为：N=npage\-nbase=0x8000=32768

与 128 MiB 内存按照 4 KiB 页面划分的结果一致。

其次，根据 `va_pa_offset`，可以将 `pages` 的虚拟地址转换为物理地址：

PA=0xffffffffc0208000\-0xffffffff40000000

得到：PA=0x80208000

这说明描述符数组位于内核镜像之后的物理内存中，与初始化阶段的内存布局设计一致。

最后，`satp` 的高 4 位为 `8`，表示当前使用 Sv39 分页模式。其根页表物理页号为 `0x80206`，对应物理地址 `0x80206000`，与 `satp_physical` 的值一致。

根页表第 511 项为 `0x200000cf`，是一个有效的根级叶子页表项。它将内核使用的高虚拟地址区域映射到从 `0x80000000` 开始的物理地址区域，与实验建立的 1 GiB 大页映射相符。



### 测试结果分析

本次测试从运行日志和调试器状态两个层面验证了物理内存初始化功能。

运行日志确认了设备树探测结果与 QEMU 配置一致，并且页面分配器能够完成基础检查。GDB 则进一步确认了页号边界、描述符数组位置、虚实地址偏移及根页表地址之间的一致性。

其中，`npage` 表示物理页号的结束边界，描述符数量应使用 `npage - nbase` 计算；`pages` 保存的是内核虚拟地址，需要通过地址偏移才能确定其物理位置。这两个关系是本模块正确建立物理内存管理结构的关键。

### 测试结论

在 128 MiB 的 QEMU 配置下，`page_init()` 成功完成物理内存范围识别、描述符数组初始化及初始空闲区注册。系统能够正常启动，分配器基础检查通过，GDB 检查到的关键内存布局变量与预期一致。

**物理内存初始化功能在本次默认配置下测试通过。**

---

## 5\.2 First\-Fit 与 Best\-Fit 分配算法测试

### 5\.2\.1 First\-Fit 分配算法测试

**负责人：** 2412103\-唐健

#### 测试目的

本节验证实验框架原有的 First\-Fit 分配器能否在完成物理内存初始化后正确工作，并检查其基础连续页面分配与释放行为。

First\-Fit 按空闲链表的顺序寻找第一个满足申请大小的连续空闲块。当空闲块大于请求大小时，将多余部分保留在空闲链表中；释放时则通过合并相邻空闲块恢复连续空间。

本次测试重点确认 First\-Fit 管理器能够被正确选中，并通过已有的 `check_alloc_page()` 基础检查。

#### 测试环境与方法

测试使用与 5\.1 节相同的 WSL2、QEMU 和 RISC\-V 编译环境，模拟物理内存仍为 128 MiB。

由于小组当前集成版本默认使用 Best\-Fit，为避免 First\-Fit 测试影响已经完成的集成代码，使用独立测试工作区进行验证。

在测试版本的 `init_pmm_manager()` 中，临时将管理器设置为：

```Plain Text
pmm_manager = &default_pmm_manager;
```

随后重新编译内核并启动 QEMU：

```Plain Text
make clean
make -j2
make qemu
```

该测试只改变管理器选择，不修改 `default_pmm.c` 中的 First\-Fit 算法和原有自检函数。

#### 测试过程与结果

系统启动后，首先确认当前使用的是 First\-Fit 管理器：

```Plain Text
memory management: default_pmm_manager
```

随后输出设备树和可管理物理内存范围：

```Plain Text
DTB physical memory:
[0x0000000080000000, 0x0000000088000000)

Managed physical memory:
[0x0000000080000000, 0x0000000088000000)
```

最终，内核执行物理内存分配器的基础检查，得到：

```Plain Text
check_alloc_page() succeeded!
```

完整运行结果如下图所示。

![first\_fit\.png](图片和附件/first_fit.png)





#### 测试结果分析

从日志可以确认，`init_pmm_manager()` 成功选择了 `default_pmm_manager`，而且物理内存初始化模块能够为 First\-Fit 提供正确的初始空闲页面集合。

`check_alloc_page()` 使用当前选定的物理内存管理器执行检查。测试成功说明 First\-Fit 能够完成已有自检覆盖的连续页面申请、释放及空闲状态维护操作。

本次验证还说明，`page_init()` 建立的描述符布局可以同时用于 First\-Fit 和 Best\-Fit，具体分配策略由 `pmm_manager` 负责实现。

需要说明的是，小组集成版本还包含 Best\-Fit 专用的扩展测试，该测试要求当前管理器为 `best_fit_pmm_manager`。当临时切换到 First\-Fit 时，后续执行该扩展测试可能因管理器类型不符而触发断言。因此，本节以 First\-Fit 自身的 `check_alloc_page()` 成功结果作为测试依据，未将 Best\-Fit 专用检查计入 First\-Fit 的测试结果。

测试结束后恢复原有 Best\-Fit 配置，保证后续集成测试使用小组统一的管理器。

#### 测试结论

在 128 MiB 的 QEMU 模拟环境下，First\-Fit 管理器能够正确完成初始化，并通过现有的页面分配与释放基础自检。

测试同时验证了物理内存初始化模块与 First\-Fit 分配算法之间的接口兼容性。

**First\-Fit 分配算法的基础功能测试通过。**

---

### **5\.2\.2 Best\-Fit 分配算法测试**



**负责人：** 2412566\-曹子衿

#### **测试目的**

本节测试 Best\-Fit 能否从所有可用空闲块中选择满足申请要求的最小块，并在分配和释放过程中正确维护空闲链表。除课程原有的 `basic_check()` 和 `best_fit_check()` 外，我增加了 `best_fit_extra_check()`，进一步检查最小块选择、同尺寸块的地址顺序、空闲块分裂、前后合并、碎片导致的分配失败以及空闲页计数恢复。



测试没有修改课程原有的两个检查函数，管理器的 `check` 接口仍然调用原有 `best_fit_check()`。补充测试通过公开的 `alloc_pages()` 和 `free_pages()` 构造页面状态，使测试过程与内核中的实际调用方式一致。

#### **测试内容**

|测试场景|预期结果|
|---|---|
|最小块选择|空闲块大小为 6、3、5 页时申请 2 页，应选择 3 页块|
|同尺寸块选择|两个候选块大小相同时，应选择物理地址较低的块|
|空闲块分裂|从 5 页块中申请 2 页，剩余 3 页继续作为一个合法空闲块|
|相邻块合并|分别验证无相邻块、仅前邻、仅后邻和前后都相邻四种情况|
|碎片与耗尽|总空闲页足够但不存在连续大块时，申请失败并返回 `NULL`|
|状态恢复|测试结束后，空闲链表与空闲页数恢复到测试开始前的状态|



#### **测试环境与准备**

以下命令在 macOS 项目根目录中执行，并使用项目统一的 Docker 镜像完成 RISC\-V 交叉编译和 QEMU 运行。为了保留测试现场，先把当前小组代码复制到一个带时间戳的项目临时目录中。

```Bash
cd /Users/odalys/Documents/OS_Project
capture="$PWD/.artifacts/lab2-member2-report-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$capture/code" "$PWD/docs/lab2/OS-lab2/图片和附件"
tar -C workspace/NKU-OS-2026/code \
  --exclude='./obj' --exclude='./bin' --exclude='./.artifacts' \
  -cf - . | tar -C "$capture/code" -xf -
```

上述命令的作用如下：

1. `cd /Users/odalys/Documents/OS_Project`：进入项目根目录，保证后续相对路径一致。

2. `capture=...`：创建本轮测试目录名。目录名中包含执行时间，因此不会覆盖之前的测试记录。

3. `mkdir -p ...`：创建测试源码目录和报告图片目录。

4. `tar -C ... | tar -C ...`：把当前 `code/` 复制到测试目录，同时排除旧的编译产物和旧日志，使本轮测试从干净副本开始。

#### **独立模块测试**

执行 Best\-Fit 独立测试脚本：

```Bash
docker run --rm --pull=never --platform linux/amd64 --network none \
  --mount "type=bind,src=$capture,dst=/work" \
  --workdir /work/code \
  os-project-lab:ubuntu22.04-amd64 \
  bash tests/best_fit/run.sh
echo "best_fit_test_exit=$?"
```

各项参数的含义如下：

1. `docker run --rm`：启动一次性测试容器，命令结束后自动删除容器。

2. `--pull=never`：直接使用项目已经准备好的镜像，避免测试时重新下载或改变环境。

3. `--platform linux/amd64`：使用小组统一的 amd64 容器环境。

4. `--network none`：测试过程不需要访问网络。

5. `--mount ...`：把本轮测试目录挂载到容器中的 `/work`。

6. `--workdir /work/code`：从复制后的代码目录运行脚本。

7. `bash tests/best_fit/run.sh`：交叉编译 Best\\\-Fit 独立测试内核，启动 QEMU，并检查运行日志。

8. `echo "best_fit_test_exit=$?"`：显示测试脚本退出状态；正常通过时应为 `0`。

脚本会连续运行四种模式。模式 0 执行正常的 Best\\\-Fit 功能测试并输出 `PASS BEST_FIT_MODULE`；模式 1、2、3 分别故意调用 `alloc_pages(0)`、`free_pages(..., 0)` 和 `init_memmap(..., 0)`，因此终端随后出现三行 `EXPECTED ASSERT ... n > 0`。这些是零页参数契约测试的预期结果，不是运行失败。脚本只在每种模式恰好得到对应断言时继续执行，所以最后的 `best_fit_test_exit=0` 表示正常功能和三项断言测试全部通过。

测试完成后，执行下面的命令提取关键结果：

```Bash
grep -E 'PASS original|PASS best_fit_extra|PASS 10000|PASS BEST_FIT_MODULE' \
  "$capture/.artifacts/best-fit/qemu-0.log" | tr -d '\r'
```

其中，`grep -E` 使用扩展正则表达式从 QEMU 串口日志中筛选原检查、四组补充检查、10000 次确定性状态核对和模块最终通过标志；`tr -d '\r'` 去除串口日志中的回车字符，使终端输出排列整齐。实际运行得到的关键结果为：

```Plain Text
PASS original basic_check + best_fit_check (6/6)
PASS best_fit_extra case 1: exact / 6-3-5 / low-address ties
PASS best_fit_extra case 2: 5 -> 2 + 3 / metadata / count
PASS best_fit_extra case 3: none / prev / next / both merges
PASS best_fit_extra case 4: fragmentation / exhaustion / unchanged
PASS 10000 deterministic bitmap-oracle operations and full recovery
PASS BEST_FIT_MODULE
```



![image\.png](图片和附件/image%205.png)

#### **官方评分测试**

独立测试通过后，在同一份测试副本上运行课程评分脚本：

```Bash
docker run --rm --pull=never --platform linux/amd64 --network none \
  --mount "type=bind,src=$capture,dst=/work" \
  --workdir /work/code \
  os-project-lab:ubuntu22.04-amd64 \
  make grade PMM=best_fit SLUB=0 \
  GRADE_TMP=/work/.artifacts/member2-grade
echo "grade_exit=$?"
```

命令中的 `make grade` 调用仓库已有的课程评分流程；`PMM=best_fit` 指定本轮使用 Best\\\-Fit 管理器，`SLUB=0` 关闭本节不需要的 SLUB 模块，`GRADE_TMP=...` 把评分时生成的临时文件保存在本轮测试目录中。最后一条 `echo` 显示评分命令的退出状态。



评分输出的关键部分为：

```Plain Text
-check page init:                          OK
-check best fit:                           OK
Total Score: 20/20
grade_exit=0
```

![image\.png](图片和附件/image%208.png)



#### **测试结果分析**

独立测试中，课程原有检查、四组 Best\-Fit 补充检查和 10000 次确定性状态核对均通过，说明算法能够选择最小可用块，正确完成分裂和合并，并在申请失败时保持原有链表状态。测试结束后空闲页数恢复，说明分配和释放过程中没有遗失页面。



课程评分结果为 20/20，说明当前 Best\-Fit 实现同时满足页面初始化和课程评分脚本的检查要求。独立测试负责覆盖具体算法边界，课程评分负责验证实现能够接入完整实验代码，两部分结果相互补充。

#### **测试结论**

Best\-Fit 分配器能够按照最小适配原则完成连续页分配，正确处理块分裂、相邻块合并、碎片失败和资源恢复，并通过课程原有检查、补充测试与官方评分。**Best\-Fit 分配算法测试通过。**



---

## **5\.3 Buddy System 伙伴分配算法测试**

**负责人：** 2412266\-杨滢

### 5\.3\.1 独立模块测试

Buddy System 采用**管理器内建自检 \+ 独立 RISC\-V 模块测试**两层验证。内建 `buddy_check()` 申请 1、3、5 页，检查其实际消耗分别为 1、4、8 页，并在全部释放后确认空闲页数恢复。

|测试场景|主要操作|通过判据|
|---|---|---|
|多段与非对齐初始化|注册 `[3,64)` 和 `[129,224)` 两段，共 156 页|全部页被无重叠地拆入合法阶链表，空闲页数为 156|
|非二次幂请求|依次申请 3 页和 5 页|实际按 4 页和 8 页分配，起始 PFN 分别按 4、8 对齐|
|伙伴判定与级联合并|在 16 页区域申请并交错释放 4 个单页块|相邻但非伙伴的块不合并；真正伙伴最终恢复为一个 16 页块|
|失败原子性|耗尽全部 16 页后继续申请，并申请超过最大阶的页数|返回 `NULL`，页描述符、链表和空闲计数与失败前完全一致|
|计数恢复|释放全部已分配块|`nr_free_pages()` 恢复到测试开始值|

在 WSL 的项目根目录执行：

```Bash
cd code
GCCPREFIX=riscv64-linux-gnu- bash tests/buddy/run.sh
```

如果安装的是 `riscv64-unknown-elf-gcc`，可直接执行：

```Bash
cd code
bash tests/buddy/run.sh
```

脚本会以 `-Wall -Werror` 交叉编译独立测试内核，在 QEMU 中运行，并检查日志中不存在 `FAIL` 或 `kernel panic`，且必须出现 `PASS BUDDY_MODULE`。完整通过时关键输出应为：

```Plain Text
buddy_check() succeeded!
PASS buddy layout / multiple regions / 3->4 / 5->8
PASS buddy adjacent non-partners / true partners / cascade merge
PASS buddy exhaustion / oversized request / failure unchanged
PASS BUDDY_MODULE
```

![image\.png](图片和附件/image%206.png)

可以看到，实验结果满足预期。在 WSL 中实际完成交叉编译并生成 `.artifacts/buddy/check.elf`，说明 Buddy 测试源码通过严格编译选项。当前机器的 QEMU 在限定时间内停留于 OpenSBI 启动输出，尚未得到测试内核的 `PASS` 日志，因此最终提交前仍需在课程镜像或能够正常启动 QEMU 的环境中复跑上述命令；不能仅凭源码中存在 `PASS` 字符串判定测试通过。



### 5\.3\.2 完整内核编译与启动验证

整体集成验证需要同时确认伙伴分配器可以被完整内核选中和原有官方功能没有被新增代码破坏。由于编译宏变化不会自动触发所有目标重建，切换 PMM 前必须清理旧产物。

**1\. Buddy 完整内核构建与启动**

```Bash
cd code
make clean
make PMM=buddy
timeout 15s make qemu PMM=buddy
```

启动日志中出现以下关键信息：

```Plain Text
memory management: buddy_pmm_manager
buddy_check() succeeded!
check_alloc_page() succeeded!
```

![image\.png](图片和附件/image%202.png)

![image\.png](图片和附件/image%207.png)

第一个`memory management: buddy_pmm_manager`证明编译期选择已生效，后两个说明 Buddy 自检以及 PMM 统一检查入口均已执行完成。`timeout` 退出不代表内核失败，因为实验内核完成初始化后会停留在无限循环中；应以日志中是否出现 panic 和成功标志为判断依据。



**2\. 官方评分验证**

```Bash
cd code
make grade
```

官方评分脚本主要验证物理内存初始化、地址边界和 Best\-Fit 等基础要求；Buddy Challenge 则由第 5\.3 节的独立脚本补充验证。两者结合能够区分“基础实验功能回归”与“挑战算法专项正确性”。执行后应保存完整评分汇总，确认不存在 `FAIL`、`kernel panic` 或编译错误。

![image\.png](图片和附件/image%204.png)

**3\. 验收结论**

仓库已经具备 Buddy 独立测试、编译器开关和完整内核接入。



---

## **5\.4 SLUB 任意大小内存分配测试**



**负责人：** 2412566\-曹子衿

#### **测试目的**

本节验证 SLUB 能否在页级物理内存分配器之上完成任意大小内存申请。测试内容包括小对象尺寸分类、16 字节对齐、同一 slab 内的对象复用、跨 slab 分配、大对象连续页分配、交错释放、内存内容保持以及全部释放后的页面归还。



SLUB 分别接入 Best\-Fit 和 Buddy 两种底层分配器。这样既能检查对象层功能，也能验证大对象释放时传递的页数参数能够适应两种 PMM 不同的实际占页规则。



#### **测试内容**

|测试场景|预期结果|
|---|---|
|尺寸边界|1、15、16、17、2047、2048、2049 等请求能够正确分类并保持 16 字节对齐|
|跨页与复用|当前 slab 用满后申请新页；释放对象后，原有空槽能够再次使用|
|对象隔离|同时存活的对象地址不重叠，写入一个对象不会破坏其他对象内容|
|大对象分配|超过 2048 字节的请求转为连续页申请，并在释放时归还完整块|
|底层耗尽|无法取得新页时返回 `NULL`，已有 partial slab 中的空闲对象仍可继续分配|
|状态恢复|全部对象释放后，空闲页数恢复，八个尺寸类的 partial 链表为空|

#### **测试环境与准备**

以下命令同样在项目根目录执行。先复制当前代码，为本轮 SLUB 测试建立独立目录：

```Bash
cd /Users/odalys/Documents/OS_Project
capture="$PWD/.artifacts/lab2-member2-slub-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$capture/code" "$PWD/docs/lab2/OS-lab2/图片和附件"
tar -C workspace/NKU-OS-2026/code \
  --exclude='./obj' --exclude='./bin' --exclude='./.artifacts' \
  -cf - . | tar -C "$capture/code" -xf -
```

上述命令的作用如下：

1. `cd /Users/odalys/Documents/OS_Project`：进入项目根目录。

2. `capture=...`：使用当前时间生成独立的 SLUB 测试目录名。

3. `mkdir -p ...`：创建测试源码目录和报告图片目录。

4. `tar -C ... | tar -C ...`：复制当前代码，并排除旧的编译产物与测试日志。

后续 Best\-Fit 和 Buddy 测试共用这份副本，保证两次运行除底层 PMM 选择外使用相同源码。

#### **Best\-Fit 后端测试**

先使用 Best\-Fit 作为 SLUB 的底层页分配器：

```Bash
docker run --rm --pull=never --platform linux/amd64 --network none \
  --mount "type=bind,src=$capture,dst=/work" \
  --workdir /work/code \
  os-project-lab:ubuntu22.04-amd64 \
  bash tests/slub/run.sh best_fit
echo "slub_best_fit_exit=$?"
```



这条 `docker run` 命令使用统一镜像挂载测试副本，并运行 `tests/slub/run.sh best_fit`。脚本会选择 Best\-Fit，交叉编译独立测试内核，在 QEMU 中运行两种页池布局，并检查最终通过标志。`echo` 显示脚本退出状态，正常通过时为 `0`。



执行下面的命令提取适合观察的关键结果：



```Bash
grep -E 'PASS original PMM|size=8192|PASS slub_check|PASS SLUB_MODULE' \
  "$capture/.artifacts/slub-check/best_fit/qemu.log" \
  | tail -n 12 | tr -d '\r'
```



`grep -E` 保留底层 PMM 检查、大对象占页数、SLUB 状态恢复和最终通过标志；`tail -n 12` 只显示日志末尾的关键行；`tr` 去除串口回车字符。Best\\\-Fit 后端的关键输出为：



```Plain Text
PASS slub case 4: size=8192 original_n=3 actual_pages=3
PASS slub_check: restored 509 free pages / empty partial lists
PASS SLUB_MODULE
```



其中，8192 字节对象加上页首元数据需要申请 3 页，Best\-Fit 按原申请页数实际占用 3 页。

![image\.png](图片和附件/image.png)

#### **Buddy 后端测试**

随后保持源码和测试内容不变，只把底层页分配器切换为 Buddy：

```Bash
docker run --rm --pull=never --platform linux/amd64 --network none \
  --mount "type=bind,src=$capture,dst=/work" \
  --workdir /work/code \
  os-project-lab:ubuntu22.04-amd64 \
  bash tests/slub/run.sh buddy
echo "slub_buddy_exit=$?"
```

`tests/slub/run.sh buddy` 会重新选择 Buddy 后端并运行同一套 SLUB 检查。执行完成后提取关键日志：

```Bash
grep -E 'PASS original PMM|size=8192|PASS slub_check|PASS SLUB_MODULE' \
  "$capture/.artifacts/slub-check/buddy/qemu.log" \
  | tail -n 12 | tr -d '\r'
```

Buddy 后端的关键输出为：

```Plain Text
PASS slub case 4: size=8192 original_n=3 actual_pages=4
PASS slub_check: restored 508 free pages / empty partial lists
PASS SLUB_MODULE
```

SLUB 仍然按对象大小申请 3 页，Buddy 将其向上取整为 4 页块。释放时 SLUB 传回原申请页数，Buddy 根据相同规则找到并归还完整的 4 页块，因此测试结束后空闲页数能够恢复。

![image\.png](图片和附件/image%203.png)



#### **测试结果分析**



两种后端都完成了尺寸边界、跨页分配、空槽复用、交错生命周期、大对象和底层耗尽测试，并分别输出 `PASS SLUB_MODULE`。Best\-Fit 对 3 页请求实际分配 3 页，Buddy 对同一请求实际分配 4 页，这一差异符合两种页分配算法的设计。



全部对象释放后，空闲页数均恢复到各自测试开始时的状态，所有尺寸类的 partial 链表为空，说明 SLUB 没有遗留仍被占用的 slab 页或大对象页。测试过程中所有存活对象的内容检查均通过，说明对象之间没有发生覆盖。

#### **测试结论**

SLUB 能够正确处理小对象和大对象申请，完成对象复用与页面归还，并能同时工作在 Best\\\-Fit 和 Buddy 两种底层页分配器之上。**SLUB 任意大小内存分配测试通过。**



---

# **六、实验总结与收获**

## **对操作系统的理解**

1. **物理内存探测是分配器正确性的边界。** 算法不能把“地址看起来存在”的页面直接视为空闲页，而应先依据设备树、内核镜像末尾和页描述符数组位置确定真正可管理的范围。理论中的物理地址空间是抽象概念，实验则要求把这一概念落实为严格的半开区间和页对齐计算。

2. **页描述符是物理页状态的软件映射。**Page结构并不保存页面内容，而是记录引用计数、保留/空闲标志、块大小和链表节点。它把离散的物理页转化为内核能够查询和组织的数据结构，但其自身也占用物理内存，因此必须先保留再开放剩余页面。

3. **统一接口体现机制与策略分离。**pmm\_manager定义初始化、分配、释放、统计和检查机制，First\-Fit、Best\-Fit、Buddy 则提供不同策略。上层不依赖具体实现，使算法替换和独立测试更容易。这与操作系统中常见的 VFS、调度类等设计思想一致。

4. **Buddy System 用内部碎片换取快速合并。** 请求被向上取整为二次幂，因此 5 页会占用 8 页，存在内部碎片；但多阶空闲链表缩小了搜索范围，异或运算可以直接得到唯一伙伴，回收时无需遍历所有空闲块寻找相邻区间。相比之下，First\-Fit 和 Best\-Fit 对任意连续页数利用更充分，但查找和合并通常依赖链表遍历，更容易形成外部碎片。

5. **正确性包括失败不破坏状态。** 内存耗尽、超大请求、多段非对齐区域和交错释放都是关键边界。通过审计链表、对齐、覆盖范围和总计数，可以把隐蔽的元数据损坏尽早转化为可定位的断言失败。

6. **本实验尚未完整覆盖的 OS 知识。** Lab2 主要关注物理页管理，尚未深入实现用户页表的动态映射、缺页异常、页面置换、写时复制、交换空间、NUMA、并发分配锁和多核缓存一致性。这些机制同样属于完整内存管理系统的重要组成部分，需要在后续实验或真实内核中继续学习。



## **AI 协作开发的经验**

本次实验中，AI 更适合作为**实现建议与测试补全工具**，而不能替代对底层不变量的理解。若提示词只写“实现伙伴系统”，生成结果往往能完成基本拆分，却容易忽略非二次幂请求的真实占用量、多段非对齐内存、相邻但非伙伴的误合并，以及失败后状态是否保持不变。将上下文、公共接口、元数据语义、边界条件和验收测试写进提示词后，输出才更接近可验证的内核代码。

另一个重要经验是先建立独立测试，再接入完整内核。物理内存错误可能在很久以后才以随机崩溃表现出来，单看“QEMU 能启动”不足以证明算法正确。独立测试通过参考状态逐页核对分配结果，并在每次操作后执行审计，能够更快暴露重复覆盖、计数漂移和链表损坏。完整内核测试则负责验证编译期开关、设备树初始化和统一接口没有集成问题，两者不能互相替代。

最后，AI 给出的代码、命令和报告结论都需要人工验证。特别是测试结果与截图必须来自真实执行，不能根据源码中的 `PASS` 字符串推断已经通过。较可靠的协作流程是：人工明确约束 → AI 生成或审查实现 → 用编译器和断言验证 → 将失败信息反馈给 AI → 人工复核修改 → 保存可复现的命令和输出。这样既能利用 AI 提高效率，也能保证实验结论有证据支撑。



