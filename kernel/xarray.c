/**
 * @file xarray.c
 * @brief XArray implementation — Linux-style radix tree for xv6.
 *
 * A radix tree mapping unsigned long indices to void* entries.
 * Each internal node has 64 slots (6-bit fan-out).  The tree grows
 * upward when indices exceed the current depth and shrinks when the
 * top node collapses to a single entry.
 *
 * Write operations hold xa_lock (the xarray's internal spinlock).
 * Read operations (xa_load) are lock-free under RCU.
 *
 * Three independent mark bitmaps per node allow efficient iteration
 * over tagged entries.
 */

#include "types.h"
#include "defs.h"
#include "printf.h"
#include "string.h"
#include "xarray.h"
#include <mm/slab.h>
#include "lock/rcu.h"
#include <smp/atomic.h>
#include "errno.h"

/* ====================================================================== */
/*  Node slab allocator                                                    */
/* ====================================================================== */

static slab_cache_t __xa_node_cache;
static int __xa_node_cache_initialized = 0;

void xarray_global_init(void)
{
    if (__xa_node_cache_initialized)
        return;
    slab_cache_init(&__xa_node_cache, "xa_node",
                    sizeof(struct xa_node),
                    SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
    __xa_node_cache_initialized = 1;
    printf("XArray subsystem initialized\n");
}

static struct xa_node *xa_node_alloc(void)
{
    struct xa_node *node = slab_alloc(&__xa_node_cache);
    if (node == NULL)
        return NULL;
    memset(node, 0, sizeof(*node));
    return node;
}

static void __xa_node_free_rcu_cb(void *data)
{
    struct xa_node *node = (struct xa_node *)data;
    slab_free(node);
}

static void xa_node_free_rcu(struct xa_node *node)
{
    if (node == NULL)
        return;
    call_rcu(&node->rcu_head, __xa_node_free_rcu_cb, node);
}

static void xa_node_free_now(struct xa_node *node)
{
    if (node == NULL)
        return;
    slab_free(node);
}

/* ====================================================================== */
/*  Internal helpers — slot access                                         */
/* ====================================================================== */

/** Calculate the slot offset for @index at a node with the given @shift. */
static inline uint8 xa_offset(uint64 index, uint8 shift)
{
    return (uint8)((index >> shift) & XA_CHUNK_MASK);
}

/** Maximum index that can be addressed by a tree of the given @shift. */
static inline uint64 xa_max_index(uint8 shift)
{
    /* shift is the shift of the root node.  The tree covers indices
     * 0 .. (1 << (shift + XA_CHUNK_SHIFT)) - 1. */
    uint8 total_bits = shift + XA_CHUNK_SHIFT;
    if (total_bits >= 64)
        return ~0UL;
    return (1UL << total_bits) - 1;
}

/** Base index covered by a node level containing @index. */
static inline uint64 xa_level_base(uint64 index, uint8 shift)
{
    uint8 total_bits = shift + XA_CHUNK_SHIFT;

    if (total_bits >= 64)
        return 0;
    return index & ~((1UL << total_bits) - 1);
}

/** Load a slot with RCU acquire semantics. */
static inline void *xa_slot_load(void *_Atomic *slot)
{
    return __atomic_load_n(slot, __ATOMIC_ACQUIRE);
}

/** Store to a slot with release semantics (visible to RCU readers). */
static inline void xa_slot_store(void *_Atomic *slot, void *entry)
{
    __atomic_store_n(slot, entry, __ATOMIC_RELEASE);
}

/** Decode the head entry to a node pointer, or NULL if not a node. */
static inline struct xa_node *xa_head_to_node(void *head)
{
    if (xa_is_internal(head) && head != XA_RETRY_ENTRY &&
        head != XA_ZERO_ENTRY && !xa_is_sibling(head))
        return xa_to_internal(head);
    return NULL;
}

#define XA_HEAD_MARK_SHIFT 29
#define XA_HEAD_MARK_MASK  ((((1U << XA_MAX_MARKS) - 1U) << XA_HEAD_MARK_SHIFT))

static inline unsigned int xa_head_mark_flag(xa_mark_t mark)
{
    return 1U << (XA_HEAD_MARK_SHIFT + mark);
}

static inline bool xa_head_get_mark(const struct xarray *xa, xa_mark_t mark)
{
    if (mark >= XA_MAX_MARKS)
        return false;

    unsigned int flags = __atomic_load_n(&xa->xa_flags, __ATOMIC_ACQUIRE);
    return (flags & xa_head_mark_flag(mark)) != 0;
}

static inline void xa_head_set_mark(struct xarray *xa, xa_mark_t mark)
{
    if (mark >= XA_MAX_MARKS)
        return;
    __atomic_fetch_or(&xa->xa_flags, xa_head_mark_flag(mark), __ATOMIC_RELEASE);
}

static inline void xa_head_clear_mark(struct xarray *xa, xa_mark_t mark)
{
    if (mark >= XA_MAX_MARKS)
        return;
    __atomic_fetch_and(&xa->xa_flags, ~xa_head_mark_flag(mark), __ATOMIC_RELEASE);
}

static inline void xa_head_clear_all_marks(struct xarray *xa)
{
    __atomic_fetch_and(&xa->xa_flags, ~XA_HEAD_MARK_MASK, __ATOMIC_RELEASE);
}

/* ====================================================================== */
/*  Mark helpers                                                           */
/* ====================================================================== */

static inline bool node_get_mark(const struct xa_node *node, uint8 offset,
                                 xa_mark_t mark)
{
    if (mark >= XA_MAX_MARKS)
        return false;
    return (node->marks[mark][offset >> 6] >> (offset & 63)) & 1;
}

static inline void node_set_mark(struct xa_node *node, uint8 offset,
                                 xa_mark_t mark)
{
    if (mark >= XA_MAX_MARKS)
        return;
    node->marks[mark][offset >> 6] |= 1UL << (offset & 63);
}

static inline void node_clear_mark(struct xa_node *node, uint8 offset,
                                   xa_mark_t mark)
{
    if (mark >= XA_MAX_MARKS)
        return;
    node->marks[mark][offset >> 6] &= ~(1UL << (offset & 63));
}

/** Check if any slot in @node has @mark set. */
static inline bool node_any_mark(const struct xa_node *node, xa_mark_t mark)
{
    if (mark >= XA_MAX_MARKS)
        return false;
    for (int i = 0; i < XA_MARK_LONGS; i++) {
        if (node->marks[mark][i])
            return true;
    }
    return false;
}

/* ====================================================================== */
/*  Internal: tree growth (expand) and shrinkage                           */
/* ====================================================================== */

/**
 * xa_expand - Grow the tree until it can accommodate @index.
 *
 * Called with xa_lock held.  Allocates new root nodes as needed.
 * Returns 0 on success, -ENOMEM on allocation failure.
 *
 * When the head is a single raw entry (not a node), it only represents
 * index 0.  Any store to index > 0 first promotes the entry into a leaf
 * node (shift = 0).  Then, if the index exceeds the current root's
 * coverage, new root nodes are added *above* the existing root until
 * the tree is tall enough.
 *
 * Capacity formula: a root at shift S covers indices 0 .. 2^(S+6)−1,
 * i.e. xa_max_index(S).
 */
static int xa_expand(struct xarray *xa, uint64 index)
{
    void *head = xa->xa_head;

    /* If tree is empty and index is 0, no expansion needed —
     * xas_create / xas_store will set xa_head directly. */
    if (head == NULL && index == 0)
        return 0;

    /* If tree is empty but index > 0, we must create a leaf node so that
     * the entry is stored at the correct slot rather than directly in
     * xa_head (which only represents index 0). */
    if (head == NULL) {
        struct xa_node *leaf = xa_node_alloc();
        if (leaf == NULL)
            return -ENOMEM;

        leaf->shift   = 0;
        leaf->offset  = 0;
        leaf->count   = 0;   /* empty leaf — no entries yet */
        leaf->parent  = NULL;
        leaf->array   = xa;

        head = xa_mk_internal(leaf);
        xa_slot_store((void *_Atomic *)&xa->xa_head, head);
        /* Fall through to the growth loop if index > 63. */
    }

    struct xa_node *node = xa_head_to_node(head);

    /* Single-entry head covers only index 0.  Promote to a leaf. */
    if (node == NULL) {
        if (index == 0)
            return 0;   /* xas_create handles index-0 in xa_head directly. */

        struct xa_node *leaf = xa_node_alloc();
        if (leaf == NULL)
            return -ENOMEM;

        leaf->shift   = 0;
        leaf->offset  = 0;
        leaf->count   = 1;   /* one entry in slot 0 */
        leaf->parent  = NULL;
        leaf->array   = xa;
        leaf->slots[0] = head;

        for (xa_mark_t m = 0; m < XA_MAX_MARKS; m++) {
            if (xa_head_get_mark(xa, m))
                node_set_mark(leaf, 0, m);
        }
        xa_head_clear_all_marks(xa);

        head = xa_mk_internal(leaf);
        xa_slot_store((void *_Atomic *)&xa->xa_head, head);
        node = leaf;
    }

    /* Grow upward: add a new root ABOVE the current root until the tree
     * covers @index.  xa_max_index(node->shift) is the real capacity of a
     * tree whose root node has shift == node->shift. */
    while (index > xa_max_index(node->shift)) {
        struct xa_node *new_node = xa_node_alloc();
        if (new_node == NULL)
            return -ENOMEM;

        new_node->shift  = node->shift + XA_CHUNK_SHIFT;
        new_node->offset = 0;
        new_node->count  = 1;
        new_node->parent = NULL;
        new_node->array  = xa;

        /* Move the current head into slot 0 of the new root. */
        new_node->slots[0] = head;
        node->offset = 0;
        node->parent = new_node;

        /* Propagate marks from the old root to slot 0 of the new root. */
        for (xa_mark_t m = 0; m < XA_MAX_MARKS; m++) {
            if (node_any_mark(node, m))
                node_set_mark(new_node, 0, m);
        }

        head = xa_mk_internal(new_node);
        xa_slot_store((void *_Atomic *)&xa->xa_head, head);
        node = new_node;
    }

    return 0;
}

/**
 * xa_shrink - Shrink the tree if the root node has only one child.
 *
 * Repeatedly removes the top-level node and promotes its single child
 * (or entry) to the head position.  Called with xa_lock held.
 *
 * We can only collapse a node whose single remaining child is at slot 0,
 * because slot 0 covers the same index range as the parent after removal.
 * If the sole child is at a different slot, shrinking would lose the
 * index mapping.
 */
static void xa_shrink(struct xarray *xa)
{
    for (;;) {
        void *head = xa->xa_head;
        struct xa_node *node = xa_head_to_node(head);

        if (node == NULL)
            break;
        if (node->count != 1 && node->count != 0)
            break;
        if (node->count == 0) {
            /* Empty root node — tree is empty. */
            xa_slot_store((void *_Atomic *)&xa->xa_head, NULL);
            xa_head_clear_all_marks(xa);
            xa_node_free_rcu(node);
            break;
        }

        /* count == 1: can only shrink if the sole child is at slot 0. */
        void *child = node->slots[0];
        if (child == NULL)
            break;  /* Sole entry is in a different slot — cannot shrink. */

        struct xa_node *child_node = xa_head_to_node(child);
        if (child_node) {
            child_node->parent = NULL;
            child_node->offset = 0;
            xa_head_clear_all_marks(xa);
        } else {
            xa_head_clear_all_marks(xa);
            for (xa_mark_t m = 0; m < XA_MAX_MARKS; m++) {
                if (node_get_mark(node, 0, m))
                    xa_head_set_mark(xa, m);
            }
        }

        xa_slot_store((void *_Atomic *)&xa->xa_head, child);
        node->slots[0] = NULL;
        node->count = 0;
        xa_node_free_rcu(node);

        /* If the promoted child is a node, try to shrink further. */
        if (child_node == NULL)
            break;
    }
}

/* ====================================================================== */
/*  Internal: walk to a slot                                               */
/* ====================================================================== */

/**
 * xas_descend - Walk the xa_state cursor down to the node containing
 * the slot for xas->xa_index.
 *
 * On success, xas->xa_node points to the leaf node and xas->xa_offset
 * is the slot index within that node.  xas->xa_shift is 0 for leaf.
 *
 * On failure (index out of range or tree is empty), returns NULL and
 * sets xas->xa_node to an appropriate sentinel.
 *
 * Caller must be in an RCU read section or hold xa_lock.
 */
static void *xas_descend_to_leaf(struct xa_state *xas)
{
    struct xarray *xa = xas->xa;
    void *head = xa_slot_load((void *_Atomic *)&xa->xa_head);

    if (head == NULL) {
        xas->xa_node = XAS_BOUNDS;
        return NULL;
    }

    struct xa_node *node = xa_head_to_node(head);
    if (node == NULL) {
        /* Head is a single entry — only index 0 is valid. */
        if (xas->xa_index != 0) {
            xas->xa_node = XAS_BOUNDS;
            return NULL;
        }
        xas->xa_node = NULL;  /* NULL means "entry is directly in xa_head" */
        xas->xa_offset = 0;
        xas->xa_shift = 0;
        return head;
    }

    /* Descend through internal nodes. */
    if (xas->xa_index > xa_max_index(node->shift)) {
        xas->xa_node = XAS_BOUNDS;
        return NULL;
    }

    while (node) {
        uint8 offset = xa_offset(xas->xa_index, node->shift);
        void *entry = xa_slot_load((void *_Atomic *)&node->slots[offset]);

        xas->xa_node = node;
        xas->xa_offset = offset;
        xas->xa_shift = node->shift;

        if (node->shift == 0) {
            /* Leaf level — entry is a user entry (or NULL). */
            return entry;
        }

        struct xa_node *child = xa_head_to_node(entry);
        if (child == NULL) {
            /* Slot is NULL or a leaf entry at a non-leaf level.
             * For lookups this means "not found"; for stores we need
             * to create intermediate nodes. */
            return entry;
        }

        node = child;
    }

    return NULL;
}

/**
 * xas_create - Ensure all intermediate nodes exist for xas->xa_index.
 *
 * Called with xa_lock held.  Allocates nodes as needed.  On success,
 * xas->xa_node/xa_offset point to the leaf slot.  On failure,
 * xas_error(xas) is set.
 *
 * Returns the current entry in the leaf slot.
 */
static void *xas_create(struct xa_state *xas)
{
    struct xarray *xa = xas->xa;
    void *head;
    struct xa_node *node;

    /* Expand the tree if needed. */
    if (xa_expand(xa, xas->xa_index) != 0) {
        xas_set_err(xas, -ENOMEM);
        return NULL;
    }

    head = xa_slot_load((void *_Atomic *)&xa->xa_head);

    /* Empty tree — will be handled by the caller setting xa_head directly. */
    if (head == NULL) {
        xas->xa_node = NULL;
        xas->xa_offset = 0;
        xas->xa_shift = 0;
        return NULL;
    }

    node = xa_head_to_node(head);
    if (node == NULL) {
        /* Head is a single entry. If index == 0, we're done. */
        if (xas->xa_index == 0) {
            xas->xa_node = NULL;
            xas->xa_offset = 0;
            xas->xa_shift = 0;
            return head;
        }
        /* Need to convert single entry to a node — xa_expand should
         * have handled this above. */
        xas_set_err(xas, -ENOMEM);
        return NULL;
    }

    /* Walk down, creating intermediate nodes as needed. */
    while (node->shift > 0) {
        uint8 offset = xa_offset(xas->xa_index, node->shift);
        void *entry = xa_slot_load((void *_Atomic *)&node->slots[offset]);
        struct xa_node *child = xa_head_to_node(entry);

        if (child == NULL) {
            /* Need to allocate an intermediate node. */
            if (entry != NULL && !xa_is_internal(entry)) {
                /* A leaf entry exists at a non-leaf level — this shouldn't
                 * happen in normal usage.  Treat as empty. */
            }

            child = xa_node_alloc();
            if (child == NULL) {
                xas_set_err(xas, -ENOMEM);
                return NULL;
            }
            child->shift = node->shift - XA_CHUNK_SHIFT;
            child->offset = offset;
            child->count = 0;
            child->parent = node;
            child->array = xa;

            xa_slot_store((void *_Atomic *)&node->slots[offset],
                          xa_mk_internal(child));
            if (entry == NULL)
                node->count++;
        }

        node = child;
    }

    /* Now at the leaf level. */
    uint8 offset = xa_offset(xas->xa_index, 0);
    xas->xa_node = node;
    xas->xa_offset = offset;
    xas->xa_shift = 0;
    return xa_slot_load((void *_Atomic *)&node->slots[offset]);
}

/* ====================================================================== */
/*  Internal: node cleanup after erase                                     */
/* ====================================================================== */

/**
 * xas_delete_node - After erasing an entry, free the node if it
 * became empty, and propagate upward.
 *
 * Called with xa_lock held.
 */
static void xas_delete_node(struct xa_state *xas)
{
    struct xa_node *node = xas->xa_node;

    while (node) {
        if (node->count > 0)
            break;

        struct xa_node *parent = node->parent;
        if (parent) {
            xa_slot_store((void *_Atomic *)&parent->slots[node->offset], NULL);

            for (xa_mark_t m = 0; m < XA_MAX_MARKS; m++) {
                if (!node_get_mark(parent, node->offset, m))
                    continue;

                node_clear_mark(parent, node->offset, m);

                struct xa_node *cur = parent;
                while (cur->parent) {
                    uint8 off = cur->offset;
                    if (node_any_mark(cur, m))
                        break;
                    cur = cur->parent;
                    node_clear_mark(cur, off, m);
                }
            }

            parent->count--;
        } else {
            /* This was the root node — tree is now empty. */
            xa_slot_store((void *_Atomic *)&xas->xa->xa_head, NULL);
            xa_head_clear_all_marks(xas->xa);
        }

        xa_node_free_rcu(node);
        node = parent;
    }

    /* Try to shrink the tree. */
    xa_shrink(xas->xa);
}

/* ====================================================================== */
/*  XAS (cursor) API implementation                                        */
/* ====================================================================== */

void *xas_load(struct xa_state *xas)
{
    return xas_descend_to_leaf(xas);
}

void *xas_store(struct xa_state *xas, void *entry)
{
    void *old;

    /* Create path to the leaf slot. */
    old = xas_create(xas);
    if (xas_error(xas))
        return NULL;

    struct xa_node *node = xas->xa_node;

    if (node == NULL) {
        /* Storing directly into xa_head (single-entry or first entry). */
        void *prev = xa_slot_load((void *_Atomic *)&xas->xa->xa_head);
        xa_slot_store((void *_Atomic *)&xas->xa->xa_head, entry);

        if (entry == NULL || prev == NULL)
            xa_head_clear_all_marks(xas->xa);

        /* If we're erasing (setting to NULL) and there's no node, done. */
        if (entry == NULL && prev != NULL) {
            /* Tree becomes empty. */
        }
        return prev;
    }

    /* Store into the leaf slot. */
    old = xa_slot_load((void *_Atomic *)&node->slots[xas->xa_offset]);

    if (entry == NULL && old != NULL) {
        for (xa_mark_t m = 0; m < XA_MAX_MARKS; m++) {
            if (!node_get_mark(node, xas->xa_offset, m))
                continue;

            node_clear_mark(node, xas->xa_offset, m);

            struct xa_node *cur = node;
            while (cur->parent) {
                uint8 off = cur->offset;
                if (node_any_mark(cur, m))
                    break;
                cur = cur->parent;
                node_clear_mark(cur, off, m);
            }
        }
    }

    xa_slot_store((void *_Atomic *)&node->slots[xas->xa_offset], entry);

    /* Update count. */
    if (entry != NULL && old == NULL)
        node->count++;
    else if (entry == NULL && old != NULL)
        node->count--;

    /* If we erased an entry, try to free empty nodes. */
    if (entry == NULL && node->count == 0)
        xas_delete_node(xas);

    return old;
}

void *xas_find(struct xa_state *xas, uint64 max)
{
    struct xarray *xa = xas->xa;
    void *head;

    if (xas_error(xas))
        return NULL;

    /* If this is the first call, start from xa_index.
     * Otherwise, advance to the next index. */
    if (!xas_is_restart(xas)) {
        if (xas->xa_index >= max)
            return NULL;
        xas->xa_index++;
    }

    head = xa_slot_load((void *_Atomic *)&xa->xa_head);
    if (head == NULL)
        return NULL;

    /* Simple case: single entry — only index 0. */
    struct xa_node *root_node = xa_head_to_node(head);
    if (root_node == NULL) {
        if (xas->xa_index == 0 && 0 <= max) {
            xas->xa_node = NULL;
            xas->xa_offset = 0;
            xas->xa_shift = 0;
            xas->xa_index = 0;
            return head;
        }
        return NULL;
    }

    /* Walk the tree looking for the next non-NULL entry. */
    while (xas->xa_index <= max) {
        xas->xa_node = XAS_RESTART;
        void *entry = xas_descend_to_leaf(xas);
        if (entry != NULL && xa_is_entry(entry))
            return entry;

        /* Advance to the next possible populated index. */
        struct xa_node *node = xas->xa_node;
        if (xas_is_special(xas)) {
            /* Out of bounds — done. */
            return NULL;
        }

        /* Try next slot in the current node. */
        bool found = false;
        while (node) {
            uint8 offset = xas->xa_offset + 1;
            while (offset < XA_CHUNK_SIZE) {
                void *slot_entry = xa_slot_load(
                    (void *_Atomic *)&node->slots[offset]);
                if (slot_entry != NULL) {
                    /* Compute the index of this slot. */
                    uint64 base = xa_level_base(xas->xa_index, node->shift);
                    xas->xa_index = base | ((uint64)offset << node->shift);
                    found = true;
                    break;
                }
                offset++;
            }
            if (found)
                break;

            /* No more slots in this node — go up to parent. */
            xas->xa_offset = node->offset;
            node = node->parent;
            if (node == NULL) {
                /* Exhausted the tree. */
                return NULL;
            }
        }

        if (!found)
            return NULL;

        /* Re-descend from the new index. */
    }

    return NULL;
}

void *xas_find_marked(struct xa_state *xas, uint64 max, xa_mark_t mark)
{
    struct xarray *xa = xas->xa;
    void *head;

    if (xas_error(xas) || mark >= XA_MAX_MARKS)
        return NULL;

    if (!xas_is_restart(xas)) {
        if (xas->xa_index >= max)
            return NULL;
        xas->xa_index++;
    }

    head = xa_slot_load((void *_Atomic *)&xa->xa_head);
    if (head == NULL)
        return NULL;

    if (xa_head_to_node(head) == NULL) {
        if (xas->xa_index == 0 && xa_head_get_mark(xa, mark)) {
            xas->xa_node = NULL;
            xas->xa_offset = 0;
            xas->xa_shift = 0;
            xas->xa_index = 0;
            return head;
        }
        return NULL;
    }

    /* Walk the tree using mark bitmaps to skip unmarked subtrees. */
    while (xas->xa_index <= max) {
        xas->xa_node = XAS_RESTART;
        void *entry = xas_descend_to_leaf(xas);
        struct xa_node *node = xas->xa_node;

        if (node == XAS_BOUNDS || node == XAS_ERROR || node == XAS_RESTART)
            return NULL;

        /* Check if the found entry has the mark. */
        if (node != NULL) {
            if (entry != NULL && node_get_mark(node, xas->xa_offset, mark))
                return entry;
        } else {
            /* Single-entry in xa_head is handled by xa_head_get_mark(). */
            return NULL;
        }

        /* Advance to next marked slot. */
        bool found = false;
        while (node) {
            uint8 offset = xas->xa_offset + 1;
            while (offset < XA_CHUNK_SIZE) {
                if (node_get_mark(node, offset, mark)) {
                    uint64 base = xa_level_base(xas->xa_index, node->shift);
                    xas->xa_index = base | ((uint64)offset << node->shift);
                    found = true;
                    break;
                }
                offset++;
            }
            if (found)
                break;

            xas->xa_offset = node->offset;
            node = node->parent;
            if (node == NULL)
                return NULL;
        }

        if (!found)
            return NULL;
    }

    return NULL;
}

void xas_set_mark(struct xa_state *xas, xa_mark_t mark)
{
    struct xa_node *node = xas->xa_node;

    if (mark >= XA_MAX_MARKS)
        return;
    if (node == XAS_BOUNDS || node == XAS_ERROR || node == XAS_RESTART)
        return;

    if (node == NULL) {
        if (xas->xa_index != 0)
            return;
        void *head = xa_slot_load((void *_Atomic *)&xas->xa->xa_head);
        if (xa_is_entry(head))
            xa_head_set_mark(xas->xa, mark);
        return;
    }

    /* Set the mark bit at the leaf. */
    node_set_mark(node, xas->xa_offset, mark);

    /* Propagate upward. */
    while (node->parent) {
        uint8 off = node->offset;
        node = node->parent;
        if (node_get_mark(node, off, mark))
            break;  /* Already set — ancestors already propagated. */
        node_set_mark(node, off, mark);
    }
}

void xas_clear_mark(struct xa_state *xas, xa_mark_t mark)
{
    struct xa_node *node = xas->xa_node;

    if (mark >= XA_MAX_MARKS)
        return;
    if (node == XAS_BOUNDS || node == XAS_ERROR || node == XAS_RESTART)
        return;

    if (node == NULL) {
        if (xas->xa_index == 0)
            xa_head_clear_mark(xas->xa, mark);
        return;
    }

    node_clear_mark(node, xas->xa_offset, mark);

    /* Propagate upward: clear parent mark only if no sibling has it. */
    while (node->parent) {
        uint8 off = node->offset;
        if (node_any_mark(node, mark))
            break;  /* Some sibling still has the mark. */
        node = node->parent;
        node_clear_mark(node, off, mark);
    }
}

bool xas_get_mark(struct xa_state *xas, xa_mark_t mark)
{
    struct xa_node *node = xas->xa_node;

    if (mark >= XA_MAX_MARKS)
        return false;
    if (node == XAS_BOUNDS || node == XAS_ERROR || node == XAS_RESTART)
        return false;

    if (node == NULL) {
        if (xas->xa_index != 0)
            return false;

        void *head = xa_slot_load((void *_Atomic *)&xas->xa->xa_head);
        if (!xa_is_entry(head))
            return false;
        return xa_head_get_mark(xas->xa, mark);
    }

    return node_get_mark(node, xas->xa_offset, mark);
}

/* ====================================================================== */
/*  Simple API implementation                                              */
/* ====================================================================== */

void *xa_load(struct xarray *xa, uint64 index)
{
    void *entry;
    XA_STATE(xas, xa, index);

    rcu_read_lock();
    entry = xas_load(&xas);

    /* Handle retry entries — shouldn't normally happen because we hold
     * an RCU read lock, but be safe. */
    if (entry == XA_RETRY_ENTRY || entry == XA_ZERO_ENTRY)
        entry = NULL;
    /* Filter out internal entries. */
    if (xa_is_internal(entry))
        entry = NULL;
    rcu_read_unlock();

    return entry;
}

void *xa_store(struct xarray *xa, uint64 index, void *entry, uint64 gfp)
{
    void *old;
    XA_STATE(xas, xa, index);
    (void)gfp;  /* reserved for future use */

    xa_lock(xa);
    old = xas_store(&xas, entry);
    xa_unlock(xa);

    if (xas_error(&xas))
        return XA_ZERO_ENTRY;  /* Signal error to caller. */

    /* Filter out internal entries from the old value. */
    if (xa_is_internal(old))
        old = NULL;

    return old;
}

void *xa_erase(struct xarray *xa, uint64 index)
{
    void *old;
    XA_STATE(xas, xa, index);

    xa_lock(xa);
    old = xas_store(&xas, NULL);
    xa_unlock(xa);

    if (xa_is_internal(old))
        old = NULL;

    return old;
}

/* ====================================================================== */
/*  Destroy                                                                */
/* ====================================================================== */

/** Recursively free all xa_nodes under @node.  Does NOT free entries. */
static void xa_destroy_node(struct xa_node *node)
{
    if (node == NULL)
        return;

    if (node->shift > 0) {
        /* Internal node — recurse into children. */
        for (int i = 0; i < XA_CHUNK_SIZE; i++) {
            void *entry = node->slots[i];
            struct xa_node *child = xa_head_to_node(entry);
            if (child != NULL)
                xa_destroy_node(child);
        }
    }

    /* Free this node immediately (not RCU — destroy is called when
     * no concurrent readers exist). */
    xa_node_free_now(node);
}

void xa_destroy(struct xarray *xa)
{
    void *head;

    xa_lock(xa);
    head = xa->xa_head;
    xa->xa_head = NULL;
    xa_head_clear_all_marks(xa);
    xa_unlock(xa);

    struct xa_node *node = xa_head_to_node(head);
    if (node != NULL)
        xa_destroy_node(node);
}

/* ====================================================================== */
/*  Mark simple API                                                        */
/* ====================================================================== */

void xa_set_mark(struct xarray *xa, uint64 index, xa_mark_t mark)
{
    XA_STATE(xas, xa, index);

    xa_lock(xa);
    /* Walk to the entry. */
    void *entry = xas_load(&xas);
    if (entry != NULL)
        xas_set_mark(&xas, mark);
    xa_unlock(xa);
}

void xa_clear_mark(struct xarray *xa, uint64 index, xa_mark_t mark)
{
    XA_STATE(xas, xa, index);

    xa_lock(xa);
    void *entry = xas_load(&xas);
    if (entry != NULL)
        xas_clear_mark(&xas, mark);
    xa_unlock(xa);
}

bool xa_get_mark(struct xarray *xa, uint64 index, xa_mark_t mark)
{
    bool result;
    XA_STATE(xas, xa, index);

    rcu_read_lock();
    xas_load(&xas);
    result = xas_get_mark(&xas, mark);
    rcu_read_unlock();

    return result;
}

/* ====================================================================== */
/*  xa_find / xa_find_after                                                */
/* ====================================================================== */

/**
 * __xa_find - Internal find implementation.
 *
 * If @mark == XA_MARK_MAX, finds the next non-NULL entry regardless of marks.
 * Otherwise, finds the next entry with @mark set.
 */
static void *__xa_find(struct xarray *xa, uint64 *indexp, uint64 max,
                       xa_mark_t mark)
{
    void *entry = NULL;
    XA_STATE(xas, xa, *indexp);

    rcu_read_lock();

    if (mark == XA_MARK_MAX) {
        /* Find any entry. */
        xas.xa_node = XAS_RESTART;
        entry = xas_descend_to_leaf(&xas);

        /* If the direct index has an entry, return it. */
        if (entry != NULL && xa_is_entry(entry)) {
            *indexp = xas.xa_index;
            rcu_read_unlock();
            return entry;
        }

        /* Otherwise, scan forward. */
        entry = xas_find(&xas, max);
        if (entry != NULL) {
            *indexp = xas.xa_index;
        }
    } else {
        /* First, try the starting index directly. */
        xas.xa_node = XAS_RESTART;
        void *e = xas_descend_to_leaf(&xas);
        if (e != NULL && xas.xa_node != XAS_BOUNDS && xas.xa_node != XAS_ERROR &&
            xas.xa_node != XAS_RESTART) {
            struct xa_node *node = xas.xa_node;
            if ((node != NULL && node_get_mark(node, xas.xa_offset, mark)) ||
                (node == NULL && xas.xa_index == 0 && xa_head_get_mark(xa, mark))) {
                *indexp = xas.xa_index;
                rcu_read_unlock();
                return e;
            }
        }

        /* Scan forward for marked entry. */
        entry = xas_find_marked(&xas, max, mark);
        if (entry != NULL) {
            *indexp = xas.xa_index;
        }
    }

    rcu_read_unlock();
    return entry;
}

void *xa_find(struct xarray *xa, uint64 *indexp, uint64 max, xa_mark_t mark)
{
    return __xa_find(xa, indexp, max, mark);
}

void *xa_find_after(struct xarray *xa, uint64 *indexp, uint64 max,
                    xa_mark_t mark)
{
    if (*indexp >= max)
        return NULL;
    (*indexp)++;
    return __xa_find(xa, indexp, max, mark);
}
