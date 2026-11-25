/* Test-only fixture, appended to proc.c in an isolated copy.
 * It can inspect static PCB helpers without publishing test APIs in the kernel.
 * The trusted ELF loader below is NOT Member B's production load_icode.
 */
#include "../../tools/test_lab5.c"
#include <sbi.h>
#include <syscall.h>

static uintptr_t seen_wait_ptr;
static int seen_wait_pid;
int do_wait(int pid, int *store)
{
    seen_wait_pid = pid;
    seen_wait_ptr = (uintptr_t)store;
    return -E_BAD_PROC; // dispatch fixture only, not a wait implementation
}
int do_execve(const char *name, size_t len, unsigned char *binary, size_t size)
{
    assert((uintptr_t)name == 0x1234 && len == 7);
    assert((uintptr_t)binary == 0x5678 && size == 9);
    return -E_UNIMP; // dispatch fixture only, no production ELF loader
}

static void ma_pass(const char *name) { cprintf("[Member A] %s PASS\n", name); }
static struct mm_struct *ma_mm(void)
{
    struct mm_struct *mm = mm_create();
    assert(mm != NULL && setup_pgdir(mm) == 0);
    return mm;
}
static void ma_drop(struct mm_struct *mm)
{
    assert(mm_count(mm) == 0);
    exit_mmap(mm); put_pgdir(mm); mm_destroy(mm);
}
static struct Page *ma_map(struct mm_struct *mm, uintptr_t va, uint32_t flags)
{
    assert(mm_map(mm, va, PGSIZE, flags, NULL) == 0);
    struct Page *page = pgdir_alloc_page(mm->pgdir, va,
        PTE_U | PTE_R | ((flags & VM_WRITE) ? PTE_W : 0));
    assert(page != NULL);
    memset(page2kva(page), 0x35, PGSIZE);
    return page;
}

static int allocation_budget;
static const struct pmm_manager *real_pmm;
static struct Page *ma_limited_alloc(size_t n)
{
    if (allocation_budget <= 0) return NULL;
    allocation_budget--;
    return real_pmm->alloc_pages(n);
}
static struct pmm_manager limited_pmm;
static void ma_limit(int budget)
{
    real_pmm = pmm_manager;
    limited_pmm = *real_pmm;
    limited_pmm.alloc_pages = ma_limited_alloc;
    allocation_budget = budget;
    pmm_manager = &limited_pmm;
}
static void ma_unlimit(void) { pmm_manager = real_pmm; }

static void ma_cow_tests(void)
{
    const uintptr_t va = UTEXT, ro = UTEXT + PGSIZE;
    struct mm_struct *a = ma_mm(), *b = ma_mm(), *c = ma_mm();
    struct Page *original = ma_map(a, va, VM_READ | VM_WRITE);
    struct Page *readonly = ma_map(a, ro, VM_READ);
    assert(dup_mmap_cow(b, a) == 0);
    pte_t *ap = get_pte(a->pgdir, va, 0), *bp = get_pte(b->pgdir, va, 0);
    assert((*ap & PTE_COW) && !(*ap & PTE_W));
    assert(PTE_ADDR(*ap) == PTE_ADDR(*bp) && page_ref(original) == 2);
    assert(!( *get_pte(b->pgdir, ro, 0) & (PTE_COW | PTE_W)));
    assert(cow_resolve(b, ro) == -E_INVAL && page_ref(readonly) == 2);
    ma_pass("COW shared mappings and true read-only protection");

    pte_t before = *bp;
    ma_limit(0);
    int ret = cow_resolve(b, va);
    ma_unlimit();
    assert(ret == -E_NO_MEM && *bp == before && page_ref(original) == 2);
    ma_pass("COW write OOM preserves mapping and references");

    assert(cow_resolve(b, va) == 0);
    struct Page *private = get_page(b->pgdir, va, NULL);
    assert(private != original && page_ref(original) == 1);
    assert(memcmp(page2kva(private), page2kva(original), PGSIZE) == 0);
    *(int *)page2kva(private) = 71;
    assert(*(int *)page2kva(original) != 71);
    assert(cow_resolve(a, va) == 0 && get_page(a->pgdir, va, NULL) == original);
    assert((*ap & PTE_W) && !(*ap & PTE_COW));
    ma_pass("COW copy on shared write and exclusive permission restore");

    assert(dup_mmap_cow(c, a) == 0);
    struct mm_struct *d = ma_mm();
    assert(dup_mmap_cow(d, c) == 0 && page_ref(original) == 3);
    ma_drop(c);
    assert(page_ref(original) == 2);
    assert(cow_resolve(d, va) == 0);
    *(int *)page2kva(get_page(d->pgdir, va, NULL)) = 93;
    assert(*(int *)page2kva(original) != 93);
    ma_drop(d);
    assert(page_ref(original) == 1);
    ma_pass("COW multigeneration isolation and reference release");

    assert(cow_resolve(a, USERBASE) == -E_INVAL);
    assert(cow_resolve(a, KERNBASE) == -E_INVAL);
    assert(cow_prepare_write(a, (uintptr_t)-8, 32) == -E_INVAL);
    // A stack guard must remain inaccessible even if a malicious PTE is present.
    uintptr_t guard = USTACKTOP - 4 * PGSIZE;
    ma_map(a, guard, VM_READ | VM_WRITE | VM_STACK);
    assert(cow_prepare_write(a, guard, 4) == -E_INVAL);
    ma_pass("COW invalid address overflow and stack guard rejection");
    ma_drop(b); ma_drop(a);

    a = ma_mm(); b = ma_mm();
    original = ma_map(a, va, VM_READ | VM_WRITE);
    size_t free_before = nr_free_pages();
    ma_limit(1); // allocate first directory, fail next table
    ret = copy_range_cow(b->pgdir, a->pgdir, va, va + PGSIZE);
    ma_unlimit();
    assert(ret == -E_NO_MEM && nr_free_pages() == free_before);
    assert(page_ref(original) == 1 && (*get_pte(a->pgdir, va, 0) & PTE_W));
    ma_drop(b); ma_drop(a);
    ma_pass("COW clone page-table OOM rollback");

    a = ma_mm(); b = ma_mm();
    original = ma_map(a, va, VM_READ | VM_WRITE);
    ma_map(a, va + PTSIZE, VM_READ | VM_WRITE);
    free_before = nr_free_pages();
    ma_limit(2); // first mapping succeeds; the next leaf table allocation fails
    ret = copy_range_cow(b->pgdir, a->pgdir, va, va + PTSIZE + PGSIZE);
    ma_unlimit();
    assert(ret == -E_NO_MEM && nr_free_pages() == free_before);
    assert(page_ref(original) == 1 && (*get_pte(a->pgdir, va, 0) & PTE_COW));
    assert(cow_resolve(a, va) == 0);
    ma_drop(b); ma_drop(a);
    ma_pass("COW late clone failure rolls back child and leaves safe parent");

    a = ma_mm();
    original = ma_map(a, va, VM_READ | VM_WRITE);
    uintptr_t distant = va + PTSIZE;
    private = ma_map(a, distant, VM_READ | VM_WRITE);
    unmap_range(a->pgdir, va, va + PGSIZE);
    exit_range(a->pgdir, va, va + PGSIZE);
    assert(get_page(a->pgdir, distant, NULL) == private);
    ma_drop(a);
    ma_pass("partial VMA teardown preserves sibling page tables");

    // Warm allocator arenas, then repeated creation must have zero net page loss.
    a = ma_mm(); ma_map(a, va, VM_READ | VM_WRITE);
    b = ma_mm(); assert(dup_mmap_cow(b, a) == 0); ma_drop(b);
    free_before = nr_free_pages();
    for (int i = 0; i < 32; i++) {
        b = ma_mm(); assert(dup_mmap_cow(b, a) == 0);
        assert(cow_resolve(b, va) == 0); ma_drop(b);
    }
    assert(nr_free_pages() == free_before);
    ma_drop(a);
    ma_pass("32 COW clone-write-destroy cycles without page leak");
}

static void ma_syscalls(void)
{
    test_lab5_syscall();
    test_lab5_process_control();
    struct trapframe tf;
    memset(&tf, 0, sizeof(tf));
    struct trapframe *old = current->tf;
    current->tf = &tf;
    uintptr_t invalid[] = {0, SYS_clone, (uintptr_t)-1, 1000000};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
        tf.gpr.a0 = invalid[i]; syscall();
        assert((intptr_t)tf.gpr.a0 == -E_INVAL);
    }
    tf.gpr.a0 = SYS_wait; tf.gpr.a1 = 123; tf.gpr.a2 = 0x12345678;
    syscall();
    assert(seen_wait_pid == 123 && seen_wait_ptr == 0x12345678);
    tf.gpr.a0 = SYS_exec; tf.gpr.a1 = 0x1234; tf.gpr.a2 = 7;
    tf.gpr.a3 = 0x5678; tf.gpr.a4 = 9; syscall();
    assert((intptr_t)tf.gpr.a0 == -E_UNIMP);
    current->tf = old;
    assert(do_kill(initproc->pid) == -E_INVAL && do_kill(-1) == -E_INVAL);
    ma_pass("syscall boundaries argument dispatch and protected init");
}

static void ma_reap(struct proc_struct *proc)
{
    assert(proc != current && proc->state == PROC_ZOMBIE && proc->cptr == NULL);
    unhash_proc(proc); remove_links(proc); put_kstack(proc); kfree(proc);
}
static int ma_exit_worker(void *arg) { return (int)(uintptr_t)arg; }
static int orphan_pid;
static int ma_orphan_worker(void *arg)
{
    orphan_pid = kernel_thread(ma_exit_worker, (void *)66, 0);
    assert(orphan_pid > 0);
    find_proc(orphan_pid)->state = PROC_SLEEPING;
    find_proc(orphan_pid)->wait_state = WT_CHILD;
    return 55;
}
static void ma_process_tests(void)
{
    int pid = kernel_thread(ma_exit_worker, (void *)71, 0);
    struct proc_struct *child = find_proc(pid);
    current->state = PROC_SLEEPING; current->wait_state = WT_CHILD;
    schedule();
    assert(current == initproc && current->state == PROC_RUNNABLE && current->wait_state == 0);
    assert(child->state == PROC_ZOMBIE && child->exit_code == 71);
    assert(do_kill(pid) == -E_INVAL);
    ma_reap(child);
    ma_pass("do_exit zombie status parent wakeup and deferred PCB reclaim");

    struct mm_struct *shared = ma_mm();
    struct Page *shared_page = ma_map(shared, UTEXT, VM_READ | VM_WRITE);
    set_mm_count(shared, 2); // one retained holder plus the exiting worker
    pid = kernel_thread(ma_exit_worker, (void *)72, 0);
    child = find_proc(pid); child->mm = shared; child->pgdir = PADDR(shared->pgdir);
    schedule();
    assert(child->state == PROC_ZOMBIE && child->mm == NULL && mm_count(shared) == 1);
    assert(get_page(shared->pgdir, UTEXT, NULL) == shared_page);
    ma_reap(child); set_mm_count(shared, 0); ma_drop(shared);
    ma_pass("do_exit retains mm while another holder exists");

    pid = kernel_thread(ma_orphan_worker, NULL, 0);
    child = find_proc(pid);
    schedule();
    struct proc_struct *orphan = find_proc(orphan_pid);
    assert(child->state == PROC_ZOMBIE && child->cptr == NULL);
    assert(orphan->parent == initproc && initproc->cptr == orphan);
    assert(orphan->optr == child && child->yptr == orphan);
    assert(do_kill(orphan_pid) == 0 && orphan->state == PROC_RUNNABLE);
    assert(orphan->wait_state == 0 && (orphan->flags & PF_EXITING));
    assert(do_kill(orphan_pid) == -E_INVAL);
    schedule();
    assert(orphan->state == PROC_ZOMBIE);
    ma_reap(orphan); ma_reap(child);
    ma_pass("orphan adoption sibling links and kill interruptible sleeper");
}

// Only trusted fixture ELF data is accepted; this is not a general exec API.
static struct mm_struct *ma_probe_image(uintptr_t *entry)
{
    extern unsigned char _binary_obj___user_member_a_probe_out_start[];
    unsigned char *binary = _binary_obj___user_member_a_probe_out_start;
    struct elfhdr *elf = (void *)binary;
    assert(elf->e_magic == ELF_MAGIC);
    struct mm_struct *mm = ma_mm();
    struct proghdr *ph = (void *)(binary + elf->e_phoff);
    for (size_t i = 0; i < elf->e_phnum; i++) {
        if (ph[i].p_type != ELF_PT_LOAD || ph[i].p_memsz == 0) continue;
        uintptr_t start = ROUNDDOWN(ph[i].p_va, PGSIZE);
        uintptr_t end = ROUNDUP(ph[i].p_va + ph[i].p_memsz, PGSIZE);
        uint32_t flags = VM_READ | ((ph[i].p_flags & ELF_PF_W) ? VM_WRITE : 0) |
                         ((ph[i].p_flags & ELF_PF_X) ? VM_EXEC : 0);
        assert(mm_map(mm, start, end-start, flags, NULL) == 0);
        for (uintptr_t va = start; va < end; va += PGSIZE) {
            struct Page *page = pgdir_alloc_page(mm->pgdir, va, PTE_U | PTE_R |
                ((flags & VM_WRITE) ? PTE_W : 0) | ((flags & VM_EXEC) ? PTE_X : 0));
            assert(page != NULL); memset(page2kva(page), 0, PGSIZE);
            uintptr_t lo = va > ph[i].p_va ? va : ph[i].p_va;
            uintptr_t hi = va + PGSIZE < ph[i].p_va + ph[i].p_filesz ?
                           va + PGSIZE : ph[i].p_va + ph[i].p_filesz;
            if (hi > lo) memcpy((char *)page2kva(page) + lo-va,
                binary + ph[i].p_offset + lo-ph[i].p_va, hi-lo);
        }
    }
    ma_map(mm, USTACKTOP - PGSIZE, VM_READ | VM_WRITE);
    ma_map(mm, 0x01000000, VM_READ | VM_WRITE);
    *entry = elf->e_entry;
    return mm;
}
static void ma_user_tests(void)
{
    uintptr_t entry;
    struct mm_struct *parent = ma_probe_image(&entry), *childmm = ma_mm();
    assert(dup_mmap_cow(childmm, parent) == 0);
    int pid = kernel_thread(ma_exit_worker, NULL, 0);
    struct proc_struct *child = find_proc(pid);
    child->mm = childmm; child->pgdir = PADDR(childmm->pgdir);
    set_mm_count(childmm, 1);
    memset(child->tf, 0, sizeof(*child->tf));
    child->tf->epc = entry; child->tf->gpr.sp = USTACKTOP;
    child->tf->status = (read_csr(sstatus) & ~(SSTATUS_SPP | SSTATUS_SIE)) | SSTATUS_SPIE | SSTATUS_SUM;
    asm volatile(".option push\n.option arch,+zifencei\nfence.i\n.option pop" ::: "memory");
    while (child->state != PROC_ZOMBIE) schedule();
    assert(child->exit_code == 73 && child->mm == NULL);
    assert(*(unsigned *)page2kva(get_page(parent->pgdir, 0x01000000, NULL)) == 0x35353535);
    assert(page_ref(get_page(parent->pgdir, 0x01000000, NULL)) == 1);
    ma_reap(child);
    // Actual supervisor store to active COW user mapping must retry correctly too.
    childmm = ma_mm(); assert(dup_mmap_cow(childmm, parent) == 0);
    current->mm = childmm; current->pgdir = PADDR(childmm->pgdir);
    write_csr(satp, ((uintptr_t)SATP_MODE_SV39 << 60) | (current->pgdir >> PGSHIFT)); flush_tlb();
    *(volatile unsigned *)0x01000000 = 0x12345678;
    assert(*(unsigned *)page2kva(get_page(parent->pgdir, 0x01000000, NULL)) == 0x35353535);
    assert(copy_to_user(childmm, (void *)0x01000004, "COW", 4));
    assert(memcmp((void *)0x01000004, "COW", 4) == 0);
    uintptr_t copy_target = USTACKTOP - PGSIZE + 8;
    assert(*get_pte(childmm->pgdir, copy_target, 0) & PTE_COW);
    assert(copy_to_user(childmm, (void *)copy_target, "COW", 4));
    assert(!(*get_pte(childmm->pgdir, copy_target, 0) & PTE_COW));
    assert(memcmp((void *)copy_target, "COW", 4) == 0);
    assert(memcmp((char *)page2kva(get_page(parent->pgdir, copy_target, NULL)) + 8, "COW", 4) != 0);
    current->mm = NULL; current->pgdir = boot_pgdir_pa;
    write_csr(satp, ((uintptr_t)SATP_MODE_SV39 << 60) | (boot_pgdir_pa >> PGSHIFT)); flush_tlb();
    ma_drop(childmm); ma_drop(parent);
    ma_pass("real U-mode syscall COW store sret yield and exit");
    ma_pass("real S-mode COW store retry and copy_to_user");
}

static int member_a_suite(void)
{
    cprintf("[Member A] isolated fixture; B/C production APIs remain pending\n");
    ma_syscalls(); ma_cow_tests(); ma_process_tests(); ma_user_tests();
    ma_pass("ALL TESTS");
    sbi_shutdown();
    panic("shutdown returned");
}
