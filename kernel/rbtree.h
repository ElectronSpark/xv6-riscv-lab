#ifndef __KERNEL_RB_TREE_H__
#define __KERNEL_RB_TREE_H__

#include "bintree.h"

/**
 * @brief 判断红黑树节点是否是黑节点。
 * 
 * 此处采用与linux的红黑树节点相同的方法储存红黑树节点的颜色。
 * 在定义红黑树节点的结构体时，规定红黑树节点必须按 8 字节对齐，所以红黑树节点的指针的最低几位
 * 中至少有一位能保证是0，红黑树的颜色就储存在此处。
 * 规定 __parent_color 元素的最低位为 1 时为黑节点，为 0 时为红节点，且NULL节点颜色为黑色。
 * 
 * @param node struct rb_node* 红黑树节点结构体。
 * 
 * @retval true 红黑树节点是NULL节点或者是黑节点。
 * @retval false 红黑树节点是红节点。
 */
#define rb_is_node_black(node)  \
    ((node) == NULL || ((node)->__parent_color & 1))

/**
 * @brief 向红黑树中插入一个新的节点。
 * 
 * 在二叉树插入操作的基础上，通过旋转和染色等操作保证红黑树的黑平衡。
 * 和二叉树一样，如果新插入节点的key值已经存在于红黑树中，则不进行任何操作。
 * 
 * @param root 进行插入的红黑树根节点。
 * @param node 被插入的红黑树节点。
 * 
 * @return struct rb_node* 如果key值发生冲突，返回红黑树中相应的节点；如果key值没冲突，
 *                         返回新插入的节点；插入失败返回NULL。
 */
extern struct rb_node *rb_insert_color(
    struct rb_root *root,
    struct rb_node *node
);

/**
 * @brief Delete a node from the red-black tree.
 * 
 * @param root 进行节点删除的红黑树根节点。
 * @param node 被删除节点的指针。
 *
 * @return struct rb_node* 删除失败返回NULL，删除成功返回被删除的节点。
 */
extern struct rb_node *rb_delete_node_color(struct rb_root *root, struct rb_node *node);

/**
 * @brief 通过key值从红黑树中删除一个节点。
 * 
 * 在二叉树删除操作的基础上，通过旋转和染色等操作保证红黑树的黑平衡。
 * 
 * @param root 进行节点删除的红黑树根节点。
 * @param key 被删除节点的key值。
 * 
 * @return struct rb_node* 删除失败返回NULL，删除成功返回被删除的节点。
 */
extern struct rb_node *rb_delete_key_color(struct rb_root *root, unsigned long key);

#endif // __KERNEL_RB_TREE_H__
