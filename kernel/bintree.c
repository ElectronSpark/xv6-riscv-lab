#include "bintree.h"

/**
 * @brief 获得二叉树节点的兄弟节点的链接指针。
 * 
 * @param node struct rb_node* 二叉树节点。
 * 
 * @return struct rb_node** 二叉树节点有有兄弟节点时返回其兄弟节点的链接指针，否则返回
 *                          NULL。
 */
static inline struct rb_node **__rb_brother_link(struct rb_node *node)
{
    struct rb_node *__parent = rb_parent(node);
    if (__parent == NULL || __parent == node) {
        return NULL;
    }
    if (node == __parent->left) {
        return &__parent->right;
    } else {
        return &__parent->left;
    }
}

struct rb_node *rb_brother(struct rb_node *node)
{
    struct rb_node **__brother_link = __rb_brother_link(node);
    if (__brother_link == NULL) {
        return NULL;
    }
    return *__brother_link;
}

struct rb_node **__rb_node_link(
    struct rb_root *root,
    struct rb_node *node
)
{
    struct rb_node *__parent = rb_parent(node);
    if (__parent == NULL) {
        return &root->node;
    }
    if (__parent == node) {
        return NULL;
    }
    if (node == __parent->left) {
        return &__parent->left;
    } else {
        return &__parent->right;
    }
}

struct rb_node *rb_first_node(struct rb_root *root)
{
    if (root == NULL || root->node == NULL) {
        return NULL;
    }
    struct rb_node *pos = root->node;
    while (pos->left != NULL) {
        pos = pos->left;
    }
    return pos;
}

struct rb_node *rb_last_node(struct rb_root *root)
{
    if (root == NULL || root->node == NULL) {
        return NULL;
    }
    struct rb_node *pos = root->node;
    while (pos->right != NULL) {
        pos = pos->right;
    }
    return pos;
}

struct rb_node *rb_next_node(struct rb_node *node)
{
    if (rb_node_is_empty(node)) {
        return NULL;
    }
    struct rb_node *parent = node;
    struct rb_node *pos = parent->right;
    
    if (pos != NULL) {
        while (pos->left != NULL) {
            pos = pos->left;
        }
        return pos;
    }

    do {
        pos = parent;
        parent = rb_parent(pos);
    } while (parent != NULL && pos == parent->right);

    return parent;
}

struct rb_node *rb_prev_node(struct rb_node *node)
{
    if (rb_node_is_empty(node)) {
        return NULL;
    }
    struct rb_node *parent = node;
    struct rb_node *pos = parent->left;
    
    if (pos != NULL) {
        while (pos->right != NULL) {
            pos = pos->right;
        }
        return pos;
    }

    do {
        pos = parent;
        parent = rb_parent(pos);
    } while (parent != NULL && pos == parent->left);

    return parent;
}

void __rb_replace_node(
    struct rb_node **link,
    struct rb_node *new_node,
    struct rb_node *old_node
)
{
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

void __rb_transplant(
    struct rb_root *root, 
    struct rb_node *new_node,
    struct rb_node *old_node 
) {
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

struct rb_node **__rb_find_key_link(
    struct rb_root *root,
    struct rb_node **ret_parent,
    unsigned long key
)
{
    struct rb_node *pos = root->node;
    struct rb_node *parent = NULL;
    struct rb_node **link = &root->node;
    while (pos != NULL) {
        int cmp_result = rb_keys_cmp(root, rb_get_node_key(root, pos), key);
        if (cmp_result > 0) {
            link = &pos->left;
        } else if (cmp_result < 0) {
            link = &pos->right;
        } else {
            /// key值对比相等直接返回当前节点的链接。
            break;
        }
        parent = pos;
        pos = *link;
    }
    *ret_parent = parent;
    return link;
}

struct rb_node *rb_find_key(struct rb_root *root, unsigned long key)
{
    if (!rb_root_is_initialized(root)) {
        return NULL;
    }
    struct rb_node *parent = NULL;
    struct rb_node **link = __rb_find_key_link(root, &parent, key);
    if (link == NULL) {
        return NULL;
    }
    return *link;
}

struct rb_node *rb_insert_node(
    struct rb_root *root,
    struct rb_node *new_node
)
{
    if (!rb_root_is_initialized(root) || new_node == NULL) {
        return NULL;
    }
    unsigned long key = rb_get_node_key(root, new_node);
    struct rb_node *parent = NULL;
    struct rb_node **link = __rb_find_key_link(root, &parent, key);
    if (*link == NULL) {
        __rb_link_nodes(parent, new_node, link);
        new_node->left = NULL;
        new_node->right = NULL;
    }
    return *link;
}

static inline struct rb_node **__find_replacement_for_deletion(
    struct rb_root *root,
    struct rb_node *delete_node,
    struct rb_node **ret_parent
)
{
    /// 只有一个子节点的情况下直接用子节点替换当前节点。
    if (delete_node->left == NULL) {
        *ret_parent = delete_node;
        return &delete_node->right;
    } else if (delete_node->right == NULL) {
        *ret_parent = delete_node;
        return &delete_node->left;
    }

    /// 如果有两个子节点，用前驱节点来代替当前节点。
    struct rb_node **leaf_link = &delete_node->left;
    struct rb_node *leaf = *leaf_link;
    while (leaf->right != NULL) {
        leaf_link = &leaf->right;
        leaf = *leaf_link;
    }

    *ret_parent = rb_parent(leaf);
    return leaf_link;
}

struct rb_node *rb_delete_key(struct rb_root *root, unsigned long key)
{
    if (!rb_root_is_initialized(root)) {
        return NULL;
    }
    struct rb_node *parent = NULL;
    struct rb_node **link = __rb_find_key_link(root, &parent, key);
    struct rb_node *delete_node = *link;
    if (delete_node == NULL) {
        return NULL;
    }

    if (rb_node_is_leaf(delete_node)) {
        /// 没有子树的情况下该节点是叶子节点，直接删除。
        __rb_delink_node(link, delete_node);
        return delete_node;
    }
    /// 只有一个子节点的情况下直接用子节点替换当前节点。
    if (delete_node->left == NULL) {
        __rb_transplant(root, delete_node->right, delete_node);
        rb_set_parent(delete_node, delete_node);
        return delete_node;
    } else if (delete_node->right == NULL) {
        __rb_transplant(root, delete_node->left, delete_node);
        rb_set_parent(delete_node, delete_node);
        return delete_node;
    }

    /// 如果有两个子节点，用前驱节点来代替当前节点。
    struct rb_node **leaf_link = &delete_node->left;
    struct rb_node *leaf = *leaf_link;
    while (leaf->right != NULL) {
        leaf_link = &leaf->right;
        leaf = *leaf_link;
    }

    __rb_transplant(root, leaf->left, leaf);
    __rb_replace_node(link, leaf, delete_node);
    return delete_node;
}

struct rb_node *__rb_rotate_left(
    struct rb_root *root,
    struct rb_node *node
)
{
    if (node == NULL) {
        return NULL;
    }
    struct rb_node **link = __rb_node_link(root, node);
    struct rb_node *parent = rb_parent(node);
    struct rb_node *right = node->right;
    struct rb_node *right_left = NULL;
    if (right == NULL) {
        return node;
    }
    right_left = right->left;
    
    if (parent == NULL) {
        link = &root->node;
    }
    __rb_link_nodes(parent, right, link);
    if (right_left != NULL) {
        __rb_link_nodes(node, right_left, &node->right);
    } else {
        node->right = NULL;
    }
    __rb_link_nodes(right, node, &right->left);

    return right;
}

struct rb_node *__rb_rotate_right(
    struct rb_root *root,
    struct rb_node *node
)
{
    if (node == NULL) {
        return NULL;
    }
    struct rb_node **link = __rb_node_link(root, node);
    struct rb_node *parent = rb_parent(node);
    struct rb_node *left = node->left;
    struct rb_node *left_right = NULL;
    if (left == NULL) {
        return node;
    }
    left_right = left->right;
    
    if (parent == NULL) {
        link = &root->node;
    }
    __rb_link_nodes(parent, left, link);
    if (left_right != NULL) {
        __rb_link_nodes(node, left_right, &node->left);
    } else {
        node->left = NULL;
    }
    __rb_link_nodes(left, node, &left->right);

    return left;
}
