/**
 * @file xarray_type.h
 * @brief XArray type definitions — Linux-style radix tree.
 *
 * The XArray is an abstract data type which behaves like a very large array
 * of pointers.  It meets many of the same needs as a hash or a conventional
 * resizable array.  Unlike a hash, it allows you to sensibly go to the next
 * or previous entry in a cache-efficient manner.  It supports marks, allowing
 * you to tag entries (e.g. dirty, LRU) and iterate over only marked entries.
 *
 * Simplified from the Linux kernel xarray (6.x):
 *  - 64 slots per node (6-bit chunk), matching 64-bit unsigned long keys.
 *  - 3 independent mark bitmaps per node for tagging entries.
 *  - Optional locking and RCU support via preprocessor configuration.
 */

#ifndef XARRAY_TYPE_H
#define XARRAY_TYPE_H

#include "xarray_config.h"

/* ----------------------------------------------------------------------- */
/*  Constants                                                               */
/* ----------------------------------------------------------------------- */

/**
 * XA_CHUNK_SHIFT - Number of bits consumed per tree level.
 *
 * Each xa_node has 2^XA_CHUNK_SHIFT slots.  With a 6-bit shift we get
 * 64 slots per node, and at most ceil(64/6) = 11 levels for a full
 * 64-bit key space.
 */
#define XA_CHUNK_SHIFT  6
#define XA_CHUNK_SIZE   (1UL << XA_CHUNK_SHIFT)
#define XA_CHUNK_MASK   (XA_CHUNK_SIZE - 1)

/** Maximum number of marks supported per entry. */
#define XA_MAX_MARKS    3

/* xa_flags bit range reserved for single-entry head marks. */
#define XA_HEAD_MARK_SHIFT 29
#define XA_HEAD_MARK_MASK  ((((1U << XA_MAX_MARKS) - 1U) << XA_HEAD_MARK_SHIFT))

/* ----------------------------------------------------------------------- */
/*  Mark types                                                              */
/* ----------------------------------------------------------------------- */

/**
 * xa_mark_t - Type-safe mark identifier.
 *
 * XA_MARK_0, XA_MARK_1, XA_MARK_2 can be used to tag entries and
 * then efficiently iterate over only tagged entries.
 */
typedef unsigned int xa_mark_t;

#define XA_MARK_0       ((xa_mark_t)0)
#define XA_MARK_1       ((xa_mark_t)1)
#define XA_MARK_2       ((xa_mark_t)2)

/* ----------------------------------------------------------------------- */
/*  Internal entry encoding                                                 */
/* ----------------------------------------------------------------------- */

/*
 * We steal the lowest two bits of pointers stored in slots to encode
 * internal node pointers vs user entries:
 *
 *  bit 0 = 1: internal xa_node pointer (shifted by 2)
 *  bit 0 = 0, bit 1 = 1: value entry (integer stored as pointer)
 *  bit 0 = 0, bit 1 = 0: plain pointer entry (user data)
 *
 * Special sentinel values:
 *  XA_RETRY_ENTRY  - slot is being modified under RCU; reader should retry
 *  XA_ZERO_ENTRY   - placeholder that keeps a node alive but represents NULL
 */
#define XA_INTERNAL_FLAG    1UL
#define XA_VALUE_FLAG       2UL
#define XA_IS_NODE_FLAG     XA_INTERNAL_FLAG

/** Encode a pointer to an xa_node as an internal slot entry. */
#define xa_mk_internal(node) \
    ((void *)((uintptr_t)(node) | XA_INTERNAL_FLAG))

/** Decode an internal slot entry back to an xa_node pointer. */
#define xa_to_internal(entry) \
    ((struct xa_node *)((uintptr_t)(entry) & ~XA_INTERNAL_FLAG))

/** Check whether a slot entry is an internal (xa_node) pointer. */
#define xa_is_internal(entry) \
    (((uintptr_t)(entry) & 3) == XA_INTERNAL_FLAG)

/** Encode an integer value as a value entry. */
#define xa_mk_value(v)      ((void *)(((uintptr_t)(v) << 2) | XA_VALUE_FLAG))
/** Decode a value entry back to an integer. */
#define xa_to_value(entry)  ((uintptr_t)(entry) >> 2)
/** Check whether a slot entry is a value entry. */
#define xa_is_value(entry)  (((uintptr_t)(entry) & 3) == XA_VALUE_FLAG)

/** Check whether a slot entry is a plain pointer (user data). */
#define xa_is_pointer(entry) \
    ((entry) != NULL && !xa_is_internal(entry) && !xa_is_value(entry))

/*
 * Sentinel entries.  These use the internal flag + high-bit patterns
 * so they can never collide with real node pointers or user data.
 */
#define XA_RETRY_ENTRY      xa_mk_internal((void *)0x100)
#define XA_ZERO_ENTRY       xa_mk_internal((void *)0x200)

/** Sibling entry: points to the canonical slot within the same node. */
#define xa_mk_sibling(offset)   xa_mk_internal((void *)((uintptr_t)(offset) << 2))
#define xa_to_sibling(entry)    ((uint8_t)((uintptr_t)xa_to_internal(entry) >> 2))
#define xa_is_sibling(entry)    (xa_is_internal(entry) && \
    (uintptr_t)xa_to_internal(entry) >= 4 && \
    (uintptr_t)xa_to_internal(entry) < (XA_CHUNK_SIZE << 2))

/** Check if entry is a real user entry (not NULL, not internal sentinel). */
static inline bool xa_is_entry(const void *entry)
{
    return entry != NULL &&
           entry != XA_RETRY_ENTRY &&
           entry != XA_ZERO_ENTRY &&
           !xa_is_internal(entry);
}

/* Number of uint64_t needed to hold XA_CHUNK_SIZE bits. */
#define XA_MARK_LONGS   ((XA_CHUNK_SIZE + 63) / 64)

/* ----------------------------------------------------------------------- */
/*  Node structure                                                          */
/* ----------------------------------------------------------------------- */

/**
 * struct xa_node - Internal radix tree node.
 *
 * @shift:    Number of bits remaining below this node.  A leaf node has
 *            shift == 0.  The root node has the largest shift.
 * @offset:   This node's slot index within its parent.
 * @count:    Number of non-NULL slots (entries + child nodes).
 * @nr_values: Number of value entries (xa_is_value) in this node.
 * @parent:   Pointer to the parent node (NULL for the root's child).
 * @array:    Pointer back to the owning xarray.
 * @slots:    Child/entry pointers — 64 slots.
 * @marks:    Per-mark bitmaps — marks[m] has bit i set iff slot[i]
 *            (or any descendant through slot[i]) has mark m set.
 *
 * Nodes are allocated via xa_alloc_fn and freed via xa_free_fn.
 */
struct xa_node {
    uint8_t  shift;
    uint8_t  offset;
    uint8_t  count;
    uint8_t  nr_values;
    struct xa_node *parent;
    struct xarray  *array;
    void   *slots[XA_CHUNK_SIZE];
    uint64_t marks[XA_MAX_MARKS][XA_MARK_LONGS];
    /* Embedded RCU head used by the kernel-side xa_call_rcu trampoline. */
    rcu_head_t rcu_head;
};

/* ----------------------------------------------------------------------- */
/*  Root structure                                                          */
/* ----------------------------------------------------------------------- */

/**
 * struct xarray - The XArray root.
 *
 * @xa_flags: User flags plus reserved bits for single-entry head marks.
 * @xa_head:  Root slot — either NULL (empty), a single user entry
 *            (one-entry optimisation), or an xa_mk_internal(node)
 *            pointing to the top-level xa_node.
 *
 * When XA_CONFIG_LOCK is enabled, xa_lock is included for write-side
 * protection.  Readers using xa_load() can proceed lock-free when
 * XA_CONFIG_RCU is enabled.
 */
struct xarray {
#ifdef XA_CONFIG_LOCK
    xa_lock_t   xa_lock;
#endif
    unsigned int xa_flags;
    void        *xa_head;
};

/* ----------------------------------------------------------------------- */
/*  Static initialisers                                                     */
/* ----------------------------------------------------------------------- */

#ifdef XA_CONFIG_LOCK
#define __XA_LOCK_INIT(name) .xa_lock = XA_LOCK_INITIALIZER(name),
#else
#define __XA_LOCK_INIT(name)
#endif

#define XARRAY_INIT(name, flags) {                              \
    __XA_LOCK_INIT(name)                                        \
    .xa_flags = (flags),                                        \
    .xa_head  = NULL,                                           \
}

#define DEFINE_XARRAY_FLAGS(name, flags)                        \
    struct xarray name = XARRAY_INIT(name, flags)

#define DEFINE_XARRAY(name) DEFINE_XARRAY_FLAGS(name, 0)

/* ----------------------------------------------------------------------- */
/*  Cursor (xa_state) structure                                             */
/* ----------------------------------------------------------------------- */

/**
 * struct xa_state - XArray operation state / cursor.
 *
 * @xa:        Pointer to the xarray being operated on.
 * @xa_index:  The index being looked up or stored.
 * @xa_shift:  Shift of the current node (cached to avoid re-walking).
 * @xa_sibs:   Number of sibling entries (for multi-index entries).
 * @xa_offset: Slot offset within the current node.
 * @xa_node:   Current node (NULL = haven't descended yet, or at head).
 * @xa_error:  Sticky error status (0 or -errno).
 *
 * Instantiate on the stack with XA_STATE().
 */
struct xa_state {
    struct xarray  *xa;
    uint64_t        xa_index;
    uint8_t         xa_shift;
    uint8_t         xa_sibs;
    uint8_t         xa_offset;
    struct xa_node *xa_node;
    int             xa_error;
};

/**
 * XA_STATE - Declare and initialise an xa_state on the stack.
 * @name:  Variable name.
 * @array: Pointer to the struct xarray.
 * @index: Initial index.
 */
#define XA_STATE(name, array, index) \
    struct xa_state name = {         \
        .xa        = (array),        \
        .xa_index  = (index),        \
        .xa_shift  = 0,              \
        .xa_sibs   = 0,              \
        .xa_offset = 0,              \
        .xa_node   = XAS_RESTART,    \
        .xa_error  = 0,              \
    }

/* Special xa_node values used in xa_state. */
#define XAS_RESTART  ((struct xa_node *)0x1UL)   /* Walk not started */
#define XAS_ERROR    ((struct xa_node *)0x3UL)   /* Error state */
#define XAS_BOUNDS   ((struct xa_node *)0x5UL)   /* Out of bounds */

/** Check if xa_state is in an error/special state. */
static inline bool xas_is_special(const struct xa_state *xas)
{
    return (uintptr_t)xas->xa_node <= (uintptr_t)XAS_BOUNDS;
}

/** Check if xa_state indicates an error. */
static inline bool xas_is_error(const struct xa_state *xas)
{
    return xas->xa_node == XAS_ERROR;
}

/** Check if xa_state needs to restart the walk. */
static inline bool xas_is_restart(const struct xa_state *xas)
{
    return xas->xa_node == XAS_RESTART;
}

/** Retrieve the error code from the xa_state. */
static inline int xas_error(const struct xa_state *xas)
{
    if (xas_is_error(xas))
        return xas->xa_error;
    return 0;
}

#endif /* XARRAY_TYPE_H */
