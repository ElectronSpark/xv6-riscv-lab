#ifndef __KERNEL_MM_KASAN_H
#define __KERNEL_MM_KASAN_H

#include "types.h"

#ifdef XV6_KASAN
void kasan_enable(void);
void kasan_disable(void);
void kasan_poison(const void *addr, size_t size, uint8 tag);
void kasan_unpoison(const void *addr, size_t size);
void kasan_page_alloc(const void *pa, uint64 order);
void kasan_page_free(const void *pa, uint64 order);
int kasan_check_range(const void *addr, size_t size, int write,
                      unsigned long ret_ip);
#else
static inline void kasan_enable(void) {}
static inline void kasan_disable(void) {}
static inline void kasan_poison(const void *addr, size_t size, uint8 tag)
{
    (void)addr;
    (void)size;
    (void)tag;
}
static inline void kasan_unpoison(const void *addr, size_t size)
{
    (void)addr;
    (void)size;
}
static inline void kasan_page_alloc(const void *pa, uint64 order)
{
    (void)pa;
    (void)order;
}
static inline void kasan_page_free(const void *pa, uint64 order)
{
    (void)pa;
    (void)order;
}
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
