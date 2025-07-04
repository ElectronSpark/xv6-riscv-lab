// implementation of bi-directional list
#ifndef __KERNEL_PAGE_TYPE_H
#define __KERNEL_PAGE_TYPE_H

#include "types.h"
#include "spinlock.h"
#include "list_type.h"

// The maximum size of a buddy page is 2**PAGE_BUDDY_MAX_ORDER continuous pages
#define PAGE_BUDDY_MAX_ORDER        10


// for pointers to slab pools
typedef struct slab_struct slab_t;

typedef struct page_struct {
    uint64          physical_address;
    uint64          flags;
// this part is a reference to the linux kernel
#define PAGE_FLAG_LOCKED            (1U << 0)
// #define PAGE_FLAG_ERROR             (1U << 1)
// #define PAGE_FLAG_REFERENCED        (1U << 2)
#define PAGE_FLAG_UPTODATE          (1U << 3)
#define PAGE_FLAG_DIRTY             (1U << 4)
// #define PAGE_FLAG_LRU               (1U << 5)
// #define PAGE_FLAG_ACTIVE            (1U << 6)
#define PAGE_FLAG_SLAB              (1U << 7)
// #define PAGE_FLAG_WRITEBACK         (1U << 8)
// #define PAGE_FLAG_RECLAIM           (1U << 9)
#define PAGE_FLAG_BUDDY             (1U << 10)
// #define PAGE_FLAG_MMAP              (1U << 11)
#define PAGE_FLAG_ANON              (1U << 12)
// #define PAGE_FLAG_SWAPCACHE         (1U << 13)
// #define PAGE_FLAG_SWAPBACKED        (1U << 14)
// #define PAGE_FLAG_COMPOUND_HEAD     (1U << 15)
// #define PAGE_FLAG_COMPOUND_TAIL     (1U << 16)
// #define PAGE_FLAG_HUGE              (1U << 17)
// #define PAGE_FLAG_UNEVICTABLE       (1U << 18)
// #define PAGE_FLAG_HWPOISON          (1U << 19)
// #define PAGE_FLAG_NOPAGE            (1U << 20)
// #define PAGE_FLAG_KSM               (1U << 21)
// #define PAGE_FLAG_THP               (1U << 22)
// #define PAGE_FLAG_OFFLINE           (1U << 23)
// #define PAGE_FLAG_ZERO_PAGE         (1U << 24)
// #define PAGE_FLAG_IDLE              (1U << 25)
#define PAGE_FLAG_PGTABLE           (1U << 26)
    int             ref_count;
    spinlock_t      lock;
    /* choose the section of the union according to the page type */
    union {
        /* Anonymous page
            Can be mapped into virtual memory, but doesn't belong to any 
            entity, and can be referenced many times  */
        struct {
            //
        } anon;
        /* Buddy pages
            Pages managed by buddy system are free pages*/
        struct {
            list_node_t            lru_entry;
            struct page_struct     *buddy_head;
            uint32                 order;
        } buddy;
        /* Slab pages
            Objects occupied less than one page are managed by slab system.
            One slab  */
        struct {
            slab_t              *slab;     // pointing its slab descriptor
        } slab;
    };
} page_t;

// pools storing each size of free buddy pages
// __buddy_pools[ page_order ]
typedef struct {
    list_node_t     lru_head;
    spinlock_t      lock;
    uint64          count;
} buddy_pool_t;

#endif              /* __KERNEL_PAGE_TYPE_H */
