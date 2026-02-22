/**
 * @file cache.h
 * @brief Architecture-agnostic DMA cache maintenance API.
 *
 * Shared kernel code should include this header and use:
 *   - dma_cache_clean()
 *   - dma_cache_inval()
 *   - dma_cache_flush()
 *
 * Architecture-specific semantics and instructions are hidden in
 * kernel/inc/arch/cache.h and the corresponding arch private headers.
 */
#ifndef __KERNEL_CACHE_H
#define __KERNEL_CACHE_H

#include "arch/cache.h"

#endif /* __KERNEL_CACHE_H */
