// implementation of bi-directional list
#ifndef __KERNEL_PAGE_TYPE_H
#define __KERNEL_PAGE_TYPE_H

#include "types.h"
#include "spinlock.h"
#include "list_type.h"
#include "bintree.h"

// The maximum size of a buddy page is 2**PAGE_BUDDY_MAX_ORDER continuous pages
#define PAGE_BUDDY_MAX_ORDER        10

// Buddy page states
#define BUDDY_STATE_FREE            0  // Free and available for allocation in buddy pool
#define BUDDY_STATE_MERGING         1  // Currently being merged with its buddy
#define BUDDY_STATE_CACHED          2  // Cached in per-CPU cache


// for pointers to slab pools
typedef struct slab_struct slab_t;
struct pcache;
struct pcache_node;

enum page_type {
    PAGE_TYPE_ANON = 0UL,      // Anonymous page
    PAGE_TYPE_BUDDY,         // Buddy page
    PAGE_TYPE_SLAB,          // Slab page
    PAGE_TYPE_PGTABLE,       // Page table page
    PAGE_TYPE_PCACHE,        // Page cache page
    __PAGE_TYPE_MAX
};

#define PAGE_FLAG_TYPE_BITS         8UL
#define PAGE_FLAG_TYPE_MASK         ((1UL << PAGE_FLAG_TYPE_BITS) - 1UL)
#define PAGE_FLAG_MASK              (~PAGE_FLAG_TYPE_MASK)
#define PAGE_FLAG_GET_TYPE(flags)   ((flags) & PAGE_FLAG_TYPE_MASK)
#define PAGE_FLAG_SET_TYPE(flags, type)   \
    do { \
        (flags) &= PAGE_FLAG_MASK; \
        (flags) |= ((uint64)(type) & PAGE_FLAG_TYPE_MASK); \
    } while (0)
#define PAGE_FLAG_IS_TYPE(flags, type)   \
    (PAGE_FLAG_GET_TYPE(flags) == (type))
#define PAGE_IS_TYPE(page, type)   \
    ((page) && PAGE_FLAG_IS_TYPE((page)->flags, (type)))

#if __PAGE_TYPE_MAX > PAGE_FLAG_TYPE_MASK
    #error "Not enough bits to store page type in page flags"
#endif

typedef struct page_struct {
    uint64          physical_address;
    union {

        uint64          flags;
    };
// this part is a reference to the linux kernel
// #define PAGE_FLAG_UPTODATE          (1U << 8)
// #define PAGE_FLAG_DIRTY             (1U << 9)
// #define PAGE_FLAG_WRITEBACK         (1U << 10)
// #define PAGE_FLAG_RECLAIM           (1U << 9)
// #define PAGE_FLAG_MMAP              (1U << 11)
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
#define PAGE_FLAG_LOCKED            (1U << 26)
#define PAGE_FLAG_IO_PROGRESSING    (1U << 28)
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
            uint32                 state;  // buddy state: FREE, MERGING, etc.
        } buddy;
        /* Slab pages
            Objects occupied less than one page are managed by slab system.
            One slab  */
        struct {
            slab_t              *slab;     // pointing its slab descriptor
        } slab;
        /* Page cache pages
            Pages used by pcache system to cache disk blocks */
        struct {
            struct pcache      *pcache;    // pointing its pcache
            struct pcache_node *pcache_node; // pointing its pcache node
        } pcache;
    };
} page_t;

// pools storing each size of free buddy pages
// __buddy_pools[ page_order ]
typedef struct {
    list_node_t     lru_head;
    uint64          count;
    spinlock_t      lock;  // per-order lock for fine-grained concurrency
} buddy_pool_t;

#endif              /* __KERNEL_PAGE_TYPE_H */
