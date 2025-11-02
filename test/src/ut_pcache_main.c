#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <cmocka.h>

#include "pcache.h"
#include "list.h"
#include "param.h"
#include "bio.h"
#include "rbtree.h"
#include "page.h"
#include "page_type.h"
#include "spinlock.h"
#include "riscv.h"
#include "completion.h"
#include "timer.h"

extern void pcache_test_run_flusher_round(uint64 round_start, bool force_round);
extern void pcache_test_set_retry_hook(void (*hook)(struct pcache *, uint64));

extern void spin_init(struct spinlock *lock, char *name);

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
    struct scripted_op write_begin_script;
    struct scripted_op write_page_script;
    struct scripted_op write_end_script;
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

static int dummy_read_page(struct pcache *pcache, page_t *page) {
    (void)pcache;
    (void)page;
    return 0;
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
    fixture->ops.read_page = dummy_read_page;
    fixture->ops.write_page = scripted_write_page;
    fixture->ops.write_begin = scripted_write_begin;
    fixture->ops.write_end = scripted_write_end;
    fixture->ops.mark_dirty = dummy_mark_dirty;
    fixture->cache.ops = &fixture->ops;
}

static int pcache_test_setup(void **state) {
    static bool global_initialized = false;
    if (!global_initialized) {
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
    fixture->write_begin_calls = 0;
    fixture->write_page_calls = 0;
    fixture->write_end_calls = 0;

    int rc = pcache_init(&fixture->cache);
    assert_int_equal(rc, 0);

    g_active_fixture = fixture;
    *state = fixture;
    return 0;
}

static int pcache_test_teardown(void **state) {
    struct pcache_test_fixture *fixture = *state;
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
    completion_init(&node->io_completion);
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

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_pcache_init_defaults, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_get_page_from_lru, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_mark_page_dirty_tracks_state, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_mark_page_dirty_busy, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_invalidate_dirty_page, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_get_page_from_dirty_refcount_one, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_get_page_from_dirty_refcount_many, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_get_page_up_to_date, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_get_page_not_up_to_date, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_get_page_eviction_success, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_get_page_eviction_failure, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_get_page_retry_after_invalid_first_lookup, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_get_page_invalid_block, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_flush_worker_cleans_dirty_page, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_flush_worker_write_begin_failure, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_flush_worker_write_page_failure, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_flush_worker_write_end_error_propagates, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_flusher_force_round_flushes_dirty_page, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_flusher_respects_dirty_threshold, pcache_test_setup, pcache_test_teardown),
        cmocka_unit_test_setup_teardown(test_pcache_flusher_time_based_flush, pcache_test_setup, pcache_test_teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
