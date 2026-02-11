/**
 * Red-Black Tree Implementation (adapted from kernel/bintree.c and rbtree.c)
 */

#include "rbtree.h"

/* Get the link pointer for a node in its parent */
static struct rb_node **__rb_node_link(struct rb_root *root,
                                       struct rb_node *node,
                                       struct rb_node **ret_parent) {
    struct rb_node *parent = rb_parent(node);
    if (ret_parent)
        *ret_parent = parent;
    if (parent == NULL)
        return &root->node;
    if (parent == node)
        return NULL; /* Empty node */
    if (node == parent->left)
        return &parent->left;
    return &parent->right;
}

/* Find link for a key value */
static struct rb_node **__rb_find_key_link(struct rb_root *root,
                                           struct rb_node **ret_parent,
                                           uint64_t key) {
    struct rb_node *pos = root->node;
    struct rb_node *parent = NULL;
    struct rb_node **link = &root->node;
    while (pos != NULL) {
        int cmp = rb_keys_cmp(root, rb_get_node_key(root, pos), key);
        if (cmp > 0) {
            link = &pos->left;
        } else if (cmp < 0) {
            link = &pos->right;
        } else {
            break;
        }
        parent = pos;
        pos = *link;
    }
    *ret_parent = parent;
    return link;
}

/* Transplant: replace old_node with new_node in the tree */
static void __rb_transplant(struct rb_root *root, struct rb_node *new_node,
                            struct rb_node *old_node) {
    struct rb_node *parent = rb_parent(old_node);
    if (parent == NULL) {
        root->node = new_node;
    } else if (parent->left == old_node) {
        parent->left = new_node;
    } else {
        parent->right = new_node;
    }
    if (new_node != NULL) {
        rb_set_parent(new_node, parent);
    }
}

/* Replace old_node with new_node (copy structure) */
static void __rb_replace_node(struct rb_node **link, struct rb_node *new_node,
                              struct rb_node *old_node) {
    *new_node = *old_node;
    *link = new_node;
    if (old_node->left != NULL) {
        rb_set_parent(old_node->left, new_node);
    }
    if (old_node->right != NULL) {
        rb_set_parent(old_node->right, new_node);
    }
    rb_node_init(old_node);
}

/* Left rotation */
static struct rb_node *__rb_rotate_left(struct rb_root *root,
                                        struct rb_node *node) {
    if (node == NULL)
        return NULL;
    struct rb_node **link = __rb_node_link(root, node, NULL);
    struct rb_node *parent = rb_parent(node);
    struct rb_node *right = node->right;
    if (right == NULL)
        return node;
    struct rb_node *right_left = right->left;
    if (parent == NULL)
        link = &root->node;
    __rb_link_nodes(parent, right, link);
    if (right_left != NULL) {
        __rb_link_nodes(node, right_left, &node->right);
    } else {
        node->right = NULL;
    }
    __rb_link_nodes(right, node, &right->left);
    return right;
}

/* Right rotation */
static struct rb_node *__rb_rotate_right(struct rb_root *root,
                                         struct rb_node *node) {
    if (node == NULL)
        return NULL;
    struct rb_node **link = __rb_node_link(root, node, NULL);
    struct rb_node *parent = rb_parent(node);
    struct rb_node *left = node->left;
    if (left == NULL)
        return node;
    struct rb_node *left_right = left->right;
    if (parent == NULL)
        link = &root->node;
    __rb_link_nodes(parent, left, link);
    if (left_right != NULL) {
        __rb_link_nodes(node, left_right, &node->left);
    } else {
        node->left = NULL;
    }
    __rb_link_nodes(left, node, &left->right);
    return left;
}

/* Insert node into tree (BST insert, no rebalancing) */
static struct rb_node *rb_insert_node(struct rb_root *root,
                                      struct rb_node *new_node) {
    if (!rb_root_is_initialized(root) || new_node == NULL)
        return NULL;
    uint64_t key = rb_get_node_key(root, new_node);
    struct rb_node *parent = NULL;
    struct rb_node **link = __rb_find_key_link(root, &parent, key);
    if (*link == NULL) {
        __rb_link_nodes(parent, new_node, link);
        new_node->left = NULL;
        new_node->right = NULL;
    }
    return *link;
}

/* Delete fixup for maintaining red-black properties */
static void __rb_delete_color_fixup(struct rb_root *root,
                                    struct rb_node *node) {
    struct rb_node *brother;
    struct rb_node *parent = rb_parent(node);
    while (node != root->node && rb_is_node_black(node)) {
        if (node == rb_left(parent)) {
            brother = rb_right(parent);
            if (!rb_is_node_black(brother)) {
                rb_node_dye_red(parent);
                rb_node_dye_black(brother);
                __rb_rotate_left(root, parent);
                parent = rb_parent(node);
                brother = rb_right(parent);
            }
            if (rb_is_node_black(rb_left(brother)) &&
                rb_is_node_black(rb_right(brother))) {
                rb_node_dye_red(brother);
                node = parent;
                parent = rb_parent(node);
            } else {
                if (rb_is_node_black(rb_right(brother))) {
                    rb_node_dye_black(rb_left(brother));
                    rb_node_dye_red(brother);
                    __rb_rotate_right(root, brother);
                    parent = rb_parent(node);
                    brother = rb_right(parent);
                }
                rb_node_dye_as(brother, parent);
                rb_node_dye_black(parent);
                rb_node_dye_black(rb_right(brother));
                __rb_rotate_left(root, parent);
                node = root->node;
                parent = NULL;
            }
        } else {
            brother = rb_left(parent);
            if (!rb_is_node_black(brother)) {
                rb_node_dye_red(parent);
                rb_node_dye_black(brother);
                __rb_rotate_right(root, parent);
                parent = rb_parent(node);
                brother = rb_left(parent);
            }
            if (rb_is_node_black(rb_left(brother)) &&
                rb_is_node_black(rb_right(brother))) {
                rb_node_dye_red(brother);
                node = parent;
                parent = rb_parent(node);
            } else {
                if (rb_is_node_black(rb_left(brother))) {
                    rb_node_dye_black(rb_right(brother));
                    rb_node_dye_red(brother);
                    __rb_rotate_left(root, brother);
                    parent = rb_parent(node);
                    brother = rb_left(parent);
                }
                rb_node_dye_as(brother, parent);
                rb_node_dye_black(parent);
                rb_node_dye_black(rb_left(brother));
                __rb_rotate_right(root, parent);
                node = root->node;
                parent = NULL;
            }
        }
    }
    rb_node_dye_black(node);
}

/* Internal delete implementation */
static void __rb_do_delete_node_color(struct rb_root *root,
                                      struct rb_node **link) {
    struct rb_node *delete_node = *link;
    if (delete_node == NULL)
        return;

    struct rb_node *target = delete_node;
    if (target->left != NULL && target->right != NULL) {
        struct rb_node *successor = target->right;
        while (successor->left != NULL)
            successor = successor->left;
        target = successor;
    }

    struct rb_node *replacement = NULL;
    if (target->left != NULL) {
        replacement = target->left;
    } else {
        replacement = target->right;
    }

    if (replacement != NULL) {
        __rb_transplant(root, replacement, target);
        rb_set_parent(target, target);
        if (rb_is_node_black(target)) {
            __rb_delete_color_fixup(root, replacement);
        }
    } else if (rb_node_is_top(target)) {
        root->node = NULL;
    } else {
        if (rb_is_node_black(target)) {
            __rb_delete_color_fixup(root, target);
        }
        if (!rb_node_is_top(target)) {
            struct rb_node **target_link = __rb_node_link(root, target, NULL);
            *target_link = NULL;
            rb_set_parent(target, target);
        }
    }

    if (target != delete_node) {
        __rb_replace_node(__rb_node_link(root, delete_node, NULL), target,
                          delete_node);
    }
}

/*
 * Public API implementations
 */

struct rb_node *rb_first_node(struct rb_root *root) {
    if (root == NULL || root->node == NULL)
        return NULL;
    struct rb_node *pos = root->node;
    while (pos->left != NULL)
        pos = pos->left;
    return pos;
}

struct rb_node *rb_last_node(struct rb_root *root) {
    if (root == NULL || root->node == NULL)
        return NULL;
    struct rb_node *pos = root->node;
    while (pos->right != NULL)
        pos = pos->right;
    return pos;
}

struct rb_node *rb_next_node(struct rb_node *node) {
    if (rb_node_is_empty(node))
        return NULL;
    struct rb_node *parent = node;
    struct rb_node *pos = parent->right;
    if (pos != NULL) {
        while (pos->left != NULL)
            pos = pos->left;
        return pos;
    }
    do {
        pos = parent;
        parent = rb_parent(pos);
    } while (parent != NULL && pos == parent->right);
    return parent;
}

struct rb_node *rb_find_key_rup(struct rb_root *root, uint64_t key) {
    if (!rb_root_is_initialized(root))
        return NULL;
    struct rb_node *parent = NULL;
    struct rb_node **link = __rb_find_key_link(root, &parent, key);
    if (link == NULL)
        return NULL;
    if (*link != NULL)
        return *link;
    if (parent == NULL)
        return NULL;
    uint64_t pkey = rb_get_node_key(root, parent);
    if (rb_keys_cmp(root, pkey, key) >= 0)
        return parent;
    return rb_next_node(parent);
}

struct rb_node *rb_find_key_rdown(struct rb_root *root, uint64_t key) {
    if (!rb_root_is_initialized(root))
        return NULL;
    struct rb_node *parent = NULL;
    struct rb_node **link = __rb_find_key_link(root, &parent, key);
    if (link == NULL)
        return NULL;
    if (*link != NULL)
        return *link;
    if (parent == NULL)
        return NULL;
    uint64_t pkey = rb_get_node_key(root, parent);
    if (rb_keys_cmp(root, pkey, key) <= 0)
        return parent;
    /* Get previous node */
    struct rb_node *node = parent;
    if (rb_node_is_empty(node))
        return NULL;
    struct rb_node *pos = node->left;
    if (pos != NULL) {
        while (pos->right != NULL)
            pos = pos->right;
        return pos;
    }
    do {
        pos = node;
        node = rb_parent(pos);
    } while (node != NULL && pos == node->left);
    return node;
}

struct rb_node *rb_insert_color(struct rb_root *root, struct rb_node *node) {
    if (root == NULL || node == NULL)
        return NULL;
    struct rb_node *pos = rb_insert_node(root, node);
    if (pos != node)
        return pos;

    rb_node_dye_red(pos);
    struct rb_node *parent = rb_parent(pos);
    struct rb_node *grand_parent = rb_parent(parent);
    struct rb_node *uncle = NULL;

    while (!rb_is_node_black(parent)) {
        if (parent == grand_parent->left) {
            uncle = grand_parent->right;
            if (!rb_is_node_black(uncle)) {
                rb_node_dye_black(parent);
                rb_node_dye_black(uncle);
                rb_node_dye_red(grand_parent);
                pos = grand_parent;
            } else if (pos == parent->right) {
                __rb_rotate_left(root, parent);
                __rb_rotate_right(root, grand_parent);
                rb_node_dye_black(parent);
                rb_node_dye_black(grand_parent);
            } else {
                __rb_rotate_right(root, grand_parent);
                rb_node_dye_black(pos);
                rb_node_dye_black(grand_parent);
                pos = parent;
            }
        } else {
            uncle = grand_parent->left;
            if (!rb_is_node_black(uncle)) {
                rb_node_dye_black(parent);
                rb_node_dye_black(uncle);
                rb_node_dye_red(grand_parent);
                pos = grand_parent;
            } else if (pos == parent->left) {
                __rb_rotate_right(root, parent);
                __rb_rotate_left(root, grand_parent);
                rb_node_dye_black(parent);
                rb_node_dye_black(grand_parent);
            } else {
                __rb_rotate_left(root, grand_parent);
                rb_node_dye_black(pos);
                rb_node_dye_black(grand_parent);
                pos = parent;
            }
        }
        parent = rb_parent(pos);
        grand_parent = rb_parent(parent);
    }

    if (rb_node_is_top(pos)) {
        rb_node_dye_black(pos);
    }
    return node;
}

struct rb_node *rb_delete_node_color(struct rb_root *root,
                                     struct rb_node *node) {
    struct rb_node **link = __rb_node_link(root, node, NULL);
    if (link == NULL || *link == NULL)
        return NULL;
    struct rb_node *target = *link;
    __rb_do_delete_node_color(root, link);
    return target;
}
