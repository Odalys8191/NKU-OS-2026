/* Member C 2412566: test-copy-only accounting; real allocator remains in use. */
extern size_t p4_slob_free_bytes(void);
extern void *__real_kmalloc(size_t);
extern void __real_kfree(void *);
static void *p4_objects[2048], *p4_before_objects[2048];
static int p4_objects_count, p4_before_count;
static size_t p4_before_free, p4_before_cache;
void *__wrap_kmalloc(size_t n) {
    void *p = __real_kmalloc(n);
    if (p) {
        int i; for (i = 0; i < 2048 && p4_objects[i]; i++);
        assert(i < 2048); p4_objects[i] = p; p4_objects_count++;
    }
    return p;
}
void __wrap_kfree(void *p) {
    if (p) {
        int i; for (i = 0; i < 2048 && p4_objects[i] != p; i++);
        assert(i < 2048 && p4_before_objects[i] == NULL);
        p4_objects[i] = NULL; p4_objects_count--;
    }
    __real_kfree(p);
}
static void p4_account(int after) {
    bool intr_flag;
    local_intr_save(intr_flag);
    size_t free = nr_free_pages(), cache = p4_slob_free_bytes();
    cprintf("P4 ACCOUNT after=%d free=%d slob_free_bytes=%d live_metadata=%d\n",
            after, (int)free, (int)cache, p4_objects_count);
    if (!after) {
        p4_before_free = free; p4_before_cache = cache;
        p4_before_count = p4_objects_count;
        memcpy(p4_before_objects, p4_objects, sizeof(p4_objects));
    } else {
        assert(p4_objects_count == p4_before_count);
        for (int i = 0; i < 2048; i++) {
            if (p4_before_objects[i]) {
                int j; for (j = 0; j < 2048 && p4_objects[j] != p4_before_objects[i]; j++);
                assert(j < 2048);
            }
        }
        assert(cache == p4_before_cache && free == p4_before_free);
        cprintf("P4 ACCOUNTING RESTORED\n");
    }
    local_intr_restore(intr_flag);
}
