#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "types.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <cmocka.h>

#include <mm/pcache.h>
#include "list.h"
#include "param.h"
#include "dev/bio.h"
#include "rbtree.h"
#include <mm/page.h>
#include "mm/page_type.h"
#include "spinlock.h"
#include "riscv.h"
#include "completion.h"
#include "timer/timer.h"
#include "concurrency_harness.h"

// proc_queue_t is available via pcache_types.h -> proc_queue_type.h.
// We only forward-declare the functions we call to avoid the heavy
// proc_queue.h -> proc_types.h -> percpu.h -> printf.h include chain
// that conflicts with the host stdio.h.
void proc_queue_init(proc_queue_t *q, const char *name, spinlock_t *lock);

#include "wrapper_tracking.h"

// Forward declarations for wrapped functions (linked via --wrap)
void spin_init(struct spinlock *lock, char *name);
void spin_lock(struct spinlock *lock);
void spin_unlock(struct spinlock *lock);

extern void pcache_test_run_flusher_round(uint64 round_start, bool force_round);
extern void pcache_test_set_retry_hook(void (*hook)(struct pcache *, uint64));
extern void pcache_test_fail_next_queue_work(void);
extern void pcache_test_set_break_on_sleep(bool enable);
extern void pcache_test_fail_next_page_alloc(void);
extern void pcache_test_fail_next_slab_alloc(void);

#define SCRIPTED_OP_MAX 8

struct scripted_op {
    int values[SCRIPTED_OP_MAX];
    size_t count;
    size_t index;
    int default_value;
};

struct pcache_test_fixture {
    struct pcache cache;
    struct pcache_ops ops;
    int mark_dirty_calls;
    page_t *last_mark_dirty_page;
    struct scripted_op read_page_script;
    struct scripted_op write_begin_script;
    struct scripted_op write_page_script;
    struct scripted_op write_end_script;
    int read_page_calls;
    int write_begin_calls;
    int write_page_calls;
    int write_end_calls;
};

static struct pcache_test_fixture *g_active_fixture;
static page_t *g_retry_page;
static struct pcache_node *g_retry_node;
static bool g_retry_hook_used;
static bool g_retry_hook_armed;

static void scripted_reset(struct scripted_op *op, int default_value) {
    if (op == NULL) {
        return;
    }
    op->count = 0;
    op->index = 0;
    op->default_value = default_value;
}

static void scripted_append(struct scripted_op *op, int value) {
    assert_non_null(op);
    assert_true(op->count < SCRIPTED_OP_MAX);
    op->values[op->count++] = value;
}

static int scripted_next(struct scripted_op *op) {
    if (op == NULL) {
        return 0;
    }
    if (op->index < op->count) {
        return op->values[op->index++];
    }
    return op->default_value;
}

static int scripted_read_page(struct pcache *pcache, page_t *page) {
    (void)pcache;
    (void)page;
    if (g_active_fixture == NULL) {
        return 0;
    }
    g_active_fixture->read_page_calls++;
    return scripted_next(&g_active_fixture->read_page_script);
}

static int scripted_write_page(struct pcache *pcache, page_t *page) {
    (void)pcache;
    (void)page;
    if (g_active_fixture == NULL) {
        return 0;
    }
    g_active_fixture->write_page_calls++;
    return scripted_next(&g_active_fixture->write_page_script);
}

static int scripted_write_begin(struct pcache *pcache, page_t *page) {
    (void)pcache;
    (void)page;
    if (g_active_fixture == NULL) {
        return 0;
    }
    g_active_fixture->write_begin_calls++;
    return scripted_next(&g_active_fixture->write_begin_script);
}

static int scripted_write_end(struct pcache *pcache, page_t *page) {
    (void)pcache;
    (void)page;
    if (g_active_fixture == NULL) {
        return 0;
    }
    g_active_fixture->write_end_calls++;
    return scripted_next(&g_active_fixture->write_end_script);
}

static void dummy_mark_dirty(struct pcache *pcache, page_t *page) {
    (void)pcache;
    if (g_active_fixture != NULL) {
        g_active_fixture->mark_dirty_calls++;
        g_active_fixture->last_mark_dirty_page = page;
    }
}

static void configure_fixture_ops(struct pcache_test_fixture *fixture) {
    fixture->ops.read_page = scripted_read_page;
    fixture->ops.write_page = scripted_write_page;
    fixture->ops.write_begin = scripted_write_begin;
    fixture->ops.write_end = scripted_write_end;
    fixture->ops.mark_dirty = dummy_mark_dirty;
    fixture->cache.ops = &fixture->ops;
}

static int pcache_test_setup(void **state) {
    static bool global_initialized = false;
    if (!global_initialized) {
        // Setup mock for kernel_proc_create that pcache_global_init will call
        // We'll return a fake proc pointer and a positive PID
        will_return(__wrap_kernel_proc_create, (void*)0x1000);  // Return fake proc pointer
        will_return(__wrap_kernel_proc_create, 1);  // Return PID (success)
        
        pcache_global_init();
        global_initialized = true;
    }

    struct pcache_test_fixture *fixture = calloc(1, sizeof(*fixture));
    assert_non_null(fixture);

    configure_fixture_ops(fixture);
    fixture->cache.blk_count = 128;
    scripted_reset(&fixture->write_begin_script, 0);
    scripted_reset(&fixture->write_page_script, 0);
    scripted_reset(&fixture->write_end_script, 0);
    scripted_reset(&fixture->read_page_script, 0);
    fixture->read_page_calls = 0;
    fixture->write_begin_calls = 0;
    fixture->write_page_calls = 0;
    fixture->write_end_calls = 0;

    int rc = pcache_init(&fixture->cache);
    assert_int_equal(rc, 0);

    // Enable proc_queue tracking so wakeup_all/wait wrappers don't
    // require will_return() entries — they return 0 via the tracking struct.
    static proc_queue_tracking_t pq_tracking;
    memset(&pq_tracking, 0, sizeof(pq_tracking));
    wrapper_tracking_enable_proc_queue(&pq_tracking);

    g_active_fixture = fixture;
    *state = fixture;
    return 0;
}

static int pcache_test_teardown(void **state) {
    struct pcache_test_fixture *fixture = *state;
    wrapper_tracking_disable_proc_queue();
    pcache_test_unregister(&fixture->cache);
    pcache_test_set_retry_hook(NULL);
    g_retry_page = NULL;
    g_retry_node = NULL;
    g_retry_hook_used = false;
    g_retry_hook_armed = false;
    free(fixture);
    g_active_fixture = NULL;
    return 0;
}

static void init_mock_page(page_t *page, uint64 physical) {
    memset(page, 0, sizeof(*page));
    page->physical_address = physical;
    PAGE_FLAG_SET_TYPE(page->flags, PAGE_TYPE_PCACHE);
    page->ref_count = 1;
    spin_init(&page->lock, "pcache_test_page");
}

static void init_mock_node(struct pcache_node *node, struct pcache *cache, page_t *page, uint64 blkno) {
    memset(node, 0, sizeof(*node));
    rb_node_init(&node->tree_entry);
    list_entry_init(&node->lru_entry);
    node->pcache = cache;
    node->page = page;
    node->page_count = 1;
    node->blkno = blkno;
    node->size = PGSIZE;
    node->uptodate = 1;
    page->pcache.pcache = cache;
    page->pcache.pcache_node = node;
}

static void make_dirty_page(struct pcache *cache, struct pcache_node *node, page_t *page, uint64 blkno) {
    init_mock_page(page, blkno << BLK_SIZE_SHIFT);
    init_mock_node(node, cache, page, blkno);
    proc_queue_init(&node->io_waiters, "pcache_io_test", NULL);
    int rc = pcache_mark_page_dirty(cache, page);
    assert_int_equal(rc, 0);
    assert_int_equal(cache->dirty_count, 1);
}

static uint64 align_blkno(uint64 blkno) {
    uint64 blks_per_page = (uint64)PGSIZE >> BLK_SIZE_SHIFT;
    uint64 mask = blks_per_page - 1;
    return blkno & ~mask;
}

static page_t *create_cached_page(struct pcache *cache, uint64 blkno) {
    page_t *page = pcache_get_page(cache, blkno);
    assert_non_null(page);
    struct pcache_node *node = page->pcache.pcache_node;
    assert_non_null(node);
    assert_int_equal(node->blkno, align_blkno(blkno));
    page_lock_acquire(page);
    node->uptodate = 1;
    node->dirty = 0;
    page_lock_release(page);
    return page;
}

static void retry_restore_hook(struct pcache *cache, uint64 blkno) {
    (void)cache;
    (void)blkno;
    if (!g_retry_hook_armed || g_retry_page == NULL || g_retry_node == NULL) {
        return;
    }
    page_lock_acquire(g_retry_page);
    g_retry_page->pcache.pcache_node = g_retry_node;
    page_lock_release(g_retry_page);
    g_retry_hook_used = true;
    g_retry_hook_armed = false;
}

static void normalize_page_state(page_t *page) {
    if (page == NULL) {
        return;
    }
    page_lock_acquire(page);
    page->ref_count = 1;
    if (page->pcache.pcache_node != NULL) {
        page->pcache.pcache_node->dirty = 0;
        page->pcache.pcache_node->uptodate = 1;
        page->pcache.pcache_node->io_in_progress = 0;
    }
    page_lock_release(page);
}

static void test_pcache_init_defaults(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    assert_true(cache->active);
    assert_int_equal(cache->max_pages, PCACHE_DEFAULT_MAX_PAGES);
    assert_int_equal(cache->dirty_rate, PCACHE_DEFAULT_DIRTY_RATE);
    assert_true(LIST_IS_EMPTY(&cache->lru));
    assert_true(LIST_IS_EMPTY(&cache->dirty_list));
    assert_true(rb_root_is_empty(&cache->page_map));
    assert_int_equal(cache->page_count, 0);
    assert_int_equal(cache->dirty_count, 0);
    assert_int_equal(cache->flush_error, 0);
    assert_false(cache->flush_requested);
}

static void test_pcache_mark_page_dirty_tracks_state(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    page_t page;
    init_mock_page(&page, 0x2000);
    page.ref_count = 2;
    struct pcache_node node;
    uint64 blkno = 0;
    init_mock_node(&node, cache, &page, blkno);

    int rc = pcache_mark_page_dirty(cache, &page);
    assert_int_equal(rc, 0);
    assert_int_equal(node.dirty, 1);
    assert_int_equal(node.uptodate, 1);
    assert_int_equal(cache->dirty_count, 1);
    assert_false(LIST_NODE_IS_DETACHED(&node, lru_entry));
    assert_int_equal(fixture->mark_dirty_calls, 1);
    assert_ptr_equal(fixture->last_mark_dirty_page, &page);
}

static void test_pcache_mark_page_dirty_busy(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    page_t page;
    init_mock_page(&page, 0x3000);
    struct pcache_node node;
    uint64 blkno = (PGSIZE >> BLK_SIZE_SHIFT);
    init_mock_node(&node, cache, &page, blkno);
    node.io_in_progress = 1;

    int rc = pcache_mark_page_dirty(cache, &page);
    assert_int_equal(rc, -EBUSY);
    assert_int_equal(node.dirty, 0);
    assert_int_equal(cache->dirty_count, 0);
    assert_int_equal(fixture->mark_dirty_calls, 0);
}

static void test_pcache_mark_page_dirty_detaches_lru(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    uint64 blkno = 50;
    page_t *page = create_cached_page(cache, blkno);
    struct pcache_node *node = page->pcache.pcache_node;

    pcache_put_page(cache, page);
    assert_int_equal(cache->lru_count, 1);
    assert_int_equal(cache->dirty_count, 0);

    spin_lock(&cache->spinlock);
    page_lock_acquire(page);
    page->ref_count = 2;
    page_lock_release(page);
    spin_unlock(&cache->spinlock);

    int rc = pcache_mark_page_dirty(cache, page);
    assert_int_equal(rc, 0);
    assert_int_equal(cache->dirty_count, 1);
    assert_int_equal(cache->lru_count, 0);
    assert_true(node->dirty);
    assert_false(LIST_NODE_IS_DETACHED(node, lru_entry));
    assert_int_equal(fixture->mark_dirty_calls, 1);
    assert_ptr_equal(fixture->last_mark_dirty_page, page);

    assert_int_equal(pcache_invalidate_page(cache, page), 0);
    pcache_put_page(cache, page);
}

static void test_pcache_mark_page_dirty_idempotent(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    uint64 blkno = 52;
    page_t *page = create_cached_page(cache, blkno);
    struct pcache_node *node = page->pcache.pcache_node;

    int rc = pcache_mark_page_dirty(cache, page);
    assert_int_equal(rc, 0);
    assert_int_equal(cache->dirty_count, 1);
    assert_true(node->dirty);
    assert_int_equal(fixture->mark_dirty_calls, 1);

    rc = pcache_mark_page_dirty(cache, page);
    assert_int_equal(rc, 0);
    assert_int_equal(cache->dirty_count, 1);
    assert_true(node->dirty);
    assert_int_equal(fixture->mark_dirty_calls, 1);
    assert_false(LIST_NODE_IS_DETACHED(node, lru_entry));

    assert_int_equal(pcache_invalidate_page(cache, page), 0);
    pcache_put_page(cache, page);
}

static void test_pcache_mark_page_dirty_rejects_invalid_page(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    page_t page;
    init_mock_page(&page, 0x6000);
    page.ref_count = 2;

    int rc = pcache_mark_page_dirty(cache, &page);
    assert_int_equal(rc, -EINVAL);
    assert_int_equal(cache->dirty_count, 0);
    assert_int_equal(fixture->mark_dirty_calls, 0);
}

static void test_pcache_invalidate_dirty_page(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    page_t page;
    init_mock_page(&page, 0x4000);
    page.ref_count = 2;
    struct pcache_node node;
    uint64 blkno = (PGSIZE >> BLK_SIZE_SHIFT) * 3;
    init_mock_node(&node, cache, &page, blkno);

    assert_int_equal(pcache_mark_page_dirty(cache, &page), 0);
    assert_int_equal(cache->dirty_count, 1);

    int rc = pcache_invalidate_page(cache, &page);
    assert_int_equal(rc, 0);
    assert_int_equal(node.dirty, 0);
    assert_int_equal(node.uptodate, 0);
    assert_true(LIST_NODE_IS_DETACHED(&node, lru_entry));
    assert_int_equal(cache->dirty_count, 0);
}

static void test_pcache_invalidate_clean_lru_page(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    uint64 blkno = 54;
    page_t *page = create_cached_page(cache, blkno);
    struct pcache_node *node = page->pcache.pcache_node;

    assert_false(node->dirty);
    assert_true(node->uptodate);

    pcache_put_page(cache, page);
    assert_int_equal(cache->lru_count, 1);
    assert_false(LIST_NODE_IS_DETACHED(node, lru_entry));

    int rc = pcache_invalidate_page(cache, page);
    assert_int_equal(rc, 0);
    assert_true(LIST_NODE_IS_DETACHED(node, lru_entry));
    assert_int_equal(cache->lru_count, 0);
    assert_int_equal(cache->dirty_count, 0);
    assert_false(node->dirty);
    assert_false(node->uptodate);
}

static void test_pcache_invalidate_page_io_in_progress(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    uint64 blkno = 56;
    page_t *page = create_cached_page(cache, blkno);
    struct pcache_node *node = page->pcache.pcache_node;

    assert_int_equal(pcache_mark_page_dirty(cache, page), 0);
    assert_int_equal(cache->dirty_count, 1);
    assert_false(LIST_NODE_IS_DETACHED(node, lru_entry));

    page_lock_acquire(page);
    node->io_in_progress = 1;
    page_lock_release(page);

    int rc = pcache_invalidate_page(cache, page);
    assert_int_equal(rc, -EBUSY);
    assert_true(node->dirty);
    assert_false(LIST_NODE_IS_DETACHED(node, lru_entry));
    assert_int_equal(cache->dirty_count, 1);

    page_lock_acquire(page);
    node->io_in_progress = 0;
    page_lock_release(page);

    rc = pcache_invalidate_page(cache, page);
    assert_int_equal(rc, 0);
    assert_false(node->dirty);
    assert_true(LIST_NODE_IS_DETACHED(node, lru_entry));
    assert_int_equal(cache->dirty_count, 0);
}

static void test_pcache_invalidate_page_invalid_page(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    page_t page;
    init_mock_page(&page, 0x6800);
    page.ref_count = 2;

    int rc = pcache_invalidate_page(cache, &page);
    assert_int_equal(rc, -EINVAL);
    assert_int_equal(cache->dirty_count, 0);
    assert_int_equal(cache->lru_count, 0);
}

static void test_pcache_get_page_from_lru(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    uint64 blkno = 20;
    page_t *page = create_cached_page(cache, blkno);
    assert_int_equal(page->ref_count, 2);

    pcache_put_page(cache, page);
    assert_int_equal(cache->lru_count, 1);

    page_t *result = pcache_get_page(cache, blkno);
    assert_ptr_equal(result, page);
    assert_int_equal(result->ref_count, 2);
    assert_int_equal(cache->lru_count, 0);

    pcache_put_page(cache, result);
}

static void test_pcache_get_page_from_dirty_refcount_one(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    uint64 blkno = 22;
    page_t *page = create_cached_page(cache, blkno);
    struct pcache_node *node = page->pcache.pcache_node;

    assert_int_equal(pcache_mark_page_dirty(cache, page), 0);
    assert_int_equal(cache->dirty_count, 1);

    page_lock_acquire(page);
    page->ref_count = 1;
    page_lock_release(page);

    page_t *result = pcache_get_page(cache, blkno);
    assert_ptr_equal(result, page);
    assert_int_equal(result->ref_count, 2);
    assert_int_equal(cache->dirty_count, 1);
    assert_false(LIST_NODE_IS_DETACHED(node, lru_entry));
    normalize_page_state(result);
}

static void test_pcache_get_page_from_dirty_refcount_many(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    uint64 blkno = 24;
    page_t *page = create_cached_page(cache, blkno);
    struct pcache_node *node = page->pcache.pcache_node;

    assert_int_equal(pcache_mark_page_dirty(cache, page), 0);
    assert_int_equal(cache->dirty_count, 1);

    page_lock_acquire(page);
    page->ref_count = 3;
    page_lock_release(page);

    page_t *result = pcache_get_page(cache, blkno);
    assert_ptr_equal(result, page);
    assert_int_equal(result->ref_count, 4);
    assert_int_equal(cache->dirty_count, 1);
    assert_false(LIST_NODE_IS_DETACHED(node, lru_entry));

    normalize_page_state(result);
}

static void test_pcache_get_page_up_to_date(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    uint64 blkno = 26;
    page_t *page = create_cached_page(cache, blkno);
    struct pcache_node *node = page->pcache.pcache_node;
    node->uptodate = 1;

    pcache_put_page(cache, page);
    page_t *result = pcache_get_page(cache, blkno);

    assert_ptr_equal(result, page);
    assert_true(node->uptodate);

    pcache_put_page(cache, result);
}

static void test_pcache_get_page_not_up_to_date(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    uint64 blkno = 28;
    page_t *page = create_cached_page(cache, blkno);
    struct pcache_node *node = page->pcache.pcache_node;
    page_lock_acquire(page);
    node->uptodate = 0;
    page_lock_release(page);

    pcache_put_page(cache, page);
    page_t *result = pcache_get_page(cache, blkno);

    assert_ptr_not_equal(result, page);

    normalize_page_state(result);
}

static void test_pcache_get_page_eviction_success(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    cache->max_pages = 1;

    uint64 victim_blk = 30;
    page_t *victim = create_cached_page(cache, victim_blk);
    pcache_put_page(cache, victim);
    assert_int_equal(cache->lru_count, 1);

    uint64 new_blk = 32;
    page_t *new_page = pcache_get_page(cache, new_blk);
    assert_non_null(new_page);
    assert_int_equal(cache->page_count, 1);
    assert_int_equal(cache->lru_count, 0);
    assert_int_not_equal(new_page, victim);

    pcache_put_page(cache, new_page);
}

static void test_pcache_get_page_eviction_failure(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    cache->max_pages = 1;

    uint64 resident_blk = 34;
    page_t *resident = create_cached_page(cache, resident_blk);
    assert_non_null(resident);
    assert_int_equal(cache->page_count, 1);

    assert_int_equal(pcache_mark_page_dirty(cache, resident), 0);
    page_lock_acquire(resident);
    resident->ref_count = 2;
    page_lock_release(resident);

    // Make slab allocation fail so __pcache_page_alloc returns NULL
    pcache_test_fail_next_slab_alloc();

    uint64 request_blk = resident_blk + ((uint64)PGSIZE >> BLK_SIZE_SHIFT);
    page_t *result = pcache_get_page(cache, request_blk);
    assert_null(result);
    assert_int_equal(cache->page_count, 1);

    page_lock_acquire(resident);
    resident->ref_count = 1;
    page_lock_release(resident);
    normalize_page_state(resident);
    page_lock_acquire(resident);
    resident->ref_count = 2;
    page_lock_release(resident);
    pcache_put_page(cache, resident);
}

static void test_pcache_get_page_retry_after_invalid_first_lookup(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    uint64 blkno = 38;
    page_t *page = create_cached_page(cache, blkno);
    struct pcache_node *node = page->pcache.pcache_node;

    pcache_put_page(cache, page);

    page_lock_acquire(page);
    page->pcache.pcache_node = NULL;
    page_lock_release(page);

    g_retry_page = page;
    g_retry_node = node;
    g_retry_hook_used = false;
    g_retry_hook_armed = true;
    pcache_test_set_retry_hook(retry_restore_hook);

    page_t *result = pcache_get_page(cache, blkno);
    assert_ptr_equal(result, page);
    assert_true(g_retry_hook_used);

    pcache_test_set_retry_hook(NULL);
    g_retry_page = NULL;
    g_retry_node = NULL;
    normalize_page_state(result);
    page_lock_acquire(result);
    result->ref_count = 2;
    page_lock_release(result);
    pcache_put_page(cache, result);
}

static void test_pcache_get_page_invalid_block(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    uint64 invalid_blk = cache->blk_count + 10;
    page_t *result = pcache_get_page(cache, invalid_blk);
    assert_null(result);
}

static void test_pcache_read_page_populates_clean_page(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    uint64 blkno = 58;
    page_t *page = create_cached_page(cache, blkno);
    struct pcache_node *node = page->pcache.pcache_node;

    page_lock_acquire(page);
    node->uptodate = 0;
    node->dirty = 0;
    page_lock_release(page);

    int rc = pcache_read_page(cache, page);
    assert_int_equal(rc, 0);
    assert_int_equal(fixture->read_page_calls, 1);

    page_lock_acquire(page);
    assert_true(node->uptodate);
    assert_false(node->dirty);
    page_lock_release(page);

    pcache_put_page(cache, page);
}

static void test_pcache_read_page_propagates_failure(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    uint64 blkno = 60;
    page_t *page = create_cached_page(cache, blkno);
    struct pcache_node *node = page->pcache.pcache_node;

    page_lock_acquire(page);
    node->uptodate = 0;
    node->dirty = 0;
    page_lock_release(page);

    scripted_append(&fixture->read_page_script, -EIO);

    int rc = pcache_read_page(cache, page);
    assert_int_equal(rc, -EIO);
    assert_int_equal(fixture->read_page_calls, 1);

    page_lock_acquire(page);
    assert_false(node->uptodate);
    page_lock_release(page);

    pcache_put_page(cache, page);
}

static void test_pcache_put_page_requeues_dirty_detached(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    uint64 blkno = 44;
    page_t *page = create_cached_page(cache, blkno);
    struct pcache_node *node = page->pcache.pcache_node;

    assert_int_equal(pcache_mark_page_dirty(cache, page), 0);
    assert_int_equal(cache->dirty_count, 1);

    spin_lock(&cache->spinlock);
    page_lock_acquire(page);
    list_node_detach(node, lru_entry);
    cache->dirty_count--;
    page->ref_count = 2;
    page_lock_release(page);
    spin_unlock(&cache->spinlock);

    assert_true(LIST_NODE_IS_DETACHED(node, lru_entry));
    assert_int_equal(cache->dirty_count, 0);

    pcache_put_page(cache, page);

    assert_int_equal(cache->dirty_count, 1);
    spin_lock(&cache->spinlock);
    page_lock_acquire(page);
    assert_true(node->dirty);
    assert_false(LIST_NODE_IS_DETACHED(node, lru_entry));
    assert_int_equal(page->ref_count, 1);
    node->dirty = 0;
    list_node_detach(node, lru_entry);
    cache->dirty_count--;
    node->uptodate = 1;
    page->ref_count = 2;
    page_lock_release(page);
    spin_unlock(&cache->spinlock);

    pcache_put_page(cache, page);
}

static void test_pcache_flush_worker_cleans_dirty_page(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    page_t page;
    struct pcache_node node;
    make_dirty_page(cache, &node, &page, 4);

    int rc = pcache_flush(cache);
    assert_int_equal(rc, 0);
    assert_int_equal(cache->dirty_count, 0);
    assert_int_equal(cache->lru_count, 1);
    assert_false(node.dirty);
    assert_int_equal(cache->flush_error, 0);
    assert_int_equal(page.ref_count, 1);
    assert_false(LIST_NODE_IS_DETACHED(&node, lru_entry));
    assert_int_equal(fixture->write_begin_calls, 1);
    assert_int_equal(fixture->write_page_calls, 1);
    assert_int_equal(fixture->write_end_calls, 1);
}

static void test_pcache_flush_worker_write_begin_failure(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    page_t page;
    struct pcache_node node;
    make_dirty_page(cache, &node, &page, 6);
    scripted_append(&fixture->write_begin_script, -EIO);

    int rc = pcache_flush(cache);
    assert_int_equal(rc, -EIO);
    assert_int_equal(cache->dirty_count, 1);
    assert_true(node.dirty);
    assert_false(LIST_NODE_IS_DETACHED(&node, lru_entry));
    assert_int_equal(cache->lru_count, 0);
    assert_int_equal(fixture->write_begin_calls, 1);
    assert_int_equal(fixture->write_page_calls, 0);
    assert_int_equal(fixture->write_end_calls, 0);
}

static void test_pcache_flush_worker_write_page_failure(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    page_t page;
    struct pcache_node node;
    make_dirty_page(cache, &node, &page, 8);
    scripted_append(&fixture->write_page_script, -EIO);
    scripted_append(&fixture->write_end_script, -EPIPE);

    int rc = pcache_flush(cache);
    assert_int_equal(rc, -EPIPE);
    assert_int_equal(cache->dirty_count, 1);
    assert_true(node.dirty);
    assert_false(LIST_NODE_IS_DETACHED(&node, lru_entry));
    assert_int_equal(cache->lru_count, 0);
    assert_int_equal(fixture->write_begin_calls, 1);
    assert_int_equal(fixture->write_page_calls, 1);
    assert_int_equal(fixture->write_end_calls, 1);
}

static void test_pcache_flush_worker_write_end_error_propagates(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    page_t page;
    struct pcache_node node;
    make_dirty_page(cache, &node, &page, 10);
    page.ref_count = 2;
    scripted_append(&fixture->write_end_script, -EAGAIN);

    int rc = pcache_flush(cache);
    assert_int_equal(rc, -EAGAIN);
    assert_int_equal(cache->dirty_count, 0);
    assert_true(LIST_NODE_IS_DETACHED(&node, lru_entry));
    assert_false(node.dirty);
    assert_int_equal(cache->lru_count, 0);
    assert_int_equal(fixture->write_begin_calls, 1);
    assert_int_equal(fixture->write_page_calls, 1);
    assert_int_equal(fixture->write_end_calls, 1);
}

static void test_pcache_flush_queue_failure_returns_new_error(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    page_t page;
    struct pcache_node node;
    make_dirty_page(cache, &node, &page, 18);
    scripted_append(&fixture->write_page_script, -EIO);
    scripted_append(&fixture->write_end_script, -EPIPE);

    int rc = pcache_flush(cache);
    assert_int_equal(rc, -EPIPE);
    assert_int_equal(cache->flush_error, -EPIPE);
    assert_true(node.dirty);
    assert_int_equal(cache->dirty_count, 1);
    assert_int_equal(fixture->write_begin_calls, 1);
    assert_int_equal(fixture->write_page_calls, 1);
    assert_int_equal(fixture->write_end_calls, 1);

    pcache_test_fail_next_queue_work();

    rc = pcache_flush(cache);
    assert_int_equal(rc, -EAGAIN);
    assert_int_equal(cache->flush_error, -EAGAIN);
    assert_true(node.dirty);
    assert_int_equal(cache->dirty_count, 1);
    assert_int_equal(fixture->write_begin_calls, 1);
    assert_int_equal(fixture->write_page_calls, 1);
    assert_int_equal(fixture->write_end_calls, 1);
}

static void test_pcache_flusher_force_round_flushes_dirty_page(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    page_t page;
    struct pcache_node node;
    make_dirty_page(cache, &node, &page, 12);

    uint64 round_start = get_jiffs();
    pcache_test_run_flusher_round(round_start, true);

    assert_int_equal(cache->dirty_count, 0);
    assert_false(node.dirty);
    assert_int_equal(cache->lru_count, 1);
    assert_false(LIST_NODE_IS_DETACHED(&node, lru_entry));
    assert_int_equal(fixture->write_begin_calls, 1);
    assert_int_equal(fixture->write_page_calls, 1);
    assert_int_equal(fixture->write_end_calls, 1);
}

static void test_pcache_flusher_respects_dirty_threshold(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    page_t page;
    struct pcache_node node;
    make_dirty_page(cache, &node, &page, 14);

    cache->page_count = 100;
    cache->dirty_rate = 80;
    cache->last_flushed = 1000;
    cache->last_request = 1000;
    uint64 round_start = cache->last_flushed + 1;

    pcache_test_run_flusher_round(round_start, false);

    assert_int_equal(cache->dirty_count, 1);
    assert_true(node.dirty);
    assert_int_equal(cache->lru_count, 0);
    assert_false(LIST_NODE_IS_DETACHED(&node, lru_entry));
    assert_int_equal(cache->flush_requested, 0);
    assert_int_equal(fixture->write_begin_calls, 0);
    assert_int_equal(fixture->write_page_calls, 0);
    assert_int_equal(fixture->write_end_calls, 0);
}

static void test_pcache_flusher_time_based_flush(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    page_t page;
    struct pcache_node node;
    make_dirty_page(cache, &node, &page, 16);

    cache->page_count = 100;
    cache->dirty_rate = 80;
    cache->last_flushed = 5;
    cache->last_request = 5;
    uint64 round_start = cache->last_flushed + PCACHE_FLUSH_INTERVAL_JIFFS + 5;

    pcache_test_run_flusher_round(round_start, false);

    assert_int_equal(cache->dirty_count, 0);
    assert_false(node.dirty);
    assert_int_equal(cache->lru_count, 1);
    assert_false(LIST_NODE_IS_DETACHED(&node, lru_entry));
    assert_int_equal(fixture->write_begin_calls, 1);
    assert_int_equal(fixture->write_page_calls, 1);
    assert_int_equal(fixture->write_end_calls, 1);
}

/******************************************************************************
 * Concurrency tests
 *
 * These use real pthread threads and the concurrency harness that maps xv6
 * spinlocks to pthread mutexes and proc_queue to condvars so that
 * lock contention and blocking/wakeup actually happen.
 ******************************************************************************/

static int pcache_conc_test_setup(void **state) {
    int rc = pcache_test_setup(state);
    if (rc != 0) return rc;
    concurrency_mode_enable();
    return 0;
}

static int pcache_conc_test_teardown(void **state) {
    concurrency_mode_disable();
    return pcache_test_teardown(state);
}

// ---------------------------------------------------------------------------
// Test 1: Two threads call pcache_get_page for the same block concurrently.
//         Both must receive the same page.
// ---------------------------------------------------------------------------

struct conc_get_same_page_ctx {
    struct pcache *cache;
    uint64 blkno;
    page_t *result;
};

static void *conc_get_same_page_thread(void *arg) {
    struct conc_get_same_page_ctx *ctx = arg;
    ctx->result = pcache_get_page(ctx->cache, ctx->blkno);
    return NULL;
}

static void test_conc_get_page_same_block(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    struct conc_get_same_page_ctx ctx1 = { .cache = cache, .blkno = 8, .result = NULL };
    struct conc_get_same_page_ctx ctx2 = { .cache = cache, .blkno = 8, .result = NULL };

    conc_thread_create(0, conc_get_same_page_thread, &ctx1);
    conc_thread_create(1, conc_get_same_page_thread, &ctx2);
    conc_thread_join(0, NULL);
    conc_thread_join(1, NULL);

    // Both threads must get a valid page
    assert_non_null(ctx1.result);
    assert_non_null(ctx2.result);
    // Both must point to the same physical page (same block number -> same cache entry)
    assert_ptr_equal(ctx1.result, ctx2.result);

    pcache_put_page(cache, ctx1.result);
    pcache_put_page(cache, ctx2.result);
}

// ---------------------------------------------------------------------------
// Test 2: Two threads call pcache_get_page for different blocks concurrently.
//         They must get distinct pages.
// ---------------------------------------------------------------------------

static void test_conc_get_page_different_blocks(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    uint64 blks_per_page = (uint64)PGSIZE >> BLK_SIZE_SHIFT;
    struct conc_get_same_page_ctx ctx1 = { .cache = cache, .blkno = 0, .result = NULL };
    struct conc_get_same_page_ctx ctx2 = { .cache = cache, .blkno = blks_per_page, .result = NULL };

    conc_thread_create(0, conc_get_same_page_thread, &ctx1);
    conc_thread_create(1, conc_get_same_page_thread, &ctx2);
    conc_thread_join(0, NULL);
    conc_thread_join(1, NULL);

    assert_non_null(ctx1.result);
    assert_non_null(ctx2.result);
    assert_ptr_not_equal(ctx1.result, ctx2.result);

    pcache_put_page(cache, ctx1.result);
    pcache_put_page(cache, ctx2.result);
}

// ---------------------------------------------------------------------------
// Test 3: IO wait / IO end across threads.
//         Thread A reads a page (starts IO, marks in-progress, calls read_page
//         callback, completes IO).
//         Thread B calls pcache_read_page on the same page after IO is
//         started, sees io_in_progress, and waits. Thread A finishing IO wakes
//         Thread B.
// ---------------------------------------------------------------------------

struct conc_io_ctx {
    struct pcache *cache;
    page_t *page;
    int result;
};

static void *conc_read_page_thread(void *arg) {
    struct conc_io_ctx *ctx = arg;
    ctx->result = pcache_read_page(ctx->cache, ctx->page);
    return NULL;
}

static _Atomic int g_conc_read_page_calls;

static int conc_slow_read_page(struct pcache *pcache, page_t *page) {
    (void)pcache;
    (void)page;
    __atomic_fetch_add(&g_conc_read_page_calls, 1, __ATOMIC_SEQ_CST);
    // Sleep long enough for Thread B to see io_in_progress and block
    // on the proc_queue before we finish IO.
    conc_sleep_ms(50);
    return 0;
}

static void test_conc_io_wait_and_complete(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    // Override read_page with a slow callback
    fixture->ops.read_page = conc_slow_read_page;
    __atomic_store_n(&g_conc_read_page_calls, 0, __ATOMIC_SEQ_CST);

    // Pre-create a cached page that is NOT up-to-date
    page_t *page = pcache_get_page(cache, 16);
    assert_non_null(page);

    struct conc_io_ctx ctx1 = { .cache = cache, .page = page, .result = -1 };
    struct conc_io_ctx ctx2 = { .cache = cache, .page = page, .result = -1 };

    // Thread A: first reader, will call read_page callback (sleeps 50ms)
    conc_thread_create(0, conc_read_page_thread, &ctx1);
    // Small delay so Thread A wins the race to io_begin
    conc_sleep_ms(5);
    // Thread B: second reader, should see io_in_progress and wait
    conc_thread_create(1, conc_read_page_thread, &ctx2);

    conc_thread_join(0, NULL);
    conc_thread_join(1, NULL);

    assert_int_equal(ctx1.result, 0);
    assert_int_equal(ctx2.result, 0);

    // Only one thread should have actually called the read_page callback
    // (the other should have waited and then seen uptodate=1)
    assert_int_equal(__atomic_load_n(&g_conc_read_page_calls, __ATOMIC_SEQ_CST), 1);

    pcache_put_page(cache, page);
}

// ---------------------------------------------------------------------------
// Test 4: Many threads all get different pages concurrently.
//         Stress test for lock contention on the pcache spinlock and tree_lock.
// ---------------------------------------------------------------------------

#define CONC_STRESS_THREAD_COUNT 8
#define CONC_STRESS_PAGES_PER_THREAD 4

struct conc_stress_ctx {
    struct pcache *cache;
    int thread_id;
    page_t *pages[CONC_STRESS_PAGES_PER_THREAD];
    int success_count;
};

static void *conc_stress_get_pages_thread(void *arg) {
    struct conc_stress_ctx *ctx = arg;
    uint64 blks_per_page = (uint64)PGSIZE >> BLK_SIZE_SHIFT;
    ctx->success_count = 0;
    for (int i = 0; i < CONC_STRESS_PAGES_PER_THREAD; i++) {
        uint64 blkno = (uint64)(ctx->thread_id * CONC_STRESS_PAGES_PER_THREAD + i) * blks_per_page;
        ctx->pages[i] = pcache_get_page(ctx->cache, blkno);
        if (ctx->pages[i] != NULL) {
            ctx->success_count++;
        }
    }
    return NULL;
}

static void test_conc_stress_get_pages(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;

    // Make sure max_pages and blk_count are high enough for all threads
    cache->max_pages = CONC_STRESS_THREAD_COUNT * CONC_STRESS_PAGES_PER_THREAD + 16;
    uint64 blks_per_page = (uint64)PGSIZE >> BLK_SIZE_SHIFT;
    cache->blk_count = (uint64)CONC_STRESS_THREAD_COUNT * CONC_STRESS_PAGES_PER_THREAD * blks_per_page + blks_per_page;

    struct conc_stress_ctx ctxs[CONC_STRESS_THREAD_COUNT];

    for (int i = 0; i < CONC_STRESS_THREAD_COUNT; i++) {
        memset(&ctxs[i], 0, sizeof(ctxs[i]));
        ctxs[i].cache = cache;
        ctxs[i].thread_id = i;
    }

    for (int i = 0; i < CONC_STRESS_THREAD_COUNT; i++) {
        conc_thread_create(i, conc_stress_get_pages_thread, &ctxs[i]);
    }
    for (int i = 0; i < CONC_STRESS_THREAD_COUNT; i++) {
        conc_thread_join(i, NULL);
    }

    // All pages should be successfully allocated
    int total = 0;
    for (int i = 0; i < CONC_STRESS_THREAD_COUNT; i++) {
        total += ctxs[i].success_count;
    }
    assert_int_equal(total, CONC_STRESS_THREAD_COUNT * CONC_STRESS_PAGES_PER_THREAD);

    // All pages should be distinct
    page_t *all_pages[CONC_STRESS_THREAD_COUNT * CONC_STRESS_PAGES_PER_THREAD];
    int idx = 0;
    for (int i = 0; i < CONC_STRESS_THREAD_COUNT; i++) {
        for (int j = 0; j < CONC_STRESS_PAGES_PER_THREAD; j++) {
            assert_non_null(ctxs[i].pages[j]);
            all_pages[idx++] = ctxs[i].pages[j];
        }
    }
    // Check uniqueness with O(n^2) - fine for small counts
    for (int i = 0; i < idx; i++) {
        for (int j = i + 1; j < idx; j++) {
            assert_ptr_not_equal(all_pages[i], all_pages[j]);
        }
    }

    // Cleanup
    for (int i = 0; i < idx; i++) {
        pcache_put_page(cache, all_pages[i]);
    }
}

// ---------------------------------------------------------------------------
// Test 5: Concurrent get_page + mark_dirty.
//         Multiple threads each get a page and immediately mark it dirty.
//         Verify dirty_count is correct at the end.
// ---------------------------------------------------------------------------

struct conc_dirty_ctx {
    struct pcache *cache;
    uint64 blkno;
    page_t *page;
    int get_ok;
    int dirty_ok;
};

static void *conc_get_and_dirty_thread(void *arg) {
    struct conc_dirty_ctx *ctx = arg;
    ctx->page = pcache_get_page(ctx->cache, ctx->blkno);
    ctx->get_ok = (ctx->page != NULL);
    if (ctx->get_ok) {
        ctx->dirty_ok = (pcache_mark_page_dirty(ctx->cache, ctx->page) == 0);
    }
    return NULL;
}

static void test_conc_get_and_dirty(void **state) {
    struct pcache_test_fixture *fixture = *state;
    struct pcache *cache = &fixture->cache;
    cache->max_pages = 64;

    const int N = 4;
    struct conc_dirty_ctx ctxs[4];
    uint64 blks_per_page = (uint64)PGSIZE >> BLK_SIZE_SHIFT;

    for (int i = 0; i < N; i++) {
        memset(&ctxs[i], 0, sizeof(ctxs[i]));
        ctxs[i].cache = cache;
        ctxs[i].blkno = (uint64)i * blks_per_page;
    }

    for (int i = 0; i < N; i++) {
        conc_thread_create(i, conc_get_and_dirty_thread, &ctxs[i]);
    }
    for (int i = 0; i < N; i++) {
        conc_thread_join(i, NULL);
    }

    for (int i = 0; i < N; i++) {
        assert_true(ctxs[i].get_ok);
        assert_true(ctxs[i].dirty_ok);
    }
    assert_int_equal(cache->dirty_count, N);

    for (int i = 0; i < N; i++) {
        pcache_put_page(cache, ctxs[i].page);
    }
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_pcache_init_defaults, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_get_page_from_lru, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_mark_page_dirty_tracks_state, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_mark_page_dirty_busy, pcache_test_setup, pcache_test_teardown),
    cmocka_unit_test_setup_teardown(test_pcache_mark_page_dirty_detaches_lru, pcache_test_setup, pcache_test_teardown),
    cmocka_unit_test_setup_teardown(test_pcache_mark_page_dirty_idempotent, pcache_test_setup, pcache_test_teardown),
    cmocka_unit_test_setup_teardown(test_pcache_mark_page_dirty_rejects_invalid_page, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_invalidate_dirty_page, pcache_test_setup, pcache_test_teardown),
    cmocka_unit_test_setup_teardown(test_pcache_invalidate_clean_lru_page, pcache_test_setup, pcache_test_teardown),
    cmocka_unit_test_setup_teardown(test_pcache_invalidate_page_io_in_progress, pcache_test_setup, pcache_test_teardown),
    cmocka_unit_test_setup_teardown(test_pcache_invalidate_page_invalid_page, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_get_page_from_dirty_refcount_one, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_get_page_from_dirty_refcount_many, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_get_page_up_to_date, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_get_page_not_up_to_date, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_get_page_eviction_success, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_get_page_eviction_failure, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_get_page_retry_after_invalid_first_lookup, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_get_page_invalid_block, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_read_page_populates_clean_page, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_read_page_propagates_failure, pcache_test_setup, pcache_test_teardown),
    cmocka_unit_test_setup_teardown(test_pcache_put_page_requeues_dirty_detached, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_flush_worker_cleans_dirty_page, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_flush_worker_write_begin_failure, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_flush_worker_write_page_failure, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_flush_worker_write_end_error_propagates, pcache_test_setup, pcache_test_teardown),
    cmocka_unit_test_setup_teardown(test_pcache_flush_queue_failure_returns_new_error, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_flusher_force_round_flushes_dirty_page, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_flusher_respects_dirty_threshold, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_flusher_time_based_flush, pcache_test_setup, pcache_test_teardown),
        // Concurrency tests
        cmocka_unit_test_setup_teardown(test_conc_get_page_same_block, pcache_conc_test_setup, pcache_conc_test_teardown),
        cmocka_unit_test_setup_teardown(test_conc_get_page_different_blocks, pcache_conc_test_setup, pcache_conc_test_teardown),
        cmocka_unit_test_setup_teardown(test_conc_io_wait_and_complete, pcache_conc_test_setup, pcache_conc_test_teardown),
        cmocka_unit_test_setup_teardown(test_conc_stress_get_pages, pcache_conc_test_setup, pcache_conc_test_teardown),
        cmocka_unit_test_setup_teardown(test_conc_get_and_dirty, pcache_conc_test_setup, pcache_conc_test_teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
