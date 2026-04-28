/**
 * @file xarray.h
 * @brief XArray public API — Linux-style radix tree.
 *
 * The XArray is an abstract data type which behaves like a very large array
 * of pointers.  It is more cache-efficient than a hash table and provides
 * efficient iteration.  Entries can be tagged with up to 3 independent marks.
 *
 * Two API levels are provided:
 *
 *  1. **Simple API** (xa_load, xa_store, xa_erase, …):
 *     When XA_CONFIG_LOCK is enabled, takes xa_lock internally.
 *     Safe to call from any context.
 *
 *  2. **Advanced / cursor API** (xas_load, xas_store, …):
 *     When locking is enabled, caller must hold xa_lock.
 *     Provides finer-grained control and iteration.
 */

#ifndef XARRAY_H
#define XARRAY_H

#include "xarray_type.h"

/**
 * xarray_global_init - Initialise the xa_node slab cache.
 * Must be called once during boot before any xarray is used.
 */
void xarray_global_init(void);

/* ====================================================================== */
/*  XArray initialisation                                                  */
/* ====================================================================== */

/** Initialise an xarray with flags. */
static inline void xa_init_flags(struct xarray *xa, unsigned int flags)
{
#ifdef XA_CONFIG_LOCK
    xa_lock_init(&xa->xa_lock);
#endif
    xa->xa_flags = flags & ~XA_HEAD_MARK_MASK;
    xa->xa_head = NULL;
}

/** Initialise an xarray (no flags). */
static inline void xa_init(struct xarray *xa)
{
    xa_init_flags(xa, 0);
}

/** Return true if the xarray contains no entries. */
static inline bool xa_empty(const struct xarray *xa)
{
#ifdef XA_CONFIG_RCU
    void *head = *(void *const volatile *)&xa->xa_head;
    return head == NULL;
#else
    return xa->xa_head == NULL;
#endif
}

/* ====================================================================== */
/*  Locking helpers                                                        */
/* ====================================================================== */

#ifdef XA_CONFIG_LOCK

/** Acquire the xarray's internal lock. */
static inline void xa_lock(struct xarray *xa)
{
    xa_spin_lock(&xa->xa_lock);
}

/** Release the xarray's internal lock. */
static inline void xa_unlock(struct xarray *xa)
{
    xa_spin_unlock(&xa->xa_lock);
}

#else /* !XA_CONFIG_LOCK */

static inline void xa_lock(struct xarray *xa) { (void)xa; }
static inline void xa_unlock(struct xarray *xa) { (void)xa; }

#endif /* XA_CONFIG_LOCK */

/* ====================================================================== */
/*  RCU helpers                                                            */
/* ====================================================================== */

#ifdef XA_CONFIG_RCU

static inline void xa_rcu_lock(void) __acquires(__rcu_context)
{
    xa_rcu_read_lock();
}
static inline void xa_rcu_unlock(void) __releases(__rcu_context)
{
    xa_rcu_read_unlock();
}

#else /* !XA_CONFIG_RCU */

static inline void xa_rcu_lock(void) {}
static inline void xa_rcu_unlock(void) {}

#endif /* XA_CONFIG_RCU */

/* ====================================================================== */
/*  Simple API                                                             */
/* ====================================================================== */

/**
 * xa_load - Look up an entry in the xarray.
 * @xa:    Xarray.
 * @index: Index to look up.
 *
 * Returns the entry at @index, or NULL if absent.
 * When XA_CONFIG_RCU is enabled, uses rcu_read_lock internally.
 */
void *xa_load(struct xarray *xa, uint64_t index);

/**
 * xa_store - Store an entry in the xarray.
 * @xa:    Xarray.
 * @index: Index to store at.
 * @entry: Entry to store (must not be an internal entry).
 * @gfp:   Allocation flags (currently unused, reserved).
 *
 * Returns the old entry at @index, or NULL.
 * On allocation failure, returns XA_ZERO_ENTRY and the xarray is unchanged.
 */
void *xa_store(struct xarray *xa, uint64_t index, void *entry, uint64_t gfp);

/**
 * xa_erase - Remove an entry from the xarray.
 * @xa:    Xarray.
 * @index: Index to erase.
 *
 * Returns the old entry, or NULL.
 */
void *xa_erase(struct xarray *xa, uint64_t index);

/**
 * xa_destroy - Free all internal nodes of the xarray.
 * @xa: Xarray.
 *
 * After this call the xarray is empty.  Does NOT free user entries —
 * the caller must have already freed or detached them.  When
 * XA_CONFIG_RCU is enabled, internal nodes are reclaimed after a grace
 * period.
 */
void xa_destroy(struct xarray *xa);

/* ====================================================================== */
/*  Mark API                                                               */
/* ====================================================================== */

/**
 * xa_set_mark - Set a mark on an entry.
 * @xa:    Xarray.
 * @index: Index whose entry should be marked.
 * @mark:  Mark to set (XA_MARK_0, XA_MARK_1, or XA_MARK_2).
 *
 * Does nothing if there is no entry at @index.
 */
void xa_set_mark(struct xarray *xa, uint64_t index, xa_mark_t mark);

/**
 * xa_clear_mark - Clear a mark on an entry.
 * @xa:    Xarray.
 * @index: Index whose mark should be cleared.
 * @mark:  Mark to clear.
 */
void xa_clear_mark(struct xarray *xa, uint64_t index, xa_mark_t mark);

/**
 * xa_get_mark - Test whether a mark is set on an entry.
 * @xa:    Xarray.
 * @index: Index to test.
 * @mark:  Mark to test.
 *
 * Returns true if @mark is set on the entry at @index, false otherwise.
 */
bool xa_get_mark(struct xarray *xa, uint64_t index, xa_mark_t mark);

/**
 * xa_find - Find the first entry with a given mark.
 * @xa:      Xarray.
 * @indexp:  Pointer to starting index; updated to the found index.
 * @max:     Maximum index to search.
 * @mark:    Mark to search for.
 *
 * Returns the first marked entry at or after *@indexp, up to @max.
 * Updates *@indexp to the index of the found entry.
 * Returns NULL if no marked entry is found.
 */
void *xa_find(struct xarray *xa, uint64_t *indexp, uint64_t max, xa_mark_t mark);

/**
 * xa_find_after - Find the next entry with a given mark after an index.
 * @xa:      Xarray.
 * @indexp:  Pointer to index; search starts at *@indexp + 1.
 * @max:     Maximum index to search.
 * @mark:    Mark to search for.
 *
 * Like xa_find() but starts searching after *@indexp.
 */
void *xa_find_after(struct xarray *xa, uint64_t *indexp, uint64_t max,
                    xa_mark_t mark);

/* ====================================================================== */
/*  Advanced / cursor API                                                  */
/* ====================================================================== */

/** Reset an xa_state to restart the walk from a new index. */
static inline void xas_set(struct xa_state *xas, uint64_t index)
{
    xas->xa_index = index;
    xas->xa_node = XAS_RESTART;
    xas->xa_error = 0;
}

/** Reset an xa_state to restart from the current index. */
static inline void xas_rewind(struct xa_state *xas)
{
    xas->xa_node = XAS_RESTART;
}

/** Set the xa_state into an error state. */
static inline void xas_set_err(struct xa_state *xas, int err)
{
    xas->xa_node = XAS_ERROR;
    xas->xa_error = err;
}

/** Check if an entry is a retry entry (reader should restart). */
static inline bool xas_retry(struct xa_state *xas, const void *entry)
{
    if (entry == XA_RETRY_ENTRY) {
        xas_rewind(xas);
        return true;
    }
    return false;
}

/**
 * xas_load - Load the entry at the cursor's current index.
 * @xas: XA state (cursor).
 *
 * Returns the entry, or NULL.
 */
void *xas_load(struct xa_state *xas);

/**
 * xas_store - Store an entry at the cursor's current index.
 * @xas:   XA state (cursor).
 * @entry: Entry to store (must not be an internal entry).
 *
 * Returns the old entry.
 */
void *xas_store(struct xa_state *xas, void *entry);

/**
 * xas_find - Find the next non-NULL entry.
 * @xas: XA state (cursor).  On return, xas->xa_index is the found index.
 * @max: Maximum index to search.
 *
 * Returns the entry, or NULL if no more entries exist up to @max.
 */
void *xas_find(struct xa_state *xas, uint64_t max);

/**
 * xas_find_marked - Find the next marked entry.
 * @xas:  XA state (cursor).
 * @max:  Maximum index to search.
 * @mark: Mark to search for.
 *
 * Returns the entry, or NULL if none found.
 */
void *xas_find_marked(struct xa_state *xas, uint64_t max, xa_mark_t mark);

/**
 * xas_set_mark - Set a mark at the cursor's current position.
 * @xas:  XA state.
 * @mark: Mark to set.
 *
 * Propagates mark up to ancestors.
 */
void xas_set_mark(struct xa_state *xas, xa_mark_t mark);

/**
 * xas_clear_mark - Clear a mark at the cursor's current position.
 * @xas:  XA state.
 * @mark: Mark to clear.
 *
 * Clears mark in ancestors if no sibling still has the mark.
 */
void xas_clear_mark(struct xa_state *xas, xa_mark_t mark);

/**
 * xas_get_mark - Test whether a mark is set at the cursor's position.
 * @xas:  XA state.
 * @mark: Mark to test.
 */
bool xas_get_mark(struct xa_state *xas, xa_mark_t mark);

/* ====================================================================== */
/*  Iteration macros                                                       */
/* ====================================================================== */

/**
 * xa_for_each - Iterate over every entry in the xarray.
 * @xa:    Xarray.
 * @index: uint64_t loop variable (updated to current index).
 * @entry: void * loop variable (set to current entry).
 *
 * Only yields non-NULL, non-internal entries.
 */
#define xa_for_each(xa, index, entry)                                   \
    for ((index) = 0,                                                   \
         (entry) = xa_find((xa), &(index), ~0ULL, XA_MARK_MAX);        \
         (entry) != NULL;                                               \
         (entry) = xa_find_after((xa), &(index), ~0ULL, XA_MARK_MAX))

/**
 * xa_for_each_marked - Iterate over entries with a specific mark.
 * @xa:    Xarray.
 * @index: uint64_t loop variable.
 * @entry: void * loop variable.
 * @mark:  Mark to filter on.
 */
#define xa_for_each_marked(xa, index, entry, mark)                      \
    for ((index) = 0,                                                   \
         (entry) = xa_find((xa), &(index), ~0ULL, (mark));             \
         (entry) != NULL;                                               \
         (entry) = xa_find_after((xa), &(index), ~0ULL, (mark)))

/*
 * XA_MARK_MAX - Sentinel mark value meaning "any/all marks" for
 * xa_for_each (iterate all entries regardless of marks).
 */
#define XA_MARK_MAX     ((xa_mark_t)0xFF)

#endif /* XARRAY_H */
