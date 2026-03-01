/**
 * @file maple_tree_type.h
 * @brief Maple tree type definitions — Linux-style B-tree for VM ranges.
 *
 * The maple tree is an RCU-safe B-tree that stores non-overlapping ranges
 * [index, last] → void* entry.  It is designed to replace the red-black tree
 * + linked lists used in vm_t for VMA management.
 *
 * Simplified from the Linux kernel maple tree (6.x) for xv6:
 *  - Single node type (maple_range_64) with 16 slots/pivots.
 *  - 256-byte nodes aligned to cache-line boundary.
 *  - Gap tracking for O(log n) free-range search.
 *  - RCU-safe reads; write-side uses a per-tree spinlock.
 */

#ifndef __KERNEL_MAPLE_TREE_TYPE_H
#define __KERNEL_MAPLE_TREE_TYPE_H

#include "types.h"
#include "compiler.h"
#include "lock/rcu_type.h"

/*
 * Node geometry.
 *
 * MAPLE_NODE_SLOTS: branching factor (number of child/entry pointers).
 * Pivots: MAPLE_NODE_SLOTS - 1 (15).
 * A node with N used slots has N-1 defined pivots; the last slot implicitly
 * extends to the parent's right-boundary.
 *
 * Node size is 256 bytes (4 cache lines).
 */
#define MAPLE_NODE_SLOTS    16
#define MAPLE_NODE_PIVOTS   (MAPLE_NODE_SLOTS - 1)

/* Tagged-pointer flags stored in the low bits of maple_tree.ma_root
 * and maple_node.parent. */
#define MAPLE_ROOT_NODE     0x02UL  /* root contains a node pointer */

/* Minimum / maximum index values. */
#define MAPLE_MIN           0UL
#define MAPLE_MAX           (~0UL)

/* Forward declarations. */
struct maple_node;
struct maple_tree;
struct ma_state;

/**
 * enum maple_type - Node type discriminator.
 * @maple_leaf_64:   Leaf node: slots point to user entries (void *).
 * @maple_arange_64: Internal/augmented node: slots point to child nodes,
 *                   gap[] tracks the largest gap in each subtree.
 */
enum maple_type {
    maple_leaf_64   = 0,
    maple_arange_64 = 1,
};

/**
 * struct maple_node - A single B-tree node.
 *
 * @parent:    Tagged pointer to parent node.  Bit 0 set means "is root".
 *             Remaining bits hold the parent pointer (masked with ~0x1).
 * @parent_slot: Slot index within the parent node (0..15).
 * @type:      maple_leaf_64 or maple_arange_64.
 * @slot_len:  Number of slots in use (1..MAPLE_NODE_SLOTS).
 * @pivot:     Separating keys.  Entry at slot[i] covers the range
 *             (pivot[i-1], pivot[i]].  pivot[-1] is implied to be the
 *             node's minimum, pivot[slot_len-1] is implied to be the
 *             node's maximum.
 * @slot:      Pointers — either child nodes (internal) or entries (leaf).
 * @gap:       (arange_64 only) Largest gap under each child subtree.
 * @rcu_head:  For RCU-deferred freeing.
 *
 * Layout is carefully ordered so that pivots and slots (the hot data)
 * occupy the first two cache lines.
 */
struct maple_node {
    /* 8 bytes */
    uint64 parent;

    /* 1 + 1 + 1 + 5 pad = 8 bytes */
    uint8  type;
    uint8  slot_len;
    uint8  parent_slot;
    uint8  __pad[5];

    /* 15 * 8 = 120 bytes — pivots */
    uint64 pivot[MAPLE_NODE_PIVOTS];

    /* 16 * 8 = 128 bytes — slots */
    void  *slot[MAPLE_NODE_SLOTS];

    /* Remaining space for gap + rcu (in a union to keep size down) */
    union {
        /* gap tracking: only meaningful for maple_arange_64 */
        uint64 gap[MAPLE_NODE_SLOTS];   /* 128 bytes */
        struct {
            uint64 __gap_pad[MAPLE_NODE_SLOTS - 4];
            rcu_head_t rcu_head;        /* overlaps last 4 gap entries */
        };
    };
} __ALIGNED_CACHELINE;

/*
 * Verify node size. We want exactly 512 bytes (or close) to fit in a
 * power-of-2 slab bucket.  Adjust if needed based on rcu_head_t size.
 */
_Static_assert(sizeof(struct maple_node) <= 1024,
               "maple_node too large");

/**
 * struct maple_tree - Root structure.
 *
 * @ma_root:  Tagged pointer.  Can be:
 *            - NULL: empty tree.
 *            - A single entry (for range [0, MAPLE_MAX]) with no node
 *              overhead — optimisation for one-entry trees (NOT USED in our
 *              VM scenario; we always allocate nodes).
 *            - A node pointer with MAPLE_ROOT_NODE set.
 * @ma_flags: Reserved for future flags.
 *
 * Write-side protection is provided externally (e.g. vm->rw_lock).
 */
struct maple_tree {
    void *_Atomic  ma_root;
    unsigned int   ma_flags;
};

/**
 * struct ma_state (MAS) - Maple-tree walk / cursor state.
 *
 * Working state passed to all low-level maple tree operations.
 *
 * @tree:   Pointer to the maple tree being operated on.
 * @index:  Start of the range being searched / stored.
 * @last:   End of the range being searched / stored (inclusive).
 * @node:   Current node (or NULL / error token).
 * @min:    Minimum index reachable through @node.
 * @max:    Maximum index reachable through @node.
 * @offset: Slot offset within @node.
 * @depth:  Current depth in the tree (0 = root).
 */
struct ma_state {
    struct maple_tree *tree;
    uint64 index;
    uint64 last;
    struct maple_node *node;
    uint64 min;
    uint64 max;
    uint8  offset;
    uint8  depth;
};

/** Initialiser for an ma_state on the stack. */
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

#endif /* __KERNEL_MAPLE_TREE_TYPE_H */
