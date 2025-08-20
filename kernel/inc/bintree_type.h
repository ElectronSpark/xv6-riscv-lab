#ifndef __KERNEL_BIN_TREE_TYPE_H__
#define __KERNEL_BIN_TREE_TYPE_H__

#include "types.h"

/* 红黑树的结构体，有时也直接作为二叉树。下面是红黑树的普通节点。 */
struct rb_node {
    uint64   __parent_color; /// 当前节点的父节点指针和当前节点的颜色。
    struct rb_node  *left;  /// 当前节点的左子树。
    struct rb_node  *right; /// 当前节点的右子树。
} __attribute__((aligned(8)));

/* 操作红黑树的方法的函数指针集。 */
struct rb_root_opts {
    /**
     * @brief 用于对比两个红黑树节点的 key 值大小关系。
     * 
     * @param $0    第一个 key 值。
     * @param $1    第二个 key 值。
     * 
     * @return int  当$0小于$1时返回一个负数；相等时返回0；大于时返回一个正数。
     */
    int (*keys_cmp_fun)(uint64, uint64);
    /**
     * @brief 获得红黑树节点的key值。
     * 
     * @param $0    指向红黑树节点的指针。
     * 
     * @return uint64    给定红黑树节点key值。
     */
    uint64 (*get_key_fun)(struct rb_node *);
};

/* 红黑树的根节点 */
struct rb_root {
    struct rb_node  *node;  /// 指向首个红黑树节点。
    struct rb_root_opts *opts;  /// 操作红黑树的方法的函数指针集。
};

#endif // __KERNEL_BIN_TREE_TYPE_H__
