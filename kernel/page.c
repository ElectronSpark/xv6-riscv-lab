// Page allocator, managing free pages.
// Free pages are managed by buddy system.
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "list.h"
#include "page.h"
#include "page_private.h"



STATIC buddy_pool_t __buddy_pools[PAGE_BUDDY_MAX_ORDER + 1];

// Every physical pages 
// TODO: The number of managed pages are fix right now.
STATIC page_t __pages[TOTALPAGES] = { 0 };
// The start address and the end address of the managed memory
STATIC uint64 __managed_start = KERNBASE;
STATIC uint64 __managed_end = PHYSTOP;

// acquire the spinlock of a buddy pool
STATIC_INLINE void __buddy_pool_lock(buddy_pool_t *pool) {
    acquire(&pool->lock);
}

// release the spinlock of a buddy pool
STATIC_INLINE void __buddy_pool_unlock(buddy_pool_t *pool) {
    release(&pool->lock);
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
    if (flags & (~(PAGE_FLAG_SLAB | PAGE_FLAG_ANON | PAGE_FLAG_PGTABLE))) {
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
    initlock(&page->lock, "page_t");
}

// initialize a buddy pool
// no validity check here
STATIC_INLINE void __buddy_pool_init() {
    // __buddy_pools[PAGE_BUDDY_MAX_ORDER + 1];
    for (int i = 0; i < PAGE_BUDDY_MAX_ORDER + 1; i++) {
        initlock(&(__buddy_pools[i].lock), "buddy_system_pool");
        __buddy_pools[i].count = 0;
        list_entry_init(&__buddy_pools[i].lru_head);
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
    __page_init(page, page->physical_address, 0, PAGE_FLAG_BUDDY);
    page->buddy.buddy_head = buddy_head;
    page->buddy.order = order;
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
    pool = &__buddy_pools[order];
    // pages managed by buddy system are hold by buddy system, so only need to
    // acquire the lock of buddy pool to hold these pages.
    __buddy_pool_lock(pool);
    {
        page = __buddy_pop_page(pool);
    }
    __buddy_pool_unlock(pool);

    // found available buddy pages, 
    if (page != NULL) {
        goto found;
    }

    // if don't find, try to get a bigger buddy page and split it.
    for (tmp_order = order + 1; tmp_order <= PAGE_BUDDY_MAX_ORDER; tmp_order++) {
        pool = &__buddy_pools[tmp_order];
        // pages managed by buddy system are hold by buddy system, so only need to
        // acquire the lock of buddy pool to hold these pages.
        __buddy_pool_lock(pool);
        {
            page = __buddy_pop_page(pool);
        }
        __buddy_pool_unlock(pool);
        // break the for loop when finding a free buddy page
        if (page != NULL) {
            break;
        }
    }

    // if still not found available buddy pages after the for loop, then
    // there's no buddy page that meets the requirement.
    if (page == NULL) {
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
        __buddy_pool_lock(pool);
        {
            __buddy_push_page(pool, buddy);
        }
        __buddy_pool_unlock(pool);
    } while (tmp_order > order);

found:
    page_count = 1UL << order;
    for (int i = 0; i < page_count; i++) {
        page->flags &= ~PAGE_FLAG_BUDDY;
        __page_init(&page[i], page[i].physical_address, 1, flags);
    }
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

    __page_as_buddy(page, page, 0);

    for (tmp_order = 0; tmp_order <= PAGE_BUDDY_MAX_ORDER; tmp_order++) {
        // to prevent the buddy page from being allocated before we locked
        // the buddy pool.
        pool = &__buddy_pools[tmp_order];
        __buddy_pool_lock(pool);
        {
            // try to find the buddy page
            buddy = __get_buddy_page(page);
            if (buddy != NULL) {
                // if buddy was found, pop the buddy for merge
                __buddy_detach_page(pool, buddy);
            } 
        }
        __buddy_pool_unlock(pool);
        if (buddy == NULL) {
            // if no buddy was found, put it back to the pool and break the loop
            __page_order_change_commit(page);
            __buddy_pool_lock(pool);
            {
                __buddy_push_page(pool, page);
            }
            __buddy_pool_unlock(pool);
            break;
        } else {
            // otherwise, merge them, then continue the for loop to find the next
            // buddy page group
            page = __buddy_merge(page, buddy);
            if (page == NULL) {
                panic("__buddy_put(): Get NULL after merging pages");
            }
        }
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
#endif

    return 0;
}

STATIC_INLINE int __page_ref_inc_unlocked(page_t *page) {
    if (page == NULL) {
        return -1;
    }
    if (page->ref_count == 0) {
        // page with 0 reference should be put back to the buddy system
        return -1;
    }
    page->ref_count++;
    return page->ref_count;
}

STATIC_INLINE int __page_ref_dec_unlocked(page_t *page) {
    if (page == NULL) {
        return -1;
    }
    if (page->ref_count > 0) {
        page->ref_count--;
        return page->ref_count;
    }
    return 0;
}

page_t *__page_alloc(uint64 order, uint64 flags) {
    if (!__page_flags_validity(flags)) {
        // @TODO: Some flags need to be mutually exclusive
        return NULL;
    }
    if (order > PAGE_BUDDY_MAX_ORDER) {
        return NULL;
    }
    return __buddy_get(order, flags);
}

// the base address of the page should be aligned to order
// otherwise panic
void __page_free(page_t *page, uint64 order) {
    uint64 count = 1UL << order;
    if (page == NULL) {
        return;
    }
    if (order > PAGE_BUDDY_MAX_ORDER) {
        panic("trying to free too many pages");
    }
    if (page->physical_address & PAGE_BUDDY_OFFSET_MASK(order)) {
        panic("free pages not aligned to order");
    }
    for (int i = 0; i < count; i++) {
        if (__buddy_put(&page[i]) != 0) {
            panic("failed to free page(s)");
        }
    }
}

// helper function for __page_alloc. Convert the page struct to the base
// address of the page
void *page_alloc(uint64 order, uint64 flags) {
    void *pa;
    page_t *page = __page_alloc(0, flags);
    if (page == NULL) {
        return NULL;
    }

    pa = (void *)__page_to_pa(page);

    if(pa)
        memset((char*)pa, 5, PGSIZE << order); // fill with junk
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

void page_lock_aqcuire(page_t *page) {
    if (page == NULL) {
        return;
    }
    acquire(&page->lock);
}

void page_lock_release(page_t *page) {
    if (page == NULL) {
        return;
    }
    release(&page->lock);
}

int __page_ref_inc(page_t *page) {
    int ret = 0;
    if (page == NULL) {
        return -1;
    }
    page_lock_aqcuire(page);
    {
        ret = __page_ref_inc_unlocked(page);
    }
    page_lock_release(page);
    return ret;
}

int __page_ref_dec(page_t *page) {
    int ret = 0;
    if (page == NULL) {
        return -1;
    }
    page_lock_aqcuire(page);
    {
        ret = __page_ref_dec_unlocked(page);
    }
    page_lock_release(page);
    if (ret == 0) {
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
    for (int i = 0; i <= PAGE_BUDDY_MAX_ORDER && i < size; i++) {
        __buddy_pool_lock(&__buddy_pools[i]);
        ret_arr[i] = __buddy_pools[i].count;
        if (empty_arr != NULL) {
            empty_arr[i] = LIST_IS_EMPTY(&__buddy_pools[i].lru_head);
        }
        __buddy_pool_unlock(&__buddy_pools[i]);
    }
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
