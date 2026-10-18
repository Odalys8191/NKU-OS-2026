#include <sbi.h>
#include <string.h>
#include "../../kern/mm/buddy_pmm.c"

#define NPAGES 256

static struct Page descriptors[NPAGES];
struct Page *pages = descriptors;
const size_t nbase = DRAM_BASE / PGSIZE;
size_t npage = DRAM_BASE / PGSIZE + NPAGES;
uint64_t va_pa_offset;
const struct pmm_manager *pmm_manager = &buddy_pmm_manager;

/* State: 0 reserved, 1 free, 2 allocated. */
static unsigned char state[NPAGES];
static size_t requests[NPAGES];
static unsigned char covered[NPAGES];

#ifdef BUDDY_HOST_TEST
extern void abort(void) __attribute__((noreturn));
void sbi_shutdown(void) { }
int cprintf(const char *fmt, ...) { return 0; }
void __noreturn __panic(const char *file, int line, const char *fmt, ...) {
    abort();
}
#else
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
#endif

struct Page *alloc_pages(size_t n) { return pmm_manager->alloc_pages(n); }
void free_pages(struct Page *page, size_t n) { pmm_manager->free_pages(page, n); }
size_t nr_free_pages(void) { return pmm_manager->nr_free_pages(); }

static size_t rounded_pages(size_t n) {
    size_t count = 1;
    while (count < n) { count <<= 1; }
    return count;
}

static void audit(void) {
    memset(covered, 0, sizeof(covered));
    size_t total = 0;
    for (unsigned int order = 0; order <= BUDDY_MAX_ORDER; order ++) {
        list_entry_t *head = &buddy_free_area[order];
        list_entry_t *entry = list_next(head), *previous = head;
        while (entry != head) {
            struct Page *base = le2page(entry, page_link);
            size_t start = base - pages;
            size_t count = buddy_order_pages(order);
            assert(entry->prev == previous && start + count <= NPAGES);
            assert((page2ppn(base) & (count - 1)) == 0);
            assert(PageProperty(base) && base->property == order);
            for (size_t i = start; i < start + count; i ++) {
                assert(state[i] == 1 && covered[i] == 0);
                covered[i] = 1;
            }
            total += count;
            previous = entry;
            entry = list_next(entry);
        }
        assert(head->prev == previous);
    }
    for (size_t i = 0; i < NPAGES; i ++) {
        assert(covered[i] == (state[i] == 1));
        assert(PageReserved(pages + i) == (state[i] == 0));
    }
    assert(total == nr_free_pages());
}

static void reset(void) {
    memset(descriptors, 0, sizeof(descriptors));
    memset(state, 0, sizeof(state));
    memset(requests, 0, sizeof(requests));
    for (size_t i = 0; i < NPAGES; i ++) {
        SetPageReserved(pages + i);
        pages[i].property = 99;
        pages[i].ref = 7;
    }
    pmm_manager->init();
    audit();
}

static void add_region(size_t start, size_t n) {
    pmm_manager->init_memmap(pages + start, n);
    for (size_t i = start; i < start + n; i ++) { state[i] = 1; }
    audit();
}

static struct Page *take(size_t n) {
    struct Page *page = alloc_pages(n);
    if (page != NULL) {
        size_t start = page - pages, count = rounded_pages(n);
        assert(start + count <= NPAGES && requests[start] == 0);
        assert((page2ppn(page) & (count - 1)) == 0);
        requests[start] = n;
        for (size_t i = start; i < start + count; i ++) {
            assert(state[i] == 1);
            state[i] = 2;
        }
    }
    audit();
    return page;
}

static void release(struct Page *page, size_t n) {
    size_t start = page - pages, count = rounded_pages(n);
    assert(requests[start] == n);
    requests[start] = 0;
    for (size_t i = start; i < start + count; i ++) {
        assert(state[i] == 2);
        state[i] = 1;
    }
    free_pages(page, n);
    audit();
}

static void layout_and_rounding(void) {
    reset();
    add_region(3, 61);
    add_region(129, 95);
    assert(nr_free_pages() == 156);
    struct Page *three = take(3);
    struct Page *five = take(5);
    assert(three != NULL && five != NULL && nr_free_pages() == 144);
    release(three, 3);
    release(five, 5);
    assert(nr_free_pages() == 156);
    pmm_manager->check();
    audit();
    cprintf("PASS buddy layout / multiple regions / 3->4 / 5->8\n");
}

static void merge_and_nonbuddy(void) {
    reset();
    add_region(0, 16);
    struct Page *blocks[4];
    for (size_t i = 0; i < 4; i ++) {
        blocks[i] = take(1);
        assert(blocks[i] != NULL);
    }
    /* The first four one-page results form one aligned four-page parent. */
    for (size_t i = 0; i < 4; i ++) {
        for (size_t j = i + 1; j < 4; j ++) {
            if (blocks[j] < blocks[i]) {
                struct Page *tmp = blocks[i]; blocks[i] = blocks[j]; blocks[j] = tmp;
            }
        }
    }
    assert(blocks[3] == blocks[0] + 3);
    release(blocks[1], 1);
    release(blocks[2], 1);
    assert(PageProperty(blocks[1]) && blocks[1]->property == 0);
    assert(PageProperty(blocks[2]) && blocks[2]->property == 0);
    release(blocks[0], 1);
    release(blocks[3], 1);
    assert(nr_free_pages() == 16);
    assert(PageProperty(pages) && pages->property == 4);
    cprintf("PASS buddy adjacent non-partners / true partners / cascade merge\n");
}

static void failure_unchanged(void) {
    reset();
    add_region(0, 16);
    struct Page *all = take(16);
    assert(all != NULL && nr_free_pages() == 0);
    struct Page saved[NPAGES];
    memcpy(saved, pages, sizeof(saved));
    size_t saved_free = nr_free_pages();
    assert(take(1) == NULL);
    assert(memcmp(saved, pages, sizeof(saved)) == 0);
    assert(nr_free_pages() == saved_free);
    assert(alloc_pages(((size_t)1 << BUDDY_MAX_ORDER) + 1) == NULL);
    assert(memcmp(saved, pages, sizeof(saved)) == 0);
    release(all, 16);
    cprintf("PASS buddy exhaustion / oversized request / failure unchanged\n");
}

void test_main(void) {
    layout_and_rounding();
    merge_and_nonbuddy();
    failure_unchanged();
    cprintf("PASS BUDDY_MODULE\n");
    sbi_shutdown();
}

#ifdef BUDDY_HOST_TEST
int main(void) {
    test_main();
    return 0;
}
#endif
