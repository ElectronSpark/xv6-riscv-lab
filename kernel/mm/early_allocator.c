/*
 * Early Allocator - Buddy-style memory allocator used during kernel
 * initialization
 *
 * DESIGN OVERVIEW:
 * ================
 * This allocator manages memory before the full buddy system is initialized.
 * It uses order-based free lists and an advancing pointer strategy.
 *
 * KEY FEATURES:
 * - Small objects (≤ 64KB): Allocated from buddy-style free lists, aligned to
 * their size
 * - Large objects (> 64KB): Allocated by advancing pointer, with user-specified
 * alignment
 * - Alignment gaps are recycled as properly aligned power-of-2 chunks
 * - Returns the end pointer for buddy system initialization
 *
 * ALLOCATION STRATEGY:
 * - Small allocations: Ignore user alignment, align to chunk size (power of 2)
 * - Large allocations: Respect user alignment requirement
 * - Free lists maintain chunks from 2^5 (32 bytes) to 2^16 (64KB)
 * - Larger chunks are split buddy-style when needed
 * - Alignment gaps are broken into aligned chunks and added to free lists
 * - Too-small fragments (< 32 bytes) are discarded
 */

#include "types.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "string.h"
#include "list.h"

// Magic number for chunk corruption detection
#define EARLYALLOC_CHUNK_MAGIC 0xEAACCCCCEAACCCCCL

// Order bounds: 2^5 (32 bytes) to 2^16 (64KB)
#define EARLYALLOC_SMALLEST_ORDER 5 // 32 bytes
#define EARLYALLOC_SMALLEST_CHUNK (1UL << EARLYALLOC_SMALLEST_ORDER)
#define EARLYALLOC_LARGEST_ORDER 16 // 64KB
#define EARLYALLOC_LARGEST_CHUNK (1UL << EARLYALLOC_LARGEST_ORDER)
#define EARLYALLOC_ORDERS                                                      \
    (EARLYALLOC_LARGEST_ORDER - EARLYALLOC_SMALLEST_ORDER + 1)

// Alignment utilities
#define EARLYALLOC_ALIGN_MASK(align) ((align) - 1)
#define EARLYALLOC_ALIGN(x, align)                                             \
    ((typeof(x))(((uint64)(x) + EARLYALLOC_ALIGN_MASK(align)) &                \
                 ~EARLYALLOC_ALIGN_MASK(align)))

/*
 * Chunk structure - represents a free memory block
 * Must be power-of-2 sized and aligned to its size for buddy system
 * compatibility
 */
struct earalloc_chunk {
    uint64 magic;           // Magic number for corruption detection
    size_t size;            // Actual size of this chunk (must be power of 2)
    list_node_t list_entry; // Linkage for free list
};

/*
 * Allocator state
 * - free_lists[i]: Free chunks of size 2^(i + EARLYALLOC_SMALLEST_ORDER)
 * - current: Next address to allocate from (advancing pointer)
 * - end: Upper bound of allocatable memory
 */
static struct earalloc_params {
    list_node_t free_lists[EARLYALLOC_ORDERS]; // Free lists for each order
    void *current; // Current position for advancing allocator
    void *end;     // End of early allocator memory region
} earalloc_params;

/*
 * Calculate the order (log2 rounded up) of a size
 * Used to determine which free list a chunk belongs to
 */
static inline int __size_to_order(size_t size) {
    int order = 0;
    size_t s = size;
    while (s > (1UL << order)) {
        order++;
    }
    return order;
}

/*
 * Create a chunk structure at the given address
 * The caller must ensure the address is properly aligned to the size
 */
static struct earalloc_chunk *__make_chunk(void *addr, size_t size) {
    struct earalloc_chunk *chunk = (struct earalloc_chunk *)addr;
    chunk->magic = EARLYALLOC_CHUNK_MAGIC;
    chunk->size = size;
    list_entry_init(&chunk->list_entry);
    return chunk;
}

/*
 * Add a chunk to the appropriate free list based on its size
 *
 * REQUIREMENT: The chunk MUST be properly aligned to its size
 * This is critical for buddy system compatibility - each chunk of size 2^n
 * must start at an address that is a multiple of 2^n
 */
static void __add_chunk_to_freelist(struct earalloc_chunk *chunk) {
    int order = __size_to_order(chunk->size);
    int list_idx = order - EARLYALLOC_SMALLEST_ORDER;

    // Verify the chunk is properly aligned to its size
    assert(((uint64)chunk & (chunk->size - 1)) == 0,
           "__add_chunk_to_freelist: chunk not aligned to its size");

    if (list_idx >= 0 && list_idx < EARLYALLOC_ORDERS) {
        list_entry_push_back(&earalloc_params.free_lists[list_idx],
                             &chunk->list_entry);
    }
}

/*
 * Convert a memory region into properly aligned power-of-2 chunks
 *
 * STRATEGY:
 * 1. Find the largest power-of-2 chunk that fits in the remaining space
 * 2. Check if it's properly aligned to its size
 * 3. If aligned and within bounds, create chunk and add to free list
 * 4. If not aligned, try smaller size
 * 5. Discard fragments smaller than EARLYALLOC_SMALLEST_CHUNK
 *
 * This ensures all freed chunks are properly aligned for buddy system reuse
 */
static void __free_region_to_chunks(uint64 start, uint64 end) {
    while (start < end) {
        size_t remaining = end - start;

        // Find the largest power-of-2 chunk that fits
        int order = __size_to_order(remaining);
        if ((1UL << order) > remaining) {
            order--;
        }

        // Make sure the chunk is aligned to its size and within bounds
        size_t chunk_size = 1UL << order;
        while (order >= EARLYALLOC_SMALLEST_ORDER) {
            chunk_size = 1UL << order;

            // Check if this chunk is properly aligned
            if ((start & (chunk_size - 1)) == 0) {
                // Aligned! Create the chunk
                if (order <= EARLYALLOC_LARGEST_ORDER) {
                    struct earalloc_chunk *chunk =
                        __make_chunk((void *)start, chunk_size);
                    __add_chunk_to_freelist(chunk);
                    start += chunk_size;
                    break;
                }
            }

            // Not aligned or too large, try smaller size
            order--;
        }

        // If we couldn't create a chunk (too small or couldn't align), skip
        // this byte
        if (order < EARLYALLOC_SMALLEST_ORDER) {
            start++;
        }
    }
}

void early_allocator_init(void *pa_start, void *pa_end) {
    assert(pa_start != NULL && pa_end != NULL && pa_end > pa_start,
           "early_allocator_init: invalid memory range, start: %p, end: %p",
           pa_start, pa_end);

    // Initialize all free lists
    for (int i = 0; i < EARLYALLOC_ORDERS; i++) {
        list_entry_init(&earalloc_params.free_lists[i]);
    }

    // Align the start to the smallest chunk size
    uint64 start_aligned =
        EARLYALLOC_ALIGN((uint64)pa_start, EARLYALLOC_SMALLEST_CHUNK);
    uint64 end = (uint64)pa_end;

    assert(start_aligned < end,
           "early_allocator_init: invalid memory range after alignment");

    earalloc_params.current = (void *)start_aligned;
    earalloc_params.end = (void *)end;
}

/*
 * Get a chunk from free lists, splitting larger chunks if necessary
 *
 * BUDDY SYSTEM SPLITTING:
 * 1. First check if a chunk of exact size exists in the target order's free
 * list
 * 2. If not, find the smallest larger chunk available
 * 3. Split the larger chunk recursively:
 *    - Split chunk into two halves (buddies)
 *    - Add upper half to free list for its size
 *    - Continue splitting lower half until reaching target size
 * 4. Return the final chunk of target size
 *
 * All chunks are guaranteed to be aligned to their size
 */
static struct earalloc_chunk *__get_chunk_from_freelist(int target_order) {
    int list_idx = target_order - EARLYALLOC_SMALLEST_ORDER;

    if (list_idx < 0 || list_idx >= EARLYALLOC_ORDERS) {
        return NULL;
    }

    // Try to find a chunk in the target order's free list
    if (!LIST_IS_EMPTY(&earalloc_params.free_lists[list_idx])) {
        list_node_t *entry =
            list_entry_pop_back(&earalloc_params.free_lists[list_idx]);
        struct earalloc_chunk *chunk =
            container_of(entry, struct earalloc_chunk, list_entry);
        assert(chunk->magic == EARLYALLOC_CHUNK_MAGIC,
               "__get_chunk_from_freelist: corrupted chunk");
        return chunk;
    }

    // Try to find a larger chunk and split it buddy-style
    for (int order = target_order + 1; order <= EARLYALLOC_LARGEST_ORDER;
         order++) {
        int idx = order - EARLYALLOC_SMALLEST_ORDER;
        if (idx >= EARLYALLOC_ORDERS) {
            break;
        }

        if (!LIST_IS_EMPTY(&earalloc_params.free_lists[idx])) {
            // Found a larger chunk, split it down to the target order
            list_node_t *entry =
                list_entry_pop_back(&earalloc_params.free_lists[idx]);
            struct earalloc_chunk *chunk =
                container_of(entry, struct earalloc_chunk, list_entry);
            assert(chunk->magic == EARLYALLOC_CHUNK_MAGIC,
                   "__get_chunk_from_freelist: corrupted chunk");

            // Split the chunk down to target order
            while (order > target_order) {
                order--;
                size_t half_size = 1UL << order;

                // Create a buddy chunk from the upper half
                struct earalloc_chunk *buddy = __make_chunk(
                    (void *)((uint64)chunk + half_size), half_size);
                __add_chunk_to_freelist(buddy);

                // Update the current chunk to be the lower half
                chunk->size = half_size;
            }

            return chunk;
        }
    }

    return NULL;
}

/*
 * Allocate memory by advancing the current pointer
 * Used for large allocations (> 64KB) that exceed the largest chunk size
 *
 * PROCESS:
 * 1. Align current pointer to requested alignment
 * 2. Allocate the requested size at aligned address
 * 3. Convert any alignment gap into properly aligned chunks
 * 4. Add salvaged chunks to appropriate free lists
 * 5. Advance current pointer past the allocation
 *
 * This respects user-specified alignment for large allocations
 */
static void *__alloc_by_advancing(size_t size, size_t align) {
    uint64 current = (uint64)earalloc_params.current;
    uint64 aligned_addr = EARLYALLOC_ALIGN(current, align);
    uint64 end_addr = aligned_addr + size;

    if (end_addr > (uint64)earalloc_params.end) {
        panic("early_alloc_align: out of memory");
        return NULL;
    }

    // Free any unused space before the aligned address as properly aligned
    // chunks
    if (aligned_addr > current) {
        __free_region_to_chunks(current, aligned_addr);
    }

    earalloc_params.current = (void *)end_addr;
    return (void *)aligned_addr;
}

/*
 * Main allocation function with alignment support
 *
 * ALLOCATION POLICY:
 *
 * Small objects (size ≤ 64KB after rounding to power of 2):
 *   - Ignore user-specified alignment
 *   - Align to chunk size (power of 2)
 *   - Try to allocate from buddy-style free lists
 *   - If no free chunk, advance current pointer with chunk-size alignment
 *   - Recycle alignment gaps as properly aligned chunks
 *
 * Large objects (size > 64KB):
 *   - Respect user-specified alignment
 *   - Allocate by advancing current pointer
 *   - Recycle alignment gaps as properly aligned chunks
 *
 * This dual strategy ensures small objects follow buddy system rules
 * while large allocations can have custom alignment requirements
 */
void *early_alloc_align(size_t size, size_t align) {
    assert(align > 0 && (align & (align - 1)) == 0,
           "early_alloc_align: alignment must be a power of 2");

    if (size == 0)
        return NULL;

    // Calculate the required chunk order
    size_t chunk_size = size;
    if (chunk_size < EARLYALLOC_SMALLEST_CHUNK) {
        chunk_size = EARLYALLOC_SMALLEST_CHUNK;
    }

    // Round up to next power of 2
    int order = __size_to_order(chunk_size);
    size_t actual_size = 1UL << order;

    // For small objects (within the largest chunk size), use buddy system
    if (actual_size <= EARLYALLOC_LARGEST_CHUNK &&
        order <= EARLYALLOC_LARGEST_ORDER) {
        // Small object: ignore user alignment, align to chunk size
        struct earalloc_chunk *chunk = __get_chunk_from_freelist(order);
        if (chunk != NULL) {
            // Chunk is already aligned to its size (verified in
            // __add_chunk_to_freelist)
            return (void *)chunk;
        }

        // No free chunk available, allocate by advancing
        // Need to find an address aligned to the chunk size
        uint64 current = (uint64)earalloc_params.current;
        uint64 aligned_addr = EARLYALLOC_ALIGN(current, actual_size);
        uint64 end_addr = aligned_addr + actual_size;

        if (end_addr > (uint64)earalloc_params.end) {
            panic("early_alloc_align: out of memory");
            return NULL;
        }

        // Free any unused space before the aligned address
        if (aligned_addr > current) {
            __free_region_to_chunks(current, aligned_addr);
        }

        earalloc_params.current = (void *)end_addr;
        return (void *)aligned_addr;
    }

    // For large objects (larger than largest chunk), allocate with
    // user-specified alignment
    return __alloc_by_advancing(size, align);
}

void *early_alloc(size_t size) {
    return early_alloc_align(size, EARLYALLOC_SMALLEST_CHUNK);
}

void *early_alloc_end_ptr(void) { return earalloc_params.current; }
