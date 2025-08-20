#ifndef __KERNEL_BIN_TREE_H__
#define __KERNEL_BIN_TREE_H__

#include "bintree_type.h"

/**
 * @brief 用于对比属于同一个红黑树的两个红黑树节点的 key 值大小关系。
 * 
 * @param root struct rb_root* key 值对应红黑树节点所在的红黑树树根。
 * @param key1 unsigned long 第一个 key 值。
 * @param key2 unsigned long 第二个 key 值。
 * 
 * @return int  当 key1 小于 key2 时返回一个负数；相等时返回0；大于时返回一个正数。
 */
#define rb_keys_cmp(root, key1, key2)   ((root)->opts->keys_cmp_fun(key1, key2))

/**
 * @brief 获得红黑树节点的key值。
 * 
 * @param root struct rb_root* key 值对应红黑树节点所在的红黑树树根。
 * @param node struct rb_node* 红黑树节点。
 * 
 * @return unsigned long    给定红黑树节点key值。
 */
#define rb_get_node_key(root, node) ((root)->opts->get_key_fun(node))

/**
 * @brief 二叉树未按 8 字节对齐的比特被用作特殊用途。
 */
#define _RB_COLOR_MASK  7UL

/**
 * @brief 获得父节点。
 * 
 * @param node 指向当前节点的指针。
 * 
 * @return struct rb_node* 指向父节点的指针。
 */
static inline struct rb_node *rb_parent(struct rb_node *node)
{
    if (node != NULL) {
        return (void *)(node->__parent_color & (~_RB_COLOR_MASK));
    }
    return NULL;
}

/**
 * @brief 获得左子节点。
 * 
 * @param node 指向当前节点的指针。
 * 
 * @return struct rb_node* 指向左子节点的指针。
 */
static inline struct rb_node *rb_left(struct rb_node *node)
{
    if (node != NULL) {
        return node->left;
    }
    return NULL;
}

/**
 * @brief 获得右子节点。
 * 
 * @param node 指向当前节点的指针。
 * 
 * @return struct rb_node* 指向右子节点的指针。
 */
static inline struct rb_node *rb_right(struct rb_node *node)
{
    if (node != NULL) {
        return node->right;
    }
    return NULL;
}

/**
 * @brief 设置父节点
 * 
 * @param node 指向当前节点的指针。
 * @param parent 指向父节点的指针。
 */
static inline void rb_set_parent(struct rb_node *node, struct rb_node *parent)
{
    unsigned long __parent_color = node->__parent_color;
    __parent_color &= _RB_COLOR_MASK;
    __parent_color |= (unsigned long)(parent);
    node->__parent_color = __parent_color;
}

/**
 * @brief 检查二叉树树根是否已经被初始化。
 * 
 * @param root struct rb_root* 二叉树根结构体。
 * 
 * @retval true 二叉树的根节点已经被初始化。
 * @retval false 二叉树的根节点没有被初始化。
 */
#define rb_root_is_initialized(root)    \
    (  (root) != NULL   \
    && (root)->opts != NULL \
    && (root)->opts->keys_cmp_fun != NULL   \
    && (root)->opts->get_key_fun != NULL)

/**
 * @brief 判断二叉树是否为空树。
 * 
 * @param root struct rb_root* 二叉树根结构体。
 * 
 * @retval true 二叉树为空。
 * @retval false 二叉树不为空。
 */
#define rb_root_is_empty(root)  ((root)->node == NULL)

/**
 * @brief 判断二叉树节点是否为空节点。
 * 
 * @param node struct rb_node* 二叉树节点。
 * 
 * @retval true 二叉树节点为空节点。
 * @retval false 二叉树节点不为空节点。
 */
#define rb_node_is_empty(node)  \
    ((node) == NULL || rb_parent(node) == (node))

/**
 * @brief 判断二叉树节点是否为最顶端的首节点。
 * 
 * @param node struct rb_node* 二叉树节点。
 * 
 * @retval true 二叉树节点为最顶端节点。
 * @retval false 二叉树节点不为最顶端节点。
 */
#define rb_node_is_top(node)  (rb_parent(node) == NULL)

/**
 * @brief 判断二叉树节点是否为叶子节点。 
 * 
 * @param node struct rb_node* 二叉树节点。
 * 
 * @retval true 二叉树节点是叶子节点。
 * @retval false 二叉树节点不是叶子节点。
 */
#define rb_node_is_leaf(node)   ((node)->left == NULL && (node)->right == NULL)

/**
 * @brief 判断二叉树节点是否为左子树。
 * 
 * 若二叉树节点没有父节点，或二叉树节点为空，同样视为不属于左子树。
 * 
 * @param node 二叉树节点。
 * 
 * @retval true 二叉树节点是其父节点的左子树。
 * @retval false 二叉树节点不是其父节点的左子树。
 */
static inline int rb_node_is_left(struct rb_node *node)
{
    struct rb_node  *__parent = rb_parent(node);
    if (__parent != NULL && __parent != (node)) {
        return __parent->left == (node);
    }
    return 0;
}

/**
 * @brief 获得二叉树节点的兄弟节点。
 * 
 * 若二叉树节点没有父节点，或二叉树节点为空，同样视为没有兄弟节点。
 * 
 * @param node struct rb_node* 二叉树节点。
 * 
 * @return struct rb_node* 二叉树节点有兄弟节点时返回其兄弟节点，否则返回NULL。
 */
extern struct rb_node *rb_brother(struct rb_node *node);

/**
 * @brief 获得二叉树节点的链接指针。
 * 
 * 由于一个二叉树节点有两个指向子节点的指针，所以对二叉树节点的某些操作，不仅需要明确当前节点
 * 和其父节点，还需要明确当前节点属于其父节点的左子树还是右子树。为了减少因这个原因引起的一些
 * 重复的操作，定义二叉树节点的链接，为指向二叉树父节点指向当前节点的指针的指针（下文简称
 * 链接）。
 * 
 * @param root 指向二叉树根结构体的指针。
 * @param node struct rb_node* 二叉树节点。
 * @param ret_parent 用于返回目标节点父节点的指针。
 * 
 * @return struct rb_node** 二叉树节点有有父节点时返回该节点在其父节点上的链接指针，否则
 *                          返回NULL。
 */
extern struct rb_node **__rb_node_link(
    struct rb_root *root,
    struct rb_node *node,
    struct rb_node **ret_parent
);

/**
 * @brief 获得顺序遍历二叉树时的第一个二叉树节点。
 * 
 * @param root 指向二叉树根结构体的指针。
 * 
 * @return struct rb_node* 如果二叉树不为空，返回第一个节点，否则返回NULL。
 */
extern struct rb_node *rb_first_node(struct rb_root *root);

/**
 * @brief 获得顺序遍历二叉树时的最后一个二叉树节点。
 * 
 * @param root 指向二叉树根结构体的指针。
 * 
 * @return struct rb_node* 如果二叉树不为空，返回最后一个节点，否则返回NULL。
 */
extern struct rb_node *rb_last_node(struct rb_root *root);

/**
 * @brief 获得顺序遍历二叉树时的下一个二叉树节点。
 * 
 * @param node 当前的二叉树节点。
 * 
 * @return struct rb_node* 如果有下一个二叉树节点返回下一个节点，否则返回NULL。
 */
extern struct rb_node *rb_next_node(struct rb_node *node);

/**
 * @brief 获得顺序遍历二叉树时的上一个二叉树节点。
 * 
 * @param node 当前的二叉树节点。
 * 
 * @return struct rb_node* 如果有上一个二叉树节点返回上一个节点，否则返回NULL。
 */
extern struct rb_node *rb_prev_node(struct rb_node *node);

/**
 * @brief 二叉树树根的初始化。
 * 
 * 初始化一个二叉树的根结构体，使其成为一个空的二叉树。
 * 
 * 因为二叉树的节点仅仅包括其颜色以及对其前驱、后继节点的指针，不直接包括一个节点所对应的key值，
 * 所以需要指定两个函数，分别用于获得二叉树节点的key值，和对比二叉树节点和某个key值的大小关系。
 * 上述的两个函数必须给出，否则将会初始化失败。
 * 
 * @param root 进行初始化的二叉树根结构体。
 * @param opts 操作红黑树的方法的函数指针集。
 * 
 * @return struct rb_root* 初始化成功返回初始化后的二叉树根结构体，否则返回NULL。
 */
static inline struct rb_root *rb_root_init(
    struct rb_root  *root,
    struct rb_root_opts *opts
)
{
    if (root == NULL || opts == NULL) {
        return NULL;
    }
    if (opts->keys_cmp_fun == NULL || opts->get_key_fun == NULL) {
        return NULL;
    }
    root->node = NULL;
    root->opts = opts;
    return root;
}

/**
 * @brief 二叉树节点的初始化。
 * 
 * 通过指向二叉树节点父节点的指针指向这个节点本身，将一个二叉树节点初始化为空节点。
 * 
 * @param node 进行初始化的二叉树节点。
 * 
 * @return struct rb_node* 初始化成功返回初始化后的二叉树节点，否则返回NULL。
 */
static inline struct rb_node *rb_node_init(struct rb_node *node)
{
    if (node == NULL) {
        return NULL;
    }
    node->__parent_color = (unsigned long)node;
    return node;
}

/**
 * @brief 链接两个节点。
 * 
 * @param parent 父节点。
 * @param node 当前节点。
 * @param link 当前节点在其父节点中的链接。
 */
static inline void __rb_link_nodes(
    struct rb_node *parent,
    struct rb_node *node,
    struct rb_node **link
)
{
    rb_set_parent(node, parent);
    *link = node;
}

/**
 * @brief 断开节点与父节点。
 * 
 * @param node 当前节点。
 * @param link 当前节点在其父节点中的链接。
 */
static inline void __rb_delink_node(
    struct rb_node **link,
    struct rb_node *node
)
{
    *link = NULL;
    rb_set_parent(node, node);
}

/**
 * @brief 用一个新的节点替换一个旧的节点。
 * 
 * @param link 旧节点在其父节点中的链接。
 * @param new_node 用于替换的节点。
 * @param old_node 被替换的节点。
 */
extern void __rb_replace_node(
    struct rb_node **link,
    struct rb_node *new_node,
    struct rb_node *old_node
);

/**
 * @brief 用一个新的子树替换一个旧的子树。
 * 
 * @param link 旧节点在其父节点中的链接。
 * @param new_node 用于替换的子树的根节点。
 * @param old_node 被替换的子树的根节点。
 */
extern void __rb_transplant(
    struct rb_root *root, 
    struct rb_node *new_node,
    struct rb_node *old_node 
);

/**
 * @brief 得到一个key值在红黑树中对应的链接。
 * 
 * @param root 进行寻找的红黑树根结构体。
 * @param[out] ret_parent 用于返回目标节点父节点的指针。
 * @param key 目标的key值。
 * 
 * @return struct rb_node** 返回key值在红黑树中对应的链接。
 */
extern struct rb_node **__rb_find_key_link(
    struct rb_root *root,
    struct rb_node **ret_parent,
    unsigned long key
);

/**
 * @brief 查找最接近且大于等于一个值的节点。
 * 
 * @param root 进行寻找的红黑树根结构体。
 * @param key 目标的key值。
 * 
 * @return struct rb_node* 如果找到key值对应的节点，返回指向该节点的指针，否则返回NULL。
 */
extern struct rb_node *rb_find_key_rup(struct rb_root *root, uint64 key);

/**
 * @brief 查找最接近且小于等于一个值的节点。
 * 
 * @param root 进行寻找的红黑树根结构体。
 * @param key 目标的key值。
 * 
 * @return struct rb_node* 如果找到key值对应的节点，返回指向该节点的指针，否则返回NULL。
 */
extern struct rb_node *rb_find_key_rdown(struct rb_root *root, uint64 key);

/**
 * @brief 查找一个值的节点。
 * 
 * @param root 进行寻找的红黑树根结构体。
 * @param key 目标的key值。
 * 
 * @return struct rb_node* 如果找到key值对应的节点，返回指向该节点的指针，否则返回NULL。
 */
extern struct rb_node *rb_find_key(struct rb_root *root, unsigned long key);

/**
 * @brief 插入一个节点。
 * 
 * 如果新节点的key值没有在二叉树中的发生冲突，插入新节点后返回新节点的指针。如果新节点的key值
 * 发生冲突，直接返回原本对应该key值的节点的指针。
 * 
 * @param root 二叉树根结构体。
 * @param new_node 进行插入的二叉树节点。
 * 
 * @return struct rb_node*
 */
extern struct rb_node *rb_insert_node(
    struct rb_root *root,
    struct rb_node *new_node
);

/**
 * @brief 删除一个节点。
 * 
 * 如果在二叉树中找到key值对应的节点，将其删除后返回这个节点的指针；如果没有找到，直接返回
 * NULL。
 * 
 * @param root 二叉树根结构体。
 * @param key 进行删除的二叉树节点的key值。
 * 
 * @return struct rb_node*
 */
extern struct rb_node *rb_delete_key(struct rb_root *root, unsigned long key);

/**
 * @brief 二叉树节点的左旋。
 * 
 * 对二叉树进行左旋操作，该操作不会破坏二叉查找树的特性。
 * 
 * @param root 二叉树节点所在的二叉树根结构体。
 * @param node 进行旋转的二叉树节点。
 * 
 * @return struct rb_node* 完成旋转操作后，指向位于进行旋转的二叉树节点原来位置的新节点。
 */
extern struct rb_node *__rb_rotate_left(
    struct rb_root *root,
    struct rb_node *node
);

/**
 * @brief 二叉树节点的右旋。
 * 
 * 对二叉树进行右旋操作，该操作不会破坏二叉查找树的特性。
 * 
 * @param root 二叉树节点所在的二叉树根结构体。
 * @param node 进行旋转的二叉树节点。
 * 
 * @return struct rb_node* 完成旋转操作后，指向位于进行旋转的二叉树节点原来位置的新节点。
 */
extern struct rb_node *__rb_rotate_right(
    struct rb_root *root,
    struct rb_node *node
);

/**
 * @brief 获得指向不为NULL的二叉树节点所在结构体的指针。
 * 
 * 对 container_of 的封装。
 */
#define rb_entry(ptr, type, member) container_of(ptr, type, member)

/**
 * @brief 获得指向二叉树节点所在结构体的指针。
 * 
 * 该宏会对传入的二叉树节点指针进行检查。如果传入的指针为NULL，直接返回NULL；确定传入的指针
 * 不为NULL后，才返回通过 rb_entry 得到的结构体指针。
 * 
 * @param ptr struct rb_node* 指向二叉树节点的指针。
 * @param type 二叉树节点所在结构体的类型。
 * @param member 二叉树节点在其所在结构体内的成员名。
 * 
 * @return type* 如果 ptr 不为NULL，返回其所在结构体的指针，否则返回NULL。
 */
#define rb_entry_safe(ptr, type, member) ({  \
    type *__ptr = NULL; \
    if ((ptr) != NULL) {    \
        __ptr = rb_entry(ptr, type, member);    \
    }   \
    __ptr;  \
})

/**
 * @brief 获得顺序遍历二叉树时的第一个二叉树节点所在的结构体。
 * 
 * @param root struct rb_root* 指向二叉树根结构体的指针。
 * @param type 二叉树节点所在结构体的类型。
 * @param member 二叉树节点在其所在结构体内的成员名。
 * 
 * @return type* 二叉树不为空时返回第一个节点所在结构体，否则返回NULL。
 */
#define rb_first_entry_safe(root, type, member)  \
    rb_entry_safe(rb_first_node(root), type, member)

/**
 * @brief 获得顺序遍历二叉树时的最后一个二叉树节点所在的结构体。
 * 
 * @param root struct rb_root* 指向二叉树根结构体的指针。
 * @param type 二叉树节点所在结构体的类型。
 * @param member 二叉树节点在其所在结构体内的成员名。
 * 
 * @return type* 二叉树不为空时返回最后一个节点所在结构体，否则返回NULL。
 */
#define rb_last_entry_safe(root, type, member)  \
    rb_entry_safe(rb_last_node(root), type, member)

/**
 * @brief 获得顺序遍历二叉树时的下一个二叉树节点所在的结构体。
 * 
 * @param this_entry type* 指向当前二叉树节点所在的结构体。
 * @param member 二叉树节点在其所在结构体内的成员名。
 * 
 * @return type* 如果有下一个节点，返回下一个节点所在结构体的指针，否则返回NULL。
 */
#define rb_next_entry_safe(this_entry, member)  ({  \
    struct rb_node *__node = NULL;  \
    if ((this_entry) != NULL) { \
        __node = &((this_entry)->member);   \
    }   \
    rb_entry_safe(rb_next_node(__node), typeof(*(this_entry)), member);  \
})

/**
 * @brief 获得顺序遍历二叉树时的上一个二叉树节点所在的结构体。
 * 
 * @param this_entry type* 指向当前二叉树节点所在的结构体。
 * @param member 二叉树节点在其所在结构体内的成员名。
 * 
 * @return type* 如果有上一个节点，返回上一个节点所在结构体的指针，否则返回NULL。
 */
#define rb_prev_entry_safe(this_entry, member)   ({  \
    struct rb_node *__node = NULL;  \
    if ((this_entry) != NULL) { \
        __node = &((this_entry)->member);   \
    }   \
    rb_entry_safe(rb_prev_node(__node), typeof(*(this_entry)), member);  \
})

/**
 * @brief 顺序遍历整个二叉树。
 * 
 * 对for循环的封装，从最左下角到最右下角的节点顺序遍历整个二叉树。
 * 
 * @param root struct rb_root* 指向二叉树根结构体的指针。
 * @param pos type* 作为游标存放二叉树节点所在结构体的指针，遍历整个二叉树。
 * @param n type* 和pos一样的作用。
 * @param member 二叉树节点在其所在结构体内的成员名。
 */
#define rb_foreach_entry_safe(root, pos, n, member) \
    for (   (pos) = rb_first_entry_safe(root, typeof(*(pos)), member),  \
            (n) = rb_next_entry_safe(pos, member);  \
            (pos) != NULL;  \
            (pos) = (n),    \
            (n) = rb_next_entry_safe(pos, member))

/**
 * @brief 逆序遍历整个二叉树。
 * 
 * 对for循环的封装，从最右下角到最左下角的节点逆序遍历整个二叉树。
 * 
 * @param root struct rb_root* 指向二叉树根结构体的指针。
 * @param pos type* 作为游标存放二叉树节点所在结构体的指针，遍历整个二叉树。
 * @param n type* 和pos一样的作用。
 * @param member 二叉树节点在其所在结构体内的成员名。
 */
#define rb_foreach_entry_safe_inv(root, pos, n, member) \
    for (   (pos) = rb_last_entry_safe(root, typeof(*(pos)), member),   \
            (n) = rb_prev_entry_safe(pos, member);  \
            (pos) != NULL;  \
            (pos) = (n),    \
            (n) = rb_prev_entry_safe(pos, member))

#endif // __KERNEL_BIN_TREE_H__
