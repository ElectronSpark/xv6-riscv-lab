#ifndef __KERNEL_PAGE_CACHE_H__
#define __KERNEL_PAGE_CACHE_H__

#include "types.h"
#include "list_type.h"
#include "bintree_type.h"
#include "mutex_types.h"
#include "kobject.h"
#include "completion_types.h"
#include "dev_types.h"
#include "workqueue_types.h"


typedef struct page_struct page_t;
struct pcache;
struct pcache_ops;

struct pcache_ops {
    int (*read_page)(struct pcache *pcache, page_t *page);
    int (*write_page)(struct pcache *pcache, page_t *page);
    int (*write_begin)(struct pcache *pcache);
    int (*write_end)(struct pcache *pcache);
    void (*invalidate_page)(struct pcache *pcache, page_t *page);
    void (*mark_dirty)(struct pcache *pcache, page_t *page);
    void (*abort_io)(struct pcache *pcache, page_t *page);
};

#define PCACHE_DEFAULT_DIRTY_RATE 15  // in percentage
#define PCACHE_DEFAULT_MAX_PAGES 4096

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
    list_node_t flush_list; // Link pcache with dirty pages
    list_node_t lru;        // Local LRU list. Only have clean pages with ref_count == 1
    list_node_t dirty_list; // Link pcache with dirty pages
    uint8 dirty_rate;       // Dirty rate in percentage. When the dirty pages exceed this rate, the pcache will be flushed
    uint64 dirty_count;     // Number of dirty pages
    uint64 page_count;      // Total number of pages
    uint64 max_pages;       // Maximum number of pages allowed in the pcache
    uint64 blk_count;       // Total number of blocks (512-byte) managed by the pcache
    mutex_t lock;
    completion_t flush_completion; // Completion for flush operation
    void *private_data; // For filesystem specific data
    union {
        uint64 flags;
        struct {
            uint64 active: 1;
            uint64 flush_requested: 1;
        };
    };
    struct rb_root rb;
    uint64 gfp_flags;
    struct pcache_ops *ops;
    struct work_struct flush_work;
    int flush_error;
};

void pcache_global_init(void);
int pcache_init(struct pcache *pcache);
page_t *pcache_get_page(struct pcache *pcache, uint64 blkno);
void pcache_put_page(struct pcache *pcache, page_t *page);
int pcache_mark_page_dirty(struct pcache *pcache, page_t *page);
int pcache_invalidate_page(struct pcache *pcache, page_t *page);
int pcache_flush(struct pcache *pcache);
// void pcache_destroy(struct pcache *pcache);
// int pcache_evict_pages(struct pcache *pcache, size_t target_size);
// @TODO: do eviction in OOM
int pcache_read_page(struct pcache *pcache, page_t *page);

// ssize_t bread(dev_t dev, uint64 blockno, void *data, size_t size, bool user);
// ssize_t bwrite(dev_t dev, uint64 blockno, const void *data, size_t size, bool user);

#endif /* __KERNEL_PAGE_CACHE_H__ */
