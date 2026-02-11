/**
 * Red-Black Tree Implementation (adapted from kernel/bintree.c and rbtree.c)
 *
 * This is a copy of the kernel's rb-tree with minimal modifications for
 * userspace. The key insight is that the parent pointer's lowest bits are
 * used to store the node color (exploiting 8-byte alignment).
 */

#ifndef _UTILS_RBTREE_H_
#define _UTILS_RBTREE_H_

#include <stdint.h>
#include <stddef.h>

/* rb_node structure - must be 8-byte aligned so we can use low bits for color
 */
struct rb_node {
    uint64_t __parent_color; /* Parent pointer + color in lowest bit */
    struct rb_node *left;
    struct rb_node *right;
} __attribute__((aligned(8)));

/* Operations for the rb-tree: key comparison and extraction */
struct rb_root_opts {
    int (*keys_cmp_fun)(uint64_t, uint64_t);
    uint64_t (*get_key_fun)(struct rb_node *);
};

/* Root of an rb-tree */
struct rb_root {
    struct rb_node *node;
    struct rb_root_opts *opts;
};

/* Color mask - low 3 bits of parent pointer are available */
#define _RB_COLOR_MASK 7UL

/* Get parent pointer (clear color bits) */
static inline struct rb_node *rb_parent(struct rb_node *node) {
    if (node != NULL) {
        return (struct rb_node *)(node->__parent_color & (~_RB_COLOR_MASK));
    }
    return NULL;
}

/* Get left child */
static inline struct rb_node *rb_left(struct rb_node *node) {
    return node ? node->left : NULL;
}

/* Get right child */
static inline struct rb_node *rb_right(struct rb_node *node) {
    return node ? node->right : NULL;
}

/* Set parent pointer (preserving color) */
static inline void rb_set_parent(struct rb_node *node, struct rb_node *parent) {
    uint64_t pc = node->__parent_color;
    pc &= _RB_COLOR_MASK;
    pc |= (uint64_t)parent;
    node->__parent_color = pc;
}

/* Check if node is black (NULL is considered black) */
#define rb_is_node_black(node) ((node) == NULL || ((node)->__parent_color & 1))

/* Check if node is the root (no parent) */
#define rb_node_is_top(node) (rb_parent(node) == NULL)

/* Check if node is empty (parent points to self) */
#define rb_node_is_empty(node) ((node) == NULL || rb_parent(node) == (node))

/* Check if node is a leaf */
#define rb_node_is_leaf(node) ((node)->left == NULL && (node)->right == NULL)

/* Key comparison macro */
#define rb_keys_cmp(root, key1, key2) ((root)->opts->keys_cmp_fun(key1, key2))

/* Get key from node macro */
#define rb_get_node_key(root, node) ((root)->opts->get_key_fun(node))

/* Check if root is initialized */
#define rb_root_is_initialized(root)                                           \
    ((root) != NULL && (root)->opts != NULL &&                                 \
     (root)->opts->keys_cmp_fun != NULL && (root)->opts->get_key_fun != NULL)

/* Initialize a node (set parent to self = empty) */
static inline struct rb_node *rb_node_init(struct rb_node *node) {
    if (node == NULL)
        return NULL;
    node->__parent_color = (uint64_t)node;
    node->left = NULL;
    node->right = NULL;
    return node;
}

/* Initialize a root */
static inline struct rb_root *rb_root_init(struct rb_root *root,
                                           struct rb_root_opts *opts) {
    if (root == NULL || opts == NULL)
        return NULL;
    if (opts->keys_cmp_fun == NULL || opts->get_key_fun == NULL)
        return NULL;
    root->node = NULL;
    root->opts = opts;
    return root;
}

/* Dye node black */
static inline void rb_node_dye_black(struct rb_node *node) {
    if (node == NULL)
        return;
    node->__parent_color |= 1;
}

/* Dye node red */
static inline void rb_node_dye_red(struct rb_node *node) {
    if (node == NULL)
        return;
    node->__parent_color &= ~1UL;
}

/* Copy color from source to target */
static inline void rb_node_dye_as(struct rb_node *target,
                                  struct rb_node *source) {
    if (rb_is_node_black(source)) {
        rb_node_dye_black(target);
    } else {
        rb_node_dye_red(target);
    }
}

/* Link nodes: set parent's child pointer and node's parent pointer */
static inline void __rb_link_nodes(struct rb_node *parent, struct rb_node *node,
                                   struct rb_node **link) {
    rb_set_parent(node, parent);
    *link = node;
}

/*
 * Function declarations
 */

/* Get first node (minimum) */
struct rb_node *rb_first_node(struct rb_root *root);

/* Get last node (maximum) */
struct rb_node *rb_last_node(struct rb_root *root);

/* Get next node (in-order successor) */
struct rb_node *rb_next_node(struct rb_node *node);

/* Find node with key >= target (ceiling) */
struct rb_node *rb_find_key_rup(struct rb_root *root, uint64_t key);

/* Find node with key <= target (floor) */
struct rb_node *rb_find_key_rdown(struct rb_root *root, uint64_t key);

/* Insert with red-black rebalancing */
struct rb_node *rb_insert_color(struct rb_root *root, struct rb_node *node);

/* Delete a node from the tree */
struct rb_node *rb_delete_node_color(struct rb_root *root,
                                     struct rb_node *node);

#endif /* _UTILS_RBTREE_H_ */
