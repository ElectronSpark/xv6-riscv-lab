/**
 * @file maple_tree.c
 * @brief Maple tree implementation — RCU-safe B-tree for index ranges.
 *
 * Standalone C11 implementation based on the Linux kernel maple_tree.
 *
 * Design:
 *  - 16 slots per node, 15 pivots.
 *  - Leaf nodes store void* entries; internal nodes store child pointers.
 *  - Gap tracking in internal nodes for efficient free-range search.
 *  - Optional RCU-safe reads via mt_rcu_dereference / mt_rcu_assign_pointer.
 *  - Write-side externally locked.
 *  - Freed nodes optionally deferred via mt_call_rcu.
 */

#include "maple_tree.h"

#include "types.h"
#include "defs.h"
#include "printf.h"
#include "string.h"
#include "errno.h"
#include <mm/slab.h>
#include "lock/rcu.h"

/* Kernel-side printf formatter for 64-bit hex (replaces <inttypes.h>). */
#ifndef PRIx64
#define PRIx64 "lx"
#endif

/* Error codes — keep fallbacks in case kernel errno.h is ever not pulled in. */
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EBUSY
#define EBUSY 16
#endif

/* ====================================================================== */
/*  Kernel integration — slab cache, RCU trampoline                        */
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
    printf("Maple tree subsystem initialized\n");
}

void *mt_alloc_fn(size_t size)
{
    (void)size; /* fixed-size: struct maple_node */
    void *p = slab_alloc(&__mt_node_cache);
    if (p)
        memset(p, 0, sizeof(struct maple_node));
    return p;
}

void mt_free_fn(void *ptr)
{
    if (ptr)
        slab_free(ptr);
}

void mt_rcu_read_lock(void)
{
    rcu_read_lock();
}

void mt_rcu_read_unlock(void)
{
    rcu_read_unlock();
}

void mt_call_rcu(mt_rcu_callback_t cb, void *node)
{
    struct maple_node *n = (struct maple_node *)node;
    call_rcu(&n->rcu_head, (rcu_callback_t)cb, node);
}

/* ====================================================================== */
/*  Node allocator                                                         */
/* ====================================================================== */

static struct maple_node *mt_alloc_node(void)
{
    struct maple_node *node = mt_alloc_fn(sizeof(struct maple_node));
    if (node == NULL)
        return NULL;
    /* mt_alloc_fn returns zeroed memory; type defaults to maple_leaf_64 (0). */
    node->slot_len = 0;
    node->parent = 0;
    return node;
}

static void mt_free_node_now(struct maple_node *node)
{
    if (node == NULL)
        return;
    mt_free_fn(node);
}

#ifdef MT_CONFIG_RCU
static void __mt_free_rcu_cb(void *data)
{
    mt_free_fn(data);
}

static void mt_free_node_rcu(struct maple_node *node)
{
    if (node == NULL)
        return;
    mt_call_rcu(__mt_free_rcu_cb, node);
}
#else
#define mt_free_node_rcu mt_free_node_now
#endif

/* ====================================================================== */
/*  Tagged-pointer helpers for ma_root                                     */
/* ====================================================================== */

static inline bool mt_is_node(const void *ptr)
{
    return ((uintptr_t)ptr & MAPLE_ROOT_NODE) != 0;
}

static inline struct maple_node *mt_to_node(const void *ptr)
{
    return (struct maple_node *)((uintptr_t)ptr & ~MAPLE_ROOT_NODE);
}

static inline void *mt_mk_root(struct maple_node *node)
{
    return (void *)((uintptr_t)node | MAPLE_ROOT_NODE);
}

/* ====================================================================== */
/*  Parent-pointer encoding (Linux-style, bits packed in low 8 bits)       */
/* ====================================================================== */

static inline void mn_set_parent(struct maple_node *node,
                                 struct maple_node *parent, uint8_t slot)
{
    uint8_t type = (node->parent >> MAPLE_PARENT_TYPE_SHIFT) & 0x3;
    node->parent = ((uint64_t)(uintptr_t)parent & ~(uint64_t)MAPLE_NODE_MASK)
                 | ((uint64_t)slot << MAPLE_PARENT_SLOT_SHIFT)
                 | ((uint64_t)type << MAPLE_PARENT_TYPE_SHIFT);
}

static inline void mn_set_root(struct maple_node *node)
{
    uint8_t type = (node->parent >> MAPLE_PARENT_TYPE_SHIFT) & 0x3;
    node->parent = MAPLE_PARENT_ROOT
                 | ((uint64_t)type << MAPLE_PARENT_TYPE_SHIFT);
}

static inline bool mn_is_root(const struct maple_node *node)
{
    return (node->parent & MAPLE_PARENT_ROOT) != 0;
}

static inline struct maple_node *mn_get_parent(const struct maple_node *node)
{
    return (struct maple_node *)(uintptr_t)(node->parent & ~(uint64_t)MAPLE_NODE_MASK);
}

static inline uint8_t mn_get_parent_slot(const struct maple_node *node)
{
    return (node->parent & MAPLE_PARENT_SLOT_MASK) >> MAPLE_PARENT_SLOT_SHIFT;
}

static inline void mn_set_type(struct maple_node *node, enum maple_type type)
{
    node->parent = (node->parent & ~(uint64_t)MAPLE_PARENT_TYPE_MASK)
                 | ((uint64_t)type << MAPLE_PARENT_TYPE_SHIFT);
}

static inline enum maple_type mn_get_type(const struct maple_node *node)
{
    return (enum maple_type)((node->parent & MAPLE_PARENT_TYPE_MASK)
                              >> MAPLE_PARENT_TYPE_SHIFT);
}

static inline bool mn_is_leaf(const struct maple_node *node)
{
    return mn_get_type(node) == maple_leaf_64;
}

/* ====================================================================== */
/*  RCU mode & dead-node helpers                                           */
/* ====================================================================== */

/**
 * mt_in_rcu - Check whether the tree is operating in RCU mode.
 *
 * When true, structural modifications (pivot/slot_len changes) must
 * copy-on-write: allocate a new node, modify the copy, publish it,
 * mark the old node dead, and defer freeing via RCU.
 *
 * Returns true only when the library is compiled with MT_CONFIG_RCU
 * *and* the tree's MT_FLAGS_USE_RCU flag is set.
 */
static inline bool mt_in_rcu(const struct maple_tree *mt)
{
#ifdef MT_CONFIG_RCU
    return (mt->ma_flags & MT_FLAGS_USE_RCU) != 0;
#else
    (void)mt;
    return false;
#endif
}

/**
 * mn_is_dead - Check whether a node has been replaced and is awaiting
 * RCU-deferred freeing.  Readers that land on a dead node must retry.
 *
 * A dead node is marked by setting parent to point to itself.
 */
static inline bool mn_is_dead(const struct maple_node *node)
{
    mt_smp_rmb();
    return (uintptr_t)(node->parent & ~(uint64_t)MAPLE_NODE_MASK)
        == (uintptr_t)node;
}

/**
 * mn_mark_dead - Mark a node as dead (replaced).
 *
 * Sets the parent pointer to the node itself (preserving low-bit encoding
 * is unnecessary since no one will read metadata from a dead node).
 */
static inline void mn_mark_dead(struct maple_node *node)
{
    node->parent = (uint64_t)(uintptr_t)node;
}

/**
 * mn_copy_node - Allocate a new node that is a shallow copy of @src.
 *
 * Copies all fields (parent, slot_len, pivots, slots, gaps).
 * The caller must fix up parent pointers on children and the parent link
 * in the new node before publishing it.
 */
static inline struct maple_node *mn_copy_node(const struct maple_node *src)
{
    struct maple_node *dst = mt_alloc_node();
    if (dst == NULL)
        return NULL;
    memcpy(dst, src, sizeof(struct maple_node));
    return dst;
}

/**
 * mn_replace_node - Replace @old with @new in @old's parent.
 *
 * Publishes @new via rcu_assign_pointer into the parent slot,
 * marks @old dead, and defers freeing.  If @old is the tree root,
 * updates mt->ma_root instead.
 *
 * Callers must have already set up @new's parent encoding (mn_set_parent
 * or mn_set_root) and re-parented @new's children.
 */
static inline void mn_retire_node_rcu(struct maple_node *node);

static inline void mn_replace_node(struct maple_tree *mt,
                                    struct maple_node *old,
                                    struct maple_node *new_node)
{
    if (mn_is_root(old)) {
        mn_set_root(new_node);
        mt_rcu_assign_pointer(&mt->ma_root, mt_mk_root(new_node));
    } else {
        struct maple_node *parent = mn_get_parent(old);
        uint8_t pslot = mn_get_parent_slot(old);
        mn_set_parent(new_node, parent, pslot);
        mt_rcu_assign_pointer(&parent->slot[pslot], new_node);
    }
    mn_retire_node_rcu(old);
}

static inline void mn_retire_node_rcu(struct maple_node *node)
{
    mn_mark_dead(node);
    mt_free_node_rcu(node);
}

static inline void mn_drop_root_node(struct maple_tree *mt,
                                     struct maple_node *node)
{
    mt_rcu_assign_pointer(&mt->ma_root, NULL);
    if (mt_in_rcu(mt)) {
        mn_retire_node_rcu(node);
        return;
    }
    mt_free_node_now(node);
}

/* ====================================================================== */
/*  Parent-child relationship helpers                                      */
/* ====================================================================== */

/**
 * mn_reparent_children - Fix up parent_slot encoding for children[from..to).
 *
 * Call after shifting or copying child pointers so each child's encoded
 * parent_slot matches its actual position in @parent.
 */
static inline void mn_reparent_children(struct maple_node *parent,
                                     uint8_t from, uint8_t to)
{
    for (uint8_t i = from; i < to; i++) {
        struct maple_node *child = parent->slot[i];
        if (child != NULL)
            mn_set_parent(child, parent, i);
    }
}

/**
 * mn_link_child - Establish a parent-child relationship.
 *
 * Sets @parent->slot[@slot] to @child and encodes the back-pointer in
 * @child so that mn_get_parent() / mn_get_parent_slot() work correctly.
 */
static inline void mn_link_child(struct maple_node *parent, uint8_t slot,
                                  struct maple_node *child)
{
    mt_rcu_assign_pointer(&parent->slot[slot], child);
    mn_set_parent(child, parent, slot);
}

/**
 * mn_truncate - Set @node's slot_len and clear trailing slots/gaps.
 *
 * Clears slot[new_len..MAPLE_NODE_SLOTS-1] to NULL and gap[] to 0.
 */
static inline void mn_truncate(struct maple_node *node, uint8_t new_len)
{
    node->slot_len = new_len;
    for (uint8_t i = new_len; i < MAPLE_NODE_SLOTS; i++) {
        node->slot[i] = NULL;
        node->gap[i] = 0;
    }
}

/**
 * mn_copy_entries - Copy @count entries from @src to @dst.
 *
 * Copies slot, pivot (for all but the last entry), and optionally gap.
 */
static inline void mn_copy_entries(struct maple_node *dst, uint8_t dst_off,
                                    const struct maple_node *src,
                                    uint8_t src_off, uint8_t count,
                                    bool copy_gaps)
{
    for (uint8_t i = 0; i < count; i++) {
        dst->slot[dst_off + i] = src->slot[src_off + i];
        if (i < count - 1 && (dst_off + i) < MAPLE_NODE_PIVOTS)
            dst->pivot[dst_off + i] = src->pivot[src_off + i];
        if (copy_gaps)
            dst->gap[dst_off + i] = src->gap[src_off + i];
    }
}

/**
 * mn_write_slots - Write @count slot/pivot pairs from arrays into @node.
 *
 * @pos:      Starting slot index in @node.
 * @node_len: Total slot_len of the node (for pivot-boundary guard).
 */
static inline void mn_write_slots(struct maple_node *node, uint8_t pos,
                                   void *slots[], uint64_t pivots[],
                                   uint8_t count, uint8_t node_len)
{
    for (uint8_t i = 0; i < count; i++) {
        node->slot[pos + i] = slots[i];
        if ((pos + i) < node_len - 1)
            node->pivot[pos + i] = pivots[i];
    }
}

/**
 * mn_shift_down - Remove slot @pos by shifting later entries down.
 *
 * Shifts slots, pivots, and gaps (for internal nodes) down by one.
 * Decrements slot_len and clears the trailing slot.
 */
static inline void mn_shift_down(struct maple_node *node, uint8_t pos)
{
    bool shift_gaps = !mn_is_leaf(node);
    for (uint8_t i = pos; i + 1 < node->slot_len; i++) {
        node->slot[i] = node->slot[i + 1];
        if (i < MAPLE_NODE_PIVOTS && i + 1 < node->slot_len - 1)
            node->pivot[i] = node->pivot[i + 1];
        if (shift_gaps)
            node->gap[i] = node->gap[i + 1];
    }
    node->slot_len--;
    node->slot[node->slot_len] = NULL;
}

/**
 * mn_remove_child - Remove slot @pos from @node, shifting later entries down.
 *
 * Handles slots, pivots, gaps (for internal nodes), slot_len decrement,
 * trailing slot clear, and re-parenting of shifted children.
 */
static inline void mn_remove_child(struct maple_node *node, uint8_t pos)
{
    mn_shift_down(node, pos);
    mn_reparent_children(node, pos, node->slot_len);
}

/* ====================================================================== */
/*  Pivot helpers                                                          */
/* ====================================================================== */

static inline uint64_t mn_pivot(const struct maple_node *node, uint8_t i,
                                 uint64_t node_max)
{
    if (i >= node->slot_len - 1)
        return node_max;
    return node->pivot[i];
}

static inline uint64_t mn_slot_min(const struct maple_node *node, uint8_t i,
                                    uint64_t node_min)
{
    if (i == 0)
        return node_min;
    return node->pivot[i - 1] + 1;
}

/* ====================================================================== */
/*  Pivot binary search                                                    */
/* ====================================================================== */

/**
 * mas_set_slot - Position the cursor on @node at @slot.
 *
 * Sets node, offset, min, and max in a single call.
 * Callers that also need mas->index updated should do so afterwards.
 */
static inline void mas_set_slot(struct ma_state *mas,
                                struct maple_node *node, uint8_t slot,
                                uint64_t node_min, uint64_t node_max)
{
    mas->node   = node;
    mas->offset = slot;
    mas->min    = mn_slot_min(node, slot, node_min);
    mas->max    = mn_pivot(node, slot, node_max);
}

/**
 * mas_set_bounds - Position the cursor with pre-computed bounds.
 *
 * Like mas_set_slot(), but uses caller-supplied min/max directly
 * instead of deriving them from pivots. Also sets index = min so later
 * iteration resumes from the start of the explicit bounds.
 */
static inline void mas_set_bounds(struct ma_state *mas,
                                  struct maple_node *node, uint8_t slot,
                                  uint64_t min, uint64_t max)
{
    mas->node   = node;
    mas->offset = slot;
    mas->min    = min;
    mas->max    = max;
    mas->index  = min;
}

/**
 * mas_invalidate - Mark the cursor as unpositioned.
 */
static inline void mas_invalidate(struct ma_state *mas)
{
    mas->node = NULL;
    mas->min = 0;
    mas->max = MAPLE_MAX;
    mas->offset = 0;
    mas->depth = 0;
}

static inline void mas_request_retry(struct ma_state *mas)
{
    mas_invalidate(mas);
    mas->depth = UINT8_MAX;
}

static inline bool mas_retry_requested(const struct ma_state *mas)
{
    return mas->depth == UINT8_MAX;
}

static inline bool mas_node_dead(const struct ma_state *mas,
                                 const struct maple_node *node)
{
    return node != NULL && mt_in_rcu(mas->tree) && mn_is_dead(node);
}

static inline void mas_store_frame(struct ma_state *mas, uint8_t level,
                                   struct maple_node *node, uint8_t slot,
                                   uint64_t node_min, uint64_t node_max)
{
    mas->path[level].node = node;
    mas->path[level].slot = slot;
    mas->path[level].node_min = node_min;
    mas->path[level].node_max = node_max;
}

static inline struct maple_path_frame *mas_frame(struct ma_state *mas,
                                                 uint8_t level)
{
    return &mas->path[level];
}

static inline const struct maple_path_frame *mas_frame_const(
    const struct ma_state *mas, uint8_t level)
{
    return &mas->path[level];
}

static inline void mas_apply_frame(struct ma_state *mas, uint8_t level)
{
    const struct maple_path_frame *frame = mas_frame_const(mas, level);
    mas->node = frame->node;
    mas->offset = frame->slot;
    mas->min = mn_slot_min(frame->node, frame->slot, frame->node_min);
    mas->max = mn_pivot(frame->node, frame->slot, frame->node_max);
    mas->depth = level + 1;
}

static inline bool mas_path_full(uint8_t depth)
{
    return depth >= MAPLE_CURSOR_MAX_DEPTH;
}

static void *mas_descend_subtree(struct ma_state *mas,
                                 struct maple_node *node,
                                 uint64_t node_min, uint64_t node_max,
                                 uint8_t start_level, bool reverse)
{
    uint8_t level = start_level;

    while (1) {
        if (node == NULL)
            return NULL;
        if (mas_node_dead(mas, node)) {
            mas_request_retry(mas);
            return NULL;
        }
        if (mas_path_full(level)) {
            mas_invalidate(mas);
            return NULL;
        }

        uint8_t len = READ_ONCE(node->slot_len);
        if (len == 0)
            return NULL;

        int start = reverse ? (int)len - 1 : 0;
        int end = reverse ? -1 : (int)len;
        int step = reverse ? -1 : 1;

        for (int i = start; i != end; i += step) {
            uint8_t slot = (uint8_t)i;
            void *entry = mt_rcu_dereference(&node->slot[slot]);
            if (entry == NULL)
                continue;

            mas_store_frame(mas, level, node, slot, node_min, node_max);

            if (mn_is_leaf(node)) {
                mas_apply_frame(mas, level);
                mas->index = mas->min;
                return entry;
            }

            node_min = mn_slot_min(node, slot, node_min);
            node_max = mn_pivot(node, slot, node_max);
            node = (struct maple_node *)entry;
            level++;
            goto next_level;
        }

        return NULL;

next_level:
        continue;
    }
}

enum mas_rewalk_result {
    mas_rewalk_stop,
    mas_rewalk_continue,
    mas_rewalk_found,
};

static inline uint64_t mas_next_retry_index(const struct ma_state *mas)
{
    if (mas->node == NULL)
        return mas->index;
    return (mas->max == MAPLE_MAX) ? MAPLE_MAX : mas->max + 1;
}

static inline uint64_t mas_prev_retry_index(const struct ma_state *mas)
{
    if (mas->node == NULL)
        return mas->index;
    return (mas->min == 0) ? 0 : mas->min - 1;
}

static inline enum mas_rewalk_result
mas_rewalk_to_index(struct ma_state *mas, uint64_t retry_index,
                    uint64_t limit, bool reverse, void **entry_out)
{
    *entry_out = NULL;

    if ((!reverse && retry_index > limit) || (reverse && retry_index < limit)) {
        mas_invalidate(mas);
        mas->depth = 0;
        return mas_rewalk_stop;
    }

    mas->depth = 0;
    mas->index = retry_index;
    *entry_out = mas_walk(mas);
    if (*entry_out != NULL) {
        if ((!reverse && mas->min > limit) || (reverse && mas->max < limit)) {
            *entry_out = NULL;
            return mas_rewalk_stop;
        }
        return mas_rewalk_found;
    }

    if (mas->node == NULL)
        return mas_rewalk_stop;

    return mas_rewalk_continue;
}

static inline void mas_restart_retry(struct ma_state *mas,
                                     uint64_t retry_index)
{
    mas->depth = 0;
    mas->index = retry_index;
    mas_invalidate(mas);
}

/**
 * mas_advance_index - Move index past the current entry.
 *
 * Sets mas->index to mas->max + 1, clamped at MAPLE_MAX.
 */
static inline void mas_advance_index(struct ma_state *mas)
{
    mas->index = (mas->max == MAPLE_MAX) ? MAPLE_MAX : mas->max + 1;
}

/**
 * mas_validate_result - Apply limit checks to a candidate entry.
 *
 * Returns @entry when the current cursor bounds still satisfy @limit.
 * On a bounded miss, invalidates the cursor so callers do not retain a
 * stale position. When @advance is true, also moves mas->index past the
 * accepted entry.
 */
static inline void *mas_validate_result(struct ma_state *mas, void *entry,
                                        uint64_t limit, bool reverse,
                                        bool advance)
{
    if (entry == NULL)
        return NULL;

    if ((!reverse && mas->min > limit) || (reverse && mas->max < limit)) {
        mas_invalidate(mas);
        return NULL;
    }

    if (advance)
        mas_advance_index(mas);

    return entry;
}

/**
 * mas_set_gap - Record a found gap of @size starting at @start.
 */
static inline void mas_set_gap(struct ma_state *mas, uint64_t start,
                               uint64_t size)
{
    mas->index = start;
    mas->last  = start + size - 1;
}

/**
 * __mn_bsearch - Binary-search the pivot array for the slot covering @index.
 *
 * Returns the smallest slot in [lo, len-1] whose pivot >= @index.
 * The caller must ensure len > 0.
 */
static inline uint8_t __mn_bsearch(const struct maple_node *node,
                                    uint64_t index, uint8_t lo, uint8_t len)
{
    uint8_t hi = len - 1;
    while (lo < hi) {
        uint8_t mid = (lo + hi) >> 1;
        uint64_t piv = READ_ONCE(node->pivot[mid]);
        if (index <= piv)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

static inline uint8_t mn_find_slot(const struct maple_node *node,
                                    uint64_t index, uint8_t len)
{
    return __mn_bsearch(node, index, 0, len);
}

static inline uint8_t mn_find_slot_from(const struct maple_node *node,
                                         uint64_t index, uint8_t from,
                                         uint8_t len)
{
    return __mn_bsearch(node, index, from, len);
}

/* ====================================================================== */
/*  Walk / lookup                                                          */
/* ====================================================================== */

static void *__mt_lookup(struct maple_tree *mt, uint64_t index)
{
    void *root = mt_rcu_dereference(&mt->ma_root);
    if (root == NULL)
        return NULL;
    if (!mt_is_node(root))
        return root;

    struct maple_node *node = mt_to_node(root);
    uint64_t node_min = 0;
    uint64_t node_max = MAPLE_MAX;

    while (1) {
        uint8_t len = READ_ONCE(node->slot_len);
        if (len == 0)
            return NULL;

        uint8_t slot = mn_find_slot(node, index, len);
        void *entry = mt_rcu_dereference(&node->slot[slot]);

        if (mn_is_leaf(node))
            return entry;

        if (entry == NULL)
            return NULL;

        node_min = mn_slot_min(node, slot, node_min);
        node_max = mn_pivot(node, slot, node_max);
        node = (struct maple_node *)entry;
    }
}

/**
 * mtree_load - Look up the entry at @index.
 *
 * RCU sequence (read-only — no structural changes):
 *  1. mt_rcu_lock()   — enter RCU read-side critical section.
 *  2. Walk top-down via mt_rcu_dereference() at each level.
 *  3. mt_rcu_unlock() — leave critical section.
 *
 * Write-side locking: none required.
 */
void *mtree_load(struct maple_tree *mt, uint64_t index)
{
    void *entry;
    mt_rcu_lock();
    entry = __mt_lookup(mt, index);
    mt_rcu_unlock();
    return entry;
}

/* ====================================================================== */
/*  MAS walk                                                               */
/* ====================================================================== */

/**
 * mas_walk - Walk to the leaf slot covering mas->index.
 *
 * RCU sequence (read-only — no structural changes):
 *  1. Walk top-down via mt_rcu_dereference() at each level.
 *  2. Populate mas->node/min/max/offset with the leaf position.
 *
 * Caller must hold mt_rcu_lock() or the tree lock.
 */
void *mas_walk(struct ma_state *mas)
{
    struct maple_tree *mt = mas->tree;
    void *root = mt_rcu_dereference(&mt->ma_root);

    mas_invalidate(mas);

    if (root == NULL) {
        return NULL;
    }

    if (!mt_is_node(root)) {
        return root;
    }

    struct maple_node *node = mt_to_node(root);
    uint64_t node_min = 0;
    uint64_t node_max = MAPLE_MAX;
    uint8_t depth = 0;

    while (1) {
        if (mas_path_full(depth)) {
            mas_invalidate(mas);
            return NULL;
        }

        uint8_t len = READ_ONCE(node->slot_len);
        if (len == 0) {
            mas_invalidate(mas);
            return NULL;
        }

        uint8_t slot = mn_find_slot(node, mas->index, len);
        mas_store_frame(mas, depth, node, slot, node_min, node_max);

        if (mn_is_leaf(node)) {
            void *entry = mt_rcu_dereference(&node->slot[slot]);
            mas_apply_frame(mas, depth);
            return entry;
        }

        void *child = mt_rcu_dereference(&node->slot[slot]);
        if (child == NULL) {
            mas_apply_frame(mas, depth);
            return NULL;
        }

        node_min = mn_slot_min(node, slot, node_min);
        node_max = mn_pivot(node, slot, node_max);
        node = (struct maple_node *)child;
        depth++;
    }
}

/* ====================================================================== */
/*  Node splitting                                                         */
/* ====================================================================== */

static int __mt_split_node(struct maple_tree *mt, struct maple_node *node,
                           uint64_t node_min, uint64_t node_max);

static void __find_node_bounds(struct maple_tree *mt, struct maple_node *node,
                               uint64_t *out_min, uint64_t *out_max);

static void __mt_shrink_root(struct maple_tree *mt);

static uint64_t __mt_range_size(uint64_t start, uint64_t end)
{
    if (end < start)
        return 0;
    if (start == 0 && end == MAPLE_MAX)
        return MAPLE_MAX;
    return end - start + 1;
}

static uint64_t __mt_node_max_gap(struct maple_node *node,
                                   uint64_t node_min, uint64_t node_max)
{
    if (node == NULL)
        return __mt_range_size(node_min, node_max);

    uint64_t max_gap = 0;

    if (mn_is_leaf(node)) {
        for (uint8_t i = 0; i < node->slot_len; i++) {
            if (node->slot[i] != NULL)
                continue;
            uint64_t slot_min = mn_slot_min(node, i, node_min);
            uint64_t slot_max = mn_pivot(node, i, node_max);
            uint64_t gap = __mt_range_size(slot_min, slot_max);
            if (gap > max_gap)
                max_gap = gap;
        }
        return max_gap;
    }

    for (uint8_t i = 0; i < node->slot_len; i++) {
        uint64_t slot_min = mn_slot_min(node, i, node_min);
        uint64_t slot_max = mn_pivot(node, i, node_max);
        struct maple_node *child = (struct maple_node *)node->slot[i];
        uint64_t gap;
        if (child == NULL) {
            gap = __mt_range_size(slot_min, slot_max);
        } else if (mn_is_leaf(child)) {
            gap = __mt_node_max_gap(child, slot_min, slot_max);
        } else {
            gap = 0;
            for (uint8_t j = 0; j < child->slot_len; j++) {
                uint64_t child_gap;
                if (child->slot[j] == NULL) {
                    uint64_t child_slot_min = mn_slot_min(child, j, slot_min);
                    uint64_t child_slot_max = mn_pivot(child, j, slot_max);
                    child_gap = __mt_range_size(child_slot_min, child_slot_max);
                } else {
                    child_gap = child->gap[j];
                }
                if (child_gap > gap)
                    gap = child_gap;
            }
        }
        if (gap > max_gap)
            max_gap = gap;
    }
    return max_gap;
}

static uint64_t __mt_refresh_node_gap(struct maple_node *node,
                                       uint64_t node_min, uint64_t node_max)
{
    if (node == NULL)
        return __mt_range_size(node_min, node_max);

    if (mn_is_leaf(node))
        return __mt_node_max_gap(node, node_min, node_max);

    uint64_t max_gap = 0;
    for (uint8_t i = 0; i < node->slot_len; i++) {
        uint64_t slot_min = mn_slot_min(node, i, node_min);
        uint64_t slot_max = mn_pivot(node, i, node_max);
        struct maple_node *child = (struct maple_node *)node->slot[i];
        uint64_t gap = __mt_node_max_gap(child, slot_min, slot_max);
        node->gap[i] = gap;
        if (gap > max_gap)
            max_gap = gap;
    }
    for (uint8_t i = node->slot_len; i < MAPLE_NODE_SLOTS; i++)
        node->gap[i] = 0;
    return max_gap;
}

static void __mt_propagate_gaps_from(struct maple_tree *mt,
                                      struct maple_node *node)
{
    while (node != NULL) {
        uint64_t node_min, node_max;
        __find_node_bounds(mt, node, &node_min, &node_max);
        __mt_refresh_node_gap(node, node_min, node_max);

        if (mn_is_root(node))
            break;
        node = mn_get_parent(node);
    }
}

/* ====================================================================== */
/*  Gap search                                                             */
/* ====================================================================== */

static int __mt_find_gap_dir(struct maple_node *node, uint64_t node_min,
                             uint64_t node_max, uint64_t size,
                             uint64_t search_min, uint64_t search_max,
                             uint64_t *gap_start, bool reverse)
{
    if (node == NULL)
        return -EBUSY;

    int start = reverse ? (int)node->slot_len - 1 : 0;
    int end = reverse ? -1 : (int)node->slot_len;
    int step = reverse ? -1 : 1;

    if (mn_is_leaf(node)) {
        for (int i = start; i != end; i += step) {
            uint8_t slot = (uint8_t)i;
            uint64_t slot_min = mn_slot_min(node, slot, node_min);
            uint64_t slot_max = mn_pivot(node, slot, node_max);
            if (slot_max < search_min || slot_min > search_max)
                continue;
            if (node->slot[slot] != NULL)
                continue;

            uint64_t clipped_min = slot_min < search_min ? search_min : slot_min;
            uint64_t clipped_max = slot_max > search_max ? search_max : slot_max;
            if (__mt_range_size(clipped_min, clipped_max) < size)
                continue;

            *gap_start = reverse ? clipped_max - size + 1 : clipped_min;
            return 0;
        }
        return -EBUSY;
    }

    for (int i = start; i != end; i += step) {
        uint8_t slot = (uint8_t)i;
        uint64_t slot_min = mn_slot_min(node, slot, node_min);
        uint64_t slot_max = mn_pivot(node, slot, node_max);
        if (slot_max < search_min || slot_min > search_max)
            continue;

        uint64_t clipped_min = slot_min < search_min ? search_min : slot_min;
        uint64_t clipped_max = slot_max > search_max ? search_max : slot_max;
        if (__mt_range_size(clipped_min, clipped_max) < size)
            continue;

        struct maple_node *child = (struct maple_node *)node->slot[slot];
        if (child == NULL) {
            *gap_start = reverse ? clipped_max - size + 1 : clipped_min;
            return 0;
        }
        if (node->gap[slot] < size)
            continue;
        if (__mt_find_gap_dir(child, slot_min, slot_max, size,
                              search_min, search_max, gap_start,
                              reverse) == 0)
            return 0;
    }
    return -EBUSY;
}

static int __mt_find_gap_fwd(struct maple_node *node, uint64_t node_min,
                             uint64_t node_max, uint64_t size,
                             uint64_t search_min, uint64_t search_max,
                             uint64_t *gap_start)
{
    return __mt_find_gap_dir(node, node_min, node_max, size,
                             search_min, search_max, gap_start, false);
}

static int __mt_find_gap_rev(struct maple_node *node, uint64_t node_min,
                             uint64_t node_max, uint64_t size,
                             uint64_t search_min, uint64_t search_max,
                             uint64_t *gap_start)
{
    return __mt_find_gap_dir(node, node_min, node_max, size,
                             search_min, search_max, gap_start, true);
}

/* ====================================================================== */
/*  Insert into parent / split helpers                                     */
/* ====================================================================== */

static int __mt_insert_into_parent(struct maple_tree *mt,
                                    struct maple_node *parent,
                                    uint8_t slot,
                                    uint64_t pivot_val,
                                    struct maple_node *right,
                                    uint64_t parent_min,
                                    uint64_t parent_max)
{
    if (parent->slot_len < MAPLE_NODE_SLOTS) {
        uint8_t len = parent->slot_len;

        /*
         * RCU mode: copy-on-write the parent so readers never see a
         * partially-shifted node.
         */
        struct maple_node *target = parent;
        if (mt_in_rcu(mt)) {
            target = mn_copy_node(parent);
            if (target == NULL)
                return -ENOMEM;
        }

        for (int i = len; i > (int)slot + 1; i--) {
            target->slot[i] = parent->slot[i - 1];
            if (i - 1 < MAPLE_NODE_PIVOTS)
                target->pivot[i - 1] = parent->pivot[i - 2];
            if (!mn_is_leaf(parent))
                target->gap[i] = parent->gap[i - 1];
        }
        target->pivot[slot] = pivot_val;
        target->slot_len = len + 1;

        mn_link_child(target, slot + 1, right);

        if (!mn_is_leaf(target))
            mn_reparent_children(target, 0, target->slot_len);

        if (mt_in_rcu(mt))
            mn_replace_node(mt, parent, target);

        return 0;
    }

    /* Parent full — split it first. */
    struct maple_node *left_child = parent->slot[slot];

    int ret = __mt_split_node(mt, parent, parent_min, parent_max);
    if (ret != 0)
        return ret;

    struct maple_node *new_parent = mn_get_parent(left_child);
    uint8_t new_slot = mn_get_parent_slot(left_child);

    uint64_t new_pmin, new_pmax;
    __find_node_bounds(mt, new_parent, &new_pmin, &new_pmax);

    return __mt_insert_into_parent(mt, new_parent, new_slot, pivot_val,
                                    right, new_pmin, new_pmax);
}

static void __find_node_bounds(struct maple_tree *mt, struct maple_node *node,
                                uint64_t *out_min, uint64_t *out_max)
{
    if (mn_is_root(node)) {
        *out_min = 0;
        *out_max = MAPLE_MAX;
        return;
    }

    struct maple_node *parent = mn_get_parent(node);
    uint64_t pmin, pmax;
    __find_node_bounds(mt, parent, &pmin, &pmax);

    uint8_t slot = mn_get_parent_slot(node);
    *out_min = mn_slot_min(parent, slot, pmin);
    *out_max = mn_pivot(parent, slot, pmax);
}

static int __mt_split_node(struct maple_tree *mt, struct maple_node *node,
                            uint64_t node_min, uint64_t node_max)
{
    (void)node_min;
    (void)node_max;

    struct maple_node *right = mt_alloc_node();
    if (right == NULL)
        return -ENOMEM;

    uint8_t split = node->slot_len >> 1;
    uint64_t median = node->pivot[split - 1];
    mn_set_type(right, mn_get_type(node));

    uint8_t right_len = node->slot_len - split;
    mn_copy_entries(right, 0, node, split, right_len, !mn_is_leaf(node));
    right->slot_len = right_len;

    if (!mn_is_leaf(right))
        mn_reparent_children(right, 0, right_len);

    /*
     * RCU mode: copy-on-write the left half instead of mutating in place.
     * Non-RCU: modify node directly.
     */
    struct maple_node *left = node;
    if (mt_in_rcu(mt)) {
        left = mn_copy_node(node);
        if (left == NULL) {
            mt_free_node_now(right);
            return -ENOMEM;
        }
    }
    /* Clear the right-half slots/gaps on left to avoid stale pointers. */
    mn_truncate(left, split);

    if (!mn_is_leaf(left) && mt_in_rcu(mt))
        mn_reparent_children(left, 0, split);

    if (mn_is_root(node)) {
        struct maple_node *new_root = mt_alloc_node();
        if (new_root == NULL) {
            if (mt_in_rcu(mt)) {
                mt_free_node_now(left);
            } else {
                /* Undo split. */
                mn_copy_entries(node, split, right, 0, right_len,
                                !mn_is_leaf(node));
                node->slot_len = split + right_len;
                if (!mn_is_leaf(node))
                    mn_reparent_children(node, split, node->slot_len);
            }
            mt_free_node_now(right);
            return -ENOMEM;
        }
        mn_set_type(new_root, maple_arange_64);
        new_root->pivot[0] = median;
        new_root->slot_len = 2;

        mn_set_root(new_root);
        mn_link_child(new_root, 0, left);
        mn_link_child(new_root, 1, right);

        mt_rcu_assign_pointer(&mt->ma_root, mt_mk_root(new_root));

        if (mt_in_rcu(mt)) {
            mn_retire_node_rcu(node);
        }
        return 0;
    }

    struct maple_node *parent = mn_get_parent(node);
    uint8_t pslot = mn_get_parent_slot(node);

    /*
     * In RCU mode, replace node with left in the parent slot before
     * inserting right, so __mt_insert_into_parent sees the new left.
     */
    if (mt_in_rcu(mt)) {
        mn_set_parent(left, parent, pslot);
        mt_rcu_assign_pointer(&parent->slot[pslot], left);
        mn_retire_node_rcu(node);
    }

    uint64_t pmin, pmax;
    __find_node_bounds(mt, parent, &pmin, &pmax);

    int ret = __mt_insert_into_parent(mt, parent, pslot, median, right,
                                       pmin, pmax);
    if (ret != 0) {
        if (!mt_in_rcu(mt)) {
            /* Undo split (non-RCU only — in RCU mode node is already dead). */
            mn_copy_entries(node, split, right, 0, right_len,
                            !mn_is_leaf(node));
            node->slot_len = split + right_len;
            if (!mn_is_leaf(node))
                mn_reparent_children(node, split, node->slot_len);
        }
        mt_free_node_now(right);
        return ret;
    }
    return 0;
}

/* ====================================================================== */
/*  Store (insert / overwrite)                                             */
/* ====================================================================== */

static int __mt_store_leaf(struct maple_tree *mt, struct maple_node *node,
                            uint8_t slot, uint64_t node_min, uint64_t node_max,
                            uint64_t first, uint64_t last, void *entry,
                            struct maple_node **new_out)
{
    uint64_t slot_min = mn_slot_min(node, slot, node_min);
    uint64_t slot_max = mn_pivot(node, slot, node_max);

    /* Exact match — just overwrite. */
    if (slot_min == first && slot_max == last) {
        mt_rcu_assign_pointer(&node->slot[slot], entry);
        return 0;
    }

    void *old_entry = node->slot[slot];
    int need_left  = (first > slot_min) ? 1 : 0;
    int need_right = (last < slot_max) ? 1 : 0;
    int extra = need_left + need_right;

    if (node->slot_len + extra > MAPLE_NODE_SLOTS) {
        int ret = __mt_split_node(mt, node, node_min, node_max);
        if (ret != 0)
            return ret;
        ret = mtree_store_range(mt, first, last, entry);
        /* Return 1 (positive) to tell caller the store completed via
         * re-walk; node may be dead in RCU mode, so skip post-processing. */
        return ret != 0 ? ret : 1;
    }

    int n_pieces = 1 + need_left + need_right;
    void  *p_slot[3];
    uint64_t p_pivot[3];

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

    uint8_t old_len = node->slot_len;
    uint8_t new_len = old_len + (uint8_t)extra;

    /*
     * RCU mode: copy-on-write.  Build the new layout on a fresh node.
     * The copy is NOT published yet — the caller does that after
     * coalescing via mn_replace_node().
     */
    struct maple_node *target = node;
    if (mt_in_rcu(mt)) {
        target = mn_copy_node(node);
        if (target == NULL)
            return -ENOMEM;
    }

    for (int i = new_len - 1; i >= (int)slot + n_pieces; i--) {
        int src = i - extra;
        target->slot[i] = node->slot[src];
        if (src < (int)old_len - 1)
            target->pivot[i] = node->pivot[src];
    }

    mn_write_slots(target, slot, p_slot, p_pivot, (uint8_t)n_pieces,
                    new_len);

    target->slot_len = new_len;

    /* In RCU mode, return the unpublished new node via *new_out. */
    if (mt_in_rcu(mt))
        *new_out = target;

    return 0;
}

static void __mt_coalesce_leaf(struct maple_node *node, uint64_t node_max)
{
    (void)node_max;
    if (node->slot_len <= 1)
        return;

    uint8_t dst = 0;
    for (uint8_t src = 1; src < node->slot_len; src++) {
        if (node->slot[src] == node->slot[dst]) {
            if (src < node->slot_len - 1)
                node->pivot[dst] = node->pivot[src];
        } else {
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

/**
 * mtree_store_range - Store @entry for indices [@first, @last].
 *
 * RCU sequence (per affected leaf, and per parent on split):
 *  1. Walk to the target leaf (no RCU dereference needed — write-locked).
 *  2. mn_copy_node() — allocate a new node, shallow-copy the old.
 *  3. Modify the copy (insert/split slots, update pivots).
 *  4. mn_reparent_children() — update children's parent back-pointers
 *     to the new node (safe: readers traverse top-down, not bottom-up).
 *  5. mn_replace_node() — single rcu_assign_pointer() into the parent
 *     slot (or ma_root), then mark old node dead, RCU-defer-free it.
 *  6. __mt_propagate_gaps_from() — update gap[] values upward.
 *
 * For exact-match slot overwrites, steps 2–5 are skipped; only
 * rcu_assign_pointer on the slot is needed (tier A — in-place store).
 *
 * On split: the leaf is COW'd first, then __mt_insert_into_parent()
 * COW's the parent (same 5-step sequence) to add the new right child.
 * If the parent is also full, it splits recursively upward.
 *
 * Caller must hold the tree lock.
 */
int mtree_store_range(struct maple_tree *mt, uint64_t first, uint64_t last,
                       void *entry)
{
    if (first > last)
        return -EINVAL;

    void *root = mt_rcu_dereference(&mt->ma_root);

    if (root == NULL && entry == NULL)
        return 0;

    /* Empty tree: create first node. */
    if (root == NULL) {
        struct maple_node *node = mt_alloc_node();
        if (node == NULL)
            return -ENOMEM;
        mn_set_type(node, maple_leaf_64);
        if (first > 0) {
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
        mt_rcu_assign_pointer(&mt->ma_root, mt_mk_root(node));
        return 0;
    }

    if (!mt_is_node(root)) {
        struct maple_node *node = mt_alloc_node();
        if (node == NULL)
            return -ENOMEM;
        mn_set_type(node, maple_leaf_64);
        node->slot[0] = root;
        node->slot_len = 1;
        mn_set_root(node);
        mt_rcu_assign_pointer(&mt->ma_root, mt_mk_root(node));
        root = mt_rcu_dereference(&mt->ma_root);
    }

    struct maple_node *node = mt_to_node(root);
    uint64_t node_min = 0;
    uint64_t node_max = MAPLE_MAX;

    while (!mn_is_leaf(node)) {
        uint8_t slot = mn_find_slot(node, first, node->slot_len);
        uint64_t child_min = mn_slot_min(node, slot, node_min);
        uint64_t child_max = mn_pivot(node, slot, node_max);

        struct maple_node *child = node->slot[slot];
        if (child == NULL) {
            child = mt_alloc_node();
            if (child == NULL)
                return -ENOMEM;
            mn_set_type(child, maple_leaf_64);
            child->slot[0] = NULL;
            child->slot_len = 1;
            mn_link_child(node, slot, child);
        }
        node_min = child_min;
        node_max = child_max;
        node = child;
    }

    uint64_t orig_last = last;
    if (last > node_max)
        last = node_max;

    uint8_t start_slot = mn_find_slot(node, first, node->slot_len);
    uint8_t end_slot = mn_find_slot_from(node, last, start_slot,
                                          node->slot_len);

    if (start_slot == end_slot) {
        struct maple_node *new_node = NULL;
        int ret = __mt_store_leaf(mt, node, start_slot, node_min, node_max,
                                  first, last, entry, &new_node);
        if (ret > 0)
            return 0; /* split+re-walk completed the store */
        if (ret != 0)
            return ret;
        struct maple_node *live = new_node ? new_node : node;
        __mt_coalesce_leaf(live, node_max);
        if (new_node)
            mn_replace_node(mt, node, new_node);
        if (orig_last > node_max)
            return mtree_store_range(mt, node_max + 1, orig_last, entry);
        __mt_propagate_gaps_from(mt, live);
        __mt_shrink_root(mt);
        return 0;
    }

    /* Multiple slots affected. */
    uint64_t start_slot_min = mn_slot_min(node, start_slot, node_min);
    uint64_t end_slot_max = mn_pivot(node, end_slot, node_max);

    void *left_entry = node->slot[start_slot];
    void *right_entry = node->slot[end_slot];

    int need_left   = (first > start_slot_min) ? 1 : 0;
    int need_right  = (last < end_slot_max) ? 1 : 0;
    int new_range_slots = need_left + 1 + need_right;
    int old_range_slots = end_slot - start_slot + 1;
    int delta = new_range_slots - old_range_slots;

    if (node->slot_len + delta > MAPLE_NODE_SLOTS) {
        int ret = __mt_split_node(mt, node, node_min, node_max);
        if (ret != 0)
            return ret;
        return mtree_store_range(mt, first, orig_last, entry);
    }

    void  *p_slot[3];
    uint64_t p_pivot[3];
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

    uint8_t old_len = node->slot_len;
    uint8_t new_len = (uint8_t)((int)old_len + delta);

    int trail_start_src = end_slot + 1;

    /*
     * RCU mode: copy-on-write.  Build the new layout on a fresh node.
     */
    struct maple_node *target = node;
    if (mt_in_rcu(mt)) {
        target = mn_copy_node(node);
        if (target == NULL)
            return -ENOMEM;
    }

    if (delta > 0) {
        for (int i = (int)old_len - 1; i >= trail_start_src; i--) {
            int d = i + delta;
            target->slot[d] = node->slot[i];
            if (i < (int)old_len - 1)
                target->pivot[d] = node->pivot[i];
        }
    } else if (delta < 0) {
        for (int i = trail_start_src; i < (int)old_len; i++) {
            int d = i + delta;
            target->slot[d] = node->slot[i];
            if (i < (int)old_len - 1)
                target->pivot[d] = node->pivot[i];
        }
    }

    mn_write_slots(target, start_slot, p_slot, p_pivot, (uint8_t)n_pieces,
                    new_len);

    target->slot_len = new_len;
    __mt_coalesce_leaf(target, node_max);

    if (mt_in_rcu(mt))
        mn_replace_node(mt, node, target);

    if (orig_last > node_max)
        return mtree_store_range(mt, node_max + 1, orig_last, entry);
    __mt_propagate_gaps_from(mt, target);
    __mt_shrink_root(mt);
    return 0;
}

/* ====================================================================== */
/*  Rebalancing — merging, redistribution, and tree shrinking              */
/* ====================================================================== */

/**
 * Minimum number of slots a non-root node should have after erasure.
 * Below this threshold, try to merge or redistribute with a sibling.
 */
#define MAPLE_NODE_MIN  (MAPLE_NODE_SLOTS / 4)  /* 2 when slots=10 */

#ifndef MT_REBAL_MAX_DEAD
#define MT_REBAL_MAX_DEAD  (MAPLE_CURSOR_MAX_DEPTH * 3)
#endif

#ifndef MT_REBAL_MAX_UNPUB
#define MT_REBAL_MAX_UNPUB  MAPLE_CURSOR_MAX_DEPTH
#endif

static inline bool mt_rebal_capacity_ok(int n_dead, int dead_need,
                                        int n_unpub, int unpub_need)
{
    return n_dead + dead_need <= MT_REBAL_MAX_DEAD &&
           n_unpub + unpub_need <= MT_REBAL_MAX_UNPUB;
}

/**
 * __mt_shrink_root - Collapse a single-child root to reduce tree height.
 *
 * If the root is an internal node with exactly one child, replace the root
 * with that child.  Repeat until the root has >1 child or is a leaf.
 */
static void __mt_shrink_root(struct maple_tree *mt)
{
    while (1) {
        void *root = mt_rcu_dereference(&mt->ma_root);
        if (root == NULL || !mt_is_node(root))
            return;

        struct maple_node *node = mt_to_node(root);
        if (mn_is_leaf(node)) {
            if (node->slot_len == 1 && node->slot[0] == NULL) {
                mn_drop_root_node(mt, node);
            }
            return;
        }
        if (node->slot_len != 1)
            return;

        struct maple_node *child = node->slot[0];
        if (child == NULL)
            return;

        /* Promote child to root. */
        mn_set_root(child);
        mt_rcu_assign_pointer(&mt->ma_root, mt_mk_root(child));
        mt_free_node_rcu(node);
    }
}

/**
 * __mt_get_sibling - Find a sibling node and its slot bounds.
 *
 * Prefers the left sibling; falls back to the right sibling.
 * Returns the sibling node, or NULL if none exists (i.e. node is root or
 * parent has only one child).
 *
 * @node:         The node that needs rebalancing.
 * @sib_out:      Returned sibling.
 * @sib_slot_out: Sibling's slot index within the parent.
 * @is_right_out: True if the returned sibling is to the right of @node.
 */
static struct maple_node *
__mt_get_sibling(struct maple_node *node,
                 uint8_t *sib_slot_out, bool *is_right_out)
{
    if (mn_is_root(node))
        return NULL;

    struct maple_node *parent = mn_get_parent(node);
    uint8_t pslot = mn_get_parent_slot(node);

    /* Prefer left sibling. */
    if (pslot > 0) {
        struct maple_node *left = parent->slot[pslot - 1];
        if (left != NULL) {
            *sib_slot_out = pslot - 1;
            *is_right_out = false;
            return left;
        }
    }
    /* Fall back to right sibling. */
    if (pslot + 1 < parent->slot_len) {
        struct maple_node *right = parent->slot[pslot + 1];
        if (right != NULL) {
            *sib_slot_out = pslot + 1;
            *is_right_out = true;
            return right;
        }
    }
    return NULL;
}

/**
 * __mt_merge_nodes - Merge two leaf siblings into one and remove the
 * empty node from their parent.
 *
 * @mt:      The tree.
 * @left:    Left sibling leaf (destination).
 * @right:   Right sibling leaf (source — will be freed).
 * @parent:  Parent of both nodes.
 * @left_slot: @left's slot index in @parent.
 *
 * Precondition: left->slot_len + right->slot_len <= MAPLE_NODE_SLOTS.
 */
static int __mt_merge_leaves(struct maple_tree *mt,
                             struct maple_node *left,
                             struct maple_node *right,
                             struct maple_node *parent,
                             uint8_t left_slot,
                             struct maple_node **merged_out,
                             struct maple_node **parent_out)
{
    *merged_out = NULL;
    *parent_out = NULL;

    uint8_t right_slot = left_slot + 1;
    uint64_t pmin, pmax;
    __find_node_bounds(mt, parent, &pmin, &pmax);
    uint64_t left_max = mn_pivot(parent, left_slot, pmax);
    uint64_t right_max = mn_pivot(parent, right_slot, pmax);

    /*
     * Build the merged leaf — in RCU mode on a new node, otherwise
     * modify left in place.
     */
    struct maple_node *merged = left;
    if (mt_in_rcu(mt)) {
        merged = mn_copy_node(left);
        if (merged == NULL)
            return -ENOMEM;
    }

    /* Append right's entries after left's. */
    uint8_t dst = merged->slot_len;
    if (dst > 0 && dst - 1 < MAPLE_NODE_PIVOTS)
        merged->pivot[dst - 1] = left_max;

    mn_copy_entries(merged, dst, right, 0, right->slot_len, false);
    merged->slot_len = dst + right->slot_len;

    __mt_coalesce_leaf(merged, right_max);

    /*
     * Update parent: pivot[left_slot] = right_max, remove right_slot.
     * In RCU mode, COW the parent.
     */
    struct maple_node *new_parent = parent;
    if (mt_in_rcu(mt)) {
        new_parent = mn_copy_node(parent);
        if (new_parent == NULL) {
            mt_free_node_now(merged);
            return -ENOMEM;
        }
    }

    new_parent->slot[left_slot] = merged;
    if (left_slot < MAPLE_NODE_PIVOTS)
        new_parent->pivot[left_slot] = right_max;

    /* Shift out the right_slot. */
    mn_shift_down(new_parent, right_slot);

    /* Fix up parent_slot encoding for all children of new parent. */
    mn_reparent_children(new_parent, 0, new_parent->slot_len);

    /*
     * Publish and cleanup are deferred to __mt_rebalance_node so that
     * cascading merges can be committed atomically in RCU mode.
     */

    *merged_out = merged;
    *parent_out = new_parent;
    return 0;
}

/**
 * __mt_merge_internal - Merge two internal siblings into one.
 *
 * Same idea as leaf merge, but also re-parents the children.
 */
static int __mt_merge_internal(struct maple_tree *mt,
                               struct maple_node *left,
                               struct maple_node *right,
                               struct maple_node *parent,
                               uint8_t left_slot,
                               struct maple_node **merged_out,
                               struct maple_node **parent_out)
{
    *merged_out = NULL;
    *parent_out = NULL;

    uint8_t right_slot = left_slot + 1;
    uint64_t pmin, pmax;
    __find_node_bounds(mt, parent, &pmin, &pmax);
    uint64_t left_max = mn_pivot(parent, left_slot, pmax);
    uint64_t right_max = mn_pivot(parent, right_slot, pmax);

    /*
     * Build the merged internal node — COW in RCU mode.
     */
    struct maple_node *merged = left;
    if (mt_in_rcu(mt)) {
        merged = mn_copy_node(left);
        if (merged == NULL)
            return -ENOMEM;
    }

    uint8_t dst = merged->slot_len;
    if (dst > 0 && dst - 1 < MAPLE_NODE_PIVOTS)
        merged->pivot[dst - 1] = left_max;

    mn_copy_entries(merged, dst, right, 0, right->slot_len, true);
    merged->slot_len = dst + right->slot_len;

    /* Re-parent all children to point at merged. */
    mn_reparent_children(merged, 0, merged->slot_len);

    /*
     * Update parent: replace left with merged, remove right.
     * COW the parent in RCU mode.
     */
    struct maple_node *new_parent = parent;
    if (mt_in_rcu(mt)) {
        new_parent = mn_copy_node(parent);
        if (new_parent == NULL) {
            mt_free_node_now(merged);
            return -ENOMEM;
        }
    }

    new_parent->slot[left_slot] = merged;
    if (left_slot < MAPLE_NODE_PIVOTS)
        new_parent->pivot[left_slot] = right_max;

    mn_shift_down(new_parent, right_slot);

    mn_reparent_children(new_parent, 0, new_parent->slot_len);

    /*
     * Publish and cleanup are deferred to __mt_rebalance_node so that
     * cascading merges can be committed atomically in RCU mode.
     */

    *merged_out = merged;
    *parent_out = new_parent;
    return 0;
}

/**
 * __mt_redistribute_leaves - Balance entries between two leaf siblings.
 *
 * Moves entries from the heavier sibling to the lighter one so that both
 * have approximately equal occupancy.
 *
 * @mt:      The tree.
 * @node:    The underfull node.
 * @sibling: Its sibling.
 * @parent:  Parent of both.
 * @node_slot: @node's slot index in @parent.
 * @sib_slot:  @sibling's slot index in @parent.
 */
static int __mt_redistribute_leaves(struct maple_tree *mt,
                                    struct maple_node *node,
                                    struct maple_node *sibling,
                                    struct maple_node *parent,
                                    uint8_t node_slot,
                                    uint8_t sib_slot,
                                    struct maple_node **node_out,
                                    struct maple_node **sib_out,
                                    struct maple_node **parent_out)
{
    *node_out = NULL;
    *sib_out = NULL;
    *parent_out = NULL;

    /* Determine left/right ordering. */
    struct maple_node *left, *right;
    uint8_t left_slot;
    if (node_slot < sib_slot) {
        left = node;
        right = sibling;
        left_slot = node_slot;
    } else {
        left = sibling;
        right = node;
        left_slot = sib_slot;
    }

    /*
     * Collect all entries from both nodes into a temporary buffer,
     * then split them evenly.  This is simpler and less error-prone
     * than shifting entries one-by-one.
     */
    uint8_t total = left->slot_len + right->slot_len;
    void    *tmp_slot[MAPLE_NODE_SLOTS * 2];
    uint64_t tmp_pivot[MAPLE_NODE_SLOTS * 2];

    uint64_t pmin, pmax;
    __find_node_bounds(mt, parent, &pmin, &pmax);
    uint64_t left_max = mn_pivot(parent, left_slot, pmax);

    /* Copy left entries. */
    uint8_t ti = 0;
    for (uint8_t i = 0; i < left->slot_len; i++, ti++) {
        tmp_slot[ti] = left->slot[i];
        if (i < left->slot_len - 1)
            tmp_pivot[ti] = left->pivot[i];
        else
            tmp_pivot[ti] = left_max;
    }
    /* Copy right entries. */
    uint64_t right_max = mn_pivot(parent, left_slot + 1, pmax);
    for (uint8_t i = 0; i < right->slot_len; i++, ti++) {
        tmp_slot[ti] = right->slot[i];
        if (i < right->slot_len - 1)
            tmp_pivot[ti] = right->pivot[i];
        else
            tmp_pivot[ti] = right_max;
    }

    /* Split: left gets first half, right gets second half. */
    uint8_t left_new = total >> 1;
    uint8_t right_new = total - left_new;

    /*
     * RCU mode: allocate new left and right nodes, fill them,
     * then COW the parent to swap both slots atomically.
     */
    struct maple_node *new_left = left;
    struct maple_node *new_right = right;
    if (mt_in_rcu(mt)) {
        new_left = mt_alloc_node();
        new_right = mt_alloc_node();
        if (new_left == NULL || new_right == NULL) {
            if (new_left) mt_free_node_now(new_left);
            if (new_right) mt_free_node_now(new_right);
            return -ENOMEM;
        }
        mn_set_type(new_left, mn_get_type(left));
        mn_set_type(new_right, mn_get_type(right));
    }

    /* Refill left. */
    mn_write_slots(new_left, 0, tmp_slot, tmp_pivot, left_new, left_new);
    mn_truncate(new_left, left_new);

    /* Refill right. */
    mn_write_slots(new_right, 0, &tmp_slot[left_new], &tmp_pivot[left_new],
                   right_new, right_new);
    mn_truncate(new_right, right_new);

    /*
     * Update parent pivot and slot pointers.
     * In RCU mode, COW the parent.
     */
    struct maple_node *new_parent = parent;
    if (mt_in_rcu(mt)) {
        new_parent = mn_copy_node(parent);
        if (new_parent == NULL) {
            mt_free_node_now(new_left);
            mt_free_node_now(new_right);
            return -ENOMEM;
        }
    }

    new_parent->slot[left_slot] = new_left;
    new_parent->slot[left_slot + 1] = new_right;
    new_parent->pivot[left_slot] = tmp_pivot[left_new - 1];
    mn_reparent_children(new_parent, 0, new_parent->slot_len);

    /*
     * Publish and cleanup are deferred to __mt_rebalance_node so that
     * the commit is atomic in RCU mode.
     */

    /* Return the new nodes for the caller's gap propagation. */
    if (node_slot < sib_slot) {
        *node_out = new_left;
        *sib_out = new_right;
    } else {
        *node_out = new_right;
        *sib_out = new_left;
    }
    *parent_out = new_parent;
    return 0;
}

static int __mt_redistribute_internal(struct maple_tree *mt,
                                      struct maple_node *node,
                                      struct maple_node *sibling,
                                      struct maple_node *parent,
                                      uint8_t node_slot,
                                      uint8_t sib_slot,
                                      struct maple_node **node_out,
                                      struct maple_node **sib_out,
                                      struct maple_node **parent_out)
{
    *node_out = NULL;
    *sib_out = NULL;
    *parent_out = NULL;

    struct maple_node *left, *right;
    uint8_t left_slot;
    if (node_slot < sib_slot) {
        left = node;
        right = sibling;
        left_slot = node_slot;
    } else {
        left = sibling;
        right = node;
        left_slot = sib_slot;
    }

    uint8_t total = left->slot_len + right->slot_len;
    void *tmp_slot[MAPLE_NODE_SLOTS * 2];
    uint64_t tmp_pivot[MAPLE_NODE_SLOTS * 2];

    uint64_t pmin, pmax;
    __find_node_bounds(mt, parent, &pmin, &pmax);
    uint64_t left_max = mn_pivot(parent, left_slot, pmax);

    uint8_t ti = 0;
    for (uint8_t i = 0; i < left->slot_len; i++, ti++) {
        tmp_slot[ti] = left->slot[i];
        if (i < left->slot_len - 1)
            tmp_pivot[ti] = left->pivot[i];
        else
            tmp_pivot[ti] = left_max;
    }

    uint64_t right_max = mn_pivot(parent, left_slot + 1, pmax);
    for (uint8_t i = 0; i < right->slot_len; i++, ti++) {
        tmp_slot[ti] = right->slot[i];
        if (i < right->slot_len - 1)
            tmp_pivot[ti] = right->pivot[i];
        else
            tmp_pivot[ti] = right_max;
    }

    uint8_t left_new = total >> 1;
    uint8_t right_new = total - left_new;

    struct maple_node *new_left = left;
    struct maple_node *new_right = right;
    if (mt_in_rcu(mt)) {
        new_left = mt_alloc_node();
        new_right = mt_alloc_node();
        if (new_left == NULL || new_right == NULL) {
            if (new_left) mt_free_node_now(new_left);
            if (new_right) mt_free_node_now(new_right);
            return -ENOMEM;
        }
        mn_set_type(new_left, mn_get_type(left));
        mn_set_type(new_right, mn_get_type(right));
    }

    mn_write_slots(new_left, 0, tmp_slot, tmp_pivot, left_new, left_new);
    mn_truncate(new_left, left_new);
    mn_reparent_children(new_left, 0, left_new);

    mn_write_slots(new_right, 0, &tmp_slot[left_new], &tmp_pivot[left_new],
                   right_new, right_new);
    mn_truncate(new_right, right_new);
    mn_reparent_children(new_right, 0, right_new);

    struct maple_node *new_parent = parent;
    if (mt_in_rcu(mt)) {
        new_parent = mn_copy_node(parent);
        if (new_parent == NULL) {
            mt_free_node_now(new_left);
            mt_free_node_now(new_right);
            return -ENOMEM;
        }
    }

    new_parent->slot[left_slot] = new_left;
    new_parent->slot[left_slot + 1] = new_right;
    new_parent->pivot[left_slot] = tmp_pivot[left_new - 1];
    mn_reparent_children(new_parent, 0, new_parent->slot_len);

    if (node_slot < sib_slot) {
        *node_out = new_left;
        *sib_out = new_right;
    } else {
        *node_out = new_right;
        *sib_out = new_left;
    }
    *parent_out = new_parent;
    return 0;
}

/**
 * __mt_rebalance_node - Rebalance a node after erasure (batched RCU commit).
 *
 * Called when a node drops below MAPLE_NODE_MIN entries.
 * Tries to merge with a sibling (if combined <= MAPLE_NODE_SLOTS),
 * falling back to leaf redistribution otherwise.
 *
 * In RCU mode, cascading merges at successive tree levels are collected
 * without publishing.  A single mn_replace_node() at the topmost affected
 * ancestor then atomically commits the entire rebalance to readers.
 * All old live nodes are RCU-freed after the commit; intermediate new nodes
 * that were never published are freed immediately.
 *
 * In non-RCU mode, in-place modifications need no batching; only the
 * removed right node is freed at each level.
 */
static void __mt_rebalance_node(struct maple_tree *mt,
                                struct maple_node *node)
{
    /* Never rebalance the root itself (shrinking handles that). */
    if (mn_is_root(node))
        return;

    /* Only rebalance if underfull. */
    if (node->slot_len >= MAPLE_NODE_MIN)
        return;

    /*
     * Batch state (RCU mode only):
     *   dead[]  – old live nodes to mark dead + RCU-free after commit.
     *   unpub[] – intermediate new nodes never published; immediate-free.
    *   top_old/top_new – topmost (old, new) parent pair for the single
    *                     mn_replace_node() publish.
    *   first_merged – bottommost merged node for gap propagation.
    *   redistributed_* – nodes whose gap metadata must be refreshed after
    *                     a redistribution stops the upward walk.
     */
    struct maple_node *dead[MT_REBAL_MAX_DEAD];
    int n_dead = 0;
    struct maple_node *unpub[MT_REBAL_MAX_UNPUB];
    int n_unpub = 0;
    struct maple_node *top_old = NULL;
    struct maple_node *top_new = NULL;
    struct maple_node *first_merged = NULL;

    /* Track whether cur was newly allocated (never published). */
    bool cur_is_new = false;

    /* For redistribution at the stopping level. */
    bool did_redistribute = false;
    struct maple_node *redistributed_node = NULL;
    struct maple_node *redistributed_sibling = NULL;

    struct maple_node *cur = node;

    while (!mn_is_root(cur) && cur->slot_len < MAPLE_NODE_MIN) {
        uint8_t sib_slot;
        bool is_right;
        struct maple_node *sibling =
            __mt_get_sibling(cur, &sib_slot, &is_right);
        if (sibling == NULL)
            break;

        struct maple_node *parent = mn_get_parent(cur);
        uint8_t cur_slot = mn_get_parent_slot(cur);
        uint8_t combined = cur->slot_len + sibling->slot_len;

        if (combined <= MAPLE_NODE_SLOTS) {
            /* --- Merge --- */
            if (mt_in_rcu(mt)) {
                int dead_need = 1 + (cur_is_new ? 0 : 1) + (top_old ? 1 : 0);
                int unpub_need = cur_is_new ? 1 : 0;

                if (!mt_rebal_capacity_ok(n_dead, dead_need,
                                          n_unpub, unpub_need))
                    break;
            }

            struct maple_node *left, *right;
            uint8_t left_slot;
            if (!is_right) {
                left = sibling;
                right = cur;
                left_slot = sib_slot;
            } else {
                left = cur;
                right = sibling;
                left_slot = cur_slot;
            }

            struct maple_node *merged, *new_parent;
            int ret;
            if (mn_is_leaf(left))
                ret = __mt_merge_leaves(mt, left, right, parent, left_slot,
                                        &merged, &new_parent);
            else
                ret = __mt_merge_internal(mt, left, right, parent, left_slot,
                                          &merged, &new_parent);

            if (ret != 0)
                break;

            if (!first_merged)
                first_merged = merged;

            if (mt_in_rcu(mt)) {
                /*
                 * sibling is always live in the tree.
                 * cur is live at level 0, or never-published at level > 0.
                 */
                dead[n_dead++] = sibling;
                if (cur_is_new)
                    unpub[n_unpub++] = cur;
                else
                    dead[n_dead++] = cur;

                /*
                 * The previous top_old (if any) is a live ancestor that
                 * becomes unreachable once the topmost publish replaces
                 * its successor.  Defer-free it.
                 */
                if (top_old)
                    dead[n_dead++] = top_old;

                top_old = parent;
                top_new = new_parent;
            } else {
                /* Non-RCU: in-place; just free the removed right node. */
                mn_mark_dead(right);
                mt_free_node_now(right);
            }

            /* Check whether the parent is now underfull. */
            cur = new_parent;
            cur_is_new = true;
        } else {
            /* --- Redistribute --- */
            if (mt_in_rcu(mt) &&
                !mt_rebal_capacity_ok(n_dead, 2 + (top_old ? 1 : 0),
                                      n_unpub, 0)) {
                break;
            }

                        struct maple_node *new_parent;
                        struct maple_node *updated_node, *updated_sibling;
            int ret;
            if (mn_is_leaf(cur)) {
                ret = __mt_redistribute_leaves(mt, cur, sibling, parent,
                                               cur_slot, sib_slot,
                                                                                             &updated_node,
                                                                                             &updated_sibling,
                                                                                             &new_parent);
            } else {
                ret = __mt_redistribute_internal(mt, cur, sibling, parent,
                                                 cur_slot, sib_slot,
                                                                                                 &updated_node,
                                                                                                 &updated_sibling,
                                                                                                 &new_parent);
            }

            if (ret != 0)
                break;

            if (mt_in_rcu(mt)) {
                dead[n_dead++] = cur;
                dead[n_dead++] = sibling;
                if (top_old)
                    dead[n_dead++] = top_old;
                top_old = parent;
                top_new = new_parent;
            }

            did_redistribute = true;
            redistributed_node = updated_node;
            redistributed_sibling = updated_sibling;
            break;
        }
    }

    /* --- Commit --- */
    if (mt_in_rcu(mt) && top_old && top_new) {
        /*
         * Single atomic publish of the topmost new parent.
         * mn_replace_node() also marks top_old dead and RCU-frees it.
         */
        mn_replace_node(mt, top_old, top_new);

        /* RCU-free all other old live nodes that are now unreachable. */
        for (int i = 0; i < n_dead; i++) {
            mn_retire_node_rcu(dead[i]);
        }

        /* Immediately free intermediate nodes never visible to readers. */
        for (int i = 0; i < n_unpub; i++)
            mt_free_node_now(unpub[i]);
    }

    /* --- Gap propagation (after commit so parent chain is live) --- */
    if (did_redistribute) {
        __mt_propagate_gaps_from(mt, redistributed_node);
        __mt_propagate_gaps_from(mt, redistributed_sibling);
    } else if (first_merged) {
        __mt_propagate_gaps_from(mt, first_merged);
    }

    __mt_shrink_root(mt);
}

/* ====================================================================== */
/*  Erase                                                                  */
/* ====================================================================== */

/**
 * mtree_erase - Remove the entry at @index, return it.
 *
 * RCU sequence:
 *  1. Walk to the target leaf.
 *  2. mn_copy_node() — COW the leaf.
 *  3. Null out the slot on the copy, coalesce adjacent NULL ranges.
 *  4. mn_replace_node() — single rcu_assign_pointer() into the parent
 *     slot (or ma_root), mark old node dead, RCU-defer-free it.
 *  5. __mt_propagate_gaps_from() — update gap[] values upward.
 *  6. If the leaf is now underfull, __mt_rebalance_node() runs:
 *     - Iteratively merge/redistribute with siblings bottom-up.
 *     - In RCU mode, all per-level COW operations are batched;
 *       a single mn_replace_node() at the topmost affected ancestor
 *       atomically publishes the entire rebalance.
 *     - Old live nodes: mark dead + RCU-defer-free.
 *     - Intermediate unpublished nodes: immediate free.
 *  7. __mt_shrink_root() — collapse single-child root if needed.
 *
 * Caller must hold the tree lock.
 */
void *mtree_erase(struct maple_tree *mt, uint64_t index)
{
    void *root = mt_rcu_dereference(&mt->ma_root);
    if (root == NULL)
        return NULL;

    if (!mt_is_node(root)) {
        mt_rcu_assign_pointer(&mt->ma_root, NULL);
        return root;
    }

    struct maple_node *node = mt_to_node(root);
    uint64_t node_min = 0;
    uint64_t node_max = MAPLE_MAX;

    while (!mn_is_leaf(node)) {
        uint8_t slot = mn_find_slot(node, index, node->slot_len);
        node_min = mn_slot_min(node, slot, node_min);
        node_max = mn_pivot(node, slot, node_max);

        struct maple_node *child = node->slot[slot];
        if (child == NULL)
            return NULL;
        node = child;
    }

    uint8_t slot = mn_find_slot(node, index, node->slot_len);

    void *old = node->slot[slot];

    /*
     * RCU mode: copy-on-write the leaf, null the slot on the copy,
     * coalesce, then publish.  Non-RCU: mutate in place.
     */
    struct maple_node *target = node;
    if (mt_in_rcu(mt)) {
        target = mn_copy_node(node);
        if (target == NULL)
            return NULL;  /* allocation failure — tree unchanged */
    }

    target->slot[slot] = NULL;
    __mt_coalesce_leaf(target, node_max);

    if (mt_in_rcu(mt))
        mn_replace_node(mt, node, target);

    __mt_propagate_gaps_from(mt, target);

    /* Rebalance if the leaf is underfull and not the root. */
    if (!mn_is_root(target) && target->slot_len < MAPLE_NODE_MIN)
        __mt_rebalance_node(mt, target);

    /* Even without rebalance, the root might have become single-child. */
    __mt_shrink_root(mt);

    return old;
}

/* ====================================================================== */
/*  Partial erase (single-index hole punch)                                */
/* ====================================================================== */

/**
 * mtree_erase_index - Punch a NULL hole at @index, return the old entry.
 *
 * RCU sequence: delegates to mtree_store_range(mt, index, index, NULL).
 * See mtree_store_range() for the full COW → publish → free sequence.
 *
 * Caller must hold the tree lock.
 */
void *mtree_erase_index(struct maple_tree *mt, uint64_t index)
{
    void *old = mtree_load(mt, index);
    if (old != NULL)
        mtree_store_range(mt, index, index, NULL);
    return old;
}

/* ====================================================================== */
/*  Destroy                                                                */
/* ====================================================================== */

static void __mt_destroy_walk(struct maple_node *node)
{
    if (node == NULL)
        return;

    if (!mn_is_leaf(node)) {
        for (uint8_t i = 0; i < node->slot_len; i++) {
            struct maple_node *child = node->slot[i];
            if (child != NULL)
                __mt_destroy_walk(child);
        }
    }
    mt_free_node_now(node);
}

/**
 * mtree_destroy - Free all nodes in the tree.
 *
 * RCU sequence: none — all nodes are freed immediately via
 * mt_free_node_now().  The caller must ensure no concurrent readers
 * (i.e., an RCU grace period has elapsed or exclusive access is held).
 *
 * Caller must hold the tree lock (or have exclusive access).
 */
void mtree_destroy(struct maple_tree *mt)
{
    void *root = mt_rcu_dereference(&mt->ma_root);
    if (root == NULL)
        return;

    if (mt_is_node(root))
        __mt_destroy_walk(mt_to_node(root));

    mt_rcu_assign_pointer(&mt->ma_root, NULL);
}

/* ====================================================================== */
/*  Debug dump                                                             */
/* ====================================================================== */

static void __mt_dump_node(struct maple_node *node, uint64_t node_min,
                            uint64_t node_max, int depth)
{
    if (node == NULL) {
        printf("%*s(null node)\n", depth * 2, "");
        return;
    }
    printf("%*snode=%p %s len=%d range=[%" PRIx64 ", %" PRIx64 "]"
           " parent=%" PRIx64 " pslot=%d\n",
           depth * 2, "", (void *)node,
           mn_is_leaf(node) ? "LEAF" : "INTERNAL",
           node->slot_len, node_min, node_max,
           node->parent, mn_get_parent_slot(node));
    for (uint8_t i = 0; i < node->slot_len; i++) {
        uint64_t smin = mn_slot_min(node, i, node_min);
        uint64_t smax = mn_pivot(node, i, node_max);
        if (mn_is_leaf(node)) {
            printf("%*s  [%d] [%" PRIx64 ", %" PRIx64 "] -> %p\n",
                   depth * 2, "", i, smin, smax, node->slot[i]);
        } else {
            printf("%*s  [%d] pivot_upper=%" PRIx64 " -> child=%p\n",
                   depth * 2, "", i, smax, node->slot[i]);
            if (node->slot[i] != NULL)
                __mt_dump_node((struct maple_node *)node->slot[i],
                                smin, smax, depth + 1);
        }
    }
    printf("%*s  raw pivots:", depth * 2, "");
    for (uint8_t i = 0; i < node->slot_len && i < MAPLE_NODE_PIVOTS; i++)
        printf(" [%d]=%" PRIx64, i, node->pivot[i]);
    printf("\n");
}

/**
 * mt_dump_tree - Print the tree structure to stdout (debug).
 *
 * RCU sequence: read-only walk via mt_rcu_dereference().
 * Caller should hold the tree lock or mt_rcu_lock() for consistency.
 */
void mt_dump_tree(struct maple_tree *mt)
{
    void *root = mt_rcu_dereference(&mt->ma_root);
    printf("=== MAPLE TREE DUMP mt=%p root=%p ===\n", (void *)mt, root);
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

/* ====================================================================== */
/*  Cursor next / prev                                                     */
/* ====================================================================== */

static void *mas_adjacent_slot(struct ma_state *mas, bool reverse)
{
    struct maple_node *node = mas->node;
    if (node == NULL)
        return NULL;
    if (mas_node_dead(mas, node)) {
        mas_request_retry(mas);
        return NULL;
    }

    uint8_t slot;
    if (reverse) {
        if (mas->offset == 0)
            return NULL;
        slot = mas->offset - 1;
    } else {
        slot = mas->offset + 1;
        if (slot >= node->slot_len)
            return NULL;
    }

    if (reverse == false && slot >= node->slot_len)
        return NULL;

    struct maple_path_frame *frame = mas_frame(mas, mas->depth - 1);
    frame->slot = slot;
    mas_apply_frame(mas, mas->depth - 1);
    mas->index = mas->min;
    return mt_rcu_dereference(&node->slot[slot]);
}

/**
 * mas_seek_adjacent_entry - Reposition the cursor into a neighbouring subtree.
 *
 * Ascends cached path frames until it finds the next branch in the chosen
 * direction that can still satisfy @limit, then descends that subtree to the
 * first live entry. On a bounded miss, invalidates the cursor.
 */
static void *mas_seek_adjacent_entry(struct ma_state *mas, uint64_t limit,
                                     bool reverse)
{
    if (mas->node == NULL || mas->depth == 0)
        return NULL;

    for (int level = (int)mas->depth - 2; level >= 0; level--) {
        struct maple_path_frame *frame = mas_frame(mas, (uint8_t)level);
        struct maple_node *node = frame->node;

        if (mas_node_dead(mas, node)) {
            mas_request_retry(mas);
            return NULL;
        }

        int start = reverse ? (int)frame->slot - 1 : (int)frame->slot + 1;
        int end = reverse ? -1 : (int)node->slot_len;
        int step = reverse ? -1 : 1;

        for (int i = start; i != end; i += step) {
            uint8_t slot = (uint8_t)i;
            uint64_t child_min = mn_slot_min(node, slot, frame->node_min);
            uint64_t child_max = mn_pivot(node, slot, frame->node_max);
            if ((!reverse && child_min > limit) ||
                (reverse && child_max < limit)) {
                mas_invalidate(mas);
                return NULL;
            }

            struct maple_node *child =
                (struct maple_node *)mt_rcu_dereference(&node->slot[slot]);
            if (child == NULL)
                continue;

            frame->slot = slot;
            void *entry = mas_descend_subtree(mas, child, child_min,
                                             child_max, (uint8_t)level + 1,
                                             reverse);
            if (entry != NULL || mas_retry_requested(mas))
                return entry;
        }
    }

    mas_invalidate(mas);
    return NULL;
}

static void *__mas_next_slot(struct ma_state *mas)
{
    return mas_adjacent_slot(mas, false);
}

static void *__mas_next_node(struct ma_state *mas, uint64_t limit)
{
    return mas_seek_adjacent_entry(mas, limit, false);
}

/**
 * mas_next - Advance the cursor to the next non-NULL entry <= @max.
 *
 * RCU sequence (read-only):
 *  1. Try the next slot in the current leaf.
 *  2. If exhausted, ascend via cached path frames and descend into the
 *     next subtree, using mt_rcu_dereference() at each slot read.
 *
 * Caller must hold mt_rcu_lock() or the tree lock.
 */
void *mas_next(struct ma_state *mas, uint64_t max)
{
    while (1) {
        uint64_t retry_index = mas_next_retry_index(mas);
        void *entry;

        if (mas->node == NULL || mas_node_dead(mas, mas->node)) {
            switch (mas_rewalk_to_index(mas, retry_index, max, false,
                                        &entry)) {
            case mas_rewalk_stop:
                return NULL;
            case mas_rewalk_found:
                return entry;
            case mas_rewalk_continue:
                break;
            }
        }

        entry = __mas_next_slot(mas);
        if (entry != NULL)
            return mas_validate_result(mas, entry, max, false, false);

        if (mas_retry_requested(mas)) {
            mas_restart_retry(mas, retry_index);
            continue;
        }

        if (mas->node != NULL && mas->offset + 1 < mas->node->slot_len)
            continue;

        entry = __mas_next_node(mas, max);
        if (entry != NULL)
            return mas_validate_result(mas, entry, max, false, false);
        if (mas_retry_requested(mas)) {
            mas_restart_retry(mas, retry_index);
            continue;
        }
        if (mas->node == NULL)
            return NULL;
    }
}

static void *__mas_prev_slot(struct ma_state *mas)
{
    return mas_adjacent_slot(mas, true);
}

static void *__mas_prev_node(struct ma_state *mas, uint64_t limit)
{
    return mas_seek_adjacent_entry(mas, limit, true);
}

/**
 * mas_prev - Move the cursor to the previous non-NULL entry >= @min.
 *
 * RCU sequence (read-only):
 *  1. Try the previous slot in the current leaf.
 *  2. If exhausted, ascend via cached path frames and descend into the
 *     previous subtree, using mt_rcu_dereference() at each slot read.
 *
 * Caller must hold mt_rcu_lock() or the tree lock.
 */
void *mas_prev(struct ma_state *mas, uint64_t min)
{
    while (1) {
        uint64_t retry_index = mas_prev_retry_index(mas);
        void *entry;

        if (mas->node == NULL || mas_node_dead(mas, mas->node)) {
            switch (mas_rewalk_to_index(mas, retry_index, min, true,
                                        &entry)) {
            case mas_rewalk_stop:
                return NULL;
            case mas_rewalk_found:
                return entry;
            case mas_rewalk_continue:
                break;
            }
        }

        /* Try stepping backward within the current leaf node. */
        while (mas->node != NULL && mas->offset > 0) {
            entry = __mas_prev_slot(mas);
            if (entry != NULL)
                return mas_validate_result(mas, entry, min, true, false);
            if (mas_retry_requested(mas)) {
                mas_restart_retry(mas, retry_index);
                break;
            }
            /* Slot was NULL — keep trying earlier slots. */
        }

        if (mas_retry_requested(mas))
            continue;

        /* At start of node or no node — ascend to previous node. */
        entry = __mas_prev_node(mas, min);
        if (mas_retry_requested(mas)) {
            mas_restart_retry(mas, retry_index);
            continue;
        }
        if (mas->node == NULL)
            return NULL;
        if (entry != NULL)
            return mas_validate_result(mas, entry, min, true, false);
        /* __mas_prev_node positioned us on a new node but the entry
         * was NULL; loop back to try slots in this new node. */
    }
}

/* ====================================================================== */
/*  mas_find                                                               */
/* ====================================================================== */

/**
 * mas_find - Find the next non-NULL entry at or after mas->index, up to @max.
 *
 * RCU sequence (read-only):
 *  1. If not yet positioned, mas_walk() to the current index.
 *  2. Advance via mas_next() using mt_rcu_dereference() for slot reads.
 *
 * Caller must hold mt_rcu_lock() or the tree lock.
 */
void *mas_find(struct ma_state *mas, uint64_t max)
{
    if (mas->index > max)
        return NULL;

    while (1) {
        void *entry;

        if (mas->node == NULL || mas_node_dead(mas, mas->node)) {
            switch (mas_rewalk_to_index(mas, mas->index, max, false,
                                        &entry)) {
            case mas_rewalk_stop:
                return NULL;
            case mas_rewalk_found:
                mas_advance_index(mas);
                return entry;
            case mas_rewalk_continue:
                break;
            }
        }

        entry = __mas_next_slot(mas);
        if (entry != NULL)
            return mas_validate_result(mas, entry, max, false, true);

        if (mas_retry_requested(mas)) {
            mas_restart_retry(mas, mas->index);
            continue;
        }

        if (mas->node != NULL && mas->offset + 1 < mas->node->slot_len)
            continue;

        entry = __mas_next_node(mas, max);
        if (entry != NULL)
            return mas_validate_result(mas, entry, max, false, true);
        if (mas_retry_requested(mas)) {
            mas_restart_retry(mas, mas->index);
            continue;
        }
        if (mas->node == NULL)
            return NULL;
    }
}

/* ====================================================================== */
/*  mas_store / mas_erase                                                  */
/* ====================================================================== */

/**
 * mas_store - Store @entry at [mas->index, mas->last].
 *
 * RCU sequence: delegates to mtree_store_range().
 * See mtree_store_range() for the full COW → publish → free sequence.
 *
 * Caller must hold the tree lock.
 */
int mas_store(struct ma_state *mas, void *entry)
{
    return mtree_store_range(mas->tree, mas->index, mas->last, entry);
}

/**
 * mas_erase - Erase the entry at mas->index.
 *
 * RCU sequence: delegates to mtree_erase().
 * See mtree_erase() for the full COW → rebalance → publish → free sequence.
 *
 * Caller must hold the tree lock.
 */
void *mas_erase(struct ma_state *mas)
{
    return mtree_erase(mas->tree, mas->index);
}

/* ====================================================================== */
/*  Gap search                                                             */
/* ====================================================================== */

/**
 * mas_empty_area - Find a forward gap of at least @size in [@min, @max].
 *
 * RCU sequence (read-only):
 *  1. Walk the gap[] metadata top-down via mt_rcu_dereference().
 *  2. At each internal node, skip children whose gap[] < @size.
 *  3. At the leaf, scan for a contiguous NULL range >= @size.
 *  4. Set mas->index/last to the found gap.
 *
 * Caller must hold mt_rcu_lock() or the tree lock.
 */
int mas_empty_area(struct ma_state *mas, uint64_t min, uint64_t max,
                    uint64_t size)
{
    if (size == 0 || min > max)
        return -EBUSY;

    if ((size - 1) > (max - min))
        return -EBUSY;

    void *root = mt_rcu_dereference(&mas->tree->ma_root);
    if (root == NULL) {
        mas_set_gap(mas, min, size);
        return 0;
    }

    if (!mt_is_node(root)) {
        if (min == 0 && max == MAPLE_MAX)
            return -EBUSY;
        mas_set_gap(mas, min, size);
        return 0;
    }

    uint64_t gap_start;
    if (__mt_find_gap_fwd(mt_to_node(root), 0, MAPLE_MAX, size,
                           min, max, &gap_start) != 0)
        return -EBUSY;

    mas_set_gap(mas, gap_start, size);
    return 0;
}

/**
 * mas_empty_area_rev - Find a reverse gap of at least @size in [@min, @max].
 *
 * RCU sequence (read-only):
 *  1. Walk the gap[] metadata top-down (right-to-left) via
 *     mt_rcu_dereference().
 *  2. At each internal node, skip children whose gap[] < @size.
 *  3. At the leaf, scan backwards for a contiguous NULL range >= @size.
 *  4. Set mas->index/last to the found gap (highest address).
 *
 * Caller must hold mt_rcu_lock() or the tree lock.
 */
int mas_empty_area_rev(struct ma_state *mas, uint64_t min, uint64_t max,
                        uint64_t size)
{
    if (size == 0 || min > max)
        return -EBUSY;

    if ((size - 1) > (max - min))
        return -EBUSY;

    void *root = mt_rcu_dereference(&mas->tree->ma_root);
    if (root == NULL) {
        mas_set_gap(mas, max - size + 1, size);
        return 0;
    }

    if (!mt_is_node(root)) {
        if (min == 0 && max == MAPLE_MAX)
            return -EBUSY;
        mas_set_gap(mas, max - size + 1, size);
        return 0;
    }

    uint64_t gap_start;
    if (__mt_find_gap_rev(mt_to_node(root), 0, MAPLE_MAX, size,
                           min, max, &gap_start) != 0)
        return -EBUSY;

    mas_set_gap(mas, gap_start, size);
    return 0;
}

/* ====================================================================== */
/*  RCU read-side helpers                                                  */
/* ====================================================================== */

/**
 * mt_find - Find the next non-NULL entry at or after *@index, up to @max.
 *
 * RCU sequence (read-only):
 *  1. mt_rcu_lock()   — enter RCU read-side critical section.
 *  2. mas_find() — walk/advance via mt_rcu_dereference().
 *  3. mt_rcu_unlock() — leave critical section.
 *  4. Advance *@index past the found entry for iteration.
 *
 * Write-side locking: none required.
 */
void *mt_find(struct maple_tree *mt, uint64_t *index, uint64_t max)
{
    if (*index > max)
        return NULL;

    mt_rcu_lock();
    MA_STATE(mas, mt, *index, *index);
    void *entry = mas_find(&mas, max);
    if (entry != NULL) {
        if (mas.max == MAPLE_MAX)
            *index = MAPLE_MAX;
        else
            *index = mas.max + 1;
    }
    mt_rcu_unlock();
    return entry;
}

/**
 * mt_next - Return the next non-NULL entry after @index, up to @max.
 *
 * RCU sequence (read-only):
 *  1. mt_rcu_lock()   — enter RCU read-side critical section.
 *  2. mas_walk() + mas_find() via mt_rcu_dereference().
 *  3. mt_rcu_unlock() — leave critical section.
 *
 * Write-side locking: none required.
 */
void *mt_next(struct maple_tree *mt, uint64_t index, uint64_t max)
{
    if (index >= max)
        return NULL;

    mt_rcu_lock();
    MA_STATE(mas, mt, index + 1, index + 1);
    void *entry = mas_walk(&mas);
    if (entry != NULL) {
        mt_rcu_unlock();
        return entry;
    }
    entry = mas_find(&mas, max);
    mt_rcu_unlock();
    return entry;
}

/**
 * mt_prev - Return the previous non-NULL entry before @index, down to @min.
 *
 * RCU sequence (read-only):
 *  1. mt_rcu_lock()   — enter RCU read-side critical section.
 *  2. mas_walk() + mas_prev() via mt_rcu_dereference().
 *  3. mt_rcu_unlock() — leave critical section.
 *
 * Write-side locking: none required.
 */
void *mt_prev(struct maple_tree *mt, uint64_t index, uint64_t min)
{
    if (index <= min)
        return NULL;

    mt_rcu_lock();
    MA_STATE(mas, mt, index - 1, index - 1);
    void *entry = mas_walk(&mas);
    if (entry != NULL) {
        mt_rcu_unlock();
        return entry;
    }
    entry = mas_prev(&mas, min);
    mt_rcu_unlock();
    return entry;
}
