#ifndef __KERNEL_MM_KASAN_H
#define __KERNEL_MM_KASAN_H

#include "types.h"

#ifdef XV6_KASAN
int kasan_config_enabled(void);
int kasan_enabled(void);
void kasan_enable(void);
void kasan_disable(void);
void kasan_poison(const void *addr, size_t size, uint8 tag);
void kasan_unpoison(const void *addr, size_t size);
void kasan_page_alloc(const void *pa, uint64 order);
void kasan_page_free(const void *pa, uint64 order);
void kasan_vmalloc_alloc(const void *addr, size_t requested, size_t allocated,
                         const char *tag, unsigned long caller);
void kasan_vmalloc_free(const void *addr, size_t size);
void kasan_vmalloc_selftest(void);
void kasan_report_access(uint64 addr, size_t size, int write,
                         const char *reason, unsigned long ret_ip);
int kasan_check_range(const void *addr, size_t size, int write,
                      unsigned long ret_ip);
#else
static inline int kasan_config_enabled(void) { return 0; }
static inline int kasan_enabled(void) { return 0; }
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
static inline void kasan_vmalloc_alloc(const void *addr, size_t requested,
                                       size_t allocated, const char *tag,
                                       unsigned long caller)
{
    (void)addr;
    (void)requested;
    (void)allocated;
    (void)tag;
    (void)caller;
}
static inline void kasan_vmalloc_free(const void *addr, size_t size)
{
    (void)addr;
    (void)size;
}
static inline void kasan_vmalloc_selftest(void) {}
static inline void kasan_report_access(uint64 addr, size_t size, int write,
                                       const char *reason,
                                       unsigned long ret_ip)
{
    (void)addr;
    (void)size;
    (void)write;
    (void)reason;
    (void)ret_ip;
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
