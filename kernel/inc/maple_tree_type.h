/**
 * @file maple_tree_type.h
 * @brief Maple tree type definitions — Linux-style B-tree for ranges.
 *
 * The maple tree is an RCU-safe B-tree that stores non-overlapping ranges
 * [index, last] → void* entry.  Designed for VM area management.
 *
 * Simplified from the Linux kernel maple tree (6.x):
 *  - 16 slots per node, 15 pivots.
 *  - Gap tracking for O(log n) free-range search.
 *  - Optional RCU-safe reads; write-side uses external locking.
 */

#ifndef MAPLE_TREE_TYPE_H
#define MAPLE_TREE_TYPE_H

#include "maple_tree_config.h"

/* ----------------------------------------------------------------------- */
/*  Constants                                                               */
/* ----------------------------------------------------------------------- */

/** Number of child/entry pointers per node (branching factor). */
#define MAPLE_NODE_SLOTS    10

/** Number of pivot (separator) keys per node. */
#define MAPLE_NODE_PIVOTS   (MAPLE_NODE_SLOTS - 1)

/** Tagged-pointer flag: ma_root contains a node pointer. */
#define MAPLE_ROOT_NODE     0x02UL

/** Minimum / maximum index values. */
#define MAPLE_MIN           0UL
#define MAPLE_MAX           (~(uint64_t)0)

/* ----------------------------------------------------------------------- */
/*  Node type                                                               */
/* ----------------------------------------------------------------------- */

/**
 * enum maple_type - Node type discriminator.
 * @maple_leaf_64:   Leaf node — slots point to user entries.
 * @maple_arange_64: Internal node — slots point to children, gap[] tracks
 *                   the largest gap in each subtree.
 */
enum maple_type {
    maple_leaf_64   = 0,
    maple_arange_64 = 1,
};

/* ----------------------------------------------------------------------- */
/*  Parent-pointer encoding (Linux-style)                                   */
/* ----------------------------------------------------------------------- */

/**
 * Nodes are 256-byte aligned, giving 8 usable low bits in any pointer
 * to a maple_node.  The @parent field packs metadata into these bits:
 *
 *   Bit  0     : root flag (1 = this node is the tree root)
 *   Bits 1–2   : node type (enum maple_type of this node)
 *   Bits 3–7   : parent_slot (slot index within the parent, 0..31)
 *   Bits 8–63  : actual parent pointer (256-byte aligned)
 */
#define MAPLE_NODE_MASK          0xFFUL
#define MAPLE_PARENT_ROOT        0x01UL
#define MAPLE_PARENT_TYPE_MASK   0x06UL
#define MAPLE_PARENT_TYPE_SHIFT  1
#define MAPLE_PARENT_SLOT_MASK   0xF8UL
#define MAPLE_PARENT_SLOT_SHIFT  3

/* ----------------------------------------------------------------------- */
/*  Node structure                                                          */
/* ----------------------------------------------------------------------- */

/**
 * struct maple_node - A single B-tree node (256-byte aligned).
 *
 * @parent:   Tagged pointer to parent.  Low 8 bits encode root flag,
 *            node type, and slot index within the parent (see above).
 * @slot_len: Number of slots in use (1..MAPLE_NODE_SLOTS).
 * @pivot:    Separating keys — pivot[i] is the inclusive upper bound
 *            of slot[i].  The last slot extends to the node's max.
 * @slot:     Pointers — child nodes (internal) or entries (leaf).
 * @gap:      (arange_64 only) Largest gap under each child subtree.
 */
struct maple_node {
    uint64_t parent;
    uint8_t  slot_len;
    uint8_t  __pad[7];

    uint64_t pivot[MAPLE_NODE_PIVOTS];
    void    *slot[MAPLE_NODE_SLOTS];
    uint64_t gap[MAPLE_NODE_SLOTS];
    /* Embedded RCU head used by the kernel-side mt_call_rcu trampoline. */
    rcu_head_t rcu_head;
} __attribute__((aligned(256)));

/* ----------------------------------------------------------------------- */
/*  Tree root                                                               */
/* ----------------------------------------------------------------------- */

/**
 * struct maple_tree - Root structure.
 *
 * @ma_root:  Tagged pointer — NULL (empty), or a node pointer | MAPLE_ROOT_NODE.
 * @ma_flags: Feature flags (e.g. MT_FLAGS_USE_RCU).
 * @ma_lock:  (MT_CONFIG_LOCK only) Per-tree lock.
 */

/** Per-tree flag: use RCU copy-on-write for structural modifications.
 *  Only meaningful when the library is compiled with MT_CONFIG_RCU. */
#define MT_FLAGS_USE_RCU  0x01U

struct maple_tree {
    void        *ma_root;
    unsigned int ma_flags;
#ifdef MT_CONFIG_LOCK
    mt_lock_t    ma_lock;
#endif
};

/* ----------------------------------------------------------------------- */
/*  Cursor (walk state)                                                     */
/* ----------------------------------------------------------------------- */

/**
 * Maximum cached cursor depth.
 *
 * The standalone maple tree uses a fixed-size path cache inside
 * struct ma_state so forward/backward cursor movement can ascend and
 * descend without re-deriving parent bounds from the live tree.
 *
 * With a 10-way branching factor, 32 cached levels cover more than
 * 10^32 leaf positions, far beyond any tree that could exist in memory.
 * Keeping the cache fixed-size avoids dynamic allocation in cursor code,
 * which is important for kernel-style use.
 */
#define MAPLE_CURSOR_MAX_DEPTH  32

/**
 * struct maple_path_frame - One cached step in a cursor's root-to-node path.
 *
 * @node:     Node visited at this tree level.
 * @node_min: Inclusive minimum index covered by @node as a whole.
 * @node_max: Inclusive maximum index covered by @node as a whole.
 * @slot:     Selected slot within @node.
 *
 * For internal nodes, @slot is the child branch chosen while descending.
 * For leaf nodes, @slot is the current entry/gap slot that the cursor is
 * positioned on.  Caching node-wide bounds rather than slot-local bounds
 * allows the cursor to recompute slot min/max locally from pivots while
 * moving laterally within the same node.
 */
struct maple_path_frame {
    struct maple_node *node;
    uint64_t node_min;
    uint64_t node_max;
    uint8_t  slot;
    uint8_t  __pad[7];
};

/**
 * struct ma_state (MAS) - Walk / cursor state.
 *
 * @tree:   Pointer to the maple tree.
 * @index:  Start of the search/store range, or the next retry position for
 *          iteration helpers.
 * @last:   End of the search/store range (inclusive).
 * @node:   Current positioned node, typically the leaf that owns @offset.
 *          NULL means the cursor is not positioned on a node.
 * @min:    Inclusive minimum index of the current slot.
 * @max:    Inclusive maximum index of the current slot.
 * @offset: Slot offset within @node.
 * @depth:  Number of valid frames currently cached in @path when @node is
 *          non-NULL.  A value of 0 means there is no cached path.
 *          UINT8_MAX is reserved internally as an RCU retry sentinel.
 * @path:   Cached root-to-node walk.  path[0] is the root frame and
 *          path[depth - 1] is the current node frame.
 *
 * The extended cursor caches the exact walk from the root to the current
 * node.  That gives sequential operations kernel-friendly behavior:
 * mas_next() / mas_prev() can ascend by walking cached frames rather than
 * recovering parent pointers and subtree bounds from the live tree each
 * time.  The cache is rebuilt by mas_walk() and invalidated whenever the
 * cursor becomes unpositioned or an RCU retry is required.
 */
struct ma_state {
    struct maple_tree *tree;
    uint64_t index;
    uint64_t last;
    struct maple_node *node;
    uint64_t min;
    uint64_t max;
    uint8_t  offset;
    uint8_t  depth;
    struct maple_path_frame path[MAPLE_CURSOR_MAX_DEPTH];
};

/** Stack initialiser for an ma_state. */
#define MA_STATE(name, mt, first, end)          \
    struct ma_state name = {                    \
        .tree   = (mt),                         \
        .index  = (first),                      \
        .last   = (end),                        \
        .node   = NULL,                         \
        .min    = 0,                            \
        .max    = MAPLE_MAX,                    \
        .offset = 0,                            \
        .depth  = 0,                            \
    }

#endif /* MAPLE_TREE_TYPE_H */
