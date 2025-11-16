#ifndef KERNEL_PAGE_CACHE_TYPES_H
#define KERNEL_PAGE_CACHE_TYPES_H


#include "types.h"
#include "list_type.h"
#include "bintree_type.h"
#include "mutex_types.h"
#include "rwlock_types.h"
#include "spinlock.h"
#include "kobject.h"
#include "completion_types.h"
#include "dev_types.h"
#include "workqueue_types.h"


typedef struct page_struct page_t;
struct pcache;
struct pcache_node;
struct pcache_ops;

struct pcache_ops {
    int (*read_page)(struct pcache *pcache, page_t *page);
    int (*write_page)(struct pcache *pcache, page_t *page);
    int (*write_begin)(struct pcache *pcache, page_t *page);
    int (*write_end)(struct pcache *pcache, page_t *page);
    void (*mark_dirty)(struct pcache *pcache, page_t *page);
};

#define PCACHE_DEFAULT_DIRTY_RATE 15  // in percentage
#define PCACHE_DEFAULT_MAX_PAGES 4096

#define PCACHE_FLUSH_INTERVAL_JIFFS (30 * HZ) // 30 seconds

// Page cache structure
//
// Needs to reside on other objects.
//
// Needs to be zero-initialized before use
// The following members should user should always specify before initialization:
//  - ops: 
//      the operations for the pcache
//  - block_count:
//      the total number of blocks (512-byte) managed by the pcache
//      Should not change after initialization
//  The following members are optional for users to specify before initialization:
//  - gfp_flags: 
//      the flags for page allocation, default to 0
//  - private_data:
//      filesystem specific data, default to NULL
//  - dirty_rate:
//      the dirty rate in percentage, default to PCACHE_DEFAULT_DIRTY_RATE
//  - max_pages:
//      the maximum number of pages allowed in the pcache, default to PCACHE_DEFAULT_MAX_PAGES
struct pcache {
    list_node_t list_entry; // Link active pcache in the global list
    list_node_t lru;        // Local LRU list. Only have clean pages with ref_count == 1
    list_node_t dirty_list; // Link pcache with dirty pages
    uint8 dirty_rate;       // Dirty rate in percentage. When the dirty pages exceed this rate, the pcache will be flushed
    int64 lru_count;        // Number of pages in the lru list
    int64 dirty_count;      // Number of dirty pages
    int64 page_count;       // Total number of pages
    uint64 max_pages;       // Maximum number of pages allowed in the pcache
    uint64 blk_count;       // Total number of blocks (512-byte) managed by the pcache
    uint64 last_request;    // Last IO request timestamp in jiffies
    uint64 last_flushed;    // Last flushed timestamp in jiffies
    completion_t flush_completion; // Completion for flush operation
    void *private_data; // For filesystem specific data
    union {
        uint64 flags;
        struct {
            uint64 active: 1;
            uint64 flush_requested: 1;
        };
    };
    struct rb_root page_map;
    struct spinlock spinlock;   // Spinlock to protect the pcache structure
    struct spinlock tree_lock;           // protect the red-black tree structure
    uint64 gfp_flags;
    struct pcache_ops *ops;
    struct work_struct flush_work; // Work structure for flush operation
    int flush_error;
};

// Extension of page structure for page cache use
// Protected by page structures' spinlock
// Need to acquire both pcache spinlock and page spinlock when 
// attaching or detaching pcache_node
struct pcache_node {
    struct rb_node     tree_entry; // node in the rb-tree of pcache
    list_node_t        lru_entry;  // entry in the local dirty or lru list of pcache
    struct pcache      *pcache;    // pointer to the parent pcache
    page_t             *page;      // pointer to the page
    void               *data;      // pointer to the data area in the page
    int64              page_count; // number of pages in this node
    uint64             last_request;    // Last IO request timestamp in jiffies
    uint64             last_flushed;    // Last flushed timestamp in jiffies
    struct {
        uint64 dirty: 1;   // whether the page is dirty
        uint64 uptodate: 1; // whether the page data is up-to-date
        uint64 io_in_progress: 1; // whether the page is in IO progressing state
    };
    uint64             blkno;      // starting block number (512-byte) of the page
    size_t             size;       // size of valid data area in the page
    completion_t       io_completion; // Completion for IO operation
};

#endif // KERNEL_PAGE_CACHE_TYPES_H
