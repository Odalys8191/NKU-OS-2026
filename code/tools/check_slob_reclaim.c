/* Included in kmalloc.c only in the allocator test build. */
#include <string.h>

static size_t
check_slob_free_bytes(void)
{
	size_t total = 0;
	uintptr_t previous_end = (uintptr_t)&arena + sizeof(arena);
	bool cursor_found = slobfree == &arena;
	for (slob_t *block = arena.next; block != &arena; block = block->next)
	{
		uintptr_t start = (uintptr_t)block;
		assert(start >= previous_end && start % SLOB_UNIT == 0);
		assert(block->units > 0);
		size_t bytes = block->units * SLOB_UNIT;
		uintptr_t end = start + bytes;
		assert(ROUNDUP(start, PAGE_SIZE) >= ROUNDDOWN(end, PAGE_SIZE));
		previous_end = end;
		total += bytes;
		cursor_found |= block == slobfree;
	}
	assert(cursor_found);
	return total;
}

static void
check_payload(const unsigned char *data, size_t size, unsigned char value)
{
	for (size_t i = 0; i < size; i++)
		assert(data[i] == value);
}

void
check_slob_reclaim(void)
{
	const size_t small = 2 * SLOB_UNIT;
	const size_t wide = PAGE_SIZE - 3 * SLOB_UNIT;
	const size_t baseline = nr_free_pages();
	assert(check_slob_free_bytes() == 0);

	unsigned char *first = kmalloc(small), *second = kmalloc(small);
	assert(first && second);
	assert(ROUNDDOWN((uintptr_t)first, PAGE_SIZE) ==
	       ROUNDDOWN((uintptr_t)second, PAGE_SIZE));
	memset(second, 0x52, small);
	kfree(first);
	assert(nr_free_pages() == baseline - 1);
	assert(check_slob_free_bytes() == PAGE_SIZE - small - SLOB_UNIT);
	check_payload(second, small, 0x52);
	kfree(second);
	assert(nr_free_pages() == baseline && check_slob_free_bytes() == 0);
	cprintf("SLOB shared page and last free PASS\n");

	/* Leave exactly three adjacent real PMM pages available. */
	struct Page *window = alloc_pages(3), *held = NULL, *page;
	assert(window != NULL);
	while ((page = alloc_page()) != NULL)
	{
		*(struct Page **)page2kva(page) = held;
		held = page;
	}
	assert(nr_free_pages() == 0);
	free_pages(window, 3);
	uintptr_t base = (uintptr_t)page2kva(window);

	unsigned char *left = kmalloc(small);
	unsigned char *a = kmalloc(wide), *b = kmalloc(wide), *c = kmalloc(wide);
	unsigned char *right = kmalloc(small);
	assert((uintptr_t)left == base + SLOB_UNIT);
	assert((uintptr_t)a == base + 4 * SLOB_UNIT);
	assert((uintptr_t)b == base + PAGE_SIZE + 2 * SLOB_UNIT);
	assert((uintptr_t)c == base + 2 * PAGE_SIZE);
	assert((uintptr_t)right == base + 3 * PAGE_SIZE - small);
	assert(nr_free_pages() == 0 && check_slob_free_bytes() == 0);
	memset(left, 0x16, small);
	memset(b, 0x27, wide);
	memset(right, 0x38, small);
	kfree(a);
	kfree(c);
	assert(nr_free_pages() == 0);
	check_payload(b, wide, 0x27);
	kfree(b);
	assert(nr_free_pages() == 1);
	assert(check_slob_free_bytes() == 2 * (PAGE_SIZE - small - SLOB_UNIT));
	page = alloc_page();
	assert(page == window + 1);
	memset(page2kva(page), 0x49, PAGE_SIZE);
	first = kmalloc(small);
	second = kmalloc(small);
	assert(first && second && first != second);
	check_payload(left, small, 0x16);
	check_payload(right, small, 0x38);
	memset(first, 0x5a, small);
	memset(second, 0x6b, small);
	kfree(first);
	kfree(second);
	check_payload(page2kva(page), PAGE_SIZE, 0x49);
	assert(nr_free_pages() == 0);
	kfree(left);
	kfree(right);
	assert(nr_free_pages() == 2 && check_slob_free_bytes() == 0);
	free_page(page);
	cprintf("SLOB cross-page merge, two fragments, cursor and PMM reuse PASS\n");

	for (int round = 0; round < 8; round++)
	{
		unsigned char *big = kmalloc(2 * PAGE_SIZE);
		assert(big && (uintptr_t)big % PAGE_SIZE == 0);
		assert(ksize(big) == 2 * PAGE_SIZE && nr_free_pages() == 0);
		memset(big, 0x7c, 2 * PAGE_SIZE);
		check_payload(big, 2 * PAGE_SIZE, 0x7c);
		kfree(big);
		assert(nr_free_pages() == 3 && check_slob_free_bytes() == 0);
	}
	cprintf("SLOB big blocks and repeated reclamation PASS\n");

	/* A fresh big-block descriptor must be reclaimed on PMM failure. */
	assert(kmalloc(4 * PAGE_SIZE) == NULL);
	assert(nr_free_pages() == 3 && check_slob_free_bytes() == 0);
	first = kmalloc(small);
	assert(first);
	memset(first, 0x8d, small);
	page = alloc_pages(2);
	assert(page && nr_free_pages() == 0);
	size_t cache = check_slob_free_bytes();
	assert(kmalloc(wide) == NULL);
	assert(kmalloc(2 * PAGE_SIZE) == NULL);
	assert(nr_free_pages() == 0 && check_slob_free_bytes() == cache);
	check_payload(first, small, 0x8d);
	kfree(first);
	assert(nr_free_pages() == 1 && check_slob_free_bytes() == 0);
	free_pages(page, 2);
	while (held != NULL)
	{
		page = held;
		held = *(struct Page **)page2kva(page);
		free_page(page);
	}
	assert(nr_free_pages() == baseline && check_slob_free_bytes() == 0);
	cprintf("SLOB real PMM exhaustion, rollback and payload PASS\n");
	cprintf("SLOB RECLAIM PASS\n");
}
