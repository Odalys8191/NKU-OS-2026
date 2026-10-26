#ifndef __KERN_MM_SLUB_H__
#define __KERN_MM_SLUB_H__

#include <defs.h>

/* Single-core allocator; callers serialize all operations.
 * Initialize after PMM checks, with no live SLUB objects. Allocates no pages.
 */
void slub_init(void);

/* Requires initialization and size > 0. Returns a 16-byte-aligned object
 * with at least size writable bytes, or NULL without changing live objects.
 * Payload is not zeroed.
 */
void *kmalloc(size_t size);

/* Requires an original, non-NULL, still-live pointer returned by kmalloc.
 * Empty slabs and large allocations are returned immediately to the PMM.
 */
void kfree(void *ptr);

/* After PMM self-check and slub_init, with empty SLUB and exclusive allocation.
 * Exercises real exhaustion; returns all owned resources before returning.
 * Recognizes best_fit_pmm_manager and buddy_pmm_manager page-count contracts.
 */
void slub_check(void);

#endif /* !__KERN_MM_SLUB_H__ */
