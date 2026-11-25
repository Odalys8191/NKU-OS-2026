// lab5 2412266: private fork COW, single-hart kernel, no swap/huge user pages.
#include <vmm.h>
#include <pmm.h>
#include <error.h>
#include <string.h>

int copy_range_cow(pde_t *to, pde_t *from, uintptr_t start, uintptr_t end)
{
    if (to == NULL || from == NULL || to == from ||
        start % PGSIZE || end % PGSIZE || !USER_ACCESS(start, end))
        return -E_INVAL;
    bool intr_flag;
    local_intr_save(intr_flag);
    uintptr_t addr = start;
    int ret = 0;
    // Contract: an empty destination range. Validate before any mutations.
    for (; addr < end; addr += PGSIZE) {
        pte_t *dst = get_pte(to, addr, 0);
        if (dst != NULL && *dst != 0) { ret = -E_INVAL; goto out; }
    }
    for (addr = start; addr < end; addr += PGSIZE) {
        pte_t *src = get_pte(from, addr, 0);
        if (src == NULL || !(*src & PTE_V)) continue;
        if (!(*src & PTE_U) || !(*src & (PTE_R | PTE_X)) ||
            ((*src & PTE_COW) && (*src & PTE_W))) {
            ret = -E_INVAL; goto rollback;
        }
        uint32_t perm = *src & 0x3ff;
        if (perm & PTE_W) perm = (perm & ~PTE_W) | PTE_COW;
        ret = page_insert(to, pte2page(*src), addr, perm);
        if (ret != 0) goto rollback;
        // Publish child reference before restricting parent's PTE.
        if (*src & PTE_W) {
            *src = (*src & ~PTE_W) | PTE_COW;
            tlb_invalidate(from, addr);
        }
    }
    goto out;
rollback:
    // Parent entries safely remain COW, possibly exclusive after rollback.
    unmap_range(to, start, end);
    exit_range(to, start, end);
out:
    local_intr_restore(intr_flag);
    return ret;
}

int dup_mmap_cow(struct mm_struct *to, struct mm_struct *from)
{
    if (to == NULL || from == NULL || to == from || to->pgdir == NULL ||
        from->pgdir == NULL || to->map_count != 0 || mm_count(to) != 0)
        return -E_INVAL;
    // Caller owns source mm lock, as for ordinary dup_mmap.
    int ret = -E_NO_MEM;
    list_entry_t *head = &from->mmap_list;
    for (list_entry_t *le = list_next(head); le != head; le = list_next(le)) {
        struct vma_struct *vma = le2vma(le, list_link);
        struct vma_struct *new = vma_create(vma->vm_start, vma->vm_end, vma->vm_flags);
        if (new == NULL) goto failed;
        insert_vma_struct(to, new);
        ret = copy_range_cow(to->pgdir, from->pgdir, vma->vm_start, vma->vm_end);
        if (ret != 0) goto failed;
        ret = -E_NO_MEM;
    }
    return 0;
failed:
    // Caller must still put_pgdir + mm_destroy on unpublished destination.
    exit_mmap(to);
    return ret;
}

int cow_resolve(struct mm_struct *mm, uintptr_t addr)
{
    if (mm == NULL || mm->pgdir == NULL || addr < USERBASE || addr >= USERTOP)
        return -E_INVAL;
    struct vma_struct *vma = find_vma(mm, addr);
    if (vma == NULL || !(vma->vm_flags & VM_WRITE) ||
        ((vma->vm_flags & VM_STACK) && addr < vma->vm_start + PGSIZE))
        return -E_INVAL;
    addr = ROUNDDOWN(addr, PGSIZE);
    bool intr_flag;
    local_intr_save(intr_flag);
    pte_t *pte = get_pte(mm->pgdir, addr, 0);
    int ret = -E_INVAL;
    if (pte == NULL || (*pte & (PTE_V | PTE_U | PTE_COW)) !=
                        (PTE_V | PTE_U | PTE_COW) || (*pte & PTE_W))
        goto out;
    struct Page *old = pte2page(*pte);
    assert(page_ref(old) > 0);
    uint32_t perm = ((*pte & 0x3ff) | PTE_W | PTE_A | PTE_D) & ~PTE_COW;
    if (page_ref(old) == 1) {
        *pte = pte_create(page2ppn(old), perm);
        tlb_invalidate(mm->pgdir, addr);
        ret = 0;
    } else {
        struct Page *new = alloc_page();
        if (new == NULL) { ret = -E_NO_MEM; goto out; }
        memcpy(page2kva(new), page2kva(old), PGSIZE);
        ret = page_insert(mm->pgdir, new, addr, perm);
        if (ret != 0) free_page(new);
    }
out:
    local_intr_restore(intr_flag);
    return ret;
}

int cow_prepare_write(struct mm_struct *mm, uintptr_t addr, size_t len)
{
    if (len == 0) return 0;
    if (len > (uintptr_t)-1 - addr || !user_mem_check(mm, addr, len, 1))
        return -E_INVAL;
    if (mm == NULL) return 0;
    uintptr_t end = addr + len;
    for (uintptr_t pos = ROUNDDOWN(addr, PGSIZE); pos < end; pos += PGSIZE) {
        pte_t *pte = get_pte(mm->pgdir, pos, 0);
        if (pte == NULL || (*pte & (PTE_V | PTE_U)) != (PTE_V | PTE_U))
            return -E_INVAL;
        if (*pte & PTE_COW) {
            int ret = cow_resolve(mm, pos);
            if (ret != 0) return ret;
        } else if (!(*pte & PTE_W)) return -E_INVAL;
    }
    return 0;
}
