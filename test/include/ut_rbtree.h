#ifndef __HOST_TEST_UT_RBTREE_H
#define __HOST_TEST_UT_RBTREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <cmocka.h>

#ifndef RBTEST_ARRAY_SIZE
#define RBTEST_ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#define RBTEST_VALUE_ARRAY(...) (const uint64[]){ __VA_ARGS__ }
#define RBTEST_VALUE_COUNT(tuple) \
    (sizeof(RBTEST_VALUE_ARRAY tuple) / sizeof((RBTEST_VALUE_ARRAY tuple)[0]))

#include "types.h"
#include "rbtree.h"
#include "bintree.h"

/*
 * Small helper structure that carries an rb_node together with
 * an explicit key and optional payload value. The payload allows
 * callers to store additional context if needed while keeping
 * the test helpers generic.
 */
typedef struct rb_test_node {
    struct rb_node node;
    uint64 key;
    uint64 value;
} rb_test_node_t;

static inline void
rb_test_node_init(rb_test_node_t *node, uint64 key, uint64 value)
{
    assert_non_null(node);
    assert_non_null(rb_node_init(&node->node));
    node->key = key;
    node->value = value;
}

static inline int
rb_test_key_cmp(uint64 lhs, uint64 rhs)
{
    if (lhs < rhs) {
        return -1;
    }
    if (lhs > rhs) {
        return 1;
    }
    return 0;
}

static inline uint64
rb_test_get_key(struct rb_node *node)
{
    assert_non_null(node);
    rb_test_node_t *entry = container_of(node, rb_test_node_t, node);
    return entry->key;
}

static struct rb_root_opts rb_test_root_ops = {
    .keys_cmp_fun = rb_test_key_cmp,
    .get_key_fun = rb_test_get_key,
};

static inline struct rb_root *
rb_test_root_init(struct rb_root *root)
{
    assert_non_null(root);
    return rb_root_init(root, &rb_test_root_ops);
}

static bool
rb_test_validate_subtree(struct rb_root *root,
                         struct rb_node *node,
                         int black_count,
                         int *expected_black_height,
                         bool has_min, uint64 min_key,
                         bool has_max, uint64 max_key)
{
    if (node == NULL) {
        if (*expected_black_height == -1) {
            *expected_black_height = black_count;
            return true;
        }
        return black_count == *expected_black_height;
    }

    uint64 key = rb_get_node_key(root, node);

    if (has_min && key <= min_key) {
        return false;
    }
    if (has_max && key >= max_key) {
        return false;
    }

    if (!rb_is_node_black(node)) {
        if (!rb_is_node_black(node->left) || !rb_is_node_black(node->right)) {
            return false; /* red node cannot have red child */
        }
    } else {
        black_count++;
    }

    if (node->left != NULL && rb_parent(node->left) != node) {
        return false;
    }
    if (node->right != NULL && rb_parent(node->right) != node) {
        return false;
    }

    if (!rb_test_validate_subtree(root, node->left, black_count,
                                  expected_black_height,
                                  has_min, min_key,
                                  true, key)) {
        return false;
    }
    if (!rb_test_validate_subtree(root, node->right, black_count,
                                  expected_black_height,
                                  true, key,
                                  has_max, max_key)) {
        return false;
    }

    return true;
}

static inline bool
rb_test_validate_tree(struct rb_root *root)
{
    assert_non_null(root);
    if (!rb_root_is_initialized(root)) {
        return false;
    }

    if (root->node == NULL) {
        return true;
    }

    if (rb_parent(root->node) != NULL) {
        return false;
    }

    if (!rb_is_node_black(root->node)) {
        return false;
    }

    int expected_black_height = -1;
    return rb_test_validate_subtree(root, root->node, 0,
                                    &expected_black_height,
                                    false, 0,
                                    false, 0);
}

static inline int
rb_test_black_height(struct rb_root *root)
{
    assert_non_null(root);
    if (root->node == NULL) {
        return 0;
    }

    int expected_black_height = -1;
    bool ok = rb_test_validate_subtree(root, root->node, 0,
                                       &expected_black_height,
                                       false, 0,
                                       false, 0);
    assert_true(ok);
    return expected_black_height;
}

static inline size_t
rb_test_collect_keys(struct rb_root *root, uint64 *buffer, size_t capacity)
{
    size_t count = 0;
    for (struct rb_node *node = rb_first_node(root);
         node != NULL && count < capacity;
         node = rb_next_node(node)) {
        buffer[count++] = rb_get_node_key(root, node);
    }
    return count;
}

static inline size_t
rb_test_tree_size(struct rb_root *root)
{
    size_t count = 0;
    for (struct rb_node *node = rb_first_node(root);
         node != NULL;
         node = rb_next_node(node)) {
        count++;
    }
    return count;
}

#endif /* __HOST_TEST_UT_RBTREE_H */
