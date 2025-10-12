#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "page.h"
#include "list.h"
#include "completion.h"
#include "rbtree.h"
#include "workqueue.h"
#include "timer.h"
#include "kobject.h"
#include "pcache.h"
#include "errno.h"

// Loking order:
// 1. global pcache mutex
// 2. pcache lock
// 3. page lock

/******************************************************************************
 * Global variables
 *****************************************************************************/
// Store all pcache with dirty pages
static list_node_t __global_dirty_list = {0};
static int __global_dirty_list_count = 0;
static struct workqueue *__global_pcache_flush_wq = NULL;
static mutex_t __pcache_global_mutex = {0};

/******************************************************************************
 * Predefine functions
 *****************************************************************************/
static void __pcache_attach_global_dirty(struct pcache *pcache);
static void __pcache_detach_global_dirty(struct pcache *pcache);
static void __pcache_queue_flush(struct pcache *pcache);

/******************************************************************************
 * Helper functions to call optional pcache operations
 *****************************************************************************/
static int __pcache_read_page(struct pcache *pcache, page_t *page) {
    assert(pcache->ops && pcache->ops->read_page, "__pcache_read_page: read_page operation not defined");
    return pcache->ops->read_page(pcache, page);
}

static int __pcache_write_page(struct pcache *pcache, page_t *page) {
    assert(pcache->ops && pcache->ops->write_page, "__pcache_write_page: write_page operation not defined");
    return pcache->ops->write_page(pcache, page);
}

static int __pcache_write_begin(struct pcache *pcache) {
    if (pcache->ops && pcache->ops->write_begin) {
        return pcache->ops->write_begin(pcache);
    }
    return 0;
}

static int __pcache_write_end(struct pcache *pcache) {
    if (pcache->ops && pcache->ops->write_end) {
        return pcache->ops->write_end(pcache);
    }
    return 0;
}

static void __pcache_invalidate_page(struct pcache *pcache, page_t *page) {
    if (pcache->ops && pcache->ops->invalidate_page) {
        pcache->ops->invalidate_page(pcache, page);
    }
}

static void __pcache_mark_dirty(struct pcache *pcache, page_t *page) {
    if (pcache->ops && pcache->ops->mark_dirty) {
        pcache->ops->mark_dirty(pcache, page);
    }
}

static void __pcache_abort_io(struct pcache *pcache, page_t *page) {
    if (pcache->ops && pcache->ops->abort_io) {
        pcache->ops->abort_io(pcache, page);
    }
}

/******************************************************************************
 * Internal helper functions
 *****************************************************************************/
static int __pcache_is_active(struct pcache *pcache) {
    return pcache->active;
}

static int __pcache_page_validate(struct pcache *pcache, page_t *page) {
    if (page == NULL) {
        return -EINVAL;
    }
    if (!(page->flags & PAGE_FLAG_PCACHE)) {
        return -EINVAL;
    }
    if (page->pcache.pcache != pcache) {
        return -EINVAL;
    }
    return 0;
}

static int __pcache_init_validate(struct pcache *pcache) {
    // compulsory members check
    if (pcache == NULL) {
        return -EINVAL;
    }
    if (pcache->ops == NULL) {
        return -EINVAL;
    }
    if (pcache->ops->read_page == NULL ||
        pcache->ops->write_page == NULL) {
        return -EINVAL;
    }
    if (pcache->blk_count == 0) {
        return -EINVAL;
    }
    // Zero members check
    if (pcache->page_count != 0 ||
        pcache->dirty_count != 0 ||
        pcache->flags != 0) {
        return -EINVAL;
    }
    if (!rb_root_is_empty(&pcache->rb) ||
        !LIST_ENTRY_UNINITIALIZED(&pcache->lru) ||
        !LIST_ENTRY_UNINITIALIZED(&pcache->dirty_list) ||
        !LIST_ENTRY_UNINITIALIZED(&pcache->flush_list)) {
        return -EINVAL;
    }
    return 0;
}

static int __pcache_page_init_validate(struct pcache *pcache, uint64 blkno, size_t offset, size_t size) {
    if (pcache == NULL) {
        return -EINVAL;
    }
    if (blkno >= pcache->blk_count) {
        return -EINVAL;
    }
    if (size == 0 || size > PGSIZE || (size & (511))) {
        // size should be non-zero, not exceed one page and aligned to 512 bytes
        return -EINVAL;
    }
    if (offset & (511)) {
        // offset should be aligned to 512 bytes
        return -EINVAL;
    }
    if (offset + size > PGSIZE) {
        // cached area should not exceed one page
        return -EINVAL;
    }
    return 0;
}

// Remove a page from lru list
static void __pcache_lru_remove_page(struct pcache *pcache, page_t *page) {
    if (LIST_NODE_IS_DETACHED(page, pcache.lru_entry) == 0) {
        list_node_detach(page, pcache.lru_entry);
    }
}

// Push a page into the tail of lru list
static void __pcache_lru_push_page(struct pcache *pcache, page_t *page) {
    __pcache_lru_remove_page(pcache, page);
    list_node_push(&pcache->lru, page, pcache.lru_entry);
}

// Initialize a newly allocated page or a evicted page for pcache use
static void __pcache_page_init(page_t *page, struct pcache *pcache, uint64 blkno, size_t offset, size_t size) {
    page->pcache.pcache = pcache;
    page->pcache.blkno = blkno;
    rb_node_init(&page->pcache.node);
    list_entry_init(&page->pcache.lru_entry);
    list_entry_init(&page->pcache.dirty_entry);
    page->pcache.offset = offset;
    page->pcache.size = size;
}

// alloc and free page for pcache
// Just allocate page and initialize it. No insertion to pcache is done here.
static page_t* __pcache_alloc_page(struct pcache *pcache, uint64 blkno, size_t offset, size_t size) {
    if (__pcache_page_init_validate(pcache, blkno, offset, size) != 0) {
        return NULL;
    }
    // Find a free page
    uint64 gfp_flags = pcache->gfp_flags | PAGE_FLAG_PCACHE;
    gfp_flags &= ~PAGE_FLAG_UPTODATE;
    page_t *page = __page_alloc(0, gfp_flags);
    if (page == NULL) {
        return NULL;
    }
    __pcache_page_init(page, pcache, blkno, offset, size);
    return page;
}

// evict a specific page from pcache.
// Page being evicted could be redirect to other blocks or be freed.
// Return 0 on success, -ERRNO on failure
static int __pcache_evict_page(struct pcache *pcache, page_t *page) {
    int retval = -EINVAL;
    page_lock_acquire(page);
    {
        if (page->pcache.pcache != pcache) {
            printf("warning: evicting a page not belonging to this pcache\n");
            retval = -EINVAL;
            goto fail_ret;
        }
        if (page->flags & PAGE_FLAG_DIRTY) {
            printf("warning: evicting a dirty page\n");
            retval = -EBUSY;
            goto fail_ret;
        }
        if (page_ref_count(page) > 1) {
            // page is referenced by entities other than pcache
            printf("warning: evicting a referenced page\n");
            retval = -EBUSY;
            goto fail_ret;
        }
        // remove from rb-tree
        struct rb_node *deleted = rb_delete_node_color(&pcache->rb, &page->pcache.node);
        assert(deleted == &page->pcache.node, "__pcache_evict_page: rb_delete_node_color failed");
        // remove from lru list
        __pcache_lru_remove_page(pcache, page);
        pcache->page_count--;
    }
// ret:
    retval = 0;
fail_ret:
    page_lock_release(page);
    return retval;
}

// get a evicted page from lru list and reclaim it for new use
// Return 0 on success, -ERRNO on failure
static page_t *__pcache_get_evicted_page(struct pcache *pcache, uint64 blkno, size_t offset, size_t size) {
    if (__pcache_page_init_validate(pcache, blkno, offset, size) != 0) {
        return NULL;
    }
    page_t *page = LIST_FIRST_NODE(&pcache->lru, page_t, pcache.lru_entry);
    if (page == NULL) {
        return NULL;
    }
    int retval = __pcache_evict_page(pcache, page);
    if (retval != 0) {
        // cannot evict the page
        return NULL;
    }
    // re-initialize the page for new use
    __pcache_page_init(page, pcache, blkno, offset, size);
    return page;
}

// Add a page into pcache
// The page should be newly allocated or evicted from pcache
static void __pcache_add_page(struct pcache *pcache, page_t *page) {
    // Insert into rb-tree
    struct rb_node *inserted = rb_insert_color(&pcache->rb, &page->pcache.node);
    assert(inserted == &page->pcache.node, "__pcache_add_page: rb_insert_color failed");
    pcache->page_count++;
}

// Get a page in pcache by block number
// Return NULL if not found
static page_t *__pcache_get_page(struct pcache *pcache, uint64 blkno) {
    struct rb_node *node = rb_find_key(&pcache->rb, blkno);
    if (node == NULL) {
        return NULL;
    }
    return container_of(node, page_t, pcache.node);
}

// Manipulate global lock
void __pcache_global_lock(void) {
    mutex_lock(&__pcache_global_mutex);
}

void __pcache_global_unlock(void) {
    mutex_unlock(&__pcache_global_mutex);
}

// Manipulate dirty list
void __pcache_attach_global_dirty(struct pcache *pcache);
void __pcache_detach_global_dirty(struct pcache *pcache);

// Wait for flush to complete
int __pcache_wait_flush_complete(struct pcache *pcache);

// Notify flush complete
void __pcache_notify_flush_complete(struct pcache *pcache);

// Add a flush request to the workqueue
// will set flush_requested flag and remove pcache from the global dirty list
// The callback function will flush the data and clear flush_requested flag
void __pcache_queue_flush(struct pcache *pcache);

/******************************************************************************
 * Call back functions for workqueue and timer
 *****************************************************************************/
// Worker function to flush dirty pcache
static void __pcache_flush_worker(struct work_struct *work) {
}

// worker function to read page from disk
static void __pcache_read_page_worker(struct work_struct *work) {
}

// timer callback to periodically flush dirty pages
static void __pcache_flush_timer_callback(struct timer_node *node) {
}

/******************************************************************************
 * red-black tree callback functions
 *****************************************************************************/
// Compare function for red-black tree
static int __pcache_rb_compare(uint64 key1, uint64 key2) {
    if (key1 < key2) {
        return -1;
    } else if (key1 > key2) {
        return 1;
    } else {
        return 0;
    }
}

// Get key function for red-black tree
static uint64 __pcache_rb_get_key(struct rb_node *node) {
    page_t *page = container_of(node, page_t, pcache.node);
    return page->pcache.blkno;
}

static struct rb_root_opts __pcache_rb_opts = {
    .keys_cmp_fun = __pcache_rb_compare,
    .get_key_fun = __pcache_rb_get_key,
};

/******************************************************************************
 * Public API functions
 *****************************************************************************/
// Init page cache subsystem
void pcache_global_init(void) {
    list_entry_init(&__global_dirty_list);
    mutex_init(&__pcache_global_mutex, "global_pcache_mutex");
    __global_dirty_list_count = 0;
    __global_pcache_flush_wq = workqueue_create("pcache_flush_wq", WORKQUEUE_DEFAULT_MAX_ACTIVE);
    assert(__global_pcache_flush_wq != NULL, "Failed to create global pcache flush workqueue");
    printf("Page cache subsystem initialized\n");
}

int pcache_init(struct pcache *pcache) {
    int ret = __pcache_init_validate(pcache);
    if (ret != 0) {
        return ret;
    }
    // Initialize members
    list_entry_init(&pcache->flush_list);
    list_entry_init(&pcache->lru);
    list_entry_init(&pcache->dirty_list);
    pcache->dirty_count = 0;
    pcache->page_count = 0;
    pcache->flags = 0;
    rb_root_init(&pcache->rb, &__pcache_rb_opts);
    if (pcache->gfp_flags == 0) {
        pcache->gfp_flags = 0;
    }
    mutex_init(&pcache->lock, "pcache_mutex");
    completion_init(&pcache->flush_completion);
    pcache->private_data = NULL;
    pcache->active = 0;
    pcache->flush_requested = 0;
    if (pcache->max_pages == 0) {
        pcache->max_pages = PCACHE_DEFAULT_MAX_PAGES;
    }
    if (pcache->dirty_rate == 0 || pcache->dirty_rate > 100) {
        pcache->dirty_rate = PCACHE_DEFAULT_DIRTY_RATE;
    }
    return 0;
}

// Try to get a page from pcache
// The reference count of the page will be increased by 1 if found (2 minimum)
// Block number is in 512-byte block unit
// The block number of the page is aligned to 8 blocks (4KB)
page_t *pcache_get_page(struct pcache *pcache, uint64 blkno) {
    if (pcache == NULL) {
        return NULL;
    }
    mutex_lock(&pcache->lock);
    {
        if (!__pcache_is_active(pcache)) {
            mutex_unlock(&pcache->lock);
            return NULL;
        }
        if (blkno >= pcache->blk_count) {
            mutex_unlock(&pcache->lock);
            return NULL;
        }
        blkno &= ~7UL;
        page_t *node = __pcache_get_page(pcache, blkno);
        if (node != NULL) {
            // found
            page_lock_acquire(node);
            {
                int refcount = page_ref_inc_unlocked(node);
                if (refcount == 2) {
                    __pcache_lru_remove_page(pcache, node);
                }
            }
            page_lock_release(node);
            mutex_unlock(&pcache->lock);

            return node;
        }

        // If no found, try to allocate a new page
        // First check if page count exceeds the limit
        if (pcache->page_count >= pcache->max_pages) {
            // need to evict some pages
            node = __pcache_get_evicted_page(pcache, blkno, 0, PGSIZE);
        } else {
            node = __pcache_alloc_page(pcache, blkno, 0, PGSIZE);
        }
        if (node != NULL) {
            page_lock_acquire(node);
            {
                // increase the reference count for the caller
                int refcount = page_ref_inc_unlocked(node);
                assert(refcount == 2, "pcache_get_page: refcount should be 2 after allocation");
                __pcache_add_page(pcache, node);
            }
            page_lock_release(node);
            mutex_unlock(&pcache->lock);
            return node;
        }
    }
    mutex_unlock(&pcache->lock);
    return NULL;
}

void pcache_put_page(struct pcache *pcache, page_t *page) {
    if (pcache == NULL || page == NULL) {
        return;
    }
    mutex_lock(&pcache->lock);
    page_lock_acquire(page);
    {
        if (__pcache_page_validate(pcache, page) != 0) {
            printf("warning: pcache_put_page: invalid page\n");
            goto ret;
        }
        int refcount = page_ref_dec_unlocked(page);
        // When refcount is 1, pages should only freed after eviction
        assert(refcount > 0, "pcache_put_page: refcount should be positive after put");
        if (refcount == 1) {
            // only referenced by pcache now
            if (!(page->flags & PAGE_FLAG_DIRTY)) {
                // clean page, put it into lru list
                __pcache_lru_push_page(pcache, page);
            }
        }
    }
ret:
    page_lock_release(page);
    mutex_unlock(&pcache->lock);
}

int pcache_mark_page_dirty(struct pcache *pcache, page_t *page) {
    if (pcache == NULL || page == NULL) {
        return -1;
    }
    int retval = 0;
    bool need_flush_check = false;

    mutex_lock(&pcache->lock);
    page_lock_acquire(page);
    {
        if (__pcache_page_validate(pcache, page) != 0) {
            retval = -EINVAL;
            goto ret;
        }
        if (page->flags & PAGE_FLAG_DIRTY) {
            // already dirty
            assert(!LIST_ENTRY_IS_DETACHED(&page->pcache.dirty_entry), 
                    "pcache_mark_page_dirty: dirty page in lru list");
            retval = 0;
            goto ret;
        }
        page->flags |= PAGE_FLAG_DIRTY;
        // Assume driver wants to override the whole cached area
        page->flags |= PAGE_FLAG_UPTODATE;
        __pcache_lru_remove_page(pcache, page);
        list_node_push(&pcache->dirty_list, page, pcache.dirty_entry);
        pcache->dirty_count++;
        __pcache_mark_dirty(pcache, page);
        retval = 0;
        need_flush_check = true;
        goto ret;
    }
ret:
    page_lock_release(page);
    if (need_flush_check) {
        int dirty_rate = pcache->dirty_count * 100 / pcache->page_count;
        if (pcache->dirty_count && LIST_ENTRY_IS_DETACHED(&pcache->flush_list)) {
            // attach to global dirty list
            mutex_unlock(&pcache->lock);
            __pcache_global_lock();
            mutex_lock(&pcache->lock);
            if (LIST_ENTRY_IS_DETACHED(&pcache->flush_list)) {
                __pcache_attach_global_dirty(pcache);
            }
            __pcache_global_unlock();
        }
        if (dirty_rate >= pcache->dirty_rate && !pcache->flush_requested) {
            // need to flush dirty pages
            __pcache_queue_flush(pcache);
        }
    }
    mutex_unlock(&pcache->lock);
    return retval;
}

int pcache_invalidate_page(struct pcache *pcache, page_t *page) {
    if (pcache == NULL || page == NULL) {
        return -1;
    }
    int retval = 0;

    mutex_lock(&pcache->lock);
    page_lock_acquire(page);
    {
        if (__pcache_page_validate(pcache, page) != 0) {
            retval = -EINVAL;
            goto ret;
        }
        if (page->flags & PAGE_FLAG_DIRTY) {
            // dirty page cannot be invalidated
            retval = -EBUSY;
            goto ret;
        }
        if (page_ref_count(page) > 1) {
            // page is referenced by entities other than pcache
            retval = -EBUSY;
            goto ret;
        }
        page->flags &= ~PAGE_FLAG_UPTODATE;
        page->flags &= ~PAGE_FLAG_DIRTY;
        __pcache_invalidate_page(pcache, page);
    }
ret:
    page_lock_release(page);
    mutex_unlock(&pcache->lock);
    return retval;
}

int pcache_flush(struct pcache *pcache) {
    if (pcache == NULL) {
        return -1;
    }
    int retval = 0;

    mutex_lock(&pcache->lock);
    {
        if (pcache->dirty_count == 0) {
            // no dirty pages
            mutex_unlock(&pcache->lock);
            return 0;
        }
        if (!pcache->flush_requested) {
            __pcache_queue_flush(pcache);
        }
    }
    mutex_unlock(&pcache->lock);
    retval = __pcache_wait_flush_complete(pcache);
    return retval;
}

int pcache_read_page(struct pcache *pcache, page_t *page) {
    if (pcache == NULL || page == NULL) {
        return -1;
    }
    int retval = 0;

    mutex_lock(&pcache->lock);
    page_lock_acquire(page);
    {
        if (__pcache_page_validate(pcache, page) != 0) {
            retval = -EINVAL;
            goto ret;
        }
        if (page->flags & PAGE_FLAG_UPTODATE) {
            // already up-to-date
            retval = 0;
            goto ret;
        }
        // start reading
        retval = __pcache_read_page(pcache, page);
        if (retval == 0) {
            retval = 0;
            page->flags |= PAGE_FLAG_UPTODATE;
            goto ret;
        }
    }
ret:
    page_lock_release(page);
    mutex_unlock(&pcache->lock);
    return retval;
}
