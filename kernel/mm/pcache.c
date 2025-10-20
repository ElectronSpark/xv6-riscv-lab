#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "page.h"
#include "list.h"
#include "sched.h"
#include "completion.h"
#include "rwlock.h"
#include "rbtree.h"
#include "workqueue.h"
#include "kobject.h"
#include "pcache.h"
#include "errno.h"
#include "slab.h"

// Locking order:
// 1. __pcache_global_spinlock
// 2. pcache spinlock
// 3. page lock
// 4. pcache tree_lock

/******************************************************************************
 * Global variables
 *****************************************************************************/
// Store all pcache with dirty pages
static list_node_t __global_pcache_list = {0};
static int __global_pcache_count = 0;
static struct workqueue *__global_pcache_flush_wq = NULL;
static spinlock_t __pcache_global_spinlock = {0};
static slab_cache_t __pcache_node_slab = {0};

/******************************************************************************
 * Predefine functions
 *****************************************************************************/
static void __pcache_register(struct pcache *pcache);
// static void __pcache_unregister(struct pcache *pcache);

static bool __pcache_page_in_lru(page_t *page);
static bool __pcache_page_in_dlist(page_t *page);
static void __pcache_lru_push_page(struct pcache *pcache, page_t *page);
static void __pcache_lru_remove_page(struct pcache *pcache, page_t *page);

static void __pcache_push_local_dirty(struct pcache *pcache, page_t *page);
static void __pcache_detach_local_dirty(struct pcache *pcache, page_t *page);
static void __pcache_page_set_dirty(struct pcache *pcache, page_t *page);
static void __pcache_page_clear_dirty(struct pcache *pcache, page_t *page);

static page_t* __pcache_find_page(struct pcache *pcache, uint64 blkno);
static void __pcache_page_init(page_t *page, struct pcache *pcache, uint64 blkno, size_t offset, size_t size);
static page_t* __pcache_alloc_page(struct pcache *pcache, uint64 blkno, size_t offset, size_t size);
static int __pcache_evict_page(struct pcache *pcache, page_t *page);
static page_t *__pcache_get_evicted_page(struct pcache *pcache, uint64 blkno, size_t offset, size_t size);
static void __pcache_add_page(struct pcache *pcache, page_t *page);
static page_t *__pcache_get_page(struct pcache *pcache, uint64 blkno);

static void __pcache_queue_flush(struct pcache *pcache);
static void __pcache_flush_worker(struct work_struct *work);
static int __pcache_wait_flush_complete(struct pcache *pcache);
static void __pcache_notify_flush_complete(struct pcache *pcache);
static void __pcache_flush_done(struct pcache *pcache);

static void __pcache_flusher_start(void);
static int __pcache_wait_flusher(void);
static void __pcache_flusher_done(void);

static void __pcache_page_io_begin(page_t *page);
static void __pcache_page_io_end(page_t *page);
static int __pcache_page_io_wait(page_t *page);
static bool __pcache_page_io_in_progress(page_t *page);

static int __pcache_write_begin(struct pcache *pcache);
static int __pcache_write_end(struct pcache *pcache);
static int __pcache_write_page(struct pcache *pcache, page_t *page);
static int __pcache_read_page(struct pcache *pcache, page_t *page);

static int __pcache_tree_lock(struct pcache *pcache);
static void __pcache_tree_unlock(struct pcache *pcache);
static void __pcache_spin_lock(struct pcache *pcache);
static void __pcache_spin_unlock(struct pcache *pcache);

static void __pcache_global_lock(void);
static void __pcache_global_unlock(void);

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

static void __pcache_mark_dirty(struct pcache *pcache, page_t *page) {
    if (pcache->ops && pcache->ops->mark_dirty) {
        pcache->ops->mark_dirty(pcache, page);
    }
}

/******************************************************************************
 * Internal helper functions
 *****************************************************************************/
static int __pcache_is_active(struct pcache *pcache) {
    return pcache->active;
}

static int __pcache_page_validate(struct pcache *pcache, page_t *page) {
    if (!PAGE_IS_TYPE(page, PAGE_TYPE_PCACHE)) {
        return -EINVAL;
    }
    if (page->pcache.pcache != pcache) {
        return -EINVAL;
    }
    if (page_ref_count(page) <= 1) {
        // When the ref count is 1, the page should be referenced only by the pcache
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
    if (!rb_root_is_empty(&pcache->page_map) ||
        !LIST_ENTRY_UNINITIALIZED(&pcache->lru) ||
        !LIST_ENTRY_UNINITIALIZED(&pcache->dirty_list) ||
        !LIST_ENTRY_UNINITIALIZED(&pcache->list_entry)) {
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

// Check if a page is in lru list
static bool __pcache_page_in_lru(page_t *page) {
    return !(page->flags & (PAGE_FLAG_DIRTY | PAGE_FLAG_IO_PROGRESSING)) &&
            !LIST_NODE_IS_DETACHED(page, pcache.lru_entry);
}

// Check if a page is in dirty list
static bool __pcache_page_in_dlist(page_t *page) {
    return (page->flags & (PAGE_FLAG_DIRTY | PAGE_FLAG_IO_PROGRESSING)) == PAGE_FLAG_DIRTY &&
            !LIST_NODE_IS_DETACHED(page, pcache.lru_entry);
}

// Remove a page from lru list
static void __pcache_lru_remove_page(struct pcache *pcache, page_t *page) {
    __pcache_spin_lock_assert_holding(pcache);
    page_lock_assert_holding(page);
    assert(page->flags & (PAGE_FLAG_DIRTY | PAGE_FLAG_IO_PROGRESSING) == 0, 
            "__pcache_lru_remove_page: page is dirty or IO in progress");
    assert(LIST_NODE_IS_DETACHED(page, pcache.lru_entry), 
            "__pcache_lru_remove_page: page is not in lru list");
    list_node_detach(page, pcache.lru_entry);
    pcache->lru_count--;
    assert(pcache->lru_count >= 0, "__pcache_lru_remove_page: pcache lru count underflow");
}

// Push a page into the tail of lru list
static void __pcache_lru_push_page(struct pcache *pcache, page_t *page) {
    __pcache_spin_lock_assert_holding(pcache);
    page_lock_assert_holding(page);
    assert(page->flags & (PAGE_FLAG_DIRTY | PAGE_FLAG_IO_PROGRESSING) == 0, 
            "__pcache_lru_remove_page: page is dirty or IO in progress");
    if (LIST_NODE_IS_DETACHED(page, pcache.lru_entry)) {
        pcache->lru_count++;
    } else {
        list_node_detach(page, pcache.lru_entry);
    }
    list_node_push(&pcache->lru, page, pcache.lru_entry);
}

// Page dirty and clean lifecycle:
// page clean --> ( mark dirty ) --> (attach to dirty list) --> page dirty
//                                                                |
// +--------------------------------------------------------------+
// |
// +--> (detach from dirty list) --> ( mark io progressing ) --> page io progressing
//                                                                |
// +--------------------------------------------------------------+
// |
// +--> ( io end ) --> ( clear dirty and io progressing ) --> page clean

// Put a dirty page into the tail of local dirty list and increase dirty count
// if the page was clean before
static void __pcache_push_local_dirty(struct pcache *pcache, page_t *page) {
    __pcache_spin_lock_assert_holding(pcache);
    page_lock_assert_holding(page);
    assert((page->flags & (PAGE_FLAG_DIRTY | PAGE_FLAG_IO_PROGRESSING)) == PAGE_FLAG_DIRTY, 
            "__pcache_push_local_dirty: page is not dirty or IO in progress");
    if (LIST_NODE_IS_DETACHED(page, pcache.lru_entry)) {
        pcache->dirty_count++;
        list_node_push(&pcache->dirty_list, page, pcache.lru_entry);
    }
}

// Remove a dirty page from local dirty list and reduce dirty count
static void __pcache_detach_local_dirty(struct pcache *pcache, page_t *page) {
    __pcache_spin_lock_assert_holding(pcache);
    page_lock_assert_holding(page);
    assert(page->flags & PAGE_FLAG_DIRTY, 
            "__pcache_detach_local_dirty: page is not dirty");
    assert(!LIST_NODE_IS_DETACHED(page, pcache.lru_entry), 
            "__pcache_detach_local_dirty: page is not in dirty list");
    list_node_detach(page, pcache.lru_entry);
    pcache->dirty_count--;
    assert(pcache->dirty_count >= 0, "__pcache_detach_local_dirty: pcache dirty count underflow");
}

// Set a page dirty
// page in IO progressing cannot be set dirty
static void __pcache_page_set_dirty(struct pcache *pcache, page_t *page) {
    page_lock_assert_holding(page);
    assert(!(page->flags & PAGE_FLAG_IO_PROGRESSING), 
            "__pcache_attach_local_dirty: page is in IO progressing");
    page->flags |= PAGE_FLAG_DIRTY;
}

// Clear dirty status of a page
// page in IO progressing cannot be cleared dirty
static void __pcache_page_clear_dirty(struct pcache *pcache, page_t *page) {
    page_lock_assert_holding(page);
    assert(!(page->flags & PAGE_FLAG_IO_PROGRESSING), 
            "__pcache_attach_local_dirty: page is in IO progressing");
    page->flags &= ~PAGE_FLAG_DIRTY;
}

// Manage IO status of a page
static void __pcache_page_io_begin(page_t *page) {
    page_lock_assert_holding(page);
    assert(!(page->flags & PAGE_FLAG_IO_PROGRESSING), 
            "__pcache_page_io_begin: page IO is already in progress");
    page->flags |= PAGE_FLAG_IO_PROGRESSING;
}

static void __pcache_page_io_end(page_t *page) {
    page_lock_assert_holding(page);
    assert(page->flags & PAGE_FLAG_IO_PROGRESSING, 
            "__pcache_page_io_end: page IO is not in progress");
    page->flags &= ~PAGE_FLAG_IO_PROGRESSING;
    wakeup_on_chan(&page->pcache);
}

static int __pcache_page_io_wait(page_t *page) {
    page_lock_assert_holding(page);
    while (__pcache_page_io_in_progress(page)) {
        sleep_on_chan(&page->pcache, &page->lock);
    }
    return 0;
}

static bool __pcache_page_io_in_progress(page_t *page) {
    return (page->flags & PAGE_FLAG_IO_PROGRESSING) != 0;
}

// Find a page in pcache by block number
// Will return NULL if not found
// Will not increase page ref count
// Will remove the page from lru list if in
static page_t* __pcache_find_page(struct pcache *pcache, uint64 blkno) {

}

// Initialize a newly allocated page or a evicted page for pcache use
static void __pcache_page_init(page_t *page, struct pcache *pcache, uint64 blkno, size_t offset, size_t size) {
    page->pcache.pcache = pcache;
    page->pcache.blkno = blkno;
    rb_node_init(&page->pcache.node);
    list_entry_init(&page->pcache.lru_entry);
    page->pcache.offset = offset;
    page->pcache.size = size;
}

// alloc and free page for pcache
// Just allocate page and initialize it. No insertion to pcache is done here.
// The caller will hold the spinlock of the new page.
static page_t* __pcache_alloc_page(struct pcache *pcache, uint64 blkno, size_t offset, size_t size) {
    if (__pcache_page_init_validate(pcache, blkno, offset, size) != 0) {
        return NULL;
    }
    __pcache_spin_lock_assert_holding(pcache);
    // Find a free page
    uint64 gfp_flags = pcache->gfp_flags | PAGE_FLAG_PCACHE;
    gfp_flags &= ~(PAGE_FLAG_UPTODATE | PAGE_FLAG_DIRTY);
    page_t *page = __page_alloc(0, gfp_flags);
    if (page == NULL) {
        return NULL;
    }
    __pcache_page_init(page, pcache, blkno, offset, size);
    page_lock_acquire(page);
    return page;
}

// evict a specific page from pcache.
// Page being evicted could be redirect to other blocks or be freed.
// Return 0 on success, -ERRNO on failure
static int __pcache_evict_page(struct pcache *pcache, page_t *page) {
    __pcache_spin_lock_assert_holding(pcache);
    page_lock_assert_holding(page);
    if (page->pcache.pcache != pcache) {
        printf("warning: evicting a page not belonging to this pcache\n");
        return -EINVAL;
    }
    if (page->flags & (PAGE_FLAG_DIRTY | PAGE_FLAG_IO_PROGRESSING)) {
        printf("warning: evicting a dirty page\n");
        return -EBUSY;
    }
    if (page_ref_count(page) > 1) {
        // page is referenced by entities other than pcache
        printf("warning: evicting a referenced page\n");
        return -EBUSY;
    }
    // remove from rb-tree
    struct rb_node *deleted = rb_delete_node_color(&pcache->rb, &page->pcache.node);
    assert(deleted == &page->pcache.node, "__pcache_evict_page: rb_delete_node_color failed");
    // remove from lru list
    __pcache_lru_remove_page(pcache, page);
    pcache->page_count--;
}

// get a evicted page from lru list and reclaim it for new use
// The caller will hold the spinlock of the new page.
// Return 0 on success, -ERRNO on failure
static page_t *__pcache_get_evicted_page(struct pcache *pcache, uint64 blkno, size_t offset, size_t size) {
    if (__pcache_page_init_validate(pcache, blkno, offset, size) != 0) {
        return NULL;
    }
    __pcache_spin_lock_assert_holding(pcache);
    page_t *page = LIST_FIRST_NODE(&pcache->lru, page_t, pcache.lru_entry);
    if (page == NULL) {
        return NULL;
    }
    page_lock_acquire(page);
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
static int __pcache_read_lock(struct pcache *pcache) {
    return rwlock_acquire_read(&pcache->rb_lock);
}

static int __pcache_write_lock(struct pcache *pcache) {
    return rwlock_acquire_write(&pcache->rb_lock);
}

static void __pcache_rw_unlock(struct pcache *pcache) {
    rwlock_release(&pcache->rb_lock);
}

static void __pcache_spin_lock(struct pcache *pcache) {
    acquire(&pcache->spinlock);
}

static void __pcache_spin_unlock(struct pcache *pcache) {
    release(&pcache->spinlock);
}

static void __pcache_spin_lock_assert_holding(struct pcache *pcache) {
    assert(spin_holding(&pcache->spinlock), 
            "__pcache_spin_lock_assert_holding: pcache spinlock not held by current cpu");
}
static void __pcache_spin_lock_assert_unholding(struct pcache *pcache) {
    assert(!spin_holding(&pcache->spinlock), 
            "__pcache_spin_lock_assert_unholding: pcache spinlock held by current cpu");
}

static void __pcache_global_lock(void) {
    acquire(&__pcache_global_spinlock);
}

static void __pcache_global_unlock(void) {
    release(&__pcache_global_spinlock);
}

/******************************************************************************
 * Register and unregister pcache 
 * 
 * Due to the simplicity of the current implementation, locking global list is
 * done here.
 *****************************************************************************/
static void __pcache_register(struct pcache *pcache) {
    if (pcache == NULL) {
        return;
    }
    __pcache_spin_lock(pcache);
    __pcache_global_lock();
    assert(LIST_ENTRY_IS_DETACHED(&pcache->list_entry), "__pcache_register: pcache already registered");
    list_node_push_back(&__global_pcache_list, pcache, list_entry);
    __global_pcache_count++;
    __pcache_global_unlock();
    __pcache_spin_unlock(pcache);
}

// static void __pcache_unregister(struct pcache *pcache) {
//     if (pcache == NULL) {
//         return;
//     }
//     __pcache_spin_lock(pcache);
//     __pcache_global_lock();
//     assert(!LIST_ENTRY_IS_DETACHED(&pcache->list_entry), "__pcache_unregister: pcache not registered");
//     list_node_detach(pcache, list_entry);
//     __global_pcache_count--;
//     __pcache_global_unlock();
//     __pcache_spin_unlock(pcache);
// }

/******************************************************************************
 * Flush coordination helpers
 *****************************************************************************/
static void __pcache_notify_flush_complete(struct pcache *pcache) {
    complete_all(&pcache->flush_completion);
}

static int __pcache_wait_flush_complete(struct pcache *pcache) {
    if (pcache == NULL) {
        return -EINVAL;
    }
    wait_for_completion(&pcache->flush_completion);
    return pcache->flush_error;
}

static bool __pcache_queue_work(struct pcache *pcache) {
    init_work_struct(&pcache->flush_work, __pcache_flush_worker, (uint64)pcache);
    return queue_work(__global_pcache_flush_wq, &pcache->flush_work);
}

static void __pcache_queue_flush(struct pcache *pcache) {
    if (pcache == NULL) {
        return;
    }
    if (__global_pcache_flush_wq == NULL) {
        return;
    }
    if (pcache->flush_requested) {
        return;
    }

    pcache->flush_requested = 1;
    pcache->flush_error = 0;
    completion_reinit(&pcache->flush_completion);

    if (!__pcache_queue_work(pcache)) {
        pcache->flush_requested = 0;
        pcache->flush_error = -EAGAIN;
        __pcache_notify_flush_complete(pcache);
    }
}

static void __pcache_flush_done(struct pcache *pcache) {
    pcache->flush_requested = 0;
    __pcache_notify_flush_complete(pcache);
}

// Wake up the flusher thread to flush all dirty pcaches
static void __pcache_flusher_start(void) {

}

// Wait for flusher thread to complete its current round of flushing
static int __pcache_wait_flusher(void) {

}

// Notify the end of current round of flushing
static void __pcache_flusher_done(void) {

}

/******************************************************************************
 * Call back functions for workqueue
 *****************************************************************************/
static void __pcache_flush_worker(struct work_struct *work) {
    
}

static void __flusher_thread(uint64 a1, uint64 a2) {
    printf("pcache flusher thread started\n");
    
}

static void __create_flusher_thread(void) {
    struct proc *np = NULL;
    int ret = kernel_proc_create("pcache_flusher", &np, __flusher_thread, 0, 0, KERNEL_STACK_ORDER);
    assert(ret > 0 && np != NULL, "Failed to create pcache flusher thread");
    wakeup_proc(np);
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
    struct pcache_node *pcnode = container_of(node, struct pcache_node, tree_entry);
    return pcnode->blkno;
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
    list_entry_init(&__global_pcache_list);
    spinlock_init(&__pcache_global_spinlock, "global_pcache_spinlock");
    int ret = slab_cache_init(  &__pcache_node_slab, 
                                "pcache_node", 
                                sizeof(struct pcache_node), 
                                SLAB_FLAG_EMBEDDED);
    assert(ret == 0, "Failed to initialize pcache node slab");
    __global_pcache_count = 0;
    __global_pcache_flush_wq = workqueue_create("pcache_flush_wq", WORKQUEUE_DEFAULT_MAX_ACTIVE);
    assert(__global_pcache_flush_wq != NULL, "Failed to create global pcache flush workqueue");
    printf("Page cache subsystem initialized\n");
    __create_flusher_thread();
}

int pcache_init(struct pcache *pcache) {
    int ret = __pcache_init_validate(pcache);
    if (ret != 0) {
        return ret;
    }
    // Initialize members
    list_entry_init(&pcache->list_entry);
    list_entry_init(&pcache->lru);
    list_entry_init(&pcache->dirty_list);
    pcache->dirty_count = 0;
    pcache->lru_count = 0;
    pcache->page_count = 0;
    pcache->flags = 0;
    rb_root_init(&pcache->page_map, &__pcache_rb_opts);
    if (pcache->gfp_flags == 0) {
        pcache->gfp_flags = 0;
    }
    spin_init(&pcache->spinlock, "pcache_lock");
    spin_init(&pcache->tree_lock, "pcache_tree_lock");
    completion_init(&pcache->flush_completion);
    complete_all(&pcache->flush_completion);
    pcache->private_data = NULL;
    pcache->flush_error = 0;
    pcache->active = 1;
    pcache->flush_requested = 0;
    if (pcache->max_pages == 0) {
        pcache->max_pages = PCACHE_DEFAULT_MAX_PAGES;
    }
    if (pcache->dirty_rate == 0 || pcache->dirty_rate > 100) {
        pcache->dirty_rate = PCACHE_DEFAULT_DIRTY_RATE;
    }
    __pcache_register(pcache);
    return 0;
}

// Try to get a page from pcache
// The reference count of the page will be increased by 1 if found (2 minimum)
// Block number is in 512-byte block unit
// The block number of the page is aligned to 8 blocks (4KB)
// 
// The page returned could be either dirty or clean and could be in IO progressing state
page_t *pcache_get_page(struct pcache *pcache, uint64 blkno) {
    if (pcache == NULL) {
        return NULL;
    }
    if (__pcache_read_lock(pcache) != 0) {
        // Failed to get read lock
        return NULL;
    }
    blkno = blkno & ~(PGSIZE / 512 - 1); // Align block number to 4KB
    if (!__pcache_page_init_validate(pcache, blkno, 0, 512)) {
        // Invalid block number
        __pcache_rw_unlock(pcache);
        return NULL;
    }
    int blocks = pcache->blk_count - blkno;
    page_t *page = NULL;
    if (!__pcache_is_active(pcache)) {
        // pcache is not active
        page = NULL;
    }
    
    // Try to find the page with read lock
    page = __pcache_find_page(pcache, blkno);
    if (page) {
        goto ret;
    }
    __pcache_rw_unlock(pcache);

    // Page not found, try to get write lock and search again
    __pcache_write_lock(pcache);
    // Because we have released the read lock, the page could be added by other threads
    // So we need to search again
    if (!__pcache_is_active(pcache)) {
        page = NULL;
        goto ret;
    }
    page = __pcache_find_page(pcache, blkno);
    if (page) {
        goto ret;
    }

    // Still not found
    // Check if we can allocate a new page
    if (pcache->page_count < pcache->max_pages) {
        // Allocate a new page
        page = __pcache_alloc_page(pcache, blkno, 0, blocks * 512);
    } else {
        // Need to evict a page from lru list
        page = __pcache_get_evicted_page(pcache, blkno, 0, blocks * 512);
    }

    // @TODO: Try flush dirty pages if no page can be allocated or evicted
ret:
    if (page) {
        page_ref_inc(page);
    }
    __pcache_rw_unlock(pcache);
    return page;
}

void pcache_put_page(struct pcache *pcache, page_t *page) {
    if (pcache == NULL || page == NULL) {
        return;
    }
    __pcache_spin_lock(pcache);
    page_lock_acquire(page);
    if (__pcache_page_validate(pcache, page)) {
        // Decrease page ref count
        int ref_count = page_ref_dec(page);
        // Refcount has already been checked in __pcache_page_validate, so it must be >= 1
        assert(ref_count >= 1, "pcache_put_page: page ref count underflow");
        if (ref_count == 1) {
            // Someone other than pcache must be holding the reference of a page when doing
            // IO operation on it. So the ref count cannot reach 1 when IO is in progress
            assert(!(page->flags & PAGE_FLAG_IO_PROGRESSING), 
                    "pcache_put_page: page in IO progressing cannot be put back to lru or dirty list");
            // put back to lru list or dirty list when ref count reaches 1
            if (!(page->flags & PAGE_FLAG_DIRTY)) {
                __pcache_lru_list_add(pcache, page);
            }
        }
    } else {
        printf("warning: pcache_put_page: Invalid page\n");
    }
    page_lock_release(page);
    __pcache_spin_unlock(pcache);
}

int pcache_mark_page_dirty(struct pcache *pcache, page_t *page) {
    if (pcache == NULL || page == NULL) {
        return -EINVAL;
    }
    __pcache_spin_lock(pcache);
    page_lock_acquire(page);
    if (__pcache_page_validate(pcache, page)) {
        if (page->flags & PAGE_FLAG_IO_PROGRESSING) {
            // Page in IO progressing cannot be marked dirty
            page_lock_release(page);
            __pcache_spin_unlock(pcache);
            return -EBUSY;
        }
        
        page->flags |= PAGE_FLAG_DIRTY;

    } else {
        printf("warning: pcache_mark_page_dirty: Invalid page\n");
    }
    page_lock_release(page);
    __pcache_spin_unlock(pcache);
    return 0;
}

int pcache_invalidate_page(struct pcache *pcache, page_t *page) {
    if (pcache == NULL || page == NULL) {
        return -EINVAL;
    }
    __pcache_spin_lock(pcache);
    page_lock_acquire(page);
    
    if (!__pcache_page_validate(pcache, page)) {
        printf("warning: pcache_invalidate_page: Invalid page\n");
        page_lock_release(page);
        __pcache_spin_unlock(pcache);
        return -EINVAL;
    }

    if (page->flags & PAGE_FLAG_IO_PROGRESSING) {
        // Page in IO progressing cannot be invalidated
        page_lock_release(page);
        __pcache_spin_unlock(pcache);
        return -EBUSY;
    }
    if (page_ref_count(page) > 2) {
        // Page referenced by entities other than pcache and caller cannot be invalidated
        // @TODO: 
        page_lock_release(page);
        __pcache_spin_unlock(pcache);
        return -EBUSY;
    }
    page->flags &= ~(PAGE_FLAG_DIRTY | PAGE_FLAG_UPTODATE);
    page_lock_release(page);
    __pcache_spin_unlock(pcache);
    return 0;
}

// Flush all dirty pages in pcache and wait for completion
// User needs to check the status of the pcache after the call
// Return 0 on success, -ERRNO on failure
int pcache_flush(struct pcache *pcache) {
    if (pcache == NULL) {
        return -EINVAL;
    }
    __pcache_read_lock(pcache);
    __pcache_spin_lock(pcache);
    if (pcache->flush_requested) {
        __pcache_spin_unlock(pcache);
        __pcache_rw_unlock(pcache);
        return __pcache_wait_flush_complete(pcache);
    }
    if (pcache->dirty_count == 0) {
        // No dirty pages, nothing to do
        __pcache_spin_unlock(pcache);
        __pcache_rw_unlock(pcache);
        return 0;
    }
    if (!pcache->active) {
        // Although flusher accepts inactive pcache, it happens when pcache is being deactivated.
        // So when pcache is not flushing and inactive, just return.
        __pcache_spin_unlock(pcache);
        __pcache_rw_unlock(pcache);
        return 0;
    }
    // Flush dirty pages
    __pcache_queue_flush(pcache);
    __pcache_spin_unlock(pcache);
    __pcache_rw_unlock(pcache);
    return __pcache_wait_flush_complete(pcache);
}

int pcache_sync(void) {
    __pcache_flusher_start();
    return __pcache_wait_flusher();
}

int pcache_read_page(struct pcache *pcache, page_t *page) {
    if (pcache == NULL || page == NULL) {
        return -EINVAL;
    }
    if (__pcache_read_lock(pcache) != 0) {
        return -EAGAIN;
    }
    __pcache_spin_lock(pcache);
    page_lock_acquire(page);
    if (__pcache_page_validate(pcache, page)) {
        // Page is valid
        if (page->flags & PAGE_FLAG_UPTODATE) {
            // Page is already up-to-date
            page_lock_release(page);
            __pcache_spin_unlock(pcache);
            __pcache_rw_unlock(pcache);
            return 0;
        }
        if (page_ref_count(page) < 2) {
            // Page is not referenced by anyone other than pcache
            // This should not happen as the caller should hold a reference to the page
            printf("warning: pcache_read_page: page ref count < 2\n");
            page_lock_release(page);
            __pcache_spin_unlock(pcache);
            __pcache_rw_unlock(pcache);
            return -EINVAL;
        }
        if (page->flags & PAGE_FLAG_IO_PROGRESSING) {
            // IO is in progress, wait for it to complete
            __pcache_spin_unlock(pcache);
            __pcache_rw_unlock(pcache);
            return __pcache_page_io_wait(page);
        }
        // Start IO
        page->
        __pcache_page_io_begin(page);
        __pcache_spin_unlock(pcache);
        __pcache_rw_unlock(pcache);
        int ret = __pcache_read_page(pcache, page);
        page_lock_acquire(page);
        if (ret == 0) {
            page->flags |= PAGE_FLAG_UPTODATE;
        }
        __pcache_page_io_end(page);
        page_lock_release(page);
        return ret;
    } else {
        printf("warning: pcache_read_page: Invalid page\n");
        page_lock_release(page);
        __pcache_spin_unlock(pcache);
        __pcache_rw_unlock(pcache);
        return -EINVAL;
    }
}
