#ifndef __KERNEL_RISCV_CACHE_H
#define __KERNEL_RISCV_CACHE_H

#include "types.h"

#define CBOM_BLOCK_SIZE 64

static inline void __cbo_clean(unsigned long addr) {
    asm volatile(".insn i 0x0F, 2, x0, %0, 1" ::"r"(addr) : "memory");
}

static inline void __cbo_inval(unsigned long addr) {
    asm volatile(".insn i 0x0F, 2, x0, %0, 0" ::"r"(addr) : "memory");
}

static inline void __cbo_flush(unsigned long addr) {
    asm volatile(".insn i 0x0F, 2, x0, %0, 2" ::"r"(addr) : "memory");
}

static inline void dma_cache_flush(void *addr, uint64 size) {
    unsigned long start = (unsigned long)addr & ~(CBOM_BLOCK_SIZE - 1);
    unsigned long end = (unsigned long)addr + size;
    for (; start < end; start += CBOM_BLOCK_SIZE) {
        __cbo_flush(start);
    }
}

static inline void dma_cache_clean(void *addr, uint64 size) {
    unsigned long start = (unsigned long)addr & ~(CBOM_BLOCK_SIZE - 1);
    unsigned long end = (unsigned long)addr + size;
    for (; start < end; start += CBOM_BLOCK_SIZE) {
        __cbo_clean(start);
    }
}

static inline void dma_cache_inval(void *addr, uint64 size) {
    unsigned long start = (unsigned long)addr & ~(CBOM_BLOCK_SIZE - 1);
    unsigned long end = (unsigned long)addr + size;
    for (; start < end; start += CBOM_BLOCK_SIZE) {
        __cbo_inval(start);
    }
}

#endif /* __KERNEL_RISCV_CACHE_H */
