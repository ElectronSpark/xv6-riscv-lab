/**
 * @file cache.h
 * @brief RISC-V cache management operations (Zicbom extension)
 *
 * Provides DMA cache maintenance using cbo.clean / cbo.inval / cbo.flush
 * instructions from the Zicbom extension.  The SpacemiT X1 (and other
 * modern RISC-V SoCs) advertises Zicbom support via the device tree
 * with `riscv,cbom-block-size = <64>`.
 *
 * These operations are required for non-coherent DMA on real hardware.
 * QEMU's emulated devices don't need them (memory is coherent), but
 * adding a no-op fallback keeps the driver portable.
 */
#ifndef __KERNEL_CACHE_H
#define __KERNEL_CACHE_H

#include "types.h"

/*
 * Cache block size for Zicbom operations.
 * Must match riscv,cbom-block-size from the device tree.
 * The SpacemiT X1 uses 64-byte cache lines.
 */
#define CBOM_BLOCK_SIZE 64

/**
 * cbo.clean — write-back dirty cache lines (does NOT invalidate).
 * Use before DMA-to-device: ensures device sees latest CPU writes.
 *
 * Encoding: cbo.clean (rs1)  →  0000000 00001 rs1 010 00000 0001111
 * We use .insn to encode since older toolchains may not recognize cbo.*.
 */
static inline void __cbo_clean(unsigned long addr) {
    asm volatile(".insn i 0x0F, 2, x0, %0, 1" ::"r"(addr) : "memory");
}

/**
 * cbo.inval — invalidate cache lines (drops dirty data!).
 * Use after DMA-from-device: ensures CPU sees what device wrote.
 *
 * Encoding: cbo.inval (rs1)  →  0000000 00000 rs1 010 00000 0001111
 */
static inline void __cbo_inval(unsigned long addr) {
    asm volatile(".insn i 0x0F, 2, x0, %0, 0" ::"r"(addr) : "memory");
}

/**
 * cbo.flush — write-back AND invalidate (clean + inval).
 * Use for bidirectional DMA or when in doubt.
 *
 * Encoding: cbo.flush (rs1)  →  0000000 00010 rs1 010 00000 0001111
 */
static inline void __cbo_flush(unsigned long addr) {
    asm volatile(".insn i 0x0F, 2, x0, %0, 2" ::"r"(addr) : "memory");
}

/**
 * Flush (write-back + invalidate) a range of memory.
 * Suitable for DMA descriptors and TX/RX buffers.
 */
static inline void dma_cache_flush(void *addr, uint64 size) {
    unsigned long start = (unsigned long)addr & ~(CBOM_BLOCK_SIZE - 1);
    unsigned long end = (unsigned long)addr + size;
    for (; start < end; start += CBOM_BLOCK_SIZE) {
        __cbo_flush(start);
    }
}

/**
 * Clean (write-back) a range — use before DMA TX (CPU → device).
 */
static inline void dma_cache_clean(void *addr, uint64 size) {
    unsigned long start = (unsigned long)addr & ~(CBOM_BLOCK_SIZE - 1);
    unsigned long end = (unsigned long)addr + size;
    for (; start < end; start += CBOM_BLOCK_SIZE) {
        __cbo_clean(start);
    }
}

/**
 * Invalidate a range — use after DMA RX (device → CPU).
 * WARNING: any dirty data in these cache lines is lost.
 *          Only safe if the range is not shared with other data.
 */
static inline void dma_cache_inval(void *addr, uint64 size) {
    unsigned long start = (unsigned long)addr & ~(CBOM_BLOCK_SIZE - 1);
    unsigned long end = (unsigned long)addr + size;
    for (; start < end; start += CBOM_BLOCK_SIZE) {
        __cbo_inval(start);
    }
}

#endif /* __KERNEL_CACHE_H */
