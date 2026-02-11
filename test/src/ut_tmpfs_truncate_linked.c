/*
 * Unit tests for tmpfs truncate.c — pcache-backed model
 *
 * truncate.c no longer uses a direct/indirect/double-indirect/triple-indirect
 * bmap.  All non-embedded file data lives in the per-inode pcache (i_data).
 * This test links directly to the kernel truncate.c by including it after
 * setting up the test environment with minimal stubs and mock pcache functions.
 */

/* Include test environment FIRST — this blocks kernel headers */
#include "tmpfs_test_env.h"

/* Now we can include standard headers safely */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>

#include <cmocka.h>

/* ============================================================================
 * Mock pcache infrastructure
 *
 * We keep a small pool of mock pages.  pcache_get_page returns a page from
 * the pool (keyed by blkno), pcache_discard_blk marks it free, etc.
 * ============================================================================
 */
#define MAX_MOCK_PAGES 64

static struct mock_page {
    page_t page;
    struct pcache_node node;
    char data[PAGE_SIZE];
    uint64 blkno;
    bool in_use;
} mock_pages[MAX_MOCK_PAGES];

static int mock_get_page_call_count = 0;
static int mock_discard_call_count = 0;
static bool mock_pcache_init_fail = false; /* Make pcache_init fail */
static bool mock_get_page_fail = false;    /* Make pcache_get_page fail */

static void reset_mock_pcache(void) {
    memset(mock_pages, 0, sizeof(mock_pages));
    mock_get_page_call_count = 0;
    mock_discard_call_count = 0;
    mock_pcache_init_fail = false;
    mock_get_page_fail = false;
}

static int mock_pages_in_use(void) {
    int count = 0;
    for (int i = 0; i < MAX_MOCK_PAGES; i++) {
        if (mock_pages[i].in_use)
            count++;
    }
    return count;
}

/* --- pcache function implementations used by truncate.c ------------------- */

void tmpfs_inode_pcache_init(struct vfs_inode *inode) {
    if (mock_pcache_init_fail) {
        inode->i_data.active = 0;
        return;
    }
    inode->i_data.active = 1;
}

void tmpfs_inode_pcache_teardown(struct vfs_inode *inode) {
    /* Discard all pages that belong to this pcache */
    for (int i = 0; i < MAX_MOCK_PAGES; i++) {
        mock_pages[i].in_use = false;
    }
    inode->i_data.active = 0;
}

page_t *pcache_get_page(struct pcache *pcache, uint64 blkno) {
    (void)pcache;
    mock_get_page_call_count++;
    if (mock_get_page_fail)
        return NULL;

    /* Return existing page if already allocated for this blkno */
    for (int i = 0; i < MAX_MOCK_PAGES; i++) {
        if (mock_pages[i].in_use && mock_pages[i].blkno == blkno)
            return &mock_pages[i].page;
    }
    /* Allocate a new mock page */
    for (int i = 0; i < MAX_MOCK_PAGES; i++) {
        if (!mock_pages[i].in_use) {
            mock_pages[i].in_use = true;
            mock_pages[i].blkno = blkno;
            memset(mock_pages[i].data, 0, PAGE_SIZE);
            mock_pages[i].node.data = mock_pages[i].data;
            mock_pages[i].page.pcache.pcache_node = &mock_pages[i].node;
            return &mock_pages[i].page;
        }
    }
    return NULL;
}

int pcache_read_page(struct pcache *pcache, page_t *page) {
    (void)pcache;
    (void)page;
    return 0;
}

void pcache_put_page(struct pcache *pcache, page_t *page) {
    (void)pcache;
    (void)page;
}

int pcache_mark_page_dirty(struct pcache *pcache, page_t *page) {
    (void)pcache;
    (void)page;
    return 0;
}

int pcache_discard_blk(struct pcache *pcache, uint64 blkno) {
    (void)pcache;
    mock_discard_call_count++;
    for (int i = 0; i < MAX_MOCK_PAGES; i++) {
        if (mock_pages[i].in_use && mock_pages[i].blkno == blkno) {
            mock_pages[i].in_use = false;
            return 0;
        }
    }
    return 0; /* Page might not exist; that is fine */
}

void pcache_teardown(struct pcache *pcache) {
    pcache->active = 0;
    for (int i = 0; i < MAX_MOCK_PAGES; i++) {
        mock_pages[i].in_use = false;
    }
}

/* ============================================================================
 * Panic stub (required by truncate.c's assert macro)
 * ============================================================================
 */
__attribute__((noreturn)) void __panic_impl(const char *type, const char *fmt,
                                            ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "%s: ", type);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    fail_msg("kernel panic reached in test");
    abort();
}

/* ============================================================================
 * Include the actual kernel truncate.c source
 * ============================================================================
 */
#include "../../kernel/vfs/tmpfs/truncate.c"

/* ============================================================================
 * Test fixtures
 * ============================================================================
 */
static struct tmpfs_inode *create_test_inode(void) {
    struct tmpfs_inode *inode = calloc(1, sizeof(struct tmpfs_inode));
    inode->vfs_inode.size = 0;
    inode->vfs_inode.n_blocks = 0;
    inode->vfs_inode.i_data.active = 0;
    inode->embedded = true;
    return inode;
}

static void destroy_test_inode(struct tmpfs_inode *inode) { free(inode); }

static int test_setup(void **state) {
    (void)state;
    reset_mock_pcache();
    return 0;
}

static int test_teardown(void **state) {
    (void)state;
    reset_mock_pcache();
    return 0;
}

/* ============================================================================
 * Positive tests for __tmpfs_truncate
 * ============================================================================
 */

/* Test truncate with same size (no-op) */
static void test_truncate_same_size(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 100;
    inode->embedded = true;

    int ret = __tmpfs_truncate(&inode->vfs_inode, 100);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, 100);

    destroy_test_inode(inode);
}

/* Test truncate grow within embedded data */
static void test_truncate_grow_embedded(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 10;
    inode->embedded = true;

    int ret = __tmpfs_truncate(&inode->vfs_inode, 50);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, 50);
    assert_true(inode->embedded);

    destroy_test_inode(inode);
}

/* Test truncate shrink within embedded data */
static void test_truncate_shrink_embedded(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 100;
    inode->embedded = true;

    int ret = __tmpfs_truncate(&inode->vfs_inode, 50);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, 50);
    assert_true(inode->embedded);

    destroy_test_inode(inode);
}

/* Test truncate shrink to zero from embedded */
static void test_truncate_shrink_to_zero_embedded(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 100;
    inode->embedded = true;

    int ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, 0);

    destroy_test_inode(inode);
}

/* Test truncate grow from embedded to pcache (migration) */
static void test_truncate_grow_to_pcache(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 10;
    inode->embedded = true;

    /* Write a known pattern into embedded data */
    memset(inode->file.data, 0xAB, 10);

    loff_t new_size = TMPFS_INODE_EMBEDDED_DATA_LEN + 100;
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, new_size);
    assert_false(inode->embedded); /* migrated away from embedded */
    assert_true(inode->vfs_inode.i_data.active); /* pcache is active */

    /* Verify the first page received the embedded data */
    assert_int_equal(mock_pages_in_use(), 1);

    /* Cleanup: shrink to zero */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);

    destroy_test_inode(inode);
}

/* Test that embedded data is correctly preserved during migration */
static void test_migrate_preserves_embedded_data(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 8;
    inode->embedded = true;

    /* Write known pattern */
    memcpy(inode->file.data, "TESTDATA", 8);

    loff_t new_size = TMPFS_INODE_EMBEDDED_DATA_LEN + 1;
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_false(inode->embedded);

    /* Find the page that was allocated and check its data */
    assert_int_equal(mock_pages_in_use(), 1);
    /* The first (and only) page should contain the copied embedded data */
    for (int i = 0; i < MAX_MOCK_PAGES; i++) {
        if (mock_pages[i].in_use) {
            assert_memory_equal(mock_pages[i].data, "TESTDATA", 8);
            break;
        }
    }

    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* Test truncate grow to a large size (pages are demand-allocated) */
static void test_truncate_grow_large(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 0;
    inode->embedded = true;

    /* Grow to 1MB — pcache allocates pages lazily, so no pages yet */
    loff_t new_size = 1024 * 1024;
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, new_size);
    assert_false(inode->embedded);
    /* Migration only touched 1 page (for the zero-length embedded data copy) */
    assert_true(mock_pages_in_use() <= 1);

    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* ============================================================================
 * Negative tests for __tmpfs_truncate
 * ============================================================================
 */

/* Test truncate to size exceeding maximum */
static void test_truncate_exceed_max_size(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 0;
    inode->embedded = true;

    int ret = __tmpfs_truncate(&inode->vfs_inode, TMPFS_MAX_FILE_SIZE + 1);
    assert_int_equal(ret, -EFBIG);
    assert_int_equal(inode->vfs_inode.size, 0);

    destroy_test_inode(inode);
}

/* Test truncate at exactly maximum size */
static void test_truncate_at_max_size(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 0;
    inode->embedded = true;

    int ret = __tmpfs_truncate(&inode->vfs_inode, TMPFS_MAX_FILE_SIZE);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, (loff_t)TMPFS_MAX_FILE_SIZE);

    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* Test pcache_init failure during migration */
static void test_truncate_grow_pcache_init_fail(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 10;
    inode->embedded = true;

    mock_pcache_init_fail = true;

    loff_t new_size = TMPFS_INODE_EMBEDDED_DATA_LEN + 100;
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, -ENOMEM);
    /* Size must not have changed */
    assert_int_equal(inode->vfs_inode.size, 10);

    destroy_test_inode(inode);
}

/* Test pcache_get_page failure during migration */
static void test_truncate_grow_get_page_fail(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 10;
    inode->embedded = true;

    mock_get_page_fail = true;

    loff_t new_size = TMPFS_INODE_EMBEDDED_DATA_LEN + 100;
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, -ENOMEM);
    assert_int_equal(inode->vfs_inode.size, 10);

    destroy_test_inode(inode);
}

/* ============================================================================
 * Shrink tests — pcache pages are discarded
 * ============================================================================
 */

/* Test shrink from pcache to smaller pcache size */
static void test_truncate_shrink_pcache(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();

    /* Migrate from embedded to pcache */
    int ret = __tmpfs_truncate(&inode->vfs_inode,
                               TMPFS_INODE_EMBEDDED_DATA_LEN + PAGE_SIZE * 5);
    assert_int_equal(ret, 0);
    assert_false(inode->embedded);

    /* Pre-populate some pcache pages to simulate writes */
    struct pcache *pc = &inode->vfs_inode.i_data;
    for (int blk = 0; blk < 6; blk++) {
        pcache_get_page(pc, (uint64)blk * (PAGE_SIZE / 512));
    }
    int pages_before = mock_pages_in_use();
    assert_true(pages_before >= 6);

    mock_discard_call_count = 0;

    /* Shrink — should discard pages beyond the new boundary */
    loff_t new_size = PAGE_SIZE;
    ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, new_size);
    /* At least some discards should have happened */
    assert_true(mock_discard_call_count > 0);

    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* Test shrink pcache to zero */
static void test_truncate_shrink_pcache_to_zero(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();

    /* Migrate to pcache */
    int ret = __tmpfs_truncate(&inode->vfs_inode,
                               TMPFS_INODE_EMBEDDED_DATA_LEN + PAGE_SIZE * 3);
    assert_int_equal(ret, 0);
    assert_false(inode->embedded);

    /* Pre-populate pages */
    struct pcache *pc = &inode->vfs_inode.i_data;
    for (int blk = 0; blk < 4; blk++) {
        pcache_get_page(pc, (uint64)blk * (PAGE_SIZE / 512));
    }

    mock_discard_call_count = 0;

    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, 0);
    /* All pages should have been discarded */
    assert_true(mock_discard_call_count > 0);

    destroy_test_inode(inode);
}

/* ============================================================================
 * Edge case tests
 * ============================================================================
 */

/* Test truncate at exact page boundary */
static void test_truncate_exact_page_boundary(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();

    /* Migrate to pcache first */
    int ret = __tmpfs_truncate(&inode->vfs_inode,
                               TMPFS_INODE_EMBEDDED_DATA_LEN + PAGE_SIZE * 3);
    assert_int_equal(ret, 0);

    /* Shrink to exactly 1 page */
    ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, PAGE_SIZE);

    /* Grow to exactly 2 pages */
    ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * 2);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, PAGE_SIZE * 2);

    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* Test truncate at one byte before page boundary */
static void test_truncate_one_byte_before_page_boundary(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();

    /* Migrate to pcache */
    int ret = __tmpfs_truncate(&inode->vfs_inode,
                               TMPFS_INODE_EMBEDDED_DATA_LEN + PAGE_SIZE * 3);
    assert_int_equal(ret, 0);

    /* Shrink to one byte before a page boundary */
    ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * 2 - 1);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, PAGE_SIZE * 2 - 1);

    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* Test truncate at one byte after page boundary */
static void test_truncate_one_byte_after_page_boundary(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();

    /* Migrate to pcache */
    int ret = __tmpfs_truncate(&inode->vfs_inode,
                               TMPFS_INODE_EMBEDDED_DATA_LEN + PAGE_SIZE * 3);
    assert_int_equal(ret, 0);

    /* Shrink to one byte past a page boundary */
    ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE + 1);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, PAGE_SIZE + 1);

    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* Test multiple grow/shrink cycles */
static void test_truncate_multiple_cycles(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 0;
    inode->embedded = true;

    int ret;

    /* Cycle 1: grow past embedded */
    ret = __tmpfs_truncate(&inode->vfs_inode,
                           TMPFS_INODE_EMBEDDED_DATA_LEN + PAGE_SIZE * 5);
    assert_int_equal(ret, 0);
    assert_false(inode->embedded);

    /* Cycle 2: shrink within pcache */
    ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * 2);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, PAGE_SIZE * 2);

    /* Cycle 3: grow again within pcache */
    ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * 10);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, PAGE_SIZE * 10);

    /* Cycle 4: shrink to zero */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, 0);

    destroy_test_inode(inode);
}

/* Test embedded grow zero-fills the gap */
static void test_embedded_grow_zero_fills(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 4;
    inode->embedded = true;
    memcpy(inode->file.data, "ABCD", 4);

    /* Grow within embedded — gap should be zeroed */
    int ret = __tmpfs_truncate(&inode->vfs_inode, 8);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, 8);
    assert_true(inode->embedded);

    /* Original data preserved */
    assert_memory_equal(inode->file.data, "ABCD", 4);
    /* Gap is zeroed */
    char zeros[4] = {0};
    assert_memory_equal(&inode->file.data[4], zeros, 4);

    destroy_test_inode(inode);
}

/* Test that shrink on non-active pcache is a no-op */
static void test_shrink_inactive_pcache_noop(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->embedded = false;
    inode->vfs_inode.size = PAGE_SIZE * 3;
    inode->vfs_inode.i_data.active = 0;

    mock_discard_call_count = 0;
    int ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, PAGE_SIZE);
    /* No discards because pcache was not active */
    assert_int_equal(mock_discard_call_count, 0);

    destroy_test_inode(inode);
}

/* Test shrink discard count matches expected range */
static void test_shrink_discard_count(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();

    /* Migrate to pcache */
    int ret = __tmpfs_truncate(&inode->vfs_inode,
                               TMPFS_INODE_EMBEDDED_DATA_LEN + PAGE_SIZE * 10);
    assert_int_equal(ret, 0);
    assert_false(inode->embedded);

    /* Shrink from ~11 pages to 2 pages.
     * TMPFS_IBLOCK(old_size) pages exist; first_discard = TMPFS_IBLOCK(new_size
     * rounded up). Exactly (old_block_cnt - first_discard) discard calls
     * expected. */
    loff_t old_size = inode->vfs_inode.size;
    loff_t new_size = PAGE_SIZE * 2;
    loff_t old_block_cnt = TMPFS_IBLOCK(old_size + PAGE_SIZE - 1);
    loff_t first_discard = TMPFS_IBLOCK(new_size + PAGE_SIZE - 1);
    int expected_discards = (int)(old_block_cnt - first_discard);

    mock_discard_call_count = 0;
    ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(mock_discard_call_count, expected_discards);

    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* ============================================================================
 * Main
 * ============================================================================
 */
int main(void) {
    const struct CMUnitTest tests[] = {
        /* Positive / basic tests */
        cmocka_unit_test_setup_teardown(test_truncate_same_size, test_setup,
                                        test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_grow_embedded, test_setup,
                                        test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_shrink_embedded,
                                        test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_shrink_to_zero_embedded,
                                        test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_grow_to_pcache,
                                        test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_migrate_preserves_embedded_data,
                                        test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_grow_large, test_setup,
                                        test_teardown),

        /* Negative tests */
        cmocka_unit_test_setup_teardown(test_truncate_exceed_max_size,
                                        test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_at_max_size, test_setup,
                                        test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_grow_pcache_init_fail,
                                        test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_grow_get_page_fail,
                                        test_setup, test_teardown),

        /* Shrink tests */
        cmocka_unit_test_setup_teardown(test_truncate_shrink_pcache, test_setup,
                                        test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_shrink_pcache_to_zero,
                                        test_setup, test_teardown),

        /* Edge cases */
        cmocka_unit_test_setup_teardown(test_truncate_exact_page_boundary,
                                        test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(
            test_truncate_one_byte_before_page_boundary, test_setup,
            test_teardown),
        cmocka_unit_test_setup_teardown(
            test_truncate_one_byte_after_page_boundary, test_setup,
            test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_multiple_cycles,
                                        test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_embedded_grow_zero_fills,
                                        test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_shrink_inactive_pcache_noop,
                                        test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_shrink_discard_count, test_setup,
                                        test_teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
