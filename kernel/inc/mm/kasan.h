#ifndef __KERNEL_MM_KASAN_H
#define __KERNEL_MM_KASAN_H

#include "types.h"

#ifdef XV6_KASAN
void kasan_enable(void);
void kasan_disable(void);
int kasan_check_range(const void *addr, size_t size, int write,
                      unsigned long ret_ip);
#else
static inline void kasan_enable(void) {}
static inline void kasan_disable(void) {}
static inline int kasan_check_range(const void *addr, size_t size, int write,
                                    unsigned long ret_ip)
{
    (void)addr;
    (void)size;
    (void)write;
    (void)ret_ip;
    return 0;
}
#endif

#endif /* __KERNEL_MM_KASAN_H */
