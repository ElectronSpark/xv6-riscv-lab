/**
 * @file maple_tree.c
 * @brief Maple tree implementation — RCU-safe B-tree for index ranges.
 *
 * Simplified from the Linux kernel maple tree for xv6.
 *
 * Design:
 *  - Single node type with MAPLE_NODE_SLOTS (16) slots.
 *  - Leaf nodes store void* entries; internal nodes store child pointers.
 *  - Gap tracking in internal nodes for efficient free-range search.
 *  - RCU-safe reads: readers use rcu_dereference() on slot pointers.
 *  - Write-side is externally locked (vm->rw_lock or similar).
 *  - Freed nodes are reclaimed via call_rcu().
 *
 * Range encoding:
 *  - pivot[i] holds the *inclusive* upper bound of slot[i].
 *  - slot[i] covers the range (pivot[i-1], pivot[i]] where pivot[-1] = min
 *    of the node and pivot[slot_len-1] = max of the node.
 *  - The last valid slot extends to the node's max (from parent context).
 */

#include "types.h"
#include "defs.h"
#include "printf.h"
#include "string.h"
#include "maple_tree.h"
#include <mm/slab.h>
#include "lock/rcu.h"
#include <smp/atomic.h>
#include "errno.h"

/* ====================================================================== */
/*  Node allocator                                                         */
/* ====================================================================== */

static slab_cache_t __mt_node_cache;
static int __mt_node_cache_initialized = 0;

void maple_tree_init(void)
{
    if (__mt_node_cache_initialized)
        return;
    slab_cache_init(&__mt_node_cache, "maple_node",
                    sizeof(struct maple_node),
                    SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
    __mt_node_cache_initialized = 1;
}

static struct maple_node *mt_alloc_node(void)
{
    struct maple_node *node = slab_alloc(&__mt_node_cache);
    if (node == NULL)
        return NULL;
    memset(node, 0, sizeof(*node));
    node->type = maple_leaf_64;
    node->slot_len = 0;
    node->parent = 0;
    return node;
}

static void __mt_free_rcu_cb(void *data)
{
    struct maple_node *node = (struct maple_node *)data;
    slab_free(node);
}

static void __attribute__((unused)) mt_free_node_rcu(struct maple_node *node)
{
    if (node == NULL)
        return;
    call_rcu(&node->rcu_head, __mt_free_rcu_cb, node);
}

static void mt_free_node_now(struct maple_node *node)
{
    if (node == NULL)
        return;
    slab_free(node);
}

/* ====================================================================== */
/*  Tagged-pointer helpers for ma_root                                     */
/* ====================================================================== */

static inline int mt_is_node(const void *ptr)
{
    return ((uint64)ptr & MAPLE_ROOT_NODE) != 0;
}

static inline struct maple_node *mt_to_node(const void *ptr)
{
    return (struct maple_node *)((uint64)ptr & ~MAPLE_ROOT_NODE);
}

static inline void *mt_mk_root(struct maple_node *node)
{
    return (void *)((uint64)node | MAPLE_ROOT_NODE);
}

/* ====================================================================== */
/*  Parent-pointer encoding                                                */
/* ====================================================================== */
/*
 * node->parent encoding:
 *   bit 0:  1 = this node is the root of the tree
 *   rest:   pointer to parent node (only bit 0 used for flag;
 *           safe because slab-allocated nodes are >= 8-byte aligned).
 * node->parent_slot:
 *   slot index within parent (0..15), stored in a separate field.
 */

#define MAPLE_PARENT_ROOT   0x01UL

static inline void mn_set_parent(struct maple_node *node,
                                 struct maple_node *parent, uint8 slot)
{
    node->parent = (uint64)parent;   /* low bit is 0 for aligned ptrs */
    node->parent_slot = slot;
}

static inline void mn_set_root(struct maple_node *node)
{
    node->parent = MAPLE_PARENT_ROOT;
    node->parent_slot = 0;
}

static inline int mn_is_root(const struct maple_node *node)
{
    return (node->parent & MAPLE_PARENT_ROOT) != 0;
}

static inline struct maple_node *mn_get_parent(const struct maple_node *node)
{
    return (struct maple_node *)(node->parent & ~0x1UL);
}

static inline uint8 mn_get_parent_slot(const struct maple_node *node)
{
    return node->parent_slot;
}

static inline int mn_is_leaf(const struct maple_node *node)
{
    return node->type == maple_leaf_64;
}

/* ====================================================================== */
/*  Pivot helpers                                                          */
/* ====================================================================== */

/**
 * mn_pivot() - Get the upper-bound of slot @i.
 *
 * For the last slot (i == slot_len - 1), the pivot is the node's contextual
 * max, which must be passed via @node_max.
 */
static inline uint64 mn_pivot(const struct maple_node *node, uint8 i,
                               uint64 node_max)
{
    if (i >= node->slot_len - 1)
        return node_max;
    return node->pivot[i];
}

/**
 * mn_slot_min() - Get the lower-bound of slot @i.
 *
 * slot 0's min is the node's contextual min (passed as @node_min).
 * For slot i > 0, it is pivot[i-1] + 1.
 */
static inline uint64 mn_slot_min(const struct maple_node *node, uint8 i,
                                  uint64 node_min)
{
    if (i == 0)
        return node_min;
    return node->pivot[i - 1] + 1;
}

/* ====================================================================== */
/*  Pivot binary search                                                    */
/* ====================================================================== */

/**
 * mn_find_slot() - Binary-search the pivot array for the slot containing @index.
 *
 * Returns the smallest slot i such that index <= pivot[i], or len-1 if index
 * is beyond all explicit pivots (i.e. falls into the last slot whose upper
 * bound is the implicit node-max).
 *
 * @node:  the maple node
 * @index: the key to search for
 * @len:   node->slot_len (number of valid slots)
 *
 * Requires len >= 1.  Reads pivots with READ_ONCE for RCU safety.
 */
static inline uint8 mn_find_slot(const struct maple_node *node,
                                 uint64 index, uint8 len)
{
    uint8 lo = 0;
    uint8 hi = len - 1;          /* last slot has no explicit pivot */
    while (lo < hi) {
        uint8 mid = (lo + hi) / 2;
        uint64 piv = READ_ONCE(node->pivot[mid]);
        if (index <= piv)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

/**
 * mn_find_slot_from() - Like mn_find_slot but starts the search at @from.
 *
 * Useful when the caller already knows index > pivot[from-1].
 */
static inline uint8 mn_find_slot_from(const struct maple_node *node,
                                      uint64 index, uint8 from, uint8 len)
{
    uint8 lo = from;
    uint8 hi = len - 1;
    while (lo < hi) {
        uint8 mid = (lo + hi) / 2;
        uint64 piv = READ_ONCE(node->pivot[mid]);
        if (index <= piv)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

/* ====================================================================== */
/*  Walk / lookup                                                          */
/* ====================================================================== */

/**
 * __mt_lookup() - Find the entry containing @index in the tree rooted at @root.
 *
 * Internal lookup used by mtree_load, mt_find, etc.
 * Uses rcu_dereference() on slot pointers for RCU-safe traversal.
 */
static void *__mt_lookup(struct maple_tree *mt, uint64 index)
{
    void *root = rcu_dereference(mt->ma_root);
    if (root == NULL)
        return NULL;
    if (!mt_is_node(root)) {
        /* Single-entry tree: only covers index 0..MAPLE_MAX */
        return root;
    }

    struct maple_node *node = mt_to_node(root);
    uint64 node_min = 0;
    uint64 node_max = MAPLE_MAX;

    while (1) {
        uint8 len = READ_ONCE(node->slot_len);
        if (len == 0)
            return NULL;

        /* Binary search for the slot containing index. */
        uint8 slot = mn_find_slot(node, index, len);

        void *entry = rcu_dereference(node->slot[slot]);

        if (mn_is_leaf(node))
            return entry;

        /* Internal node: descend. */
        if (entry == NULL)
            return NULL;

        node_min = mn_slot_min(node, slot, node_min);
        node_max = mn_pivot(node, slot, node_max);
        node = (struct maple_node *)entry;
    }
}

void *mtree_load(struct maple_tree *mt, uint64 index)
{
    void *entry;
    rcu_read_lock();
    entry = __mt_lookup(mt, index);
    rcu_read_unlock();
    return entry;
}

/* ====================================================================== */
/*  MAS walk — descend and populate mas state                              */
/* ====================================================================== */

void *mas_walk(struct ma_state *mas)
{
    struct maple_tree *mt = mas->tree;
    void *root = rcu_dereference(mt->ma_root);

    mas->depth = 0;

    if (root == NULL) {
        mas->node = NULL;
        return NULL;
    }

    if (!mt_is_node(root)) {
        mas->node = NULL;
        mas->min = 0;
        mas->max = MAPLE_MAX;
        mas->offset = 0;
        return root;
    }

    struct maple_node *node = mt_to_node(root);
    uint64 node_min = 0;
    uint64 node_max = MAPLE_MAX;

    while (1) {
        mas->depth++;
        uint8 len = node->slot_len;
        if (len == 0) {
            mas->node = node;
            mas->min = node_min;
            mas->max = node_max;
            mas->offset = 0;
            return NULL;
        }

        uint8 slot = mn_find_slot(node, mas->index, len);

        if (mn_is_leaf(node)) {
            mas->node = node;
            mas->min = mn_slot_min(node, slot, node_min);
            mas->max = mn_pivot(node, slot, node_max);
            mas->offset = slot;
            return node->slot[slot];
        }

        void *child = node->slot[slot];
        if (child == NULL) {
            mas->node = node;
            mas->min = mn_slot_min(node, slot, node_min);
            mas->max = mn_pivot(node, slot, node_max);
            mas->offset = slot;
            return NULL;
        }

        node_min = mn_slot_min(node, slot, node_min);
        node_max = mn_pivot(node, slot, node_max);
        node = (struct maple_node *)child;
    }
}

/* ====================================================================== */
/*  Node splitting                                                         */
/* ====================================================================== */

/**
 * __mt_split_node() - Split a full leaf node.
 *
 * Splits @node into @node (left, first half) and @right (second half),
 * and propagates the median pivot up to the parent.  If the parent is
 * also full, recurse upward.  If we're splitting the root, create a new
 * root.
 *
 * @mt:       the maple tree
 * @node:     the leaf node being split (must be full: slot_len == MAPLE_NODE_SLOTS)
 * @node_min: minimum index for @node
 * @node_max: maximum index for @node
 *
 * Returns 0 on success, -ENOMEM on allocation failure.
 */
static int __mt_split_node(struct maple_tree *mt, struct maple_node *node,
                           uint64 node_min, uint64 node_max);

static void __find_node_bounds(struct maple_tree *mt, struct maple_node *node,
                               uint64 *out_min, uint64 *out_max);

/**
 * __mt_insert_into_parent() - Insert a new child (@right) into @parent
 *                             after slot @slot with pivot @pivot_val.
 *
 * If the parent is full, split it first.
 */
static int __mt_insert_into_parent(struct maple_tree *mt,
                                   struct maple_node *parent,
                                   uint8 slot,
                                   uint64 pivot_val,
                                   struct maple_node *right,
                                   uint64 parent_min,
                                   uint64 parent_max)
{
    if (parent->slot_len < MAPLE_NODE_SLOTS) {
        /* Shift slots/pivots right to make room. */
        uint8 len = parent->slot_len;
        for (int i = len; i > slot + 1; i--) {
            parent->slot[i] = parent->slot[i - 1];
            if (i - 1 < MAPLE_NODE_PIVOTS)
                parent->pivot[i - 1] = parent->pivot[i - 2];
            if (!mn_is_leaf(parent))
                parent->gap[i] = parent->gap[i - 1];
        }
        parent->slot[slot + 1] = right;
        parent->pivot[slot] = pivot_val;
        parent->slot_len = len + 1;

        mn_set_parent(right, parent, slot + 1);

        /* Update parent pointers' slot indices for shifted children. */
        if (!mn_is_leaf(parent)) {
            for (uint8 i = slot + 2; i < parent->slot_len; i++) {
                struct maple_node *child = parent->slot[i];
                if (child != NULL)
                    mn_set_parent(child, parent, i);
            }
        }
        return 0;
    }

    /* Parent is full — split it first, then retry the insertion. */

    /* Save the left child (node at @slot) before the parent split
     * so we can find its updated location afterward. */
    struct maple_node *left_child = parent->slot[slot];

    int ret = __mt_split_node(mt, parent, parent_min, parent_max);
    if (ret != 0)
        return ret;

    /*
     * After splitting the parent, left_child's parent pointer was
     * updated by __mt_split_node (it may now be in the left or right
     * half of the split parent).  Use it to find the correct parent
     * for reinsertion.
     */
    struct maple_node *new_parent = mn_get_parent(left_child);
    uint8 new_slot = mn_get_parent_slot(left_child);

    uint64 new_pmin, new_pmax;
    __find_node_bounds(mt, new_parent, &new_pmin, &new_pmax);

    return __mt_insert_into_parent(mt, new_parent, new_slot, pivot_val,
                                   right, new_pmin, new_pmax);
}

/**
 * __find_node_bounds() - Walk up to find a node's min/max range from context.
 */
static void __find_node_bounds(struct maple_tree *mt, struct maple_node *node,
                               uint64 *out_min, uint64 *out_max)
{
    if (mn_is_root(node)) {
        *out_min = 0;
        *out_max = MAPLE_MAX;
        return;
    }

    struct maple_node *parent = mn_get_parent(node);
    uint64 pmin, pmax;
    __find_node_bounds(mt, parent, &pmin, &pmax);

    uint8 slot = mn_get_parent_slot(node);
    *out_min = mn_slot_min(parent, slot, pmin);
    *out_max = mn_pivot(parent, slot, pmax);
}

static int __mt_split_node(struct maple_tree *mt, struct maple_node *node,
                           uint64 node_min, uint64 node_max)
{
    struct maple_node *right = mt_alloc_node();
    if (right == NULL)
        return -ENOMEM;

    uint8 split = node->slot_len / 2;   /* median index */
    uint64 median = node->pivot[split - 1];
    right->type = node->type;

    /* Copy upper half to right node. */
    uint8 right_len = node->slot_len - split;
    for (uint8 i = 0; i < right_len; i++) {
        right->slot[i] = node->slot[split + i];
        if (i < right_len - 1 && (split + i) < MAPLE_NODE_PIVOTS)
            right->pivot[i] = node->pivot[split + i];
        if (!mn_is_leaf(node))
            right->gap[i] = node->gap[split + i];
    }
    right->slot_len = right_len;

    /* Fix child parent pointers if internal. */
    if (!mn_is_leaf(right)) {
        for (uint8 i = 0; i < right_len; i++) {
            struct maple_node *child = right->slot[i];
            if (child != NULL)
                mn_set_parent(child, right, i);
        }
    }

    /* Truncate left node. */
    node->slot_len = split;

    if (mn_is_root(node)) {
        /* Create new root. */
        struct maple_node *new_root = mt_alloc_node();
        if (new_root == NULL) {
            /* Undo split. Restore node. */
            for (uint8 i = 0; i < right_len; i++) {
                node->slot[split + i] = right->slot[i];
                if (i < right_len - 1 && (split + i) < MAPLE_NODE_PIVOTS)
                    node->pivot[split + i] = right->pivot[i];
                if (!mn_is_leaf(node))
                    node->gap[split + i] = right->gap[i];
            }
            node->slot_len = split + right_len;
            if (!mn_is_leaf(node)) {
                for (uint8 i = split; i < node->slot_len; i++) {
                    struct maple_node *child = node->slot[i];
                    if (child != NULL)
                        mn_set_parent(child, node, i);
                }
            }
            mt_free_node_now(right);
            return -ENOMEM;
        }
        new_root->type = maple_arange_64;
        new_root->slot[0] = node;
        new_root->slot[1] = right;
        new_root->pivot[0] = median;
        new_root->slot_len = 2;

        mn_set_root(new_root);
        mn_set_parent(node, new_root, 0);
        mn_set_parent(right, new_root, 1);

        rcu_assign_pointer(mt->ma_root, mt_mk_root(new_root));
        return 0;
    }

    /* Non-root: insert right into parent. */
    struct maple_node *parent = mn_get_parent(node);
    uint8 pslot = mn_get_parent_slot(node);

    uint64 pmin, pmax;
    __find_node_bounds(mt, parent, &pmin, &pmax);

    return __mt_insert_into_parent(mt, parent, pslot, median, right,
                                   pmin, pmax);
}

/* ====================================================================== */
/*  Store (insert / overwrite)                                             */
/* ====================================================================== */

/**
 * __mt_store_leaf() - Store an entry in a leaf node at a specific slot.
 *
 * Handles the case where the entry exactly replaces an existing slot,
 * and the case where a slot must be split into up to 3 sub-ranges.
 */
static int __mt_store_leaf(struct maple_tree *mt, struct maple_node *node,
                           uint8 slot, uint64 node_min, uint64 node_max,
                           uint64 first, uint64 last, void *entry)
{
    uint64 slot_min = mn_slot_min(node, slot, node_min);
    uint64 slot_max = mn_pivot(node, slot, node_max);

    /* Case 1: Exact match — just overwrite. */
    if (slot_min == first && slot_max == last) {
        rcu_assign_pointer(node->slot[slot], entry);
        return 0;
    }

    /*
     * Case 2: The new range [first, last] is a sub-range of [slot_min, slot_max].
     * We may need to insert 1 or 2 new slots.
     *
     * Before: slot[i] = old_entry covering [slot_min, slot_max]
     * After:
     *   [slot_min, first-1]  → old_entry   (only if first > slot_min)
     *   [first, last]        → entry
     *   [last+1, slot_max]   → old_entry   (only if last < slot_max)
     */
    void *old_entry = node->slot[slot];
    int need_left  = (first > slot_min) ? 1 : 0;
    int need_right = (last < slot_max) ? 1 : 0;
    int extra = need_left + need_right;

    /* If this adds extra slots, check capacity. */
    if (node->slot_len + extra > MAPLE_NODE_SLOTS) {
        /* Need to split this node first, then retry. */
        int ret = __mt_split_node(mt, node, node_min, node_max);
        if (ret != 0)
            return ret;
        /* After split, the tree structure changed. Re-walk from root. */
        return mtree_store_range(mt, first, last, entry);
    }

    /*
     * Build the replacement pieces.  We'll construct the
     * new slots/pivots for positions [slot .. slot+extra] in
     * temporary arrays, then shift trailing slots and write them in.
     */
    int n_pieces = 1 + need_left + need_right;  /* 1..3 */
    void  *p_slot[3];
    uint64 p_pivot[3];   /* upper bound for each piece */

    int pi = 0;
    if (need_left) {
        p_slot[pi]  = old_entry;
        p_pivot[pi] = first - 1;
        pi++;
    }
    p_slot[pi]  = entry;
    p_pivot[pi] = last;
    pi++;
    if (need_right) {
        p_slot[pi]  = old_entry;
        p_pivot[pi] = slot_max;
        pi++;
    }

    /* Shift existing slots after 'slot' right by 'extra' positions. */
    uint8 old_len = node->slot_len;
    uint8 new_len = old_len + extra;

    /* Work backwards to avoid overwriting. */
    for (int i = new_len - 1; i >= (int)slot + n_pieces; i--) {
        int src = i - extra;
        node->slot[i] = node->slot[src];
        /* Copy pivot: pivot[i-1] if not last slot, from pivot[src-1]. */
        /* Pivots are indexed 0..len-2 for a len-slot node.  The pivot for
         * slot j is pivot[j] (upper bound of slot j), except for the last
         * slot whose upper bound is implicit (node_max). */
        if (src < (int)old_len - 1)
            node->pivot[i] = node->pivot[src];
    }

    /* Write the pieces into positions [slot .. slot + n_pieces - 1]. */
    for (int i = 0; i < n_pieces; i++) {
        int dst = slot + i;
        node->slot[dst] = p_slot[i];
        /* Write explicit pivot for every piece except potentially the last
         * slot of the whole node (whose upper bound is node_max). */
        if (dst < (int)new_len - 1)
            node->pivot[dst] = p_pivot[i];
    }

    node->slot_len = new_len;
    return 0;
}

/**
 * __mt_coalesce_leaf() - After a store, merge adjacent slots with the same
 *                        entry value to reduce slot count.
 */
static void __mt_coalesce_leaf(struct maple_node *node, uint64 node_max)
{
    if (node->slot_len <= 1)
        return;

    uint8 dst = 0;
    for (uint8 src = 1; src < node->slot_len; src++) {
        if (node->slot[src] == node->slot[dst]) {
            /* Merge src into dst — extend dst's range to cover src. */
            if (src < node->slot_len - 1)
                node->pivot[dst] = node->pivot[src];
            /* else dst becomes the last slot; its pivot is implicit (node_max). */
        } else {
            /* Move src up to dst+1. */
            dst++;
            if (dst != src) {
                node->slot[dst] = node->slot[src];
                if (src < node->slot_len - 1 && dst < MAPLE_NODE_PIVOTS)
                    node->pivot[dst] = node->pivot[src];
            }
        }
    }
    node->slot_len = dst + 1;
}

/*
 * Full store_range implementation: walk down to the leaf covering
 * [first, last], and store the entry.  Handles spanning multiple
 * existing slots by overwriting each affected slot.
 */
int mtree_store_range(struct maple_tree *mt, uint64 first, uint64 last,
                      void *entry)
{
    if (first > last)
        return -EINVAL;

    void *root = rcu_dereference(mt->ma_root);

    /* Empty tree: create a single-node tree. */
    if (root == NULL) {
        struct maple_node *node = mt_alloc_node();
        if (node == NULL)
            return -ENOMEM;
        node->type = maple_leaf_64;
        if (first > 0) {
            /* [0, first-1] → NULL, [first, last] → entry */
            node->slot[0] = NULL;
            node->pivot[0] = first - 1;
            node->slot[1] = entry;
            if (last < MAPLE_MAX) {
                node->pivot[1] = last;
                node->slot[2] = NULL;
                node->slot_len = 3;
            } else {
                node->slot_len = 2;
            }
        } else {
            /* [0, last] → entry */
            node->slot[0] = entry;
            if (last < MAPLE_MAX) {
                node->pivot[0] = last;
                node->slot[1] = NULL;
                node->slot_len = 2;
            } else {
                node->slot_len = 1;
            }
        }
        mn_set_root(node);
        rcu_assign_pointer(mt->ma_root, mt_mk_root(node));
        return 0;
    }

    if (!mt_is_node(root)) {
        /* Single-entry shortcut in ma_root — convert to node. */
        struct maple_node *node = mt_alloc_node();
        if (node == NULL)
            return -ENOMEM;
        node->type = maple_leaf_64;
        node->slot[0] = root;
        node->slot_len = 1;
        mn_set_root(node);
        rcu_assign_pointer(mt->ma_root, mt_mk_root(node));
        /* Fall through to node-based store. */
        root = rcu_dereference(mt->ma_root);
    }

    struct maple_node *node = mt_to_node(root);
    uint64 node_min = 0;
    uint64 node_max = MAPLE_MAX;

    /* Descend to the leaf containing 'first'. */
    while (!mn_is_leaf(node)) {
        uint8 slot = mn_find_slot(node, first, node->slot_len);
        uint64 child_min = mn_slot_min(node, slot, node_min);
        uint64 child_max = mn_pivot(node, slot, node_max);

        struct maple_node *child = node->slot[slot];
        if (child == NULL) {
            /* Allocate a new leaf for this empty slot. */
            child = mt_alloc_node();
            if (child == NULL)
                return -ENOMEM;
            child->type = maple_leaf_64;
            child->slot[0] = NULL;
            child->slot_len = 1;
            mn_set_parent(child, node, slot);
            rcu_assign_pointer(node->slot[slot], child);
        }
        node_min = child_min;
        node_max = child_max;
        node = child;
    }

    /* We're at a leaf node. Now overwrite all slots covered by [first, last]. */

    /*
     * Clamp last to this leaf's upper bound.  If the requested range
     * extends beyond this leaf, we store the portion that fits here
     * and then recursively handle the remainder.
     */
    uint64 orig_last = last;
    if (last > node_max)
        last = node_max;

    /* Find the first slot affected. */
    uint8 start_slot = mn_find_slot(node, first, node->slot_len);

    /* Find the last slot affected. */
    uint8 end_slot = mn_find_slot_from(node, last, start_slot, node->slot_len);

    /*
     * Simple strategy: handle the common case where we're overwriting
     * exactly one slot or inserting into one slot.
     */
    if (start_slot == end_slot) {
        /* Single slot affected. */
        int ret = __mt_store_leaf(mt, node, start_slot, node_min, node_max,
                                  first, last, entry);
        if (ret != 0)
            return ret;
        __mt_coalesce_leaf(node, node_max);
        if (orig_last > node_max)
            return mtree_store_range(mt, node_max + 1, orig_last, entry);
        return 0;
    }

    /*
     * Multiple slots affected: overwrite all slots in [start_slot, end_slot].
     * Handle the boundaries:
     * - start_slot may need to keep its left portion
     * - end_slot may need to keep its right portion
     */
    uint64 start_slot_min = mn_slot_min(node, start_slot, node_min);
    uint64 end_slot_max = mn_pivot(node, end_slot, node_max);

    /* Save boundary entries before overwriting. */
    void *left_entry = node->slot[start_slot];
    void *right_entry = node->slot[end_slot];

    /* Build new slot array in-place.  We collapse [start_slot..end_slot]
     * into at most 3 slots: left_remainder, entry, right_remainder. */

    /* Calculate how many new slots we need in the range. */
    int need_left   = (first > start_slot_min) ? 1 : 0;
    int need_right  = (last < end_slot_max) ? 1 : 0;
    int new_range_slots = need_left + 1 + need_right;
    int old_range_slots = end_slot - start_slot + 1;
    int delta = new_range_slots - old_range_slots;

    if (node->slot_len + delta > MAPLE_NODE_SLOTS) {
        /* Need to split first. Use a simpler approach: split, then retry. */
        int ret = __mt_split_node(mt, node, node_min, node_max);
        if (ret != 0)
            return ret;
        return mtree_store_range(mt, first, orig_last, entry);
    }

    /* Build the replacement pieces in a temporary buffer. */
    void  *p_slot[3];
    uint64 p_pivot[3];
    int n_pieces = 0;

    if (need_left) {
        p_slot[n_pieces]  = left_entry;
        p_pivot[n_pieces] = first - 1;
        n_pieces++;
    }
    p_slot[n_pieces]  = entry;
    p_pivot[n_pieces] = last;
    n_pieces++;
    if (need_right) {
        p_slot[n_pieces]  = right_entry;
        p_pivot[n_pieces] = end_slot_max;
        n_pieces++;
    }

    uint8 old_len = node->slot_len;
    uint8 new_len = old_len + delta;

    /* Shift trailing slots (those after end_slot) by delta positions. */
    int trail_start_src = end_slot + 1;
    (void)trail_start_src;

    if (delta > 0) {
        /* Expanding: shift right, work backwards. */
        for (int i = (int)old_len - 1; i >= trail_start_src; i--) {
            int d = i + delta;
            node->slot[d] = node->slot[i];
            if (i < (int)old_len - 1)
                node->pivot[d] = node->pivot[i];
        }
    } else if (delta < 0) {
        /* Shrinking: shift left, work forwards. */
        for (int i = trail_start_src; i < (int)old_len; i++) {
            int d = i + delta;
            node->slot[d] = node->slot[i];
            if (i < (int)old_len - 1)
                node->pivot[d] = node->pivot[i];
        }
    }

    /* Write the replacement pieces. */
    for (int i = 0; i < n_pieces; i++) {
        int dst = start_slot + i;
        node->slot[dst] = p_slot[i];
        if (dst < (int)new_len - 1)
            node->pivot[dst] = p_pivot[i];
    }

    node->slot_len = new_len;
    __mt_coalesce_leaf(node, node_max);
    if (orig_last > node_max)
        return mtree_store_range(mt, node_max + 1, orig_last, entry);
    return 0;
}

/* ====================================================================== */
/*  Erase                                                                  */
/* ====================================================================== */

void *mtree_erase(struct maple_tree *mt, uint64 index)
{
    void *root = rcu_dereference(mt->ma_root);
    if (root == NULL)
        return NULL;

    if (!mt_is_node(root)) {
        rcu_assign_pointer(mt->ma_root, NULL);
        return root;
    }

    struct maple_node *node = mt_to_node(root);
    uint64 node_min = 0;
    uint64 node_max = MAPLE_MAX;

    /* Descend to leaf. */
    while (!mn_is_leaf(node)) {
        uint8 slot = mn_find_slot(node, index, node->slot_len);
        node_min = mn_slot_min(node, slot, node_min);
        node_max = mn_pivot(node, slot, node_max);

        struct maple_node *child = node->slot[slot];
        if (child == NULL)
            return NULL;
        node = child;
    }

    /* Find the slot containing index. */
    uint8 slot = mn_find_slot(node, index, node->slot_len);

    void *old = node->slot[slot];
    node->slot[slot] = NULL;

    /* Coalesce adjacent NULL slots. */
    __mt_coalesce_leaf(node, node_max);
    return old;
}

/* ====================================================================== */
/*  Destroy — free all nodes recursively                                   */
/* ====================================================================== */

static void __mt_destroy_walk(struct maple_node *node)
{
    if (node == NULL)
        return;

    if (!mn_is_leaf(node)) {
        for (uint8 i = 0; i < node->slot_len; i++) {
            struct maple_node *child = node->slot[i];
            if (child != NULL)
                __mt_destroy_walk(child);
        }
    }
    mt_free_node_now(node);
}

/* ====================================================================== */
/*  Debug dump                                                             */
/* ====================================================================== */

static void __mt_dump_node(struct maple_node *node, uint64 node_min,
                           uint64 node_max, int depth)
{
    if (node == NULL) {
        printf("%*s(null node)\n", depth * 2, "");
        return;
    }
    printf("%*snode=%p %s len=%d range=[%lx, %lx] parent=%lx pslot=%d\n",
           depth * 2, "", node,
           mn_is_leaf(node) ? "LEAF" : "INTERNAL",
           node->slot_len, node_min, node_max,
           node->parent, node->parent_slot);
    for (uint8 i = 0; i < node->slot_len; i++) {
        uint64 smin = mn_slot_min(node, i, node_min);
        uint64 smax = mn_pivot(node, i, node_max);
        if (mn_is_leaf(node)) {
            printf("%*s  [%d] [%lx, %lx] -> %p\n",
                   depth * 2, "", i, smin, smax, node->slot[i]);
        } else {
            printf("%*s  [%d] pivot_upper=%lx -> child=%p\n",
                   depth * 2, "", i, smax, node->slot[i]);
            if (node->slot[i] != NULL)
                __mt_dump_node((struct maple_node *)node->slot[i],
                               smin, smax, depth + 1);
        }
    }
    /* Show raw pivots for debugging. */
    printf("%*s  raw pivots:", depth * 2, "");
    for (uint8 i = 0; i < node->slot_len && i < MAPLE_NODE_PIVOTS; i++)
        printf(" [%d]=%lx", i, node->pivot[i]);
    printf("\n");
}

void mt_dump_tree(struct maple_tree *mt)
{
    void *root = rcu_dereference(mt->ma_root);
    printf("=== MAPLE TREE DUMP mt=%p root=%p ===\n", mt, root);
    if (root == NULL) {
        printf("  (empty tree)\n");
        return;
    }
    if (!mt_is_node(root)) {
        printf("  (single entry: %p)\n", root);
        return;
    }
    __mt_dump_node(mt_to_node(root), 0, MAPLE_MAX, 0);
    printf("=== END MAPLE TREE DUMP ===\n");
}

void mtree_destroy(struct maple_tree *mt)
{
    void *root = rcu_dereference(mt->ma_root);
    if (root == NULL)
        return;

    if (mt_is_node(root))
        __mt_destroy_walk(mt_to_node(root));

    rcu_assign_pointer(mt->ma_root, NULL);
}

/* ====================================================================== */
/*  Cursor next / prev                                                     */
/* ====================================================================== */

/**
 * __mas_next_slot() - Advance MAS to the next slot in the same leaf.
 *
 * Returns the entry, or NULL if no more slots.
 */
static void *__mas_next_slot(struct ma_state *mas)
{
    struct maple_node *node = mas->node;
    if (node == NULL)
        return NULL;

    uint8 next = mas->offset + 1;
    if (next >= node->slot_len)
        return NULL;

    uint64 node_min, node_max;
    __find_node_bounds(mas->tree, node, &node_min, &node_max);

    mas->offset = next;
    mas->min = mn_slot_min(node, next, node_min);
    mas->max = mn_pivot(node, next, node_max);
    mas->index = mas->min;
    return node->slot[next];
}

/**
 * __mas_next_node() - Advance to the first slot in the next leaf node
 *                     (in-order successor across nodes).
 */
static void *__mas_next_node(struct ma_state *mas, uint64 limit)
{
    struct maple_node *node = mas->node;
    if (node == NULL)
        return NULL;

    /* Walk up until we find a parent where we came from a non-last slot. */
    while (!mn_is_root(node)) {
        struct maple_node *parent = mn_get_parent(node);
        uint8 pslot = mn_get_parent_slot(node);

        /* Try each right sibling (skip NULL children in internal nodes). */
        for (uint8 next = pslot + 1; next < parent->slot_len; next++) {
            uint64 node_min, node_max;
            __find_node_bounds(mas->tree, parent, &node_min, &node_max);

            uint64 child_min = mn_slot_min(parent, next, node_min);
            if (child_min > limit) {
                mas->node = NULL;
                return NULL;
            }

            struct maple_node *child = parent->slot[next];
            if (child == NULL)
                continue;   /* skip NULL child in internal node */

            if (mn_is_leaf(parent)) {
                /* Parent is a leaf (shouldn't happen, but handle). */
                uint64 child_max = mn_pivot(parent, next, node_max);
                mas->node = parent;
                mas->offset = next;
                mas->min = child_min;
                mas->max = child_max;
                mas->index = child_min;
                return parent->slot[next];
            }

            /* Descend to leftmost leaf of this subtree. */
            node = (struct maple_node *)child;
            while (!mn_is_leaf(node)) {
                if (node->slot[0] == NULL)
                    break;
                node = (struct maple_node *)node->slot[0];
            }

            uint64 nmin, nmax;
            __find_node_bounds(mas->tree, node, &nmin, &nmax);
            mas->node = node;
            mas->offset = 0;
            mas->min = mn_slot_min(node, 0, nmin);
            mas->max = mn_pivot(node, 0, nmax);
            mas->index = mas->min;
            return node->slot[0];
        }
        node = parent;
    }

    /* Reached root going up — no more entries. */
    mas->node = NULL;
    return NULL;
}

void *mas_next(struct ma_state *mas, uint64 max)
{
    /* Advance through slots and nodes, skipping NULLs. */
    while (1) {
        void *entry = __mas_next_slot(mas);
        if (entry != NULL) {
            if (mas->min > max)
                return NULL;
            return entry;
        }

        /* __mas_next_slot returned NULL.  If there are still more slots
         * in this node (the current slot was just NULL), keep scanning. */
        if (mas->node != NULL && mas->offset + 1 < mas->node->slot_len)
            continue;

        /* No more slots in current node.  Move to next node. */
        entry = __mas_next_node(mas, max);
        if (entry != NULL) {
            if (mas->min > max)
                return NULL;
            return entry;
        }
        if (mas->node == NULL)
            return NULL;  /* truly exhausted */
        /* Landed on a new node whose slot[0] is NULL — keep scanning. */
    }
}

/**
 * __mas_prev_slot() - Move MAS to the previous slot.
 */
static void *__mas_prev_slot(struct ma_state *mas)
{
    struct maple_node *node = mas->node;
    if (node == NULL || mas->offset == 0)
        return NULL;

    uint64 node_min, node_max;
    __find_node_bounds(mas->tree, node, &node_min, &node_max);

    uint8 prev = mas->offset - 1;
    mas->offset = prev;
    mas->min = mn_slot_min(node, prev, node_min);
    mas->max = mn_pivot(node, prev, node_max);
    mas->index = mas->min;
    return node->slot[prev];
}

static void *__mas_prev_node(struct ma_state *mas, uint64 limit)
{
    struct maple_node *node = mas->node;
    if (node == NULL)
        return NULL;

    while (!mn_is_root(node)) {
        struct maple_node *parent = mn_get_parent(node);
        uint8 pslot = mn_get_parent_slot(node);

        if (pslot > 0) {
            uint8 prev = pslot - 1;
            node = parent;

            uint64 node_min, node_max;
            __find_node_bounds(mas->tree, node, &node_min, &node_max);

            struct maple_node *child = node->slot[prev];
            if (child == NULL)
                return NULL;

            if (mn_is_leaf(node)) {
                uint64 child_min = mn_slot_min(node, prev, node_min);
                uint64 child_max = mn_pivot(node, prev, node_max);
                if (child_max < limit)
                    return NULL;
                mas->node = node;
                mas->offset = prev;
                mas->min = child_min;
                mas->max = child_max;
                mas->index = child_min;
                return node->slot[prev];
            }

            /* Descend to rightmost leaf of prev subtree. */
            node = (struct maple_node *)child;
            while (!mn_is_leaf(node)) {
                if (node->slot_len == 0)
                    break;
                struct maple_node *last_child = node->slot[node->slot_len - 1];
                if (last_child == NULL)
                    break;
                node = last_child;
            }

            uint64 nmin, nmax;
            __find_node_bounds(mas->tree, node, &nmin, &nmax);
            uint8 last = node->slot_len > 0 ? node->slot_len - 1 : 0;
            mas->node = node;
            mas->offset = last;
            mas->min = mn_slot_min(node, last, nmin);
            mas->max = mn_pivot(node, last, nmax);
            mas->index = mas->min;
            if (mas->max < limit)
                return NULL;
            return node->slot[last];
        }
        node = parent;
    }

    return NULL;
}

void *mas_prev(struct ma_state *mas, uint64 min)
{
    void *entry;

    entry = __mas_prev_slot(mas);
    while (entry == NULL && mas->node != NULL) {
        entry = __mas_prev_node(mas, min);
        if (entry == NULL)
            return NULL;
        if (entry != NULL)
            break;
    }

    if (entry != NULL && mas->max < min)
        return NULL;

    while (entry == NULL) {
        entry = __mas_prev_slot(mas);
        if (entry == NULL) {
            entry = __mas_prev_node(mas, min);
            if (entry == NULL)
                return NULL;
        }
        if (mas->max < min)
            return NULL;
    }

    return entry;
}

/* ====================================================================== */
/*  mas_find — find first non-NULL starting from mas->index                */
/* ====================================================================== */

void *mas_find(struct ma_state *mas, uint64 max)
{
    if (mas->index > max)
        return NULL;

    if (mas->node == NULL) {
        /* Initial state: walk to mas->index. */
        void *entry = mas_walk(mas);
        if (entry != NULL) {
            if (mas->max == MAPLE_MAX)
                mas->index = MAPLE_MAX;
            else
                mas->index = mas->max + 1;
            return entry;
        }
        /* Walk landed on a NULL slot. Try next. */
    } else {
        /* Already positioned. Just advance past current. */
    }

    /* Advance until we find a non-NULL entry. */
    while (1) {
        void *entry = __mas_next_slot(mas);
        if (entry != NULL) {
            if (mas->min > max)
                return NULL;
            if (mas->max == MAPLE_MAX)
                mas->index = MAPLE_MAX;
            else
                mas->index = mas->max + 1;
            return entry;
        }

        /* __mas_next_slot returned NULL.  If there are still more slots
         * in this node (the current slot was just NULL), keep scanning. */
        if (mas->node != NULL && mas->offset + 1 < mas->node->slot_len)
            continue;

        /* No more slots in current node.  Move to next node. */
        entry = __mas_next_node(mas, max);
        if (entry != NULL) {
            if (mas->min > max)
                return NULL;
            if (mas->max == MAPLE_MAX)
                mas->index = MAPLE_MAX;
            else
                mas->index = mas->max + 1;
            return entry;
        }
        if (mas->node == NULL)
            return NULL;  /* truly exhausted */
        /* Landed on a new node whose slot[0] is NULL — keep scanning. */
    }
}

/* ====================================================================== */
/*  mas_store / mas_erase at current position                              */
/* ====================================================================== */

int mas_store(struct ma_state *mas, void *entry)
{
    return mtree_store_range(mas->tree, mas->index, mas->last, entry);
}

void *mas_erase(struct ma_state *mas)
{
    return mtree_erase(mas->tree, mas->index);
}

/* ====================================================================== */
/*  Gap search — find empty ranges                                         */
/* ====================================================================== */

int mas_empty_area(struct ma_state *mas, uint64 min, uint64 max, uint64 size)
{
    if (size == 0 || min > max || max - min + 1 < size)
        return -EBUSY;

    /* Walk the tree looking for a gap of at least 'size' in [min, max]. */
    MA_STATE(walk, mas->tree, min, min);
    void *entry = mas_walk(&walk);

    /* If we landed on a NULL slot, check its range. */
    if (entry == NULL) {
        uint64 gap_start = walk.min < min ? min : walk.min;
        uint64 gap_end = walk.max > max ? max : walk.max;
        if (gap_end >= gap_start && gap_end - gap_start + 1 >= size) {
            mas->index = gap_start;
            mas->last = gap_start + size - 1;
            return 0;
        }
    }

    /* Scan forward for a gap. */
    while (1) {
        entry = __mas_next_slot(&walk);
        if (entry == NULL && walk.node != NULL) {
            /* Check this NULL slot's range. */
            uint64 gap_start = walk.min < min ? min : walk.min;
            uint64 gap_end = walk.max > max ? max : walk.max;
            if (gap_end >= gap_start && gap_end - gap_start + 1 >= size) {
                mas->index = gap_start;
                mas->last = gap_start + size - 1;
                return 0;
            }
            if (walk.min > max)
                return -EBUSY;
            continue;
        }
        if (entry == NULL) {
            /* Try next node. */
            entry = __mas_next_node(&walk, max);
            if (entry == NULL && walk.node != NULL) {
                uint64 gap_start = walk.min < min ? min : walk.min;
                uint64 gap_end = walk.max > max ? max : walk.max;
                if (gap_end >= gap_start && gap_end - gap_start + 1 >= size) {
                    mas->index = gap_start;
                    mas->last = gap_start + size - 1;
                    return 0;
                }
            }
            if (entry == NULL)
                return -EBUSY;
        }
        /* Non-NULL entry: skip over it. */
        if (walk.min > max)
            return -EBUSY;
    }
}

int mas_empty_area_rev(struct ma_state *mas, uint64 min, uint64 max,
                       uint64 size)
{
    if (size == 0 || min > max || max - min + 1 < size)
        return -EBUSY;

    /* Walk from the end and scan backward. */
    MA_STATE(walk, mas->tree, max, max);
    void *entry = mas_walk(&walk);

    /* Check if we landed on a NULL slot. */
    if (entry == NULL && walk.node != NULL) {
        uint64 gap_start = walk.min < min ? min : walk.min;
        uint64 gap_end = walk.max > max ? max : walk.max;
        if (gap_end >= gap_start && gap_end - gap_start + 1 >= size) {
            /* Place at the top of the gap. */
            mas->index = gap_end - size + 1;
            mas->last = mas->index + size - 1;
            if (mas->index >= min)
                return 0;
        }
    }

    /* Scan backward for a gap. */
    while (1) {
        entry = __mas_prev_slot(&walk);
        if (entry == NULL && walk.node != NULL) {
            uint64 gap_start = walk.min < min ? min : walk.min;
            uint64 gap_end = walk.max > max ? max : walk.max;
            if (gap_end >= gap_start && gap_end - gap_start + 1 >= size) {
                mas->index = gap_end - size + 1;
                mas->last = mas->index + size - 1;
                if (mas->index >= min)
                    return 0;
            }
            if (walk.max < min)
                return -EBUSY;
            continue;
        }
        if (entry == NULL) {
            entry = __mas_prev_node(&walk, min);
            if (entry == NULL && walk.node != NULL) {
                uint64 gap_start = walk.min < min ? min : walk.min;
                uint64 gap_end = walk.max > max ? max : walk.max;
                if (gap_end >= gap_start && gap_end - gap_start + 1 >= size) {
                    mas->index = gap_end - size + 1;
                    mas->last = mas->index + size - 1;
                    if (mas->index >= min)
                        return 0;
                }
            }
            if (entry == NULL)
                return -EBUSY;
        }
        if (walk.max < min)
            return -EBUSY;
    }
}

/* ====================================================================== */
/*  RCU read-side helpers                                                  */
/* ====================================================================== */

void *mt_find(struct maple_tree *mt, uint64 *index, uint64 max)
{
    if (*index > max)
        return NULL;

    rcu_read_lock();
    MA_STATE(mas, mt, *index, *index);
    void *entry = mas_find(&mas, max);
    if (entry != NULL) {
        /*
         * Advance *index past the found entry so that the next call
         * to mt_find will start searching from the entry after this one.
         * Guard against overflow when mas.max == MAPLE_MAX.
         */
        if (mas.max == MAPLE_MAX)
            *index = MAPLE_MAX;
        else
            *index = mas.max + 1;
    }
    rcu_read_unlock();
    return entry;
}

void *mt_next(struct maple_tree *mt, uint64 index, uint64 max)
{
    if (index >= max)
        return NULL;

    rcu_read_lock();
    MA_STATE(mas, mt, index + 1, index + 1);
    void *entry = mas_walk(&mas);
    if (entry != NULL) {
        rcu_read_unlock();
        return entry;
    }
    /* If we landed on NULL, find next non-NULL. */
    entry = mas_find(&mas, max);
    rcu_read_unlock();
    return entry;
}

void *mt_prev(struct maple_tree *mt, uint64 index, uint64 min)
{
    if (index <= min)
        return NULL;

    rcu_read_lock();
    MA_STATE(mas, mt, index - 1, index - 1);
    void *entry = mas_walk(&mas);
    if (entry != NULL) {
        rcu_read_unlock();
        return entry;
    }
    /* If NULL, scan backward. */
    entry = mas_prev(&mas, min);
    rcu_read_unlock();
    return entry;
}
