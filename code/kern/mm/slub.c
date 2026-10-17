#include <slub.h>
#include <pmm.h>
#include <list.h>
#include <stdio.h>
#include <string.h>

#define SLUB_ALIGNMENT 16
#define SLUB_CLASS_COUNT 8
#define SLUB_MAX_SMALL 2048

struct slub_cache {
    size_t object_size;
    list_entry_t partial;
};

enum slub_kind { SLUB_SMALL, SLUB_LARGE };

/* Metadata belongs to the allocation's first page, never to Page.property.
 * Small allocations use cache through partial_link; large ones use nr_pages.
 */
struct slub_header {
    enum slub_kind kind;
    struct slub_cache *cache;
    size_t capacity;
    size_t inuse;
    void *free_head;
    list_entry_t partial_link;
    size_t nr_pages;
};

/* Constant-expression equivalent of ROUNDUP(sizeof(struct slub_header), 16).
 * defs.h's ROUNDUP is a statement expression, unsuitable at file scope.
 */
#define SLUB_HEADER_SIZE \
    ((sizeof(struct slub_header) + SLUB_ALIGNMENT - 1) / SLUB_ALIGNMENT * \
     SLUB_ALIGNMENT)
typedef char slub_header_fits[
    (SLUB_HEADER_SIZE > 0 && SLUB_HEADER_SIZE < PGSIZE &&
     (PGSIZE - SLUB_HEADER_SIZE) / SLUB_MAX_SMALL > 0) ? 1 : -1];
typedef char slub_free_pointer_fits[
    sizeof(void *) <= SLUB_ALIGNMENT ? 1 : -1];

static struct slub_cache caches[SLUB_CLASS_COUNT];

void slub_init(void) {
    for (size_t i = 0; i < SLUB_CLASS_COUNT; i ++) {
        caches[i].object_size = (size_t)SLUB_ALIGNMENT << i;
        list_init(&caches[i].partial);
    }
}

static struct slub_header *slab_create(struct slub_cache *cache) {
    struct Page *page = alloc_pages(1);
    if (page == NULL) {
        return NULL;
    }
    struct slub_header *header =
        (struct slub_header *)(page2pa(page) + va_pa_offset);
    header->kind = SLUB_SMALL;
    header->cache = cache;
    header->capacity = (PGSIZE - SLUB_HEADER_SIZE) / cache->object_size;
    header->inuse = 0;
    header->free_head = (char *)header + SLUB_HEADER_SIZE;
    list_init(&header->partial_link);

    char *slot = header->free_head;
    for (size_t i = 0; i < header->capacity; i ++) {
        /* Only free slots contain allocator links. No per-object header. */
        *(void **)slot = i + 1 < header->capacity ?
                        slot + cache->object_size : NULL;
        slot += cache->object_size;
    }
    list_add(&cache->partial, &header->partial_link);
    return header;
}

static void *slab_alloc(struct slub_header *header) {
    void *object = header->free_head;
    header->free_head = *(void **)object;
    header->inuse ++;
    if (header->free_head == NULL) {
        list_del_init(&header->partial_link);
    }
    return object;
}

static void *slub_alloc_large(size_t size) {
    if (size > (size_t)-1 - SLUB_HEADER_SIZE) {
        return NULL;
    }
    size_t total = SLUB_HEADER_SIZE + size;
    /* Avoid overflowing total + PGSIZE - 1 when taking the ceiling. */
    size_t n = total / PGSIZE + (total % PGSIZE != 0);
    struct Page *page = alloc_pages(n);
    if (page == NULL) {
        return NULL;
    }
    struct slub_header *header =
        (struct slub_header *)(page2pa(page) + va_pa_offset);
    header->kind = SLUB_LARGE;
    header->nr_pages = n;
    return (char *)header + SLUB_HEADER_SIZE;
}

void *kmalloc(size_t size) {
    assert(size > 0);
    if (size > SLUB_MAX_SMALL) {
        return slub_alloc_large(size);
    }
    struct slub_cache *cache = caches;
    while (cache->object_size < size) {
        cache ++;
    }
    struct slub_header *header;
    if (list_empty(&cache->partial)) {
        header = slab_create(cache);
        if (header == NULL) {
            return NULL;
        }
    } else {
        header = to_struct(list_next(&cache->partial),
                           struct slub_header, partial_link);
    }
    return slab_alloc(header);
}

static void slab_free(struct slub_header *header, void *ptr) {
    bool was_full = header->free_head == NULL;
    *(void **)ptr = header->free_head;
    header->free_head = ptr;
    header->inuse --;
    if (header->inuse == 0) {
        /* Capacity-one full slabs were never left on the partial list. */
        if (!was_full) {
            list_del_init(&header->partial_link);
        }
        struct Page *page = pa2page(PADDR(header));
        free_pages(page, 1);
        /* The header is no longer owned by SLUB. */
    } else if (was_full) {
        list_add(&header->cache->partial, &header->partial_link);
    }
}

void kfree(void *ptr) {
    struct slub_header *header =
        (struct slub_header *)ROUNDDOWN((uintptr_t)ptr, PGSIZE);
    if (header->kind == SLUB_LARGE) {
        struct Page *page = pa2page(PADDR(header));
        size_t n = header->nr_pages;
        /* Return the original request, including with Buddy's rounding. */
        free_pages(page, n);
    } else {
        slab_free(header, ptr);
    }
}

/* Independent test oracle. Only sizeof(header) is shared with the layout;
 * classes, occupancy, live ranges and page costs come from the public SPEC.
 * All bookkeeping is static, outside allocator-owned pages and free slots.
 */
#define CHECK_OBJECTS (PGSIZE / 16 + 1)
#define CHECK_PAGES ((PHYSICAL_MEMORY_END - DRAM_BASE) / PGSIZE)
static const size_t check_classes[] = {16, 32, 64, 128, 256, 512, 1024, 2048};
struct slub_check_object {
    unsigned char *ptr;
    size_t size;
    unsigned int tag;
};
struct slub_check_slab {
    uintptr_t base;
    size_t class_index, live;
    bool listed;
};
static struct slub_check_object check_objects[CHECK_OBJECTS];
static struct slub_check_slab check_slabs[CHECK_OBJECTS];
static unsigned char check_held[(CHECK_PAGES + 7) / 8];
static size_t check_start_pages, check_held_count;
static bool check_buddy;
static unsigned int check_tag;

static size_t slub_check_header_size(void) {
    return (sizeof(struct slub_header) + 15) / 16 * 16;
}

static size_t slub_check_class(size_t size) {
    size_t i = 0;
    while (i < 8 && size > check_classes[i]) { i ++; }
    return i;
}

static uintptr_t slub_check_base(const void *ptr) {
    return (uintptr_t)ptr / PGSIZE * PGSIZE;
}

static size_t slub_check_capacity(size_t i) {
    return (PGSIZE - slub_check_header_size()) / check_classes[i];
}

static size_t slub_check_cost(size_t n) {
    if (!check_buddy) { return n; }
    size_t cost = 1;
    while (cost < n) { cost *= 2; }
    return cost;
}

static unsigned char slub_check_pattern(unsigned int tag, size_t offset) {
    return (unsigned char)(tag * 37 + offset * 13 + offset / 251);
}

/* Reconstruct expected slabs from the test's live object inventory, never
 * from inuse/capacity/free_head. Validate links only after matching them to
 * an owned page/slot, so cycles and stale nodes fail bounded assertions.
 */
static void slub_check_audit(void) {
    size_t slabs = 0, cost = check_held_count;
    size_t h = slub_check_header_size();
    for (size_t i = 0; i < CHECK_OBJECTS; i ++) {
        struct slub_check_object *o = &check_objects[i];
        if (o->ptr == NULL) { continue; }
        uintptr_t addr = (uintptr_t)o->ptr, base = slub_check_base(o->ptr);
        assert(addr % 16 == 0);
        for (size_t j = 0; j < i; j ++) {
            struct slub_check_object *p = &check_objects[j];
            if (p->ptr != NULL) {
                assert(addr + o->size <= (uintptr_t)p->ptr ||
                       (uintptr_t)p->ptr + p->size <= addr);
            }
        }
        for (size_t j = 0; j < o->size; j ++) {
            assert(o->ptr[j] == slub_check_pattern(o->tag, j));
        }
        struct slub_header *header = (struct slub_header *)base;
        size_t ci = slub_check_class(o->size);
        if (ci == 8) {
            size_t n = (h + o->size + PGSIZE - 1) / PGSIZE;
            assert(addr == base + h && header->kind == SLUB_LARGE);
            assert(header->nr_pages == n);
            cost += slub_check_cost(n);
            continue;
        }
        assert(addr >= base + h);
        assert((addr - base - h) % check_classes[ci] == 0);
        assert((addr - base - h) / check_classes[ci] < slub_check_capacity(ci));
        size_t s = 0;
        while (s < slabs && check_slabs[s].base != base) { s ++; }
        if (s == slabs) {
            check_slabs[s] = (struct slub_check_slab){base, ci, 0, 0};
            slabs ++;
            cost ++;
        }
        assert(check_slabs[s].class_index == ci);
        check_slabs[s].live ++;
    }
    assert(cost <= check_start_pages);
    assert(nr_free_pages() == check_start_pages - cost);

    for (size_t s = 0; s < slabs; s ++) {
        struct slub_check_slab *p = &check_slabs[s];
        size_t ci = p->class_index, capacity = slub_check_capacity(ci);
        struct slub_header *header = (struct slub_header *)p->base;
        assert(header->kind == SLUB_SMALL && header->cache == &caches[ci]);
        assert(header->capacity == capacity && header->inuse == p->live);
        assert(p->live > 0 && p->live <= capacity);
        unsigned char slots[PGSIZE / 16] = {0};
        for (size_t i = 0; i < CHECK_OBJECTS; i ++) {
            struct slub_check_object *o = &check_objects[i];
            if (o->ptr != NULL && slub_check_base(o->ptr) == p->base) {
                size_t slot = ((uintptr_t)o->ptr - p->base - h) / check_classes[ci];
                assert(slots[slot] == 0);
                slots[slot] = 1;
            }
        }
        void *slot = header->free_head;
        size_t free_slots = 0;
        while (slot != NULL) {
            uintptr_t addr = (uintptr_t)slot;
            assert(addr >= p->base + h);
            size_t offset = addr - p->base - h;
            assert(offset % check_classes[ci] == 0);
            size_t index = offset / check_classes[ci];
            assert(index < capacity && slots[index] == 0);
            slots[index] = 2;
            free_slots ++;
            slot = *(void **)slot;
        }
        assert(free_slots == capacity - p->live);
    }
    for (size_t ci = 0; ci < 8; ci ++) {
        assert(caches[ci].object_size == check_classes[ci]);
        list_entry_t *head = &caches[ci].partial;
        list_entry_t *prev = head, *node = head->next;
        while (node != head) {
            size_t s = 0;
            while (s < slabs && node !=
                   &((struct slub_header *)check_slabs[s].base)->partial_link) { s ++; }
            assert(s < slabs && !check_slabs[s].listed);
            assert(check_slabs[s].class_index == ci);
            assert(check_slabs[s].live < slub_check_capacity(ci));
            assert(node->prev == prev);
            check_slabs[s].listed = 1;
            prev = node;
            node = node->next;
        }
        assert(head->prev == prev);
    }
    for (size_t s = 0; s < slabs; s ++) {
        assert(check_slabs[s].listed ==
               (check_slabs[s].live < slub_check_capacity(check_slabs[s].class_index)));
    }
}

static void slub_check_take(size_t id, size_t size) {
    assert(id < CHECK_OBJECTS && check_objects[id].ptr == NULL);
    unsigned char *ptr = kmalloc(size);
    assert(ptr != NULL && (uintptr_t)ptr % 16 == 0);
    for (size_t i = 0; i < CHECK_OBJECTS; i ++) {
        struct slub_check_object *o = &check_objects[i];
        if (o->ptr != NULL) {
            assert((uintptr_t)ptr + size <= (uintptr_t)o->ptr ||
                   (uintptr_t)o->ptr + o->size <= (uintptr_t)ptr);
        }
    }
    check_objects[id] = (struct slub_check_object){ptr, size, ++check_tag};
    for (size_t j = 0; j < size; j ++) {
        ptr[j] = slub_check_pattern(check_objects[id].tag, j);
    }
    slub_check_audit();
}

static void slub_check_drop(size_t id) {
    assert(id < CHECK_OBJECTS && check_objects[id].ptr != NULL);
    slub_check_audit();
    kfree(check_objects[id].ptr);
    check_objects[id].ptr = NULL;
    slub_check_audit();
}

static void slub_check_empty(void) {
    for (size_t i = 0; i < CHECK_OBJECTS; i ++) { assert(check_objects[i].ptr == NULL); }
    assert(check_held_count == 0);
    slub_check_audit();
}

static void slub_check_boundaries(void) {
    slub_check_take(0, 1);
    slub_check_drop(0);
    for (size_t ci = 0; ci < 8; ci ++) {
        for (size_t size = check_classes[ci] - 1; size <= check_classes[ci] + 1; size ++) {
            slub_check_take(0, size);
            slub_check_drop(0);
        }
    }
    slub_check_empty();
    cprintf("PASS slub case 1: 25 boundary sizes / alignment / payload\n");
}

static void slub_check_lifecycle(void) {
    for (size_t ci = 0; ci < 8; ci ++) {
        size_t capacity = slub_check_capacity(ci), size = check_classes[ci];
        for (size_t i = 0; i <= capacity; i ++) {
            slub_check_take(i, size);
            if (i < capacity) {
                assert(slub_check_base(check_objects[i].ptr) == slub_check_base(check_objects[0].ptr));
            }
        }
        assert(slub_check_base(check_objects[capacity].ptr) != slub_check_base(check_objects[0].ptr));
        assert(nr_free_pages() == check_start_pages - 2);
        /* C>1 now has two partial slabs. Remove the second, leaving exactly
         * one reusable slot; this does not prescribe a partial-list order.
         * C=1 instead returns each whole page immediately.
         */
        void *released = check_objects[0].ptr;
        slub_check_drop(0);
        assert(nr_free_pages() == check_start_pages - (capacity == 1 ? 1 : 2));
        slub_check_drop(capacity);
        if (capacity == 1) { assert(nr_free_pages() == check_start_pages); }
        slub_check_take(0, size);
        if (capacity > 1) { assert(check_objects[0].ptr == released); }
        assert(nr_free_pages() == check_start_pages - 1);
        for (size_t i = 0; i < capacity; i += 2) { slub_check_drop(i); }
        for (size_t i = 0; i < capacity; i += 2) { slub_check_take(i, size); }
        for (size_t i = 1; i < capacity; i += 2) { slub_check_drop(i); }
        for (size_t i = 0; i < capacity; i += 2) { slub_check_drop(i); }
        slub_check_empty();
        cprintf("PASS slub case 2: class=%lu C=%lu / C+1 / reuse / last free\n", size, capacity);
    }
}

static void slub_check_mixed(void) {
    static const size_t sizes[] = {
        1, 17, 63, 65, 255, 257, 1023, 1025, 2049, 4095, 8192,
        15, 32, 64, 128, 256, 512, 1024, 2048, 4097
    };
    size_t count = sizeof(sizes) / sizeof(sizes[0]);
    for (size_t i = 0; i < count; i ++) { slub_check_take(i, sizes[i]); }
    for (size_t i = 1; i < count; i += 2) { slub_check_drop(i); }
    for (size_t i = 1; i < count; i += 2) { slub_check_take(i, sizes[count - 1 - i]); }
    for (size_t i = count; i > 0; i --) { slub_check_drop(i - 1); }
    slub_check_empty();
    cprintf("PASS slub case 3: mixed ranges / distinct payloads / interleaved lifetime\n");
}

static void slub_check_large(void) {
    size_t h = slub_check_header_size();
    const size_t sizes[] = {4095, 4096, 4097, 8192, PGSIZE - h, PGSIZE - h + 1};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i ++) {
        size_t n = (h + sizes[i] + PGSIZE - 1) / PGSIZE;
        slub_check_take(0, sizes[i]);
        assert(nr_free_pages() == check_start_pages - slub_check_cost(n));
        if (sizes[i] == 8192) { assert(n == 3); }
        cprintf("PASS slub case 4: size=%lu original_n=%lu actual_pages=%lu\n",
                sizes[i], n, check_start_pages - nr_free_pages());
        slub_check_drop(0);
    }
    /* Impossible sizes fail before page arithmetic can wrap. */
    assert(kmalloc((size_t)-1) == NULL);
    assert(kmalloc((size_t)-1 - h + 1) == NULL);
    slub_check_empty();
}

static void slub_check_exhaustion(void) {
    slub_check_take(0, 16);       /* Partial page with live content. */
    slub_check_take(1, 2048);     /* Full capacity-one page. */
    slub_check_take(2, 8192);     /* Live multi-page payload. */
    uintptr_t partial = slub_check_base(check_objects[0].ptr);
    size_t available = nr_free_pages();
    struct Page *page;
    while ((page = alloc_pages(1)) != NULL) {
        assert(page >= pages && page < pages + (npage - nbase));
        size_t index = page - pages;
        assert((check_held[index / 8] & (1U << (index % 8))) == 0);
        check_held[index / 8] |= 1U << (index % 8);
        check_held_count ++;
        assert(check_held_count <= available);
        assert(nr_free_pages() == available - check_held_count);
    }
    assert(check_held_count == available && nr_free_pages() == 0);
    slub_check_audit();
    assert(kmalloc(32) == NULL);   /* Empty cache needs a new slab. */
    assert(kmalloc(2048) == NULL); /* Existing full slab cannot serve it. */
    assert(kmalloc(2049) == NULL);
    assert(kmalloc(8192) == NULL);
    slub_check_audit();
    size_t capacity = slub_check_capacity(0);
    for (size_t i = 3; i < capacity + 2; i ++) {
        slub_check_take(i, 16);
        assert(slub_check_base(check_objects[i].ptr) == partial);
        assert(nr_free_pages() == 0);
    }
    assert(kmalloc(16) == NULL); /* Partial is now full, PMM still exhausted. */
    slub_check_audit();
    slub_check_drop(0);
    slub_check_take(0, 16);     /* Full -> partial -> full without a PMM page. */
    assert(slub_check_base(check_objects[0].ptr) == partial);
    for (size_t i = 0; i < capacity + 2; i += 2) { slub_check_drop(i); }
    for (size_t i = 1; i < capacity + 2; i += 2) { slub_check_drop(i); }
    for (size_t i = 0; i < npage - nbase; i ++) {
        if (check_held[i / 8] & (1U << (i % 8))) {
            free_pages(pages + i, 1);
            check_held[i / 8] &= ~(1U << (i % 8));
            check_held_count --;
        }
    }
    slub_check_empty();
    cprintf("PASS slub case 5: exhausted %lu pages / NULL / partial reuse / recovery\n", available);
}

void slub_check(void) {
    assert(npage >= nbase && npage - nbase <= CHECK_PAGES);
    check_buddy = strcmp(pmm_manager->name, "buddy_pmm_manager") == 0;
    assert(check_buddy || strcmp(pmm_manager->name, "best_fit_pmm_manager") == 0);
    /* Do not reinitialize caches here: doing so would hide stale list nodes. */
    check_start_pages = nr_free_pages();
    slub_check_empty();
    for (size_t i = 0; i < sizeof(check_held); i ++) { assert(check_held[i] == 0); }
    cprintf("slub_check: manager=%s H=%lu free=%lu\n", pmm_manager->name,
            slub_check_header_size(), check_start_pages);
    slub_check_boundaries();
    slub_check_lifecycle();
    slub_check_mixed();
    slub_check_large();
    slub_check_exhaustion();
    slub_check_empty();
    cprintf("PASS slub_check: restored %lu free pages / empty partial lists\n", check_start_pages);
}
