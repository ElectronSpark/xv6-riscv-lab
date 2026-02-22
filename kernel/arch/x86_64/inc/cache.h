#ifndef __KERNEL_X86_64_CACHE_H
#define __KERNEL_X86_64_CACHE_H

#include "types.h"

static inline void dma_cache_flush(void *addr, uint64 size) {
    (void)addr;
    (void)size;
}

static inline void dma_cache_clean(void *addr, uint64 size) {
    (void)addr;
    (void)size;
}

static inline void dma_cache_inval(void *addr, uint64 size) {
    (void)addr;
    (void)size;
}

#endif /* __KERNEL_X86_64_CACHE_H */
