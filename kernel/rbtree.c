/**
 * 红黑树基本操作的实现
 */
#include "rbtree.h"

/**
 * @brief 将红黑树节点染黑。
 *
 * @param node 进行染色的红黑树节点。
 */
static inline void rb_node_dye_black(struct rb_node *node) {
    if (node == NULL) {
        return;
    }
    node->__parent_color |= 1;
}

/**
 * @brief 将红黑树节点染红。
 *
 * @param node 进行染色的红黑树节点。
 */
static inline void rb_node_dye_red(struct rb_node *node) {
    if (node == NULL) {
        return;
    }
    node->__parent_color &= ~1UL;
}

/**
 * @brief 将一个红黑树节点的颜色与另一个红黑树节点的颜色同步。
 *
 * @param target_node 进行染色的红黑树节点。
 * @param source_node 获取颜色的红黑树节点。
 */
static inline void rb_node_dye_as(struct rb_node *target_node,
                                  struct rb_node *source_node) {
    if (rb_is_node_black(source_node)) {
        rb_node_dye_black(target_node);
    } else {
        rb_node_dye_red(target_node);
    }
}

struct rb_node *rb_insert_color(struct rb_root *root, struct rb_node *node) {
    /// 检测输入是否合法，不合法直接视为插入失败。
    if (root == NULL || node == NULL) {
        return NULL;
    }

    /// 此处直接复用二叉树的插入方法插入节点，插入成功的情况下总是返回被插入节点的指针。
    struct rb_node *pos = rb_insert_node(root, node);
    if (pos != node) {
        return pos;
    }

    /// 将被插入的节点染红，然后循环执行平衡操作，直到 pos 的父节点为黑色。
    rb_node_dye_red(pos);
    struct rb_node *parent = rb_parent(pos);
    struct rb_node *grand_parent = rb_parent(parent);
    struct rb_node *uncle = NULL;
    while (!rb_is_node_black(parent)) {
        if (parent == grand_parent->left) {
            uncle = grand_parent->right;
            if (!rb_is_node_black(uncle)) {
                /// 父节点和叔叔节点都是红色。
                rb_node_dye_black(parent);
                rb_node_dye_black(uncle);
                rb_node_dye_red(grand_parent);
                pos = grand_parent;
            } else if (pos == parent->right) {
                /// 如果parent为grandparent的左子节点，pos也必须为parent的左节点。
                __rb_rotate_left(root, parent);
                __rb_rotate_right(root, grand_parent);
                rb_node_dye_black(parent);
                rb_node_dye_black(grand_parent);
            } else {
                /// 直接旋转并重新染色。
                __rb_rotate_right(root, grand_parent);
                rb_node_dye_black(pos);
                rb_node_dye_black(grand_parent);
                pos = parent;
            }
        } else {
            /// 父节点为右子节点和其为左子节点的情况是对称的。
            uncle = grand_parent->left;
            if (!rb_is_node_black(uncle)) {
                /// 父节点和叔叔节点都是红色。
                rb_node_dye_black(parent);
                rb_node_dye_black(uncle);
                rb_node_dye_red(grand_parent);
                pos = grand_parent;
            } else if (pos == parent->left) {
                /// 如果parent为grandparent的左子节点，pos也必须为parent的左节点。
                __rb_rotate_right(root, parent);
                __rb_rotate_left(root, grand_parent);
                rb_node_dye_black(parent);
                rb_node_dye_black(grand_parent);
            } else {
                /// 直接旋转并重新染色。
                __rb_rotate_left(root, grand_parent);
                rb_node_dye_black(pos);
                rb_node_dye_black(grand_parent);
                pos = parent;
            }
        }
        /// 跳转相关变量的值，以便进行下一次操作。
        parent = rb_parent(pos);
        grand_parent = rb_parent(parent);
    }

    /// 必须保证红黑树的根节点为黑色。
    if (rb_node_is_top(pos)) {
        rb_node_dye_black(pos);
    }

    return node;
}

/**
 * @brief 若真正删除的节点为黑节点，则需要通过该函数重新进行黑平衡。
 *
 * 如果用于代替被删除节点，或被删除的节点本身为黑节点，那完成删除操作后红黑树的黑平衡会被打破，
 * 此时就需要让红黑树重新达到黑平衡。
 *
 */
static inline void __rb_delete_color_fixup(struct rb_root *root,
                                           struct rb_node *node) {
    struct rb_node *brother;
    struct rb_node *parent = rb_parent(node);
    while (node != root->node && rb_is_node_black(node)) {
        if (node == rb_left(parent)) {
            brother = rb_right(parent);
            if (!rb_is_node_black(brother)) {
                /// 找到真正的兄弟节点。
                rb_node_dye_red(parent);
                rb_node_dye_black(brother);
                __rb_rotate_left(root, parent);
                parent = rb_parent(node);
                brother = rb_right(parent);
            }
            if (rb_is_node_black(rb_left(brother)) &&
                rb_is_node_black(rb_right(brother))) {
                /// 兄弟节点拿不出红节点。
                rb_node_dye_red(brother);
                node = parent;
                parent = rb_parent(node);
            } else {
                if (rb_is_node_black(rb_right(brother))) {
                    /// 对兄弟节点进行排序
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
                /// 找到真正的兄弟节点。
                rb_node_dye_red(parent);
                rb_node_dye_black(brother);
                __rb_rotate_right(root, parent);
                parent = rb_parent(node);
                brother = rb_left(parent);
            }
            if (rb_is_node_black(rb_left(brother)) &&
                rb_is_node_black(rb_right(brother))) {
                /// 兄弟节点拿不出红节点。
                rb_node_dye_red(brother);
                node = parent;
                parent = rb_parent(node);
            } else {
                if (rb_is_node_black(rb_left(brother))) {
                    /// 对兄弟节点进行排序
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

static void __rb_do_delete_node_color(struct rb_root *root,
                                      struct rb_node *parent,
                                      struct rb_node **link) {
    struct rb_node *target;
    struct rb_node *delete_node = *link;

    if (delete_node == NULL) {
        return;
    }

    target = delete_node;

    if (target->left != NULL && target->right != NULL) {
        struct rb_node *successor = target->right;
        while (successor->left != NULL) {
            successor = successor->left;
        }
        /// swap successor and delete_node
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
        /// need rebalance
        if (rb_is_node_black(target)) {
            __rb_delete_color_fixup(root, replacement);
        }
    } else if (rb_node_is_top(target)) {
        /// 删除节点为根节点。
        root->node = NULL;
    } else {
        /// 删除节点为叶子节点。
        if (rb_is_node_black(target)) {
            __rb_delete_color_fixup(root, target);
        }
        if (!rb_node_is_top(target)) {
            struct rb_node **target_link = __rb_node_link(root, target, NULL);
            *target_link = NULL;
            rb_set_parent(target, target);
        }
    }

    /// 用真正被删除的节点替代原有的节点。
    if (target != delete_node) {
        __rb_replace_node(__rb_node_link(root, delete_node, NULL), target,
                          delete_node);
    }
}

struct rb_node *rb_delete_node_color(struct rb_root *root,
                                     struct rb_node *node) {
    struct rb_node *parent;
    struct rb_node **link = __rb_node_link(root, node, &parent);
    struct rb_node *target;
    if (link == NULL || *link == NULL) {
        return NULL;
    }
    target = *link;
    __rb_do_delete_node_color(root, parent, link);
    return target;
}

struct rb_node *rb_delete_key_color(struct rb_root *root, unsigned long key) {
    if (!rb_root_is_initialized(root)) {
        return NULL;
    }
    struct rb_node *parent;
    struct rb_node **link = __rb_find_key_link(root, &parent, key);
    struct rb_node *target;
    if (link == NULL || *link == NULL) {
        return NULL;
    }
    target = *link;
    __rb_do_delete_node_color(root, parent, link);
    return target;
}