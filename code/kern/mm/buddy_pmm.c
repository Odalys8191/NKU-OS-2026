#include <buddy_pmm.h>
#include <list.h>
#include <stdio.h>

/* QEMU's supported physical range contains at most 2^15 pages. */
#define BUDDY_MAX_ORDER 15

static list_entry_t buddy_free_area[BUDDY_MAX_ORDER + 1];
static size_t buddy_nr_free;

static size_t
buddy_order_pages(unsigned int order) {
    return (size_t)1 << order;
}

static int
buddy_order_for_pages(size_t n) {
    size_t pages_count = 1;
    for (unsigned int order = 0; order <= BUDDY_MAX_ORDER; order ++) {
        if (pages_count >= n) {
            return (int)order;
        }
        pages_count <<= 1;
    }
    return -1;
}

static void
buddy_insert_block(struct Page *base, unsigned int order) {
    assert(order <= BUDDY_MAX_ORDER);
    assert(!PageReserved(base) && !PageProperty(base));
    base->property = order;
    SetPageProperty(base);
    list_add(&buddy_free_area[order], &base->page_link);
}

static struct Page *
buddy_take_block(unsigned int order) {
    assert(order <= BUDDY_MAX_ORDER);
    assert(!list_empty(&buddy_free_area[order]));
    list_entry_t *entry = list_next(&buddy_free_area[order]);
    struct Page *base = le2page(entry, page_link);
    list_del_init(entry);
    ClearPageProperty(base);
    return base;
}

static bool
buddy_pfn_range_valid(size_t pfn, size_t count) {
    return pfn >= nbase && pfn < npage && count <= npage - pfn;
}

static bool
buddy_is_free_block(size_t pfn, unsigned int order) {
    size_t count = buddy_order_pages(order);
    if (!buddy_pfn_range_valid(pfn, count)) {
        return 0;
    }
    struct Page *page = &pages[pfn - nbase];
    return !PageReserved(page) && PageProperty(page) &&
           page->property == order;
}

/* Add an initially free block and coalesce it with blocks registered by an
 * earlier init_memmap call when the two really are buddies. */
static void
buddy_add_free_block(struct Page *base, unsigned int order) {
    size_t pfn = page2ppn(base);
    while (order < BUDDY_MAX_ORDER) {
        size_t buddy_pfn = pfn ^ buddy_order_pages(order);
        if (!buddy_is_free_block(buddy_pfn, order)) {
            break;
        }
        struct Page *other = &pages[buddy_pfn - nbase];
        list_del_init(&other->page_link);
        ClearPageProperty(other);
        other->property = 0;
        if (buddy_pfn < pfn) {
            base = other;
            pfn = buddy_pfn;
        }
        order ++;
    }
    buddy_insert_block(base, order);
}

static void
buddy_init(void) {
    for (unsigned int order = 0; order <= BUDDY_MAX_ORDER; order ++) {
        list_init(&buddy_free_area[order]);
    }
    buddy_nr_free = 0;
}

static void
buddy_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    assert(base >= pages && base <= pages + (npage - nbase));
    assert(n <= (size_t)(pages + (npage - nbase) - base));

    for (struct Page *page = base; page < base + n; page ++) {
        assert(PageReserved(page));
        page->flags = 0;
        page->property = 0;
        set_page_ref(page, 0);
        list_init(&page->page_link);
    }

    struct Page *current = base;
    size_t remaining = n;
    while (remaining > 0) {
        size_t pfn = page2ppn(current);
        unsigned int order = BUDDY_MAX_ORDER;
        while (order > 0) {
            size_t count = buddy_order_pages(order);
            if (count <= remaining && (pfn & (count - 1)) == 0) {
                break;
            }
            order --;
        }
        buddy_add_free_block(current, order);
        size_t count = buddy_order_pages(order);
        current += count;
        remaining -= count;
    }
    buddy_nr_free += n;
}

static struct Page *
buddy_alloc_pages(size_t n) {
    assert(n > 0);
    int wanted = buddy_order_for_pages(n);
    if (wanted < 0) {
        return NULL;
    }

    unsigned int found = (unsigned int)wanted;
    while (found <= BUDDY_MAX_ORDER && list_empty(&buddy_free_area[found])) {
        found ++;
    }
    if (found > BUDDY_MAX_ORDER) {
        return NULL;
    }

    struct Page *base = buddy_take_block(found);
    while (found > (unsigned int)wanted) {
        found --;
        struct Page *upper = base + buddy_order_pages(found);
        buddy_insert_block(upper, found);
    }
    base->property = (unsigned int)wanted;
    ClearPageProperty(base);
    buddy_nr_free -= buddy_order_pages((unsigned int)wanted);
    return base;
}

static void
buddy_free_pages(struct Page *base, size_t n) {
    assert(n > 0);
    int requested_order = buddy_order_for_pages(n);
    assert(requested_order >= 0);
    unsigned int order = (unsigned int)requested_order;
    size_t count = buddy_order_pages(order);
    size_t pfn = page2ppn(base);

    assert(buddy_pfn_range_valid(pfn, count));
    assert((pfn & (count - 1)) == 0);
    assert(!PageReserved(base) && !PageProperty(base));
    assert(base->property == order);
    for (size_t i = 1; i < count; i ++) {
        assert(!PageReserved(base + i) && !PageProperty(base + i));
    }
    for (size_t i = 0; i < count; i ++) {
        (base + i)->flags = 0;
        (base + i)->property = 0;
        set_page_ref(base + i, 0);
        list_init(&(base + i)->page_link);
    }

    buddy_nr_free += count;
    while (order < BUDDY_MAX_ORDER) {
        size_t buddy_pfn = pfn ^ buddy_order_pages(order);
        if (!buddy_is_free_block(buddy_pfn, order)) {
            break;
        }
        struct Page *other = &pages[buddy_pfn - nbase];
        list_del_init(&other->page_link);
        ClearPageProperty(other);
        other->property = 0;
        if (buddy_pfn < pfn) {
            base->property = 0;
            base = other;
            pfn = buddy_pfn;
        }
        order ++;
    }
    base->property = 0;
    buddy_insert_block(base, order);
}

static size_t
buddy_nr_free_pages(void) {
    return buddy_nr_free;
}

static void
buddy_audit(void) {
    size_t total = 0;
    for (unsigned int order = 0; order <= BUDDY_MAX_ORDER; order ++) {
        list_entry_t *head = &buddy_free_area[order];
        list_entry_t *entry = list_next(head);
        list_entry_t *previous = head;
        while (entry != head) {
            struct Page *base = le2page(entry, page_link);
            size_t pfn = page2ppn(base);
            size_t count = buddy_order_pages(order);
            assert(entry->prev == previous);
            assert(buddy_pfn_range_valid(pfn, count));
            assert((pfn & (count - 1)) == 0);
            assert(PageProperty(base) && base->property == order);
            if (order < BUDDY_MAX_ORDER) {
                assert(!buddy_is_free_block(pfn ^ count, order));
            }
            total += count;
            previous = entry;
            entry = list_next(entry);
        }
        assert(head->prev == previous);
    }
    assert(total == buddy_nr_free);
}

static void
buddy_check(void) {
    size_t initial = buddy_nr_free_pages();
    buddy_audit();
    assert(initial >= 13);

    struct Page *one = buddy_alloc_pages(1);
    struct Page *three = buddy_alloc_pages(3);
    struct Page *five = buddy_alloc_pages(5);
    assert(one != NULL && three != NULL && five != NULL);
    assert(buddy_nr_free_pages() == initial - 13);
    assert((page2ppn(three) & 3) == 0);
    assert((page2ppn(five) & 7) == 0);

    buddy_free_pages(three, 3);
    buddy_free_pages(one, 1);
    buddy_free_pages(five, 5);
    assert(buddy_nr_free_pages() == initial);
    buddy_audit();
    cprintf("buddy_check() succeeded!\n");
}

const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};
