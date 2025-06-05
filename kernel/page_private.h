// declarations on some functions an macros not visible to outside
#ifndef __KERNEL_PAGE_PRIVATE_H__
#define __KERNEL_PAGE_PRIVATE_H__

#include "page_type.h"

// The page struct belongs to a buddy page
#define PAGE_IS_BUDDY(page)                                                 \
    ((page) != NULL && ((page)->flags & PAGE_FLAG_BUDDY))

// The page struct is the head of a buddy page
#define PAGE_IS_BUDDY_GROUP_HEAD(page)                                      \
    (PAGE_IS_BUDDY(page) && (page)->buddy.buddy_head == (page))

// The page struct is a tail of a buddy page
#define PAGE_IS_BUDDY_GROUP_TAIL(page)                                      \
    (PAGE_IS_BUDDY(page) && (page)->buddy_head != (page))

// Try to get the head of a buddy page
#define PAGE_GET_BUDDY_GROUP_HEAD(page) ({                                  \
    page_t *__buddy_head_ptr = NULL;                                        \
    if (PAGE_IS_BUDDY(page)) {                                              \
        __buddy_head_ptr = (page)->buddy.buddy_head;                        \
    }                                                                       \
    __buddy_head_ptr;                                                       \
})

// The size of a buddy group in bytes
#define PAGE_BUDDY_BYTES(order)                                             \
    (1UL << ((order) + PAGE_SHIFT))

// The address mask to get the offset address of a buddy group
#define PAGE_BUDDY_OFFSET_MASK(order)                                       \
    (PAGE_BUDDY_BYTES(order) - 1)

// The address mask to get the base address of a buddy group
#define PAGE_BUDDY_BASE_MASK(order)                                         \
    (~PAGE_BUDDY_OFFSET_MASK(order))

// Get the base address of a buddy group
#define PAGE_ADDR_GET_BUDDY_GROUP_ADDR(physical, order)                     \
    ((physical) & PAGE_BUDDY_BASE_MASK(order))

// Check weather two pages are buddies
#define PAGES_ARE_BUDDIES(page1, page2)                                     \
    ((page1) != NULL                                                        \
    && (page2) != NULL                                                      \
    && (page1)->physical_address != (page2)->physical_address               \
    && (page1)->buddy.order == (page2)->buddy.order                         \
    && (page1)->buddy.order < PAGE_BUDDY_MAX_ORDER                          \
    && (((page1)->physical_address ^ PAGE_BUDDY_BYTES((page1)->buddy.order))\
        == (page2)->physical_address) )


STATIC_INLINE void __buddy_pool_lock(buddy_pool_t *pool);
STATIC_INLINE void __buddy_pool_unlock(buddy_pool_t *pool);
STATIC_INLINE uint64 __total_pages();
STATIC_INLINE bool __page_base_validity(uint64 physical);
STATIC_INLINE bool __page_init_flags_validity(uint64 flags);
STATIC_INLINE bool __page_flags_validity(uint64 flags);
STATIC_INLINE bool __page_is_freeable(page_t *page);
STATIC_INLINE void __page_init(page_t *page, uint64 physical, int ref_count,
                                 uint64 flags);
STATIC_INLINE void __buddy_pool_init();
STATIC_INLINE int __init_range_flags( uint64 pa_start, uint64 pa_end, 
                                      uint64 flags);
STATIC_INLINE void __page_as_buddy( page_t *page, page_t *buddy_head,
                                    uint64 order);
STATIC_INLINE void __page_as_buddy_group(page_t *buddy_head, uint64 order);
STATIC_INLINE void __page_order_change_commit(page_t *page);
STATIC_INLINE void __buddy_push_page(buddy_pool_t *pool, page_t *page);
STATIC_INLINE page_t *__buddy_pop_page(buddy_pool_t *pool);
STATIC_INLINE void __buddy_detach_page(buddy_pool_t *pool, page_t *page);
STATIC_INLINE uint64 __get_buddy_addr(uint64 physical, uint32 order);
STATIC_INLINE page_t *__get_buddy_page(page_t *page);
STATIC_INLINE page_t *__buddy_split(page_t *page);
STATIC_INLINE page_t *__buddy_merge(page_t *page1, page_t *page2);
STATIC page_t *__buddy_get(uint64 order, uint64 flags);
STATIC int __buddy_put(page_t *page);

#endif  /* __KERNEL_PAGE_PRIVATE_H__ */
