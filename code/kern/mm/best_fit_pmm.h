#ifndef __KERN_MM_BEST_FIT_PMM_H__
#define  __KERN_MM_BEST_FIT_PMM_H__

#include <pmm.h>

extern const struct pmm_manager best_fit_pmm_manager;

// After Best-Fit init_memmap and the original manager check; exclusive access,
// with at least one 24-page contiguous free block. Returns all borrowed pages.
void best_fit_extra_check(void);

#endif /* ! __KERN_MM_BEST_FIT_PMM_H__ */

