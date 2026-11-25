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
    tf.gpr.s0 = (uintptr_t)fn;
    tf.gpr.s1 = (uintptr_t)arg;
    tf.status = read_csr(sstatus);
    tf.status &= ~SSTATUS_SIE;
    tf.status |= SSTATUS_SPP | SSTATUS_SPIE;
    tf.epc = (uintptr_t)kernel_thread_entry;
    return do_fork(clone_flags, 0, &tf);
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



//lab5 function2 YOUR CODE update alloc_proc


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

//lab5 function2 YOUR CODE update kernel_thread


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

//lab5 function3 YOUR CODE update copy_mm
// Member C: replace Lab4 kernel-only baseline with user mm cloning.
static int
copy_mm(uint32_t clone_flags, struct proc_struct *proc)
{
    (void)clone_flags;
    assert(proc != NULL);

    /* This Lab4 stage creates kernel threads only; user mm cloning is unsupported. */
    if (current == NULL || current->mm != NULL)
    {
        return -E_UNIMP;
    }
    proc->mm = NULL;
    proc->pgdir = boot_pgdir_pa;
    return 0;
}



//lab5 function2 YOUR CODE


//lab5 function3 YOUR CODE
// Member C: upgrade the migrated kernel-thread creation path.
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


//lab5 function2 YOUR CODE




// Member B: replace Lab4 init_main and implement user_main/kernel_execve/load_icode/do_execve/do_wait.
static int
init_main(void *arg)
{
    cprintf("Member A baseline ready: member B user loader is pending.\n");
    // Keep the protected init process alive until B installs the real lifecycle.
    for (;;) schedule();
    return 0;
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
