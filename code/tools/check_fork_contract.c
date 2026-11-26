/* Member C 2412566. Included only in the disposable proc.c test build. */
extern void *__real_kmalloc(size_t);
extern void __real_kfree(void *);
static void *p4_live[2048];
static int p4_live_count;
void *__wrap_kmalloc(size_t n) {
    void *p = __real_kmalloc(n);
    if (p) {
        int i; for (i = 0; i < 2048 && p4_live[i]; i++);
        assert(i < 2048); p4_live[i] = p; p4_live_count++;
    }
    return p;
}
void __wrap_kfree(void *p) {
    if (p) {
        int i; for (i = 0; i < 2048 && p4_live[i] != p; i++);
        assert(i < 2048); p4_live[i] = NULL; p4_live_count--;
    }
    __real_kfree(p);
}
static int p4_fail_stage, p4_fail_n, p4_hits;
int p4_fail(int stage) {
    if (p4_fail_stage != stage) return 0;
    if (++p4_hits != p4_fail_n) return 0;
    return 1;
}
static void p4_check(void) {
    const uintptr_t text = 0x800000, data = 0xa00000;
    size_t initial = nr_free_pages();
    int initial_live = p4_live_count;
    assert(current->mm == NULL);
    struct mm_struct *mm = mm_create();
    assert(mm && setup_pgdir(mm) == 0);
    assert(mm_map(mm, text, PGSIZE, VM_READ | VM_EXEC, NULL) == 0);
    assert(mm_map(mm, data, PGSIZE, VM_READ | VM_WRITE, NULL) == 0);
    struct Page *code = pgdir_alloc_page(mm->pgdir, text, PTE_U | PTE_R | PTE_X);
    struct Page *page = pgdir_alloc_page(mm->pgdir, data, PTE_U | PTE_R | PTE_W);
    assert(code && page);
    /* RV64: store 77 to data; exit(0). Actual U-mode execution. */
    uint32_t program[] = {0x00a002b7, 0x04d00313, 0x0062a023,
                          0x00100513, 0x00000593, 0x00000073};
    memcpy(page2kva(code), program, sizeof(program));
    *(int *)page2kva(page) = 19;
    mm_count_inc(mm); current->mm = mm; current->pgdir = PADDR(mm->pgdir);
    struct trapframe tf;
    memset(&tf, 0, sizeof(tf)); tf.epc = text;
    tf.gpr.sp = data + PGSIZE;
    tf.status = (read_csr(sstatus) & ~(SSTATUS_SPP | SSTATUS_SIE)) | SSTATUS_SPIE;
    bool check_intr;
    int base_nr = nr_process;
    list_entry_t *base_next = list_next(&proc_list);
    struct proc_struct *base_child = current->cptr;

    /* Every reap must restore resources to the pre-fork state. */
    size_t stable = nr_free_pages();
    int stable_live = p4_live_count;
    for (int round = 0; round < 2; round++) {
        local_intr_save(check_intr);
        int pid = do_fork(0, tf.gpr.sp, &tf);
        assert(pid > 0);
        struct proc_struct *child = find_proc(pid);
        assert(child && child->mm != mm && child->pgdir != current->pgdir);
        pte_t cp = *get_pte(child->mm->pgdir, data, 0);
        pte_t ro = *get_pte(child->mm->pgdir, text, 0);
        assert(pte2page(cp) != page && pte2page(ro) != code);
        assert(memcmp(page2kva(pte2page(cp)), page2kva(page), PGSIZE) == 0);
        assert(memcmp(page2kva(pte2page(ro)), page2kva(code), PGSIZE) == 0);
        assert(page_ref(pte2page(cp)) == 1 && page_ref(page) == 1);
        assert(*(int *)page2kva(pte2page(cp)) == 19);
        assert(!(ro & PTE_W) && (ro & (PTE_R | PTE_X | PTE_U)) == (PTE_R | PTE_X | PTE_U));
        *(int *)page2kva(page) = 23;
        assert(*(int *)page2kva(pte2page(cp)) == 19);
        assert(child->tf->gpr.a0 == 0);
        cprintf("P4 PRIVATE pid=%d parent_mm=%p child_mm=%p roots=%lx/%lx pages=%lx/%lx child_a0=0\n",
                pid, mm, child->mm, current->pgdir, child->pgdir, page2pa(page), page2pa(pte2page(cp)));
        local_intr_restore(check_intr);
        assert(do_wait(pid, NULL) == 0);
        assert(nr_free_pages() == stable && p4_live_count == stable_live);
        assert(*(int *)page2kva(page) == 23);
        *(int *)page2kva(page) = 19;
    }
    /* A real U-mode store to the copied RX page must terminate the child. */
    ((uint32_t *)page2kva(code))[0] = 0x008002b7; /* lui t0,0x800 */
    int ro_pid = do_fork(0, tf.gpr.sp, &tf);
    assert(ro_pid > 0);
    struct proc_struct *ro_child = find_proc(ro_pid);
    while (ro_child->state != PROC_ZOMBIE) schedule();
    assert(ro_child->exit_code == -E_KILLED);
    assert(do_wait(ro_pid, NULL) == 0);
    assert(nr_free_pages() == stable && p4_live_count == stable_live);
    assert(((uint32_t *)page2kva(code))[0] == 0x008002b7);
    ((uint32_t *)page2kva(code))[0] = program[0];
    cprintf("P4 READONLY U-mode store rejected/parent intact PASS\n");
    /* Stage 1 PCB, 2 stack, 3 mm, 4 root, 5 VMA, 6 data, 7 mapping,
       8 second capacity check; later hits exercise partial-copy cleanup. */
    for (int stage = 1; stage <= 10; stage++) {
        int limit = (stage == 5 || stage == 6 || stage == 7 || stage == 10) ? 2 : 1;
        for (int hit = 1; hit <= limit; hit++) {
            p4_fail_stage = stage; p4_fail_n = hit; p4_hits = 0;
            int ret = do_fork(0, tf.gpr.sp, &tf);
            p4_fail_stage = 0;
            assert(ret == (stage == 8 ? -E_NO_FREE_PROC : -E_NO_MEM));
            assert(p4_hits == hit && nr_process == base_nr);
            assert(current->cptr == base_child && list_next(&proc_list) == base_next);
            int listed = 0, hashed = 0;
            for (list_entry_t *le = list_next(&proc_list); le != &proc_list; le = list_next(le)) listed++;
            for (int h = 0; h < HASH_LIST_SIZE; h++)
                for (list_entry_t *le = list_next(&hash_list[h]); le != &hash_list[h]; le = list_next(le)) hashed++;
            assert(listed == base_nr && hashed == listed - 1);
            assert(mm_count(mm) == 1 && mm->mm_lock == 0);
            lock_mm(mm); unlock_mm(mm);
            assert(*(int *)page2kva(page) == 19 && page_ref(page) == 1);
            assert(nr_free_pages() == stable && p4_live_count == stable_live);
            cprintf("P4 FAIL_STAGE=%d HIT=%d ret=%d free=%d table/lock/source/reclaim PASS\n",
                    stage, hit, ret, (int)stable);
        }
    }
    /* CLONE_VM publication failure must undo only the new reference. */
    p4_fail_stage = 8; p4_fail_n = 1; p4_hits = 0;
    assert(do_fork(CLONE_VM, tf.gpr.sp, &tf) == -E_NO_FREE_PROC);
    p4_fail_stage = 0;
    assert(mm_count(mm) == 1 && nr_free_pages() == stable && nr_process == base_nr);
    cprintf("P4 CLONE_VM publication rollback PASS\n");
    local_intr_save(check_intr);
    int pid = do_fork(CLONE_VM, tf.gpr.sp, &tf);
    assert(pid > 0 && find_proc(pid)->mm == mm && mm_count(mm) == 2);
    local_intr_restore(check_intr);
    assert(do_wait(pid, NULL) == 0);
    assert(mm_count(mm) == 1 && *(int *)page2kva(page) == 77);
    assert(nr_free_pages() == stable && p4_live_count == stable_live);
    cprintf("P4 CLONE_VM shared content/ref 1->2->1 real do_exit/do_wait PASS\n");
    local_intr_save(check_intr);
    pid = do_fork(CLONE_VM, tf.gpr.sp, &tf);
    assert(pid > 0 && mm_count(mm) == 2);
    /* Detach the parent before scheduling; child's do_exit drops last reference. */
    current->mm = NULL; current->pgdir = boot_pgdir_pa;
    write_csr(satp, ((uintptr_t)SATP_MODE_SV39 << 60) | (boot_pgdir_pa >> RISCV_PGSHIFT));
    flush_tlb(); assert(mm_count_dec(mm) == 1);
    local_intr_restore(check_intr);
    assert(do_wait(pid, NULL) == 0);
    assert(page_ref(page) == 0 && page_ref(code) == 0);
    assert(nr_free_pages() == initial && nr_process == base_nr);
    assert(p4_live_count == initial_live);
    cprintf("P4 COUNTERS initial_free=%d pre_fork_free=%d final_free=%d metadata_restored=%d\n", (int)initial, (int)stable, (int)nr_free_pages(), p4_live_count);
    cprintf("P4 CLONE_VM last-reference real do_exit/reclaim PASS\nP4 CONTRACT PASS\n");
}
