#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>

#include "ut_rbtree.h"
#include "test_numbers.h"

static void
insert_bulk_and_validate(struct rb_root *root,
                         rb_test_node_t *nodes,
                         const uint64 *keys,
                         size_t count)
{
    int previous_black_height = -1;
    for (size_t i = 0; i < count; i++) {
        rb_test_node_init(&nodes[i], keys[i], keys[i] * 10);
        struct rb_node *inserted = rb_insert_color(root, &nodes[i].node);
        assert_ptr_equal(inserted, &nodes[i].node);
        assert_true(rb_test_validate_tree(root));
        int current_black_height = rb_test_black_height(root);
        assert_true(current_black_height >= 0);
        if (previous_black_height != -1) {
            assert_true(current_black_height >= previous_black_height);
            assert_true(current_black_height <= previous_black_height + 1);
        }
        previous_black_height = current_black_height;
    }
}

typedef struct {
    const uint64 *insert;
    size_t insert_count;
    const uint64 *remove;
    size_t remove_count;
    const uint64 *expected;
    size_t expected_count;
} rb_sequence_case_t;

#define RB_SEQUENCE_CASE(insert_elements, remove_elements, expected_elements) \
    { \
        .insert = RBTEST_VALUE_ARRAY insert_elements, \
        .insert_count = RBTEST_VALUE_COUNT(insert_elements), \
        .remove = RBTEST_VALUE_ARRAY remove_elements, \
        .remove_count = RBTEST_VALUE_COUNT(remove_elements), \
        .expected = RBTEST_VALUE_ARRAY expected_elements, \
        .expected_count = RBTEST_VALUE_COUNT(expected_elements), \
    }

static void
run_sequence_case(const rb_sequence_case_t *test_case)
{
    struct rb_root root_storage = {0};
    struct rb_root *root = rb_test_root_init(&root_storage);

    rb_test_node_t *nodes = NULL;
    if (test_case->insert_count > 0) {
        nodes = calloc(test_case->insert_count, sizeof(*nodes));
        assert_non_null(nodes);
        insert_bulk_and_validate(root, nodes, test_case->insert, test_case->insert_count);
    }

    if (test_case->remove_count > 0) {
        int previous_black_height = rb_test_black_height(root);
        for (size_t i = 0; i < test_case->remove_count; i++) {
            uint64 key = test_case->remove[i];
            struct rb_node *removed = rb_delete_key_color(root, key);
            assert_non_null(removed);
            assert_true(rb_test_validate_tree(root));
            int current_black_height = rb_test_black_height(root);
            assert_true(current_black_height <= previous_black_height);
            assert_true(current_black_height >= 0);
            previous_black_height = current_black_height;
        }
    }

    size_t capacity = test_case->insert_count > 0 ? test_case->insert_count : 1;
    uint64 *buffer = calloc(capacity, sizeof(uint64));
    assert_non_null(buffer);
    size_t visited = rb_test_collect_keys(root, buffer, capacity);
    assert_int_equal(visited, test_case->expected_count);
    for (size_t i = 0; i < visited; i++) {
        assert_int_equal(buffer[i], test_case->expected[i]);
    }

    int final_black_height = rb_test_black_height(root);
    if (test_case->expected_count == 0) {
        assert_int_equal(final_black_height, 0);
    } else {
        assert_true(final_black_height > 0);
    }

    free(buffer);
    free(nodes);
}

static const rb_sequence_case_t rb_sequence_cases[] = {
    RB_SEQUENCE_CASE((4, 2, 6, 1, 3, 5, 7), (), (1, 2, 3, 4, 5, 6, 7)),
    RB_SEQUENCE_CASE((10, 5, 1, 7, 40, 50), (7, 10), (1, 5, 40, 50)),
    RB_SEQUENCE_CASE((8, 4, 12, 2, 6, 10, 14, 1, 3), (2, 14, 8), (1, 3, 4, 6, 10, 12)),
    RB_SEQUENCE_CASE((30, 15, 60, 7, 22, 45, 75, 17, 27), (45, 22, 75, 7), (15, 17, 27, 30, 60)),
};

static int
cmp_uint64(const void *lhs, const void *rhs)
{
    uint64 a = *(const uint64 *)lhs;
    uint64 b = *(const uint64 *)rhs;
    if (a < b) {
        return -1;
    }
    if (a > b) {
        return 1;
    }
    return 0;
}

static void
test_rbtree_insert_sequential(void **state)
{
    (void)state;

    struct rb_root root_storage = {0};
    struct rb_root *root = rb_test_root_init(&root_storage);

    const uint64 keys[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    rb_test_node_t nodes[RBTEST_ARRAY_SIZE(keys)];

    insert_bulk_and_validate(root, nodes, keys, RBTEST_ARRAY_SIZE(keys));

    uint64 in_order[RBTEST_ARRAY_SIZE(keys)] = {0};
    size_t visited = rb_test_collect_keys(root, in_order, RBTEST_ARRAY_SIZE(in_order));
    assert_int_equal(visited, RBTEST_ARRAY_SIZE(keys));
    for (size_t i = 0; i < visited; i++) {
        assert_int_equal(in_order[i], keys[i]);
    }

    assert_true(rb_test_validate_tree(root));
}

static void
test_rbtree_insert_duplicate(void **state)
{
    (void)state;

    struct rb_root root_storage = {0};
    struct rb_root *root = rb_test_root_init(&root_storage);

    rb_test_node_t primary = {0};
    rb_test_node_init(&primary, 42, 1);
    struct rb_node *first = rb_insert_color(root, &primary.node);
    assert_ptr_equal(first, &primary.node);
    assert_true(rb_test_validate_tree(root));

    rb_test_node_t duplicate = {0};
    rb_test_node_init(&duplicate, 42, 99);
    struct rb_node *existing = rb_insert_color(root, &duplicate.node);
    assert_ptr_equal(existing, &primary.node);
    assert_true(rb_node_is_empty(&duplicate.node));
    assert_true(rb_test_validate_tree(root));

    uint64 inorder[2] = {0};
    size_t visited = rb_test_collect_keys(root, inorder, RBTEST_ARRAY_SIZE(inorder));
    assert_int_equal(visited, 1);
    assert_int_equal(inorder[0], 42);
}

static void
test_rbtree_sequence_cases(void **state)
{
    (void)state;

    for (size_t i = 0; i < RBTEST_ARRAY_SIZE(rb_sequence_cases); i++) {
        run_sequence_case(&rb_sequence_cases[i]);
    }
}

static void
test_rbtree_delete_balancing(void **state)
{
    (void)state;

    struct rb_root root_storage = {0};
    struct rb_root *root = rb_test_root_init(&root_storage);

    const uint64 keys[] = {41, 38, 31, 12, 19, 8, 4, 1, 2, 5, 64, 50, 80, 90, 70};
    rb_test_node_t nodes[RBTEST_ARRAY_SIZE(keys)];
    insert_bulk_and_validate(root, nodes, keys, RBTEST_ARRAY_SIZE(keys));

    size_t expected_size = RBTEST_ARRAY_SIZE(keys);
    assert_int_equal(rb_test_tree_size(root), expected_size);

    const uint64 delete_keys[] = {8, 12, 41, 64, 1};
    for (size_t i = 0; i < RBTEST_ARRAY_SIZE(delete_keys); i++) {
        uint64 key = delete_keys[i];
        struct rb_node *victim = rb_find_key(root, key);
        assert_non_null(victim);
        struct rb_node *removed = NULL;
        if (i % 2 == 0) {
            removed = rb_delete_key_color(root, key);
        } else {
            removed = rb_delete_node_color(root, victim);
        }
        assert_non_null(removed);
        expected_size--;
        assert_int_equal(rb_test_tree_size(root), expected_size);
        assert_null(rb_find_key(root, key));
        assert_true(rb_test_validate_tree(root));
    }

    /* remove remaining nodes to hit corner cases */
    while (root->node != NULL) {
        uint64 key = rb_get_node_key(root, root->node);
        struct rb_node *removed = rb_delete_key_color(root, key);
        assert_non_null(removed);
        expected_size--;
        assert_true(rb_test_validate_tree(root));
    }
    assert_int_equal(expected_size, 0);
    assert_true(rb_root_is_empty(root));
    assert_true(rb_test_validate_tree(root));
}

static void
test_rbtree_iteration_order(void **state)
{
    (void)state;

    struct rb_root root_storage = {0};
    struct rb_root *root = rb_test_root_init(&root_storage);

    const uint64 keys[] = {20, 10, 30, 5, 15, 25, 35};
    rb_test_node_t nodes[RBTEST_ARRAY_SIZE(keys)];
    insert_bulk_and_validate(root, nodes, keys, RBTEST_ARRAY_SIZE(keys));

    uint64 forward[RBTEST_ARRAY_SIZE(keys)];
    size_t forward_count = rb_test_collect_keys(root, forward, RBTEST_ARRAY_SIZE(forward));
    assert_int_equal(forward_count, RBTEST_ARRAY_SIZE(keys));

    for (size_t i = 1; i < forward_count; i++) {
        assert_true(forward[i-1] < forward[i]);
    }

    struct rb_node *node = rb_last_node(root);
    assert_non_null(node);

    size_t idx = forward_count;
    while (node != NULL) {
        assert_true(idx > 0);
        assert_int_equal(rb_get_node_key(root, node), forward[idx - 1]);
        node = rb_prev_node(node);
        idx--;
    }
    assert_int_equal(idx, 0);
    assert_true(rb_test_validate_tree(root));
}

static void
test_rbtree_delete_missing(void **state)
{
    (void)state;

    struct rb_root root_storage = {0};
    struct rb_root *root = rb_test_root_init(&root_storage);

    const uint64 keys[] = {11, 7, 18, 3, 10, 15, 20};
    rb_test_node_t nodes[RBTEST_ARRAY_SIZE(keys)];
    insert_bulk_and_validate(root, nodes, keys, RBTEST_ARRAY_SIZE(keys));

    int baseline_black_height = rb_test_black_height(root);
    struct rb_node *missing = rb_delete_key_color(root, 99);
    assert_null(missing);
    assert_true(rb_test_validate_tree(root));
    assert_int_equal(rb_test_black_height(root), baseline_black_height);
    assert_int_equal(rb_test_tree_size(root), RBTEST_ARRAY_SIZE(keys));

    /* Clean up to ensure tree can empty without imbalance */
    while (root->node != NULL) {
        uint64 key = rb_get_node_key(root, root->node);
        struct rb_node *removed = rb_delete_key_color(root, key);
        assert_non_null(removed);
        assert_true(rb_test_validate_tree(root));
    }
    assert_true(rb_root_is_empty(root));
}

static void
test_rbtree_scale_numbers(void **state)
{
    (void)state;

    struct rb_root root_storage = {0};
    struct rb_root *root = rb_test_root_init(&root_storage);

    rb_test_node_t *nodes = calloc(TEST_NUMBERS_COUNT, sizeof(*nodes));
    assert_non_null(nodes);

    int previous_black_height = -1;
    for (size_t i = 0; i < TEST_NUMBERS_COUNT; i++) {
        rb_test_node_init(&nodes[i], scale_test_numbers[i], scale_test_numbers[i]);
        struct rb_node *inserted = rb_insert_color(root, &nodes[i].node);
        assert_ptr_equal(inserted, &nodes[i].node);
        assert_true(rb_test_validate_tree(root));

        int current_black_height = rb_test_black_height(root);
        assert_true(current_black_height >= 0);
        if (previous_black_height != -1) {
            assert_true(current_black_height >= previous_black_height);
            assert_true(current_black_height <= previous_black_height + 1);
        }
        previous_black_height = current_black_height;

        if (((i + 1) % 100) == 0) {
            printf("[scale] insert %zu: black height %d\n", i + 1, current_black_height);
        }
    }

    assert_int_equal(rb_test_tree_size(root), TEST_NUMBERS_COUNT);

    uint64 *sorted = malloc(sizeof(uint64) * TEST_NUMBERS_COUNT);
    assert_non_null(sorted);
    memcpy(sorted, scale_test_numbers, sizeof(uint64) * TEST_NUMBERS_COUNT);
    qsort(sorted, TEST_NUMBERS_COUNT, sizeof(uint64), cmp_uint64);

    uint64 *inorder = malloc(sizeof(uint64) * TEST_NUMBERS_COUNT);
    assert_non_null(inorder);
    size_t visited = rb_test_collect_keys(root, inorder, TEST_NUMBERS_COUNT);
    assert_int_equal(visited, TEST_NUMBERS_COUNT);
    for (size_t i = 0; i < visited; i++) {
        assert_int_equal(inorder[i], sorted[i]);
    }

    int previous_delete_black_height = rb_test_black_height(root);
    assert_true(previous_delete_black_height > 0);

    size_t removed = 0;
    for (size_t i = 0; i < TEST_NUMBERS_COUNT; i += 2) {
        uint64 key = sorted[i];
        struct rb_node *removed_node = rb_delete_key_color(root, key);
        assert_non_null(removed_node);
        removed++;
        assert_true(rb_test_validate_tree(root));
        int current_black_height = rb_test_black_height(root);
        assert_true(current_black_height <= previous_delete_black_height);
        assert_true(current_black_height >= 0);
        previous_delete_black_height = current_black_height;
        assert_null(rb_find_key(root, key));

        if ((removed % 100) == 0) {
            printf("[scale] delete %zu: black height %d\n", removed, current_black_height);
        }
    }

    size_t remaining = TEST_NUMBERS_COUNT - removed;
    assert_int_equal(rb_test_tree_size(root), remaining);

    size_t visited_after = rb_test_collect_keys(root, inorder, TEST_NUMBERS_COUNT);
    assert_int_equal(visited_after, remaining);
    for (size_t idx = 0, exp = 1; idx < visited_after; idx++, exp += 2) {
        assert_true(exp < TEST_NUMBERS_COUNT);
        assert_int_equal(inorder[idx], sorted[exp]);
    }

    while (root->node != NULL) {
        uint64 key = rb_get_node_key(root, root->node);
        struct rb_node *removed_node = rb_delete_key_color(root, key);
        assert_non_null(removed_node);
        assert_true(rb_test_validate_tree(root));
        int current_black_height = rb_test_black_height(root);
        assert_true(current_black_height >= 0);
        previous_delete_black_height = current_black_height;

        removed++;
        if ((removed % 100) == 0) {
            printf("[scale] delete %zu: black height %d\n", removed, current_black_height);
        }
    }

    assert_true(rb_root_is_empty(root));
    assert_int_equal(rb_test_black_height(root), 0);

    free(inorder);
    free(sorted);
    free(nodes);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_rbtree_insert_sequential),
        cmocka_unit_test(test_rbtree_insert_duplicate),
        cmocka_unit_test(test_rbtree_sequence_cases),
        cmocka_unit_test(test_rbtree_delete_balancing),
        cmocka_unit_test(test_rbtree_iteration_order),
        cmocka_unit_test(test_rbtree_delete_missing),
        cmocka_unit_test(test_rbtree_scale_numbers),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
