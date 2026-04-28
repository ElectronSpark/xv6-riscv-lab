/**
 * @file xarray.c
 * @brief XArray implementation — Linux-style radix tree (standalone).
 *
 * A radix tree mapping unsigned long indices to void* entries.
 * Each internal node has 64 slots (6-bit fan-out).  The tree grows
 * upward when indices exceed the current depth and shrinks when the
 * top node collapses to a single entry.
 *
 * When XA_CONFIG_LOCK is enabled, write operations hold xa_lock.
 * When XA_CONFIG_RCU is enabled, read operations are lock-free.
 *
 * Three independent mark bitmaps per node allow efficient iteration
 * over tagged entries.
 */

#include "xarray.h"

#include "types.h"
#include "defs.h"
#include "printf.h"
#include "string.h"
#include "errno.h"
#include <mm/slab.h>
#include "lock/rcu.h"

/* ====================================================================== */
/*  Kernel integration — slab cache, RCU trampoline                        */
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

void *xa_alloc_fn(size_t size)
{
    (void)size; /* fixed-size: struct xa_node */
    void *p = slab_alloc(&__xa_node_cache);
    if (p)
        memset(p, 0, sizeof(struct xa_node));
    return p;
}

void xa_free_fn(void *ptr)
{
    if (ptr)
        slab_free(ptr);
}

void xa_rcu_read_lock(void) __acquires(__rcu_context)
{
    rcu_read_lock();
}

void xa_rcu_read_unlock(void) __releases(__rcu_context)
{
    rcu_read_unlock();
}

/*
 * xa_call_rcu trampoline.  The portable xarray invokes us with a
 * callback of type void(*)(void*) and the xa_node pointer as data.
 * Kernel call_rcu() requires an rcu_head_t embedded in the target
 * object, so we use the node's embedded head and wrap the caller's
 * callback (which already handles the final free via xa_free_fn).
 */
void xa_call_rcu(xa_rcu_callback_t cb, void *node)
{
    struct xa_node *n = (struct xa_node *)node;
    /* The kernel rcu_callback_t signature matches xa_rcu_callback_t. */
    call_rcu(&n->rcu_head, (rcu_callback_t)cb, node);
}

/* ====================================================================== */
/*  Node allocation                                                        */
/* ====================================================================== */

static struct xa_node *xa_node_alloc(void)
{
    struct xa_node *node = (struct xa_node *)xa_alloc_fn(sizeof(struct xa_node));
    return node; /* xa_alloc_fn returns zeroed memory */
}

#ifdef XA_CONFIG_RCU
static void __xa_node_free_cb(void *data)
{
    xa_free_fn(data);
}
#endif

static void xa_node_free_rcu(struct xa_node *node)
{
    if (node == NULL)
        return;
#ifdef XA_CONFIG_RCU
    xa_call_rcu(__xa_node_free_cb, node);
#else
    xa_free_fn(node);
#endif
}

static void xa_node_free_now(struct xa_node *node)
{
    if (node == NULL)
        return;
    xa_free_fn(node);
}

/* ====================================================================== */
/*  Internal helpers — slot access                                         */
/* ====================================================================== */

/** Calculate the slot offset for @index at a node with the given @shift. */
static inline uint8_t xa_offset(uint64_t index, uint8_t shift)
{
    return (uint8_t)((index >> shift) & XA_CHUNK_MASK);
}

/** Maximum index that can be addressed by a tree of the given @shift. */
static inline uint64_t xa_max_index(uint8_t shift)
{
    uint8_t total_bits = shift + XA_CHUNK_SHIFT;
    if (total_bits >= 64)
        return ~0ULL;
    return (1ULL << total_bits) - 1;
}

/** Base index covered by a node level containing @index. */
static inline uint64_t xa_level_base(uint64_t index, uint8_t shift)
{
    uint8_t total_bits = shift + XA_CHUNK_SHIFT;

    if (total_bits >= 64)
        return 0;
    return index & ~((1ULL << total_bits) - 1);
}

/** Decode the head entry to a node pointer, or NULL if not a node. */
static inline struct xa_node *xa_head_to_node(void *head)
{
    if (xa_is_internal(head) && head != XA_RETRY_ENTRY &&
        head != XA_ZERO_ENTRY && !xa_is_sibling(head))
        return xa_to_internal(head);
    return NULL;
}

static inline bool node_get_mark(const struct xa_node *node, uint8_t offset,
                                 xa_mark_t mark);
static inline void node_clear_mark(struct xa_node *node, uint8_t offset,
                                   xa_mark_t mark);
static inline bool node_any_mark(const struct xa_node *node, xa_mark_t mark);
static void node_propagate_mark_clear(struct xa_node *node, uint8_t offset,
                                      xa_mark_t mark);

static void *xa_resolve_sibling(struct xa_node *node, uint8_t *offset)
{
    void *entry;
    uint8_t cur;

    if (node == NULL || offset == NULL)
        return NULL;

    cur = *offset;
    entry = xa_slot_load(&node->slots[cur]);
    while (xa_is_sibling(entry)) {
        uint8_t sibling = xa_to_sibling(entry);
        if (sibling >= XA_CHUNK_SIZE)
            return NULL;
        cur = sibling;
        entry = xa_slot_load(&node->slots[cur]);
    }

    *offset = cur;
    return entry;
}

static uint8_t xa_sibling_span(struct xa_node *node, uint8_t canonical_offset)
{
    uint8_t span = 1;

    if (node == NULL)
        return 0;

    while ((uint16_t)canonical_offset + span < XA_CHUNK_SIZE) {
        void *entry = xa_slot_load(&node->slots[canonical_offset + span]);
        if (!xa_is_sibling(entry) ||
            xa_to_sibling(entry) != canonical_offset)
            break;
        span++;
    }

    return span;
}

static void xa_clear_marks_at(struct xa_state *xas, struct xa_node *node,
                              uint8_t offset)
{
    for (xa_mark_t m = 0; m < XA_MAX_MARKS; m++) {
        if (node_get_mark(node, offset, m))
            node_propagate_mark_clear(node, offset, m);
    }
    (void)xas;
}

static inline unsigned int xa_head_mark_flag(xa_mark_t mark)
{
    return 1U << (XA_HEAD_MARK_SHIFT + mark);
}

static inline bool xa_head_get_mark(const struct xarray *xa, xa_mark_t mark)
{
    if (mark >= XA_MAX_MARKS)
        return false;

    unsigned int flags = xa_flags_load(&xa->xa_flags);
    return (flags & xa_head_mark_flag(mark)) != 0;
}

static inline void xa_head_set_mark(struct xarray *xa, xa_mark_t mark)
{
    if (mark >= XA_MAX_MARKS)
        return;
    xa_flags_or(&xa->xa_flags, xa_head_mark_flag(mark));
}

static inline void xa_head_clear_mark(struct xarray *xa, xa_mark_t mark)
{
    if (mark >= XA_MAX_MARKS)
        return;
    xa_flags_and(&xa->xa_flags, ~xa_head_mark_flag(mark));
}

static inline void xa_head_clear_all_marks(struct xarray *xa)
{
    xa_flags_and(&xa->xa_flags, ~XA_HEAD_MARK_MASK);
}

/* ====================================================================== */
/*  Mark helpers                                                           */
/* ====================================================================== */

static inline bool node_get_mark(const struct xa_node *node, uint8_t offset,
                                 xa_mark_t mark)
{
    if (mark >= XA_MAX_MARKS)
        return false;
    return (node->marks[mark][offset >> 6] >> (offset & 63)) & 1;
}

static inline void node_set_mark(struct xa_node *node, uint8_t offset,
                                 xa_mark_t mark)
{
    if (mark >= XA_MAX_MARKS)
        return;
    node->marks[mark][offset >> 6] |= 1ULL << (offset & 63);
}

static inline void node_clear_mark(struct xa_node *node, uint8_t offset,
                                   xa_mark_t mark)
{
    if (mark >= XA_MAX_MARKS)
        return;
    node->marks[mark][offset >> 6] &= ~(1ULL << (offset & 63));
}

/** Check if any slot in @node has @mark set. */
static inline bool node_any_mark(const struct xa_node *node, xa_mark_t mark)
{
    if (mark >= XA_MAX_MARKS)
        return false;
    for (size_t i = 0; i < XA_MARK_LONGS; i++) {
        if (node->marks[mark][i])
            return true;
    }
    return false;
}

/**
 * node_propagate_mark_set - Set a mark and propagate it up to ancestors.
 *
 * Sets @mark at @offset in @node, then walks up the parent chain setting
 * the mark on each ancestor until an ancestor already has the mark set.
 */
static void node_propagate_mark_set(struct xa_node *node, uint8_t offset,
                                    xa_mark_t mark)
{
    node_set_mark(node, offset, mark);
    while (node->parent) {
        uint8_t off = node->offset;
        node = node->parent;
        if (node_get_mark(node, off, mark))
            break;
        node_set_mark(node, off, mark);
    }
}

/**
 * node_propagate_mark_clear - Clear a mark and propagate up to ancestors.
 *
 * Clears @mark at @offset in @node, then walks up the parent chain
 * clearing the mark from each ancestor whose children no longer carry it.
 */
static void node_propagate_mark_clear(struct xa_node *node, uint8_t offset,
                                      xa_mark_t mark)
{
    node_clear_mark(node, offset, mark);
    while (node->parent) {
        uint8_t off = node->offset;
        if (node_any_mark(node, mark))
            break;
        node = node->parent;
        node_clear_mark(node, off, mark);
    }
}

/* ====================================================================== */
/*  Internal: tree growth (expand) and shrinkage                           */
/* ====================================================================== */

/**
 * xa_expand - Grow the tree until it can accommodate @index.
 *
 * Called with xa_lock held (if locking enabled).
 * Returns 0 on success, -ENOMEM on allocation failure.
 */
static int xa_expand(struct xarray *xa, uint64_t index)
{
    void *head = xa->xa_head;

    if (head == NULL && index == 0)
        return 0;

    if (head == NULL) {
        struct xa_node *leaf = xa_node_alloc();
        if (leaf == NULL)
            return -ENOMEM;

        leaf->shift   = 0;
        leaf->offset  = 0;
        leaf->count   = 0;
        leaf->parent  = NULL;
        leaf->array   = xa;

        head = xa_mk_internal(leaf);
        xa_slot_store(&xa->xa_head, head);
    }

    struct xa_node *node = xa_head_to_node(head);

    /* Single-entry head covers only index 0.  Promote to a leaf. */
    if (node == NULL) {
        if (index == 0)
            return 0;

        struct xa_node *leaf = xa_node_alloc();
        if (leaf == NULL)
            return -ENOMEM;

        leaf->shift   = 0;
        leaf->offset  = 0;
        leaf->count   = 1;
        leaf->parent  = NULL;
        leaf->array   = xa;
        leaf->slots[0] = head;

        for (xa_mark_t m = 0; m < XA_MAX_MARKS; m++) {
            if (xa_head_get_mark(xa, m))
                node_set_mark(leaf, 0, m);
        }
        xa_head_clear_all_marks(xa);

        head = xa_mk_internal(leaf);
        xa_slot_store(&xa->xa_head, head);
        node = leaf;
    }

    /* Grow upward until the tree covers @index. */
    while (index > xa_max_index(node->shift)) {
        struct xa_node *new_node = xa_node_alloc();
        if (new_node == NULL)
            return -ENOMEM;

        new_node->shift  = node->shift + XA_CHUNK_SHIFT;
        new_node->offset = 0;
        new_node->count  = 1;
        new_node->parent = NULL;
        new_node->array  = xa;

        new_node->slots[0] = head;
        node->offset = 0;
        node->parent = new_node;

        for (xa_mark_t m = 0; m < XA_MAX_MARKS; m++) {
            if (node_any_mark(node, m))
                node_set_mark(new_node, 0, m);
        }

        head = xa_mk_internal(new_node);
        xa_slot_store(&xa->xa_head, head);
        node = new_node;
    }

    return 0;
}

/**
 * xa_shrink - Shrink the tree if the root node has only one child.
 *
 * Called with xa_lock held (if locking enabled).
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
            xa_slot_store(&xa->xa_head, NULL);
            xa_head_clear_all_marks(xa);
            xa_node_free_rcu(node);
            break;
        }

        void *child = node->slots[0];
        if (child == NULL)
            break;

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

        xa_slot_store(&xa->xa_head, child);
        node->slots[0] = NULL;
        node->count = 0;
        xa_node_free_rcu(node);

        if (child_node == NULL)
            break;
    }
}

/* ====================================================================== */
/*  Internal: walk to a slot                                               */
/* ====================================================================== */

/**
 * xas_descend_to_leaf - Walk the xa_state cursor down to the node
 * containing the slot for xas->xa_index.
 */
static void *xas_descend_to_leaf(struct xa_state *xas)
{
    struct xarray *xa = xas->xa;
    void *head = xa_slot_load(&xa->xa_head);

    if (head == NULL) {
        xas->xa_node = XAS_BOUNDS;
        return NULL;
    }

    struct xa_node *node = xa_head_to_node(head);
    if (node == NULL) {
        if (xas->xa_index != 0) {
            xas->xa_node = XAS_BOUNDS;
            return NULL;
        }
        xas->xa_node = NULL;
        xas->xa_offset = 0;
        xas->xa_shift = 0;
        return head;
    }

    if (xas->xa_index > xa_max_index(node->shift)) {
        xas->xa_node = XAS_BOUNDS;
        return NULL;
    }

    while (node) {
        uint8_t offset = xa_offset(xas->xa_index, node->shift);
        void *entry = xa_slot_load(&node->slots[offset]);

        xas->xa_node = node;
        xas->xa_offset = offset;
        xas->xa_shift = node->shift;
        xas->xa_sibs = 0;

        if (node->shift == 0) {
            entry = xa_resolve_sibling(node, &xas->xa_offset);
            if (entry != NULL && xas->xa_offset == offset)
                xas->xa_sibs = xa_sibling_span(node, xas->xa_offset) - 1;
            return entry;
        }

        struct xa_node *child = xa_head_to_node(entry);
        if (child == NULL) {
            return entry;
        }

        node = child;
    }

    return NULL;
}

/**
 * xas_create - Ensure all intermediate nodes exist for xas->xa_index.
 *
 * Called with xa_lock held (if locking enabled).
 */
static void *xas_create(struct xa_state *xas)
{
    struct xarray *xa = xas->xa;
    void *head;
    struct xa_node *node;

    if (xa_expand(xa, xas->xa_index) != 0) {
        xas_set_err(xas, -ENOMEM);
        return NULL;
    }

    head = xa_slot_load(&xa->xa_head);

    if (head == NULL) {
        xas->xa_node = NULL;
        xas->xa_offset = 0;
        xas->xa_shift = 0;
        return NULL;
    }

    node = xa_head_to_node(head);
    if (node == NULL) {
        if (xas->xa_index == 0) {
            xas->xa_node = NULL;
            xas->xa_offset = 0;
            xas->xa_shift = 0;
            return head;
        }
        xas_set_err(xas, -ENOMEM);
        return NULL;
    }

    while (node->shift > 0) {
        uint8_t offset = xa_offset(xas->xa_index, node->shift);
        void *entry = xa_slot_load(&node->slots[offset]);
        struct xa_node *child = xa_head_to_node(entry);

        if (child == NULL) {
            if (entry != NULL && !xa_is_internal(entry)) {
                /* A leaf entry at a non-leaf level — treat as empty. */
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

            xa_slot_store(&node->slots[offset], xa_mk_internal(child));
            if (entry == NULL)
                node->count++;
        }

        node = child;
    }

    uint8_t offset = xa_offset(xas->xa_index, 0);
    xas->xa_node = node;
    xas->xa_offset = offset;
    xas->xa_shift = 0;
    return xa_slot_load(&node->slots[offset]);
}

/* ====================================================================== */
/*  Internal: node cleanup after erase                                     */
/* ====================================================================== */

static void xas_delete_node(struct xa_state *xas)
{
    struct xa_node *node = xas->xa_node;

    while (node) {
        if (node->count > 0)
            break;

        struct xa_node *parent = node->parent;
        if (parent) {
            xa_slot_store(&parent->slots[node->offset], NULL);

            for (xa_mark_t m = 0; m < XA_MAX_MARKS; m++) {
                if (node_get_mark(parent, node->offset, m))
                    node_propagate_mark_clear(parent, node->offset, m);
            }

            parent->count--;
        } else {
            xa_slot_store(&xas->xa->xa_head, NULL);
            xa_head_clear_all_marks(xas->xa);
        }

        xa_node_free_rcu(node);
        node = parent;
    }

    xa_shrink(xas->xa);
}

/* ====================================================================== */
/*  XAS (cursor) API implementation                                        */
/* ====================================================================== */

/**
 * xas_walk_next - Advance the cursor to the next qualifying slot.
 *
 * Starting from xas->xa_offset + 1 in @node, scan forward for:
 *   - Any non-NULL non-sibling entry (when @mark >= XA_MAX_MARKS)
 *   - A slot with the given @mark set (when @mark < XA_MAX_MARKS)
 *
 * If no qualifying slot is found in @node, walk up to the parent
 * and repeat.  Updates xas->xa_index to the new position on success.
 *
 * Returns true if a qualifying slot was found, false if exhausted.
 */
static bool xas_walk_next(struct xa_state *xas, struct xa_node *node,
                          xa_mark_t mark)
{
    while (node) {
        for (uint8_t off = xas->xa_offset + 1; off < XA_CHUNK_SIZE; off++) {
            bool match;
            if (mark < XA_MAX_MARKS)
                match = node_get_mark(node, off, mark);
            else {
                void *slot = xa_slot_load(&node->slots[off]);
                match = (slot != NULL && !xa_is_sibling(slot));
            }
            if (match) {
                uint64_t base = xa_level_base(xas->xa_index, node->shift);
                xas->xa_index = base | ((uint64_t)off << node->shift);
                return true;
            }
        }
        xas->xa_offset = node->offset;
        node = node->parent;
    }
    return false;
}

/**
 * xas_invalid - Check if cursor is in an error or unpositioned state.
 *
 * Returns true for BOUNDS, ERROR, RESTART — but NOT for NULL (which is
 * a valid positioned state meaning "at the head entry").  Use this as
 * the guard in mark operations where the NULL/head case must proceed.
 */
static inline bool xas_invalid(const struct xa_state *xas)
{
    return xas->xa_node == XAS_BOUNDS ||
           xas->xa_node == XAS_ERROR ||
           xas->xa_node == XAS_RESTART;
}

void *xas_load(struct xa_state *xas)
{
    return xas_descend_to_leaf(xas);
}

/**
 * xas_store_to_head - Handle the head-entry (single slot, no node) case.
 *
 * When the tree has only index 0 stored directly in xa_head, this
 * function replaces or erases it.  Returns the previous head entry.
 */
static void *xas_store_to_head(struct xa_state *xas, void *entry)
{
    if (xas->xa_sibs != 0) {
        xas_set_err(xas, -EINVAL);
        return NULL;
    }
    void *prev = xa_slot_load(&xas->xa->xa_head);
    xa_slot_store(&xas->xa->xa_head, entry);

    if (entry == NULL || prev == NULL)
        xa_head_clear_all_marks(xas->xa);

    return prev;
}

/**
 * xas_validate_sibling_range - Check that the target slot range is valid.
 *
 * Ensures the range [canonical, canonical + new_span) fits within the
 * node and does not overlap with foreign (non-sibling) entries.
 * Returns true if valid, false and sets -EINVAL if not.
 */
static bool xas_validate_sibling_range(struct xa_state *xas,
                                       struct xa_node *node,
                                       uint8_t canonical, uint8_t new_span)
{
    if ((uint16_t)canonical + new_span > XA_CHUNK_SIZE) {
        xas_set_err(xas, -EINVAL);
        return false;
    }
    for (uint8_t i = 1; i < new_span; i++) {
        void *slot = xa_slot_load(&node->slots[canonical + i]);
        if (slot == NULL)
            continue;
        if (!xa_is_sibling(slot) || xa_to_sibling(slot) != canonical) {
            xas_set_err(xas, -EINVAL);
            return false;
        }
    }
    return true;
}

/**
 * xas_clear_slot_range - Clear old slots and their marks.
 *
 * Clears up to @count slots starting at @canonical.  Marks on the
 * canonical slot are preserved when @preserve_canonical_marks is true
 * (i.e. overwrite rather than erase).
 */
static void xas_clear_slot_range(struct xa_state *xas, struct xa_node *node,
                                 uint8_t canonical, uint8_t count,
                                 bool preserve_canonical_marks)
{
    for (uint8_t i = 0; i < count; i++) {
        uint8_t off = canonical + i;
        if (off >= XA_CHUNK_SIZE)
            break;
        void *slot = xa_slot_load(&node->slots[off]);
        if (slot == NULL)
            continue;
        if (off != canonical &&
            (!xa_is_sibling(slot) || xa_to_sibling(slot) != canonical))
            break;
        if (!preserve_canonical_marks || off != canonical)
            xa_clear_marks_at(xas, node, off);
        xa_slot_store(&node->slots[off], NULL);
        node->count--;
    }
}

/**
 * xas_fill_slot_range - Write an entry and its sibling pointers.
 *
 * Stores @entry at @canonical and fills slots [canonical+1, canonical+count)
 * with sibling pointers back to @canonical.
 */
static void xas_fill_slot_range(struct xa_node *node, uint8_t canonical,
                                void *entry, uint8_t count)
{
    xa_slot_store(&node->slots[canonical], entry);
    node->count++;
    for (uint8_t i = 1; i < count; i++) {
        xa_slot_store(&node->slots[canonical + i],
                      xa_mk_sibling(canonical));
        node->count++;
    }
}

void *xas_store(struct xa_state *xas, void *entry)
{
    void *old;

    if (xa_is_internal(entry)) {
        xas_set_err(xas, -EINVAL);
        return NULL;
    }

    old = xas_create(xas);
    if (xas_error(xas))
        return NULL;

    struct xa_node *node = xas->xa_node;

    /* Single-entry head: no node in the tree yet. */
    if (node == NULL)
        return xas_store_to_head(xas, entry);

    /* Resolve the canonical (non-sibling) slot and compute spans. */
    uint8_t canonical = xas->xa_offset;
    old = xa_resolve_sibling(node, &canonical);
    uint8_t old_span = (old != NULL) ? xa_sibling_span(node, canonical) : 1;
    uint8_t new_span = xas->xa_sibs + 1;

    if (!xas_validate_sibling_range(xas, node, canonical, new_span))
        return NULL;

    /* Clear old slots; preserve canonical marks on overwrite. */
    uint8_t clear_span = old_span > new_span ? old_span : new_span;
    xas_clear_slot_range(xas, node, canonical, clear_span,
                         entry != NULL);

    /* Write the new entry (and sibling pointers if multi-slot). */
    if (entry != NULL)
        xas_fill_slot_range(node, canonical, entry, new_span);

    xas->xa_offset = canonical;
    xas->xa_sibs = (entry != NULL) ? (new_span - 1) : 0;

    if (entry == NULL && node->count == 0)
        xas_delete_node(xas);

    return old;
}

void *xas_find(struct xa_state *xas, uint64_t max)
{
    struct xarray *xa = xas->xa;
    void *head;

    if (xas_error(xas))
        return NULL;

    if (!xas_is_restart(xas)) {
        if (xas->xa_index >= max)
            return NULL;
        xas->xa_index++;
    }

    head = xa_slot_load(&xa->xa_head);
    if (head == NULL)
        return NULL;

    struct xa_node *root_node = xa_head_to_node(head);
    if (root_node == NULL) {
        if (xas->xa_index == 0) {
            xas->xa_node = NULL;
            xas->xa_offset = 0;
            xas->xa_shift = 0;
            xas->xa_index = 0;
            return head;
        }
        return NULL;
    }

    while (xas->xa_index <= max) {
        xas->xa_node = XAS_RESTART;
        void *entry = xas_descend_to_leaf(xas);
        if (entry != NULL && xa_is_entry(entry)) {
            uint64_t base = xa_level_base(xas->xa_index, 0);
            uint64_t canonical = base | xas->xa_offset;
            if (canonical == xas->xa_index)
                return entry;
        }

        if (xas_is_special(xas))
            return NULL;

        if (!xas_walk_next(xas, xas->xa_node, XA_MARK_MAX))
            return NULL;
    }

    return NULL;
}

void *xas_find_marked(struct xa_state *xas, uint64_t max, xa_mark_t mark)
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

    head = xa_slot_load(&xa->xa_head);
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

    while (xas->xa_index <= max) {
        xas->xa_node = XAS_RESTART;
        void *entry = xas_descend_to_leaf(xas);

        if (xas_is_special(xas))
            return NULL;

        struct xa_node *node = xas->xa_node;
        if (node == NULL)
            return NULL;

        uint64_t base = xa_level_base(xas->xa_index, 0);
        uint64_t canonical = base | xas->xa_offset;
        if (entry != NULL && canonical == xas->xa_index &&
            node_get_mark(node, xas->xa_offset, mark))
            return entry;

        if (!xas_walk_next(xas, node, mark))
            return NULL;
    }

    return NULL;
}

void xas_set_mark(struct xa_state *xas, xa_mark_t mark)
{
    if (mark >= XA_MAX_MARKS || xas_invalid(xas))
        return;

    struct xa_node *node = xas->xa_node;

    if (node == NULL) {
        if (xas->xa_index != 0)
            return;
        void *head = xa_slot_load(&xas->xa->xa_head);
        if (xa_is_entry(head))
            xa_head_set_mark(xas->xa, mark);
        return;
    }

    node_propagate_mark_set(node, xas->xa_offset, mark);
}

void xas_clear_mark(struct xa_state *xas, xa_mark_t mark)
{
    if (mark >= XA_MAX_MARKS || xas_invalid(xas))
        return;

    struct xa_node *node = xas->xa_node;

    if (node == NULL) {
        if (xas->xa_index == 0)
            xa_head_clear_mark(xas->xa, mark);
        return;
    }

    node_propagate_mark_clear(node, xas->xa_offset, mark);
}

bool xas_get_mark(struct xa_state *xas, xa_mark_t mark)
{
    if (mark >= XA_MAX_MARKS || xas_invalid(xas))
        return false;

    struct xa_node *node = xas->xa_node;

    if (node == NULL) {
        if (xas->xa_index != 0)
            return false;
        void *head = xa_slot_load(&xas->xa->xa_head);
        if (!xa_is_entry(head))
            return false;
        return xa_head_get_mark(xas->xa, mark);
    }

    return node_get_mark(node, xas->xa_offset, mark);
}

/* ====================================================================== */
/*  Simple API implementation                                              */
/* ====================================================================== */

void *xa_load(struct xarray *xa, uint64_t index)
{
    void *entry;
    XA_STATE(xas, xa, index);

    xa_rcu_lock();
    entry = xas_load(&xas);

    if (entry == XA_RETRY_ENTRY || entry == XA_ZERO_ENTRY)
        entry = NULL;
    if (xa_is_internal(entry))
        entry = NULL;
    xa_rcu_unlock();

    return entry;
}

void *xa_store(struct xarray *xa, uint64_t index, void *entry, uint64_t gfp)
{
    void *old;
    XA_STATE(xas, xa, index);
    (void)gfp;

    /* Reject internal/sentinel entries — they would corrupt the tree. */
    if (xa_is_internal(entry))
        return XA_ZERO_ENTRY;

    xa_lock(xa);
    old = xas_store(&xas, entry);
    xa_unlock(xa);

    if (xas_error(&xas))
        return XA_ZERO_ENTRY;

    if (xa_is_internal(old))
        old = NULL;

    return old;
}

void *xa_erase(struct xarray *xa, uint64_t index)
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

static void xa_destroy_node(struct xa_node *node)
{
    if (node == NULL)
        return;

    if (node->shift > 0) {
        for (size_t i = 0; i < XA_CHUNK_SIZE; i++) {
            void *entry = node->slots[i];
            struct xa_node *child = xa_head_to_node(entry);
            if (child != NULL)
                xa_destroy_node(child);
        }
    }

    xa_node_free_now(node);
}

#ifdef XA_CONFIG_RCU
static void __xa_destroy_node_cb(void *data)
{
    xa_destroy_node((struct xa_node *)data);
}
#endif

void xa_destroy(struct xarray *xa)
{
    void *head;

    xa_lock(xa);
    head = xa->xa_head;
    xa->xa_head = NULL;
    xa_head_clear_all_marks(xa);
    xa_unlock(xa);

    struct xa_node *node = xa_head_to_node(head);
    if (node != NULL) {
#ifdef XA_CONFIG_RCU
        xa_call_rcu(__xa_destroy_node_cb, node);
#else
        xa_destroy_node(node);
#endif
    }
}

/* ====================================================================== */
/*  Mark simple API                                                        */
/* ====================================================================== */

void xa_set_mark(struct xarray *xa, uint64_t index, xa_mark_t mark)
{
    XA_STATE(xas, xa, index);

    xa_lock(xa);
    void *entry = xas_load(&xas);
    if (entry != NULL)
        xas_set_mark(&xas, mark);
    xa_unlock(xa);
}

void xa_clear_mark(struct xarray *xa, uint64_t index, xa_mark_t mark)
{
    XA_STATE(xas, xa, index);

    xa_lock(xa);
    void *entry = xas_load(&xas);
    if (entry != NULL)
        xas_clear_mark(&xas, mark);
    xa_unlock(xa);
}

bool xa_get_mark(struct xarray *xa, uint64_t index, xa_mark_t mark)
{
    bool result;
    XA_STATE(xas, xa, index);

    xa_rcu_lock();
    xas_load(&xas);
    result = xas_get_mark(&xas, mark);
    xa_rcu_unlock();

    return result;
}

/* ====================================================================== */
/*  xa_find / xa_find_after                                                */
/* ====================================================================== */

static void *__xa_find(struct xarray *xa, uint64_t *indexp, uint64_t max,
                       xa_mark_t mark)
{
    void *entry = NULL;

    if (*indexp > max)
        return NULL;

    XA_STATE(xas, xa, *indexp);

    xa_rcu_lock();

    if (mark == XA_MARK_MAX) {
        xas.xa_node = XAS_RESTART;
        entry = xas_descend_to_leaf(&xas);

        if (entry != NULL && xa_is_entry(entry)) {
            /* After sibling resolution, xa_offset may differ from the
             * offset implied by xa_index.  Report the canonical index. */
            uint64_t found_index = xas.xa_index;
            struct xa_node *node = xas.xa_node;
            if (node != NULL) {
                uint64_t base = xa_level_base(xas.xa_index, 0);
                found_index = base | xas.xa_offset;
            }

            if (found_index == xas.xa_index) {
                *indexp = found_index;
                xa_rcu_unlock();
                return entry;
            }
        }

        entry = xas_find(&xas, max);
        if (entry != NULL) {
            *indexp = xas.xa_index;
        }
    } else {
        xas.xa_node = XAS_RESTART;
        void *e = xas_descend_to_leaf(&xas);
        if (e != NULL && xas.xa_node != XAS_BOUNDS && xas.xa_node != XAS_ERROR &&
            xas.xa_node != XAS_RESTART) {
            struct xa_node *node = xas.xa_node;
            /* For marked find, only match if the canonical index equals
             * the requested index.  Sibling slots share the canonical
             * entry but should not produce duplicate results. */
            uint64_t canonical_index = xas.xa_index;
            if (node != NULL) {
                uint64_t base = xa_level_base(xas.xa_index, 0);
                canonical_index = base | xas.xa_offset;
            }
            if (canonical_index == xas.xa_index) {
                if ((node != NULL && node_get_mark(node, xas.xa_offset, mark)) ||
                    (node == NULL && xas.xa_index == 0 && xa_head_get_mark(xa, mark))) {
                    *indexp = xas.xa_index;
                    xa_rcu_unlock();
                    return e;
                }
            }
        }

        entry = xas_find_marked(&xas, max, mark);
        if (entry != NULL) {
            *indexp = xas.xa_index;
        }
    }

    xa_rcu_unlock();
    return entry;
}

void *xa_find(struct xarray *xa, uint64_t *indexp, uint64_t max, xa_mark_t mark)
{
    return __xa_find(xa, indexp, max, mark);
}

void *xa_find_after(struct xarray *xa, uint64_t *indexp, uint64_t max,
                    xa_mark_t mark)
{
    if (*indexp >= max)
        return NULL;
    (*indexp)++;
    return __xa_find(xa, indexp, max, mark);
}
