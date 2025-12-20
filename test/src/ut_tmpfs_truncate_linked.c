/*
 * Unit tests for tmpfs truncate.c
 * 
 * This file links directly to the kernel truncate.c by including it after
 * setting up the test environment with minimal stubs.
 */

/* Include test environment FIRST - this blocks kernel headers */
#include "tmpfs_test_env.h"

/* Now we can include standard headers safely */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>

#include <cmocka.h>

/* ============================================================================
 * Mock state tracking
 * ============================================================================ */
static int kalloc_call_count = 0;
static int kfree_call_count = 0;
static int kalloc_fail_after = -1;  /* -1 means never fail */
static void **allocated_blocks = NULL;
static int allocated_blocks_count = 0;
static int allocated_blocks_capacity = 0;

/* ============================================================================
 * Shared page infrastructure for triple indirect testing
 * 
 * To test triple indirect without allocating ~262K blocks (~1GB), we use
 * shared pages. For each layer level, most allocations return the same
 * shared page. This lets us test the logic without the memory overhead.
 * ============================================================================ */
static void *shared_data_page = NULL;           /* Level 0: data blocks */
static void *shared_indirect_page = NULL;       /* Level 1: indirect pointer blocks */
static void *shared_double_indirect_page = NULL;/* Level 2: double indirect blocks */
static bool shared_pages_active = false;
static int shared_page_free_count = 0;  /* Count kfree calls on shared pages */

static void track_allocation(void *ptr) {
    if (allocated_blocks_count >= allocated_blocks_capacity) {
        int new_capacity = allocated_blocks_capacity == 0 ? 256 : allocated_blocks_capacity * 2;
        allocated_blocks = realloc(allocated_blocks, new_capacity * sizeof(void *));
        allocated_blocks_capacity = new_capacity;
    }
    allocated_blocks[allocated_blocks_count++] = ptr;
}

static bool is_tracked_allocation(void *ptr) {
    for (int i = 0; i < allocated_blocks_count; i++) {
        if (allocated_blocks[i] == ptr) {
            return true;
        }
    }
    return false;
}

static bool is_shared_page(void *ptr) {
    return ptr == shared_data_page || 
           ptr == shared_indirect_page || 
           ptr == shared_double_indirect_page;
}

static void untrack_allocation(void *ptr) {
    for (int i = 0; i < allocated_blocks_count; i++) {
        if (allocated_blocks[i] == ptr) {
            allocated_blocks[i] = allocated_blocks[--allocated_blocks_count];
            return;
        }
    }
}

static void reset_mock_state(void) {
    kalloc_call_count = 0;
    kfree_call_count = 0;
    kalloc_fail_after = -1;
    shared_page_free_count = 0;
    shared_pages_active = false;
    /* Free any remaining tracked allocations */
    for (int i = 0; i < allocated_blocks_count; i++) {
        if (allocated_blocks[i] != NULL) {
            free(allocated_blocks[i]);
        }
    }
    allocated_blocks_count = 0;
    /* Free shared pages if allocated */
    if (shared_data_page != NULL) {
        free(shared_data_page);
        shared_data_page = NULL;
    }
    if (shared_indirect_page != NULL) {
        free(shared_indirect_page);
        shared_indirect_page = NULL;
    }
    if (shared_double_indirect_page != NULL) {
        free(shared_double_indirect_page);
        shared_double_indirect_page = NULL;
    }
}

/* ============================================================================
 * Stub implementations required by truncate.c
 * ============================================================================ */
void *kalloc(void) {
    kalloc_call_count++;
    if (kalloc_fail_after >= 0 && kalloc_call_count > kalloc_fail_after) {
        return NULL;
    }
    void *ptr = malloc(PAGE_SIZE);
    if (ptr != NULL) {
        memset(ptr, 0, PAGE_SIZE);
        track_allocation(ptr);
    }
    return ptr;
}

void kfree(void *ptr) {
    kfree_call_count++;
    if (ptr == NULL) {
        return;
    }
    /* Handle shared pages - don't actually free, just count */
    if (shared_pages_active && is_shared_page(ptr)) {
        shared_page_free_count++;
        return;
    }
    assert_true(is_tracked_allocation(ptr));
    untrack_allocation(ptr);
    free(ptr);
}

__attribute__((noreturn))
void __panic_impl(const char *type, const char *fmt, ...) {
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
 * ============================================================================ */
#include "../../kernel/vfs/tmpfs/truncate.c"

/* ============================================================================
 * Test fixtures
 * ============================================================================ */
static struct tmpfs_inode *create_test_inode(void) {
    struct tmpfs_inode *inode = calloc(1, sizeof(struct tmpfs_inode));
    inode->vfs_inode.size = 0;
    inode->vfs_inode.n_blocks = 0;
    inode->embedded = true;
    return inode;
}

static void destroy_test_inode(struct tmpfs_inode *inode) {
    free(inode);
}

static int test_setup(void **state) {
    (void)state;
    reset_mock_state();
    return 0;
}

static int test_teardown(void **state) {
    (void)state;
    /* Verify no memory leaks */
    if (allocated_blocks_count != 0) {
        print_message("Memory leak detected: %d blocks still allocated\n", allocated_blocks_count);
    }
    assert_int_equal(allocated_blocks_count, 0);
    reset_mock_state();
    return 0;
}

/* ============================================================================
 * Positive tests for __tmpfs_truncate
 * ============================================================================ */

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

/* Test truncate grow from embedded to one block */
static void test_truncate_grow_embedded_to_one_block(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 10;
    inode->embedded = true;

    loff_t new_size = TMPFS_INODE_EMBEDDED_DATA_LEN + 100;
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, new_size);
    assert_false(inode->embedded);
    assert_int_equal(inode->vfs_inode.n_blocks, 1);

    /* Cleanup: shrink back to free the block */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);

    destroy_test_inode(inode);
}

/* Test truncate grow to multiple direct blocks */
static void test_truncate_grow_direct_blocks(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 10;
    inode->embedded = true;

    loff_t new_size = PAGE_SIZE * 5;
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, new_size);
    assert_false(inode->embedded);
    assert_int_equal(inode->vfs_inode.n_blocks, 5);

    /* Verify direct blocks are allocated */
    for (int i = 0; i < 5; i++) {
        assert_non_null(inode->file.direct[i]);
    }

    /* Cleanup */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, 0);

    destroy_test_inode(inode);
}

/* Test truncate shrink from multiple direct blocks */
static void test_truncate_shrink_direct_blocks(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 0;
    inode->embedded = true;

    /* First grow to 10 blocks */
    loff_t new_size = PAGE_SIZE * 10;
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, 10);

    /* Now shrink to 3 blocks */
    new_size = PAGE_SIZE * 3;
    ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, new_size);
    assert_int_equal(inode->vfs_inode.n_blocks, 3);

    /* Verify only first 3 blocks remain */
    for (int i = 0; i < 3; i++) {
        assert_non_null(inode->file.direct[i]);
    }
    for (int i = 3; i < 10; i++) {
        assert_null(inode->file.direct[i]);
    }

    /* Cleanup */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);

    destroy_test_inode(inode);
}

/* Test truncate shrink to zero from direct blocks */
static void test_truncate_shrink_to_zero_direct(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 0;
    inode->embedded = true;

    /* First grow to 5 blocks */
    loff_t new_size = PAGE_SIZE * 5;
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);

    int kfree_before = kfree_call_count;

    /* Now shrink to 0 */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.size, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, 0);

    /* Verify all blocks were freed (5 data blocks) */
    assert_int_equal(kfree_call_count - kfree_before, 5);

    destroy_test_inode(inode);
}

/* ============================================================================
 * Negative tests for __tmpfs_truncate
 * ============================================================================ */

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

/* Test truncate with allocation failure during grow */
static void test_truncate_grow_alloc_failure(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 0;
    inode->embedded = true;

    /* Make kalloc fail after the first allocation (migration block) */
    kalloc_fail_after = 1;

    loff_t new_size = PAGE_SIZE * 5;
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, -ENOMEM);
    /* Size should not have changed on failure */
    assert_int_equal(inode->vfs_inode.size, 0);

    destroy_test_inode(inode);
}

/* Test truncate grow with allocation failure after some blocks allocated */
static void test_truncate_grow_partial_alloc_failure(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 0;
    inode->embedded = true;

    /* First successful grow to establish non-embedded state */
    int ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * 2);
    assert_int_equal(ret, 0);

    int blocks_before = inode->vfs_inode.n_blocks;

    /* Now fail after 2 more allocations */
    kalloc_fail_after = kalloc_call_count + 2;

    ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * 10);
    assert_int_equal(ret, -ENOMEM);
    /* Size should not have changed */
    assert_int_equal(inode->vfs_inode.size, PAGE_SIZE * 2);
    assert_int_equal(inode->vfs_inode.n_blocks, blocks_before);

    /* Cleanup */
    kalloc_fail_after = -1;
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);

    destroy_test_inode(inode);
}

/* ============================================================================
 * Tests for __tmpfs_truncate_free_blocks
 * ============================================================================ */

/* Test freeing a simple range of blocks at level 0 */
static void test_free_blocks_level0_simple(void **state) {
    (void)state;
    void *blocks[8];
    
    /* Allocate blocks */
    for (int i = 0; i < 8; i++) {
        blocks[i] = kalloc();
        assert_non_null(blocks[i]);
    }

    int kfree_before = kfree_call_count;
    __tmpfs_truncate_free_blocks(blocks, 2, 6, 0);

    /* Verify blocks 2-5 were freed */
    assert_int_equal(kfree_call_count - kfree_before, 4);
    for (int i = 0; i < 2; i++) {
        assert_non_null(blocks[i]);
    }
    for (int i = 2; i < 6; i++) {
        assert_null(blocks[i]);
    }
    for (int i = 6; i < 8; i++) {
        assert_non_null(blocks[i]);
    }

    /* Cleanup remaining blocks */
    for (int i = 0; i < 8; i++) {
        if (blocks[i] != NULL) {
            kfree(blocks[i]);
        }
    }
}

/* Test freeing all blocks at level 0 */
static void test_free_blocks_level0_all(void **state) {
    (void)state;
    void *blocks[8];
    
    /* Allocate blocks */
    for (int i = 0; i < 8; i++) {
        blocks[i] = kalloc();
        assert_non_null(blocks[i]);
    }

    int kfree_before = kfree_call_count;
    __tmpfs_truncate_free_blocks(blocks, 0, 8, 0);

    assert_int_equal(kfree_call_count - kfree_before, 8);
    for (int i = 0; i < 8; i++) {
        assert_null(blocks[i]);
    }
}

/* Test freeing with NULL blocks at level 0 */
static void test_free_blocks_level0_with_nulls(void **state) {
    (void)state;
    void *blocks[8] = {0};
    
    /* Only allocate some blocks */
    blocks[0] = kalloc();
    blocks[2] = kalloc();
    blocks[4] = kalloc();

    int kfree_before = kfree_call_count;
    __tmpfs_truncate_free_blocks(blocks, 0, 5, 0);

    /* Only the non-NULL blocks should be freed */
    assert_int_equal(kfree_call_count - kfree_before, 3);
    for (int i = 0; i < 5; i++) {
        assert_null(blocks[i]);
    }
}

/* Test freeing empty range at level 0 */
static void test_free_blocks_level0_empty_range(void **state) {
    (void)state;
    void *blocks[4];
    
    for (int i = 0; i < 4; i++) {
        blocks[i] = kalloc();
    }

    int kfree_before = kfree_call_count;
    __tmpfs_truncate_free_blocks(blocks, 2, 2, 0);

    /* No blocks should be freed */
    assert_int_equal(kfree_call_count - kfree_before, 0);
    for (int i = 0; i < 4; i++) {
        assert_non_null(blocks[i]);
    }

    /* Cleanup */
    for (int i = 0; i < 4; i++) {
        kfree(blocks[i]);
    }
}

/* ============================================================================
 * Tests for __tmpfs_do_shrink_blocks
 * ============================================================================ */

/* Test shrink with no blocks to free */
static void test_shrink_blocks_no_change(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->embedded = false;
    inode->vfs_inode.n_blocks = 5;

    int kfree_before = kfree_call_count;
    __tmpfs_do_shrink_blocks(inode, 5, 5);

    assert_int_equal(kfree_call_count - kfree_before, 0);

    destroy_test_inode(inode);
}

/* ============================================================================
 * Edge case tests
 * ============================================================================ */

/* Test truncate at exact block boundary */
static void test_truncate_exact_block_boundary(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 0;
    inode->embedded = true;

    /* Grow to exactly 2 pages */
    loff_t new_size = PAGE_SIZE * 2;
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, 2);

    /* Shrink to exactly 1 page */
    new_size = PAGE_SIZE;
    ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, 1);

    /* Cleanup */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);

    destroy_test_inode(inode);
}

/* Test truncate at one byte before block boundary */
static void test_truncate_one_byte_before_boundary(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 0;
    inode->embedded = true;

    /* Grow to 2 pages */
    int ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * 2);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, 2);

    /* Shrink to one byte before page boundary (needs 1 block) */
    loff_t new_size = PAGE_SIZE - 1;
    ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, 1);

    /* Cleanup */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);

    destroy_test_inode(inode);
}

/* Test truncate at one byte after block boundary */
static void test_truncate_one_byte_after_boundary(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 0;
    inode->embedded = true;

    /* Grow to just past one page boundary (needs 2 blocks) */
    loff_t new_size = PAGE_SIZE + 1;
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, 2);

    /* Cleanup */
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
    
    /* Cycle 1: grow then shrink */
    ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * 5);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, 5);

    ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * 2);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, 2);

    /* Cycle 2: grow again */
    ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * 8);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, 8);

    /* Cycle 3: shrink to 0 */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, 0);

    destroy_test_inode(inode);
}

/* Test growing to all direct blocks */
static void test_truncate_all_direct_blocks(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 0;
    inode->embedded = true;

    /* Grow to use all direct blocks */
    loff_t new_size = PAGE_SIZE * TMPFS_INODE_DBLOCKS;
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, (int)TMPFS_INODE_DBLOCKS);

    /* Verify all direct blocks are allocated */
    for (int i = 0; i < (int)TMPFS_INODE_DBLOCKS; i++) {
        assert_non_null(inode->file.direct[i]);
    }

    /* Cleanup */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);

    destroy_test_inode(inode);
}

/* Test growing to indirect blocks */
static void test_truncate_indirect_blocks(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 0;
    inode->embedded = true;

    /* Grow beyond direct blocks to use indirect blocks */
    loff_t new_size = PAGE_SIZE * (TMPFS_INODE_DBLOCKS + 10);
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, (int)(TMPFS_INODE_DBLOCKS + 10));

    /* Verify indirect block is allocated */
    assert_non_null(inode->file.indirect);

    /* Cleanup */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, 0);

    destroy_test_inode(inode);
}

/* Test shrinking from indirect to direct */
static void test_truncate_shrink_indirect_to_direct(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 0;
    inode->embedded = true;

    /* Grow to indirect blocks */
    loff_t new_size = PAGE_SIZE * (TMPFS_INODE_DBLOCKS + 10);
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_non_null(inode->file.indirect);

    /* Shrink back to direct only */
    new_size = PAGE_SIZE * 5;
    ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, 5);

    /* Cleanup */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);

    destroy_test_inode(inode);
}

/* Test allocate blocks returns ENOMEM on first allocation */
static void test_allocate_blocks_first_alloc_fails(void **state) {
    (void)state;
    void *blocks[4] = {0};
    
    kalloc_fail_after = 0;  /* Fail immediately */
    
    int ret = __tmpfs_truncate_allocate_blocks(blocks, 0, 4, 0);
    assert_int_equal(ret, -ENOMEM);
    
    /* No blocks should have been allocated */
    for (int i = 0; i < 4; i++) {
        assert_null(blocks[i]);
    }
}

/* Test level 1 free blocks (indirect) */
static void test_free_blocks_level1(void **state) {
    (void)state;
    
    /* Create an indirect block array */
    void **indirect = calloc(TMPFS_INODE_INDRECT_ITEMS, sizeof(void *));
    assert_non_null(indirect);
    
    /* Allocate a few data blocks via the indirect table */
    for (int i = 0; i < 5; i++) {
        indirect[i] = kalloc();
        assert_non_null(indirect[i]);
    }
    
    int kfree_before = kfree_call_count;
    
    /* Free blocks 1-4 (indices 1, 2, 3 - 3 blocks) */
    __tmpfs_truncate_free_blocks(indirect, 1, 4, 0);
    
    assert_int_equal(kfree_call_count - kfree_before, 3);
    assert_non_null(indirect[0]);  /* Should remain */
    assert_null(indirect[1]);
    assert_null(indirect[2]);
    assert_null(indirect[3]);
    assert_non_null(indirect[4]);  /* Should remain */
    
    /* Cleanup */
    kfree(indirect[0]);
    kfree(indirect[4]);
    free(indirect);
}

/* ============================================================================
 * Comprehensive grow tests - Direct blocks layer
 * ============================================================================ */

/* Test grow from embedded to partial direct blocks */
static void test_grow_embedded_to_partial_direct(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 10;
    inode->embedded = true;

    /* Grow to 15 blocks (less than TMPFS_INODE_DBLOCKS=32) */
    loff_t new_size = PAGE_SIZE * 15;
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, 15);
    assert_false(inode->embedded);

    /* Verify blocks 0-14 are allocated */
    for (int i = 0; i < 15; i++) {
        assert_non_null(inode->file.direct[i]);
    }
    /* Verify blocks 15-31 are NOT allocated */
    for (int i = 15; i < (int)TMPFS_INODE_DBLOCKS; i++) {
        assert_null(inode->file.direct[i]);
    }

    /* Cleanup */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* Test grow from partial direct to full direct */
static void test_grow_partial_direct_to_full_direct(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* Start with 10 blocks */
    int ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * 10);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, 10);

    /* Grow to full 32 direct blocks */
    loff_t new_size = PAGE_SIZE * TMPFS_INODE_DBLOCKS;
    ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, (int)TMPFS_INODE_DBLOCKS);

    /* Verify all direct blocks allocated */
    for (int i = 0; i < (int)TMPFS_INODE_DBLOCKS; i++) {
        assert_non_null(inode->file.direct[i]);
    }

    /* Cleanup */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* Test grow failure at first direct block allocation */
static void test_grow_direct_fail_first_block(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 10;
    inode->embedded = true;

    /* Fail on first kalloc (migration block) */
    kalloc_fail_after = 0;

    loff_t new_size = PAGE_SIZE * 5;
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, -ENOMEM);
    assert_int_equal(inode->vfs_inode.size, 10);
    assert_true(inode->embedded);

    destroy_test_inode(inode);
}

/* Test grow failure at middle direct block allocation */
static void test_grow_direct_fail_middle_block(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 10;
    inode->embedded = true;

    /* Fail after migration block + 3 data blocks = 4 allocations */
    kalloc_fail_after = 4;

    loff_t new_size = PAGE_SIZE * 10;
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, -ENOMEM);
    /* Size unchanged */
    assert_int_equal(inode->vfs_inode.size, 10);
    /* After failure, file is no longer embedded (migration happened before failure) */
    /* but we don't migrate back to reduce thrashing, so n_blocks = 1 */
    assert_false(inode->embedded);
    assert_int_equal(inode->vfs_inode.n_blocks, 1);

    /* Cleanup - need to shrink to 0 to free the migration block */
    kalloc_fail_after = -1;
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);

    destroy_test_inode(inode);
}

/* Test grow failure at last direct block allocation */
static void test_grow_direct_fail_last_block(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* First grow to 5 blocks successfully */
    int ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * 5);
    assert_int_equal(ret, 0);
    int size_before = inode->vfs_inode.size;
    int blocks_before = inode->vfs_inode.n_blocks;

    /* Now fail on the last of 5 more blocks (10th block total) */
    kalloc_fail_after = kalloc_call_count + 4;  /* Allow 4 more, fail on 5th */

    ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * 10);
    assert_int_equal(ret, -ENOMEM);
    assert_int_equal(inode->vfs_inode.size, size_before);
    assert_int_equal(inode->vfs_inode.n_blocks, blocks_before);

    /* Cleanup */
    kalloc_fail_after = -1;
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* ============================================================================
 * Comprehensive grow tests - Indirect blocks layer
 * ============================================================================ */

/* Test grow from full direct to partial indirect */
static void test_grow_full_direct_to_partial_indirect(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* First fill all direct blocks */
    int ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * TMPFS_INODE_DBLOCKS);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, (int)TMPFS_INODE_DBLOCKS);

    /* Grow to use 10 indirect entries */
    loff_t new_size = PAGE_SIZE * (TMPFS_INODE_DBLOCKS + 10);
    ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, (int)(TMPFS_INODE_DBLOCKS + 10));
    assert_non_null(inode->file.indirect);

    /* Cleanup */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* Test grow from partial indirect to full indirect */
static void test_grow_partial_indirect_to_full_indirect(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* First grow to use some indirect entries */
    loff_t partial_size = PAGE_SIZE * (TMPFS_INODE_DBLOCKS + 10);
    int ret = __tmpfs_truncate(&inode->vfs_inode, partial_size);
    assert_int_equal(ret, 0);

    /* Grow to full indirect (all TMPFS_INODE_INDRECT_ITEMS entries) */
    loff_t full_indirect_size = PAGE_SIZE * TMPFS_INODE_DINDRECT_START;
    ret = __tmpfs_truncate(&inode->vfs_inode, full_indirect_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, (int)TMPFS_INODE_DINDRECT_START);

    /* Cleanup */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* Test grow failure when allocating indirect pointer block */
static void test_grow_indirect_fail_pointer_block(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* First fill all direct blocks */
    int ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * TMPFS_INODE_DBLOCKS);
    assert_int_equal(ret, 0);
    int size_before = inode->vfs_inode.size;
    int blocks_before = inode->vfs_inode.n_blocks;

    /* Fail on the indirect pointer allocation (first kalloc after direct is full) */
    kalloc_fail_after = kalloc_call_count;

    loff_t new_size = PAGE_SIZE * (TMPFS_INODE_DBLOCKS + 5);
    ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, -ENOMEM);
    assert_int_equal(inode->vfs_inode.size, size_before);
    assert_int_equal(inode->vfs_inode.n_blocks, blocks_before);
    assert_null(inode->file.indirect);

    /* Cleanup */
    kalloc_fail_after = -1;
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* Test grow failure at first indirect data block */
static void test_grow_indirect_fail_first_data_block(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* First fill all direct blocks */
    int ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * TMPFS_INODE_DBLOCKS);
    assert_int_equal(ret, 0);
    int size_before = inode->vfs_inode.size;
    int blocks_before = inode->vfs_inode.n_blocks;

    /* Allow indirect pointer allocation, fail on first data block */
    kalloc_fail_after = kalloc_call_count + 1;

    loff_t new_size = PAGE_SIZE * (TMPFS_INODE_DBLOCKS + 5);
    ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, -ENOMEM);
    assert_int_equal(inode->vfs_inode.size, size_before);
    assert_int_equal(inode->vfs_inode.n_blocks, blocks_before);

    /* Cleanup */
    kalloc_fail_after = -1;
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* Test grow failure at middle indirect data block */
static void test_grow_indirect_fail_middle_data_block(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* First fill all direct blocks */
    int ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * TMPFS_INODE_DBLOCKS);
    assert_int_equal(ret, 0);
    int size_before = inode->vfs_inode.size;
    int blocks_before = inode->vfs_inode.n_blocks;

    /* Allow indirect pointer + 3 data blocks, fail on 4th */
    kalloc_fail_after = kalloc_call_count + 4;

    loff_t new_size = PAGE_SIZE * (TMPFS_INODE_DBLOCKS + 10);
    ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, -ENOMEM);
    assert_int_equal(inode->vfs_inode.size, size_before);
    assert_int_equal(inode->vfs_inode.n_blocks, blocks_before);

    /* Cleanup */
    kalloc_fail_after = -1;
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* ============================================================================
 * Comprehensive grow tests - Double indirect blocks layer
 * ============================================================================ */

/* Test grow from full indirect to partial double indirect */
static void test_grow_full_indirect_to_partial_double(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* First fill up to end of indirect */
    int ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * TMPFS_INODE_DINDRECT_START);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, (int)TMPFS_INODE_DINDRECT_START);

    /* Grow to use 10 double indirect entries */
    loff_t new_size = PAGE_SIZE * (TMPFS_INODE_DINDRECT_START + 10);
    ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, (int)(TMPFS_INODE_DINDRECT_START + 10));
    assert_non_null(inode->file.double_indirect);

    /* Cleanup */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* Test grow spanning multiple inner indirect blocks within double indirect */
static void test_grow_double_indirect_multiple_inner(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* Fill up to end of indirect */
    int ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * TMPFS_INODE_DINDRECT_START);
    assert_int_equal(ret, 0);

    /* Grow to span 3 inner indirect blocks (3 * TMPFS_INODE_INDRECT_ITEMS entries) */
    loff_t new_size = PAGE_SIZE * (TMPFS_INODE_DINDRECT_START + 3 * TMPFS_INODE_INDRECT_ITEMS);
    ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, 
                     (int)(TMPFS_INODE_DINDRECT_START + 3 * TMPFS_INODE_INDRECT_ITEMS));

    /* Cleanup */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* Test grow failure when allocating double indirect pointer block */
static void test_grow_double_indirect_fail_pointer_block(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* Fill up to end of indirect */
    int ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * TMPFS_INODE_DINDRECT_START);
    assert_int_equal(ret, 0);
    int size_before = inode->vfs_inode.size;
    int blocks_before = inode->vfs_inode.n_blocks;

    /* Fail on the double indirect pointer allocation */
    kalloc_fail_after = kalloc_call_count;

    loff_t new_size = PAGE_SIZE * (TMPFS_INODE_DINDRECT_START + 5);
    ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, -ENOMEM);
    assert_int_equal(inode->vfs_inode.size, size_before);
    assert_int_equal(inode->vfs_inode.n_blocks, blocks_before);
    assert_null(inode->file.double_indirect);

    /* Cleanup */
    kalloc_fail_after = -1;
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* Test grow failure when allocating first inner indirect block */
static void test_grow_double_indirect_fail_first_inner(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* Fill up to end of indirect */
    int ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * TMPFS_INODE_DINDRECT_START);
    assert_int_equal(ret, 0);
    int size_before = inode->vfs_inode.size;
    int blocks_before = inode->vfs_inode.n_blocks;

    /* Allow double indirect pointer, fail on first inner indirect block */
    kalloc_fail_after = kalloc_call_count + 1;

    loff_t new_size = PAGE_SIZE * (TMPFS_INODE_DINDRECT_START + 5);
    ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, -ENOMEM);
    assert_int_equal(inode->vfs_inode.size, size_before);
    assert_int_equal(inode->vfs_inode.n_blocks, blocks_before);

    /* Cleanup */
    kalloc_fail_after = -1;
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* Test grow failure at data block within double indirect */
static void test_grow_double_indirect_fail_data_block(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* Fill up to end of indirect */
    int ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * TMPFS_INODE_DINDRECT_START);
    assert_int_equal(ret, 0);
    int size_before = inode->vfs_inode.size;
    int blocks_before = inode->vfs_inode.n_blocks;

    /* Allow double indirect pointer + first inner indirect + 3 data blocks, fail on 4th */
    kalloc_fail_after = kalloc_call_count + 5;

    loff_t new_size = PAGE_SIZE * (TMPFS_INODE_DINDRECT_START + 10);
    ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, -ENOMEM);
    assert_int_equal(inode->vfs_inode.size, size_before);
    assert_int_equal(inode->vfs_inode.n_blocks, blocks_before);

    /* Cleanup */
    kalloc_fail_after = -1;
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* Test grow failure when allocating second inner indirect block */
static void test_grow_double_indirect_fail_second_inner(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* Fill up to end of indirect + one full inner indirect block worth of data */
    loff_t initial_size = PAGE_SIZE * (TMPFS_INODE_DINDRECT_START + TMPFS_INODE_INDRECT_ITEMS);
    int ret = __tmpfs_truncate(&inode->vfs_inode, initial_size);
    assert_int_equal(ret, 0);
    int size_before = inode->vfs_inode.size;
    int blocks_before = inode->vfs_inode.n_blocks;

    /* Fail on the second inner indirect block allocation */
    kalloc_fail_after = kalloc_call_count;

    loff_t new_size = PAGE_SIZE * (TMPFS_INODE_DINDRECT_START + TMPFS_INODE_INDRECT_ITEMS + 10);
    ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, -ENOMEM);
    assert_int_equal(inode->vfs_inode.size, size_before);
    assert_int_equal(inode->vfs_inode.n_blocks, blocks_before);

    /* Cleanup */
    kalloc_fail_after = -1;
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* ============================================================================
 * Triple indirect tests with shared pages
 * 
 * To test triple indirect without allocating ~262K blocks (~1GB memory), we use
 * shared pages. All data blocks point to shared_data_page, all indirect blocks
 * point to shared_indirect_page, and all double-indirect blocks point to
 * shared_double_indirect_page. This lets us verify the freeing logic counts
 * without the massive memory overhead.
 * ============================================================================ */

/* Initialize shared pages for triple indirect testing */
static void init_shared_pages(void) {
    shared_data_page = malloc(PAGE_SIZE);
    shared_indirect_page = malloc(PAGE_SIZE);
    shared_double_indirect_page = malloc(PAGE_SIZE);
    memset(shared_data_page, 0, PAGE_SIZE);
    memset(shared_indirect_page, 0, PAGE_SIZE);
    memset(shared_double_indirect_page, 0, PAGE_SIZE);
    shared_pages_active = true;
    shared_page_free_count = 0;
}

/* 
 * Setup a triple indirect structure with shared pages.
 * 
 * This fills the structure as if it has n_triple_blocks data blocks in the
 * triple indirect layer (beyond TMPFS_INODE_TINDRECT_START).
 * 
 * All data blocks point to shared_data_page.
 * All indirect blocks point to shared_indirect_page (which itself points to shared_data_page).
 * All double-indirect blocks point to shared_double_indirect_page.
 * 
 * The shared_indirect_page has all its entries pointing to shared_data_page.
 * The shared_double_indirect_page has all its entries pointing to shared_indirect_page.
 */
static void setup_triple_indirect_with_shared_pages(
    struct tmpfs_inode *inode,
    int n_triple_blocks  /* Number of blocks in triple indirect layer */
) {
    init_shared_pages();
    
    /* Fill shared_indirect_page: all entries point to shared_data_page */
    void **indirect_entries = (void **)shared_indirect_page;
    for (int i = 0; i < (int)TMPFS_INODE_INDRECT_ITEMS; i++) {
        indirect_entries[i] = shared_data_page;
    }
    
    /* Fill shared_double_indirect_page: all entries point to shared_indirect_page */
    void **double_indirect_entries = (void **)shared_double_indirect_page;
    for (int i = 0; i < (int)TMPFS_INODE_INDRECT_ITEMS; i++) {
        double_indirect_entries[i] = shared_indirect_page;
    }
    
    /* Allocate the triple indirect pointer block (this one is unique) */
    inode->file.triple_indirect = (void ****)malloc(PAGE_SIZE);
    memset(inode->file.triple_indirect, 0, PAGE_SIZE);
    track_allocation(inode->file.triple_indirect);
    
    /* Calculate how many double-indirect blocks we need */
    int blocks_per_double = TMPFS_INODE_DINDRECT_ITEMS; /* 512 * 512 */
    int n_double_indirects = (n_triple_blocks + blocks_per_double - 1) / blocks_per_double;
    if (n_double_indirects > (int)TMPFS_INODE_INDRECT_ITEMS) {
        n_double_indirects = (int)TMPFS_INODE_INDRECT_ITEMS;
    }
    
    /* Fill triple_indirect with pointers to shared_double_indirect_page */
    for (int i = 0; i < n_double_indirects; i++) {
        inode->file.triple_indirect[i] = (void ***)shared_double_indirect_page;
    }
    
    /* Set inode state */
    inode->embedded = false;
    inode->vfs_inode.n_blocks = TMPFS_INODE_TINDRECT_START + n_triple_blocks;
    inode->vfs_inode.size = (loff_t)inode->vfs_inode.n_blocks * PAGE_SIZE;
    
    /* Also need to fill direct, indirect, and double_indirect to represent full file */
    /* For simplicity, we'll allocate minimal real blocks for these lower layers */
    /* and use shared pages concept - but actually for the test we just set them to NULL */
    /* since __tmpfs_do_shrink_blocks checks block_cnt against layer boundaries */
    
    /* Actually, to properly test shrink, we need the lower layers filled too */
    /* Let's allocate the layer pointer blocks but use shared pages for contents */
    
    /* Direct blocks - use shared_data_page for all */
    for (int i = 0; i < (int)TMPFS_INODE_DBLOCKS; i++) {
        inode->file.direct[i] = shared_data_page;
    }
    
    /* Indirect block pointer + contents */
    inode->file.indirect = (void **)malloc(PAGE_SIZE);
    memset(inode->file.indirect, 0, PAGE_SIZE);
    track_allocation(inode->file.indirect);
    for (int i = 0; i < (int)TMPFS_INODE_INDRECT_ITEMS; i++) {
        inode->file.indirect[i] = shared_data_page;
    }
    
    /* Double indirect block pointer + contents (use shared for inner) */
    inode->file.double_indirect = (void ***)malloc(PAGE_SIZE);
    memset(inode->file.double_indirect, 0, PAGE_SIZE);
    track_allocation(inode->file.double_indirect);
    for (int i = 0; i < (int)TMPFS_INODE_INDRECT_ITEMS; i++) {
        inode->file.double_indirect[i] = (void **)shared_indirect_page;
    }
}

/* Test shrinking a triple indirect file to zero using shared pages */
static void test_shrink_triple_indirect_to_zero_shared(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* Setup with 1000 blocks in triple indirect (arbitrary, tests the logic) */
    int n_triple_blocks = 1000;
    setup_triple_indirect_with_shared_pages(inode, n_triple_blocks);
    
    int initial_blocks = inode->vfs_inode.n_blocks;
    assert_int_equal(initial_blocks, (int)TMPFS_INODE_TINDRECT_START + n_triple_blocks);
    
    /* Remember how many real allocations we have (the 3 pointer blocks) */
    int real_allocs_before = allocated_blocks_count;
    assert_int_equal(real_allocs_before, 3);  /* triple, indirect, double_indirect ptrs */
    
    /* Shrink to zero */
    int ret = __tmpfs_truncate_shrink(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, 0);
    
    /* Verify the layer pointers were freed */
    assert_null(inode->file.triple_indirect);
    assert_null(inode->file.double_indirect);
    assert_null(inode->file.indirect);
    
    /* Real allocations should be freed */
    assert_int_equal(allocated_blocks_count, 0);
    
    /* Shared pages should have been "freed" many times (counted but not actually freed) */
    /* The exact count depends on the structure traversal */
    assert_true(shared_page_free_count > 0);
    
    destroy_test_inode(inode);
}

/* Test shrinking from triple indirect to double indirect boundary */
static void test_shrink_triple_to_double_indirect_shared(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* Setup with 1000 blocks in triple indirect */
    int n_triple_blocks = 1000;
    setup_triple_indirect_with_shared_pages(inode, n_triple_blocks);
    
    /* Shrink to exactly TMPFS_INODE_TINDRECT_START (double indirect boundary) */
    loff_t new_size = (loff_t)TMPFS_INODE_TINDRECT_START * PAGE_SIZE;
    int ret = __tmpfs_truncate_shrink(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, (int)TMPFS_INODE_TINDRECT_START);
    
    /* Triple indirect pointer should be freed */
    assert_null(inode->file.triple_indirect);
    
    /* Double indirect should still exist */
    assert_non_null(inode->file.double_indirect);
    
    /* Cleanup - shrink rest to 0 */
    ret = __tmpfs_truncate_shrink(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    
    destroy_test_inode(inode);
}

/* Test shrinking partial triple indirect (within the layer) */
static void test_shrink_partial_triple_indirect_shared(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* Setup with 10000 blocks in triple indirect */
    int n_triple_blocks = 10000;
    setup_triple_indirect_with_shared_pages(inode, n_triple_blocks);
    
    int initial_free_count = shared_page_free_count;
    
    /* Shrink to 5000 blocks in triple indirect layer */
    int new_triple_blocks = 5000;
    loff_t new_size = (loff_t)(TMPFS_INODE_TINDRECT_START + new_triple_blocks) * PAGE_SIZE;
    int ret = __tmpfs_truncate_shrink(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, (int)TMPFS_INODE_TINDRECT_START + new_triple_blocks);
    
    /* Triple indirect pointer should still exist */
    assert_non_null(inode->file.triple_indirect);
    
    /* Some shared pages should have been "freed" */
    assert_true(shared_page_free_count > initial_free_count);
    
    /* Cleanup */
    ret = __tmpfs_truncate_shrink(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    
    destroy_test_inode(inode);
}

/* Test that free counts are reasonable for triple indirect structure */
static void test_triple_indirect_free_counts_shared(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* Setup with exactly 512 blocks in triple indirect (one full indirect block) */
    /* This avoids the shared page corruption issue where multiple indirect blocks
     * would point to the same shared_indirect_page */
    int n_triple_blocks = TMPFS_INODE_INDRECT_ITEMS;  /* 512 */
    setup_triple_indirect_with_shared_pages(inode, n_triple_blocks);
    
    shared_page_free_count = 0;
    kfree_call_count = 0;
    
    /* Shrink just the triple indirect layer (to TINDRECT_START) */
    loff_t new_size = (loff_t)TMPFS_INODE_TINDRECT_START * PAGE_SIZE;
    int ret = __tmpfs_truncate_shrink(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    
    /* 
     * For 512 blocks in triple indirect (exactly one indirect block):
     * - 512 data block frees (shared_data_page)
     * - 1 indirect block free (shared_indirect_page)
     * - 1 double-indirect block free (shared_double_indirect_page)
     * Total: 514 shared frees
     * Plus 1 real free (triple_indirect pointer block)
     */
    int expected_shared_frees = n_triple_blocks + 1 + 1;  /* data + indirect + double */
    
    assert_int_equal(shared_page_free_count, expected_shared_frees);
    
    /* Triple indirect pointer should be freed (real allocation) */
    assert_null(inode->file.triple_indirect);
    
    /* Cleanup remaining layers */
    ret = __tmpfs_truncate_shrink(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    
    destroy_test_inode(inode);
}

/* ============================================================================
 * Cross-layer grow tests
 * ============================================================================ */

/* Test grow spanning from embedded directly to indirect layer */
static void test_grow_embedded_to_indirect(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    inode->vfs_inode.size = 10;
    inode->embedded = true;

    /* Grow directly to indirect range */
    loff_t new_size = PAGE_SIZE * (TMPFS_INODE_DBLOCKS + 5);
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, (int)(TMPFS_INODE_DBLOCKS + 5));
    assert_non_null(inode->file.indirect);

    /* Verify all direct blocks are filled */
    for (int i = 0; i < (int)TMPFS_INODE_DBLOCKS; i++) {
        assert_non_null(inode->file.direct[i]);
    }

    /* Cleanup */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* Test grow spanning from direct to double indirect (skipping through indirect) */
static void test_grow_direct_to_double_indirect(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* Start with 10 direct blocks */
    int ret = __tmpfs_truncate(&inode->vfs_inode, PAGE_SIZE * 10);
    assert_int_equal(ret, 0);

    /* Grow directly to double indirect range */
    loff_t new_size = PAGE_SIZE * (TMPFS_INODE_DINDRECT_START + 5);
    ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, (int)(TMPFS_INODE_DINDRECT_START + 5));
    assert_non_null(inode->file.indirect);
    assert_non_null(inode->file.double_indirect);

    /* Cleanup */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* ============================================================================
 * Cross-layer tests: Double indirect <-> Triple indirect
 * 
 * These tests use the shared page infrastructure for triple indirect portion
 * while allocating real blocks for the lower layers.
 * ============================================================================ */

/*
 * Helper: Setup an inode at exactly the triple indirect boundary
 * (full double indirect, ready to enter triple indirect)
 * Uses shared pages for the double indirect contents to save memory.
 */
static void setup_at_triple_boundary_with_shared(struct tmpfs_inode *inode) {
    init_shared_pages();
    
    /* Fill shared_indirect_page: all entries point to shared_data_page */
    void **indirect_entries = (void **)shared_indirect_page;
    for (int i = 0; i < (int)TMPFS_INODE_INDRECT_ITEMS; i++) {
        indirect_entries[i] = shared_data_page;
    }
    
    /* Set inode state to exactly TMPFS_INODE_TINDRECT_START blocks */
    inode->embedded = false;
    inode->vfs_inode.n_blocks = TMPFS_INODE_TINDRECT_START;
    inode->vfs_inode.size = (loff_t)TMPFS_INODE_TINDRECT_START * PAGE_SIZE;
    
    /* Direct blocks - use shared_data_page for all */
    for (int i = 0; i < (int)TMPFS_INODE_DBLOCKS; i++) {
        inode->file.direct[i] = shared_data_page;
    }
    
    /* Indirect block pointer (real) + contents (shared) */
    inode->file.indirect = (void **)malloc(PAGE_SIZE);
    memset(inode->file.indirect, 0, PAGE_SIZE);
    track_allocation(inode->file.indirect);
    for (int i = 0; i < (int)TMPFS_INODE_INDRECT_ITEMS; i++) {
        inode->file.indirect[i] = shared_data_page;
    }
    
    /* Double indirect block pointer (real) + contents (shared indirects) */
    inode->file.double_indirect = (void ***)malloc(PAGE_SIZE);
    memset(inode->file.double_indirect, 0, PAGE_SIZE);
    track_allocation(inode->file.double_indirect);
    for (int i = 0; i < (int)TMPFS_INODE_INDRECT_ITEMS; i++) {
        inode->file.double_indirect[i] = (void **)shared_indirect_page;
    }
    
    /* No triple indirect yet */
    inode->file.triple_indirect = NULL;
}

/* Test grow from double indirect boundary into triple indirect */
static void test_grow_double_to_triple_indirect(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* Setup at exactly the triple indirect boundary */
    setup_at_triple_boundary_with_shared(inode);
    
    assert_int_equal(inode->vfs_inode.n_blocks, (int)TMPFS_INODE_TINDRECT_START);
    assert_null(inode->file.triple_indirect);
    
    /* Grow into triple indirect range (add 10 blocks) */
    loff_t new_size = PAGE_SIZE * (TMPFS_INODE_TINDRECT_START + 10);
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, (int)(TMPFS_INODE_TINDRECT_START + 10));
    
    /* Triple indirect should now be allocated */
    assert_non_null(inode->file.triple_indirect);
    
    /* Cleanup - shrink to zero */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    
    destroy_test_inode(inode);
}

/* Test shrink from triple indirect back to double indirect boundary */
static void test_shrink_triple_to_double_boundary(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* Setup at triple indirect boundary, then add some triple blocks */
    setup_at_triple_boundary_with_shared(inode);
    
    /* Manually add triple indirect with some blocks (using shared pages) */
    inode->file.triple_indirect = (void ****)malloc(PAGE_SIZE);
    memset(inode->file.triple_indirect, 0, PAGE_SIZE);
    track_allocation(inode->file.triple_indirect);
    
    /* Fill shared_double_indirect_page to point to shared_indirect_page */
    void **double_indirect_entries = (void **)shared_double_indirect_page;
    for (int i = 0; i < (int)TMPFS_INODE_INDRECT_ITEMS; i++) {
        double_indirect_entries[i] = shared_indirect_page;
    }
    
    /* Add one double-indirect block to triple indirect */
    inode->file.triple_indirect[0] = (void ***)shared_double_indirect_page;
    
    /* Update block count to include 512 triple indirect blocks */
    int n_triple_blocks = TMPFS_INODE_INDRECT_ITEMS;  /* 512 */
    inode->vfs_inode.n_blocks = TMPFS_INODE_TINDRECT_START + n_triple_blocks;
    inode->vfs_inode.size = (loff_t)inode->vfs_inode.n_blocks * PAGE_SIZE;
    
    /* Shrink back to exactly the double indirect boundary */
    loff_t new_size = PAGE_SIZE * TMPFS_INODE_TINDRECT_START;
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, (int)TMPFS_INODE_TINDRECT_START);
    
    /* Triple indirect should be freed */
    assert_null(inode->file.triple_indirect);
    
    /* Double indirect should still exist */
    assert_non_null(inode->file.double_indirect);
    
    /* Cleanup */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    
    destroy_test_inode(inode);
}

/* Test grow failure when allocating triple indirect pointer (at boundary) */
static void test_grow_double_to_triple_fail_pointer(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* Setup at exactly the triple indirect boundary */
    setup_at_triple_boundary_with_shared(inode);
    
    int size_before = inode->vfs_inode.size;
    int blocks_before = inode->vfs_inode.n_blocks;
    
    /* Fail on the very first allocation (triple indirect pointer block) */
    kalloc_fail_after = kalloc_call_count;
    
    loff_t new_size = PAGE_SIZE * (TMPFS_INODE_TINDRECT_START + 10);
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, -ENOMEM);
    assert_int_equal(inode->vfs_inode.size, size_before);
    assert_int_equal(inode->vfs_inode.n_blocks, blocks_before);
    
    /* Triple indirect should NOT be allocated */
    assert_null(inode->file.triple_indirect);
    
    /* Cleanup */
    kalloc_fail_after = -1;
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    
    destroy_test_inode(inode);
}

/* Test grow failure when allocating first data block in triple indirect */
static void test_grow_double_to_triple_fail_data(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* Setup at exactly the triple indirect boundary */
    setup_at_triple_boundary_with_shared(inode);
    
    int size_before = inode->vfs_inode.size;
    int blocks_before = inode->vfs_inode.n_blocks;
    
    /* Allow: triple_indirect ptr (1) + double_indirect[0] (1) + indirect[0] (1)
     * Fail on: first data block (4th allocation) */
    kalloc_fail_after = kalloc_call_count + 3;
    
    loff_t new_size = PAGE_SIZE * (TMPFS_INODE_TINDRECT_START + 10);
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, -ENOMEM);
    assert_int_equal(inode->vfs_inode.size, size_before);
    assert_int_equal(inode->vfs_inode.n_blocks, blocks_before);
    
    /* Cleanup */
    kalloc_fail_after = -1;
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    
    destroy_test_inode(inode);
}

/* Test one block past triple indirect boundary (TINDRECT_START + 1) */
static void test_grow_one_past_triple_boundary(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* Setup at exactly the triple indirect boundary */
    setup_at_triple_boundary_with_shared(inode);
    
    /* Grow by exactly 1 block into triple indirect */
    loff_t new_size = PAGE_SIZE * (TMPFS_INODE_TINDRECT_START + 1);
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, (int)(TMPFS_INODE_TINDRECT_START + 1));
    
    /* Triple indirect should be allocated */
    assert_non_null(inode->file.triple_indirect);
    
    /* Cleanup */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    
    destroy_test_inode(inode);
}

/* ============================================================================
 * Boundary edge cases
 * ============================================================================ */

/* Test grow to exactly the direct/indirect boundary */
static void test_grow_exact_indirect_boundary(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* Grow to exactly TMPFS_INODE_DBLOCKS (boundary between direct and indirect) */
    loff_t new_size = PAGE_SIZE * TMPFS_INODE_DBLOCKS;
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, (int)TMPFS_INODE_DBLOCKS);
    /* Should NOT have indirect pointer allocated yet */
    assert_null(inode->file.indirect);

    /* Cleanup */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* Test grow to one block past direct/indirect boundary */
static void test_grow_one_past_indirect_boundary(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* Grow to exactly TMPFS_INODE_DBLOCKS + 1 */
    loff_t new_size = PAGE_SIZE * (TMPFS_INODE_DBLOCKS + 1);
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, (int)(TMPFS_INODE_DBLOCKS + 1));
    /* Now should have indirect pointer */
    assert_non_null(inode->file.indirect);

    /* Cleanup */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* Test grow to exactly the indirect/double-indirect boundary */
static void test_grow_exact_double_indirect_boundary(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* Grow to exactly TMPFS_INODE_DINDRECT_START */
    loff_t new_size = PAGE_SIZE * TMPFS_INODE_DINDRECT_START;
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, (int)TMPFS_INODE_DINDRECT_START);
    /* Should NOT have double indirect pointer yet */
    assert_null(inode->file.double_indirect);

    /* Cleanup */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* Test grow to one block past indirect/double-indirect boundary */
static void test_grow_one_past_double_indirect_boundary(void **state) {
    (void)state;
    struct tmpfs_inode *inode = create_test_inode();
    
    /* Grow to exactly TMPFS_INODE_DINDRECT_START + 1 */
    loff_t new_size = PAGE_SIZE * (TMPFS_INODE_DINDRECT_START + 1);
    int ret = __tmpfs_truncate(&inode->vfs_inode, new_size);
    assert_int_equal(ret, 0);
    assert_int_equal(inode->vfs_inode.n_blocks, (int)(TMPFS_INODE_DINDRECT_START + 1));
    assert_non_null(inode->file.double_indirect);

    /* Cleanup */
    ret = __tmpfs_truncate(&inode->vfs_inode, 0);
    assert_int_equal(ret, 0);
    destroy_test_inode(inode);
}

/* NOTE: test_grow_exact_triple_indirect_boundary, test_grow_one_past_triple_indirect_boundary,
 * and test_grow_to_max_file_size are omitted because they require allocating
 * TMPFS_INODE_TINDRECT_START (~262,688) blocks = ~1GB memory. */

/* ============================================================================
 * Main
 * ============================================================================ */
int main(void) {
    const struct CMUnitTest tests[] = {
        /* Positive tests */
        cmocka_unit_test_setup_teardown(test_truncate_same_size, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_grow_embedded, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_shrink_embedded, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_shrink_to_zero_embedded, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_grow_embedded_to_one_block, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_grow_direct_blocks, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_shrink_direct_blocks, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_shrink_to_zero_direct, test_setup, test_teardown),
        
        /* Negative tests */
        cmocka_unit_test_setup_teardown(test_truncate_exceed_max_size, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_grow_alloc_failure, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_grow_partial_alloc_failure, test_setup, test_teardown),
        
        /* Free blocks tests */
        cmocka_unit_test_setup_teardown(test_free_blocks_level0_simple, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_free_blocks_level0_all, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_free_blocks_level0_with_nulls, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_free_blocks_level0_empty_range, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_free_blocks_level1, test_setup, test_teardown),
        
        /* Shrink blocks tests */
        cmocka_unit_test_setup_teardown(test_shrink_blocks_no_change, test_setup, test_teardown),
        
        /* Edge case tests */
        cmocka_unit_test_setup_teardown(test_truncate_exact_block_boundary, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_one_byte_before_boundary, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_one_byte_after_boundary, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_multiple_cycles, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_all_direct_blocks, test_setup, test_teardown),
        
        /* Indirect block tests */
        cmocka_unit_test_setup_teardown(test_truncate_indirect_blocks, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_truncate_shrink_indirect_to_direct, test_setup, test_teardown),
        
        /* Allocation failure tests */
        cmocka_unit_test_setup_teardown(test_allocate_blocks_first_alloc_fails, test_setup, test_teardown),

        /* ================================================================
         * Comprehensive grow tests - Direct blocks layer
         * ================================================================ */
        cmocka_unit_test_setup_teardown(test_grow_embedded_to_partial_direct, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_grow_partial_direct_to_full_direct, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_grow_direct_fail_first_block, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_grow_direct_fail_middle_block, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_grow_direct_fail_last_block, test_setup, test_teardown),

        /* ================================================================
         * Comprehensive grow tests - Indirect blocks layer
         * ================================================================ */
        cmocka_unit_test_setup_teardown(test_grow_full_direct_to_partial_indirect, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_grow_partial_indirect_to_full_indirect, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_grow_indirect_fail_pointer_block, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_grow_indirect_fail_first_data_block, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_grow_indirect_fail_middle_data_block, test_setup, test_teardown),

        /* ================================================================
         * Comprehensive grow tests - Double indirect blocks layer
         * ================================================================ */
        cmocka_unit_test_setup_teardown(test_grow_full_indirect_to_partial_double, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_grow_double_indirect_multiple_inner, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_grow_double_indirect_fail_pointer_block, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_grow_double_indirect_fail_first_inner, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_grow_double_indirect_fail_data_block, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_grow_double_indirect_fail_second_inner, test_setup, test_teardown),

        /* ================================================================
         * Triple indirect tests with shared pages
         * ================================================================ */
        cmocka_unit_test_setup_teardown(test_shrink_triple_indirect_to_zero_shared, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_shrink_triple_to_double_indirect_shared, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_shrink_partial_triple_indirect_shared, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_triple_indirect_free_counts_shared, test_setup, test_teardown),

        /* ================================================================
         * Cross-layer grow tests
         * ================================================================ */
        cmocka_unit_test_setup_teardown(test_grow_embedded_to_indirect, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_grow_direct_to_double_indirect, test_setup, test_teardown),
        /* NOTE: Cross-layer failure tests omitted - allocation count assumptions complex */

        /* ================================================================
         * Boundary edge cases
         * ================================================================ */
        cmocka_unit_test_setup_teardown(test_grow_exact_indirect_boundary, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_grow_one_past_indirect_boundary, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_grow_exact_double_indirect_boundary, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_grow_one_past_double_indirect_boundary, test_setup, test_teardown),

        /* ================================================================
         * Cross-layer double/triple indirect tests (with shared pages)
         * ================================================================ */
        cmocka_unit_test_setup_teardown(test_grow_double_to_triple_indirect, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_shrink_triple_to_double_boundary, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_grow_double_to_triple_fail_pointer, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_grow_double_to_triple_fail_data, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_grow_one_past_triple_boundary, test_setup, test_teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
