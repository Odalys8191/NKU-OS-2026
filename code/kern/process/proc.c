#include <proc.h>
#include <kmalloc.h>
#include <string.h>
#include <sync.h>
#include <pmm.h>
#include <error.h>
#include <sched.h>
#include <elf.h>
#include <vmm.h>
#include <trap.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <kmonitor.h>

/* ------------- process/thread mechanism design&implementation -------------
(an simplified Linux process/thread mechanism )
introduction:
  ucore implements a simple process/thread mechanism. process contains the independent memory sapce, at least one threads
for execution, the kernel data(for management), processor state (for context switch), files(in lab6), etc. ucore needs to
manage all these details efficiently. In ucore, a thread is just a special kind of process(share process's memory).
------------------------------
process state       :     meaning               -- reason
    PROC_UNINIT     :   uninitialized           -- alloc_proc
    PROC_SLEEPING   :   sleeping                -- try_free_pages, do_wait, do_sleep
    PROC_RUNNABLE   :   runnable(maybe running) -- proc_init, wakeup_proc,
    PROC_ZOMBIE     :   almost dead             -- do_exit

-----------------------------
process state changing:

  alloc_proc                                 RUNNING
      +                                   +--<----<--+
      +                                   + proc_run +
      V                                   +-->---->--+
PROC_UNINIT -- proc_init/wakeup_proc --> PROC_RUNNABLE -- try_free_pages/do_wait/do_sleep --> PROC_SLEEPING --
                                           A      +                                                           +
                                           |      +--- do_exit --> PROC_ZOMBIE                                +
                                           +                                                                  +
                                           -----------------------wakeup_proc----------------------------------
-----------------------------
process relations
parent:           proc->parent  (proc is children)
children:         proc->cptr    (proc is parent)
older sibling:    proc->optr    (proc is younger sibling)
younger sibling:  proc->yptr    (proc is older sibling)
-----------------------------
related syscall for process:
SYS_exit        : process exit,                           -->do_exit
SYS_fork        : create child process, dup mm            -->do_fork-->wakeup_proc
SYS_wait        : wait process                            -->do_wait
SYS_exec        : after fork, process execute a program   -->load a program and refresh the mm
SYS_clone       : create child thread                     -->do_fork-->wakeup_proc
SYS_yield       : process flag itself need resecheduling, -- proc->need_sched=1, then scheduler will rescheule this process
SYS_sleep       : process sleep                           -->do_sleep
SYS_kill        : kill process                            -->do_kill-->proc->flags |= PF_EXITING
                                                                 -->wakeup_proc-->do_wait-->do_exit
SYS_getpid      : get the process's pid

*/

// the process set's list
list_entry_t proc_list;

#define HASH_SHIFT 10
#define HASH_LIST_SIZE (1 << HASH_SHIFT)
#define pid_hashfn(x) (hash32(x, HASH_SHIFT))

// has list for process set based on pid
static list_entry_t hash_list[HASH_LIST_SIZE];

// idle proc
struct proc_struct *idleproc = NULL;
// init proc
struct proc_struct *initproc = NULL;
// current proc
struct proc_struct *current = NULL;

static int nr_process = 0;

void kernel_thread_entry(void);
void forkrets(struct trapframe *tf);
void switch_to(struct context *from, struct context *to);

//lab4 2412266: migrated baseline; Lab5 module 2/3 owners replace their marked functions.
static int init_main(void *arg);
static int setup_kstack(struct proc_struct *proc);
static void put_kstack(struct proc_struct *proc);
static int get_pid(void);
static void hash_proc(struct proc_struct *proc);
static void forkret(void);

//lab4 2412266: migrated PCB baseline; Lab5 module 2 update belongs to B.
//lab5 function2 2412103 update alloc_proc
static struct proc_struct *
alloc_proc(void)
{
    struct proc_struct *proc = kmalloc(sizeof(struct proc_struct));
    if (proc == NULL)
    {
        return NULL;
    }

    memset(proc, 0, sizeof(*proc));
    proc->state = PROC_UNINIT;
    proc->pid = -1;
    proc->runs = 0;
    proc->kstack = 0;
    proc->need_resched = 0;
    proc->parent = NULL;
    proc->mm = NULL;
    memset(&proc->context, 0, sizeof(proc->context));
    proc->tf = NULL;
    proc->pgdir = boot_pgdir_pa;
    proc->flags = 0;
    memset(proc->name, 0, sizeof(proc->name));
    list_init(&proc->list_link);
    list_init(&proc->hash_link);
    // 新进程尚未退出、等待或接入亲属链表。
    proc->exit_code = 0;
    proc->wait_state = 0;
    proc->cptr = proc->yptr = proc->optr = NULL;
    return proc;
}

//lab4 2412266: migrated address-space and execution-context switch.
void
proc_run(struct proc_struct *proc)
{
    if (proc == current)
    {
        return;
    }

    bool intr_flag;
    local_intr_save(intr_flag);
    struct proc_struct *prev = current;
    current = proc;

    uintptr_t satp = ((uintptr_t)SATP_MODE_SV39 << 60) |
                     ((proc->pgdir >> RISCV_PGSHIFT) & SATP64_PPN);
    write_csr(satp, satp);
    flush_tlb();
    switch_to(&prev->context, &proc->context);

    // Resuming this call also restores this thread's saved interrupt flag.
    local_intr_restore(intr_flag);
}

//lab4 2412266: migrated idle scheduling loop.
void
cpu_idle(void)
{
    while (1)
    {
        if (current->need_resched)
        {
            schedule();
        }
    }
}

//lab4 2412266: migrated kernel-thread baseline; Lab5 update belongs to B.
//lab5 function2 2412103 update kernel_thread
int
kernel_thread(int (*fn)(void *), void *arg, uint32_t clone_flags)
{
    struct trapframe tf;

    if (fn == NULL)
    {
        return -E_INVAL;
    }

    memset(&tf, 0, sizeof(tf));
    asm volatile("mv %0, gp" : "=r"(tf.gpr.gp));
    asm volatile("mv %0, tp" : "=r"(tf.gpr.tp));
    // 入口汇编从 s0、s1 取出函数和参数。
    tf.gpr.s0 = (uintptr_t)fn;
    tf.gpr.s1 = (uintptr_t)arg;
    tf.status = read_csr(sstatus);
    tf.status &= ~SSTATUS_SIE;
    tf.status |= SSTATUS_SPP | SSTATUS_SPIE;
    tf.epc = (uintptr_t)kernel_thread_entry;
    return do_fork(clone_flags | CLONE_VM, 0, &tf);
}

//lab4 2412266: migrated initial trapframe and context construction.
static void
copy_thread(struct proc_struct *proc, uintptr_t stack, struct trapframe *tf)
{
    uintptr_t kstack_top;

    assert(proc != NULL && proc->kstack != 0 && tf != NULL);
    kstack_top = proc->kstack + KSTACKSIZE;
    proc->tf = (struct trapframe *)(kstack_top - sizeof(struct trapframe));
    memcpy(proc->tf, tf, sizeof(struct trapframe));
    proc->tf->gpr.a0 = 0;
    proc->tf->gpr.sp = (stack == 0)
                           ? (uintptr_t)proc->tf
                           : (stack & ~((uintptr_t)16 - 1));

    assert((proc->tf->gpr.sp & 0xF) == 0);
    proc->context.ra = (uintptr_t)forkret;
    proc->context.sp = (uintptr_t)proc->tf;
}





// set_proc_name - set the name of proc
char *
set_proc_name(struct proc_struct *proc, const char *name)
{
    memset(proc->name, 0, sizeof(proc->name));
    for (size_t i = 0; i < PROC_NAME_LEN && name[i]; i++) proc->name[i] = name[i];
    return proc->name;
}

// get_proc_name - get the name of proc
char *
get_proc_name(struct proc_struct *proc)
{
    static char name[PROC_NAME_LEN + 1];
    memset(name, 0, sizeof(name));
    return memcpy(name, proc->name, PROC_NAME_LEN);
}

// set_links - set the relation links of process
static void
set_links(struct proc_struct *proc)
{
    list_add(&proc_list, &(proc->list_link));
    proc->yptr = NULL;
    if ((proc->optr = proc->parent->cptr) != NULL)
    {
        proc->optr->yptr = proc;
    }
    proc->parent->cptr = proc;
    nr_process++;
}

// remove_links - clean the relation links of process
static void
remove_links(struct proc_struct *proc)
{
    list_del(&(proc->list_link));
    if (proc->optr != NULL)
    {
        proc->optr->yptr = proc->yptr;
    }
    if (proc->yptr != NULL)
    {
        proc->yptr->optr = proc->optr;
    }
    else
    {
        proc->parent->cptr = proc->optr;
    }
    nr_process--;
}

// get_pid - alloc a unique pid for process
static int
get_pid(void)
{
    static_assert(MAX_PID > MAX_PROCESS);
    struct proc_struct *proc;
    list_entry_t *list = &proc_list, *le;
    static int next_safe = MAX_PID, last_pid = MAX_PID;
    if (++last_pid >= MAX_PID)
    {
        last_pid = 1;
        goto inside;
    }
    if (last_pid >= next_safe)
    {
    inside:
        next_safe = MAX_PID;
    repeat:
        le = list;
        while ((le = list_next(le)) != list)
        {
            proc = le2proc(le, list_link);
            if (proc->pid == last_pid)
            {
                if (++last_pid >= next_safe)
                {
                    if (last_pid >= MAX_PID)
                    {
                        last_pid = 1;
                    }
                    next_safe = MAX_PID;
                    goto repeat;
                }
            }
            else if (proc->pid > last_pid && next_safe > proc->pid)
            {
                next_safe = proc->pid;
            }
        }
    }
    return last_pid;
}






// forkret -- the first kernel entry point of a new thread/process
// NOTE: the addr of forkret is setted in copy_thread function
//       after switch_to, the current proc will execute here.
static void
forkret(void)
{
    forkrets(current->tf);
}

// hash_proc - add proc into proc hash_list
static void
hash_proc(struct proc_struct *proc)
{
    list_add(hash_list + pid_hashfn(proc->pid), &(proc->hash_link));
}

// unhash_proc - delete proc from proc hash_list
static void
unhash_proc(struct proc_struct *proc)
{
    list_del(&(proc->hash_link));
}

// find_proc - find proc frome proc hash_list according to pid
struct proc_struct *
find_proc(int pid)
{
    if (0 < pid && pid < MAX_PID)
    {
        list_entry_t *list = hash_list + pid_hashfn(pid), *le = list;
        while ((le = list_next(le)) != list)
        {
            struct proc_struct *proc = le2proc(le, hash_link);
            if (proc->pid == pid)
            {
                return proc;
            }
        }
    }
    return NULL;
}



// setup_kstack - alloc pages with size KSTACKPAGE as process kernel stack
static int
setup_kstack(struct proc_struct *proc)
{
    struct Page *page = alloc_pages(KSTACKPAGE);
    if (page != NULL)
    {
        proc->kstack = (uintptr_t)page2kva(page);
        return 0;
    }
    return -E_NO_MEM;
}

// put_kstack - free the memory space of process kernel stack
static void
put_kstack(struct proc_struct *proc)
{
    free_pages(kva2page((void *)(proc->kstack)), KSTACKPAGE);
}

// setup_pgdir - alloc one page as PDT
static int
setup_pgdir(struct mm_struct *mm)
{
    struct Page *page;
    if ((page = alloc_page()) == NULL)
    {
        return -E_NO_MEM;
    }
    pde_t *pgdir = page2kva(page);
    memcpy(pgdir, boot_pgdir_va, PGSIZE);

    mm->pgdir = pgdir;
    return 0;
}

// put_pgdir - free the memory space of PDT
static void
put_pgdir(struct mm_struct *mm)
{
    free_page(kva2page(mm->pgdir));
}

//lab5 function3 2412566 update copy_mm
// Member C (2412566): ordinary fork.
static int
copy_mm(uint32_t clone_flags, struct proc_struct *proc)
{
    assert(proc != NULL && current != NULL);

    struct mm_struct *oldmm = current->mm, *mm;
    int ret;
    proc->mm = NULL;
    proc->pgdir = boot_pgdir_pa;
    if (oldmm == NULL)
    {
        return 0;
    }
    if (clone_flags & CLONE_VM)
    {
        mm = oldmm;
    }
    else
    {
        if ((mm = mm_create()) == NULL)
        {
            return -E_NO_MEM;
        }
        if ((ret = setup_pgdir(mm)) != 0)
        {
            mm_destroy(mm);
            return ret;
        }
        lock_mm(oldmm);
        /* A's future COW integration point; see ADR 0008 cleanup contract. */
        ret = dup_mmap(mm, oldmm);
        unlock_mm(oldmm);
        if (ret != 0)
        {
            exit_mmap(mm);
            put_pgdir(mm);
            mm_destroy(mm);
            return ret;
        }
    }
    mm_count_inc(mm);
    proc->mm = mm;
    proc->pgdir = PADDR(mm->pgdir);
    return 0;
}



//lab5 function2 2412103


//lab5 function3 2412566
// Member C: prepare privately, then publish exactly once with interrupts off.
int
do_fork(uint32_t clone_flags, uintptr_t stack, struct trapframe *tf)
{
    struct proc_struct *proc;
    bool intr_flag;
    int ret;

    if (current == NULL || tf == NULL)
    {
        return -E_INVAL;
    }

    local_intr_save(intr_flag);
    if (nr_process >= MAX_PROCESS)
    {
        local_intr_restore(intr_flag);
        return -E_NO_FREE_PROC;
    }
    local_intr_restore(intr_flag);

    proc = alloc_proc();
    if (proc == NULL)
    {
        return -E_NO_MEM;
    }
    proc->parent = current;

    if ((ret = setup_kstack(proc)) != 0)
    {
        goto failed;
    }
    if ((ret = copy_mm(clone_flags, proc)) != 0)
    {
        goto failed;
    }
    copy_thread(proc, stack, tf);
    if (current->mm != NULL)
    {
        /* sys_fork supplies a valid user sp; preserve it without rounding. */
        proc->tf->gpr.sp = stack;
    }

    /* Serialize the capacity check and publication of the fully initialized PCB. */
    local_intr_save(intr_flag);
    if (nr_process >= MAX_PROCESS)
    {
        local_intr_restore(intr_flag);
        ret = -E_NO_FREE_PROC;
        goto failed;
    }
    proc->pid = get_pid();
    hash_proc(proc);
    set_links(proc);
    wakeup_proc(proc);
    local_intr_restore(intr_flag);
    return proc->pid;

failed:
    if (proc->mm != NULL)
    {
        struct mm_struct *mm = proc->mm;
        proc->mm = NULL;
        proc->pgdir = boot_pgdir_pa;
        if (mm_count_dec(mm) == 0)
        {
            exit_mmap(mm);
            put_pgdir(mm);
            mm_destroy(mm);
        }
    }
    if (proc->kstack != 0)
    {
        put_kstack(proc);
    }
    kfree(proc);
    return ret;
}


//lab5 function4 2412266
int do_exit(int error_code)
{
    if (current == idleproc || current == initproc)
        panic("do_exit: idle/init must not exit");

    bool intr_flag;
    local_intr_save(intr_flag);
    struct mm_struct *mm = current->mm;
    if (mm != NULL) {
        // Stop using user page tables before releasing them.
        write_csr(satp, ((uintptr_t)SATP_MODE_SV39 << 60) |
                        (boot_pgdir_pa >> RISCV_PGSHIFT));
        flush_tlb();
        current->mm = NULL;
        current->pgdir = boot_pgdir_pa;
        assert(mm_count(mm) > 0);
        if (mm_count_dec(mm) == 0) {
            exit_mmap(mm);
            put_pgdir(mm);
            mm_destroy(mm);
        }
    }
    current->exit_code = error_code;
    current->state = PROC_ZOMBIE;
    current->wait_state = 0;
    struct proc_struct *parent = current->parent;
    if (parent != NULL && parent->wait_state == WT_CHILD)
        wakeup_proc(parent);

    // Transfer all children to init, maintaining both sibling directions.
    while (current->cptr != NULL) {
        struct proc_struct *child = current->cptr;
        current->cptr = child->optr;
        if (current->cptr != NULL) current->cptr->yptr = NULL;
        child->parent = initproc;
        child->yptr = NULL;
        child->optr = initproc->cptr;
        if (child->optr != NULL) child->optr->yptr = child;
        initproc->cptr = child;
        if (child->state == PROC_ZOMBIE && initproc->wait_state == WT_CHILD)
            wakeup_proc(initproc);
    }
    // wait, not exit, reclaims this PCB and the still-active kernel stack.
    schedule();
    local_intr_restore(intr_flag);
    panic("do_exit: zombie resumed");
}

int do_yield(void)
{
    current->need_resched = 1;
    return 0;
}

int do_kill(int pid)
{
    bool intr_flag;
    local_intr_save(intr_flag);
    struct proc_struct *proc = find_proc(pid);
    int ret = -E_INVAL;
    if (proc != NULL && proc != idleproc && proc != initproc &&
        proc->state != PROC_ZOMBIE && !(proc->flags & PF_EXITING)) {
        proc->flags |= PF_EXITING;
        if (proc->wait_state & WT_INTERRUPTED) wakeup_proc(proc);
        ret = 0;
    }
    local_intr_restore(intr_flag);
    return ret;
}


#define __KERNEL_EXECVE(name, binary, size) ({           \
    cprintf("kernel_execve: pid = %d, name = \"%s\".\n", \
            current->pid, name);                         \
    kernel_execve(name, binary, (size_t)(size));         \
})

#define KERNEL_EXECVE(x) ({                                    \
    extern unsigned char _binary_obj___user_##x##_out_start[], \
        _binary_obj___user_##x##_out_size[];                   \
    __KERNEL_EXECVE(#x, _binary_obj___user_##x##_out_start,    \
                    _binary_obj___user_##x##_out_size);        \
})

#define __KERNEL_EXECVE2(x, xstart, xsize) ({   \
    extern unsigned char xstart[], xsize[];     \
    __KERNEL_EXECVE(#x, xstart, (size_t)xsize); \
})

#define KERNEL_EXECVE2(x, xstart, xsize) __KERNEL_EXECVE2(x, xstart, xsize)


//lab5 function2 2412103
static int
kernel_execve(const char *name, unsigned char *binary, size_t size)
{
    size_t len = strlen(name);
    register uintptr_t a0 asm("a0") = SYS_exec;
    register uintptr_t a1 asm("a1") = (uintptr_t)name;
    register uintptr_t a2 asm("a2") = len;
    register uintptr_t a3 asm("a3") = (uintptr_t)binary;
    register uintptr_t a4 asm("a4") = size;

    // S 态 ecall 用于 SBI，内核 exec 通过断点进入本地分发。
    asm volatile(".option push\n.option norvc\nebreak\n.option pop"
                 : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a4) : "memory");
    return (int)a0;
}

static bool
exec_read(void *dst, const void *src, size_t len)
{
    struct mm_struct *mm = current->mm;
    if (len == 0)
        return 1;
    if (!user_mem_check(mm, (uintptr_t)src, len, 0))
        return 0;
    if (mm == NULL)
        return copy_from_user(NULL, dst, src, len, 0);

    // 通过物理页别名读取，缺页或不可读映射直接返回错误。
    uintptr_t addr = (uintptr_t)src;
    while (len != 0) {
        size_t chunk = PGSIZE - (addr % PGSIZE);
        if (chunk > len)
            chunk = len;
        pte_t *pte = get_pte(mm->pgdir, addr, 0);
        if (pte == NULL || (*pte & (PTE_V | PTE_U | PTE_R)) != (PTE_V | PTE_U | PTE_R))
            return 0;
        memcpy(dst, (char *)page2kva(pte2page(*pte)) + addr % PGSIZE, chunk);
        addr += chunk;
        dst = (char *)dst + chunk;
        len -= chunk;
    }
    return 1;
}

static int
load_icode(unsigned char *binary, size_t size)
{
    struct elfhdr elf;
    if (size < sizeof(elf))
        return -E_INVAL_ELF;
    if (!exec_read(&elf, binary, sizeof(elf)))
        return -E_INVAL;
    // ELF64、小端、当前 ELF 版本、RISC-V 可执行文件。
    if (elf.e_magic != ELF_MAGIC || elf.e_elf[0] != 2 ||
        elf.e_elf[1] != 1 || elf.e_elf[2] != 1 || elf.e_type != 2 ||
        elf.e_machine != 243 || elf.e_version != 1 ||
        elf.e_ehsize != sizeof(elf) || elf.e_phentsize != sizeof(struct proghdr) ||
        elf.e_phoff > size || elf.e_phnum > (size - elf.e_phoff) / sizeof(struct proghdr))
        return -E_INVAL_ELF;

    struct mm_struct *mm = mm_create();
    if (mm == NULL)
        return -E_NO_MEM;
    int ret = setup_pgdir(mm);
    if (ret != 0)
        goto destroy_mm;

    const uintptr_t stack_bottom = USTACKTOP - USTACKSIZE;
    bool entry_valid = 0;
    for (size_t i = 0; i < elf.e_phnum; i++) {
        struct proghdr ph;
        ret = -E_INVAL_ELF;
        if (!exec_read(&ph, binary + elf.e_phoff + i * sizeof(ph), sizeof(ph))) {
            ret = -E_INVAL;
            goto destroy_pgdir;
        }
        if (ph.p_type != ELF_PT_LOAD)
            continue;
        if (ph.p_filesz > ph.p_memsz || ph.p_offset > size ||
            ph.p_filesz > size - ph.p_offset)
            goto destroy_pgdir;
        if (ph.p_memsz == 0)
            continue;
        if (ph.p_va < USERBASE || ph.p_va >= stack_bottom ||
            ph.p_memsz > stack_bottom - ph.p_va)
            goto destroy_pgdir;

        uintptr_t end = ph.p_va + ph.p_memsz;
        uintptr_t start_page = ROUNDDOWN(ph.p_va, PGSIZE);
        uintptr_t end_page = ROUNDUP(end, PGSIZE);
        // mm_map 的插入路径要求页对齐后的区间互不重叠。
        list_entry_t *head = &mm->mmap_list;
        for (list_entry_t *le = list_next(head); le != head; le = list_next(le)) {
            struct vma_struct *vma = le2vma(le, list_link);
            if (start_page < vma->vm_end && vma->vm_start < end_page)
                goto destroy_pgdir;
        }

        uint32_t vm_flags = 0, perm = PTE_U;
        if (ph.p_flags & ELF_PF_R) {
            vm_flags |= VM_READ;
            perm |= PTE_R;
        }
        if (ph.p_flags & ELF_PF_W) {
            vm_flags |= VM_WRITE;
            perm |= PTE_W | PTE_R;
        }
        if (ph.p_flags & ELF_PF_X) {
            vm_flags |= VM_EXEC;
            perm |= PTE_X;
            if (ph.p_va <= elf.e_entry && elf.e_entry < end)
                entry_valid = 1;
        }
        if (vm_flags == 0)
            goto destroy_pgdir;
        ret = mm_map(mm, ph.p_va, ph.p_memsz, vm_flags, NULL);
        if (ret != 0)
            goto destroy_pgdir;

        uintptr_t file_end = ph.p_va + ph.p_filesz;
        for (uintptr_t addr = start_page; addr < end_page; addr += PGSIZE) {
            struct Page *page = pgdir_alloc_page(mm->pgdir, addr, perm);
            if (page == NULL) {
                ret = -E_NO_MEM;
                goto destroy_pgdir;
            }
            char *data = page2kva(page);
            // 整页清零覆盖 BSS 和段边缘，再复制本页的文件数据。
            memset(data, 0, PGSIZE);
            uintptr_t lo = addr < ph.p_va ? ph.p_va : addr;
            uintptr_t hi = addr + PGSIZE < file_end ? addr + PGSIZE : file_end;
            if (lo < hi && !exec_read(data + (lo - addr),
                                     binary + ph.p_offset + (lo - ph.p_va), hi - lo)) {
                ret = -E_INVAL;
                goto destroy_pgdir;
            }
        }
    }

    if (!entry_valid || (elf.e_entry & 1)) {
        ret = -E_INVAL_ELF;
        goto destroy_pgdir;
    }
    ret = mm_map(mm, stack_bottom, USTACKSIZE, VM_READ | VM_WRITE | VM_STACK, NULL);
    if (ret != 0)
        goto destroy_pgdir;
    // 本实验未提供按需分配，预映射可用栈页并留出底部保护页。
    for (uintptr_t addr = stack_bottom + PGSIZE; addr < USTACKTOP; addr += PGSIZE) {
        struct Page *page = pgdir_alloc_page(mm->pgdir, addr, PTE_U | PTE_R | PTE_W);
        if (page == NULL) {
            ret = -E_NO_MEM;
            goto destroy_pgdir;
        }
        memset(page2kva(page), 0, PGSIZE);
    }

    // 只在完整加载后切换地址空间，再释放旧页表。
    bool intr_flag;
    local_intr_save(intr_flag);
    struct mm_struct *old_mm = current->mm;
    mm_count_inc(mm);
    current->mm = mm;
    current->pgdir = PADDR(mm->pgdir);
    asm volatile("fence.i" ::: "memory");
    write_csr(satp, ((uintptr_t)SATP_MODE_SV39 << 60) |
                    ((current->pgdir >> RISCV_PGSHIFT) & SATP64_PPN));
    flush_tlb();
    struct trapframe *tf = current->tf;
    uintptr_t status = tf->status;
    memset(tf, 0, sizeof(*tf));
    tf->gpr.sp = USTACKTOP;
    tf->epc = elf.e_entry;
    tf->status = (status & ~(SSTATUS_SPP | SSTATUS_SIE)) | SSTATUS_SPIE;
    if (old_mm != NULL && mm_count_dec(old_mm) == 0) {
        exit_mmap(old_mm);
        put_pgdir(old_mm);
        mm_destroy(old_mm);
    }
    local_intr_restore(intr_flag);
    return 0;

destroy_pgdir:
    exit_mmap(mm);
    put_pgdir(mm);
destroy_mm:
    mm_destroy(mm);
    return ret;
}

int
do_execve(const char *name, size_t len, unsigned char *binary, size_t size)
{
    char buffer[PROC_NAME_LEN + 1] = {0};
    if (len > PROC_NAME_LEN)
        len = PROC_NAME_LEN;
    if (name == NULL || binary == NULL || size == 0 ||
        !user_mem_check(current->mm, (uintptr_t)binary, size, 0) ||
        !exec_read(buffer, name, len))
        return -E_INVAL;

    int ret = load_icode(binary, size);
    if (ret == 0)
        set_proc_name(current, buffer);
    return ret;
}

int
do_wait(int pid, int *code_store)
{
    if (pid < 0)
        return -E_BAD_PROC;
    if (code_store != NULL &&
        !user_mem_check(current->mm, (uintptr_t)code_store, sizeof(*code_store), 1))
        return -E_INVAL;

    bool intr_flag;
    local_intr_save(intr_flag);
    for (;;) {
        if (current->flags & PF_EXITING)
            do_exit(-E_KILLED);

        bool has_child = 0;
        struct proc_struct *child = pid == 0 ? current->cptr : find_proc(pid);
        for (; child != NULL; child = pid == 0 ? child->optr : NULL) {
            if (child->parent != current)
                break;
            has_child = 1;
            if (child->state != PROC_ZOMBIE)
                continue;

            // 写回失败时保留僵尸进程，允许再次等待。
            if (code_store != NULL &&
                !copy_to_user(current->mm, code_store, &child->exit_code, sizeof(*code_store))) {
                local_intr_restore(intr_flag);
                return -E_INVAL;
            }
            unhash_proc(child);
            remove_links(child);
            local_intr_restore(intr_flag);
            put_kstack(child);
            kfree(child);
            return 0;
        }
        if (!has_child) {
            local_intr_restore(intr_flag);
            return -E_BAD_PROC;
        }

        // 关中断完成检查和休眠登记，避免丢失子进程退出通知。
        current->state = PROC_SLEEPING;
        current->wait_state = WT_CHILD;
        schedule();
    }
}

static int
user_main(void *arg)
{
    (void)arg;
#if defined(TEST)
    return KERNEL_EXECVE2(TEST, TESTSTART, TESTSIZE);
#else
    return KERNEL_EXECVE(exit);
#endif
}

#ifdef LAB5_COW_CHECK
//lab5 function2 2412103：独立状态模型，映射归属为只读源页或独占副本。
static void
check_cow_model(void)
{
    struct {
        bool private_copy;
        bool writable;
    } mappings[] = {{0, 0}, {1, 0}};
    unsigned int source_refs = 2, private_refs = 0;

    // 归属已经更新，缓存的引用计数仍是旧值。
    assert(source_refs == 2 && private_refs == 0 && !mappings[1].writable);
    source_refs = private_refs = 0;
    for (size_t i = 0; i < sizeof(mappings) / sizeof(mappings[0]); i++) {
        // 从映射归属重建引用计数和写权限。
        mappings[i].writable = mappings[i].private_copy;
        if (mappings[i].private_copy)
            private_refs++;
        else
            source_refs++;
    }
    assert(source_refs == 1 && private_refs == 1);
    assert(!mappings[0].writable && mappings[1].writable);
}

static void
check_cow(void)
{
    size_t free_before = nr_free_pages();
    const uintptr_t rw = USERBASE, ro = USERBASE + PGSIZE;
    struct mm_struct *spaces[3];
    for (size_t i = 0; i < 3; i++) {
        spaces[i] = mm_create();
        assert(spaces[i] != NULL && setup_pgdir(spaces[i]) == 0);
    }
    struct mm_struct *parent = spaces[0];
    assert(mm_map(parent, rw, PGSIZE, VM_READ | VM_WRITE, NULL) == 0);
    assert(mm_map(parent, ro, PGSIZE, VM_READ, NULL) == 0);
    struct Page *shared = pgdir_alloc_page(parent->pgdir, rw, PTE_U | PTE_R | PTE_W);
    struct Page *readonly = pgdir_alloc_page(parent->pgdir, ro, PTE_U | PTE_R);
    assert(shared != NULL && readonly != NULL);
    *(char *)page2kva(shared) = 'P';

    // 连续两次克隆，检查已经标记 COW 的页仍共享同一个物理页。
    for (size_t i = 1; i < 3; i++)
        assert(dup_mmap_cow(spaces[i], parent) == 0);
    assert(page_ref(shared) == 3 && page_ref(readonly) == 3);
    for (size_t i = 0; i < 3; i++) {
        pte_t writable_pte = *get_pte(spaces[i]->pgdir, rw, 0);
        pte_t readonly_pte = *get_pte(spaces[i]->pgdir, ro, 0);
        assert(pte2page(writable_pte) == shared);
        assert((writable_pte & (PTE_COW | PTE_W)) == PTE_COW);
        assert(pte2page(readonly_pte) == readonly);
        assert(!(readonly_pte & (PTE_COW | PTE_W)));
        assert(cow_resolve(spaces[i], ro) == -E_INVAL);
    }

    assert(cow_resolve(spaces[1], rw) == 0);
    struct Page *child = pte2page(*get_pte(spaces[1]->pgdir, rw, 0));
    assert(child != shared && *(char *)page2kva(child) == 'P');
    *(char *)page2kva(child) = 'C';
    assert(cow_resolve(parent, rw) == 0);
    struct Page *private_parent = pte2page(*get_pte(parent->pgdir, rw, 0));
    assert(private_parent != shared && private_parent != child);
    *(char *)page2kva(private_parent) = 'N';
    assert(*(char *)page2kva(child) == 'C' && *(char *)page2kva(shared) == 'P');
    assert(page_ref(shared) == 1 && cow_resolve(spaces[2], rw) == 0);
    assert(pte2page(*get_pte(spaces[2]->pgdir, rw, 0)) == shared);

    for (size_t i = 0; i < 3; i++) {
        exit_mmap(spaces[i]);
        put_pgdir(spaces[i]);
        mm_destroy(spaces[i]);
    }
    assert(page_ref(shared) == 0 && page_ref(readonly) == 0);
    assert(page_ref(child) == 0 && page_ref(private_parent) == 0);
    assert(nr_free_pages() == free_before);
    check_cow_model();
}
#endif

static int
init_main(void *arg)
{
    (void)arg;
#ifdef LAB5_COW_CHECK
    check_cow();
#endif
    // 在创建用户进程前记录基线，回收所有后代后核对。
    size_t free_pages_before = nr_free_pages();
    size_t allocated_before = kallocated();
    int pid = kernel_thread(user_main, NULL, 0);
    if (pid < 0)
        panic("cannot create user launcher: %e", pid);
    while (do_wait(0, NULL) == 0)
        schedule();

    assert(initproc->cptr == NULL && nr_process == 2);
    cprintf("all user-mode processes have quit.\n");
    assert(nr_free_pages() == free_pages_before);
    assert(kallocated() == allocated_before);
    cprintf("init check memory pass.\n");
    kmonitor(NULL);
    for (;;)
        schedule();
}

//lab4 2412266: migrated idle/init creation, pending B's user startup path.
void
proc_init(void)
{
    size_t i;

    list_init(&proc_list);
    for (i = 0; i < HASH_LIST_SIZE; i++)
    {
        list_init(&hash_list[i]);
    }
    nr_process = 0;
    idleproc = initproc = current = NULL;

    idleproc = alloc_proc();
    if (idleproc == NULL)
    {
        panic("cannot allocate idle process");
    }
    idleproc->pid = 0;
    idleproc->state = PROC_RUNNABLE;
    idleproc->kstack = (uintptr_t)bootstack;
    idleproc->need_resched = 1;
    set_proc_name(idleproc, "idle");
    list_add(&proc_list, &idleproc->list_link);
    nr_process++;
    current = idleproc;

    int pid = kernel_thread(init_main, (void *)"Hello world!!", 0);
    if (pid < 0 || (initproc = find_proc(pid)) == NULL)
    {
        panic("cannot create init process");
    }
    set_proc_name(initproc, "init");
    assert(initproc->pid == 1 && initproc->parent == idleproc);
}
