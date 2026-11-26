#include <defs.h>
#include <list.h>
#include <memlayout.h>
#include <assert.h>
#include <kmalloc.h>
#include <sync.h>
#include <pmm.h>
#include <stdio.h>

/*
 * SLOB Allocator: Simple List Of Blocks
 *
 * Matt Mackall <mpm@selenic.com> 12/30/03
 *
 * How SLOB works:
 *
 * The core of SLOB is a traditional K&R style heap allocator, with
 * support for returning aligned objects. The granularity of this
 * allocator is 8 bytes on x86, though it's perhaps possible to reduce
 * this to 4 if it's deemed worth the effort. The slob heap is a
 * singly-linked list of pages from __get_free_page, grown on demand
 * and allocation from the heap is currently first-fit.
 *
 * Above this is an implementation of kmalloc/kfree. Blocks returned
 * from kmalloc are 8-byte aligned and prepended with a 8-byte header.
 * If kmalloc is asked for objects of PAGE_SIZE or larger, it calls
 * __get_free_pages directly so that it can return page-aligned blocks
 * and keeps a linked list of such pages and their orders. These
 * objects are detected in kfree() by their page alignment.
 *
 * SLAB is emulated on top of SLOB by simply calling constructors and
 * destructors for every SLAB allocation. Objects are returned with
 * the 8-byte alignment unless the SLAB_MUST_HWCACHE_ALIGN flag is
 * set, in which case the low-level allocator will fragment blocks to
 * create the proper alignment. Again, objects of page-size or greater
 * are allocated by calling __get_free_pages. As SLAB objects know
 * their size, no separate size bookkeeping is necessary and there is
 * essentially no allocation space overhead.
 */

// some helper
#define spin_lock_irqsave(l, f) local_intr_save(f)
#define spin_unlock_irqrestore(l, f) local_intr_restore(f)
typedef unsigned int gfp_t;
#ifndef PAGE_SIZE
#define PAGE_SIZE PGSIZE
#endif

#ifndef L1_CACHE_BYTES
#define L1_CACHE_BYTES 64
#endif

#ifndef ALIGN
#define ALIGN(addr, size) (((addr) + (size) - 1) & (~((size) - 1)))
#endif

struct slob_block
{
	int units;
	struct slob_block *next;
};
typedef struct slob_block slob_t;

#define SLOB_UNIT sizeof(slob_t)
#define SLOB_UNITS(size) (((size) + SLOB_UNIT - 1) / SLOB_UNIT)
#define SLOB_ALIGN L1_CACHE_BYTES

struct bigblock
{
	int order;
	void *pages;
	struct bigblock *next;
};
typedef struct bigblock bigblock_t;

static slob_t arena = {.next = &arena, .units = 1};
static slob_t *slobfree = &arena;
static bigblock_t *bigblocks;

static void *__slob_get_free_pages(gfp_t gfp, int order)
{
	struct Page *page = alloc_pages(1 << order);
	if (!page)
		return NULL;
	return page2kva(page);
}

#define __slob_get_free_page(gfp) __slob_get_free_pages(gfp, 0)

//lab5 2412266: adapt the migrated allocator to kva2page(void *).
static inline void __slob_free_pages(unsigned long kva, int order)
{
	free_pages(kva2page((void *)kva), 1 << order);
}

/* Insert and coalesce under the SLOB lock, returning the link to the result. */
static slob_t **slob_insert(slob_t *b)
{
	slob_t *prev = &arena, *cur = &arena;
	uintptr_t start = (uintptr_t)b;
	for (;; prev = cur, cur = cur->next)
	{
		uintptr_t addr = (uintptr_t)cur;
		uintptr_t next = (uintptr_t)cur->next;
		if ((addr < start && start < next) ||
		    (addr >= next && (addr < start || start < next)))
			break;
	}

	b->next = cur->next;
	if (b->next != &arena && b + b->units == b->next)
	{
		b->units += b->next->units;
		b->next = b->next->next;
	}
	if (cur != &arena && cur + cur->units == b)
	{
		cur->units += b->units;
		cur->next = b->next;
		cur = prev;
	}
	else
		cur->next = b;

	/* The predecessor remains valid even if the merged block is reclaimed. */
	slobfree = cur;
	return &cur->next;
}

static void *slob_alloc(size_t size, gfp_t gfp, int align)
{
	assert((size + SLOB_UNIT) < PAGE_SIZE);

	slob_t *prev, *cur, *aligned = 0;
	int delta = 0, units = SLOB_UNITS(size);
	unsigned long flags;

	spin_lock_irqsave(&slob_lock, flags);
	prev = slobfree;
	for (cur = prev->next;; prev = cur, cur = cur->next)
	{
		if (align)
		{
			aligned = (slob_t *)ALIGN((unsigned long)cur, align);
			delta = aligned - cur;
		}
		if (cur != &arena && cur->units >= units + delta)
		{ /* room enough? */
			if (delta)
			{ /* need to fragment head to align? */
				aligned->units = cur->units - delta;
				aligned->next = cur->next;
				cur->next = aligned;
				cur->units = delta;
				prev = cur;
				cur = aligned;
			}

			if (cur->units == units)	/* exact fit? */
				prev->next = cur->next; /* unlink */
			else
			{ /* fragment */
				prev->next = cur + units;
				prev->next->units = cur->units - units;
				prev->next->next = cur->next;
				cur->units = units;
			}

			slobfree = prev;
			spin_unlock_irqrestore(&slob_lock, flags);
			return cur;
		}
		if (cur == slobfree)
		{
			spin_unlock_irqrestore(&slob_lock, flags);

			cur = (slob_t *)__slob_get_free_page(gfp);
			if (!cur)
				return 0;

			spin_lock_irqsave(&slob_lock, flags);
			/* Keep the refill available until this allocation can use it. */
			cur->units = SLOB_UNITS(PAGE_SIZE);
			slob_insert(cur);
			cur = slobfree;
		}
	}
}

static void slob_free(void *block, int size)
{
	slob_t *b = (slob_t *)block;
	unsigned long flags;

	if (!block)
		return;

	if (size)
		b->units = SLOB_UNITS(size);

	spin_lock_irqsave(&slob_lock, flags);
	slob_t **link = slob_insert(b);
	b = *link;
	uintptr_t start = (uintptr_t)b;
	uintptr_t end = start + b->units * SLOB_UNIT;
	uintptr_t page_start = ROUNDUP(start, PAGE_SIZE);
	uintptr_t page_end = ROUNDDOWN(end, PAGE_SIZE);
	if (page_start < page_end)
	{
		/* Keep only the edge fragments; unlink all returned pages first. */
		slob_t *next = b->next;
		if (page_end < end)
		{
			slob_t *tail = (slob_t *)page_end;
			tail->units = SLOB_UNITS(end - page_end);
			tail->next = next;
			next = tail;
		}
		if (start < page_start)
		{
			b->units = SLOB_UNITS(page_start - start);
			b->next = next;
		}
		else
			*link = next;
		free_pages(kva2page((void *)page_start),
			   (page_end - page_start) / PAGE_SIZE);
	}

	spin_unlock_irqrestore(&slob_lock, flags);
}

void slob_init(void)
{
	cprintf("use SLOB allocator\n");
}

inline void
kmalloc_init(void)
{
	slob_init();
	cprintf("kmalloc_init() succeeded!\n");
}

size_t
slob_allocated(void)
{
	return 0;
}

size_t
kallocated(void)
{
	return slob_allocated();
}

static int find_order(int size)
{
	int order = 0;
	for (; size > 4096; size >>= 1)
		order++;
	return order;
}

static void *__kmalloc(size_t size, gfp_t gfp)
{
	slob_t *m;
	bigblock_t *bb;
	unsigned long flags;

	if (size < PAGE_SIZE - SLOB_UNIT)
	{
		m = slob_alloc(size + SLOB_UNIT, gfp, 0);
		return m ? (void *)(m + 1) : 0;
	}

	bb = slob_alloc(sizeof(bigblock_t), gfp, 0);
	if (!bb)
		return 0;

	bb->order = find_order(size);
	bb->pages = (void *)__slob_get_free_pages(gfp, bb->order);

	if (bb->pages)
	{
		spin_lock_irqsave(&block_lock, flags);
		bb->next = bigblocks;
		bigblocks = bb;
		spin_unlock_irqrestore(&block_lock, flags);
		return bb->pages;
	}

	slob_free(bb, sizeof(bigblock_t));
	return 0;
}

void *
kmalloc(size_t size)
{
	return __kmalloc(size, 0);
}

void kfree(void *block)
{
	bigblock_t *bb, **last = &bigblocks;
	unsigned long flags;

	if (!block)
		return;

	if (!((unsigned long)block & (PAGE_SIZE - 1)))
	{
		/* might be on the big block list */
		spin_lock_irqsave(&block_lock, flags);
		for (bb = bigblocks; bb; last = &bb->next, bb = bb->next)
		{
			if (bb->pages == block)
			{
				*last = bb->next;
				spin_unlock_irqrestore(&block_lock, flags);
				__slob_free_pages((unsigned long)block, bb->order);
				slob_free(bb, sizeof(bigblock_t));
				return;
			}
		}
		spin_unlock_irqrestore(&block_lock, flags);
	}

	slob_free((slob_t *)block - 1, 0);
	return;
}

unsigned int ksize(const void *block)
{
	bigblock_t *bb;
	unsigned long flags;

	if (!block)
		return 0;

	if (!((unsigned long)block & (PAGE_SIZE - 1)))
	{
		spin_lock_irqsave(&block_lock, flags);
		for (bb = bigblocks; bb; bb = bb->next)
			if (bb->pages == block)
			{
				spin_unlock_irqrestore(&slob_lock, flags);
				return PAGE_SIZE << bb->order;
			}
		spin_unlock_irqrestore(&block_lock, flags);
	}

	return ((slob_t *)block - 1)->units * SLOB_UNIT;
}
