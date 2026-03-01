/**
 * @file maple_tree.h
 * @brief Maple tree public API — Linux-style RCU-safe B-tree for ranges.
 *
 * Usage patterns:
 *
 *   1. Simple API (tree lock managed internally):
 *        mtree_store_range(mt, first, last, entry)  — insert/overwrite
 *        mtree_load(mt, index)                      — point lookup (RCU-safe)
 *        mtree_erase(mt, index)                     — erase entry at index
 *        mtree_destroy(mt)                          — free all nodes
 *
 *   2. Advanced / cursor API (caller manages locking):
 *        MA_STATE(mas, mt, index, last);
 *        mas_walk(&mas);        — descend to entry at mas->index
 *        mas_store(&mas, entry) — store at current position
 *        mas_erase(&mas)        — erase at current position
 *        mas_next(&mas, max)    — advance to next entry
 *        mas_prev(&mas, min)    — move to previous entry
 *
 *   3. RCU read-side helpers (lock-free):
 *        mt_find(mt, &index, max)   — find first non-NULL entry in [index, max]
 *        mt_next(mt, index, max)    — next entry after index
 *        mt_prev(mt, index, min)    — previous entry before index
 *
 *   4. Gap search:
 *        mas_empty_area(&mas, min, max, size)       — find gap of 'size' bytes
 *        mas_empty_area_rev(&mas, min, max, size)   — reverse gap search
 *
 *   5. Iteration:
 *        mt_for_each(mt, entry, index, max)   — iterate entries in [0, max]
 *        mas_for_each(mas, entry, max)        — iterate from current position
 */

#ifndef __KERNEL_MAPLE_TREE_H
#define __KERNEL_MAPLE_TREE_H

#include "maple_tree_type.h"
#include "lock/rcu.h"

/* ====================================================================== */
/*  Initialisation                                                         */
/* ====================================================================== */

/** Initialise the global maple-tree slab cache.  Call once during boot. */
void maple_tree_init(void);

/** Initialise an empty maple tree. */
static inline void mt_init(struct maple_tree *mt)
{
    rcu_assign_pointer(mt->ma_root, NULL);
    mt->ma_flags = 0;
}

/** Initialise an empty maple tree with given flags. */
static inline void mt_init_flags(struct maple_tree *mt, unsigned int flags)
{
    rcu_assign_pointer(mt->ma_root, NULL);
    mt->ma_flags = flags;
}

/** Check whether a maple tree is empty. */
static inline int mt_empty(const struct maple_tree *mt)
{
    return rcu_dereference(mt->ma_root) == NULL;
}

/* ====================================================================== */
/*  Simple API                                                             */
/* ====================================================================== */

/**
 * mtree_load() - Look up entry at @index.
 * @mt:    Maple tree.
 * @index: Index to look up.
 *
 * Returns the entry stored at @index, or NULL if none.
 * RCU-safe: may be called under rcu_read_lock() without external locking.
 */
void *mtree_load(struct maple_tree *mt, uint64 index);

/**
 * mtree_store_range() - Store @entry for the range [@first, @last].
 * @mt:    Maple tree.
 * @first: Start of range (inclusive).
 * @last:  End of range (inclusive).
 * @entry: Pointer to store (must not be NULL; use mtree_erase for removal).
 *
 * Returns 0 on success, negative errno on error.
 * Caller must hold external write lock.
 */
int mtree_store_range(struct maple_tree *mt, uint64 first, uint64 last,
                      void *entry);

/**
 * mtree_store() - Store @entry at a single @index.
 */
static inline int mtree_store(struct maple_tree *mt, uint64 index, void *entry)
{
    return mtree_store_range(mt, index, index, entry);
}

/**
 * mtree_erase() - Erase (set to NULL) the entry at @index.
 * @mt:    Maple tree.
 * @index: Index to erase.
 *
 * Returns the old entry, or NULL.
 * Caller must hold external write lock.
 */
void *mtree_erase(struct maple_tree *mt, uint64 index);

/**
 * mtree_destroy() - Free all nodes in the tree, leaving it empty.
 * @mt: Maple tree.
 *
 * Caller must hold external write lock and ensure no concurrent readers.
 */
void mtree_destroy(struct maple_tree *mt);

/* ====================================================================== */
/*  Cursor (ma_state) API                                                  */
/* ====================================================================== */

/**
 * mas_walk() - Walk the tree to the entry at mas->index.
 *
 * On return, mas->node/offset/min/max are set, and the entry is returned.
 */
void *mas_walk(struct ma_state *mas);

/**
 * mas_store() - Store @entry at the current MAS position [mas->index, mas->last].
 *
 * mas must have been positioned by mas_walk() or mas_find().
 * Returns 0 on success, negative errno on error.
 */
int mas_store(struct ma_state *mas, void *entry);

/**
 * mas_erase() - Erase the entry at the current MAS position.
 *
 * Returns the old entry.
 */
void *mas_erase(struct ma_state *mas);

/**
 * mas_find() - Find the first non-NULL entry in [mas->index, @max].
 *
 * Advances mas->index past the found entry.  Returns the entry or NULL.
 */
void *mas_find(struct ma_state *mas, uint64 max);

/**
 * mas_next() - Advance to the next entry after current position.
 *
 * Returns the next non-NULL entry with index ≤ @max, or NULL.
 */
void *mas_next(struct ma_state *mas, uint64 max);

/**
 * mas_prev() - Move to the previous entry before current position.
 *
 * Returns the previous non-NULL entry with index ≥ @min, or NULL.
 */
void *mas_prev(struct ma_state *mas, uint64 min);

/**
 * mas_empty_area() - Find a gap (consecutive NULLs) of at least @size
 *                    in the range [@min, @max].
 *
 * On success, sets mas->index to the start of the gap and returns 0.
 * On failure, returns -EBUSY.
 */
int mas_empty_area(struct ma_state *mas, uint64 min, uint64 max, uint64 size);

/**
 * mas_empty_area_rev() - Reverse gap search (from high to low).
 *
 * On success, sets mas->index to the start of the gap and returns 0.
 * On failure, returns -EBUSY.
 */
int mas_empty_area_rev(struct ma_state *mas, uint64 min, uint64 max,
                       uint64 size);

/* ====================================================================== */
/*  RCU read-side helpers                                                  */
/* ====================================================================== */

/**
 * mt_dump_tree() - Debug: dump the full tree structure.
 */
void mt_dump_tree(struct maple_tree *mt);

/**
 * mt_find() - Find first non-NULL entry in [@*index, @max].
 *
 * @mt:    Maple tree.
 * @index: Pointer to start index; updated to the found entry's index.
 * @max:   Maximum index to search.
 *
 * Internally calls rcu_read_lock()/rcu_read_unlock().
 */
void *mt_find(struct maple_tree *mt, uint64 *index, uint64 max);

/**
 * mt_next() - RCU-safe: find next entry after @index.
 */
void *mt_next(struct maple_tree *mt, uint64 index, uint64 max);

/**
 * mt_prev() - RCU-safe: find entry before @index.
 */
void *mt_prev(struct maple_tree *mt, uint64 index, uint64 min);

/* ====================================================================== */
/*  Iteration macros                                                       */
/* ====================================================================== */

/**
 * mt_for_each - Iterate over all non-NULL entries in [0, @__max].
 * @__mt:    struct maple_tree *
 * @__entry: void * iterator variable
 * @__index: uint64 variable tracking current index
 * @__max:   Maximum index
 */
#define mt_for_each(__mt, __entry, __index, __max)                          \
    for ((__index) = 0,                                                     \
         (__entry) = mt_find((__mt), &(__index), (__max));                   \
         (__entry) != NULL;                                                 \
         (__entry) = mt_find((__mt), &(__index), (__max)))

/**
 * mas_for_each - Iterate entries from current MAS position.
 * @__mas:   struct ma_state *
 * @__entry: void * iterator variable
 * @__max:   Maximum index (inclusive)
 */
#define mas_for_each(__mas, __entry, __max)                                 \
    while (((__entry) = mas_find((__mas), (__max))) != NULL)

#endif /* __KERNEL_MAPLE_TREE_H */
