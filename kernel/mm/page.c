// Page allocator, managing free pages.
// Free pages are managed by buddy system.
#include "types.h"
#include "string.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "list.h"
#include "page.h"
#include "page_private.h"
#include "slab.h"


STATIC buddy_pool_t __buddy_pools[PAGE_BUDDY_MAX_ORDER + 1];

// Every physical pages 
// TODO: The number of managed pages are fix right now.
STATIC page_t __pages[TOTALPAGES] = { 0 };
// The start address and the end address of the managed memory
STATIC uint64 __managed_start = KERNBASE;
STATIC uint64 __managed_end = PHYSTOP;

// buddy pool of lower order must be locked before the buddy pool of higher order


#ifdef KERNEL_PAGE_SANITIZER
STATIC_INLINE void __page_sanitizer_check(const char *op, page_t *page, uint64 order, uint64 flags) {
    if (page == NULL) {
        return;
    }
    assert (order <= PAGE_BUDDY_MAX_ORDER, "__page_sanitizer_check: invalid order");
    assert (!flags || page->flags == flags, 
            "__page_sanitizer_check: page flags mismatch, expected 0x%lx, got 0x%lx",
            flags, page->flags);
    assert (page - __pages < TOTALPAGES, "__page_sanitizer_check: page out of bounds");
    assert ((page->physical_address - __managed_start) >> PAGE_SHIFT == 
            (page - __pages), "__page_sanitizer_check: page physical address mismatch, "
            "expected 0x%lx, got 0x%lx", 
            __managed_start + ((page - __pages) << PAGE_SHIFT), 
            page->physical_address);
    for (int i = 0; i < (1 << order); i++) {
        assert (page[i].physical_address == page->physical_address + (i << PAGE_SHIFT),
                "__page_sanitizer_check: page physical address mismatch, "
                "expected 0x%lx, got 0x%lx", 
                page->physical_address + (i << PAGE_SHIFT), 
                page[i].physical_address);
    }
    printf("%s: order %ld, flags 0x%lx, page 0x%lx\n",
           op, order, 0L, __page_to_pa(page));
}
#else
#define __page_sanitizer_check(op, page, order, flags) do { } while (0)
#endif


// acquire the spinlock of a specific buddy pool
STATIC_INLINE void __buddy_pool_lock(uint64 order) {
    if (order > PAGE_BUDDY_MAX_ORDER) {
        panic("__buddy_pool_lock: invalid order");
    }
    spin_acquire(&__buddy_pools[order].lock);
}

// release the spinlock of a specific buddy pool
STATIC_INLINE void __buddy_pool_unlock(uint64 order) {
    if (order > PAGE_BUDDY_MAX_ORDER) {
        panic("__buddy_pool_unlock: invalid order");
    }
    spin_release(&__buddy_pools[order].lock);
}

// acquire spinlocks for a range of buddy pools (from low to high order)
// This maintains lock ordering to prevent deadlock
STATIC_INLINE void __buddy_pool_lock_range(uint64 order_start, uint64 order_end) {
    if (order_start > order_end || order_end > PAGE_BUDDY_MAX_ORDER) {
        panic("__buddy_pool_lock_range: invalid order range");
    }
    for (uint64 i = order_start; i <= order_end; i++) {
        spin_acquire(&__buddy_pools[i].lock);
    }
}

// release spinlocks for a range of buddy pools (in reverse order)
STATIC_INLINE void __buddy_pool_unlock_range(uint64 order_start, uint64 order_end) {
    if (order_start > order_end || order_end > PAGE_BUDDY_MAX_ORDER) {
        panic("__buddy_pool_unlock_range: invalid order range");
    }
    for (int64 i = order_end; i >= (int64)order_start; i--) {
        spin_release(&__buddy_pools[i].lock);
    }
}

// get the total number of pages managed
STATIC_INLINE uint64 __total_pages() {
    return (__managed_end - __managed_start) >> PAGE_SHIFT;
}

// To check if a physical address is within the range of the managed address
#define ADDR_IN_MANAGED(addr)                                               \
    ((addr) >= __managed_start && (addr) < __managed_end)

// Check if a base address of a page is valid.
// A valid page base address should be aligned to the page size, and is in
// the range of managed memory.
STATIC_INLINE bool __page_base_validity(uint64 physical) {
    if ((physical & PAGE_MASK) || !ADDR_IN_MANAGED(physical)) {
        return false;
    }
    return true;
}

// check if a group of flags is valid in initialization process
STATIC_INLINE bool __page_init_flags_validity(uint64 flags) {
    if (flags & (~(PAGE_FLAG_LOCKED))) {
        return false;
    }
    return true;
}

// check if a group of flags is valid in allocation process
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
        // cannot free a page that has been referenced by others
        return false;
    }
    return true;
}

// initialize a page descriptor
// No validity check here
STATIC_INLINE void __page_init( page_t *page, uint64 physical, int ref_count,
                                uint64 flags) {
    memset(page, 0, sizeof(page_t));
    page->physical_address = physical;
    page->flags = flags;
    page->ref_count = ref_count;
    spin_init(&page->lock, "page_t");
}

// initialize a buddy pool
// no validity check here
STATIC_INLINE void __buddy_pool_init() {
    static char *lock_names[PAGE_BUDDY_MAX_ORDER + 1] = {
        "buddy_pool_0", "buddy_pool_1", "buddy_pool_2", "buddy_pool_3",
        "buddy_pool_4", "buddy_pool_5", "buddy_pool_6", "buddy_pool_7",
        "buddy_pool_8", "buddy_pool_9", "buddy_pool_10"
    };
    for (int i = 0; i < PAGE_BUDDY_MAX_ORDER + 1; i++) {
        __buddy_pools[i].count = 0;
        list_entry_init(&__buddy_pools[i].lru_head);
        spin_init(&__buddy_pools[i].lock, lock_names[i]);
    }
}

// initialize a range of page descriptoe with specific flags
STATIC_INLINE int __init_range_flags( uint64 pa_start, uint64 pa_end, 
                                      uint64 flags) {
    page_t *page;
    if (pa_start >= pa_end) {
        // The start address must be lower than the end address
        return -1;
    }
    if (    !__page_base_validity(pa_start) ||
            !__page_base_validity(pa_end - PAGE_SIZE)) {
        // both pa_start and pa_end should be valid physical base page address
        return -1;
    }
    if (!__page_init_flags_validity(flags)) {
        // invalid flags
        return -1;
    }

    printf("init pages from 0x%lx to 0x%lx with flags 0x%lx\n",
          pa_start, pa_end, flags);

    for (uint64 base = pa_start; base < pa_end; base += PAGE_SIZE) {
        page = __pa_to_page(base);
        if (page == NULL) {
            return -1;
        }
        __page_init(page, base, 0, flags);
    }

    return 0;
}

// initialize a page descriptor as a buddy page
STATIC_INLINE void __page_as_buddy( page_t *page, page_t *buddy_head,
                                    uint64 order) {
    __page_init(page, page->physical_address, 0, PAGE_TYPE_BUDDY);
    page->buddy.buddy_head = buddy_head;
    page->buddy.order = order;
    page->buddy.state = BUDDY_STATE_FREE;
    list_entry_init(&page->buddy.lru_entry);
}

// initialize a continuous range of pages as a buddy page in specific order
// will not check validity here
STATIC_INLINE void __page_as_buddy_group(page_t *buddy_head, uint64 order) {
    uint64 count = 1UL << order;
    for (int i = 0; i < count; i++) {
        __page_as_buddy(&buddy_head[i], buddy_head, order);
    }
}

// Attach a buddy head page into the corresponding buddy pool and increse the
// count value of the buddy pool by one.
// will not do validity check here
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
// will not do validity check here
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

// detach a buddy head page from a buddy pool and decrease the count value by
// one.
// will not do validity check here
STATIC_INLINE void __buddy_detach_page(buddy_pool_t *pool, page_t *page) {
    if (LIST_IS_EMPTY(&pool->lru_head)) {
        panic("__buddy_detach_page");
    }
    pool->count--;
    list_node_detach(page, buddy.lru_entry);
}

// Try to calculate the address of a page's buddy with the page's physical
// address. Will not validate the value of order
STATIC_INLINE uint64 __get_buddy_addr(uint64 physical, uint32 order) {
    uint64 __buddy_base_address =
        PAGE_ADDR_GET_BUDDY_GROUP_ADDR(physical, order);
    __buddy_base_address ^= PAGE_BUDDY_BYTES(order);
    return __buddy_base_address;
}

// try to find a page of a buddy
// return the buddy page descriptor if found
// return NULL if didn't find
STATIC_INLINE page_t *__get_buddy_page(page_t *page) {
    uint64 buddy_base;
    page_t *buddy_head;
    if (!PAGE_IS_BUDDY_GROUP_HEAD(page)) {
        // must be the page descriptor of a buddy header page
        return NULL;
    }
    if (page->buddy.order >= PAGE_BUDDY_MAX_ORDER) {
        // page size reach PAGE_BUDDY_MAX_ORDER doesn't have buddy
        return NULL;
    }
    buddy_base = __get_buddy_addr(page->physical_address, page->buddy.order);
    buddy_head = __pa_to_page(buddy_base);
    if (buddy_head == NULL || !PAGE_IS_BUDDY_GROUP_HEAD(buddy_head)
        || buddy_head->buddy.order != page->buddy.order) {
        // didn't find a complete buddy page.
        return NULL;
    }
    if (LIST_ENTRY_IS_DETACHED(&buddy_head->buddy.lru_entry)) {
        // The buddy header page is not in the buddy pool, which means it's
        // holding by someone else right now.
        return NULL;
    }
    if (buddy_head->buddy.state != BUDDY_STATE_FREE) {
        // The buddy is in the middle of being merged or is otherwise unavailable
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

// Split a buddy page in half and return the header page of the later half of 
// the splitted buddy pages. This fundtion will not update the tail pages 
// immediately to avoid useless updates. Have to call __page_order_change_commit
// after page splitting.
// Return NULL if falied to split
STATIC_INLINE page_t *__buddy_split(page_t *page) {
    int order_after;
    page_t *buddy;
    if (!PAGE_IS_BUDDY_GROUP_HEAD(page)) {
        return NULL;
    }
    if (page->buddy.order == 0) {
        // a single page connot be splitted.
        return NULL;
    }
    order_after = page->buddy.order - 1;
    buddy = page + (1UL << order_after);
    __page_as_buddy(page, page, order_after);
    __page_as_buddy(buddy, buddy, order_after);
    return buddy;
}

// Merge two buddy pages in and return the header page of the merged 
// buddy page. This fundtion will not update the tail pages immediately 
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

STATIC page_t *__buddy_get(uint64 order, uint64 flags) {
    buddy_pool_t *pool = NULL;
    page_t *page = NULL;
    page_t *buddy = NULL;
    uint64 tmp_order = order;
    uint64 page_count;

    if (!__page_flags_validity(flags)) {
        return NULL;
    }
    if (order > PAGE_BUDDY_MAX_ORDER) {
        return NULL;
    }

    // Lock the requested order first
    __buddy_pool_lock(order);
    pool = &__buddy_pools[order];
    page = __buddy_pop_page(pool);

    // found available buddy pages at the requested order
    if (page != NULL) {
        goto found;
    }

    // Need to search higher orders - lock them all to prevent races
    // We already hold lock for 'order', now acquire locks for order+1 to MAX
    for (tmp_order = order + 1; tmp_order <= PAGE_BUDDY_MAX_ORDER; tmp_order++) {
        __buddy_pool_lock(tmp_order);
    }

    // Try to find a bigger buddy page to split
    for (tmp_order = order + 1; tmp_order <= PAGE_BUDDY_MAX_ORDER; tmp_order++) {
        pool = &__buddy_pools[tmp_order];
        page = __buddy_pop_page(pool);
        // break the for loop when finding a free buddy page
        if (page != NULL) {
            break;
        }
    }

    // if still not found available buddy pages after the for loop, then
    // there's no buddy page that meets the requirement.
    if (page == NULL) {
        // Release all locks we acquired
        for (uint64 i = order; i <= PAGE_BUDDY_MAX_ORDER; i++) {
            __buddy_pool_unlock(i);
        }
        return NULL;
    }

    // if found one, split it and return the header page from one of the
    // splitted groups.
    do {
        buddy = __buddy_split(page);
        if (buddy == NULL) {
            // There's no way the splitting operation here would fail. If it
            // happens, then something wrong happened.
            panic("__buddy_get(): failed splitting buddy pages");
        }

        // put the later half back
        tmp_order--;
        pool = &__buddy_pools[tmp_order];
        __page_order_change_commit(buddy);
        __buddy_push_page(pool, buddy);
    } while (tmp_order > order);

    // Release locks for orders we no longer need (order+1 to MAX)
    for (uint64 i = order + 1; i <= PAGE_BUDDY_MAX_ORDER; i++) {
        __buddy_pool_unlock(i);
    }

found:
    page_count = 1UL << order;
    for (uint64 i = 0; i < page_count; i++) {
        page[i].flags = 0;
        __page_init(&page[i], page[i].physical_address, 1, flags);
    }
    __buddy_pool_unlock(order);
    return page;
}

// Put a page back to buddy system
// Right now pages can only be put one by one
STATIC int __buddy_put(page_t *page) {
    buddy_pool_t *pool = NULL;
    page_t *buddy = NULL;
    uint64 tmp_order = 0;

    if (!__page_is_freeable(page)) {
        // cannot free a page that's not freeable
        return -1;
    }

    // Initialize page as buddy and mark as merging
    __page_as_buddy(page, page, 0);
    page_lock_acquire(page);
    page->buddy.state = BUDDY_STATE_MERGING;
    page_lock_release(page);

    for (tmp_order = 0; tmp_order <= PAGE_BUDDY_MAX_ORDER; tmp_order++) {
        pool = &__buddy_pools[tmp_order];

        // Lock pool to search for buddy
        __buddy_pool_lock(tmp_order);

        // try to find the buddy page
        buddy = __get_buddy_page(page);
        if (buddy != NULL) {
            // if buddy was found, detach it from pool first
            __buddy_detach_page(pool, buddy);
            __buddy_pool_unlock(tmp_order);

            // Now change buddy state (it's no longer in pool, so safe)
            page_lock_acquire(buddy);
            buddy->buddy.state = BUDDY_STATE_MERGING;
            page_lock_release(buddy);
        } else {
            // No buddy found, this is where the page will stay
            // Change state to FREE before adding to pool
            page_lock_acquire(page);
            page->buddy.state = BUDDY_STATE_FREE;
            page_lock_release(page);

            __page_order_change_commit(page);
            __buddy_push_page(pool, page);
            __buddy_pool_unlock(tmp_order);
            break;
        }

        if (tmp_order == PAGE_BUDDY_MAX_ORDER) {
            // Reached max order with a buddy, add merged page to pool
            __buddy_pool_lock(tmp_order);
            page_lock_acquire(page);
            page->buddy.state = BUDDY_STATE_FREE;
            page_lock_release(page);

            __page_order_change_commit(page);
            __buddy_push_page(pool, page);
            __buddy_pool_unlock(tmp_order);
            break;
        }

        // Merge the two buddies
        page = __buddy_merge(page, buddy);
        if (page == NULL) {
            panic("__buddy_put(): Get NULL after merging pages");
        }

        // Mark the merged page as MERGING for next iteration
        // Lock only the head page (the other half is unreachable)
        page_lock_acquire(page);
        page->buddy.state = BUDDY_STATE_MERGING;
        page_lock_release(page);
    }

    return 0;
}

// init buddy system and add the given range of pages into it
int page_buddy_init(uint64 pa_start, uint64 pa_end) {
    if (__managed_start < pa_start
        && __init_range_flags(__managed_start, pa_start, PAGE_FLAG_LOCKED) != 0) {
            panic("page_buddy_init(): lower locked memory");
    }
    if (pa_end < __managed_end
        && __init_range_flags(pa_end, __managed_end, PAGE_FLAG_LOCKED) != 0) {
            panic("page_buddy_init(): higher locked memory");
    }
    if (__init_range_flags(pa_start, pa_end, 0) != 0) {
        panic("page_buddy_init(): free range");
    }

    __buddy_pool_init();
    
    for (uint64 base = pa_start; base < pa_end; base += PAGE_SIZE) {
        page_t *page = __pa_to_page(base);
        if (page == NULL) {
            panic("page_buddy_init(): get NULL page");
        }
        if (__buddy_put(page) != 0) {
            panic("page_buddy_init(): page put error");
        }
    }
#ifndef HOST_TEST
    print_buddy_system_stat();
    printf("page_buddy_init(): buddy pages from 0x%lx to 0x%lx\n"
           "    managed pages from 0x%lx to 0x%lx\n",
           pa_start, pa_end, __managed_start, __managed_end);
#endif

    return 0;
}

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

page_t *__page_alloc(uint64 order, uint64 flags) {
    if (order > PAGE_BUDDY_MAX_ORDER) {
        return NULL;
    }
    page_t *ret = __buddy_get(order, flags);
    __page_sanitizer_check("page_alloc", ret, order, flags);
    return ret;
}

// the base address of the page should be aligned to order
// otherwise panic
void __page_free(page_t *page, uint64 order) {
    __page_sanitizer_check("page_free", page, order, 0);
    uint64 count = 1UL << order;
    buddy_pool_t *pool = NULL;
    page_t *buddy = NULL;
    uint64 tmp_order = order;

    if (page == NULL) {
        return;
    }
    if (order > PAGE_BUDDY_MAX_ORDER) {
        panic("trying to free too many pages");
    }
    if (page->physical_address & PAGE_BUDDY_OFFSET_MASK(order)) {
        panic("free pages not aligned to order");
    }

    // Check that all pages in the block are freeable
    for (uint64 i = 0; i < count; i++) {
        if (!__page_is_freeable(&page[i])) {
            panic("__page_free(): trying to free non-freeable page");
        }
    }

    // Initialize the block as a buddy group and mark as merging
    __page_as_buddy_group(page, order);
    page_lock_acquire(page);
    page->buddy.state = BUDDY_STATE_MERGING;
    page_lock_release(page);

    for (tmp_order = order; tmp_order <= PAGE_BUDDY_MAX_ORDER; tmp_order++) {
        pool = &__buddy_pools[tmp_order];

        // Lock pool to search for buddy
        __buddy_pool_lock(tmp_order);

        buddy = __get_buddy_page(page);
        if (buddy != NULL) {
            // Detach buddy from pool first
            __buddy_detach_page(pool, buddy);
            __buddy_pool_unlock(tmp_order);

            // Change buddy state (no longer in pool, so no pool lock needed)
            page_lock_acquire(buddy);
            buddy->buddy.state = BUDDY_STATE_MERGING;
            page_lock_release(buddy);
        } else {
            // No buddy found, add to pool
            page_lock_acquire(page);
            page->buddy.state = BUDDY_STATE_FREE;
            page_lock_release(page);

            __page_order_change_commit(page);
            __buddy_push_page(pool, page);
            __buddy_pool_unlock(tmp_order);
            break;
        }

        if (tmp_order == PAGE_BUDDY_MAX_ORDER) {
            // Reached max order, add to pool
            __buddy_pool_lock(tmp_order);
            page_lock_acquire(page);
            page->buddy.state = BUDDY_STATE_FREE;
            page_lock_release(page);

            __page_order_change_commit(page);
            __buddy_push_page(pool, page);
            __buddy_pool_unlock(tmp_order);
            break;
        }

        // Merge and continue
        page = __buddy_merge(page, buddy);
        if (page == NULL) {
            panic("__page_free(): Get NULL after merging pages");
        }

        // Mark merged page as MERGING (only lock first half's head)
        page_lock_acquire(page);
        page->buddy.state = BUDDY_STATE_MERGING;
        page_lock_release(page);
    }
}

// helper function for __page_alloc. Convert the page struct to the base
// address of the page
void *page_alloc(uint64 order, uint64 flags) {
    void *pa;
    page_t *page = __page_alloc(order, flags);
    if (page == NULL) {
        return NULL;
    }

    pa = (void *)__page_to_pa(page);

    if(pa) {
        memset((char*)pa, 5, PGSIZE << order); // fill with junk
    }
    else
        panic("page_alloc");
    return pa;
}

// helper function for __page_free. Convert the base address of the page to be
// free to page struct
void page_free(void *ptr, uint64 order) {
    page_t *page = __pa_to_page((uint64)ptr);
    __page_free(page, order);
}

void page_lock_acquire(page_t *page) {
    if (page == NULL) {
        return;
    }
    spin_acquire(&page->lock);
}

void page_lock_release(page_t *page) {
    if (page == NULL) {
        return;
    }
    spin_release(&page->lock);
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

// return the reference count of a page
// return -1 if failed
int page_refcnt(void *physical) {
  page_t *page = __pa_to_page((uint64)physical);
  return page_ref_count(page);
}

// helper function to __page_ref_inc
int page_ref_inc(void *ptr) {
    page_t *page = __pa_to_page((uint64)ptr);
    return __page_ref_inc(page);
}

// helper function to __page_ref_dec
int page_ref_dec(void *ptr) {
    page_t *page = __pa_to_page((uint64)ptr);
    return __page_ref_dec(page);
}

// Get a page struct from its physical base address
page_t *__pa_to_page(uint64 physical) {
    if (__page_base_validity(physical)) {
        return &__pages[(physical - __managed_start) >> PAGE_SHIFT];
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

uint64 managed_page_base() {
    return __managed_start;
}

// record the number of buddies in each order
// will return an array of order 0 to size - 1
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

void print_buddy_system_stat(void) {
    uint64 total_free_pages = 0;
    uint64 ret_arr[PAGE_BUDDY_MAX_ORDER + 1] = { 0 };
    bool empty_arr[PAGE_BUDDY_MAX_ORDER + 1] = { false };
    page_buddy_stat(ret_arr, empty_arr, PAGE_BUDDY_MAX_ORDER + 1);
    for (int i = 0; i <= PAGE_BUDDY_MAX_ORDER; i++) {
        printf("order(%d): %ld - %s\n", i, ret_arr[i], empty_arr[i] ? "empty" : "not empty");
        total_free_pages += (1UL << i) * ret_arr[i];
    }
    printf("total free pages: %ld\n", total_free_pages);
}

void __check_page_pointer_in_range(void *ptr) {
    assert(ptr != NULL, "__check_page_pointer_in_range: NULL pointer");
    assert(ptr > (void *)__pages && ptr < (void *)&__pages[TOTALPAGES], "__check_page_pointer_in_range: page pointer out of range");
}

// helper function to check the integrity of the buddy system
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
        assert(!empty || count == 0, "buddy pool is empty but count is not zero");
        total_free_pages += (1UL << i) * count;
        if (!empty) {
            __check_page_pointer_in_range(__buddy_pools[i].lru_head.prev);
            __check_page_pointer_in_range(__buddy_pools[i].lru_head.next);
            printf("prev page: %p, next page: %p\n",
                   (__buddy_pools[i].lru_head.prev),
                   (__buddy_pools[i].lru_head.next));
        }

        list_foreach_node_safe(&__buddy_pools[i].lru_head, pos, tmp, buddy.lru_entry) {
            // check if the page is a valid buddy page
            assert(PAGE_IS_BUDDY_GROUP_HEAD(pos), "buddy page is not a group head");
            assert(pos->buddy.order == i, "buddy page order mismatch");
            assert(pos->buddy.buddy_head == pos, "buddy head mismatch");
            __check_page_pointer_in_range(pos);
            assert(__page_to_pa(pos) == pos->physical_address, 
                   "buddy page physical address mismatch");
            count--;
            printf("count = %d, buddy page: %p, order: %d, physical: 0x%lx\n",
                   count, pos, pos->buddy.order, pos->physical_address);
        }
        assert(count == 0, "buddy pool count mismatch, expected 0, got %d", count);
    }
    __buddy_pool_unlock_range(0, PAGE_BUDDY_MAX_ORDER);
}

uint64 sys_memstat(void) {
    print_buddy_system_stat();
    return 0;
}
