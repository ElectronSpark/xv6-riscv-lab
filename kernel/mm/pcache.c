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
#include "bio.h"

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

static struct pcache_node *__pcache_find_key_page(
    struct pcache *pcache,
    uint64 blkno
);
static page_t *__pcache_get_page(
    struct pcache *pcache,
    uint64 blkno,
    size_t size,
    page_t *default_page
);
static void __pcache_remove_node(struct pcache *pcache, page_t *page);

static void __pcache_push_lru(struct pcache *pcache, page_t *page);
static page_t *__pcache_pop_lru(struct pcache *pcache);
static void __pcache_remove_lru(struct pcache *pcache, page_t *page);
static void __pcache_push_dirty(struct pcache *pcache, page_t *page);
static page_t *__pcache_evict_lru(struct pcache *pcache);

static void __pcache_queue_flush(struct pcache *pcache);
static void __pcache_flush_worker(struct work_struct *work);
static int __pcache_wait_flush_complete(struct pcache *pcache);
static void __pcache_notify_flush_complete(struct pcache *pcache);
static void __pcache_flush_done(struct pcache *pcache);

static void __pcache_flusher_start(void);
static int __pcache_wait_flusher(void);
static void __pcache_flusher_done(void);

static int __pcache_node_io_begin(struct pcache *pcache, page_t *page);
static int __pcache_node_io_end(struct pcache *pcache, page_t *page);
static int __pcache_node_io_wait(struct pcache *pcache, page_t *page);

static int __pcache_write_begin(struct pcache *pcache, page_t *page);
static int __pcache_write_end(struct pcache *pcache, page_t *page);
static int __pcache_write_page(struct pcache *pcache, page_t *page);
static int __pcache_read_page(struct pcache *pcache, page_t *page);
static void __pcache_mark_dirty(struct pcache *pcache, page_t *page);

static void __pcache_tree_lock(struct pcache *pcache);
static void __pcache_tree_unlock(struct pcache *pcache);
static void __pcache_spin_lock(struct pcache *pcache);
static void __pcache_spin_unlock(struct pcache *pcache);
static void __pcache_spin_assert_holding(struct pcache *pcache);

static void __pcache_node_init(struct pcache_node *node);
static page_t *__pcache_page_alloc(void);
static page_t *__pcache_page_dup(page_t *page);
static void __pcache_page_put(page_t *page);
static void __pcache_node_attach_page(
    struct pcache *pcache, 
    page_t *page
);
static void __pcache_node_detach_page(
    struct pcache *pcache,
    page_t *page
);

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

static int __pcache_write_begin(struct pcache *pcache, page_t *page) {
    if (pcache->ops && pcache->ops->write_begin) {
        return pcache->ops->write_begin(pcache, page);
    }
    return 0;
}

static int __pcache_write_end(struct pcache *pcache, page_t *page) {
    if (pcache->ops && pcache->ops->write_end) {
        return pcache->ops->write_end(pcache, page);
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

/******************************************************************************
 * Register and unregister pcache 
 * 
 * Due to the simplicity of the current implementation, locking global list is
 * done here.
 *****************************************************************************/
static void __pcache_register(struct pcache *pcache) {
    // if (pcache == NULL) {
    //     return;
    // }
    // __pcache_spin_lock(pcache);
    // __pcache_global_lock();
    // assert(LIST_ENTRY_IS_DETACHED(&pcache->list_entry), "__pcache_register: pcache already registered");
    // list_node_push_back(&__global_pcache_list, pcache, list_entry);
    // __global_pcache_count++;
    // __pcache_global_unlock();
    // __pcache_spin_unlock(pcache);
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
 * Global Locking Helpers
 *****************************************************************************/

static void __pcache_global_lock(void) {
    spin_acquire(&__pcache_global_spinlock);
}

static void __pcache_global_unlock(void) {
    spin_release(&__pcache_global_spinlock);
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
 * pcache tree helpers
 *****************************************************************************/
static struct pcache_node *__pcache_find_key_node(
    struct pcache *pcache,
    uint64 blkno
) {
    struct rb_node *node = rb_find_key(&pcache->page_map, blkno);
    if (node == NULL) {
        return NULL;
    }
    return container_of(node, struct pcache_node, tree_entry);
}

static struct pcache_node *__pcache_insert_node(
    struct pcache *pcache,
    struct pcache_node *pcnode
) {
    struct rb_node *node = rb_insert_color(&pcache->page_map, &pcnode->tree_entry);
    if (node == NULL) {
        return NULL;
    }
    return container_of(node, struct pcache_node, tree_entry);
}

static void __pcache_tree_lock(struct pcache *pcache) {
    spin_acquire(&pcache->tree_lock);
}

static void __pcache_tree_unlock(struct pcache *pcache) {
    spin_release(&pcache->tree_lock);
}

static void __pcache_spin_lock(struct pcache *pcache) {
    spin_acquire(&pcache->spinlock);
}

static void __pcache_spin_unlock(struct pcache *pcache) {
    spin_release(&pcache->spinlock);
}

static void __pcache_spin_assert_holding(struct pcache *pcache) {
    assert(spin_holding(&pcache->spinlock), "__pcache_spin_assert_holding: pcache spinlock not held");
}

// Get or insert a pcache_node for the given block number
static page_t *__pcache_get_page(
    struct pcache *pcache,
    uint64 blkno,
    size_t size,
    page_t *default_page
) {
    if (blkno >= pcache->blk_count) {
        // Block number out of range
        return NULL;
    }
    if (blkno + (1 << (PAGE_SHIFT - BLK_SIZE_SHIFT)) >= pcache->blk_count) {
        // Block number out of range
        return NULL;
    }

    if (default_page) {
        // If want to insert into existing page, ensure the page lock is held
        page_lock_assert_holding(default_page);
        if (!PAGE_IS_TYPE(default_page, PAGE_TYPE_PCACHE) ||
            default_page->pcache.pcache ||
            !default_page->pcache.pcache_node ||
            default_page->pcache.pcache_node->page != default_page) {
            // The given default page is not from the given pcache
            printf("__pcache_get_page: default_page is not from the given pcache\n");
            return NULL;
        }
    }

    struct pcache_node *found_node = NULL;
    __pcache_tree_lock(pcache);
    if (default_page) {
        found_node = __pcache_insert_node(&pcache->page_map, default_page->pcache.pcache_node);
        if (found_node != default_page->pcache.pcache_node) {
            // While inserting, another thread has already inserted a node with the same key
            __pcache_tree_unlock(pcache);
            return found_node->page;
        }
    } else {
        found_node = __pcache_find_key_node(&pcache->page_map, blkno);
        if (!found_node) {
            __pcache_tree_unlock(pcache);
            return NULL;
        }
    }
    // If not found, unlock and create new node if needed
    __pcache_tree_unlock(pcache);
    return found_node->page;
}

// Remove a pcache_node from rb tree
static void __pcache_remove_node(struct pcache *pcache, page_t *page) {
    page_lock_assert_holding(page);

    __pcache_tree_lock(pcache);
    struct pcache_node *pcnode = page->pcache.pcache_node;
    assert(pcnode != NULL, "__pcache_remove_node: page has no pcache_node");
    assert(pcnode->page == page, "__pcache_remove_node: pcache_node does not point to the given page");
    assert(LIST_NODE_IS_DETACHED(pcnode, lru_entry), "__pcache_remove_node: pcache node must be detached from lru or dirty list before removal");
    // Remove from rb-tree
    struct rb_node *removed = rb_delete_node_color(&pcache->page_map, &pcnode->tree_entry);
    assert(removed == &pcnode->tree_entry, "__pcache_remove_node: removed rb-node does not match the pcache node");

    __pcache_tree_unlock(pcache);
}

/******************************************************************************
 * pcache_node helpers
 *****************************************************************************/
static void __pcache_node_init(struct pcache_node *node) {
    memset(node, 0, sizeof(struct pcache_node));
    rb_node_init(&node->tree_entry);
    list_entry_init(&node->lru_entry);
    completion_init(&node->io_completion);
    node->blkno = -1;
    node->page_count = 0;
}

static page_t *__pcache_page_alloc(void) {
    struct pcache_node *pcnode = slab_alloc(&__pcache_node_slab);
    if (pcnode == NULL) {
        return NULL;
    }
    page_t *page = page_alloc(0, PAGE_TYPE_PCACHE);
    if (page == NULL) {
        slab_free(pcnode);
        return NULL;
    }
    __pcache_node_init(pcnode);
    pcnode->page = page;
    pcnode->page_count = 1;
    pcnode->size = PGSIZE;
    pcnode->data = page_to_virt(page);
    page->pcache.pcache_node = pcnode;
    return pcnode;
}

// Attach a page to a pcache through a pcache_node
// Will not touch pcache tree
// Both pcache and page must be locked before calling this function
static void __pcache_node_attach_page(
    struct pcache *pcache, 
    page_t *page
) {
    page_lock_assert_holding(page);
    __pcache_spin_assert_holding(pcache);
    struct pcache_node *pcnode = page->pcache.pcache_node;
    assert(pcnode != NULL, "__pcache_node_attach_page: page has no pcache_node");
    assert(pcnode->page == page, "__pcache_node_attach_page: pcache_node does not point to the given page");
    assert(pcnode->pcache == NULL, "__pcache_node_attach_page: pcache_node's pcache must be NULL before attaching");
    pcnode->page_count = 1; // @TODO: currently only support one page per pcache_node
    pcnode->pcache = pcache;
    page->pcache.pcache = pcache;
    page->pcache.pcache_node = pcnode;
    pcache->page_count += pcnode->page_count;
}

// Detach a page to a pcache through a pcache_node
// Will not touch pcache tree
// Both pcache and page must be locked before calling this function
static void __pcache_node_detach_page(
    struct pcache *pcache,
    page_t *page
) {
    page_lock_assert_holding(page);
    __pcache_spin_assert_holding(pcache);
    struct pcache_node *pcnode = page->pcache.pcache_node;
    assert(pcnode != NULL, "__pcache_node_detach_page: page has no pcache_node");
    assert(pcnode->page == page, "__pcache_node_detach_page: pcache_node does not point to the given page");
    assert(pcnode->pcache == pcache, "__pcache_node_detach_page: pcache_node's pcache does not match the given pcache");
    assert(LIST_NODE_IS_DETACHED(pcnode, lru_entry), "__pcache_node_detach_page: pcache_node must be detached from lru or dirty list before detaching");
    page->pcache.pcache = NULL;
    pcnode->pcache = NULL;
    pcache->page_count -= pcnode->page_count;
    assert(pcache->page_count >= 0, "__pcache_node_detach_page: pcache page count negative");
}

/******************************************************************************
 * Pcache_node IO synchronization helpers
 *****************************************************************************/

static int __pcache_node_io_begin(struct pcache *pcache, page_t *page) {
    __pcache_tree_lock(pcache);
    struct pcache_node *node = page->pcache.pcache_node;
    if (node->io_in_progress) {
        return -EALREADY;
    }
    node->io_in_progress = 1;
    completion_reinit(&node->io_completion);
    __pcache_tree_unlock(pcache);
    return 0;
}

static int __pcache_node_io_end(struct pcache *pcache, page_t *page) {
    __pcache_tree_lock(pcache);
    struct pcache_node *node = page->pcache.pcache_node;
    if (!node->io_in_progress) {
        __pcache_tree_unlock(pcache);
        return -EALREADY;
    }
    node->io_in_progress = 0;
    __pcache_tree_unlock(pcache);
    complete_all(&node->io_completion);
    return 0;
}

static int __pcache_node_io_wait(struct pcache *pcache, page_t *page) {
    __pcache_tree_lock(pcache);
    struct pcache_node *node = page->pcache.pcache_node;
    if (!node->io_in_progress) {
        __pcache_tree_unlock(pcache);
        return 0;
    }
    node->io_in_progress = 0;
    wait_for_completion(&node->io_completion);
    __pcache_tree_unlock(pcache);
    return 0;
}

/******************************************************************************
 * LRU list helpers
 *****************************************************************************/

static void __pcache_push_lru(struct pcache *pcache, page_t *page) {
    __pcache_spin_assert_holding(pcache);
    page_lock_assert_holding(page);
    struct pcache_node *pcnode = page->pcache.pcache_node;
    assert(pcnode != NULL, "__pcache_push_lru: page has no pcache_node");
    assert(!pcnode->dirty, "__pcache_push_lru: pcache_node is dirty");
    assert(pcnode->pcache == pcache, "__pcache_push_lru: pcache_node's pcache does not match the given pcache");
    assert(pcnode->page == page, "__pcache_push_lru: pcache_node does not point to the given page");
    assert(LIST_NODE_IS_DETACHED(pcnode, lru_entry), "__pcache_push_lru: pcache node already in lru or dirty list");
    list_node_push_back(&pcache->lru, pcnode, lru_entry);
    pcache->lru_count++;
}

// Will not acquire the lock of the returned page
static page_t *__pcache_pop_lru(struct pcache *pcache) {
    __pcache_spin_assert_holding(pcache);
    if (LIST_IS_EMPTY(&pcache->lru)) {
        return NULL;
    }
    struct pcache_node *pcnode = list_node_pop(&pcache->lru, struct pcache_node, lru_entry);
    if (pcnode == NULL) {
        return NULL;
    }
    assert(pcnode->page != NULL, "__pcache_pop_lru: pcache_node has no page");
    assert(pcnode->pcache == pcache, "__pcache_pop_lru: pcache_node's pcache does not match the given pcache");
    page_t *page = pcnode->page;
    pcache->lru_count--;
    assert(pcache->lru_count >= 0, "__pcache_pop_lru: pcache lru count underflow");
    return page;
}

static void __pcache_remove_lru(struct pcache *pcache, page_t *page) {
    __pcache_spin_assert_holding(pcache);
    page_lock_assert_holding(page);
    struct pcache_node *pcnode = page->pcache.pcache_node;
    assert(pcnode != NULL, "__pcache_remove_lru: page has no pcache_node");
    assert(pcnode->page == page, "__pcache_remove_lru: pcache_node does not point to the given page");
    assert(pcnode->pcache == pcache, "__pcache_remove_lru: pcache_node's pcache does not match the given pcache");
    assert(!LIST_NODE_IS_DETACHED(pcnode, lru_entry), "__pcache_remove_lru: pcache node not in lru list");
    list_node_detach(pcnode, lru_entry);
    if (pcnode->dirty) {
        pcache->dirty_count--;
        assert(pcache->dirty_count >= 0, "__pcache_remove_lru: pcache dirty count underflow");
    } else {
        pcache->lru_count--;
        assert(pcache->lru_count >= 0, "__pcache_remove_lru: pcache lru count underflow");
    }
}

static void __pcache_push_dirty(struct pcache *pcache, page_t *page) {
    __pcache_spin_assert_holding(pcache);
    page_lock_assert_holding(page);
    struct pcache_node *pcnode = page->pcache.pcache_node;
    assert(pcnode != NULL, "__pcache_push_dirty: page has no pcache_node");
    assert(pcnode->dirty, "__pcache_push_dirty: pcache_node is not dirty");
    assert(pcnode->pcache == pcache, "__pcache_push_dirty: pcache_node's pcache does not match the given pcache");
    assert(pcnode->page == page, "__pcache_push_dirty: pcache_node does not point to the given page");
    assert(LIST_NODE_IS_DETACHED(pcnode, lru_entry), "__pcache_push_dirty: pcache node already in lru or dirty list");
    list_node_push_back(&pcache->dirty_list, pcnode, lru_entry);
    pcache->dirty_count++;
}

static page_t *__pcache_evict_lru(struct pcache *pcache) {
    page_t *page = __pcache_pop_lru(pcache);
    page_lock_acquire(page);
    __pcache_remove_node(pcache, page);
    __pcache_node_detach_page(pcache, page);
    page_lock_release(page);
    return page;
}

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

}

void pcache_put_page(struct pcache *pcache, page_t *page) {
}

int pcache_mark_page_dirty(struct pcache *pcache, page_t *page) {
}

int pcache_invalidate_page(struct pcache *pcache, page_t *page) {
}

// Flush all dirty pages in pcache and wait for completion
// User needs to check the status of the pcache after the call
// Return 0 on success, -ERRNO on failure
int pcache_flush(struct pcache *pcache) {
}

int pcache_sync(void) {
}

int pcache_read_page(struct pcache *pcache, page_t *page) {
}
