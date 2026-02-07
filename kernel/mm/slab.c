// SLAB allocator for kernel objects smaller than a single page.
//
// The SLAB allocator manages small kernel objects efficiently by grouping them
// into slabs (groups of contiguous pages). This reduces internal fragmentation
// and provides fast allocation/deallocation for frequently used objects.
//
// ARCHITECTURE:
//   - SLAB Cache: Collection of slabs for objects of the same size
//   - SLAB: Group of one or more pages containing objects of uniform size
//   - Objects: Fixed-size allocations managed within slabs
//   - Per-CPU Caches: Each CPU maintains its own partial and full lists
//
// KEY FEATURES:
//   - Per-CPU slab lists for scalable concurrent access
//   - Global free list shared across all CPUs
//   - Adaptive slab sizing: slab order chosen based on object size
//   - Try-lock optimization to reduce lock contention
//   - Embedded or separate slab descriptors based on object size
//   - Free list per slab for fast object allocation
//   - Optional bitmap tracking for debugging (SLAB_FLAG_DEBUG_BITMAP)
//   - Automatic slab shrinking when free objects exceed limits
//   - Emergency memory reclaim: on OOM, all caches are shrunk before retry
//
// SLAB STATES:
//   - FREE: All objects available (in global_free_list)
//   - PARTIAL: Some objects allocated (in per-CPU partial_list)
//   - FULL: All objects allocated (in per-CPU full_list)
//   - DEQUEUED: Temporarily removed from lists during operations
//
// BITMAP TRACKING (optional):
//   When SLAB_FLAG_DEBUG_BITMAP is set, each slab maintains a bitmap where
//   each bit tracks whether an object is allocated (1) or free (0). This
//   provides runtime detection of:
//   - Double allocation: Panic if allocating an already-allocated object
//   - Double free: Panic if freeing an already-free object
//   - Memory overhead: 1 bit per object (minimal impact)
//
// ALLOCATION FLOW:
//   1. Try local CPU partial_list (fast path, per-CPU lock)
//   2. If empty, take from global_free_list (global lock)
//   3. If empty, create new slab (no lock)
//   4. Get object from slab's free list
//   5. Update bitmap if enabled
//   6. Move slab between lists if state changed
//
// DEALLOCATION FLOW:
//   1. Find slab from object address via page descriptor
//   2. Determine owner CPU from slab->cpu_id
//   3. Acquire owner CPU's lock
//   4. Verify bitmap and clear bit if enabled
//   5. Return object to slab's free list
//   6. Move slab to appropriate list based on state
//   7. Special case: cross-CPU free from full→partial stays in owner's partial list
//
// LOCKING:
//   - Per-CPU locks: Protect each CPU's partial and full lists
//   - Global free lock: Protects shared free list
//   - Lock hierarchy: Per-CPU locks → Global free lock
//   - Try-lock optimization: Check state without lock, then lock and double-check
//   - Cross-CPU frees: Acquire target CPU's lock (by CPU ID to prevent deadlock)
//
#include "types.h"
#include "param.h"
#include <mm/memlayout.h>
#include "lock/spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "string.h"
#include <mm/page.h>
#include "list.h"
#include <mm/slab.h>
#include "slab_private.h"
#include <smp/percpu.h>

// ============================================================================
// Global Slab Cache Registry
// ============================================================================
// All slab caches are registered here so we can shrink them all on OOM.
static list_node_t __all_slab_caches = LIST_ENTRY_INITIALIZED(__all_slab_caches);
static spinlock_t __all_slab_caches_lock = SPINLOCK_INITIALIZED("all_slab_caches");

// Shrink all registered slab caches - called on OOM
void slab_shrink_all(void) {
    slab_cache_t *cache, *tmp;
    
    spin_lock(&__all_slab_caches_lock);
    list_foreach_node_safe(&__all_slab_caches, cache, tmp, cache_list_entry) {
        // Shrink up to half the global free slabs
        int64 free_count = __atomic_load_n(&cache->global_free_count, __ATOMIC_ACQUIRE);
        if (free_count > 0) {
            int to_shrink = (free_count + 1) / 2;
            if (to_shrink > 0) {
                spin_unlock(&__all_slab_caches_lock);
                slab_cache_shrink(cache, to_shrink);
                spin_lock(&__all_slab_caches_lock);
                // tmp may be invalid after releasing lock, restart from beginning
                cache = LIST_FIRST_NODE(&__all_slab_caches, slab_cache_t, cache_list_entry);
                if (cache == NULL) break;
                tmp = LIST_NEXT_NODE(&__all_slab_caches, cache, cache_list_entry);
            }
        }
    }
    spin_unlock(&__all_slab_caches_lock);
}

// Dump statistics for all slab caches - useful for debugging memory leaks
uint64 slab_dump_all(int detailed) {
    slab_cache_t *cache, *tmp;
    uint64 total_pages = 0;
    uint64 total_bytes;

    if (detailed >= 2) {
        printf("\n=== SLAB CACHE STATISTICS ===\n");
        printf("NAME             OBJSZ    TOTAL   ACTIVE     FREE    PAGES\n");
    }
    
    spin_lock(&__all_slab_caches_lock);
    list_foreach_node_safe(&__all_slab_caches, cache, tmp, cache_list_entry) {
        int64 slab_total = __atomic_load_n(&cache->slab_total, __ATOMIC_ACQUIRE);
        uint64 obj_active = __atomic_load_n(&cache->obj_active, __ATOMIC_ACQUIRE);
        int64 global_free = __atomic_load_n(&cache->global_free_count, __ATOMIC_ACQUIRE);
        uint64 pages = slab_total * (1UL << cache->slab_order);
        total_pages += pages;
        
        if (detailed >= 2) {
            printf("%s: objsz=%ld total=%ld active=%lu free=%ld pages=%lu\n",
                   cache->name, cache->obj_size, slab_total, obj_active, global_free, pages);
        }
    }
    spin_unlock(&__all_slab_caches_lock);
    
    if (detailed >= 2) {
        printf("-----------------------------\n");
    }
    total_bytes = total_pages * PAGE_SIZE;
    if (detailed >= 1) {
        printf("Slab:  %lu pages (", total_pages);
        if (total_bytes >= 1024 * 1024) {
            uint64 mb = total_bytes / (1024 * 1024);
            uint64 kb_remainder = (total_bytes % (1024 * 1024)) / 1024;
            printf("%lu.%luMB", mb, kb_remainder * 10 / 1024);
        } else {
            printf("%luKB", total_bytes / 1024);
        }
        printf(")\n");
        if (detailed >= 2) {
            printf("=============================\n");
        }
    }

    return total_bytes;
}

#ifdef KERNEL_PAGE_SANITIZER
STATIC_INLINE void __slab_sanitizer_check(const char *op ,slab_cache_t *cache, slab_t *slab,
                                          void *obj) {
    printf("%s: cache \"%s\" (%p), obj 0x%lx, size: %ld\n",
           op, cache->name, cache, (uint64)obj, cache->obj_size);
}
#else
#define __slab_sanitizer_check(op, page, order, flags) do { } while (0)
#endif

// ============================================================================
// SLAB Lifecycle Management
// ============================================================================

// create a detached SLAB and initialize its objects
// return the SLAB created if success
// return NULL if failed
STATIC_INLINE slab_t *__slab_make(uint64 flags, uint32 order, size_t offs,
                                  size_t obj_size, uint32 obj_num, uint32 bitmap_size) {
    page_t *page;
    void *page_base, **prev, **tmp;
    slab_t *slab;

    page = __page_alloc(order, PAGE_TYPE_SLAB);
    if (page == NULL) {
        // Emergency reclaim: try shrinking ALL slab caches to free pages
        // This handles OOM during stress tests (e.g., forkforkfork) where
        // many processes exit and their slabs are freed but not yet shrunk.
        // Note: slab_shrink_all() covers all registered caches including
        // specialized ones like __vm_pool, __vma_pool, etc., not just kmm caches.
        slab_shrink_all();
        page = __page_alloc(order, PAGE_TYPE_SLAB);
    }
    if (page == NULL) {
        // Still failed after emergency shrink - truly out of memory
        return NULL;
    }
    page_base = (void *)__page_to_pa(page);
    if (page_base == NULL) {
        panic("__slab_make");
    }
    
    // Set the page's slab order immediately so that if we need to free
    // the page on a subsequent failure (e.g., kmm_alloc failure), the
    // __page_free assertion check will see the correct order.
    page[0].slab.order = order;
    
    if (flags & SLAB_FLAG_EMBEDDED) {
        // Embedded SLAB puts its descriptor at the start of its cache page
        slab = page_base;
    } else if ((slab = slab_t_desc_alloc()) == NULL){
        __page_free(page, order);
        return NULL;
    }
    // Header page gets PAGE_TYPE_SLAB with slab pointer and order.
    // After buddy merging or when coming from per-CPU cache, tail pages may
    // point to wrong headers or have wrong types. We must update all tails.
    page[0].slab.slab = slab;
    
    // Ensure all tail pages have correct type and point to this header
    uint64 page_count = 1UL << order;
    for (uint64 i = 1; i < page_count; i++) {
        page[i].flags = PAGE_TYPE_TAIL;
        page[i].tail.head_page = &page[0];
    }

    slab->cache = NULL;
    slab->slab_order = order;
    slab->in_use = 0;
    slab->page = page;
    slab->state = SLAB_STATE_DEQUEUED;
    slab->bitmap = NULL;
    __atomic_store_n(&slab->cpu_id, -1, __ATOMIC_RELEASE);  // Initially unowned
    list_entry_init(&slab->list_entry);

    // Allocate bitmap if bitmap tracking is enabled
    if (bitmap_size > 0) {
        slab->bitmap = kmm_alloc(bitmap_size * sizeof(uint64));
        if (slab->bitmap == NULL) {
            // Failed to allocate bitmap, cleanup and return
            if ((uint64)slab != (uint64)page_base) {
                slab_t_desc_free(slab);
            }
            __page_free(page, order);
            return NULL;
        }
        // Initialize bitmap to all zeros (all objects free)
        for (uint32 i = 0; i < bitmap_size; i++) {
            slab->bitmap[i] = 0;
        }
    }

    prev = NULL;
    tmp = page_base + offs;
    for (int i = 0; i < obj_num; i++) {
        *tmp = prev;
        prev = tmp;
        tmp = (void *)tmp + obj_size;
    }

    slab->next = prev;
    return slab;
}

// Destroy an empty and detached SLAB
STATIC_INLINE void __slab_destroy(slab_t *slab) {
    page_t *page;
    uint16 order;
    uint64 page_base;
    if (slab == NULL) {
        return;
    }
    if (__SLAB_ATTACHED(slab)) {
        panic("__slab_destroy(): destroy an attached SLAB");
    }
    if (!__SLAB_EMPTY(slab)) {
        panic("__slab_destroy(): destroy a non-empty SLAB");
    }
    page = slab->page;
    order = slab->slab_order;
    page_base = __page_to_pa(page);
    if (page_base == 0) {
        panic("__slab_destroy");
    }
    // Free bitmap if it was allocated
    if (slab->bitmap != NULL) {
        kmm_free(slab->bitmap);
        slab->bitmap = NULL;
    }
    if ((uint64)slab != page_base) {
        slab_t_desc_free(slab);
    }
    __page_free(page, order);
}

// ============================================================================
// SLAB Attachment/Detachment
// ============================================================================

// Attach an empty SLAB to a SLAB cache
// SLAB must be enqueued after attaching
STATIC_INLINE void __slab_attach(slab_cache_t *cache, slab_t *slab) {
    if (!LIST_NODE_IS_DETACHED(slab, list_entry)) {
        panic("__slab_attach(): SLAB cannot be attached when in a queue");
    }
    if (slab->slab_order != cache->slab_order) {
        panic("__slab_attach(): wrong order");
    }
    if (__SLAB_ATTACHED(slab)) {
        panic("__slab_attach(): attach an attached SLAB");
    }
    if (!__SLAB_EMPTY(slab)) {
        panic("__slab_attach(): attach a non-empty SLAB");
    }
    slab->cache = cache;
    __atomic_fetch_add(&cache->slab_total, 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&cache->obj_total, cache->slab_obj_num, __ATOMIC_RELEASE);
}

// Detach an empty SLAB from its SLAB cache
// SLAB must be dequeued before detaching
STATIC_INLINE void __slab_detach(slab_cache_t *cache, slab_t *slab) {
    int64 slab_total_val;
    uint64 obj_total_val;

    if (!LIST_NODE_IS_DETACHED(slab, list_entry)) {
        panic("__slab_detach(): SLAB cannot be detached when in a queue");
    }
    if (slab->cache != cache) {
        panic("__slab_detach(): wrong SLAB cache");
    }
    if (!__SLAB_EMPTY(slab)) {
        panic("__slab_detach(): detach non-empty SLAB");
    }

    // Read atomic counters for validation
    slab_total_val = __atomic_load_n(&cache->slab_total, __ATOMIC_ACQUIRE);
    obj_total_val = __atomic_load_n(&cache->obj_total, __ATOMIC_ACQUIRE);

    if (slab_total_val == 0 || obj_total_val < cache->slab_obj_num) {
        panic("__slab_detach(): counter error");
    }

    __atomic_fetch_sub(&cache->obj_total, cache->slab_obj_num, __ATOMIC_RELEASE);
    __atomic_fetch_sub(&cache->slab_total, 1, __ATOMIC_RELEASE);
    slab->cache = NULL;
}

// ============================================================================
// SLAB Queue Management (Per-CPU Architecture)
// ============================================================================
// NOTE: In the per-CPU architecture, queue management is handled directly
// in slab_alloc() and slab_free() using list operations and atomic counters.
// No separate enqueue/dequeue functions are needed.

// ============================================================================
// Bitmap Tracking (Optional Debug Feature)
// ============================================================================

// Test-and-set: Returns previous value (0 or 1) and sets the bit
// Returns -1 if bitmap is NULL or index out of range
STATIC_INLINE int __slab_bitmap_test_and_set(slab_t *slab, int idx) {
    if (slab->bitmap == NULL) {
        return -1;
    }
    if (idx < 0 || (slab->cache && idx >= slab->cache->slab_obj_num)) {
        return -1;
    }

    int word_idx = idx / 64;
    int bit_idx = idx % 64;
    uint64 mask = 1UL << bit_idx;

    // Get old value
    int old_val = !!(slab->bitmap[word_idx] & mask);

    // Set the bit
    slab->bitmap[word_idx] |= mask;

    return old_val;
}

// Test-and-clear: Returns previous value (0 or 1) and clears the bit
// Returns -1 if bitmap is NULL or index out of range
STATIC_INLINE int __slab_bitmap_test_and_clear(slab_t *slab, int idx) {
    if (slab->bitmap == NULL) {
        return -1;
    }
    if (idx < 0 || (slab->cache && idx >= slab->cache->slab_obj_num)) {
        return -1;
    }

    int word_idx = idx / 64;
    int bit_idx = idx % 64;
    uint64 mask = 1UL << bit_idx;

    // Get old value
    int old_val = !!(slab->bitmap[word_idx] & mask);

    // Clear the bit
    slab->bitmap[word_idx] &= ~mask;

    return old_val;
}

// ============================================================================
// SLAB Object Management
// ============================================================================

// Take a SLAB object out of its SLAB and increase the in_use counter of the
// SLAB
// No validity check
// Will not change the counter in its SLAB cache.
// Return the ptr to the object if success
// Return NULL if failed
STATIC_INLINE void *__slab_obj_get(slab_t *slab) {
    void *ret_ptr;
    ret_ptr = slab->next;
    if (ret_ptr != NULL) {
        slab->next = *(void **)ret_ptr;
        slab->in_use++;
        // Update bitmap if tracking is enabled
        if (slab->bitmap != NULL) {
            int idx = __slab_obj2idx(slab, ret_ptr);
            if (idx >= 0) {
                // Use test-and-set: should return 0 (was free), now set to 1 (allocated)
                int old_val = __slab_bitmap_test_and_set(slab, idx);
                assert(old_val == 0, "__slab_obj_get(): double allocation detected");
            }
        }
    }
    return ret_ptr;
}

// Put a SLAB object back to its SLAB and decrese the in_use counter of the
// SLAB
// No validity check
// Will not change the counter in its SLAB cache.
STATIC_INLINE void __slab_obj_put(slab_t *slab, void *ptr) {
    // Update bitmap if tracking is enabled
    if (slab->bitmap != NULL) {
        int idx = __slab_obj2idx(slab, ptr);
        if (idx >= 0) {
            // Use test-and-clear: should return 1 (was allocated), now cleared to 0 (free)
            int old_val = __slab_bitmap_test_and_clear(slab, idx);
            assert(old_val == 1, "__slab_obj_put(): double free detected");
        }
    }
    *(void**)ptr = slab->next;
    slab->next = ptr;
    slab->in_use--;
}

// Get the base address of an object giving its SLAB and its index
STATIC_INLINE void *__slab_idx2obj(slab_t *slab, int idx) {
    void *ret_ptr;
    void *page_base;
    if (!__SLAB_ATTACHED(slab)) {
        // We need the SLAB cache the SLAB attached to to determine some 
        // parameters.
        return NULL;
    }
    if (idx < 0 || idx >= slab->cache->slab_obj_num) {
        // object not in the range of the SLAB
        return NULL;
    }
    page_base = __SLAB_PAGE_BASE(slab);
    ret_ptr = page_base + slab->cache->offset;
    ret_ptr += idx * slab->cache->obj_size;
    return ret_ptr;
}

// Get the index of an object.
STATIC_INLINE int __slab_obj2idx(slab_t *slab, void *ptr) {
    size_t base_offs;
    void *page_base;
    int idx;
    if (ptr == NULL) {
        return -1;
    }
    if ((uint64)ptr & 7UL) {
        // all objects are aligned with 8 bytes
        return -1;
    }
    if (!(__SLAB_ATTACHED(slab))) {
        // We need the SLAB cache the SLAB attached to to determine some 
        // parameters.
        return -1;
    }
    page_base = __SLAB_PAGE_BASE(slab) + slab->cache->offset;
    if (ptr < page_base) {
        // object not in the range of the SLAB
        return -1;
    }
    base_offs = ptr - page_base;
    idx = base_offs / slab->cache->obj_size;
    if (idx >= slab->cache->slab_obj_num) {
        // object not in the range of the SLAB
        return -1;
    }
    return idx;
}

// find the SLAB of a object giving its address
// Handles both header pages (PAGE_TYPE_SLAB) and tail pages (PAGE_TYPE_TAIL)
STATIC_INLINE slab_t *__find_obj_slab(void *ptr) {
    uint64 page_base;
    page_t *page = NULL;
    page_t *header = NULL;

    if (ptr == NULL) {
        return NULL;
    }
    
    page_base = PGROUNDDOWN((uint64)ptr);
    page = __pa_to_page(page_base);
    if (page == NULL) {
        return NULL;
    }
    
    // Handle both header and tail pages
    if (PAGE_IS_TYPE(page, PAGE_TYPE_SLAB)) {
        // This is the header page - slab pointer is directly accessible
        header = page;
    } else if (PAGE_IS_TYPE(page, PAGE_TYPE_TAIL)) {
        // This is a tail page - follow head_page to get to header
        header = page->tail.head_page;
        if (header == NULL || !PAGE_IS_TYPE(header, PAGE_TYPE_SLAB)) {
            return NULL;
        }
    } else {
        // Neither SLAB nor TAIL - not a slab page
        return NULL;
    }
    
    if (header->slab.slab == NULL) {
        return NULL;
    }
    return header->slab.slab;
}

// ============================================================================
// SLAB Cache Locking
// ============================================================================

// Acquire lock for current CPU's cache
STATIC_INLINE void __percpu_cache_lock(slab_cache_t *cache) {
    int cpu_id = cpuid();
    spin_lock(&cache->percpu_caches[cpu_id].lock);
}

// Release lock for current CPU's cache
STATIC_INLINE void __percpu_cache_unlock(slab_cache_t *cache) {
    int cpu_id = cpuid();
    spin_unlock(&cache->percpu_caches[cpu_id].lock);
}

// Acquire lock for a specific CPU's cache
STATIC_INLINE void __percpu_cache_lock_cpu(slab_cache_t *cache, int cpu_id) {
    assert(cpu_id >= 0 && cpu_id < NCPU, "invalid cpu_id");
    spin_lock(&cache->percpu_caches[cpu_id].lock);
}

// Release lock for a specific CPU's cache
STATIC_INLINE void __percpu_cache_unlock_cpu(slab_cache_t *cache, int cpu_id) {
    assert(cpu_id >= 0 && cpu_id < NCPU, "invalid cpu_id");
    spin_unlock(&cache->percpu_caches[cpu_id].lock);
}

// Acquire global free list lock
STATIC_INLINE void __global_free_lock(slab_cache_t *cache) {
    spin_lock(&cache->global_free_lock);
}

// Release global free list lock
STATIC_INLINE void __global_free_unlock(slab_cache_t *cache) {
    spin_unlock(&cache->global_free_lock);
}

// ============================================================================
// SLAB Cache Initialization and Management
// ============================================================================

// Initialize a existing SLAB cache without checking
STATIC_INLINE void __slab_cache_init(slab_cache_t *cache, char *name,
                                     size_t obj_size, uint64 flags) {
    size_t offset = 0;
    uint32 limits;
    uint16 slab_obj_num;
    uint32 slab_order;

    // The size of each object must aligned to 8 bytes
    obj_size = ((obj_size + 7) >> 3) << 3;
    if (flags & SLAB_FLAG_EMBEDDED) {
        offset = __SLAB_OBJ_OFFSET(obj_size);
    }

    // Adaptive slab sizing: choose slab order based on object size to balance
    // memory efficiency and object capacity.
    //
    // Goals:
    //   1. Each slab should hold at least 8 objects (amortize metadata overhead)
    //   2. Avoid excessive memory waste (e.g., 1MB slabs for 840-byte objects)
    //   3. Prevent memory exhaustion in systems with limited RAM (e.g., 130MB)
    //
    // Strategy:
    //   - Small objects (≤128 bytes):  order 0 (4KB)   → 32-128 objects/slab
    //   - Small objects (≤512 bytes):  order 1 (8KB)   → 16-64 objects/slab
    //   - Medium objects (≤1024 bytes): order 2 (16KB) → 16-32 objects/slab
    //   - Large objects (≤2048 bytes):  order 3 (32KB) → 16-32 objects/slab
    //   - Very large objects (>2KB):    order 4 (64KB) → up to 32 objects/slab
    //
    // Example: For 840-byte sigacts objects:
    //   - Old fixed approach: order 8 (1MB) → 1248 objects, wastes 96% of memory
    //   - New adaptive approach: order 2 (16KB) → 19 objects, 64x reduction
    //
    if (obj_size <= 128) {
        slab_order = 0;  // 1 page = 4KB
    } else if (obj_size <= 512) {
        slab_order = 1;  // 2 pages = 8KB
    } else if (obj_size <= 1024) {
        slab_order = 2;  // 4 pages = 16KB
    } else if (obj_size <= 2048) {
        slab_order = 3;  // 8 pages = 32KB
    } else {
        slab_order = 4;  // 16 pages = 64KB
    }

    // Calculate number of objects that fit in this slab
    slab_obj_num = (uint16)__SLAB_ORDER_OBJS(slab_order, offset, obj_size);

    // If we got too few objects, try a larger slab (but cap at order 5 = 128KB)
    // This ensures we meet the minimum 8 objects/slab target
    while (slab_obj_num < 8 && slab_order < 5) {
        slab_order++;
        slab_obj_num = (uint16)__SLAB_ORDER_OBJS(slab_order, offset, obj_size);
    }

    limits = slab_obj_num * 4;

    // Calculate bitmap size if bitmap tracking is enabled
    uint32 bitmap_size = 0;
    if (flags & SLAB_FLAG_DEBUG_BITMAP) {
        // Calculate number of uint64 words needed to track all objects
        bitmap_size = (slab_obj_num + 63) / 64;
    }

    // memset(cache, 0, sizeof(slab_cache_t));
    cache->name = name;
    cache->flags = flags;
    cache->obj_size = obj_size;
    cache->offset = offset;
    cache->slab_order = slab_order;
    cache->slab_obj_num = slab_obj_num;
    cache->bitmap_size = bitmap_size;
    cache->limits = limits;

    // Initialize per-CPU caches
    for (int i = 0; i < NCPU; i++) {
        list_entry_init(&cache->percpu_caches[i].partial_list);
        list_entry_init(&cache->percpu_caches[i].full_list);
        __atomic_store_n(&cache->percpu_caches[i].partial_count, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&cache->percpu_caches[i].full_count, 0, __ATOMIC_RELEASE);
        // Use cache name for lock (can't format per-CPU names without snprintf)
        spin_init(&cache->percpu_caches[i].lock, name);
    }

    // Initialize global free list
    list_entry_init(&cache->global_free_list);
    spin_init(&cache->global_free_lock, "global_free");
    __atomic_store_n(&cache->global_free_count, 0, __ATOMIC_RELEASE);

    // Initialize atomic counters
    __atomic_store_n(&cache->slab_total, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&cache->obj_active, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&cache->obj_total, 0, __ATOMIC_RELEASE);

    // Register in global cache list for OOM shrinking
    list_entry_init(&cache->cache_list_entry);
    spin_lock(&__all_slab_caches_lock);
    // Insert at end of list (after the last entry, which is prev of head)
    list_entry_insert(LIST_LAST_ENTRY(&__all_slab_caches), &cache->cache_list_entry);
    spin_unlock(&__all_slab_caches_lock);

}


// Initialize a existing SLAB cache
int slab_cache_init(slab_cache_t *cache, char *name, size_t obj_size, 
                    uint64 flags) {
    if (cache == NULL) {
        return -1;
    }
    if (flags & (~(SLAB_FLAG_STATIC | SLAB_FLAG_EMBEDDED | SLAB_FLAG_DEBUG_BITMAP))) {
        // invalid flags
        return -1;
    }
    if (obj_size > SLAB_OBJ_MAX_SIZE) {
        // the size of objects is too large
        return -1;
    }
    if (obj_size < SLAB_OBJ_MIN_SIZE) {
        // round up the size of the object to 
        obj_size = SLAB_OBJ_MIN_SIZE;
    }
    __slab_cache_init(cache, name, obj_size, flags);
    return 0;
}

// create and initialize a SLAB cache
slab_cache_t *slab_cache_create(char *name, size_t obj_size, uint64 flags) {
    slab_cache_t *slab_cache = slab_cache_t_alloc();
    if (slab_cache == NULL) {
        return NULL;
    }
    if (slab_cache_init(slab_cache, name, obj_size, flags) != 0) {
        slab_cache_t_free(slab_cache);
        slab_cache = NULL;
    }
    return slab_cache;
}

// ============================================================================
// SLAB Cache Destruction and Shrinking
// ============================================================================

// destroy a slab cache
// only non-STATIC , empty SLAB cache can be freed
// return 0 if success
// return -1 if failed
int slab_cache_destroy(slab_cache_t *cache) {
    list_node_t tmp_list;
    int64 global_free_count;
    int shrink_ret;

    if (cache == NULL) {
        return -1;
    }

    if (cache->flags & SLAB_FLAG_STATIC) {
        // cannot destroy a STATIC SLAB
        return -1;
    }

    // Check if any CPU has partial or full slabs
    for (int i = 0; i < NCPU; i++) {
        uint32 partial_count = __atomic_load_n(&cache->percpu_caches[i].partial_count, __ATOMIC_ACQUIRE);
        uint32 full_count = __atomic_load_n(&cache->percpu_caches[i].full_count, __ATOMIC_ACQUIRE);

        if (partial_count != 0 || full_count != 0) {
            // will not allow to destroy a SLAB cache with allocated objects
            return -1;
        }
    }

    // Get global free count
    global_free_count = __atomic_load_n(&cache->global_free_count, __ATOMIC_ACQUIRE);

    list_entry_init(&tmp_list);
    shrink_ret = __slab_cache_shrink_unlocked(cache, global_free_count, &tmp_list);
    if (shrink_ret != global_free_count) {
        // failed to destroy all the SLABs
        __slab_cache_free_tmp_list(&tmp_list, shrink_ret);
        return -1;
    }
    __slab_cache_free_tmp_list(&tmp_list, shrink_ret);
    slab_cache_t_free(cache);
    return 0;
}

// try to detach empty SLABs from global free list
// SLABs detached here will not be freed immediately,
// but put into a temporary list for later freeing outside the global_free_lock.
// return the actual number of SLABs detached
// return -1 if failed.
STATIC_INLINE int __slab_cache_shrink_unlocked(slab_cache_t *cache, int nums, list_node_t *tmp_list) {
    int64 global_free_count, slab_free_after;
    int64 slab_total_before;
    int counter;
    slab_t *slab = NULL;

    if (cache == NULL || tmp_list == NULL) {
        return -1;
    }

    __global_free_lock(cache);

    global_free_count = __atomic_load_n(&cache->global_free_count, __ATOMIC_ACQUIRE);

    if (nums == 0 || nums > global_free_count) {
        slab_free_after = 0;
    } else {
        slab_free_after = global_free_count - nums;
    }

    counter = 0;
    while (__atomic_load_n(&cache->global_free_count, __ATOMIC_ACQUIRE) > slab_free_after) {
        if (LIST_IS_EMPTY(&cache->global_free_list)) {
            panic("__slab_cache_shrink_unlocked: list empty but count > 0");
        }

        slab = list_node_pop_back(&cache->global_free_list, slab_t, list_entry);
        if (slab == NULL) {
            panic("__slab_cache_shrink_unlocked: slab == NULL");
        }

        __atomic_fetch_sub(&cache->global_free_count, 1, __ATOMIC_RELEASE);

        slab_total_before = __atomic_load_n(&cache->slab_total, __ATOMIC_ACQUIRE);
        __slab_detach(cache, slab);

        if (__atomic_load_n(&cache->slab_total, __ATOMIC_ACQUIRE) >= slab_total_before) {
            panic("__slab_cache_shrink_unlocked: slab_total did not decrease");
        }

        list_node_push_back(tmp_list, slab, list_entry);
        counter++;
    }

    __global_free_unlock(cache);
    return counter;
}

// Free SLABs in the temporary list
// This function is called outside the SLAB cache lock
STATIC_INLINE void __slab_cache_free_tmp_list(list_node_t *tmp_list, int expected) {
    if (expected <= 0) {
        assert(LIST_IS_EMPTY(tmp_list), "__slab_cache_free_tmp_list: list not empty");
        return;
    }
    int counter = 0;
    slab_t *slab;
    slab_t *tmp;
    list_foreach_node_safe(tmp_list, slab, tmp, list_entry) {
        counter++;
        __slab_destroy(slab);
    }
    assert(counter == expected, "__slab_cache_free_tmp_list: counter mismatch");
}

// try to delete empty SLABs from global free list
// return the actual number of SLABs deleted
// return -1 if failed.
int slab_cache_shrink(slab_cache_t *cache, int nums) {
    int ret;
    list_node_t tmp_list;

    if (cache == NULL) {
        return -1;
    }

    list_entry_init(&tmp_list);
    ret = __slab_cache_shrink_unlocked(cache, nums, &tmp_list);
    __slab_cache_free_tmp_list(&tmp_list, ret);
    return ret;
}

// ============================================================================
// Public API: Object Allocation and Deallocation
// ============================================================================

// allocate an object from a SLAB cache
// return the base address of the object if success
// return NULL if failed
void *slab_alloc(slab_cache_t *cache) {
    void *obj = NULL;
    slab_t *slab = NULL;
    int cpu_id;
    percpu_slab_cache_t *pcpu_cache;

    if (cache == NULL) {
        return NULL;
    }

    push_off();
    cpu_id = cpuid();
    pcpu_cache = &cache->percpu_caches[cpu_id];

    // PHASE 1: Try local CPU partial list (FAST PATH)
    __percpu_cache_lock_cpu(cache, cpu_id);

    if (!LIST_IS_EMPTY(&pcpu_cache->partial_list)) {
        slab = LIST_FIRST_NODE(&pcpu_cache->partial_list, slab_t, list_entry);
        assert(slab != NULL && !__SLAB_FULL(slab), "partial list invariant");

        obj = __slab_obj_get(slab);

        if (__SLAB_FULL(slab)) {
            // Move from partial to full list
            list_node_detach(slab, list_entry);
            __atomic_fetch_sub(&pcpu_cache->partial_count, 1, __ATOMIC_RELEASE);
            list_node_push_back(&pcpu_cache->full_list, slab, list_entry);
            __atomic_fetch_add(&pcpu_cache->full_count, 1, __ATOMIC_RELEASE);
            slab->state = SLAB_STATE_FULL;
        }

        __atomic_fetch_add(&cache->obj_active, 1, __ATOMIC_RELEASE);
        __percpu_cache_unlock_cpu(cache, cpu_id);
        __slab_sanitizer_check("slab_alloc", cache, slab, obj);
        pop_off();
        return obj;
    }

    __percpu_cache_unlock_cpu(cache, cpu_id);

    // PHASE 2: Try to get slab from global free list
    __global_free_lock(cache);

    if (!LIST_IS_EMPTY(&cache->global_free_list)) {
        slab = list_node_pop_back(&cache->global_free_list, slab_t, list_entry);
        __atomic_fetch_sub(&cache->global_free_count, 1, __ATOMIC_RELEASE);
        __global_free_unlock(cache);

        // Take ownership of this slab
        __atomic_store_n(&slab->cpu_id, cpu_id, __ATOMIC_RELEASE);
        slab->state = SLAB_STATE_DEQUEUED;

        // Get object
        obj = __slab_obj_get(slab);

        // Add to local partial or full list
        __percpu_cache_lock_cpu(cache, cpu_id);
        if (__SLAB_FULL(slab)) {
            list_node_push_back(&pcpu_cache->full_list, slab, list_entry);
            __atomic_fetch_add(&pcpu_cache->full_count, 1, __ATOMIC_RELEASE);
            slab->state = SLAB_STATE_FULL;
        } else {
            list_node_push_back(&pcpu_cache->partial_list, slab, list_entry);
            __atomic_fetch_add(&pcpu_cache->partial_count, 1, __ATOMIC_RELEASE);
            slab->state = SLAB_STATE_PARTIAL;
        }
        __atomic_fetch_add(&cache->obj_active, 1, __ATOMIC_RELEASE);
        __percpu_cache_unlock_cpu(cache, cpu_id);

        __slab_sanitizer_check("slab_alloc", cache, slab, obj);
        pop_off();
        return obj;
    }

    __global_free_unlock(cache);

    // PHASE 3: Create new slab (no locks held)
    // __slab_make will try emergency shrinking if initial allocation fails
    slab = __slab_make(cache->flags, cache->slab_order, cache->offset,
                       cache->obj_size, cache->slab_obj_num, cache->bitmap_size);
    if (slab == NULL) {
        // Still failed after emergency shrink - truly out of memory
        pop_off();
        return NULL;
    }

    // Attach and take ownership
    slab->cache = cache;
    __atomic_store_n(&slab->cpu_id, cpu_id, __ATOMIC_RELEASE);
    __atomic_fetch_add(&cache->slab_total, 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&cache->obj_total, cache->slab_obj_num, __ATOMIC_RELEASE);

    // Get object
    obj = __slab_obj_get(slab);

    // Add to appropriate list
    __percpu_cache_lock_cpu(cache, cpu_id);
    if (__SLAB_FULL(slab)) {
        list_node_push_back(&pcpu_cache->full_list, slab, list_entry);
        __atomic_fetch_add(&pcpu_cache->full_count, 1, __ATOMIC_RELEASE);
        slab->state = SLAB_STATE_FULL;
    } else {
        list_node_push_back(&pcpu_cache->partial_list, slab, list_entry);
        __atomic_fetch_add(&pcpu_cache->partial_count, 1, __ATOMIC_RELEASE);
        slab->state = SLAB_STATE_PARTIAL;
    }
    __atomic_fetch_add(&cache->obj_active, 1, __ATOMIC_RELEASE);
    __percpu_cache_unlock_cpu(cache, cpu_id);

    __slab_sanitizer_check("slab_alloc", cache, slab, obj);
    pop_off();
    return obj;
}

// try to free an object
// the function will find the slab of the object from the page descriptor
void slab_free(void *obj) {
    slab_t *slab;
    slab_cache_t *cache;
    int slab_cpu_id;
    percpu_slab_cache_t *pcpu_cache;

    if (obj == NULL) {
        printf("slab_free(): obj is NULL\n");
        return;
    }

    // PHASE 1: Find slab (no lock needed - page descriptor is immutable)
    slab = __find_obj_slab(obj);
    if (slab == NULL) {
        printf("slab_free(): slab is NULL for obj=%p\n", obj);
        return;
    }

    cache = slab->cache;
    if (cache == NULL) {
        printf("slab_free: ERROR - slab=%p not attached to cache, obj=%p\n", slab, obj);
        printf("  slab->page=%p, slab->in_use=%ld, slab->state=%d, slab->cpu_id=%d\n",
               slab->page, slab->in_use, slab->state, __atomic_load_n(&slab->cpu_id, __ATOMIC_ACQUIRE));
        panic("slab_free: slab not attached to cache");
    }

    // PHASE 2: Determine ownership (atomic load - no lock needed)
    slab_cpu_id = __atomic_load_n(&slab->cpu_id, __ATOMIC_ACQUIRE);

    if (slab_cpu_id < 0) {
        // Slab is in global free list - this shouldn't happen
        printf("slab_free: ERROR - object from free slab\n");
        printf("  obj=%p, slab=%p, cache='%s'\n", obj, slab, cache->name);
        printf("  slab->in_use=%ld, slab->state=%d, slab->cpu_id=%d\n",
               slab->in_use, slab->state, slab_cpu_id);
        printf("  cache->global_free_count=%ld\n",
               __atomic_load_n(&cache->global_free_count, __ATOMIC_ACQUIRE));
        panic("slab_free: object from free slab");
    }

    pcpu_cache = &cache->percpu_caches[slab_cpu_id];

    // PHASE 3: Acquire appropriate lock
    __percpu_cache_lock_cpu(cache, slab_cpu_id);

    // Double-check cpu_id hasn't changed
    if (__atomic_load_n(&slab->cpu_id, __ATOMIC_ACQUIRE) != slab_cpu_id) {
        __percpu_cache_unlock_cpu(cache, slab_cpu_id);
        panic("slab_free: slab cpu_id changed during free");
    }

    // PHASE 4: Return object to slab
    slab_state_t old_state = slab->state;
    int was_full = __SLAB_FULL(slab);

    __slab_obj_put(slab, obj);
    __atomic_fetch_sub(&cache->obj_active, 1, __ATOMIC_RELEASE);

    // PHASE 5: Move slab between lists if state changed
    if (__SLAB_EMPTY(slab)) {
        // Transition to empty - move to global free list
        if (old_state == SLAB_STATE_PARTIAL) {
            list_node_detach(slab, list_entry);
            __atomic_fetch_sub(&pcpu_cache->partial_count, 1, __ATOMIC_RELEASE);
        } else if (old_state == SLAB_STATE_FULL) {
            list_node_detach(slab, list_entry);
            __atomic_fetch_sub(&pcpu_cache->full_count, 1, __ATOMIC_RELEASE);
        }

        __atomic_store_n(&slab->cpu_id, -1, __ATOMIC_RELEASE);
        slab->state = SLAB_STATE_FREE;

        __percpu_cache_unlock_cpu(cache, slab_cpu_id);

        // Add to global free list
        __global_free_lock(cache);
        list_node_push_back(&cache->global_free_list, slab, list_entry);
        __atomic_fetch_add(&cache->global_free_count, 1, __ATOMIC_RELEASE);
        __global_free_unlock(cache);

    } else if (was_full && !__SLAB_FULL(slab)) {
        // Transition from full to partial
        list_node_detach(slab, list_entry);
        __atomic_fetch_sub(&pcpu_cache->full_count, 1, __ATOMIC_RELEASE);

        // Move to owner CPU's partial list (implements the special case requirement)
        list_node_push_back(&pcpu_cache->partial_list, slab, list_entry);
        __atomic_fetch_add(&pcpu_cache->partial_count, 1, __ATOMIC_RELEASE);
        slab->state = SLAB_STATE_PARTIAL;

        __percpu_cache_unlock_cpu(cache, slab_cpu_id);

    } else {
        // No state change, just unlock
        __percpu_cache_unlock_cpu(cache, slab_cpu_id);
    }

    __slab_sanitizer_check("slab_free", cache, slab, obj);

    // PHASE 6: Check if we need to shrink (simplified - check global free list)
    int64 free_count = __atomic_load_n(&cache->global_free_count, __ATOMIC_ACQUIRE);
    if (free_count * cache->slab_obj_num >= cache->limits) {
        // Try to shrink from global free list
        __global_free_lock(cache);
        int target_shrink = free_count / 2;
        list_node_t tmp_list;
        list_entry_init(&tmp_list);

        for (int i = 0; i < target_shrink && !LIST_IS_EMPTY(&cache->global_free_list); i++) {
            slab_t *free_slab = list_node_pop_back(&cache->global_free_list, slab_t, list_entry);
            if (free_slab != NULL) {
                __atomic_fetch_sub(&cache->global_free_count, 1, __ATOMIC_RELEASE);
                __slab_detach(cache, free_slab);
                list_node_push_back(&tmp_list, free_slab, list_entry);
            }
        }
        __global_free_unlock(cache);

        // Free slabs outside lock
        slab_t *s, *tmp;
        list_foreach_node_safe(&tmp_list, s, tmp, list_entry) {
            __slab_destroy(s);
        }
    }
}

// Free an object without triggering slab shrinking.
// Use this when freeing multiple objects in a loop where the list pointers
// are stored in the objects themselves (e.g., vm_destroy freeing VMAs).
// Shrinking during such a loop can free the page containing list pointers
// we're about to follow, causing use-after-free.
void slab_free_noshrink(void *obj) {
    slab_t *slab;
    slab_cache_t *cache;
    int slab_cpu_id;
    percpu_slab_cache_t *pcpu_cache;

    if (obj == NULL) {
        printf("slab_free_noshrink(): obj is NULL\n");
        return;
    }

    // PHASE 1: Find slab (no lock needed - page descriptor is immutable)
    slab = __find_obj_slab(obj);
    if (slab == NULL) {
        printf("slab_free_noshrink(): slab is NULL for obj=%p\n", obj);
        return;
    }

    cache = slab->cache;
    if (cache == NULL) {
        panic("slab_free_noshrink: slab not attached to cache");
    }

    // PHASE 2: Determine ownership (atomic load - no lock needed)
    slab_cpu_id = __atomic_load_n(&slab->cpu_id, __ATOMIC_ACQUIRE);

    if (slab_cpu_id < 0) {
        // Slab is in global free list - this shouldn't happen
        panic("slab_free_noshrink: object from free slab");
    }

    pcpu_cache = &cache->percpu_caches[slab_cpu_id];

    // PHASE 3: Acquire appropriate lock
    __percpu_cache_lock_cpu(cache, slab_cpu_id);

    // Double-check cpu_id hasn't changed
    if (__atomic_load_n(&slab->cpu_id, __ATOMIC_ACQUIRE) != slab_cpu_id) {
        __percpu_cache_unlock_cpu(cache, slab_cpu_id);
        panic("slab_free_noshrink: slab cpu_id changed during free");
    }

    // PHASE 4: Return object to slab
    slab_state_t old_state = slab->state;
    int was_full = __SLAB_FULL(slab);

    __slab_obj_put(slab, obj);
    __atomic_fetch_sub(&cache->obj_active, 1, __ATOMIC_RELEASE);

    // PHASE 5: Move slab between lists if state changed
    if (__SLAB_EMPTY(slab)) {
        // Transition to empty - move to global free list
        if (old_state == SLAB_STATE_PARTIAL) {
            list_node_detach(slab, list_entry);
            __atomic_fetch_sub(&pcpu_cache->partial_count, 1, __ATOMIC_RELEASE);
        } else if (old_state == SLAB_STATE_FULL) {
            list_node_detach(slab, list_entry);
            __atomic_fetch_sub(&pcpu_cache->full_count, 1, __ATOMIC_RELEASE);
        }

        __atomic_store_n(&slab->cpu_id, -1, __ATOMIC_RELEASE);
        slab->state = SLAB_STATE_FREE;

        __percpu_cache_unlock_cpu(cache, slab_cpu_id);

        // Add to global free list
        __global_free_lock(cache);
        list_node_push_back(&cache->global_free_list, slab, list_entry);
        __atomic_fetch_add(&cache->global_free_count, 1, __ATOMIC_RELEASE);
        __global_free_unlock(cache);

    } else if (was_full && !__SLAB_FULL(slab)) {
        // Transition from full to partial
        list_node_detach(slab, list_entry);
        __atomic_fetch_sub(&pcpu_cache->full_count, 1, __ATOMIC_RELEASE);

        // Move to owner CPU's partial list
        list_node_push_back(&pcpu_cache->partial_list, slab, list_entry);
        __atomic_fetch_add(&pcpu_cache->partial_count, 1, __ATOMIC_RELEASE);
        slab->state = SLAB_STATE_PARTIAL;

        __percpu_cache_unlock_cpu(cache, slab_cpu_id);

    } else {
        // No state change, just unlock
        __percpu_cache_unlock_cpu(cache, slab_cpu_id);
    }

    __slab_sanitizer_check("slab_free_noshrink", cache, slab, obj);

    // PHASE 6: Skip shrinking - that's the whole point of this function
}
