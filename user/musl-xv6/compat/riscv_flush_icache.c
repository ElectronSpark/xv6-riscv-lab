#if defined(__riscv)
int __riscv_flush_icache(void *start, void *end, unsigned long flags)
{
    (void)start;
    (void)end;
    (void)flags;
    __asm__ __volatile__("fence.i" ::: "memory");
    return 0;
}
#endif