/* Real RV64/QEMU module driver, not the unfinished course boot path.
 * Register owned RAM, run the selected real PMM check, then SLUB checks.
 * The public PMM wrappers separately audit page ownership and original n.
 */
#include <pmm.h>
#include <slub.h>
#include <sbi.h>
#include <stdio.h>
#include <string.h>
#ifdef SLUB_TEST_BUDDY
#include <buddy_pmm.h>
#define TEST_MANAGER buddy_pmm_manager
#else
#include <best_fit_pmm.h>
#define TEST_MANAGER best_fit_pmm_manager
#endif

#define POOL_PA 0x81000000UL
#define POOL_START ((POOL_PA - DRAM_BASE) / PGSIZE)
#define POOL_PAGES 512
#define DESCRIPTORS (POOL_START + POOL_PAGES)
static struct Page descriptors[DESCRIPTORS];
struct Page *pages = descriptors;
const size_t nbase = DRAM_BASE / PGSIZE;
size_t npage = DRAM_BASE / PGSIZE + DESCRIPTORS;
uint64_t va_pa_offset = PHYSICAL_MEMORY_OFFSET;
const struct pmm_manager *pmm_manager = &TEST_MANAGER;
/* State: 0 reserved, 1 free, 2 allocated. No Page.property interpretation. */
static unsigned char ownership[POOL_PAGES];
static size_t requests[POOL_PAGES];
static bool audit_enabled;

void sbi_shutdown(void) {
    register unsigned long a7 asm("a7") = 8;
    register unsigned long a0 asm("a0") = 0;
    register unsigned long a1 asm("a1") = 0;
    register unsigned long a2 asm("a2") = 0;
    asm volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a7), "r"(a2) : "memory");
}
static void putch(int ch, void *unused) { sbi_console_putchar(ch); }
int cprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintfmt(putch, NULL, fmt, ap);
    va_end(ap);
    return 0;
}
void __noreturn __panic(const char *file, int line, const char *fmt, ...) {
    cprintf("FAIL %s:%d: ", file, line);
    va_list ap;
    va_start(ap, fmt);
    vprintfmt(putch, NULL, fmt, ap);
    va_end(ap);
    cprintf("\n");
    sbi_shutdown();
    for (;;) { asm volatile("wfi"); }
}
static size_t actual_pages(size_t n) {
#ifdef SLUB_TEST_BUDDY
    size_t cost = 1;
    while (cost < n) { cost *= 2; }
    return cost;
#else
    return n;
#endif
}
static void audit_pages(void) {
    size_t free = 0;
    for (size_t i = 0; i < POOL_PAGES; i ++) { free += ownership[i] == 1; }
    assert(nr_free_pages() == free);
}
struct Page *alloc_pages(size_t n) {
    struct Page *page = pmm_manager->alloc_pages(n);
    if (audit_enabled) {
        if (page != NULL) {
            assert(page >= pages + POOL_START);
            size_t i = page - pages - POOL_START, cost = actual_pages(n);
            assert(i + cost <= POOL_PAGES && requests[i] == 0);
            requests[i] = n;
            for (size_t j = i; j < i + cost; j ++) {
                assert(ownership[j] == 1);
                ownership[j] = 2;
            }
        }
        audit_pages();
    }
    return page;
}
void free_pages(struct Page *page, size_t n) {
    if (audit_enabled) {
        assert(page >= pages + POOL_START);
        size_t i = page - pages - POOL_START, cost = actual_pages(n);
        assert(i + cost <= POOL_PAGES && requests[i] == n);
        requests[i] = 0;
        for (size_t j = i; j < i + cost; j ++) {
            assert(ownership[j] == 2);
            ownership[j] = 1;
        }
    }
    pmm_manager->free_pages(page, n);
    if (audit_enabled) { audit_pages(); }
}
size_t nr_free_pages(void) { return pmm_manager->nr_free_pages(); }

static void add_region(size_t start, size_t n) {
    pmm_manager->init_memmap(pages + POOL_START + start, n);
    for (size_t i = start; i < start + n; i ++) { ownership[i] = 1; }
}

void test_main(void) {
    extern char __bss_end[];
    assert(PADDR(__bss_end) < POOL_PA);
    cprintf("SLUB RV64 module: %s / real high VA mapping\n", pmm_manager->name);
    for (size_t layout = 0; layout < 2; layout ++) {
        audit_enabled = 0;
        memset(descriptors, 0, sizeof(descriptors));
        memset(ownership, 0, sizeof(ownership));
        memset(requests, 0, sizeof(requests));
        for (size_t i = 0; i < DESCRIPTORS; i ++) { SetPageReserved(pages + i); }
        pmm_manager->init();
        if (layout == 0) {
            add_region(0, POOL_PAGES);
        } else {
            add_region(3, 61);
            add_region(129, 195); /* Uneven regions and reserved gaps. */
        }
        size_t initial = nr_free_pages();
        pmm_manager->check();
        assert(nr_free_pages() == initial);
        cprintf("PASS original PMM check: layout=%lu free=%lu\n", layout, initial);
        audit_enabled = 1;
        struct Page *caller = alloc_pages(3);
        assert(caller != NULL);
        unsigned char *payload = (unsigned char *)(page2pa(caller) + va_pa_offset);
        memset(payload, 0x5a, 3 * PGSIZE);
        slub_init();
        assert(nr_free_pages() == initial - actual_pages(3));
        slub_check();
        slub_check(); /* No reinit: catch leaked ownership and stale nodes. */
        for (size_t i = 0; i < 3 * PGSIZE; i ++) { assert(payload[i] == 0x5a); }
        free_pages(caller, 3);
        assert(nr_free_pages() == initial);
        for (size_t i = 0; i < POOL_PAGES; i ++) {
            assert(ownership[i] != 2 && requests[i] == 0);
            if (ownership[i] == 0) { assert(PageReserved(pages + POOL_START + i)); }
        }
        cprintf("PASS page ownership / original n / caller data: layout=%lu\n", layout);
    }
    cprintf("PASS SLUB_MODULE\n");
    sbi_shutdown();
    for (;;) { asm volatile("wfi"); }
}
