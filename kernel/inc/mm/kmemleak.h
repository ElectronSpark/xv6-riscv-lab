#ifndef __KERNEL_MM_KMEMLEAK_H
#define __KERNEL_MM_KMEMLEAK_H

#include "types.h"

#ifdef XV6_KMEMLEAK
void kmemleak_init(void);
void kmemleak_alloc(const void *ptr, size_t size, const char *kind,
                    const char *tag, unsigned long caller);
void kmemleak_free(const void *ptr);
size_t kmemleak_format(char *buf, size_t size);
#else
static inline void kmemleak_init(void) {}
static inline void kmemleak_alloc(const void *ptr, size_t size,
                                  const char *kind, const char *tag,
                                  unsigned long caller)
{
    (void)ptr;
    (void)size;
    (void)kind;
    (void)tag;
    (void)caller;
}
static inline void kmemleak_free(const void *ptr)
{
    (void)ptr;
}
static inline size_t kmemleak_format(char *buf, size_t size)
{
    const char msg[] = "enabled=0 compiled=0 current=0 bytes=0\n"
                       "hint=reconfigure with XV6_KMEMLEAK=ON\n";
    size_t i = 0;

    if (buf == 0 || size == 0)
        return 0;
    while (i + 1 < size && msg[i] != '\0') {
        buf[i] = msg[i];
        i++;
    }
    buf[i] = '\0';
    return i;
}
#endif

#endif /* __KERNEL_MM_KMEMLEAK_H */
