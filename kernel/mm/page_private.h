// declarations on some functions an macros not visible to outside
#ifndef __KERNEL_PAGE_PRIVATE_H__
#define __KERNEL_PAGE_PRIVATE_H__

#include <mm/page_type.h>

// The page struct belongs to a buddy page (header)
#define PAGE_IS_BUDDY(page) PAGE_IS_TYPE(page, PAGE_TYPE_BUDDY)

// The page struct is a tail page of a buddy group
#define PAGE_IS_TAIL(page) PAGE_IS_TYPE(page, PAGE_TYPE_TAIL)

// The page struct is the head of a buddy page (identified by PAGE_TYPE_BUDDY)
#define PAGE_IS_BUDDY_GROUP_HEAD(page) PAGE_IS_BUDDY(page)

// The page struct is a tail of a buddy page (uses PAGE_TYPE_TAIL)
#define PAGE_IS_BUDDY_GROUP_TAIL(page)                                         \
    (PAGE_IS_TAIL(page) && (page)->tail.head_page != (page))

// Check if page belongs to a buddy group (either header or tail)
#define PAGE_IS_BUDDY_MEMBER(page) (PAGE_IS_BUDDY(page) || PAGE_IS_TAIL(page))

// Get the header of any page in a buddy group
// For header pages: returns itself
// For tail pages: returns head_page pointer
#define PAGE_GET_BUDDY_GROUP_HEAD(page)                                        \
    ({                                                                         \
        page_t *__buddy_head_ptr = NULL;                                       \
        if (PAGE_IS_BUDDY(page)) {                                             \
            __buddy_head_ptr = (page);                                         \
        } else if (PAGE_IS_TAIL(page)) {                                       \
            __buddy_head_ptr = (page)->tail.head_page;                         \
        }                                                                      \
        __buddy_head_ptr;                                                      \
    })

// The size of a buddy group in bytes
#define PAGE_BUDDY_BYTES(order) (1UL << ((order) + PAGE_SHIFT))

// The address mask to get the offset address of a buddy group
#define PAGE_BUDDY_OFFSET_MASK(order) (PAGE_BUDDY_BYTES(order) - 1)

// The address mask to get the base address of a buddy group
#define PAGE_BUDDY_BASE_MASK(order) (~PAGE_BUDDY_OFFSET_MASK(order))

// Get the base address of a buddy group
#define PAGE_ADDR_GET_BUDDY_GROUP_ADDR(physical, order)                        \
    ((physical) & PAGE_BUDDY_BASE_MASK(order))

// Check weather two pages are buddies
#define PAGES_ARE_BUDDIES(page1, page2)                                        \
    ((page1) != NULL && (page2) != NULL &&                                     \
     (page1)->physical_address != (page2)->physical_address &&                 \
     (page1)->buddy.order == (page2)->buddy.order &&                           \
     (page1)->buddy.order < PAGE_BUDDY_MAX_ORDER &&                            \
     (((page1)->physical_address ^ PAGE_BUDDY_BYTES((page1)->buddy.order)) ==  \
      (page2)->physical_address))

static inline void __lock_two_pages(page_t *page1, page_t *page2) {
    if (page1 < page2) {
        page_lock_acquire(page1);
        page_lock_acquire(page2);
    } else if (page1 > page2) {
        page_lock_acquire(page2);
        page_lock_acquire(page1);
    } else {
        page_lock_acquire(page1);
    }
}

static inline void __unlock_two_pages(page_t *page1, page_t *page2) {
    if (page1 < page2) {
        page_lock_release(page2);
        page_lock_release(page1);
    } else if (page1 > page2) {
        page_lock_release(page1);
        page_lock_release(page2);
    } else {
        page_lock_release(page1);
    }
}

STATIC_INLINE void __buddy_pool_lock(uint64 order);
STATIC_INLINE void __buddy_pool_unlock(uint64 order);
STATIC_INLINE void __buddy_pool_lock_range(uint64 order_start,
                                           uint64 order_end);
STATIC_INLINE void __buddy_pool_unlock_range(uint64 order_start,
                                             uint64 order_end);
STATIC_INLINE uint64 __total_pages();
STATIC_INLINE bool __page_base_validity(uint64 physical);
STATIC_INLINE bool __page_init_flags_validity(uint64 flags);
STATIC_INLINE bool __page_flags_validity(uint64 flags);
STATIC_INLINE bool __page_is_freeable(page_t *page);
// STATIC_INLINE void __page_init(page_t *page, uint64 physical, int ref_count,
//                                  uint64 flags);
STATIC_INLINE void __buddy_pool_init();
STATIC_INLINE int __init_range_flags(uint64 pa_start, uint64 pa_end,
                                     uint64 flags);
STATIC_INLINE void __page_as_buddy_tail(page_t *page, page_t *buddy_head);
STATIC_INLINE void __page_as_buddy(page_t *page, page_t *buddy_head,
                                   uint64 order, uint32 state);
STATIC_INLINE void __page_as_buddy_group(page_t *buddy_head, uint64 order,
                                         uint32 state);
STATIC_INLINE void __page_order_change_commit(page_t *page);
STATIC_INLINE void __page_split_commit_later_half(page_t *new_header,
                                                  uint64 order);
STATIC_INLINE void __page_merge_commit_later_half(page_t *merged_header,
                                                  uint64 merged_order);
STATIC_INLINE void __buddy_push_page(buddy_pool_t *pool, page_t *page);
STATIC_INLINE page_t *__buddy_pop_page(buddy_pool_t *pool);
STATIC_INLINE void __buddy_detach_page(buddy_pool_t *pool, page_t *page);
STATIC_INLINE uint64 __get_buddy_addr(uint64 physical, uint32 order);
STATIC_INLINE page_t *__lock_get_buddy_page(page_t *page);
STATIC_INLINE page_t *__buddy_split(page_t *page);
STATIC_INLINE page_t *__buddy_merge(page_t *page1, page_t *page2);
STATIC page_t *__buddy_get(uint64 order, uint64 flags);
STATIC int __buddy_put(page_t *page);

#ifdef HOST_TEST
extern buddy_pool_t __buddy_pools[PAGE_BUDDY_MAX_ORDER + 1];
extern page_t *__pages;
extern uint64 __managed_start;
extern uint64 __managed_end;
#endif

#endif /* __KERNEL_PAGE_PRIVATE_H__ */
