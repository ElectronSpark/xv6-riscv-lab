/**
 * @file early_allocator.h
 * @brief Early boot memory allocator
 *
 * Simple bump allocator for use before the full kernel memory allocator
 * is initialized. Used primarily to dynamically allocate the page array
 * based on detected physical memory size.
 *
 * Key characteristics:
 *   - Memory is NOT freeable (bump allocator)
 *   - Only used by the init hart during early boot
 *   - Operates on physical memory after kernel BSS
 *   - Allocations are contiguous and permanent
 *
 * Typical usage sequence:
 *   1. early_allocator_init(end, physical_memory_end)
 *   2. pages = early_alloc_align(page_array_size, PGSIZE)
 *   3. managed_start = early_alloc_end_ptr()
 *   4. buddy_init() uses managed_start as first allocatable page
 */

#ifndef __KERNEL_EARLY_ALLOCATOR_H
#define __KERNEL_EARLY_ALLOCATOR_H

#include "compiler.h"
#include "types.h"

/**
 * @brief Initialize the early allocator
 * @param pa_start Start of available physical memory (typically kernel end)
 * @param pa_end End of physical memory
 */
void early_allocator_init(void *pa_start, void *pa_end);

/**
 * @brief Allocate memory from early pool
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory (never NULL - panics on failure)
 */
void *early_alloc(size_t size);

/**
 * @brief Allocate aligned memory from early pool
 * @param size Number of bytes to allocate
 * @param align Alignment requirement (must be power of 2)
 * @return Pointer to aligned allocated memory
 */
void *early_alloc_align(size_t size, size_t align);

/**
 * @brief Get current end of early allocations
 * @return Pointer to first byte after all early allocations
 */
void *early_alloc_end_ptr(void);

#endif /* __KERNEL_EARLY_ALLOCATOR_H */
