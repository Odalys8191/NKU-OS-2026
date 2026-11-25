// Member A 2412266: temporary link fallbacks for Member B's pending APIs.
// Strong definitions in proc.c override these. Remove after B lands.
#include <proc.h>
#include <error.h>

__attribute__((weak)) int do_wait(int pid, int *code_store)
{
    return -E_UNIMP;
}

__attribute__((weak)) int do_execve(const char *name, size_t len,
                                 unsigned char *binary, size_t size)
{
    return -E_UNIMP;
}
