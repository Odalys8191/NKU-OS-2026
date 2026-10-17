// Independent RV64 module harness. Uses real course types, lists and allocator.
// It supplies descriptors and PMM dispatch; it does not verify page_init/Sv39.
#include <sbi.h>
#include <string.h>
#include "../../kern/mm/best_fit_pmm.c"

#define NPAGES 256
static struct Page descriptors[NPAGES];
struct Page *pages = descriptors;
const size_t nbase = DRAM_BASE / PGSIZE;
size_t npage = DRAM_BASE / PGSIZE + NPAGES;
uint64_t va_pa_offset;
const struct pmm_manager *pmm_manager = &best_fit_pmm_manager;
// Independent per-page oracle: 0 reserved, 1 allocated, 2 free.
static unsigned char state[NPAGES];
static int expect_zero_assert;

// The course sbi.h declares shutdown, but sbi.c does not define it.
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
    cprintf("%s %s:%d: ", expect_zero_assert ? "EXPECTED ASSERT" : "FAIL", file, line);
    va_list ap;
    va_start(ap, fmt);
    vprintfmt(putch, NULL, fmt, ap);
    va_end(ap);
    cprintf("\n");
    sbi_shutdown();
    for (;;) { asm volatile("wfi"); }
}
struct Page *alloc_pages(size_t n) { return pmm_manager->alloc_pages(n); }
void free_pages(struct Page *p, size_t n) { pmm_manager->free_pages(p, n); }
size_t nr_free_pages(void) { return pmm_manager->nr_free_pages(); }

static void reset(void) {
    memset(descriptors, 0, sizeof(descriptors));
    memset(state, 0, sizeof(state));
    for (int i = 0; i < NPAGES; i ++) {
        SetPageReserved(pages + i);
        pages[i].property = 99;
        pages[i].ref = 7;
    }
    pmm_manager->init();
    assert(list_empty(&free_list) && nr_free_pages() == 0);
}

static void audit(void) {
    list_entry_t *le = list_next(&free_list), *prev = &free_list;
    size_t total = 0;
    for (int i = 0; i < NPAGES; i ++) {
        struct Page *p = pages + i;
        assert(PageReserved(p) == (state[i] == 0));
        if (state[i] == 0) { continue; }
        assert(page_ref(p) == 0);
        if (state[i] == 2) { total ++; }
        if (state[i] == 2 && (i == 0 || state[i - 1] != 2)) {
            int end = i + 1;
            while (end < NPAGES && state[end] == 2) { end ++; }
            assert(le == &p->page_link && le->prev == prev);
            assert(PageProperty(p) && p->property == end - i);
            prev = le;
            le = list_next(le);
        } else {
            assert(!PageProperty(p) && p->property == 0);
        }
    }
    assert(le == &free_list && free_list.prev == prev);
    assert(total == nr_free_pages());
}

static void add_region(int start, int n) {
    for (int i = start; i < start + n; i ++) {
        assert(state[i] == 0);
        state[i] = 2;
    }
    pmm_manager->init_memmap(pages + start, n);
    audit();
}

static void release(int start, int n) {
    for (int i = start; i < start + n; i ++) {
        assert(state[i] == 1);
        // Legal allocated pages may carry references and stale numeric metadata.
        pages[i].ref = 3;
        pages[i].property = 19;
        state[i] = 2;
    }
    free_pages(pages + start, n);
    audit();
}

static int take(int n) {
    int best = -1, best_len = NPAGES + 1;
    for (int i = 0; i < NPAGES;) {
        if (state[i] != 2) { i ++; continue; }
        int start = i;
        while (i < NPAGES && state[i] == 2) { i ++; }
        if (i - start >= n && i - start < best_len) {
            best = start;
            best_len = i - start;
        }
    }
    struct Page saved[NPAGES];
    memcpy(saved, pages, sizeof(saved));
    list_entry_t saved_list = free_list;
    size_t saved_count = nr_free;
    struct Page *p = alloc_pages(n);
    assert(p == (best < 0 ? NULL : pages + best));
    if (best < 0) {
        assert(memcmp(saved, pages, sizeof(saved)) == 0);
        assert(free_list.next == saved_list.next);
        assert(free_list.prev == saved_list.prev);
        assert(nr_free == saved_count);
    } else {
        for (int i = best; i < best + n; i ++) { state[i] = 1; }
    }
    audit();
    return best;
}

static void original_check(void) {
    reset();
    add_region(0, NPAGES);
    pmm_manager->check();
    audit();
    cprintf("PASS original basic_check + best_fit_check (6/6)\n");
    best_fit_extra_check();
    audit();
    // A second run verifies reusability without reinitializing the manager.
    best_fit_extra_check();
    audit();
}

static void edge_checks(void) {
    reset();
    assert(take(1) == -1);
    add_region(0, NPAGES);
    list_entry_t saved_list = free_list;
    list_init(&free_list);
    struct Page saved[NPAGES];
    memcpy(saved, pages, sizeof(saved));
    assert(alloc_pages(1) == NULL && nr_free == NPAGES);
    assert(list_empty(&free_list) && free_list.prev == &free_list);
    assert(memcmp(saved, pages, sizeof(saved)) == 0);
    free_list = saved_list;
    audit();
    assert(take(NPAGES) == 0);
    release(10, 8);
    release(30, 6);
    release(50, 6);
    assert(take(9) == -1); // Total is enough, but no contiguous candidate.
    assert(take(NPAGES + 1) == -1);
    assert(take(5) == 30); // Smaller block beats first fit; tie uses low address.
    assert(take(6) == 50); // Exact match.
    assert(take(8) == 10);
    assert(take(1) == 35); // Split remainder.
    release(0, NPAGES);   // Combined release across former allocations.

    static const int orders[6][3] = {
        {0, 1, 2}, {0, 2, 1}, {1, 0, 2},
        {1, 2, 0}, {2, 0, 1}, {2, 1, 0}
    };
    for (int order = 0; order < 6; order ++) {
        reset();
        for (int j = 0; j < 3; j ++) { add_region(orders[order][j] * 8, 8); }
        assert(take(24) == 0);
        for (int j = 0; j < 3; j ++) { release(orders[order][j] * 8, 8); }
        assert(pages[0].property == 24 && nr_free_pages() == 24);
    }
    cprintf("PASS selection/split/failure immutability/subranges/all merge orders\n");
}

static void extra_layout_checks(void) {
    reset();
    add_region(32, 24); // Minimum contiguous arena, with no other free pages.
    pmm_manager->check();
    best_fit_extra_check();
    audit();

    reset();
    add_region(0, 128);
    add_region(160, 96); // Reserved gap plus allocations owned by a caller.
    assert(take(8) == 160);
    pmm_manager->check();
    best_fit_extra_check();
    audit(); // Independent whole-pool oracle: caller-owned/reserved pages stay so.
    release(160, 8);
    cprintf("PASS extra checks: minimum arena / multiple regions / caller ownership\n");
}

static unsigned int rng = 0x12345678;
static unsigned int next_random(void) {
    rng = rng * 1664525U + 1013904223U;
    return rng;
}
static void random_checks(void) {
    reset();
    add_region(0, NPAGES);
    for (int step = 0; step < 10000; step ++) {
        unsigned int r = next_random();
        int n = 1 + ((r >> 8) % 32);
        int start = (r >> 16) % NPAGES;
        if ((r & 1) && state[start] == 1) {
            int end = start;
            while (end < NPAGES && end < start + n && state[end] == 1) { end ++; }
            release(start, end - start);
        } else {
            take(n);
        }
    }
    for (int i = 0; i < NPAGES;) {
        if (state[i] != 1) { i ++; continue; }
        int start = i;
        while (i < NPAGES && state[i] == 1) { i ++; }
        release(start, i - start);
    }
    assert(nr_free_pages() == NPAGES && pages[0].property == NPAGES);
    cprintf("PASS 10000 deterministic bitmap-oracle operations and full recovery\n");
}

void test_main(void) {
#if TEST_ZERO
    reset();
    expect_zero_assert = 1;
#if TEST_ZERO == 1
    alloc_pages(0);
#elif TEST_ZERO == 2
    free_pages(pages, 0);
#else
    pmm_manager->init_memmap(pages, 0);
#endif
    expect_zero_assert = 0;
    panic("zero-size call did not assert");
#else
    original_check();
    extra_layout_checks();
    edge_checks();
    random_checks();
    cprintf("PASS BEST_FIT_MODULE\n");
    sbi_shutdown();
#endif
}
