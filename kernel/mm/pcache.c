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
#include "timer.h"

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
static completion_t __global_flusher_completion = {0};
static struct proc *__flusher_thread_pcb = NULL;

#define PCACHE_BLKS_PER_PAGE (PGSIZE >> BLK_SIZE_SHIFT)
#define PCACHE_BLK_MASK ((uint64)PCACHE_BLKS_PER_PAGE - 1)
#define PCACHE_ALIGN_BLKNO(blkno) ((blkno) & ~PCACHE_BLK_MASK)

/******************************************************************************
 * Predefine functions
 *****************************************************************************/
static void __pcache_register(struct pcache *pcache);
// static void __pcache_unregister(struct pcache *pcache);

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
static page_t *__pcache_pop_dirty(struct pcache *pcache, uint64 latest_flush_jiffs);
static page_t *__pcache_evict_lru(struct pcache *pcache);

static void __pcache_flush_worker(struct work_struct *work);
static int __pcache_wait_flush_complete(struct pcache *pcache);
static void __pcache_notify_flush_complete(struct pcache *pcache);
static void __pcache_flush_done(struct pcache *pcache);

static void __pcache_flusher_start(void);
static int __pcache_wait_flusher(void);
static void __pcache_flusher_done(void);
static bool __pcache_flusher_in_progress(void);
static bool __pcache_schedule_flushes_locked(uint64 round_start, bool force_round);
static void __pcache_wait_for_pending_flushes(void);

#ifdef HOST_TEST
void pcache_test_run_flusher_round(uint64 round_start, bool force_round);
void pcache_test_unregister(struct pcache *pcache);
void pcache_test_set_retry_hook(void (*hook)(struct pcache *, uint64));
#endif

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
static void __pcache_page_put(page_t *page);
static void __pcache_node_attach_page(
    struct pcache *pcache, 
    page_t *page
);
static void __pcache_node_detach_page(
    struct pcache *pcache,
    page_t *page
);

static void __pcache_global_lock_assert_holding(void);
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
        pcache->lru.next != NULL || pcache->lru.prev != NULL ||
        pcache->dirty_list.next != NULL || pcache->dirty_list.prev != NULL ||
        pcache->list_entry.next != NULL || pcache->list_entry.prev != NULL) {
        return -EINVAL;
    }
    return 0;
}

static inline int __pcache_page_valid(struct pcache *pcache, page_t *page) {
    if (pcache == NULL || page == NULL) {
        return 0;
    }
    if (!PAGE_IS_TYPE(page, PAGE_TYPE_PCACHE)) {
        return 0;
    }
    return page->pcache.pcache == pcache && page->pcache.pcache_node != NULL;
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
    __pcache_global_lock();
    __pcache_spin_lock(pcache);
    if (LIST_ENTRY_IS_DETACHED(&pcache->list_entry)) {
        list_node_push_back(&__global_pcache_list, pcache, list_entry);
        __global_pcache_count++;
    } else {
        printf("warning: __pcache_register: pcache already registered");
    }
    
    __pcache_spin_unlock(pcache);
    __pcache_global_unlock();
}

#ifdef HOST_TEST
void pcache_test_unregister(struct pcache *pcache) {
    if (pcache == NULL) {
        return;
    }

    __pcache_global_lock();
    __pcache_spin_lock(pcache);
    if (!LIST_ENTRY_IS_DETACHED(&pcache->list_entry)) {
        list_node_detach(pcache, list_entry);
        if (__global_pcache_count > 0) {
            __global_pcache_count--;
        }
    }
    __pcache_spin_unlock(pcache);
    __pcache_global_unlock();
}
#endif

#ifdef HOST_TEST
static void (*__pcache_test_retry_hook)(struct pcache *, uint64) = NULL;

void pcache_test_set_retry_hook(void (*hook)(struct pcache *, uint64)) {
    __pcache_test_retry_hook = hook;
}
#endif

/******************************************************************************
 * Flush coordination helpers
 *****************************************************************************/
static void __pcache_notify_flush_complete(struct pcache *pcache) {
    if (pcache == NULL) {
        return;
    }
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
    bool queued;

    if (pcache == NULL || __global_pcache_flush_wq == NULL) {
        return false;
    }

    __pcache_spin_assert_holding(pcache);

    if (pcache->flush_requested) {
        return true;
    }

    init_work_struct(&pcache->flush_work, __pcache_flush_worker, (uint64)pcache);
    queued = queue_work(__global_pcache_flush_wq, &pcache->flush_work);
    if (queued) {
        pcache->flush_requested = 1;
        pcache->last_request = get_jiffs();
        pcache->flush_error = 0;
        completion_reinit(&pcache->flush_completion);
    }

    return queued;
}

static void __pcache_flush_done(struct pcache *pcache) {
    __pcache_spin_assert_holding(pcache);
    pcache->flush_requested = 0;
    pcache->last_flushed = get_jiffs();
    __pcache_notify_flush_complete(pcache);
}

// Wake up the flusher thread to flush all dirty pcaches
static void __pcache_flusher_start(void) {
    __pcache_global_lock();
    if (__pcache_flusher_in_progress()) {
        __pcache_global_unlock();
        return;
    }
    completion_reinit(&__global_flusher_completion);
    wakeup_proc(__flusher_thread_pcb);
    __pcache_global_unlock();
}

// Wait for flusher thread to complete its current round of flushing
static int __pcache_wait_flusher(void) {
    wait_for_completion(&__global_flusher_completion);
    return 0;
}

// Notify the end of current round of flushing
static void __pcache_flusher_done(void) {
    __pcache_global_lock_assert_holding();
    complete_all(&__global_flusher_completion);
}

static bool __pcache_flusher_in_progress(void) {
    __pcache_global_lock_assert_holding();
    return !completion_done(&__global_flusher_completion);
}

/******************************************************************************
 * Call back functions for workqueue
 *****************************************************************************/
static void __pcache_flush_worker(struct work_struct *work) {
    struct pcache *pcache = (struct pcache *)work->data;
    int ret = 0;
    uint64 start_jiffs = get_jiffs();

    // @TODO: need to come up with a more robust way to handle all pcache flush errors

    if (pcache == NULL) {
        printf("__pcache_flush_worker: pcache is NULL\n");
        return;
    }

    __pcache_spin_lock(pcache);
    while (1) {
        page_t *page = __pcache_pop_dirty(pcache, start_jiffs);
        if (page == NULL) {
            break;  // No more dirty pages to flush
        }
        ret = page_ref_inc_unlocked(page);
        assert(ret > 1, "__pcache_flush_worker: failed to increment page ref count");
        ret = __pcache_node_io_begin(pcache, page);
        assert(ret == 0, "__pcache_flush_worker: failed to begin IO on page");
        page_lock_release(page);
        __pcache_spin_unlock(pcache);

        // Real write operation outside the pcache lock
        ret = __pcache_write_begin(pcache, page);
        if (ret != 0) {
            __pcache_spin_lock(pcache);
            pcache->flush_error = ret;
            goto err_continue;
        }
        ret = __pcache_write_page(pcache, page);
        if (ret != 0) {
            ret = __pcache_write_end(pcache, page);
            __pcache_spin_lock(pcache);
            pcache->flush_error = ret;
            goto err_continue;
        }
        ret = __pcache_write_end(pcache, page);

        __pcache_spin_lock(pcache);
        page_lock_acquire(page);
        if (ret != 0) {
            pcache->flush_error = ret;
        }
        struct pcache_node *pcnode = page->pcache.pcache_node;
        assert(pcnode != NULL, "__pcache_flush_worker: page missing pcache node");
        pcnode->dirty = 0;
        pcnode->uptodate = 1;
        ret = __pcache_node_io_end(pcache, page);
        assert(ret == 0, "__pcache_flush_worker: failed to end IO on page");
        ret = page_ref_dec_unlocked(page);
        assert(ret >= 1, "__pcache_flush_worker: page refcount underflow after flush");
        if (ret == 1 && LIST_NODE_IS_DETACHED(pcnode, lru_entry)) {
            __pcache_push_lru(pcache, page);
        }
        page_lock_release(page);
        continue;

err_continue:
        __pcache_spin_assert_holding(pcache);
        page_lock_acquire(page);
        ret = __pcache_node_io_end(pcache, page);
        assert(ret == 0, "__pcache_flush_worker: failed to end IO on page");
        __pcache_push_dirty(pcache, page);
        ret = page_ref_dec_unlocked(page);
        assert(ret > 0, "__pcache_flush_worker: failed to decrement page ref count");
        page_lock_release(page);
    }
    __pcache_flush_done(pcache);
    __pcache_spin_unlock(pcache);
}

static bool __pcache_schedule_flushes_locked(uint64 round_start, bool force_round) {
    bool pending_flush = false;
    struct pcache *pcache = NULL;
    struct pcache *tmp = NULL;

    list_foreach_node_safe(&__global_pcache_list, pcache, tmp, list_entry) {
        __pcache_spin_lock(pcache);

        if (!__pcache_is_active(pcache)) {
            __pcache_spin_unlock(pcache);
            continue;
        }

        bool should_flush = false;
        if (pcache->dirty_count > 0) {
            if (force_round) {
                should_flush = true;
            } else {
                uint64 dirty_threshold = 0;
                if (pcache->page_count > 0 && pcache->dirty_rate > 0) {
                    dirty_threshold = (pcache->page_count * pcache->dirty_rate) / 100;
                }
                if (dirty_threshold == 0 && pcache->dirty_count > 0) {
                    dirty_threshold = 1;
                }

                if (dirty_threshold > 0 && pcache->dirty_count >= dirty_threshold) {
                    should_flush = true;
                } else {
                    if (round_start >= pcache->last_flushed &&
                        round_start - pcache->last_flushed >= PCACHE_FLUSH_INTERVAL_JIFFS) {
                        should_flush = true;
                    } else if (round_start >= pcache->last_request &&
                               round_start - pcache->last_request >= PCACHE_FLUSH_INTERVAL_JIFFS) {
                        should_flush = true;
                    }
                }
            }
        }

        if (should_flush) {
            if (!__pcache_queue_work(pcache)) {
                if (pcache->flush_requested == 0) {
                    printf("warning: flusher failed to queue work for pcache %p\n", pcache);
                }
            }
        }

        if (pcache->flush_requested) {
            pending_flush = true;
        }

        __pcache_spin_unlock(pcache);
    }

    return pending_flush;
}

static void __pcache_wait_for_pending_flushes(void) {
    for (;;) {
        bool still_pending = false;

        __pcache_global_lock();
        struct pcache *pcache = NULL;
        struct pcache *tmp = NULL;
        list_foreach_node_safe(&__global_pcache_list, pcache, tmp, list_entry) {
            __pcache_spin_lock(pcache);
            if (pcache->flush_requested) {
                still_pending = true;
                __pcache_spin_unlock(pcache);
                break;
            }
            __pcache_spin_unlock(pcache);
        }
        __pcache_global_unlock();

        if (!still_pending) {
            break;
        }

        sleep_ms(10);
    }
}

#ifdef HOST_TEST
void pcache_test_run_flusher_round(uint64 round_start, bool force_round) {
    bool pending_flush;

    __pcache_global_lock();
    pending_flush = __pcache_schedule_flushes_locked(round_start, force_round);
    __pcache_global_unlock();

    if (pending_flush) {
        __pcache_wait_for_pending_flushes();
    }

    __pcache_global_lock();
    __pcache_flusher_done();
    __pcache_global_unlock();
}
#endif

static void __flusher_thread(uint64 a1, uint64 a2) {
    printf("pcache flusher thread started\n");

    for (;;) {
        uint64 round_start = get_jiffs();

        __pcache_global_lock();
        bool force_round = !completion_done(&__global_flusher_completion);
        bool pending_flush = __pcache_schedule_flushes_locked(round_start, force_round);
        __pcache_global_unlock();

        if (pending_flush) {
            __pcache_wait_for_pending_flushes();
        }

        __pcache_global_lock();
        __pcache_flusher_done();
        __pcache_global_unlock();

        uint64 sleep_ticks = PCACHE_FLUSH_INTERVAL_JIFFS;
        uint64 sleep_ms_val = (sleep_ticks * 1000) / HZ;
        if (sleep_ms_val == 0) {
            sleep_ms_val = 1;
        }
        sleep_ms(sleep_ms_val);
    }
}

static void __create_flusher_thread(void) {
    struct proc *np = NULL;
    int ret = kernel_proc_create("pcache_flusher", &np, __flusher_thread, 0, 0, KERNEL_STACK_ORDER);
    assert(ret > 0 && np != NULL, "Failed to create pcache flusher thread");
    __flusher_thread_pcb = np;
    wakeup_proc(np);
}

/******************************************************************************
 * Global Locking Helpers
 *****************************************************************************/

static void __pcache_global_lock_assert_holding(void) {
    assert(spin_holding(&__pcache_global_spinlock), "__pcache_global_lock_assert_holding: global pcache spinlock not held");
}

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
    if (blkno + (1 << (PAGE_SHIFT - BLK_SIZE_SHIFT)) > pcache->blk_count) {
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
        found_node = __pcache_insert_node(pcache, default_page->pcache.pcache_node);
        if (found_node != default_page->pcache.pcache_node) {
            // While inserting, another thread has already inserted a node with the same key
            __pcache_tree_unlock(pcache);
            return found_node->page;
        }
    } else {
        found_node = __pcache_find_key_node(pcache, blkno);
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
    pcnode->data = (void *)__page_to_pa(page);
    page->pcache.pcache_node = pcnode;
    return page;
}

static void __pcache_page_put(page_t *page) {
    if (page == NULL) {
        return;
    }
    __page_ref_dec(page);
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
        __pcache_tree_unlock(pcache);
        return -EALREADY;
    }
    node->io_in_progress = 1;
    node->last_request = get_jiffs();
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
    node->last_flushed = get_jiffs();
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
    __pcache_tree_unlock(pcache);
    wait_for_completion(&node->io_completion);
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
    assert(page->ref_count == 1, "__pcache_push_lru: page ref_count is not 1");
    assert(LIST_NODE_IS_DETACHED(pcnode, lru_entry), "__pcache_push_lru: pcache node already in lru or dirty list");
    list_node_push_back(&pcache->lru, pcnode, lru_entry);
    pcache->lru_count++;
}

// Will return a page with its lock held
static page_t *__pcache_pop_lru(struct pcache *pcache) {
    __pcache_spin_assert_holding(pcache);
    if (LIST_IS_EMPTY(&pcache->lru)) {
        return NULL;
    }
retry:
    struct pcache_node *pcnode = LIST_LAST_NODE(&pcache->lru, struct pcache_node, lru_entry);
    if (pcnode == NULL) {
        return NULL;
    }

    page_t *page = pcnode->page;
    assert(page != NULL, "__pcache_pop_lru: pcache_node has no page");
    page_lock_acquire(page);

    if (LIST_NODE_IS_DETACHED(pcnode, lru_entry)) {
        page_lock_release(page);
        goto retry;
    }

    assert(pcnode->pcache == pcache, "__pcache_pop_lru: pcache_node's pcache does not match the given pcache");
    pcache->lru_count--;
    assert(pcache->lru_count >= 0, "__pcache_pop_lru: pcache lru count underflow");
    list_node_detach(pcnode, lru_entry);
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
    if (LIST_NODE_IS_DETACHED(pcnode, lru_entry)) {
        pcache->dirty_count++;
    } else {
        list_node_detach(pcnode, lru_entry);
    }
    list_node_push_back(&pcache->dirty_list, pcnode, lru_entry);
}

// Pop a dirty page from pcache dirty list
// When latest_flush_jiffs is non-zero, only pop pages that were last flushed before that jiffs value
// Will return a page with its lock held
static page_t *__pcache_pop_dirty(struct pcache *pcache, uint64 latest_flush_jiffs) {
    __pcache_spin_assert_holding(pcache);
    if (LIST_IS_EMPTY(&pcache->dirty_list)) {
        return NULL;
    }
retry:
    struct pcache_node *pcnode = LIST_LAST_NODE(&pcache->dirty_list, struct pcache_node, lru_entry);
    if (pcnode == NULL) {
        return NULL;
    }
    page_t *page = pcnode->page;
    assert(page != NULL, "__pcache_pop_dirty: pcache_node has no page");
    page_lock_acquire(page);
    if (pcnode->last_flushed > latest_flush_jiffs && latest_flush_jiffs != 0) {
        // This page was flushed too recently, skip it
        page_lock_release(page);
        return NULL;
    }
    if (LIST_NODE_IS_DETACHED(pcnode, lru_entry)) {
        // Another thread has already removed this node, retry
        page_lock_release(page);
        goto retry;
    }
    assert(pcnode->pcache == pcache, "__pcache_pop_dirty: pcache_node's pcache does not match the given pcache");
    assert(pcnode->dirty, "__pcache_pop_dirty: pcache_node is not dirty");
    assert(!pcnode->io_in_progress, "__pcache_pop_dirty: pcache_node IO in progress");
    pcache->dirty_count--;
    assert(pcache->dirty_count >= 0, "__pcache_pop_dirty: pcache dirty count underflow");
    list_node_detach(pcnode, lru_entry);
    return page;
}

static page_t *__pcache_evict_lru(struct pcache *pcache) {
    page_t *page = __pcache_pop_lru(pcache);
    if (page == NULL) {
        return NULL;
    }
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
    spin_init(&__pcache_global_spinlock, "global_pcache_spinlock");
    int ret = slab_cache_init(  &__pcache_node_slab, 
                                "pcache_node", 
                                sizeof(struct pcache_node), 
                                SLAB_FLAG_EMBEDDED);
    assert(ret == 0, "Failed to initialize pcache node slab");
    __global_pcache_count = 0;
    __global_pcache_flush_wq = workqueue_create("pcache_flush_wq", WORKQUEUE_DEFAULT_MAX_ACTIVE);
    assert(__global_pcache_flush_wq != NULL, "Failed to create global pcache flush workqueue");
    printf("Page cache subsystem initialized\n");
    completion_init(&__global_flusher_completion);
    complete_all(&__global_flusher_completion);
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
    uint64 now = get_jiffs();
    pcache->last_flushed = now;
    pcache->last_request = now;
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
    uint64 base_blkno;
    page_t *page = NULL;
    page_t *new_page = NULL;
    struct pcache_node *pcnode = NULL;
    int ref;

    if (pcache == NULL || !__pcache_is_active(pcache)) {
        return NULL;
    }

    // Align the request to the page-sized block window handled by pcache.
    base_blkno = PCACHE_ALIGN_BLKNO(blkno);
    if (base_blkno >= pcache->blk_count) {
        return NULL;
    }
    if (base_blkno + PCACHE_BLKS_PER_PAGE > pcache->blk_count) {
        return NULL;
    }

retry_lookup:
#ifdef HOST_TEST
    if (__pcache_test_retry_hook != NULL) {
        __pcache_test_retry_hook(pcache, base_blkno);
    }
#endif
    page = __pcache_get_page(pcache, base_blkno, PGSIZE, NULL);
    if (page != NULL) {
        __pcache_spin_lock(pcache);
        page_lock_acquire(page);

        if (!__pcache_page_valid(pcache, page)) {
            page_lock_release(page);
            __pcache_spin_unlock(pcache);
            goto retry_lookup;
        }

        pcnode = page->pcache.pcache_node;
        assert(pcnode != NULL, "pcache_get_page: page missing pcache node");
        if (pcnode->blkno != base_blkno) {
            page_lock_release(page);
            __pcache_spin_unlock(pcache);
            goto retry_lookup;
        }

        if (!pcnode->dirty && !LIST_NODE_IS_DETACHED(pcnode, lru_entry)) {
            // The lookup reuses a clean LRU page; pull it out so the caller owns it.
            __pcache_remove_lru(pcache, page);
        }

        ref = page_ref_inc_unlocked(page);
        if (ref < 0) {
            page_lock_release(page);
            __pcache_spin_unlock(pcache);
            goto retry_lookup;
        }

        page_lock_release(page);
        __pcache_spin_unlock(pcache);
        return page;
    }

    // No cached copy: prepare a fresh pcache page.
    new_page = __pcache_page_alloc();
    if (new_page == NULL) {
        return NULL;
    }

    page_lock_acquire(new_page);
    pcnode = new_page->pcache.pcache_node;
    assert(pcnode != NULL, "pcache_get_page: new page has no pcache node");
    pcnode->blkno = base_blkno;
    pcnode->dirty = 0;
    pcnode->uptodate = 0;
    pcnode->io_in_progress = 0;
    pcnode->size = PGSIZE;

    __pcache_spin_lock(pcache);

    if (pcache->max_pages > 0) {
        while (pcache->page_count >= pcache->max_pages) {
            page_t *victim = __pcache_evict_lru(pcache);
            if (victim == NULL) {
                page_lock_release(new_page);
                __pcache_spin_unlock(pcache);
                __pcache_page_put(new_page);
                return NULL;
            }
            // Balance residency before inserting the new node.
            __pcache_page_put(victim);
        }
    }

    page = __pcache_get_page(pcache, base_blkno, PGSIZE, new_page);
    if (page == NULL) {
        page_lock_release(new_page);
        __pcache_spin_unlock(pcache);
        __pcache_page_put(new_page);
        return NULL;
    }

    if (page != new_page) {
        page_lock_release(new_page);
        __pcache_spin_unlock(pcache);
        __pcache_page_put(new_page);
        goto retry_lookup;
    }

    __pcache_node_attach_page(pcache, new_page);

    ref = page_ref_inc_unlocked(new_page);
    assert(ref > 1, "pcache_get_page: failed to add caller reference");

    page_lock_release(new_page);
    __pcache_spin_unlock(pcache);
    return new_page;
}

void pcache_put_page(struct pcache *pcache, page_t *page) {
    struct pcache_node *pcnode;
    int refcount;
    int new_refcount;

    if (pcache == NULL || page == NULL) {
        return;
    }

    __pcache_spin_lock(pcache);
    page_lock_acquire(page);

    if (!__pcache_page_valid(pcache, page)) {
        printf("pcache_put_page(): invalid page %p for cache %p\n", page, pcache);
        goto unlock;
    }

    pcnode = page->pcache.pcache_node;
    refcount = page_ref_count(page);
    if (refcount < 2) {
        printf("pcache_put_page(): page %p refcount %d is too small to drop\n", page, refcount);
        goto unlock;
    }

    new_refcount = page_ref_dec_unlocked(page);
    assert(new_refcount >= 1, "pcache_put_page(): refcount underflow");

    if (new_refcount == 1 && !pcnode->dirty && LIST_NODE_IS_DETACHED(pcnode, lru_entry)) {
        if (!pcnode->uptodate) {
            // The cache is the lone owner of a stale page; drop it entirely.
            __pcache_remove_node(pcache, page);
            __pcache_node_detach_page(pcache, page);
            page_lock_release(page);
            __pcache_spin_unlock(pcache);
            __pcache_page_put(page);
            return;
        }
        // Only clean, single-owner, up-to-date pages can be staged on the LRU for reuse.
        __pcache_push_lru(pcache, page);
    }

unlock:
    page_lock_release(page);
    __pcache_spin_unlock(pcache);
}

int pcache_mark_page_dirty(struct pcache *pcache, page_t *page) {
    struct pcache_node *pcnode;
    int ret = 0;

    if (pcache == NULL || page == NULL) {
        return -EINVAL;
    }

    __pcache_spin_lock(pcache);
    page_lock_acquire(page);

    if (!__pcache_page_valid(pcache, page)) {
        ret = -EINVAL;
        goto out;
    }

    pcnode = page->pcache.pcache_node;
    if (pcnode->dirty) {
        goto out; // already dirty, nothing new to track
    }

    if (pcnode->io_in_progress) {
        ret = -EBUSY;
        goto out;
    }

    if (!LIST_NODE_IS_DETACHED(pcnode, lru_entry) && !pcnode->dirty) {
        // A writer is claiming the page; pull it from the clean LRU pool.
        __pcache_remove_lru(pcache, page);
    }

    pcnode->dirty = 1;
    pcnode->uptodate = 1; // Writer guarantees the contents are authoritative now.
    __pcache_mark_dirty(pcache, page);
    __pcache_push_dirty(pcache, page);

out:
    page_lock_release(page);
    __pcache_spin_unlock(pcache);
    return ret;
}

int pcache_invalidate_page(struct pcache *pcache, page_t *page) {
    // Do regular checks
    // While holding the pcache spinlock and page lock:
    // - If the page is in IO, return -EBUSY
    // - If the page dirty, remove it from dirty list and clear dirty flag
    // - clear uptodate flag
    struct pcache_node *pcnode;
    int ret = 0;

    if (pcache == NULL || page == NULL) {
        return -EINVAL;
    }

    __pcache_spin_lock(pcache);
    page_lock_acquire(page);

    if (!__pcache_page_valid(pcache, page)) {
        ret = -EINVAL;
        goto out;
    }

    pcnode = page->pcache.pcache_node;

    if (pcnode->io_in_progress) {
        // Avoid invalidating while another thread owns the page for IO.
        ret = -EBUSY;
        goto out;
    }

    if (!LIST_NODE_IS_DETACHED(pcnode, lru_entry)) {
        // Detach the page from whichever queue currently tracks it.
        __pcache_remove_lru(pcache, page);
    }

    if (pcnode->dirty) {
        pcnode->dirty = 0;
    }

    pcnode->uptodate = 0;

out:
    page_lock_release(page);
    __pcache_spin_unlock(pcache);
    return ret;
}

// Flush all dirty pages in pcache and wait for completion
// User needs to check the status of the pcache after the call
// Return 0 on success, -ERRNO on failure
int pcache_flush(struct pcache *pcache) {
    int queued;

    if (pcache == NULL) {
        return -EINVAL;
    }

    __pcache_spin_lock(pcache);
    if (!__pcache_is_active(pcache)) {
        __pcache_spin_unlock(pcache);
        return -EINVAL;
    }

    queued = __pcache_queue_work(pcache);
    if (!queued) {
        int err;
        pcache->flush_requested = 0;
        if (pcache->flush_error == 0) {
            pcache->flush_error = -EAGAIN;
        }
        err = pcache->flush_error;
        __pcache_spin_unlock(pcache);
        return err;
    }

    __pcache_spin_unlock(pcache);

    // Block until the asynchronous flush worker reports completion.
    return __pcache_wait_flush_complete(pcache);
}

// Flush all pcaches and wait for completion
int pcache_sync(void) {
    __pcache_flusher_start();
    return __pcache_wait_flusher();
}

int pcache_read_page(struct pcache *pcache, page_t *page) {
    struct pcache_node *pcnode;
    int ret = 0;
    int refcount;

    if (pcache == NULL || page == NULL) {
        return -EINVAL;
    }

retry_locked:
    __pcache_spin_lock(pcache);
    page_lock_acquire(page);

    // Basic sanity: cache must be active and the page must belong to it.
    if (!__pcache_is_active(pcache)) {
        ret = -EINVAL;
        goto out_unlock_locked;
    }

    if (!__pcache_page_valid(pcache, page)) {
        ret = -EINVAL;
        goto out_unlock_locked;
    }

    // Readers must hold a caller reference in addition to the cache's.
    refcount = page_ref_count(page);
    if (refcount < 2) {
        printf("pcache_read_page(): page %p refcount %d is too small to read\n", page, refcount);
        ret = -EINVAL;
        goto out_unlock_locked;
    }

    pcnode = page->pcache.pcache_node;
    if (pcnode->blkno >= pcache->blk_count || pcnode->size == 0 || pcnode->size > PGSIZE) {
        printf("pcache_read_page(): invalid metadata for page %p (blkno=%llu size=%zu)\n",
               page,
               (unsigned long long)pcnode->blkno,
               pcnode->size);
        ret = -EINVAL;
        goto out_unlock_locked;
    }

    // Someone else is performing IO; wait or piggy-back depending on state.
    if (pcnode->io_in_progress) {
        int dirty = pcnode->dirty;
        int uptodate = pcnode->uptodate;

        page_lock_release(page);
        __pcache_spin_unlock(pcache);

        if (dirty && uptodate) {
            return 0;
        }

        if (!dirty && !uptodate) {
            __pcache_node_io_wait(pcache, page);
            goto retry_locked;
        }

        printf("pcache_read_page(): io in progress with unexpected state (dirty=%d uptodate=%d)\n",
               dirty,
               uptodate);
        return -EIO;
    }

    // Cached copy is already valid.
    if (pcnode->uptodate) {
        ret = 0;
        goto out_unlock_locked;
    }

    // Kick off device IO while still owning the bookkeeping locks.
    ret = __pcache_node_io_begin(pcache, page);
    assert(ret == 0, "pcache_read_page(): unexpected IO begin failure");

    page_lock_release(page);
    __pcache_spin_unlock(pcache);

    ret = __pcache_read_page(pcache, page);
    if (ret != 0) {
        __pcache_node_io_end(pcache, page);
        return ret;
    }

    ret = __pcache_node_io_wait(pcache, page);
    if (ret != 0) {
        __pcache_node_io_end(pcache, page);
        return ret;
    }

    // Re-check state now that IO has completed.
    __pcache_spin_lock(pcache);
    page_lock_acquire(page);

    if (!__pcache_page_valid(pcache, page)) {
        ret = -EINVAL;
        goto out_unlock_post_io;
    }

    pcnode = page->pcache.pcache_node;
    ret = pcnode->uptodate ? 0 : -EIO;

out_unlock_post_io:
    page_lock_release(page);
    __pcache_spin_unlock(pcache);
    __pcache_node_io_end(pcache, page);
    return ret;

out_unlock_locked:
    page_lock_release(page);
    __pcache_spin_unlock(pcache);
    return ret;
}

/******************************************************************************
 * System Call Handlers
 *****************************************************************************/
uint64 sys_sync(void) {
    int ret = pcache_sync();
    if (ret != 0) {
        printf("sys_sync: pcache_sync failed with error %d\n", ret);
    }
    return 0;
}
