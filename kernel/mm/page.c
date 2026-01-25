// Physical page allocator using the buddy system algorithm.
//
// The buddy system manages free pages by organizing them into pools of
// different orders (power-of-2 sizes). This enables efficient allocation and
// coalescing of physically contiguous memory regions.
//
// KEY FEATURES:
//   - Per-order fine-grained locking for concurrent access
//   - Per-CPU hot page cache for frequently allocated orders (0-8)
//   - Lock-free order 0 cache using interrupt disabling
//   - Lazy buddy merging with MERGING state to prevent races
//   - Reference counting for shared pages
//
// LOCKING HIERARCHY (to prevent deadlocks):
//   1. Per-CPU cache locks (push_off/pop_off for order 0, spinlocks for 1-8)
//   2. Buddy pool locks (always acquired in ascending order)
//   3. Individual page locks (acquired while holding pool locks)
//
// BUDDY STATES:
//   - BUDDY_STATE_FREE: Page is in buddy pool, available for allocation
//   - BUDDY_STATE_MERGING: Page is being merged with its buddy
//   - BUDDY_STATE_CACHED: Page is in per-CPU cache
//
// ORGANIZATION:
//   1. Global Data & Configuration
//   2. Debugging & Sanitization
//   3. Locking Primitives
//   4. Validation & Helper Functions
//   5. Page Initialization
//   6. Buddy Pool Operations (List Management)
//   7. Buddy Finding & State Management
//   8. Buddy Splitting & Merging
//   9. Per-CPU Page Cache
//  10. Buddy Allocation (Core Algorithm)
//  11. Buddy Deallocation
//  12. Buddy System Initialization
//  13. Reference Counting (Internal)
//  14. Public API - Allocation & Deallocation
//  15. Public API - Page Locking
//  16. Public API - Reference Counting
//  17. Public API - Address Translation
//  18. Statistics & Debugging

#include "types.h"
#include "string.h"
#include "param.h"
#include "memlayout.h"
#include "lock/spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "list.h"
#include "page.h"
#include "page_private.h"
#include "slab.h"
#include "percpu.h"
#include "early_allocator.h"
#include "fdt.h"
#include "memstat.h"

// ============================================================================
// SECTION 1: Global Data & Configuration
// ============================================================================

STATIC buddy_pool_t __buddy_pools[PAGE_BUDDY_MAX_ORDER + 1];

// Per-CPU hot page cache for small allocations (orders 0 to SLAB_DEFAULT_ORDER)
// This reduces lock contention for the most frequent allocations
#define PCPU_CACHE_MAX_ORDER SLAB_DEFAULT_ORDER
#define PCPU_CACHE_SIZE 4 // pages per order per CPU (small to save memory)
#define PCPU_HOT_PAGE_CACHE_SIZE 64 // hot pages(order 0) per CPU

// Atomic operations for per-CPU cache counters with overflow/underflow checks
#define PCPU_CACHE_COUNT_INC(cache)                                            \
    do {                                                                       \
        uint32 __old_count =                                                   \
            __atomic_fetch_add(&(cache)->count, 1, __ATOMIC_RELEASE);          \
        assert(__old_count < (uint32) - 1, "PCPU cache counter overflow");     \
    } while (0)

#define PCPU_CACHE_COUNT_DEC(cache)                                            \
    do {                                                                       \
        uint32 __old_count =                                                   \
            __atomic_fetch_sub(&(cache)->count, 1, __ATOMIC_RELEASE);          \
        assert(__old_count > 0, "PCPU cache counter underflow");               \
    } while (0)

#define PCPU_CACHE_COUNT_LOAD(cache)                                           \
    __atomic_load_n(&(cache)->count, __ATOMIC_ACQUIRE)

typedef struct {
    list_node_t lru_head; // List of cached pages
    _Atomic uint32 count; // Number of pages in cache (atomic for thread-safety)
    spinlock_t lock; // Lock for orders > 0 (order 0 is lock-free via push_off)
} pcpu_cache_t;

STATIC pcpu_cache_t __pcpu_caches[NCPU][PCPU_CACHE_MAX_ORDER + 1];

// Every physical pages
// TODO: The number of managed pages are fix right now.
STATIC page_t *__pages = NULL;
// The start address and the end address of the managed memory
STATIC uint64 __managed_start;
STATIC uint64 __managed_end;

// buddy pool of lower order must be locked before the buddy pool of higher
// order

// ============================================================================
// SECTION 2: Debugging & Sanitization
// ============================================================================

#ifdef KERNEL_PAGE_SANITIZER
STATIC_INLINE void __page_sanitizer_check(const char *op, page_t *page,
                                          uint64 order, uint64 flags) {
    if (page == NULL) {
        return;
    }
    assert(order <= PAGE_BUDDY_MAX_ORDER,
           "__page_sanitizer_check: invalid order");
    assert(!flags || page->flags == flags,
           "__page_sanitizer_check: page flags mismatch, expected 0x%lx, got "
           "0x%lx",
           flags, page->flags);
    assert(page - __pages < TOTALPAGES,
           "__page_sanitizer_check: page out of bounds");
    assert((page->physical_address - __managed_start) >> PAGE_SHIFT ==
               (page - __pages),
           "__page_sanitizer_check: page physical address mismatch, "
           "expected 0x%lx, got 0x%lx",
           __managed_start + ((page - __pages) << PAGE_SHIFT),
           page->physical_address);
    for (int i = 0; i < (1 << order); i++) {
        assert(page[i].physical_address ==
                   page->physical_address + (i << PAGE_SHIFT),
               "__page_sanitizer_check: page physical address mismatch, "
               "expected 0x%lx, got 0x%lx",
               page->physical_address + (i << PAGE_SHIFT),
               page[i].physical_address);
    }
    printf("%s: order %ld, flags 0x%lx, page 0x%lx\n", op, order, 0L,
           __page_to_pa(page));
}
#else
#define __page_sanitizer_check(op, page, order, flags)                         \
    do {                                                                       \
    } while (0)
#endif

// ============================================================================
// SECTION 3: Locking Primitives
// ============================================================================

// Acquire the spinlock of a specific buddy pool
STATIC_INLINE void __buddy_pool_lock(uint64 order) {
    if (order > PAGE_BUDDY_MAX_ORDER) {
        panic("__buddy_pool_lock: invalid order");
    }
    spin_lock(&__buddy_pools[order].lock);
}

// Release the spinlock of a specific buddy pool
STATIC_INLINE void __buddy_pool_unlock(uint64 order) {
    if (order > PAGE_BUDDY_MAX_ORDER) {
        panic("__buddy_pool_unlock: invalid order");
    }
    spin_unlock(&__buddy_pools[order].lock);
}

// Acquire spinlocks for a range of buddy pools (from low to high order)
// This maintains lock ordering to prevent deadlock
STATIC_INLINE void __buddy_pool_lock_range(uint64 order_start,
                                           uint64 order_end) {
    if (order_start > order_end || order_end > PAGE_BUDDY_MAX_ORDER) {
        panic("__buddy_pool_lock_range: invalid order range");
    }
    for (uint64 i = order_start; i <= order_end; i++) {
        spin_lock(&__buddy_pools[i].lock);
    }
}

// Release spinlocks for a range of buddy pools (in reverse order)
STATIC_INLINE void __buddy_pool_unlock_range(uint64 order_start,
                                             uint64 order_end) {
    if (order_start > order_end || order_end > PAGE_BUDDY_MAX_ORDER) {
        panic("__buddy_pool_unlock_range: invalid order range");
    }
    for (int64 i = order_end; i >= (int64)order_start; i--) {
        spin_unlock(&__buddy_pools[i].lock);
    }
}

// ============================================================================
// SECTION 4: Validation & Helper Functions
// ============================================================================

// Get the total number of pages managed
STATIC_INLINE uint64 __total_pages() {
    return (__managed_end - __managed_start) >> PAGE_SHIFT;
}

// To check if a physical address is within the range of the managed address
#define ADDR_IN_MANAGED(addr)                                                  \
    ((addr) >= KERNBASE && (addr) < __managed_end)

// Check if a base address of a page is valid
// A valid page base address should be aligned to the page size and within
// managed memory
STATIC_INLINE bool __page_base_validity(uint64 physical) {
    if ((physical & PAGE_MASK) || !ADDR_IN_MANAGED(physical)) {
        return false;
    }
    return true;
}

// Check if flags are valid during initialization
STATIC_INLINE bool __page_init_flags_validity(uint64 flags) {
    if (flags & (~(PAGE_FLAG_LOCKED))) {
        return false;
    }
    return true;
}

// Check if flags are valid during allocation
STATIC_INLINE bool __page_flags_validity(uint64 flags) {
    // @TODO: Some flags need to be mutually exclusive
    if (PAGE_FLAG_GET_TYPE(flags) >= __PAGE_TYPE_MAX) {
        return false;
    }
    if (flags & PAGE_FLAG_MASK) {
        return false;
    }
    return true;
}

// To check if a page can be put back to the buddy system as a free page to be
// allocated again.
STATIC_INLINE bool __page_is_freeable(page_t *page) {
    if (page == NULL)
        return false;
    if (page->flags & PAGE_FLAG_LOCKED)
        return false;
    if (page->ref_count > 1) {
        // Cannot free a page that has been referenced by others
        return false;
    }
    return true;
}

// ============================================================================
// SECTION 5: Page Initialization
// ============================================================================

// Initialize a page descriptor
// No validity check here
STATIC_INLINE void __page_init(page_t *page, uint64 physical, int ref_count,
                               uint64 flags) {
    memset(page, 0, sizeof(page_t));
    page->physical_address = physical;
    page->flags = flags;
    page->ref_count = ref_count;
    spin_init(&page->lock, "page_t");
}

// Initialize buddy pools and per-CPU caches
STATIC_INLINE void __buddy_pool_init() {
    static char *lock_names[PAGE_BUDDY_MAX_ORDER + 1] = {
        "buddy_pool_0", "buddy_pool_1", "buddy_pool_2", "buddy_pool_3",
        "buddy_pool_4", "buddy_pool_5", "buddy_pool_6", "buddy_pool_7",
        "buddy_pool_8", "buddy_pool_9", "buddy_pool_10"};
    for (int i = 0; i < PAGE_BUDDY_MAX_ORDER + 1; i++) {
        __buddy_pools[i].count = 0;
        list_entry_init(&__buddy_pools[i].lru_head);
        spin_init(&__buddy_pools[i].lock, lock_names[i]);
    }

    // Initialize per-CPU caches for orders 0 to SLAB_DEFAULT_ORDER
    for (int cpu = 0; cpu < NCPU; cpu++) {
        for (int order = 0; order <= PCPU_CACHE_MAX_ORDER; order++) {
            pcpu_cache_t *cache = &__pcpu_caches[cpu][order];
            list_entry_init(&cache->lru_head);
            cache->count = 0;
            spin_init(&cache->lock, "pcpu_cache");
        }
    }
}

// Initialize a range of page descriptors with specific flags
STATIC_INLINE int __init_range_flags(uint64 pa_start, uint64 pa_end,
                                     uint64 flags) {
    page_t *page;
    if (pa_start >= pa_end) {
        // The start address must be lower than the end address
        printf("invalid range, pa_start: 0x%lx, pa_end: 0x%lx\n", pa_start, pa_end);
        return -1;
    }
    if (!__page_base_validity(pa_start) ||
        !__page_base_validity(pa_end - PAGE_SIZE)) {
        // Both pa_start and pa_end should be valid physical base page addresses
        printf("invalid range base, pa_start: 0x%lx, pa_end: 0x%lx\n", pa_start, pa_end);
        return -1;
    }
    if (!__page_init_flags_validity(flags)) {
        // Invalid flags
        printf("invalid flags: 0x%lx\n", flags);
        return -1;
    }

    printf("init pages from 0x%lx to 0x%lx with flags 0x%lx\n", pa_start,
           pa_end, flags);

    for (uint64 base = pa_start; base < pa_end; base += PAGE_SIZE) {
        page = __pa_to_page(base);
        if (page == NULL) {
            printf("failed to get page for physical address 0x%lx\n", base);
            return -1;
        }
        __page_init(page, base, 0, flags);
    }

    return 0;
}

// Initialize a single page descriptor as a buddy page
STATIC_INLINE void __page_as_buddy(page_t *page, page_t *buddy_head,
                                   uint64 order) {
    __page_init(page, page->physical_address, 0, PAGE_TYPE_BUDDY);
    page->buddy.buddy_head = buddy_head;
    page->buddy.order = order;
    page->buddy.state = BUDDY_STATE_FREE;
    list_entry_init(&page->buddy.lru_entry);
}

// Initialize a continuous range of pages as a buddy page in specific order
// Will not check validity here
STATIC_INLINE void __page_as_buddy_group(page_t *buddy_head, uint64 order) {
    uint64 count = 1UL << order;
    for (int i = 0; i < count; i++) {
        __page_as_buddy(&buddy_head[i], buddy_head, order);
    }
}

// ============================================================================
// SECTION 6: Buddy Pool Operations (List Management)
// ============================================================================

// Attach a buddy head page into the corresponding buddy pool and increase the
// count value of the buddy pool by one.
// Will not do validity check here
STATIC_INLINE void __buddy_push_page(buddy_pool_t *pool, page_t *page) {
    if (LIST_IS_EMPTY(&pool->lru_head)) {
        if (pool->count != 0) {
            panic("__buddy_push_page");
        }
    } else if (pool->count == 0) {
        panic("__buddy_push_page");
    }
    list_node_push_back(&pool->lru_head, page, buddy.lru_entry);
    pool->count++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

// Pop a buddy page from a pool and return the page descriptor of the buddy
// header. Return NULL if the given pool is empty
// Will not do validity check here
STATIC_INLINE page_t *__buddy_pop_page(buddy_pool_t *pool) {
    page_t *ret = list_node_pop_back(&pool->lru_head, page_t, buddy.lru_entry);
    if (ret == NULL) {
        if (pool->count > 0) {
            panic("__buddy_pop_page");
        }
        return NULL;
    }
    pool->count--;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return ret;
}

// Detach a buddy head page from a buddy pool and decrease the count value by
// one.
// Will not do validity check here
STATIC_INLINE void __buddy_detach_page(buddy_pool_t *pool, page_t *page) {
    if (LIST_IS_EMPTY(&pool->lru_head)) {
        panic("__buddy_detach_page");
    }
    pool->count--;
    list_node_detach(page, buddy.lru_entry);
}

// ============================================================================
// SECTION 7: Buddy Finding & State Management
// ============================================================================

// Try to calculate the address of a page's buddy with the page's physical
// address. Will not validate the value of order
STATIC_INLINE uint64 __get_buddy_addr(uint64 physical, uint32 order) {
    uint64 __buddy_base_address =
        PAGE_ADDR_GET_BUDDY_GROUP_ADDR(physical, order);
    __buddy_base_address ^= PAGE_BUDDY_BYTES(order);
    return __buddy_base_address;
}

// Try to find a page's buddy
// Return the buddy page descriptor if found
// Return NULL if didn't find
STATIC_INLINE page_t *__get_buddy_page(page_t *page) {
    uint64 buddy_base;
    page_t *buddy_head;
    if (!PAGE_IS_BUDDY_GROUP_HEAD(page)) {
        // Must be the page descriptor of a buddy header page
        return NULL;
    }
    if (page->buddy.order >= PAGE_BUDDY_MAX_ORDER) {
        // Page size reach PAGE_BUDDY_MAX_ORDER doesn't have buddy
        return NULL;
    }
    buddy_base = __get_buddy_addr(page->physical_address, page->buddy.order);
    buddy_head = __pa_to_page(buddy_base);
    if (buddy_head == NULL || !PAGE_IS_BUDDY_GROUP_HEAD(buddy_head) ||
        buddy_head->buddy.order != page->buddy.order) {
        // Didn't find a complete buddy page.
        return NULL;
    }
    if (LIST_ENTRY_IS_DETACHED(&buddy_head->buddy.lru_entry)) {
        // The buddy header page is not in the buddy pool, which means it's
        // holding by someone else right now.
        return NULL;
    }
    // Check buddy state: must be FREE (not CACHED or MERGING)
    // Pages in per-CPU cache have BUDDY_STATE_CACHED
    if (buddy_head->buddy.state != BUDDY_STATE_FREE) {
        return NULL;
    }
    return buddy_head;
}

// Update tail pages after merging and splitting operations.
STATIC_INLINE void __page_order_change_commit(page_t *page) {
    if (!PAGE_IS_BUDDY_GROUP_HEAD(page)) {
        panic("__page_order_change_commit");
    }
    __page_as_buddy_group(page, page->buddy.order);
}

// ============================================================================
// SECTION 8: Buddy Splitting & Merging
// ============================================================================

// Split a buddy page in half and return the header page of the later half of
// the splitted buddy pages. This function will not update the tail pages
// immediately to avoid useless updates. Have to call __page_order_change_commit
// after page splitting.
// Return NULL if failed to split
STATIC_INLINE page_t *__buddy_split(page_t *page) {
    int order_after;
    page_t *buddy;
    if (!PAGE_IS_BUDDY_GROUP_HEAD(page)) {
        return NULL;
    }
    if (page->buddy.order == 0) {
        // A single page cannot be splitted.
        return NULL;
    }
    order_after = page->buddy.order - 1;
    buddy = page + (1UL << order_after);
    __page_as_buddy(page, page, order_after);
    __page_as_buddy(buddy, buddy, order_after);
    return buddy;
}

// Merge two buddy pages and return the header page of the merged
// buddy page. This function will not update the tail pages immediately
// to avoid useless updates. Have to call __page_order_change_commit after page
// splitting.
// Return NULL if failed to merge
STATIC_INLINE page_t *__buddy_merge(page_t *page1, page_t *page2) {
    page_t *header, *tail;
    uint64 order_after;
    if (!PAGES_ARE_BUDDIES(page1, page2)) {
        return NULL;
    }
    if (page1->physical_address < page2->physical_address) {
        header = page1;
        tail = page2;
    } else {
        header = page2;
        tail = page1;
    }
    order_after = page1->buddy.order + 1;
    __page_as_buddy(header, header, order_after);
    __page_as_buddy(tail, header, order_after);
    return header;
}

// ============================================================================
// SECTION 9: Per-CPU Page Cache
// ============================================================================

// Try to get a page from per-CPU cache
// Returns NULL if cache is empty
STATIC page_t *__pcpu_cache_get(uint64 order, uint64 flags) {
    if (order > PCPU_CACHE_MAX_ORDER) {
        return NULL;
    }

    int cpu_id = cpuid();
    pcpu_cache_t *cache = &__pcpu_caches[cpu_id][order];
    page_t *page = NULL;

    if (order == 0) {
        // Lock-free for order 0 using interrupt disabling
        push_off();
        if (!LIST_IS_EMPTY(&cache->lru_head)) {
            page =
                list_node_pop_back(&cache->lru_head, page_t, buddy.lru_entry);
            if (page != NULL) {
                PCPU_CACHE_COUNT_DEC(cache);
            }
        }
        pop_off();
    } else {
        // Use spinlock for orders > 0 (for future cross-CPU stealing)
        spin_lock(&cache->lock);
        if (!LIST_IS_EMPTY(&cache->lru_head)) {
            page =
                list_node_pop_back(&cache->lru_head, page_t, buddy.lru_entry);
            if (page != NULL) {
                PCPU_CACHE_COUNT_DEC(cache);
            }
        }
        spin_unlock(&cache->lock);
    }

    if (page != NULL) {
        // Initialize the cached page for user allocation
        // Physical addresses are already set correctly from when pages were
        // cached
        uint64 page_count = 1UL << order;
        for (uint64 i = 0; i < page_count; i++) {
            uint64 phys_addr =
                page[i].physical_address; // Save physical address
            page[i].flags = 0;
            __page_init(&page[i], phys_addr, 1, flags);
        }
    }

    return page;
}

// Try to put a page into per-CPU cache
// Returns 0 on success, -1 if cache is full
STATIC int __pcpu_cache_put(page_t *page, uint64 order) {
    if (order > PCPU_CACHE_MAX_ORDER) {
        return -1;
    }

    int cpu_id = cpuid();
    pcpu_cache_t *cache = &__pcpu_caches[cpu_id][order];
    uint32 cache_limit =
        (order == 0) ? PCPU_HOT_PAGE_CACHE_SIZE : PCPU_CACHE_SIZE;
    int ret = -1;

    if (order == 0) {
        // Lock-free for order 0 using interrupt disabling
        push_off();
        uint32 current_count = PCPU_CACHE_COUNT_LOAD(cache);
        if (current_count < cache_limit) {
            // Initialize page as buddy before caching
            __page_as_buddy_group(page, order);
            page->buddy.state = BUDDY_STATE_CACHED;
            list_node_push_back(&cache->lru_head, page, buddy.lru_entry);
            PCPU_CACHE_COUNT_INC(cache);
            ret = 0;
        }
        pop_off();
    } else {
        // Use spinlock for orders > 0 (for future cross-CPU stealing)
        spin_lock(&cache->lock);
        uint32 current_count = PCPU_CACHE_COUNT_LOAD(cache);
        if (current_count < cache_limit) {
            // Initialize page as buddy before caching
            __page_as_buddy_group(page, order);
            page->buddy.state = BUDDY_STATE_CACHED;
            list_node_push_back(&cache->lru_head, page, buddy.lru_entry);
            PCPU_CACHE_COUNT_INC(cache);
            ret = 0;
        }
        spin_unlock(&cache->lock);
    }

    return ret;
}

// ============================================================================
// SECTION 10: Buddy Allocation (Core Algorithm)
// ============================================================================

STATIC page_t *__buddy_get(uint64 order, uint64 flags) {
    buddy_pool_t *pool = NULL;
    page_t *page = NULL;
    page_t *buddy = NULL;
    uint64 tmp_order = order;
    uint64 found_order = 0; // Track which order we found a page at
    uint64 page_count;

    if (!__page_flags_validity(flags)) {
        return NULL;
    }
    if (order > PAGE_BUDDY_MAX_ORDER) {
        return NULL;
    }

    // Try per-CPU cache first for small orders
    if (order <= PCPU_CACHE_MAX_ORDER) {
        page = __pcpu_cache_get(order, flags);
        if (page != NULL) {
            return page; // Cache hit - page already initialized
        }
    }

    // Lock the requested order only when taking out
    __buddy_pool_lock(order);
    pool = &__buddy_pools[order];
    page = __buddy_pop_page(pool);
    __buddy_pool_unlock(order); // Unlock immediately after taking out

    // found available buddy pages at the requested order
    if (page != NULL) {
        goto found;
    }

    // Need to search higher orders - lock only when taking out
    // Try to find a bigger buddy page to split
    for (tmp_order = order + 1; tmp_order <= PAGE_BUDDY_MAX_ORDER;
         tmp_order++) {
        __buddy_pool_lock(tmp_order);
        pool = &__buddy_pools[tmp_order];
        page = __buddy_pop_page(pool);
        __buddy_pool_unlock(tmp_order); // Unlock immediately after taking out

        // break the for loop when finding a free buddy page
        if (page != NULL) {
            found_order = tmp_order; // Save which order we found the page at
            break;
        }
    }

    // if still not found available buddy pages after the for loop, then
    // there's no buddy page that meets the requirement.
    if (page == NULL) {
        // All locks already released
        return NULL;
    }

    // if found one, split it and return the header page from one of the
    // splitted groups. Lock each order only when putting buddy back.
    tmp_order = found_order;
    do {
        buddy = __buddy_split(page);
        if (buddy == NULL) {
            // There's no way the splitting operation here would fail. If it
            // happens, then something wrong happened.
            panic("__buddy_get(): failed splitting buddy pages");
        }

        // put the later half back - lock only when putting in
        tmp_order--;
        pool = &__buddy_pools[tmp_order];
        __page_order_change_commit(buddy);

        __buddy_pool_lock(tmp_order);
        __buddy_push_page(pool, buddy);
        __buddy_pool_unlock(tmp_order); // Unlock immediately after putting in
    } while (tmp_order > order);

found:
    // Initialize all pages for user allocation
    page_count = 1UL << order;
    for (uint64 i = 0; i < page_count; i++) {
        page[i].flags = 0;
        __page_init(&page[i], page[i].physical_address, 1, flags);
    }
    return page;
}

// ============================================================================
// SECTION 10: Buddy Deallocation (Single Page)
// ============================================================================

// Common merge-and-insert logic for both __buddy_put and __page_free
// Assumes page is already initialized as a buddy at start_order with MERGING
// state
STATIC void __buddy_merge_and_insert(page_t *page, uint64 start_order) {
    buddy_pool_t *pool = NULL;
    page_t *buddy = NULL;
    uint64 tmp_order;

    for (tmp_order = start_order; tmp_order <= PAGE_BUDDY_MAX_ORDER;
         tmp_order++) {
        pool = &__buddy_pools[tmp_order];

        // Lock pool to search for buddy
        __buddy_pool_lock(tmp_order);

        buddy = __get_buddy_page(page);
        if (buddy != NULL) {
            // Buddy found, detach it from pool
            __buddy_detach_page(pool, buddy);
            __buddy_pool_unlock(tmp_order);

            // Mark buddy as merging
            page_lock_acquire(buddy);
            buddy->buddy.state = BUDDY_STATE_MERGING;
            page_lock_release(buddy);
        } else {
            // No buddy found, add page to pool and finish
            page_lock_acquire(page);
            page->buddy.state = BUDDY_STATE_FREE;
            page_lock_release(page);

            __page_order_change_commit(page);
            __buddy_push_page(pool, page);
            __buddy_pool_unlock(tmp_order);
            return;
        }

        // Merge the buddies
        page = __buddy_merge(page, buddy);
        if (page == NULL) {
            panic("__buddy_merge_and_insert(): failed to merge buddies");
        }

        // Mark merged page as MERGING (only lock first half's head)
        page_lock_acquire(page);
        page->buddy.state = BUDDY_STATE_MERGING;
        page_lock_release(page);
    }
}

// Put a page back to buddy system
// Right now pages can only be put one by one
STATIC int __buddy_put(page_t *page) {
    if (!__page_is_freeable(page)) {
        // cannot free a page that's not freeable
        return -1;
    }

    // Try per-CPU cache first for order 0 pages
    if (__pcpu_cache_put(page, 0) == 0) {
        return 0; // Successfully cached
    }

    // Cache full or order > PCPU_CACHE_MAX_ORDER, go to buddy system
    // Initialize page as buddy and mark as merging
    __page_as_buddy(page, page, 0);
    page_lock_acquire(page);
    page->buddy.state = BUDDY_STATE_MERGING;
    page_lock_release(page);

    // Use common merge-and-insert logic starting from order 0
    __buddy_merge_and_insert(page, 0);
    return 0;
}

// ============================================================================
// SECTION 11: Buddy System Initialization
// ============================================================================

// Check if a physical address falls within any reserved region from FDT
static int __is_reserved_page(uint64 pa) {
    // Check ramdisk region
    if (platform.has_ramdisk && platform.ramdisk_base != 0) {
        uint64 rd_start = PGROUNDDOWN(platform.ramdisk_base);
        uint64 rd_end = PGROUNDUP(platform.ramdisk_base + platform.ramdisk_size);
        if (pa >= rd_start && pa < rd_end) {
            return 1;
        }
    }
    
    // Check reserved memory regions from FDT
    for (int i = 0; i < platform.reserved_count; i++) {
        uint64 res_start = PGROUNDDOWN(platform.reserved[i].base);
        uint64 res_end = PGROUNDUP(platform.reserved[i].base + platform.reserved[i].size);
        if (res_start < res_end && pa >= res_start && pa < res_end) {
            return 1;
        }
    }
    
    return 0;
}

// Init buddy system and add the given range of pages into it
int page_buddy_init(void) {
    size_t page_arr_size = sizeof(page_t) * TOTALPAGES;
    __pages = (page_t *)early_alloc_align(page_arr_size, PGSIZE);
    assert(__pages != NULL, "page_buddy_init(): failed to allocate page array");
    __managed_start = PGROUNDUP((uint64)early_alloc_end_ptr());
    __managed_end = PHYSTOP;
    printf("page_buddy_init(): page array at 0x%lx, size 0x%lx\n",
           (uint64)__pages, page_arr_size);
    printf("__managed_start: 0x%lx, __managed_end: 0x%lx\n", __managed_start, __managed_end);
    assert(KERNBASE < __managed_start,
           "page_buddy_init(): KERNBASE: 0x%lx not less than pa_start: 0x%lx",
           KERNBASE, __managed_start);
    assert(__managed_end <= PHYSTOP,
           "page_buddy_init(): managed_end: 0x%lx higher than PHYSTOP: 0x%lx",
           __managed_end, PHYSTOP);
    assert(__managed_start < __managed_end,
           "page_buddy_init(): managed_start: 0x%lx not less than managed_end: "
           "0x%lx",
           __managed_start, __managed_end);
    assert(__init_range_flags(KERNBASE, __managed_start, PAGE_FLAG_LOCKED) == 0,
           "page_buddy_init(): lower locked memory: 0x%lx to 0x%lx", KERNBASE,
           __managed_start);
    if (__managed_end < PHYSTOP) {
        // Usually the managed_end is equal to PHYSTOP. Just in case
        assert(__init_range_flags(__managed_end, PHYSTOP, PAGE_FLAG_LOCKED) == 0,
               "page_buddy_init(): higher locked memory: 0x%lx to 0x%lx",
               __managed_end, PHYSTOP);
    }
    assert(__init_range_flags(__managed_start, __managed_end, 0) == 0,
           "page_buddy_init(): free range: 0x%lx to 0x%lx", __managed_start,
           __managed_end);

    __buddy_pool_init();

    for (uint64 base = __managed_start; base < __managed_end; base += PAGE_SIZE) {
        page_t *page = __pa_to_page(base);
        if (page == NULL) {
            panic("page_buddy_init(): get NULL page");
        }
        
        // Skip reserved and ramdisk regions - mark them as locked instead
        if (__is_reserved_page(base)) {
            page->flags = PAGE_FLAG_LOCKED;
            continue;
        }
        
        if (__buddy_put(page) != 0) {
            panic("page_buddy_init(): page put error");
        }
    }
    
#ifndef HOST_TEST
    print_buddy_system_stat(1);
#endif

    return 0;
}

// ============================================================================
// SECTION 12: Reference Counting (Internal)
// ============================================================================

STATIC_INLINE int __page_ref_inc_unlocked(page_t *page) {
    assert(spin_holding(&page->lock),
           "__page_ref_inc_unlocked: page lock not held");
    if (page->ref_count == 0) {
        // page with 0 reference should be put back to the buddy system
        return -1;
    }
    page->ref_count++;
    return page->ref_count;
}

STATIC_INLINE int __page_ref_dec_unlocked(page_t *page) {
    assert(spin_holding(&page->lock),
           "__page_ref_dec_unlocked: page lock not held");
    if (page->ref_count > 0) {
        page->ref_count--;
        return page->ref_count;
    }
    return -1;
}

// ============================================================================
// SECTION 13: Public API - Allocation & Deallocation
// ============================================================================

page_t *__page_alloc(uint64 order, uint64 flags) {
    if (order > PAGE_BUDDY_MAX_ORDER) {
        return NULL;
    }
    page_t *ret = __buddy_get(order, flags);
    __page_sanitizer_check("page_alloc", ret, order, flags);
    return ret;
}

// The base address of the page should be aligned to order
// Otherwise panic
void __page_free(page_t *page, uint64 order) {
    __page_sanitizer_check("page_free", page, order, 0);
    uint64 count = 1UL << order;

    if (page == NULL) {
        return;
    }

    assert(order <= PAGE_BUDDY_MAX_ORDER, "__page_free(): order too large");
    assert(!(page->physical_address & PAGE_BUDDY_OFFSET_MASK(order)),
           "free pages not aligned to order");

    // Check that all pages in the block are freeable
    for (uint64 i = 0; i < count; i++) {
        if (!__page_is_freeable(&page[i])) {
            panic("__page_free(): trying to free non-freeable page");
        }
    }

    // Try per-CPU cache first for cacheable orders
    if (order <= PCPU_CACHE_MAX_ORDER) {
        if (__pcpu_cache_put(page, order) == 0) {
            return; // Successfully cached
        }
    }

    // Cache full or order > PCPU_CACHE_MAX_ORDER, go to buddy system
    // Initialize the block as a buddy group and mark as merging
    __page_as_buddy_group(page, order);
    page_lock_acquire(page);
    page->buddy.state = BUDDY_STATE_MERGING;
    page_lock_release(page);

    // Use common merge-and-insert logic starting from the given order
    __buddy_merge_and_insert(page, order);
}

// Helper function for __page_alloc. Convert the page struct to the base
// address of the page
void *page_alloc(uint64 order, uint64 flags) {
    void *pa;
    page_t *page = __page_alloc(order, flags);
    if (page == NULL) {
        return NULL;
    }

    pa = (void *)__page_to_pa(page);

    if (pa) {
        memset((char *)pa, 5, PGSIZE << order); // fill with junk
    } else
        panic("page_alloc");
    return pa;
}

// Helper function for __page_free. Convert the base address of the page to be
// free to page struct
void page_free(void *ptr, uint64 order) {
    page_t *page = __pa_to_page((uint64)ptr);
    __page_free(page, order);
}

// ============================================================================
// SECTION 14: Public API - Page Locking
// ============================================================================

void page_lock_acquire(page_t *page) {
    if (page == NULL) {
        return;
    }
    spin_lock(&page->lock);
}

void page_lock_release(page_t *page) {
    if (page == NULL) {
        return;
    }
    spin_unlock(&page->lock);
}

void page_lock_assert_holding(page_t *page) {
    if (page == NULL) {
        return;
    }
    assert(spin_holding(&page->lock), "page_lock_assert_holding failed");
}

void page_lock_assert_unholding(page_t *page) {
    if (page == NULL) {
        return;
    }
    assert(!spin_holding(&page->lock), "page_lock_assert_unholding failed");
}

// ============================================================================
// SECTION 15: Public API - Reference Counting
// ============================================================================

int __page_ref_inc(page_t *page) {
    int ret = 0;
    if (page == NULL) {
        return -1;
    }
    page_lock_acquire(page);
    {
        ret = __page_ref_inc_unlocked(page);
    }
    page_lock_release(page);
    return ret;
}

int page_ref_inc_unlocked(page_t *page) {
    if (page == NULL) {
        return -1;
    }
    return __page_ref_inc_unlocked(page);
}

int page_ref_dec_unlocked(page_t *page) {
    if (page == NULL) {
        return -1;
    }
    // Use atomic decrement to prevent race conditions when multiple threads
    // call unlocked decrement simultaneously
    int old_count = __sync_fetch_and_sub(&page->ref_count, 1);
    if (old_count < 2) {
        // unlocked decrement is only allowed when the ref count was 2 or more
        // restore the count and return error
        __sync_fetch_and_add(&page->ref_count, 1);
        return -1;
    }
    return old_count - 1;
}

int __page_ref_dec(page_t *page) {
    int original_ref_count = -1;
    int ret = 0;
    __page_sanitizer_check("__page_ref_dec", page, 0, 0);
    if (page == NULL) {
        return -1;
    }
    page_lock_acquire(page);
    {
        original_ref_count = page_ref_count(page);
        if (original_ref_count < 1) {
            page_lock_release(page);
            return 0; // no need to free the page if the ref count is already 0
        }
        ret = __page_ref_dec_unlocked(page);
    }
    page_lock_release(page);
    assert(original_ref_count - ret == 1,
           "__page_ref_dec: ref_count should be decreased by 1");
    if (ret == 0) {
        if (PAGE_IS_TYPE(page, PAGE_TYPE_PCACHE) && page->pcache.pcache_node) {
            // Free the page cache node
            slab_free(page->pcache.pcache_node);
            page->pcache.pcache_node = NULL;
        }
        __page_sanitizer_check("page_free", page, 0, 0);
        if (__buddy_put(page) != 0) {
            panic("page_ref_dec");
        }
    }
    return ret;
}

// Return the reference count of a page
// Return -1 if failed
int page_refcnt(void *physical) {
    page_t *page = __pa_to_page((uint64)physical);
    return page_ref_count(page);
}

// Helper function to __page_ref_inc
int page_ref_inc(void *ptr) {
    page_t *page = __pa_to_page((uint64)ptr);
    return __page_ref_inc(page);
}

// Helper function to __page_ref_dec
int page_ref_dec(void *ptr) {
    page_t *page = __pa_to_page((uint64)ptr);
    return __page_ref_dec(page);
}

// ============================================================================
// SECTION 16: Public API - Address Translation
// ============================================================================

// Get a page struct from its physical base address
page_t *__pa_to_page(uint64 physical) {
    if (__page_base_validity(physical)) {
        return &__pages[(physical - KERNBASE) >> PAGE_SHIFT];
    }
    return NULL;
}

// Get the physical address of a page
uint64 __page_to_pa(page_t *page) {
    if (page == NULL) {
        return 0;
    }
    return page->physical_address;
}

// Get the reference count of a page
int page_ref_count(page_t *page) {
    if (page == NULL) {
        return -1;
    }
    return page->ref_count;
}

uint64 managed_page_base() { return __managed_start; }

// ============================================================================
// SECTION 17: Statistics & Debugging
// ============================================================================

// Record the number of buddies in each order
// Will return an array of order 0 to size - 1
void page_buddy_stat(uint64 *ret_arr, bool *empty_arr, size_t size) {
    if (ret_arr == NULL || size < PAGE_BUDDY_MAX_ORDER + 1) {
        return;
    }
    // Lock all orders to get a consistent snapshot
    __buddy_pool_lock_range(0, PAGE_BUDDY_MAX_ORDER);
    for (int i = 0; i <= PAGE_BUDDY_MAX_ORDER && i < size; i++) {
        ret_arr[i] = __buddy_pools[i].count;
        if (empty_arr != NULL) {
            empty_arr[i] = LIST_IS_EMPTY(&__buddy_pools[i].lru_head);
        }
    }
    __buddy_pool_unlock_range(0, PAGE_BUDDY_MAX_ORDER);
}

// Helper function to print size in human-readable format
STATIC void __print_size(uint64 bytes) {
    if (bytes >= (1UL << 30)) {
        // GB
        uint64 gb = bytes >> 30;
        uint64 mb = (bytes & ((1UL << 30) - 1)) >> 20;
        printf("%ld.%ldG", gb, (mb * 10) / 1024);
    } else if (bytes >= (1UL << 20)) {
        // MB
        uint64 mb = bytes >> 20;
        uint64 kb = (bytes & ((1UL << 20) - 1)) >> 10;
        printf("%ld.%ldM", mb, (kb * 10) / 1024);
    } else if (bytes >= (1UL << 10)) {
        // KB
        uint64 kb = bytes >> 10;
        printf("%ldK", kb);
    } else {
        // Bytes
        printf("%ldB", bytes);
    }
}

static void __buddy_stat_totals(uint64 *total_free_pages,
                                uint64 *total_cached_pages,
                                uint64 *ret_arr,
                                bool *empty_arr) {
    *total_free_pages = 0;
    *total_cached_pages = 0;

    page_buddy_stat(ret_arr, empty_arr, PAGE_BUDDY_MAX_ORDER + 1);

    for (int i = 0; i <= PAGE_BUDDY_MAX_ORDER; i++) {
        uint64 order_pages = (1UL << i) * ret_arr[i];
        *total_free_pages += order_pages;

        // Add per-CPU cache stats for cacheable orders
        if (i <= PCPU_CACHE_MAX_ORDER) {
            uint64 cache_total = 0;
            for (int cpu = 0; cpu < NCPU; cpu++) {
                // Use atomic load for lock-free read
                cache_total += PCPU_CACHE_COUNT_LOAD(&__pcpu_caches[cpu][i]);
            }

            if (cache_total > 0) {
                uint64 cached_pages = (1UL << i) * cache_total;
                *total_cached_pages += cached_pages;
            }
        }
    }
}

void print_buddy_system_stat(int detailed) {
    uint64 total_free_pages = 0;
    uint64 total_cached_pages = 0;
    uint64 ret_arr[PAGE_BUDDY_MAX_ORDER + 1] = {0};
    bool empty_arr[PAGE_BUDDY_MAX_ORDER + 1] = {false};

    __buddy_stat_totals(&total_free_pages, &total_cached_pages,
                        ret_arr, empty_arr);

    if (detailed <= 0) {
        printf("Buddy: %ld free + %ld cached = %ld pages (", total_free_pages,
               total_cached_pages, total_free_pages + total_cached_pages);
        __print_size((total_free_pages + total_cached_pages) * PAGE_SIZE);
        printf(")\n");
        return;
    }

    if (detailed) {
        printf("Buddy System Statistics:\n");
        printf("========================\n");
    }

    for (int i = 0; i <= PAGE_BUDDY_MAX_ORDER; i++) {
        uint64 order_pages = (1UL << i) * ret_arr[i];
        uint64 order_bytes = order_pages * PAGE_SIZE;

        if (detailed) {
            printf("order(%d): %ld blocks (", i, ret_arr[i]);
            __print_size(order_bytes);
            printf(")");
        }

        if (i <= PCPU_CACHE_MAX_ORDER) {
            uint64 cache_total = 0;
            for (int cpu = 0; cpu < NCPU; cpu++) {
                cache_total += PCPU_CACHE_COUNT_LOAD(&__pcpu_caches[cpu][i]);
            }

            if (cache_total > 0) {
                uint64 cached_pages = (1UL << i) * cache_total;
                uint64 cached_bytes = cached_pages * PAGE_SIZE;

                if (detailed) {
                    printf(" + %ld cached (", cache_total);
                    __print_size(cached_bytes);
                    printf(")");
                }
            }
        }

        if (detailed) {
            printf("\n");
        }
    }

    if (detailed) {
        printf("------------------------\n");
    }
    printf("Buddy: %ld free + %ld cached = %ld pages (", total_free_pages,
           total_cached_pages, total_free_pages + total_cached_pages);
    __print_size((total_free_pages + total_cached_pages) * PAGE_SIZE);
    printf(")\n");
}

void __check_page_pointer_in_range(void *ptr) {
    assert(ptr != NULL, "__check_page_pointer_in_range: NULL pointer");
    assert(ptr > (void *)__pages && ptr < (void *)&__pages[TOTALPAGES],
           "__check_page_pointer_in_range: page pointer out of range");
}

// Helper function to check the integrity of the buddy system
void check_buddy_system_integrity(void) {
    uint64 total_free_pages = 0;
    // Lock all orders to check integrity consistently
    __buddy_pool_lock_range(0, PAGE_BUDDY_MAX_ORDER);
    for (int i = 0; i <= PAGE_BUDDY_MAX_ORDER; i++) {
        int count = __buddy_pools[i].count;
        bool empty = LIST_IS_EMPTY(&__buddy_pools[i].lru_head);
        page_t *pos = NULL;
        page_t *tmp = NULL;
        assert(count >= 0, "buddy pool count is negative");
        assert(empty || count > 0, "buddy pool is not empty but count is zero");
        assert(!empty || count == 0,
               "buddy pool is empty but count is not zero");
        total_free_pages += (1UL << i) * count;
        if (!empty) {
            __check_page_pointer_in_range(__buddy_pools[i].lru_head.prev);
            __check_page_pointer_in_range(__buddy_pools[i].lru_head.next);
            printf("prev page: %p, next page: %p\n",
                   (__buddy_pools[i].lru_head.prev),
                   (__buddy_pools[i].lru_head.next));
        }

        list_foreach_node_safe(&__buddy_pools[i].lru_head, pos, tmp,
                               buddy.lru_entry) {
            // check if the page is a valid buddy page
            assert(PAGE_IS_BUDDY_GROUP_HEAD(pos),
                   "buddy page is not a group head");
            assert(pos->buddy.order == i, "buddy page order mismatch");
            assert(pos->buddy.buddy_head == pos, "buddy head mismatch");
            __check_page_pointer_in_range(pos);
            assert(__page_to_pa(pos) == pos->physical_address,
                   "buddy page physical address mismatch");
            count--;
            printf("count = %d, buddy page: %p, order: %d, physical: 0x%lx\n",
                   count, pos, pos->buddy.order, pos->physical_address);
        }
        assert(count == 0, "buddy pool count mismatch, expected 0, got %d",
               count);
    }
    __buddy_pool_unlock_range(0, PAGE_BUDDY_MAX_ORDER);
}

uint64 sys_memstat(void) {
    int flags_arg;
    argint(0, &flags_arg);
    uint32 flags = (uint32)flags_arg;
    uint64 total_free_pages = 0;
    uint64 total_cached_pages = 0;
    uint64 ret_arr[PAGE_BUDDY_MAX_ORDER + 1] = {0};
    bool empty_arr[PAGE_BUDDY_MAX_ORDER + 1] = {false};
    uint64 free_bytes = 0;
    uint64 used_bytes = 0;
    uint64 ret = 0;

    if (flags & MEMSTAT_INCLUDE_BUDDY) {
        if (flags & MEMSTAT_DETAILED) {
            print_buddy_system_stat(1);
        } else if (flags & MEMSTAT_VERBOSE) {
            print_buddy_system_stat(0);
        }
    }

    if (flags & MEMSTAT_INCLUDE_SLAB) {
        if (flags & MEMSTAT_DETAILED) {
            slab_dump_all(2);
        } else if (flags & MEMSTAT_VERBOSE) {
            slab_dump_all(1);
        }
    }

    __buddy_stat_totals(&total_free_pages, &total_cached_pages,
                        ret_arr, empty_arr);
    free_bytes = (total_free_pages + total_cached_pages) * PAGE_SIZE;

    uint64 managed_bytes = __total_pages() * PAGE_SIZE;
    used_bytes = (managed_bytes > free_bytes) ? (managed_bytes - free_bytes) : 0;

    if ((flags & (MEMSTAT_VERBOSE | MEMSTAT_DETAILED)) != 0) {
        if (flags & MEMSTAT_ADD_FREE) {
            printf("Free: ");
            __print_size(free_bytes);
            printf("\n");
        }
        if (flags & MEMSTAT_ADD_USED) {
            printf("Used: ");
            __print_size(used_bytes);
            printf("\n");
        }
    }

    if (flags & MEMSTAT_ADD_FREE) {
        ret += free_bytes;
    }
    if (flags & MEMSTAT_ADD_USED) {
        ret += used_bytes;
    }
    return ret;
}
