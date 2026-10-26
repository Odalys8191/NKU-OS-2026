#include <pmm.h>
#include <list.h>
#include <best_fit_pmm.h>
#include <stdio.h>
#include <string.h>

//lab2 function2-2 YOUR CODE
static free_area_t free_area;

#define free_list (free_area.free_list)
#define nr_free (free_area.nr_free)

static void
best_fit_init(void) {
    list_init(&free_list);
    nr_free = 0;
}

// Insert a free block in address order and absorb its adjacent neighbours.
// The caller owns the page-count update; splitting/merging never changes it.
static void
best_fit_insert_block(struct Page *base) {
    list_entry_t *le = list_next(&free_list);
    while (le != &free_list && le2page(le, page_link) < base) {
        le = list_next(le);
    }
    list_add_before(le, &base->page_link);

    le = list_prev(&base->page_link);
    if (le != &free_list) {
        struct Page *prev = le2page(le, page_link);
        if (prev + prev->property == base) {
            prev->property += base->property;
            list_del(&base->page_link);
            ClearPageProperty(base);
            base->property = 0;
            base = prev;
        }
    }

    le = list_next(&base->page_link);
    if (le != &free_list) {
        struct Page *next = le2page(le, page_link);
        if (base + base->property == next) {
            base->property += next->property;
            list_del(&next->page_link);
            ClearPageProperty(next);
            next->property = 0;
        }
    }
}

static void
best_fit_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    for (struct Page *p = base; p != base + n; p ++) {
        assert(PageReserved(p));
        p->flags = p->property = 0;
        set_page_ref(p, 0);
    }
    base->property = n;
    SetPageProperty(base);
    nr_free += n;
    best_fit_insert_block(base);
}

static struct Page *
best_fit_alloc_pages(size_t n) {
    assert(n > 0);
    if (n > nr_free) {
        return NULL;
    }

    struct Page *best = NULL;
    list_entry_t *le = &free_list;
    while ((le = list_next(le)) != &free_list) {
        struct Page *p = le2page(le, page_link);
        // Strict improvement preserves the lower address on equal sizes.
        if (p->property >= n &&
            (best == NULL || p->property < best->property)) {
            best = p;
            if (p->property == n) {
                break;
            }
        }
    }
    // The original checks temporarily empty the list with nr_free nonzero.
    if (best == NULL) {
        return NULL;
    }

    if (best->property > n) {
        struct Page *remainder = best + n;
        remainder->property = best->property - n;
        SetPageProperty(remainder);
        list_add_after(&best->page_link, &remainder->page_link);
    }
    list_del(&best->page_link);
    ClearPageProperty(best);
    best->property = 0;
    nr_free -= n;
    return best;
}

static void
best_fit_free_pages(struct Page *base, size_t n) {
    assert(n > 0);
    for (struct Page *p = base; p != base + n; p ++) {
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = p->property = 0;
        set_page_ref(p, 0);
    }
    base->property = n;
    SetPageProperty(base);
    nr_free += n;
    best_fit_insert_block(base);
}

static size_t
best_fit_nr_free_pages(void) {
    return nr_free;
}

// Framework tests: students must not modify basic_check or best_fit_check.
static void
basic_check(void) {
    struct Page *p0, *p1, *p2;
    p0 = p1 = p2 = NULL;
    assert((p0 = alloc_page()) != NULL);
    assert((p1 = alloc_page()) != NULL);
    assert((p2 = alloc_page()) != NULL);

    assert(p0 != p1 && p0 != p2 && p1 != p2);
    assert(page_ref(p0) == 0 && page_ref(p1) == 0 && page_ref(p2) == 0);

    assert(page2pa(p0) < npage * PGSIZE);
    assert(page2pa(p1) < npage * PGSIZE);
    assert(page2pa(p2) < npage * PGSIZE);

    list_entry_t free_list_store = free_list;
    list_init(&free_list);
    assert(list_empty(&free_list));

    unsigned int nr_free_store = nr_free;
    nr_free = 0;

    assert(alloc_page() == NULL);

    free_page(p0);
    free_page(p1);
    free_page(p2);
    assert(nr_free == 3);

    assert((p0 = alloc_page()) != NULL);
    assert((p1 = alloc_page()) != NULL);
    assert((p2 = alloc_page()) != NULL);

    assert(alloc_page() == NULL);

    free_page(p0);
    assert(!list_empty(&free_list));

    struct Page *p;
    assert((p = alloc_page()) == p0);
    assert(alloc_page() == NULL);

    assert(nr_free == 0);
    free_list = free_list_store;
    nr_free = nr_free_store;

    free_page(p);
    free_page(p1);
    free_page(p2);
}

static void
best_fit_check(void) {
    int score = 0, sumscore = 6;
    int count = 0, total = 0;
    list_entry_t *le = &free_list;
    while ((le = list_next(le)) != &free_list) {
        struct Page *p = le2page(le, page_link);
        assert(PageProperty(p));
        count++, total += p->property;
    }
    assert(total == nr_free_pages());

    basic_check();

    #ifdef ucore_test
    score += 1;
    cprintf("grading: %d / %d points\n", score, sumscore);
    #endif
    struct Page *p0 = alloc_pages(5), *p1, *p2;
    assert(p0 != NULL);
    assert(!PageProperty(p0));

    #ifdef ucore_test
    score += 1;
    cprintf("grading: %d / %d points\n", score, sumscore);
    #endif
    list_entry_t free_list_store = free_list;
    list_init(&free_list);
    assert(list_empty(&free_list));
    assert(alloc_page() == NULL);

    #ifdef ucore_test
    score += 1;
    cprintf("grading: %d / %d points\n", score, sumscore);
    #endif
    unsigned int nr_free_store = nr_free;
    nr_free = 0;

    // * - - * -
    free_pages(p0 + 1, 2);
    free_pages(p0 + 4, 1);
    assert(alloc_pages(4) == NULL);
    assert(PageProperty(p0 + 1) && p0[1].property == 2);
    // * - - * *
    assert((p1 = alloc_pages(1)) != NULL);
    assert(alloc_pages(2) != NULL);
    assert(p0 + 4 == p1);

    #ifdef ucore_test
    score += 1;
    cprintf("grading: %d / %d points\n", score, sumscore);
    #endif
    p2 = p0 + 1;
    free_pages(p0, 5);
    assert((p0 = alloc_pages(5)) != NULL);
    assert(alloc_page() == NULL);

    #ifdef ucore_test
    score += 1;
    cprintf("grading: %d / %d points\n", score, sumscore);
    #endif
    assert(nr_free == 0);
    nr_free = nr_free_store;

    free_list = free_list_store;
    free_pages(p0, 5);

    le = &free_list;
    while ((le = list_next(le)) != &free_list) {
        struct Page *p = le2page(le, page_link);
        count--, total -= p->property;
    }
    assert(count == 0);
    assert(total == 0);
    #ifdef ucore_test
    score += 1;
    cprintf("grading: %d / %d points\n", score, sumscore);
    #endif
}

// Supplementary tests own every page they release. Expected intervals are
// scenario constants, never inferred from the allocator's chosen candidate.
#define BEST_FIT_EXTRA_PAGES 24

struct best_fit_extra_span {
    size_t start, len;
};

static void
best_fit_extra_expect(struct Page *base,
                      const struct best_fit_extra_span *spans, size_t count) {
    list_entry_t *le = list_next(&free_list), *prev = &free_list;
    size_t total = 0;
    for (size_t j = 0; j < count; j ++) {
        struct Page *head = base + spans[j].start;
        assert(spans[j].len > 0);
        assert(spans[j].start + spans[j].len <= BEST_FIT_EXTRA_PAGES);
        if (j > 0) {
            assert(spans[j - 1].start + spans[j - 1].len < spans[j].start);
        }
        assert(le == &head->page_link && le->prev == prev);
        total += spans[j].len;
        prev = le;
        le = list_next(le);
    }
    assert(le == &free_list && free_list.prev == prev);
    assert(nr_free_pages() == total);
    for (size_t i = 0; i < BEST_FIT_EXTRA_PAGES; i ++) {
        size_t len = 0;
        for (size_t j = 0; j < count; j ++) {
            if (i == spans[j].start) { len = spans[j].len; }
        }
        assert(!PageReserved(base + i) && page_ref(base + i) == 0);
        assert(PageProperty(base + i) == (len != 0));
        assert(base[i].property == len);
    }
}

#define BEST_FIT_EXTRA_EXPECT(base, ...) do {                         \
    const struct best_fit_extra_span spans[] = { __VA_ARGS__ };       \
    best_fit_extra_expect(base, spans, sizeof(spans) / sizeof(spans[0])); \
} while (0)

static void
best_fit_extra_fail(struct Page *base, size_t n) {
    struct Page saved[BEST_FIT_EXTRA_PAGES];
    memcpy(saved, base, sizeof(saved));
    list_entry_t saved_list = free_list;
    size_t saved_count = nr_free_pages();
    assert(alloc_pages(n) == NULL);
    assert(nr_free_pages() == saved_count);
    assert(free_list.next == saved_list.next && free_list.prev == saved_list.prev);
    assert(memcmp(saved, base, sizeof(saved)) == 0);
}

static void
best_fit_extra_selection(struct Page *base) {
    free_pages(base + 12, 5);
    free_pages(base + 1, 6);
    free_pages(base + 8, 3);
    BEST_FIT_EXTRA_EXPECT(base, {1, 6}, {8, 3}, {12, 5});
    assert(alloc_pages(5) == base + 12); // Exact match after a larger block.
    BEST_FIT_EXTRA_EXPECT(base, {1, 6}, {8, 3});
    free_pages(base + 12, 5);
    assert(alloc_pages(2) == base + 8);  // 6 / 3 / 5: choose the 3-page block.
    BEST_FIT_EXTRA_EXPECT(base, {1, 6}, {10, 1}, {12, 5});
    assert(alloc_pages(6) == base + 1);
    assert(alloc_pages(1) == base + 10);
    assert(alloc_pages(5) == base + 12);
    best_fit_extra_expect(base, NULL, 0);

    // Reverse release order distinguishes address order from insertion order.
    free_pages(base + 12, 4);
    free_pages(base + 2, 4);
    BEST_FIT_EXTRA_EXPECT(base, {2, 4}, {12, 4});
    assert(alloc_pages(3) == base + 2);
    BEST_FIT_EXTRA_EXPECT(base, {5, 1}, {12, 4});
    assert(alloc_pages(1) == base + 5);
    assert(alloc_pages(4) == base + 12);
    free_pages(base + 12, 4);
    free_pages(base + 2, 4);
    assert(alloc_pages(4) == base + 2); // Equal exact matches also use low address.
    BEST_FIT_EXTRA_EXPECT(base, {12, 4});
    assert(alloc_pages(4) == base + 12);
    best_fit_extra_expect(base, NULL, 0);
    cprintf("PASS best_fit_extra case 1: exact / 6-3-5 / low-address ties\n");
}

static void
best_fit_extra_split(struct Page *base) {
    free_pages(base + 1, 1);
    free_pages(base + 4, 5);
    free_pages(base + 12, 6);
    BEST_FIT_EXTRA_EXPECT(base, {1, 1}, {4, 5}, {12, 6});
    assert(alloc_pages(2) == base + 4);
    BEST_FIT_EXTRA_EXPECT(base, {1, 1}, {6, 3}, {12, 6});
    assert(alloc_pages(3) == base + 6); // Observe the real remainder allocation.
    BEST_FIT_EXTRA_EXPECT(base, {1, 1}, {12, 6});
    assert(alloc_pages(1) == base + 1);
    assert(alloc_pages(6) == base + 12);
    best_fit_extra_expect(base, NULL, 0);
    cprintf("PASS best_fit_extra case 2: 5 -> 2 + 3 / metadata / count\n");
}

static void
best_fit_extra_merge(struct Page *base) {
    // No neighbours, then only a predecessor.
    free_pages(base + 4, 2);
    BEST_FIT_EXTRA_EXPECT(base, {4, 2});
    free_pages(base + 6, 2);
    BEST_FIT_EXTRA_EXPECT(base, {4, 4}); // +6 must lose its head flag/length.
    assert(alloc_pages(4) == base + 4);
    best_fit_extra_expect(base, NULL, 0);

    // Only a successor: its old head at +6 must disappear.
    free_pages(base + 6, 2);
    BEST_FIT_EXTRA_EXPECT(base, {6, 2});
    free_pages(base + 4, 2);
    BEST_FIT_EXTRA_EXPECT(base, {4, 4});
    assert(alloc_pages(4) == base + 4);
    best_fit_extra_expect(base, NULL, 0);

    // Both neighbours: +6 bridges +4 and +8, leaving only the +4 head.
    free_pages(base + 8, 2);
    free_pages(base + 4, 2);
    BEST_FIT_EXTRA_EXPECT(base, {4, 2}, {8, 2});
    free_pages(base + 6, 2);
    BEST_FIT_EXTRA_EXPECT(base, {4, 6});
    assert(alloc_pages(6) == base + 4);
    best_fit_extra_expect(base, NULL, 0);
    cprintf("PASS best_fit_extra case 3: none / prev / next / both merges\n");
}

static void
best_fit_extra_exhaustion(struct Page *base) {
    free_pages(base + 2, 2);
    free_pages(base + 7, 2);
    BEST_FIT_EXTRA_EXPECT(base, {2, 2}, {7, 2});
    best_fit_extra_fail(base, 3); // Four free pages, no contiguous triple.
    BEST_FIT_EXTRA_EXPECT(base, {2, 2}, {7, 2});
    best_fit_extra_fail(base, 5); // Ordinary total-count shortage.
    assert(alloc_pages(2) == base + 2);
    BEST_FIT_EXTRA_EXPECT(base, {7, 2});
    assert(alloc_pages(2) == base + 7);
    best_fit_extra_expect(base, NULL, 0);
    best_fit_extra_fail(base, 1); // Fully exhausted.
    best_fit_extra_expect(base, NULL, 0);
    cprintf("PASS best_fit_extra case 4: fragmentation / exhaustion / unchanged\n");
}

// Audit the whole real free list before isolation and after full restoration.
static void
best_fit_extra_audit(size_t expected) {
    list_entry_t *le = list_next(&free_list), *prev = &free_list;
    struct Page *end = NULL;
    size_t total = 0;
    while (le != &free_list) {
        struct Page *p = le2page(le, page_link);
        assert(le->prev == prev && PageProperty(p) && p->property > 0);
        assert(end == NULL || end < p); // Strict gap: no unmerged neighbours.
        assert(p->property <= expected - total);
        for (size_t i = 0; i < p->property; i ++) {
            assert(!PageReserved(p + i) && page_ref(p + i) == 0);
            if (i != 0) {
                assert(!PageProperty(p + i) && p[i].property == 0);
            }
        }
        total += p->property;
        end = p + p->property;
        prev = le;
        le = list_next(le);
    }
    assert(free_list.prev == prev && total == expected);
    assert(nr_free_pages() == expected);
}

void
best_fit_extra_check(void) {
    assert(pmm_manager == &best_fit_pmm_manager);
    size_t initial = nr_free_pages();
    best_fit_extra_audit(initial);
    struct Page *base = alloc_pages(BEST_FIT_EXTRA_PAGES);
    assert(base != NULL);

    // Hold all other free pages through real public allocations. Only the
    // page_link of an OWNED, allocated page is used for private bookkeeping;
    // never replace the allocator's sentinel, count, or free block metadata.
    list_entry_t held;
    list_init(&held);
    for (size_t i = BEST_FIT_EXTRA_PAGES; i < initial; i ++) {
        struct Page *p = alloc_pages(1);
        assert(p != NULL && !PageProperty(p));
        list_add(&held, &p->page_link);
    }
    best_fit_extra_expect(base, NULL, 0);
    best_fit_extra_fail(base, 1);
    best_fit_extra_selection(base);
    best_fit_extra_split(base);
    best_fit_extra_merge(base);
    best_fit_extra_exhaustion(base);

    // Every scenario returns with the entire arena allocated to this test.
    free_pages(base, BEST_FIT_EXTRA_PAGES);
    BEST_FIT_EXTRA_EXPECT(base, {0, BEST_FIT_EXTRA_PAGES});
    while (!list_empty(&held)) {
        list_entry_t *le = list_next(&held);
        list_del(le);
        free_pages(le2page(le, page_link), 1);
    }
    best_fit_extra_audit(initial);
    cprintf("PASS best_fit_extra_check: restored %u free pages\n", (unsigned)initial);
}

#undef BEST_FIT_EXTRA_EXPECT
#undef BEST_FIT_EXTRA_PAGES

const struct pmm_manager best_fit_pmm_manager = {
    .name = "best_fit_pmm_manager",
    .init = best_fit_init,
    .init_memmap = best_fit_init_memmap,
    .alloc_pages = best_fit_alloc_pages,
    .free_pages = best_fit_free_pages,
    .nr_free_pages = best_fit_nr_free_pages,
    .check = best_fit_check,
};
