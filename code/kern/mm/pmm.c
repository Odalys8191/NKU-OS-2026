#include <default_pmm.h>
#include <defs.h>
#include <error.h>
#include <kmalloc.h>
#include <memlayout.h>
#include <mmu.h>
#include <pmm.h>
#include <sbi.h>
#include <dtb.h>
#include <stdio.h>
#include <string.h>
#include <sync.h>
#include <vmm.h>
#include <riscv.h>
#include <proc.h>

// virtual address of physical page array
struct Page *pages;
// amount of physical memory (in pages)
size_t npage = 0;
// The kernel image is mapped at VA=KERNBASE and PA=info.base
uint_t va_pa_offset;
// memory starts at 0x80000000 in RISC-V
const size_t nbase = DRAM_BASE / PGSIZE;

// virtual address of boot-time page directory
pde_t *boot_pgdir_va = NULL;
// physical address of boot-time page directory
uintptr_t boot_pgdir_pa;

// physical memory management
const struct pmm_manager *pmm_manager;

static void check_alloc_page(void);
static void check_pgdir(void);
static void check_boot_pgdir(void);

// init_pmm_manager - initialize a pmm_manager instance
static void init_pmm_manager(void)
{
    pmm_manager = &default_pmm_manager;
    cprintf("memory management: %s\n", pmm_manager->name);
    pmm_manager->init();
}

// init_memmap - call pmm->init_memmap to build Page struct for free memory
static void init_memmap(struct Page *base, size_t n)
{
    pmm_manager->init_memmap(base, n);
}

// alloc_pages - call pmm->alloc_pages to allocate a continuous n*PAGESIZE
// memory
struct Page *alloc_pages(size_t n)
{
    struct Page *page = NULL;
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        page = pmm_manager->alloc_pages(n);
    }
    local_intr_restore(intr_flag);
    return page;
}

// free_pages - call pmm->free_pages to free a continuous n*PAGESIZE memory
void free_pages(struct Page *base, size_t n)
{
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        pmm_manager->free_pages(base, n);
    }
    local_intr_restore(intr_flag);
}

// nr_free_pages - call pmm->nr_free_pages to get the size (nr*PAGESIZE)
// of current free memory
size_t nr_free_pages(void)
{
    size_t ret;
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        ret = pmm_manager->nr_free_pages();
    }
    local_intr_restore(intr_flag);
    return ret;
}

//lab2 2412266: migrated DTB memory and Page-descriptor initialization.
static void page_init(void) {
    const uint64_t max_addr = ~(uint64_t)0;
    uint64_t mem_base = get_memory_base();
    uint64_t mem_size = get_memory_size();
    uint64_t mem_end, managed_start, managed_end;
    uint64_t kernel_end_pa, pages_pa, pages_end_pa;
    uint64_t free_start_pa;
    uintptr_t kernel_end_va, pages_va;
    size_t page_count, pages_bytes, i;
    extern char end[];

    if (mem_base == 0 || mem_size == 0) {
        panic("page_init: invalid DTB memory range (base=0x%016lx, size=0x%016lx)",
              (unsigned long)mem_base, (unsigned long)mem_size);
    }
    if (mem_size > max_addr - mem_base) {
        panic("page_init: DTB memory range overflows (base=0x%016lx, size=0x%016lx)",
              (unsigned long)mem_base, (unsigned long)mem_size);
    }
    mem_end = mem_base + mem_size;
    cprintf("DTB physical memory: [0x%016lx, 0x%016lx) (size 0x%016lx)\n",
            (unsigned long)mem_base, (unsigned long)mem_end,
            (unsigned long)mem_size);

    /* 链接时的地址布局和启动页表必须与框架规定的映射一致。 */
    if ((KERNTOP - PHYSICAL_MEMORY_OFFSET) <= DRAM_BASE ||
        (KERNBASE - PHYSICAL_MEMORY_OFFSET) < DRAM_BASE ||
        KERNBASE < PHYSICAL_MEMORY_OFFSET) {
        panic("page_init: unsupported physical/virtual memory layout");
    }
    va_pa_offset = PHYSICAL_MEMORY_OFFSET;

    managed_start = mem_base > DRAM_BASE ? mem_base : DRAM_BASE;
    managed_end = mem_end < (KERNTOP - PHYSICAL_MEMORY_OFFSET) ? mem_end : (KERNTOP - PHYSICAL_MEMORY_OFFSET);
    if (managed_start >= managed_end ||
        mem_base > (KERNBASE - PHYSICAL_MEMORY_OFFSET) || mem_end <= (KERNBASE - PHYSICAL_MEMORY_OFFSET)) {
        panic("page_init: DTB memory range is incompatible with the kernel layout");
    }

    kernel_end_va = (uintptr_t)end;
    if (kernel_end_va < KERNBASE || kernel_end_va < va_pa_offset) {
        panic("page_init: invalid kernel end address 0x%016lx",
              (unsigned long)kernel_end_va);
    }
    kernel_end_pa = (uint64_t)kernel_end_va - va_pa_offset;
    if (kernel_end_pa > managed_end || kernel_end_pa > mem_end) {
        panic("page_init: memory range does not contain the kernel (kernel end=0x%016lx)",
              (unsigned long)kernel_end_pa);
    }

    /* npage 是物理页号的上界，该上界本身不在管理范围内。 */
    npage = (size_t)(managed_end / PGSIZE);
    if (npage <= nbase) {
        panic("page_init: no complete physical pages in the supported range");
    }
    page_count = npage - nbase;
    if ((uint64_t)page_count > max_addr / sizeof(struct Page)) {
        panic("page_init: Page descriptor array size overflows");
    }
    pages_bytes = page_count * sizeof(struct Page);

    /* 将页描述数组放在内核末尾之后，并按页大小对齐。 */
    if ((uint64_t)kernel_end_va > max_addr - (PGSIZE - 1)) {
        panic("page_init: kernel end alignment overflows");
    }
    pages_va = (kernel_end_va + PGSIZE - 1) & ~((uintptr_t)PGSIZE - 1);
    if ((uint64_t)pages_va < va_pa_offset) {
        panic("page_init: invalid Page array virtual address");
    }
    pages_pa = (uint64_t)pages_va - va_pa_offset;
    if ((uint64_t)pages_va > max_addr - (uint64_t)pages_bytes ||
        pages_pa > max_addr - (uint64_t)pages_bytes) {
        panic("page_init: Page descriptor array address overflows");
    }
    pages_end_pa = pages_pa + pages_bytes;
    if (pages_pa < kernel_end_pa || pages_end_pa > managed_end ||
        pages_end_pa > mem_end) {
        panic("page_init: memory range cannot contain the Page descriptor array");
    }

    pages = (struct Page *)pages_va;
    for (i = 0; i < page_count; i++) {
        pages[i].ref = 0;
        pages[i].flags = 0;
        pages[i].property = 0;
        list_init(&pages[i].page_link);
        SetPageReserved(&pages[i]);
    }

    /* 不将不足一页的区域或低于设备树所报内存起址的区域登记为空闲页。 */
    if (pages_end_pa > max_addr - (PGSIZE - 1) ||
        managed_start > max_addr - (PGSIZE - 1)) {
        panic("page_init: free range alignment overflows");
    }
    free_start_pa = (pages_end_pa + PGSIZE - 1) & ~((uint64_t)PGSIZE - 1);
    if (free_start_pa < managed_start) {
        free_start_pa = (managed_start + PGSIZE - 1) & ~((uint64_t)PGSIZE - 1);
    }
    managed_end &= ~((uint64_t)PGSIZE - 1);

    cprintf("Managed physical memory: [0x%016lx, 0x%016lx)\n",
            (unsigned long)managed_start, (unsigned long)managed_end);
    if (free_start_pa < managed_end) {
        init_memmap(pa2page((uintptr_t)free_start_pa),
                    (size_t)((managed_end - free_start_pa) / PGSIZE));
    }
}

// boot_map_segment - setup&enable the paging mechanism
// parameters
//  la:   linear address of this memory need to map (after x86 segment map)
//  size: memory size
//  pa:   physical address of this memory
//  perm: permission of this memory
static void boot_map_segment(pde_t *pgdir, uintptr_t la, size_t size,
                             uintptr_t pa, uint32_t perm)
{
    assert(PGOFF(la) == PGOFF(pa));
    size_t n = ROUNDUP(size + PGOFF(la), PGSIZE) / PGSIZE;
    la = ROUNDDOWN(la, PGSIZE);
    pa = ROUNDDOWN(pa, PGSIZE);
    for (; n > 0; n--, la += PGSIZE, pa += PGSIZE)
    {
        pte_t *ptep = get_pte(pgdir, la, 1);
        assert(ptep != NULL);
        *ptep = pte_create(pa >> PGSHIFT, PTE_V | perm);
    }
}

// boot_alloc_page - allocate one page using pmm->alloc_pages(1)
// return value: the kernel virtual address of this allocated page
// note: this function is used to get the memory for PDT(Page Directory
// Table)&PT(Page Table)
static void *boot_alloc_page(void)
{
    struct Page *p = alloc_page();
    if (p == NULL)
    {
        panic("boot_alloc_page failed.\n");
    }
    return page2kva(p);
}

// pmm_init - setup a pmm to manage physical memory, build PDT&PT to setup
// paging mechanism
//         - check the correctness of pmm & paging mechanism, print PDT&PT
void pmm_init(void)
{
    // We need to alloc/free the physical memory (granularity is 4KB or other
    // size).
    // So a framework of physical memory manager (struct pmm_manager)is defined
    // in pmm.h
    // First we should init a physical memory manager(pmm) based on the
    // framework.
    // Then pmm can alloc/free the physical memory.
    // Now the first_fit/best_fit/worst_fit/buddy_system pmm are available.
    init_pmm_manager();

    // detect physical memory space, reserve already used memory,
    // then use pmm->init_memmap to create free page list
    page_init();

    // use pmm->check to verify the correctness of the alloc/free function in a
    // pmm
    check_alloc_page();

    // create boot_pgdir, an initial page directory(Page Directory Table, PDT)
    extern char boot_page_table_sv39[];
    boot_pgdir_va = (pte_t *)boot_page_table_sv39;
    boot_pgdir_pa = PADDR(boot_pgdir_va);

    check_pgdir();

    static_assert(KERNBASE % PTSIZE == 0 && KERNTOP % PTSIZE == 0);

    // now the basic virtual memory map(see memalyout.h) is established.
    // check the correctness of the basic virtual memory map.
    check_boot_pgdir();

    kmalloc_init();
}

// get_pte - get pte and return the kernel virtual address of this pte for la
//        - if the PT contians this pte didn't exist, alloc a page for PT
// parameter:
//  pgdir:  the kernel virtual base address of PDT
//  la:     the linear address need to map
//  create: a logical value to decide if alloc a page for PT
// return vaule: the kernel virtual address of this pte
//lab4 2412266: Sv39 non-leaf permissions and allocation rollback.
pte_t *get_pte(pde_t *pgdir, uintptr_t la, bool create)
{
    pde_t *pdep1 = &pgdir[PDX1(la)];
    struct Page *new_top = NULL;
    if (!(*pdep1 & PTE_V))
    {
        struct Page *page;
        if (!create || (page = alloc_page()) == NULL)
        {
            return NULL;
        }
        set_page_ref(page, 1);
        uintptr_t pa = page2pa(page);
        memset(KADDR(pa), 0, PGSIZE);
        *pdep1 = ptd_create(page2ppn(page));
        new_top = page;
    }

    if (*pdep1 & (PTE_R | PTE_W | PTE_X)) return NULL;
    pde_t *pdep0 = &((pde_t *)KADDR(PDE_ADDR(*pdep1)))[PDX0(la)];
    if (!(*pdep0 & PTE_V))
    {
        struct Page *page;
        if (!create || (page = alloc_page()) == NULL)
        {
            if (new_top != NULL) {
                *pdep1 = 0;
                free_page(new_top);
            }
            return NULL;
        }
        set_page_ref(page, 1);
        uintptr_t pa = page2pa(page);
        memset(KADDR(pa), 0, PGSIZE);
        *pdep0 = ptd_create(page2ppn(page));
    }
    if (*pdep0 & (PTE_R | PTE_W | PTE_X)) return NULL;
    return &((pte_t *)KADDR(PDE_ADDR(*pdep0)))[PTX(la)];
}

// get_page - get related Page struct for linear address la using PDT pgdir
struct Page *get_page(pde_t *pgdir, uintptr_t la, pte_t **ptep_store)
{
    pte_t *ptep = get_pte(pgdir, la, 0);
    if (ptep_store != NULL)
    {
        *ptep_store = ptep;
    }
    if (ptep != NULL && *ptep & PTE_V)
    {
        return pte2page(*ptep);
    }
    return NULL;
}

// page_remove_pte - free an Page sturct which is related linear address la
//                - and clean(invalidate) pte which is related linear address la
// note: PT is changed, so the TLB need to be invalidate
static inline void page_remove_pte(pde_t *pgdir, uintptr_t la, pte_t *ptep)
{
    if (*ptep & PTE_V)
    {
        struct Page *page = pte2page(*ptep);
        page_ref_dec(page);
        if (page_ref(page) == 0)
        {
            free_page(page);
        }
        *ptep = 0;
        tlb_invalidate(pgdir, la);
    }
}

void unmap_range(pde_t *pgdir, uintptr_t start, uintptr_t end)
{
    assert(start % PGSIZE == 0 && end % PGSIZE == 0);
    assert(USER_ACCESS(start, end));

    do
    {
        pte_t *ptep = get_pte(pgdir, start, 0);
        if (ptep == NULL)
        {
            start = ROUNDDOWN(start + PTSIZE, PTSIZE);
            continue;
        }
        if (*ptep != 0)
        {
            page_remove_pte(pgdir, start, ptep);
        }
        start += PGSIZE;
    } while (start != 0 && start < end);
}

// lab5 2412266: never free a directory while another VMA still uses it.
static bool table_empty(pde_t *table)
{
    for (size_t i = 0; i < NPTEENTRY; i++) if (table[i] != 0) return 0;
    return 1;
}
void exit_range(pde_t *pgdir, uintptr_t start, uintptr_t end)
{
    assert(start % PGSIZE == 0 && end % PGSIZE == 0 && USER_ACCESS(start, end));
    for (uintptr_t addr = ROUNDDOWN(start, PTSIZE); addr < end; addr += PTSIZE) {
        pde_t *top = &pgdir[PDX1(addr)];
        if (!(*top & PTE_V) || (*top & (PTE_R | PTE_W | PTE_X))) continue;
        pde_t *mid = KADDR(PDE_ADDR(*top));
        pde_t *entry = &mid[PDX0(addr)];
        if ((*entry & PTE_V) && !(*entry & (PTE_R | PTE_W | PTE_X)) &&
            table_empty(KADDR(PDE_ADDR(*entry)))) {
            free_page(pde2page(*entry));
            *entry = 0;
        }
        if (table_empty(mid)) {
            free_page(pde2page(*top));
            *top = 0;
        }
    }
}

//lab5 function3 2412566
// Member C (2412566): ordinary private user pages; COW uses copy_range_cow instead.
int copy_range(pde_t *to, pde_t *from, uintptr_t start, uintptr_t end, bool share)
{
    assert(to != NULL && from != NULL && to != from);
    assert(start % PGSIZE == 0 && end % PGSIZE == 0);
    assert(USER_ACCESS(start, end));
    if (share) return -E_UNIMP;

    for (uintptr_t addr = start; addr < end; ) {
        pte_t *src = get_pte(from, addr, 0);
        if (src == NULL) {
            // No ordinary leaf table: skip to the next 2 MiB boundary.
            addr = ROUNDDOWN(addr, PTSIZE) + PTSIZE;
            continue;
        }
        if (*src & PTE_V) {
            // Only ordinary 4 KiB user leaves are supported here.
            if (!(*src & PTE_U) || !(*src & (PTE_R | PTE_X)) ||
                ((*src & PTE_W) && !(*src & PTE_R))) return -E_INVAL;
            if (*src & PTE_COW) return -E_UNIMP;
            pte_t *dst = get_pte(to, addr, 0);
            assert(dst == NULL || *dst == 0);
            struct Page *page = alloc_page();
            if (page == NULL) return -E_NO_MEM;
            memcpy(page2kva(page), page2kva(pte2page(*src)), PGSIZE);
            uint32_t perm = *src & (PTE_U | PTE_R | PTE_W | PTE_X);
            int ret = page_insert(to, page, addr, perm);
            if (ret != 0) {
                // Insertion failed before taking a mapping reference.
                free_page(page);
                return ret;
            }
            // page_insert owns the sole mapping reference now. The caller
            // unmaps inserted pages and frees tables on any later failure.
        }
        addr += PGSIZE;
    }
    return 0;
}


//lab4 2412266: migrated page-mapping removal.
void page_remove(pde_t *pgdir, uintptr_t la)
{
    pte_t *ptep = get_pte(pgdir, la, 0);
    if (ptep != NULL) {
        page_remove_pte(pgdir, la, ptep);
    }
}

//lab4 2412266: migrated insertion, leaf permissions and reference accounting.
int page_insert(pde_t *pgdir, struct Page *page, uintptr_t la, uint32_t perm)
{
    pte_t *ptep = get_pte(pgdir, la, 1);
    if (ptep == NULL) {
        return -E_NO_MEM;
    }
    // Hold the new reference before releasing the old one, including aliases.
    page_ref_inc(page);
    if (*ptep & PTE_V) {
        struct Page *old = pte2page(*ptep);
        if (old == page) {
            page_ref_dec(page);
        } else {
            page_remove_pte(pgdir, la, ptep);
        }
    }
    if (perm & PTE_W) perm |= PTE_R;
    if (!(perm & (PTE_R | PTE_X))) perm |= PTE_R;
    *ptep = pte_create(page2ppn(page), PTE_V | PTE_A | ((perm & PTE_W) ? PTE_D : 0) | perm);
    tlb_invalidate(pgdir, la);
    return 0;
}


// invalidate a TLB entry, but only if the page tables being
// edited are the ones currently in use by the processor.
void tlb_invalidate(pde_t *pgdir, uintptr_t la)
{
    asm volatile("sfence.vma %0" : : "r"(la));
}

// pgdir_alloc_page - call alloc_page & page_insert functions to
//                  - allocate a page size memory & setup an addr map
//                  - pa<->la with linear address la and the PDT pgdir
struct Page *pgdir_alloc_page(pde_t *pgdir, uintptr_t la, uint32_t perm)
{
    struct Page *page = alloc_page();
    if (page != NULL)
    {
        if (page_insert(pgdir, page, la, perm) != 0)
        {
            free_page(page);
            return NULL;
        }
        // swap_map_swappable(check_mm_struct, la, page, 0);
        page->pra_vaddr = la;
        assert(page_ref(page) == 1);
        // cprintf("get No. %d  page: pra_vaddr %x, pra_link.prev %x,
        // pra_link_next %x in pgdir_alloc_page\n", (page-pages),
        // page->pra_vaddr,page->pra_page_link.prev,
        // page->pra_page_link.next);
    }

    return page;
}

static void check_alloc_page(void)
{
    pmm_manager->check();
    cprintf("check_alloc_page() succeeded!\n");
}

static void check_pgdir(void)
{
    // assert(npage <= KMEMSIZE / PGSIZE);
    // The memory starts at 2GB in RISC-V
    // so npage is always larger than KMEMSIZE / PGSIZE
    size_t nr_free_store;

    nr_free_store = nr_free_pages();

    assert(npage <= KERNTOP / PGSIZE);
    assert(boot_pgdir_va != NULL && (uint32_t)PGOFF(boot_pgdir_va) == 0);
    assert(get_page(boot_pgdir_va, 0x0, NULL) == NULL);

    struct Page *p1, *p2;
    p1 = alloc_page();
    assert(page_insert(boot_pgdir_va, p1, 0x0, 0) == 0);

    pte_t *ptep;
    assert((ptep = get_pte(boot_pgdir_va, 0x0, 0)) != NULL);
    assert(pte2page(*ptep) == p1);
    assert(page_ref(p1) == 1);

    ptep = (pte_t *)KADDR(PDE_ADDR(boot_pgdir_va[0]));
    ptep = (pte_t *)KADDR(PDE_ADDR(ptep[0])) + 1;
    assert(get_pte(boot_pgdir_va, PGSIZE, 0) == ptep);

    p2 = alloc_page();
    assert(page_insert(boot_pgdir_va, p2, PGSIZE, PTE_U | PTE_W) == 0);
    assert((ptep = get_pte(boot_pgdir_va, PGSIZE, 0)) != NULL);
    assert(*ptep & PTE_U);
    assert(*ptep & PTE_W);
    assert((boot_pgdir_va[0] & (PTE_U | PTE_A | PTE_D)) == 0);
    assert(page_ref(p2) == 1);

    assert(page_insert(boot_pgdir_va, p1, PGSIZE, 0) == 0);
    assert(page_ref(p1) == 2);
    assert(page_ref(p2) == 0);
    assert((ptep = get_pte(boot_pgdir_va, PGSIZE, 0)) != NULL);
    assert(pte2page(*ptep) == p1);
    assert((*ptep & PTE_U) == 0);

    page_remove(boot_pgdir_va, 0x0);
    assert(page_ref(p1) == 1);
    assert(page_ref(p2) == 0);

    page_remove(boot_pgdir_va, PGSIZE);
    assert(page_ref(p1) == 0);
    assert(page_ref(p2) == 0);

    assert(page_ref(pde2page(boot_pgdir_va[0])) == 1);

    pde_t *pd1 = boot_pgdir_va, *pd0 = page2kva(pde2page(boot_pgdir_va[0]));
    free_page(pde2page(pd0[0]));
    free_page(pde2page(pd1[0]));
    boot_pgdir_va[0] = 0;
    flush_tlb();

    assert(nr_free_store == nr_free_pages());

    cprintf("check_pgdir() succeeded!\n");
}

static void check_boot_pgdir(void)
{
    size_t nr_free_store;
    pte_t *ptep;
    int i;

    nr_free_store = nr_free_pages();

    for (i = ROUNDDOWN(KERNBASE, PGSIZE); i < npage * PGSIZE; i += PGSIZE)
    {
        assert((ptep = get_pte(boot_pgdir_va, (uintptr_t)KADDR(i), 0)) != NULL);
        assert(PTE_ADDR(*ptep) == i);
    }

    assert(boot_pgdir_va[0] == 0);

    struct Page *p;
    p = alloc_page();
    assert(page_insert(boot_pgdir_va, p, 0x100, PTE_W | PTE_R) == 0);
    assert(page_ref(p) == 1);
    assert(page_insert(boot_pgdir_va, p, 0x100 + PGSIZE, PTE_W | PTE_R) == 0);
    assert(page_ref(p) == 2);

    const char *str = "ucore: Hello world!!";
    strcpy((void *)0x100, str);
    assert(strcmp((void *)0x100, (void *)(0x100 + PGSIZE)) == 0);

    *(char *)(page2kva(p) + 0x100) = '\0';
    assert(strlen((const char *)0x100) == 0);

    pde_t *pd1 = boot_pgdir_va, *pd0 = page2kva(pde2page(boot_pgdir_va[0]));
    free_page(p);
    free_page(pde2page(pd0[0]));
    free_page(pde2page(pd1[0]));
    boot_pgdir_va[0] = 0;
    flush_tlb();

    assert(nr_free_store == nr_free_pages());

    cprintf("check_boot_pgdir() succeeded!\n");
}

// perm2str - use string 'u,r,w,-' to present the permission
static const char *perm2str(int perm)
{
    static char str[4];
    str[0] = (perm & PTE_U) ? 'u' : '-';
    str[1] = 'r';
    str[2] = (perm & PTE_W) ? 'w' : '-';
    str[3] = '\0';
    return str;
}

// get_pgtable_items - In [left, right] range of PDT or PT, find a continuous
// linear addr space
//                  - (left_store*X_SIZE~right_store*X_SIZE) for PDT or PT
//                  - X_SIZE=PTSIZE=4M, if PDT; X_SIZE=PGSIZE=4K, if PT
// paramemters:
//  left:        no use ???
//  right:       the high side of table's range
//  start:       the low side of table's range
//  table:       the beginning addr of table
//  left_store:  the pointer of the high side of table's next range
//  right_store: the pointer of the low side of table's next range
//  return value: 0 - not a invalid item range, perm - a valid item range with
//  perm permission
static int get_pgtable_items(size_t left, size_t right, size_t start,
                             uintptr_t *table, size_t *left_store,
                             size_t *right_store)
{
    if (start >= right)
    {
        return 0;
    }
    while (start < right && !(table[start] & PTE_V))
    {
        start++;
    }
    if (start < right)
    {
        if (left_store != NULL)
        {
            *left_store = start;
        }
        int perm = (table[start++] & PTE_USER);
        while (start < right && (table[start] & PTE_USER) == perm)
        {
            start++;
        }
        if (right_store != NULL)
        {
            *right_store = start;
        }
        return perm;
    }
    return 0;
}

// lab5 function1 2412266: user-address leaf mappings of current process.
void print_pgdir(void)
{
    pde_t *pgdir = current != NULL && current->mm != NULL ? current->mm->pgdir : boot_pgdir_va;
    for (size_t i = 0; i < NPDEENTRY; i++) {
        if (!(pgdir[i] & PTE_V)) continue;
        if (pgdir[i] & (PTE_R | PTE_W | PTE_X)) continue;
        pde_t *mid = KADDR(PDE_ADDR(pgdir[i]));
        for (size_t j = 0; j < NPDEENTRY; j++) {
            if (!(mid[j] & PTE_V) || (mid[j] & (PTE_R | PTE_W | PTE_X))) continue;
            pte_t *pt = KADDR(PDE_ADDR(mid[j]));
            for (size_t k = 0; k < NPTEENTRY; k++) if (pt[k] & PTE_V)
                cprintf("va=%p pa=%p flags=0x%x\n", (void *)PGADDR(i,j,k,0),
                        (void *)PTE_ADDR(pt[k]), (unsigned)(pt[k] & 0x3ff));
        }
    }
}
