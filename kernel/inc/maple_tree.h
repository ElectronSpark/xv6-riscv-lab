/**
 * @file maple_tree.h
 * @brief Maple tree public API — Linux kernel-style B-tree for index ranges.
 *
 * The maple tree is a B-tree that maps non-overlapping integer index ranges
 * to void* entries.  It supports gap tracking for O(log n) free-range
 * searches, automatic node splitting/merging, and optional RCU-safe reads.
 *
 * The API is organised into six sections:
 *
 *   1. **Initialisation**  — Construct and query an empty tree.
 *   2. **Simple API**      — One-call store/load/erase/destroy by index.
 *   3. **Cursor API**      — Stateful walk/find/iterate using `ma_state`.
 *   4. **Gap search**      — Find contiguous free ranges (forward/reverse).
 *   5. **RCU read-side**   — Lock-free readers (when MT_CONFIG_RCU is set).
 *   6. **Locked wrappers** — Self-locking convenience (MT_CONFIG_LOCK).
 *   7. **Iteration macros**— `mt_for_each` / `mas_for_each`.
 *   8. **Debug**           — Tree-dump for diagnostics.
 *
 * **Locking contract (default, without locked wrappers)**
 *
 *   - Reads  (`mtree_load`, `mt_find`, …) are safe against concurrent
 *     RCU-protected reads but NOT against concurrent writes unless the
 *     caller holds an external lock.
 *   - Writes (`mtree_store*`, `mtree_erase`, `mtree_destroy`) must be
 *     serialised by the caller — either using mt_lock()/mt_unlock(),
 *     the mtree_lock_* wrappers, or an external mutex.
 *
 * **Thread safety quick-start**
 *
 *   @code
 *   // Option A — locked wrappers (simplest, one op at a time):
 *   mtree_lock_store(&mt, 42, ptr);
 *   void *v = mtree_lock_load(&mt, 42);
 *
 *   // Option B — manual locking (batch multiple ops atomically):
 *   mt_lock(&mt);
 *   mtree_store(&mt, 1, a);
 *   mtree_store(&mt, 2, b);
 *   mt_unlock(&mt);
 *   @endcode
 */

#ifndef MAPLE_TREE_H
#define MAPLE_TREE_H

#include "maple_tree_type.h"

/**
 * maple_tree_init - Initialise the kernel-side maple-tree slab cache.
 * Must be called once during boot before any maple tree is used.
 */
void maple_tree_init(void);

/* ====================================================================== */
/*  Initialisation                                                         */
/* ====================================================================== */

/**
 * @defgroup init Initialisation
 * @{
 *
 * Every maple tree must be initialised before first use.  The
 * init functions zero the root pointer and set up optional locking.
 *
 * **Functions:**
 *   - mt_init()       — basic initialisation
 *   - mt_init_flags() — initialisation with feature flags
 *   - mt_empty()      — test whether the tree has any entries
 *
 * **Using together:**
 *
 * @code
 * struct maple_tree mt;
 * mt_init(&mt);                   // or mt_init_flags(&mt, flags)
 * assert(mt_empty(&mt));          // tree starts empty
 *
 * mtree_store(&mt, 0, ptr);       // now it has an entry
 * assert(!mt_empty(&mt));
 *
 * mtree_destroy(&mt);             // free all nodes
 * assert(mt_empty(&mt));          // empty again — can be re-used
 * mt_init(&mt);                   // re-init if you want to reuse
 * @endcode
 *
 * @}
 */

/**
 * mt_init - Initialise an empty maple tree.
 * @mt: Pointer to the maple tree structure to initialise.
 *
 * Sets the root pointer to NULL and flags to 0.  When MT_CONFIG_LOCK
 * is defined, the per-tree lock is also initialised.
 *
 * Must be called before any other operation on the tree.
 */
static inline void mt_init(struct maple_tree *mt)
{
    mt_rcu_assign_pointer(&mt->ma_root, NULL);
    mt->ma_flags = 0;
#ifdef MT_CONFIG_LOCK
    mt_lock_init(&mt->ma_lock);
#endif
}

/**
 * mt_init_flags - Initialise an empty maple tree with flags.
 * @mt:    Pointer to the maple tree structure.
 * @flags: Feature flags stored in mt->ma_flags.  Reserved for future use.
 */
static inline void mt_init_flags(struct maple_tree *mt, unsigned int flags)
{
    mt_rcu_assign_pointer(&mt->ma_root, NULL);
    mt->ma_flags = flags;
#ifdef MT_CONFIG_LOCK
    mt_lock_init(&mt->ma_lock);
#endif
}

/**
 * mt_empty - Check whether a maple tree contains any entries.
 * @mt: Pointer to the maple tree.
 *
 * Return: true if the tree has no entries, false otherwise.
 */
static inline bool mt_empty(const struct maple_tree *mt)
{
    return READ_ONCE(mt->ma_root) == NULL;
}

/**
 * mt_set_in_rcu - Enable RCU copy-on-write mode for this tree.
 * @mt: Pointer to the maple tree.
 *
 * Only effective when compiled with MT_CONFIG_RCU.
 */
static inline void mt_set_in_rcu(struct maple_tree *mt)
{
    mt->ma_flags |= MT_FLAGS_USE_RCU;
}

/**
 * mt_clear_in_rcu - Disable RCU copy-on-write mode for this tree.
 * @mt: Pointer to the maple tree.
 */
static inline void mt_clear_in_rcu(struct maple_tree *mt)
{
    mt->ma_flags &= ~MT_FLAGS_USE_RCU;
}

/* ====================================================================== */
/*  Locking helpers                                                        */
/* ====================================================================== */

/**
 * @defgroup locking Locking helpers
 * @{
 *
 * Low-level lock/unlock for the per-tree mutex.  These are building
 * blocks — most callers should prefer the mtree_lock_* wrappers
 * (see @ref locked_wrappers) for single-operation locking, or use
 * mt_lock()/mt_unlock() directly only when batching multiple
 * operations under one critical section.
 *
 * When MT_CONFIG_LOCK is **not** defined, both functions compile to
 * no-ops so code remains portable.
 *
 * **Functions:**
 *   - mt_lock()   — acquire the tree lock
 *   - mt_unlock() — release the tree lock
 *
 * **Using together:**
 *
 * @code
 * // Batch several writes atomically:
 * mt_lock(&mt);
 * mtree_store(&mt, 10, a);
 * mtree_store(&mt, 20, b);
 * void *old = mtree_erase(&mt, 5);
 * mt_unlock(&mt);
 *
 * // Single-operation alternative (no manual lock):
 * mtree_lock_store(&mt, 10, a);
 * @endcode
 *
 * @}
 */

#ifdef MT_CONFIG_LOCK

/**
 * mt_lock - Acquire the tree's internal lock.
 * @mt: Pointer to the maple tree.
 *
 * Only available when MT_CONFIG_LOCK is defined.  The simple API does
 * **not** call this automatically — the caller is responsible for
 * holding the lock around write operations, mirroring the Linux kernel
 * pattern where an external lock (e.g. mmap_lock) protects the tree.
 */
static inline void mt_lock(struct maple_tree *mt)
{
    mt_spin_lock(&mt->ma_lock);
}

/** mt_unlock - Release the tree's internal lock. */
static inline void mt_unlock(struct maple_tree *mt)
{
    mt_spin_unlock(&mt->ma_lock);
}

#else /* !MT_CONFIG_LOCK */

static inline void mt_lock(struct maple_tree *mt)   { (void)mt; }
static inline void mt_unlock(struct maple_tree *mt) { (void)mt; }

#endif /* MT_CONFIG_LOCK */

/* ====================================================================== */
/*  RCU helpers                                                            */
/* ====================================================================== */

/**
 * @defgroup rcu RCU read-side helpers
 * @{
 *
 * Enter/leave an RCU read-side critical section.  Required when using
 * mt_find(), mt_next(), or mt_prev() from multiple reader threads
 * concurrently with writers that defer node freeing via mt_call_rcu().
 *
 * When MT_CONFIG_RCU is **not** defined, both functions compile to
 * no-ops.
 *
 * **Functions:**
 *   - mt_rcu_lock()   — begin RCU read-side section
 *   - mt_rcu_unlock() — end RCU read-side section
 *
 * **Using together:**
 *
 * @code
 * // Explicit RCU section for cursor-based read:
 * mt_rcu_lock();
 * MA_STATE(mas, &mt, 0, 0);
 * void *entry = mas_find(&mas, UINT64_MAX);
 * // ... use entry ...
 * mt_rcu_unlock();
 *
 * // mt_find/mt_next/mt_prev acquire RCU internally, so the above
 * // is only needed when you hold a cursor across multiple calls.
 * @endcode
 *
 * @}
 */

#ifdef MT_CONFIG_RCU

/**
 * mt_rcu_lock / mt_rcu_unlock - Enter / leave an RCU read-side section.
 *
 * Wraps mt_rcu_read_lock() / mt_rcu_read_unlock() from the config
 * header.  Used internally by the RCU read-side helpers (mt_find,
 * mt_next, mt_prev).  Callers using the cursor API directly during
 * RCU reads should bracket their access with these.
 */
static inline void mt_rcu_lock(void) __acquires(__rcu_context)
{
    mt_rcu_read_lock();
}
static inline void mt_rcu_unlock(void) __releases(__rcu_context)
{
    mt_rcu_read_unlock();
}

#else

static inline void mt_rcu_lock(void)   {}
static inline void mt_rcu_unlock(void) {}

#endif

/* ====================================================================== */
/*  Simple API                                                             */
/* ====================================================================== */

/**
 * @defgroup simple Simple API
 * @{
 *
 * The simple API provides the most common operations in a single
 * function call: store a range, load by index, erase, and destroy.
 * These functions do **not** acquire any lock internally — the caller
 * must serialise writes via mt_lock() or an external mutex.
 *
 * **Functions:**
 *   - mtree_load()        — point lookup
 *   - mtree_store_range() — insert/overwrite a contiguous range
 *   - mtree_store()       — convenience: store at a single index
 *   - mtree_erase()       — remove the entire entry covering an index
 *   - mtree_destroy()     — free all internal nodes
 *
 * **Using together:**
 *
 * @code
 * struct maple_tree mt;
 * mt_init(&mt);
 *
 * // --- Store entries ---
 * mtree_store_range(&mt, 100, 199, region_a);   // range [100, 199]
 * mtree_store(&mt, 42, single_obj);             // single index 42
 *
 * // --- Look up ---
 * void *v = mtree_load(&mt, 150);               // returns region_a
 * assert(mtree_load(&mt, 50) == NULL);           // gap → NULL
 *
 * // --- Erase / hole-punch ---
 * void *old = mtree_erase(&mt, 150);             // removes ALL of [100,199]
 * // To punch a one-index hole instead:
 * mtree_store_range(&mt, 150, 150, NULL);        // only index 150 → NULL
 *
 * // --- Cleanup ---
 * mtree_destroy(&mt);                            // frees nodes, not entries
 * @endcode
 *
 * @}
 */

/**
 * mtree_load - Look up the entry stored at @index.
 * @mt:    Pointer to the maple tree.
 * @index: The index to look up.
 *
 * Searches the tree for the entry whose range contains @index.
 * Safe to call concurrently with RCU readers when MT_CONFIG_RCU is
 * enabled; the caller must hold mt_rcu_lock() or equivalent.
 *
 * Return: The stored void* entry, or NULL if the index falls in a gap.
 */
void *mtree_load(struct maple_tree *mt, uint64_t index);

/**
 * mtree_store_range - Store @entry for every index in [@first, @last].
 * @mt:    Pointer to the maple tree.
 * @first: First index of the range (inclusive).
 * @last:  Last index of the range (inclusive).  Must be >= @first.
 * @entry: The value to associate with the range.  May be NULL (creates
 *         an explicit gap / punches a hole in an existing range).
 *
 * Inserts or overwrites the entry for the given index range.  If the
 * range overlaps existing entries, those entries are split or replaced
 * as needed.  Adjacent slots with the same pointer value are coalesced
 * automatically.
 *
 * Nodes are split when full; the tree grows in height as required.
 * After erasure-style stores (entry == NULL), underfull nodes are
 * rebalanced (merged or redistributed with siblings) and the tree
 * height may shrink.
 *
 * Write-side locking is the caller's responsibility.
 *
 * Return: 0 on success, -EINVAL if @first > @last, -ENOMEM on
 *         allocation failure.
 */
int   mtree_store_range(struct maple_tree *mt, uint64_t first, uint64_t last,
                        void *entry);

/**
 * mtree_store - Store @entry at a single @index.
 * @mt:    Pointer to the maple tree.
 * @index: The index to store at.
 * @entry: The value to associate with @index.  May be NULL.
 *
 * Convenience wrapper around mtree_store_range() with first == last.
 *
 * Return: 0 on success, negative errno on failure.
 */
static inline int mtree_store(struct maple_tree *mt, uint64_t index,
                              void *entry)
{
    return mtree_store_range(mt, index, index, entry);
}

/**
 * mtree_erase - Remove the entry that covers @index.
 * @mt:    Pointer to the maple tree.
 * @index: An index within the entry's range.
 *
 * Removes the *entire* entry whose range contains @index — not just the
 * single index.  For example, if a range [10, 29] was stored and
 * mtree_erase(mt, 20) is called, the whole [10, 29] entry is removed.
 * To punch a single-index hole within a range, use
 * mtree_store_range(mt, idx, idx, NULL) instead.
 *
 * After removal, the affected leaf is coalesced and rebalanced if it
 * falls below the minimum occupancy threshold.  The tree height may
 * shrink if the root becomes a single-child internal node.
 *
 * Return: The previously stored entry (the void* pointer), or NULL if
 *         no entry covered @index.
 */
void *mtree_erase(struct maple_tree *mt, uint64_t index);

/**
 * mtree_erase_index - Remove a single index from the tree, preserving
 *                     the rest of the range.
 * @mt:    Pointer to the maple tree.
 * @index: The specific index to remove.
 *
 * Unlike mtree_erase(), which removes the *entire* entry whose range
 * covers @index, this function punches a single-index hole.  The
 * surrounding range entries are preserved.
 *
 * For example, if [10, 29] was stored and mtree_erase_index(mt, 20)
 * is called, the result is two entries: [10, 19] and [21, 29], each
 * retaining the original pointer value.  Index 20 becomes a gap.
 *
 * If @index is the only index in the entry (a point entry), the
 * behaviour is identical to mtree_erase().
 *
 * Return: The previously stored entry at @index, or NULL if no entry
 *         covered @index.
 */
void *mtree_erase_index(struct maple_tree *mt, uint64_t index);

/**
 * mtree_destroy - Free all nodes in the tree.
 * @mt: Pointer to the maple tree.
 *
 * Recursively frees every node.  After return the tree is empty
 * (mt_empty() returns true).  Safe to call on an already-empty tree.
 * Does **not** free the user-supplied entry pointers — only the
 * internal node structures.
 *
 * The caller must ensure no concurrent readers or writers are active.
 */
void  mtree_destroy(struct maple_tree *mt);

/* ====================================================================== */
/*  Cursor API                                                             */
/* ====================================================================== */

/**
 * @defgroup cursor Cursor (ma_state) API
 * @{
 *
 * The cursor API uses a stack-allocated `struct ma_state` to track
 * position within the tree.  The cursor now caches the full root-to-node
 * path in a fixed-size in-structure array rather than keeping only the
 * current node.  That makes sequential traversal much cheaper for
 * kernel-style workloads because mas_next() / mas_prev() can ascend and
 * descend using cached path frames instead of repeatedly reconstructing
 * bounds from parent pointers.
 *
 * The tradeoff is intentional: the cursor is larger on the stack, but it
 * avoids dynamic allocation and reduces per-step traversal overhead.
 * See struct ma_state in maple_tree_type.h for detailed field semantics,
 * especially the cached path frames and the meaning of depth.
 *
 * **Functions:**
 *   - MA_STATE()  — declare and initialise a cursor on the stack
 *   - mas_walk()  — descend to the slot covering mas->index
 *   - mas_store() — store an entry at [mas->index, mas->last]
 *   - mas_erase() — erase the entry covering mas->index
 *   - mas_find()  — find the next non-NULL entry at/after mas->index
 *   - mas_next()  — advance one entry forward
 *   - mas_prev()  — move one entry backward
 *
 * **Using together:**
 *
 * @code
 * // --- Walk + conditional update ---
 * MA_STATE(mas, &mt, target_idx, target_idx);
 * void *cur = mas_walk(&mas);
 * if (cur == stale_ptr)
 *     mas_store(&mas, new_ptr);       // overwrite in-place
 *
 * // --- Forward scan with cursor ---
 * MA_STATE(mas, &mt, 0, 0);
 * void *entry;
 * while ((entry = mas_find(&mas, UINT64_MAX)) != NULL) {
 *     printf("[%lu, %lu] -> %p\n", mas.index, mas.last, entry);
 * }
 *
 * // --- Backward scan ---
 * MA_STATE(mas, &mt, UINT64_MAX, UINT64_MAX);
 * mas_walk(&mas);                     // position at the end
 * void *entry;
 * while ((entry = mas_prev(&mas, 0)) != NULL) {
 *     // process entries in reverse
 * }
 *
 * // --- Cursor erase while iterating ---
 * MA_STATE(mas, &mt, 0, 0);
 * while ((entry = mas_find(&mas, 1000)) != NULL) {
 *     if (should_remove(entry))
 *         mas_erase(&mas);
 * }
 * @endcode
 *
 * @}
 */

/**
 * mas_walk - Walk the tree to the entry at mas->index.
 * @mas: Maple state cursor.  On entry, mas->index is the target index.
 *
 * Descends from the root to the leaf slot that covers mas->index.
 * On return, mas->node, mas->offset, mas->min, mas->max, mas->depth,
 * and the cached path frames are updated to reflect the located position.
 *
 * Return: The entry at that position, or NULL if it is a gap.
 */
void *mas_walk(struct ma_state *mas);

/**
 * mas_store - Store an entry at [mas->index, mas->last].
 * @mas:   Maple state cursor with index/last set to the target range.
 * @entry: The value to store.  May be NULL.
 *
 * Equivalent to mtree_store_range(mas->tree, mas->index, mas->last, entry).
 *
 * Return: 0 on success, negative errno on failure.
 */
int   mas_store(struct ma_state *mas, void *entry);

/**
 * mas_erase - Erase the entry at mas->index.
 * @mas: Maple state cursor.
 *
 * Equivalent to mtree_erase(mas->tree, mas->index).
 *
 * Return: The previously stored entry, or NULL.
 */
void *mas_erase(struct ma_state *mas);

/**
 * mas_find - Find the next non-NULL entry at or after mas->index, up to @max.
 * @mas: Maple state cursor.  On entry, mas->index is the search start.
 *       On return, mas->index is advanced past the found entry (to
 *       mas->max + 1) so that repeated calls iterate forward.
 * @max: Upper index bound (inclusive).
 *
 * If the cursor has not yet been walked (mas->node == NULL), an
 * implicit mas_walk() is performed first.
 *
 * Return: The next non-NULL entry, or NULL if none exists in
 *         [mas->index, @max].
 */
void *mas_find(struct ma_state *mas, uint64_t max);

/**
 * mas_next - Advance to the next non-NULL entry after the current position.
 * @mas: Maple state cursor, already positioned (e.g. via mas_walk).
 * @max: Upper index bound (inclusive).
 *
 * Moves forward one slot at a time, ascending and descending through the
 * cached path as needed.  Updates the current slot state plus the cached
 * path frames.
 *
 * Return: The next non-NULL entry, or NULL if none exists up to @max.
 */
void *mas_next(struct ma_state *mas, uint64_t max);

/**
 * mas_prev - Move to the previous non-NULL entry before the current position.
 * @mas: Maple state cursor, already positioned.
 * @min: Lower index bound (inclusive).
 *
 * Moves backward one slot at a time, ascending and descending through the
 * cached path as needed.
 *
 * Return: The previous non-NULL entry, or NULL if none exists down to @min.
 */
void *mas_prev(struct ma_state *mas, uint64_t min);

/* ====================================================================== */
/*  Gap search                                                             */
/* ====================================================================== */

/**
 * @defgroup gap Gap search
 * @{
 *
 * Gap-search functions locate contiguous runs of NULL entries (gaps)
 * within a specified index range.  They use the per-node gap metadata
 * maintained by the tree to search in O(log n) rather than scanning
 * every slot.
 *
 * These are the workhorses behind VM-style address-space allocators:
 * find a free region of the requested size and return its bounds.
 *
 * **Functions:**
 *   - mas_empty_area()     — lowest-address first-fit gap search
 *   - mas_empty_area_rev() — highest-address first-fit gap search
 *
 * **Using together:**
 *
 * @code
 * // --- Simple first-fit allocator ---
 * MA_STATE(mas, &mt, 0, 0);
 * int ret = mas_empty_area(&mas, 0, UINT64_MAX, page_count);
 * if (ret == 0) {
 *     // Found: gap at [mas.index, mas.last] (size >= page_count)
 *     uint64_t alloc_start = mas.index;
 *     mtree_store_range(&mt, alloc_start,
 *                       alloc_start + page_count - 1, region);
 * }
 *
 * // --- Top-down (reverse) allocator ---
 * MA_STATE(mas, &mt, 0, 0);
 * ret = mas_empty_area_rev(&mas, 0, UINT64_MAX, page_count);
 * if (ret == 0) {
 *     uint64_t alloc_start = mas.index;
 *     mtree_store_range(&mt, alloc_start,
 *                       alloc_start + page_count - 1, region);
 * }
 *
 * // --- Free + gap reclaim ---
 * mtree_store_range(&mt, alloc_start,
 *                   alloc_start + page_count - 1, NULL);  // free
 * // Subsequent gap searches will find this region again.
 * @endcode
 *
 * @}
 */

/**
 * mas_empty_area - Find the first (lowest-address) gap of at least @size.
 * @mas:  Maple state cursor.  On success, mas->index and mas->last are
 *        set to the found gap range [start, start + size - 1].
 * @min:  Minimum index to consider (inclusive).
 * @max:  Maximum index to consider (inclusive).
 * @size: Required gap size (number of contiguous NULL indices).
 *        Must be > 0.
 *
 * Searches forward through the tree for the first contiguous run of
 * @size NULL entries within [@min, @max].  Uses gap tracking in
 * internal nodes for O(log n) search when available.
 *
 * Return: 0 on success, -EBUSY if no suitable gap exists.
 */
int mas_empty_area(struct ma_state *mas, uint64_t min, uint64_t max,
                   uint64_t size);

/**
 * mas_empty_area_rev - Find the last (highest-address) gap of at least @size.
 * @mas:  Maple state cursor.  On success, mas->index and mas->last are
 *        set to the found gap range.
 * @min:  Minimum index to consider (inclusive).
 * @max:  Maximum index to consider (inclusive).
 * @size: Required gap size.  Must be > 0.
 *
 * Like mas_empty_area() but searches backward, returning the
 * highest-address gap that satisfies the size requirement.
 *
 * Return: 0 on success, -EBUSY if no suitable gap exists.
 */
int mas_empty_area_rev(struct ma_state *mas, uint64_t min, uint64_t max,
                       uint64_t size);

/* ====================================================================== */
/*  RCU read-side helpers                                                  */
/* ====================================================================== */

/**
 * @defgroup rcu_read RCU read-side traversal
 * @{
 *
 * These functions provide a convenient, self-contained read path that
 * acquires the RCU read lock internally (when MT_CONFIG_RCU is
 * enabled).  They are ideal for one-shot lookups or forward/backward
 * neighbour queries from reader threads that coexist with writers.
 *
 * Without MT_CONFIG_RCU, the RCU lock calls compile to no-ops and
 * these functions behave identically to cursor-based lookups — but
 * the caller must still serialise against concurrent writes via an
 * external lock.
 *
 * **Functions:**
 *   - mt_find() — find next non-NULL entry at/after *index
 *   - mt_next() — entry strictly after index
 *   - mt_prev() — entry strictly before index
 *
 * **Using together:**
 *
 * @code
 * // --- Iterate all entries with mt_find ---
 * uint64_t idx = 0;
 * void *entry;
 * while ((entry = mt_find(&mt, &idx, UINT64_MAX)) != NULL)
 *     process(entry);        // idx is auto-advanced past each entry
 *
 * // --- Get neighbours ---
 * void *next = mt_next(&mt, current_idx, UINT64_MAX);
 * void *prev = mt_prev(&mt, current_idx, 0);
 *
 * // --- Bounded search ---
 * // Find the first entry in [1000, 2000]:
 * uint64_t start = 1000;
 * void *e = mt_find(&mt, &start, 2000);
 * @endcode
 *
 * @}
 */

/**
 * mt_find - Find the next non-NULL entry at or after *@index, up to @max.
 * @mt:    Pointer to the maple tree.
 * @index: Pointer to the search start index.  Updated on success to
 *         one past the end of the found entry's range (so that
 *         repeated calls iterate all entries).  Set to MAPLE_MAX if
 *         the entry extends to the end of the index space.
 * @max:   Upper index bound (inclusive).
 *
 * Acquires and releases the RCU read lock internally (when
 * MT_CONFIG_RCU is enabled).
 *
 * Return: The found entry, or NULL if no non-NULL entry exists in
 *         [*@index, @max].
 */
void *mt_find(struct maple_tree *mt, uint64_t *index, uint64_t max);

/**
 * mt_next - Return the next non-NULL entry strictly after @index.
 * @mt:    Pointer to the maple tree.
 * @index: The index to search after (exclusive).
 * @max:   Upper index bound (inclusive).
 *
 * Return: The next entry, or NULL if @index >= @max or no entry exists
 *         in (@index, @max].
 */
void *mt_next(struct maple_tree *mt, uint64_t index, uint64_t max);

/**
 * mt_prev - Return the previous non-NULL entry strictly before @index.
 * @mt:    Pointer to the maple tree.
 * @index: The index to search before (exclusive).
 * @min:   Lower index bound (inclusive).
 *
 * Return: The previous entry, or NULL if @index <= @min or no entry
 *         exists in [@min, @index).
 */
void *mt_prev(struct maple_tree *mt, uint64_t index, uint64_t min);

/* ====================================================================== */
/*  Debug                                                                  */
/* ====================================================================== */

/**
 * @defgroup debug Debug
 * @{
 *
 * Diagnostic output for inspecting the internal tree structure.
 * Useful during development and when writing tests — not intended
 * for production use.
 *
 * **Functions:**
 *   - mt_dump_tree() — print every node to stdout
 *
 * **Using together:**
 *
 * @code
 * mtree_store_range(&mt, 0, 99, region);
 * mtree_store(&mt, 200, single);
 * mt_dump_tree(&mt);   // shows root, node type, pivots, entries
 * @endcode
 *
 * @}
 */

/**
 * mt_dump_tree - Print the tree structure to stdout for debugging.
 * @mt: Pointer to the maple tree.
 *
 * Prints every node, its type (LEAF / INTERNAL), range, parent info,
 * slot contents, and raw pivots.  Output goes to stdout via printf().
 */
void mt_dump_tree(struct maple_tree *mt);

/* ====================================================================== */
/*  Locked convenience wrappers                                            */
/* ====================================================================== */

/**
 * @defgroup locked_wrappers Locked convenience wrappers
 * @{
 *
 * The mtree_lock_* family acquires the tree's internal lock (when
 * MT_CONFIG_LOCK is defined) around each operation, providing a fully
 * serialised interface safe for concurrent use from multiple threads
 * without any external synchronisation.
 *
 * Without MT_CONFIG_LOCK the lock calls compile to no-ops, so these
 * wrappers remain usable (and equivalent to the plain API).
 *
 * For callers that need to batch several operations atomically, use
 * mt_lock() / mt_unlock() manually around the plain API instead.
 *
 * **Functions:**
 *   - mtree_lock_store_range() — locked version of mtree_store_range()
 *   - mtree_lock_store()       — locked version of mtree_store()
 *   - mtree_lock_load()        — locked version of mtree_load()
 *   - mtree_lock_erase()       — locked version of mtree_erase()
 *   - mtree_lock_destroy()     — locked version of mtree_destroy()
 *
 * **Using together:**
 *
 * @code
 * // Each call is independently thread-safe:
 * mtree_lock_store(&mt, 1, a);
 * mtree_lock_store(&mt, 2, b);
 * void *v = mtree_lock_load(&mt, 1);
 * mtree_lock_erase(&mt, 2);
 *
 * // When no more threads are active:
 * mtree_lock_destroy(&mt);
 *
 * // For atomic batches, drop down to the plain API:
 * mt_lock(&mt);
 * mtree_store(&mt, 10, x);
 * mtree_store(&mt, 11, y);
 * mt_unlock(&mt);
 * @endcode
 *
 * @}
 */

/**
 * mtree_lock_store_range - Locked version of mtree_store_range().
 */
static inline int mtree_lock_store_range(struct maple_tree *mt,
                                         uint64_t first, uint64_t last,
                                         void *entry)
{
    mt_lock(mt);
    int ret = mtree_store_range(mt, first, last, entry);
    mt_unlock(mt);
    return ret;
}

/**
 * mtree_lock_store - Locked version of mtree_store().
 */
static inline int mtree_lock_store(struct maple_tree *mt, uint64_t index,
                                   void *entry)
{
    return mtree_lock_store_range(mt, index, index, entry);
}

/**
 * mtree_lock_load - Locked version of mtree_load().
 */
static inline void *mtree_lock_load(struct maple_tree *mt, uint64_t index)
{
    mt_lock(mt);
    void *entry = mtree_load(mt, index);
    mt_unlock(mt);
    return entry;
}

/**
 * mtree_lock_erase - Locked version of mtree_erase().
 */
static inline void *mtree_lock_erase(struct maple_tree *mt, uint64_t index)
{
    mt_lock(mt);
    void *entry = mtree_erase(mt, index);
    mt_unlock(mt);
    return entry;
}

/**
 * mtree_lock_erase_index - Locked version of mtree_erase_index().
 */
static inline void *mtree_lock_erase_index(struct maple_tree *mt,
                                           uint64_t index)
{
    mt_lock(mt);
    void *entry = mtree_erase_index(mt, index);
    mt_unlock(mt);
    return entry;
}

/**
 * mtree_lock_destroy - Locked version of mtree_destroy().
 *
 * Acquires the lock, destroys all nodes, then unlocks.
 */
static inline void mtree_lock_destroy(struct maple_tree *mt)
{
    mt_lock(mt);
    mtree_destroy(mt);
    mt_unlock(mt);
}

/* ====================================================================== */
/*  Iteration macros                                                       */
/* ====================================================================== */

/**
 * @defgroup iteration Iteration macros
 * @{
 *
 * Convenience macros that wrap the find/cursor functions into
 * standard C for/while loops.
 *
 * **Macros:**
 *   - mt_for_each()  — iterate all non-NULL entries using mt_find()
 *   - mas_for_each() — iterate all non-NULL entries using mas_find()
 *
 * **Using together:**
 *
 * @code
 * // --- mt_for_each: simple full-tree scan ---
 * uint64_t idx = 0;
 * void *entry;
 * mt_for_each(&mt, entry, idx, UINT64_MAX) {
 *     printf("entry at range ending before idx=%lu: %p\n", idx, entry);
 * }
 *
 * // --- mas_for_each: cursor-based scan with optional early exit ---
 * MA_STATE(mas, &mt, 100, 0);     // start scanning from index 100
 * void *entry;
 * mas_for_each(&mas, entry, 999) {
 *     if (some_condition(entry))
 *         break;                   // cursor remembers position
 * }
 * // After break, mas.index/mas.last still reflect the last entry.
 *
 * // --- Bounded range dump ---
 * uint64_t idx = 500;
 * mt_for_each(&mt, entry, idx, 1000) {
 *     handle(entry);              // only entries in [500, 1000]
 * }
 * @endcode
 *
 * @}
 */

/**
 * mt_for_each - Iterate over all non-NULL entries in [0, @__max].
 * @__mt:    Pointer to the maple tree.
 * @__entry: Loop variable receiving each non-NULL entry (void *).
 * @__index: uint64_t variable used internally; after each iteration
 *           holds the next search index (one past the current entry).
 * @__max:   Upper index bound (inclusive).
 *
 * Uses mt_find() internally, so the RCU read lock is acquired and
 * released on each iteration when MT_CONFIG_RCU is enabled.
 *
 * Example:
 *   uint64_t idx = 0;
 *   void *entry;
 *   mt_for_each(&mt, entry, idx, MAPLE_MAX) {
 *       // process entry
 *   }
 */
#define mt_for_each(__mt, __entry, __index, __max)                          \
    for ((__index) = 0,                                                     \
         (__entry) = mt_find((__mt), &(__index), (__max));                   \
         (__entry) != NULL;                                                 \
         (__entry) = mt_find((__mt), &(__index), (__max)))

/**
 * mas_for_each - Iterate entries forward from the current cursor position.
 * @__mas:   Pointer to a struct ma_state (cursor).
 * @__entry: Loop variable receiving each non-NULL entry (void *).
 * @__max:   Upper index bound (inclusive).
 *
 * Calls mas_find() repeatedly.  The cursor must be initialised (e.g.
 * via MA_STATE()) before the first invocation.  The cursor is advanced
 * past each entry automatically.
 *
 * Example:
 *   MA_STATE(mas, &mt, 0, 0);
 *   void *entry;
 *   mas_for_each(&mas, entry, MAPLE_MAX) {
 *       // process entry
 *   }
 */
#define mas_for_each(__mas, __entry, __max)                                 \
    while (((__entry) = mas_find((__mas), (__max))) != NULL)

#endif /* MAPLE_TREE_H */
