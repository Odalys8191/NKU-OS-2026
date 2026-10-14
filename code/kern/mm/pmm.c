#include <default_pmm.h>
#include <best_fit_pmm.h>
#include <defs.h>
#include <error.h>
#include <memlayout.h>
#include <mmu.h>
#include <pmm.h>
#include <sbi.h>
#include <stdio.h>
#include <string.h>
#include <riscv.h>
#include <dtb.h>

// virtual address of physical page array
struct Page *pages;
// amount of physical memory (in pages)
size_t npage = 0;
// the kernel image is mapped at VA=KERNBASE and PA=info.base
uint64_t va_pa_offset;
// memory starts at 0x80000000 in RISC-V
// DRAM_BASE defined in riscv.h as 0x80000000
const size_t nbase = DRAM_BASE / PGSIZE;

// virtual address of boot-time page directory
uintptr_t *satp_virtual = NULL;
// physical address of boot-time page directory
uintptr_t satp_physical;

// physical memory management
const struct pmm_manager *pmm_manager;


static void check_alloc_page(void);

// init_pmm_manager - initialize a pmm_manager instance
static void init_pmm_manager(void) {
    pmm_manager = &best_fit_pmm_manager;
    cprintf("memory management: %s\n", pmm_manager->name);
    pmm_manager->init();
}

// init_memmap - call pmm->init_memmap to build Page struct for free memory
static void init_memmap(struct Page *base, size_t n) {
    pmm_manager->init_memmap(base, n);
}

// alloc_pages - call pmm->alloc_pages to allocate a continuous n*PAGESIZE
// memory
struct Page *alloc_pages(size_t n) {
    return pmm_manager->alloc_pages(n);
}

// free_pages - call pmm->free_pages to free a continuous n*PAGESIZE memory
void free_pages(struct Page *base, size_t n) {
    pmm_manager->free_pages(base, n);
}

// nr_free_pages - call pmm->nr_free_pages to get the size (nr*PAGESIZE)
// of current free memory
size_t nr_free_pages(void) {
    return pmm_manager->nr_free_pages();
}

//lab2 function1 YOUR CODE

/* Initialize the Page array and register the available physical pages. */
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

    /* The linker and the boot page table must use the framework's mapping. */
    if (PHYSICAL_MEMORY_END <= DRAM_BASE ||
        KERNEL_BEGIN_PADDR < DRAM_BASE ||
        KERNEL_BEGIN_VADDR < PHYSICAL_MEMORY_OFFSET ||
        KERNEL_BEGIN_VADDR - PHYSICAL_MEMORY_OFFSET != KERNEL_BEGIN_PADDR) {
        panic("page_init: unsupported physical/virtual memory layout");
    }
    va_pa_offset = PHYSICAL_MEMORY_OFFSET;

    managed_start = mem_base > DRAM_BASE ? mem_base : DRAM_BASE;
    managed_end = mem_end < PHYSICAL_MEMORY_END ? mem_end : PHYSICAL_MEMORY_END;
    if (managed_start >= managed_end ||
        mem_base > KERNEL_BEGIN_PADDR || mem_end <= KERNEL_BEGIN_PADDR) {
        panic("page_init: DTB memory range is incompatible with the kernel layout");
    }

    kernel_end_va = (uintptr_t)end;
    if (kernel_end_va < KERNEL_BEGIN_VADDR || kernel_end_va < va_pa_offset) {
        panic("page_init: invalid kernel end address 0x%016lx",
              (unsigned long)kernel_end_va);
    }
    kernel_end_pa = (uint64_t)kernel_end_va - va_pa_offset;
    if (kernel_end_pa > managed_end || kernel_end_pa > mem_end) {
        panic("page_init: memory range does not contain the kernel (kernel end=0x%016lx)",
              (unsigned long)kernel_end_pa);
    }

    /* npage is an exclusive physical page-number bound. */
    npage = (size_t)(managed_end / PGSIZE);
    if (npage <= nbase) {
        panic("page_init: no complete physical pages in the supported range");
    }
    page_count = npage - nbase;
    if ((uint64_t)page_count > max_addr / sizeof(struct Page)) {
        panic("page_init: Page descriptor array size overflows");
    }
    pages_bytes = page_count * sizeof(struct Page);

    /* Place descriptors after the kernel, aligned to a physical page. */
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

    /* Do not expose partial pages or memory below the DTB-reported range. */
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

/* pmm_init - initialize the physical memory management */
void pmm_init(void) {
    // We need to alloc/free the physical memory (granularity is 4KB or other size).
    // So a framework of physical memory manager (struct pmm_manager)is defined in pmm.h
    // First we should init a physical memory manager(pmm) based on the framework.
    // Then pmm can alloc/free the physical memory.
    // Now the first_fit/best_fit/worst_fit/buddy_system pmm are available.
    init_pmm_manager();

    // detect physical memory space, reserve already used memory,
    // then use pmm->init_memmap to create free page list
    page_init();

    // use pmm->check to verify the correctness of the alloc/free function in a pmm
    check_alloc_page();

    extern char boot_page_table_sv39[];
    satp_virtual = (pte_t*)boot_page_table_sv39;
    satp_physical = PADDR(satp_virtual);
    cprintf("satp virtual address: 0x%016lx\nsatp physical address: 0x%016lx\n", satp_virtual, satp_physical);
}

static void check_alloc_page(void) {
    pmm_manager->check();
    cprintf("check_alloc_page() succeeded!\n");
}
