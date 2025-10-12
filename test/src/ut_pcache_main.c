#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>

#include "errno.h"
#include "list.h"
#include "pcache.h"
#include "page.h"
#include "param.h"
#include "riscv.h"
#include "ut_thread.h"

extern bool __ut_queue_work_execute_immediately;
void __ut_reset_workqueue_stub(void);
void __ut_run_queued_work(void);

typedef struct {
    int read_calls;
    int write_calls;
    int write_begin_calls;
    int write_end_calls;
    int mark_dirty_calls;
    int abort_calls;
    int last_written_blk;
    int read_fail;
    int write_fail;
    int write_begin_fail;
} mock_backend_t;

static mock_backend_t *get_backend(struct pcache *pcache) {
    return (mock_backend_t *)pcache->private_data;
}

static int mock_read_page(struct pcache *pcache, page_t *page) {
    mock_backend_t *backend = get_backend(pcache);
    assert_non_null(backend);
    backend->read_calls++;
    if (backend->read_fail) {
        return -EIO;
    }
    backend->last_written_blk = (int)page->pcache.blkno;
    return 0;
}

static int mock_write_page(struct pcache *pcache, page_t *page) {
    mock_backend_t *backend = get_backend(pcache);
    assert_non_null(backend);
    backend->write_calls++;
    backend->last_written_blk = (int)page->pcache.blkno;
    if (backend->write_fail) {
        return -EIO;
    }
    return 0;
}

static int mock_write_begin(struct pcache *pcache) {
    mock_backend_t *backend = get_backend(pcache);
    assert_non_null(backend);
    backend->write_begin_calls++;
    if (backend->write_begin_fail) {
        return -EIO;
    }
    return 0;
}

static int mock_write_end(struct pcache *pcache) {
    mock_backend_t *backend = get_backend(pcache);
    assert_non_null(backend);
    backend->write_end_calls++;
    return 0;
}

static void mock_mark_dirty(struct pcache *pcache, page_t *page) {
    (void)page;
    mock_backend_t *backend = get_backend(pcache);
    assert_non_null(backend);
    backend->mark_dirty_calls++;
}

static void mock_abort_io(struct pcache *pcache, page_t *page) {
    (void)page;
    mock_backend_t *backend = get_backend(pcache);
    assert_non_null(backend);
    backend->abort_calls++;
}

static struct pcache_ops mock_ops = {
    .read_page = mock_read_page,
    .write_page = mock_write_page,
    .write_begin = mock_write_begin,
    .write_end = mock_write_end,
    .mark_dirty = mock_mark_dirty,
    .abort_io = mock_abort_io,
};

static int dummy_read_page(struct pcache *pcache, page_t *page) {
    (void)pcache;
    (void)page;
    return 0;
}

static int dummy_write_page(struct pcache *pcache, page_t *page) {
    (void)pcache;
    (void)page;
    return 0;
}

static struct pcache_ops dummy_ops = {
    .read_page = dummy_read_page,
    .write_page = dummy_write_page,
};

static void setup_pcache_with_backend(struct pcache *pcache, mock_backend_t *backend) {
    memset(pcache, 0, sizeof(*pcache));
    memset(backend, 0, sizeof(*backend));
    __ut_reset_workqueue_stub();
    pcache->ops = &mock_ops;
    pcache->blk_count = 128;
    assert_int_equal(pcache_init(pcache), 0);
    pcache->private_data = backend;
}

typedef struct dirty_thread_ctx {
    struct pcache *pcache;
    page_t *page;
} dirty_thread_ctx_t;

static void *dirty_marker_thread(void *arg) {
    dirty_thread_ctx_t *ctx = (dirty_thread_ctx_t *)arg;
    int ret = pcache_mark_page_dirty(ctx->pcache, ctx->page);
    return (void *)(intptr_t)ret;
}

static int pcache_group_setup(void **state) {
    (void)state;
    pcache_global_init();
    __ut_reset_workqueue_stub();
    return 0;
}

static int pcache_group_teardown(void **state) {
    (void)state;
    __ut_reset_workqueue_stub();
    return 0;
}

static void test_pcache_init_sets_defaults(void **state) {
    (void)state;
    struct pcache pcache = {0};
    pcache.ops = &dummy_ops;
    pcache.blk_count = 128;

    assert_int_equal(pcache_init(&pcache), 0);
    assert_true(pcache.active);
    assert_int_equal(pcache.flush_requested, 0);
    assert_int_equal(pcache.dirty_count, 0);
    assert_int_equal(pcache.page_count, 0);
    assert_int_equal(pcache.dirty_rate, PCACHE_DEFAULT_DIRTY_RATE);
    assert_int_equal(pcache.max_pages, PCACHE_DEFAULT_MAX_PAGES);
    assert_true(LIST_IS_EMPTY(&pcache.lru));
    assert_true(LIST_IS_EMPTY(&pcache.dirty_list));
    assert_true(LIST_IS_EMPTY(&pcache.flush_list));
}

static void test_pcache_init_preserves_custom_config(void **state) {
    (void)state;
    struct pcache pcache = {0};
    pcache.ops = &dummy_ops;
    pcache.blk_count = 64;
    pcache.dirty_rate = 45;
    pcache.max_pages = 512;
    pcache.gfp_flags = 0x1234;

    assert_int_equal(pcache_init(&pcache), 0);
    assert_int_equal(pcache.dirty_rate, 45);
    assert_int_equal(pcache.max_pages, 512);
    assert_int_equal(pcache.gfp_flags, 0x1234);
    assert_true(pcache.active);
}

static void test_pcache_init_rejects_invalid_config(void **state) {
    (void)state;
    struct pcache missing_ops = {0};
    missing_ops.blk_count = 32;
    assert_int_equal(pcache_init(&missing_ops), -EINVAL);

    struct pcache zero_blocks = {0};
    zero_blocks.ops = &dummy_ops;
    zero_blocks.blk_count = 0;
    assert_int_equal(pcache_init(&zero_blocks), -EINVAL);

    struct pcache dirty_state = {0};
    dirty_state.ops = &dummy_ops;
    dirty_state.blk_count = 16;
    dirty_state.page_count = 1; // non-zero should fail validation
    assert_int_equal(pcache_init(&dirty_state), -EINVAL);
}

static void test_pcache_flush_returns_zero_when_clean(void **state) {
    (void)state;
    struct pcache pcache = {0};
    pcache.ops = &dummy_ops;
    pcache.blk_count = 128;
    assert_int_equal(pcache_init(&pcache), 0);

    assert_int_equal(pcache_flush(&pcache), 0);
}

static void test_pcache_flush_propagates_error(void **state) {
    (void)state;
    struct pcache pcache = {0};
    pcache.ops = &dummy_ops;
    pcache.blk_count = 128;
    assert_int_equal(pcache_init(&pcache), 0);

    pcache.dirty_count = 2;
    pcache.flush_requested = 1;
    pcache.flush_error = -EIO;
    pcache.flush_completion.done = 1;

    assert_int_equal(pcache_flush(&pcache), -EIO);
}

static void test_pcache_get_page_reuses_cached_page(void **state) {
    (void)state;
    struct pcache pcache;
    mock_backend_t backend;
    setup_pcache_with_backend(&pcache, &backend);

    page_t *page1 = pcache_get_page(&pcache, 16);
    assert_non_null(page1);
    assert_int_equal(page1->pcache.blkno, 16UL & ~7UL);
    assert_int_equal(page1->pcache.size, PGSIZE);
    assert_int_equal(page1->ref_count, 2);
    assert_int_equal(pcache.page_count, 1);

    pcache_put_page(&pcache, page1);
    assert_int_equal(page1->ref_count, 1);
    assert_false(LIST_NODE_IS_DETACHED(page1, pcache.lru_entry));

    page_t *page2 = pcache_get_page(&pcache, 16);
    assert_ptr_equal(page1, page2);
    assert_int_equal(page2->ref_count, 2);
    assert_true(LIST_NODE_IS_DETACHED(page2, pcache.lru_entry));

    pcache_put_page(&pcache, page2);
}

static void test_pcache_mark_page_dirty_queues_flush(void **state) {
    (void)state;
    struct pcache pcache;
    mock_backend_t backend;
    setup_pcache_with_backend(&pcache, &backend);

    page_t *page = pcache_get_page(&pcache, 24);
    assert_non_null(page);

    pcache_mark_page_dirty(&pcache, page);

    assert_true(page->flags & PAGE_FLAG_DIRTY);
    assert_true(page->flags & PAGE_FLAG_UPTODATE);
    assert_false(LIST_NODE_IS_DETACHED(page, pcache.dirty_entry));
    assert_int_equal(pcache.dirty_count, 1);
    assert_int_equal(backend.mark_dirty_calls, 1);
    assert_true(pcache.flush_requested);
    assert_false(LIST_ENTRY_IS_DETACHED(&pcache.flush_list));

    pcache_put_page(&pcache, page);
}

static void test_pcache_read_page_invokes_backend(void **state) {
    (void)state;
    struct pcache pcache;
    mock_backend_t backend;
    setup_pcache_with_backend(&pcache, &backend);

    page_t *page = pcache_get_page(&pcache, 32);
    assert_non_null(page);

    assert_int_equal(pcache_read_page(&pcache, page), 0);
    assert_int_equal(backend.read_calls, 1);
    assert_true(page->flags & PAGE_FLAG_UPTODATE);

    backend.read_calls = 0;
    assert_int_equal(pcache_read_page(&pcache, page), 0);
    assert_int_equal(backend.read_calls, 0);

    pcache_put_page(&pcache, page);
}

static void test_pcache_read_page_propagates_error(void **state) {
    (void)state;
    struct pcache pcache;
    mock_backend_t backend;
    setup_pcache_with_backend(&pcache, &backend);

    page_t *page = pcache_get_page(&pcache, 40);
    assert_non_null(page);

    backend.read_fail = 1;
    page->flags &= ~PAGE_FLAG_UPTODATE;

    assert_int_equal(pcache_read_page(&pcache, page), -EIO);
    assert_int_equal(backend.read_calls, 1);
    assert_false(page->flags & PAGE_FLAG_UPTODATE);

    pcache_put_page(&pcache, page);
}

static void test_pcache_flush_writes_dirty_pages(void **state) {
    (void)state;
    struct pcache pcache;
    mock_backend_t backend;
    setup_pcache_with_backend(&pcache, &backend);

    pcache.dirty_rate = 101; // ensure mark does not queue a flush automatically

    page_t *page = pcache_get_page(&pcache, 48);
    assert_non_null(page);

    pcache_mark_page_dirty(&pcache, page);
    assert_false(pcache.flush_requested);

    pcache_put_page(&pcache, page);

    __ut_queue_work_execute_immediately = true;
    assert_int_equal(pcache_flush(&pcache), 0);
    __ut_queue_work_execute_immediately = false;

    assert_int_equal(backend.write_begin_calls, 1);
    assert_int_equal(backend.write_calls, 1);
    assert_int_equal(backend.write_end_calls, 1);
    assert_int_equal(backend.abort_calls, 0);
    assert_int_equal(pcache.dirty_count, 0);
    assert_false(page->flags & PAGE_FLAG_DIRTY);
    assert_true(page->flags & PAGE_FLAG_UPTODATE);
    assert_true(LIST_ENTRY_IS_DETACHED(&pcache.flush_list));
}

static void test_pcache_concurrent_dirty_markers(void **state) {
    (void)state;
    struct pcache pcache;
    mock_backend_t backend;
    setup_pcache_with_backend(&pcache, &backend);

    const int thread_count = 4;
    page_t *pages[thread_count];
    for (int i = 0; i < thread_count; i++) {
        pages[i] = pcache_get_page(&pcache, (uint64)(i * 8));
        assert_non_null(pages[i]);
    }

    ut_thread_t *threads[thread_count];
    dirty_thread_ctx_t ctx[thread_count];
    memset(threads, 0, sizeof(threads));
    for (int i = 0; i < thread_count; i++) {
        ctx[i].pcache = &pcache;
        ctx[i].page = pages[i];
        int rc = ut_thread_start(&threads[i], dirty_marker_thread, &ctx[i]);
        assert_int_equal(rc, 0);
    }

    for (int i = 0; i < thread_count; i++) {
        void *retval = NULL;
        int rc = ut_thread_join(threads[i], &retval);
        assert_int_equal(rc, 0);
        assert_int_equal((int)(intptr_t)retval, 0);
        ut_thread_destroy(threads[i]);
    }

    assert_int_equal(pcache.dirty_count, thread_count);
    assert_true(pcache.flush_requested);
    assert_false(LIST_ENTRY_IS_DETACHED(&pcache.flush_list));

    for (int i = 0; i < thread_count; i++) {
        assert_true(pages[i]->flags & PAGE_FLAG_DIRTY);
        assert_true(pages[i]->flags & PAGE_FLAG_UPTODATE);
    }

    __ut_queue_work_execute_immediately = true;
    assert_int_equal(pcache_flush(&pcache), 0);
    __ut_queue_work_execute_immediately = false;

    assert_int_equal(pcache.dirty_count, 0);
    assert_false(pcache.flush_requested);
    assert_true(LIST_ENTRY_IS_DETACHED(&pcache.flush_list));
    assert_int_equal(backend.write_calls, thread_count);

    for (int i = 0; i < thread_count; i++) {
        assert_false(pages[i]->flags & PAGE_FLAG_DIRTY);
        pcache_put_page(&pcache, pages[i]);
    }
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_pcache_init_sets_defaults),
        cmocka_unit_test(test_pcache_init_preserves_custom_config),
        cmocka_unit_test(test_pcache_init_rejects_invalid_config),
        cmocka_unit_test(test_pcache_flush_returns_zero_when_clean),
        cmocka_unit_test(test_pcache_flush_propagates_error),
        cmocka_unit_test(test_pcache_get_page_reuses_cached_page),
        cmocka_unit_test(test_pcache_mark_page_dirty_queues_flush),
        cmocka_unit_test(test_pcache_read_page_invokes_backend),
        cmocka_unit_test(test_pcache_read_page_propagates_error),
        cmocka_unit_test(test_pcache_flush_writes_dirty_pages),
        cmocka_unit_test(test_pcache_concurrent_dirty_markers),
    };

    return cmocka_run_group_tests(tests, pcache_group_setup, pcache_group_teardown);
}
