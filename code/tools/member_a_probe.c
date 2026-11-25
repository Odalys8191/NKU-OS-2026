// Copied into user/ only inside the isolated test tree.
#include <ulib.h>
#include <stdio.h>
int main(void)
{
    volatile unsigned long canary[2] = {0x123456789abcdef0UL, 0xfedcba9876543210UL};
    if (getpid() <= 1) return 101;
    *(volatile unsigned *)0x01000000 = 0x87654321;
    yield();
    if (*(volatile unsigned *)0x01000000 != 0x87654321) return 102;
    if (canary[0] != 0x123456789abcdef0UL || canary[1] != 0xfedcba9876543210UL) return 103;
    cprintf("[Member A user] getpid/yield/COW/stack canaries PASS\n");
    return 73;
}
