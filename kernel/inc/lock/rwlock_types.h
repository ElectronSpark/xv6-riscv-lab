/**
 * @file rwlock_types.h
 * @brief Forward-declaration of @c struct rwlock and related constants.
 *
 * Separated from rwlock.h so that headers needing the type (but not the
 * inline lock operations) can include this without pulling in the full
 * atomic/timer machinery.
 */

#ifndef __KERNEL_READ_WRITE_SPIN_LOCK_TYPES_H
#define __KERNEL_READ_WRITE_SPIN_LOCK_TYPES_H

#include "compiler.h"
#include "types.h"

/**
 * @brief Read-write spinlock.
 *
 * All locking state is encoded in the @c state word; see rwlock.h for the
 * bit-field layout.  @c w_holder is an auxiliary field used solely for
 * ownership assertions and write→read recursion checks.
 *
 * Aligned to a cache line to avoid false sharing with adjacent data.
 */
struct rwlock {
    /** Packed lock state: reader count (bits 9-63), writer-waiting hint
     *  (bit 8), and writer-holding field (bits 0-7).  Zero means unlocked. */
    _Atomic uint64 state;
    /** CPU-id of the current write holder, or @c RWLOCK_NONE_HOLDER (-1). */
    _Atomic int w_holder;
    /** Human-readable name for diagnostics / panic messages. */
    const char *name;
} __ALIGNED_CACHELINE;

/** Sentinel: no CPU holds the write lock. */
#define RWLOCK_NONE_HOLDER -1

/** Compile-time initializer (for file-scope / static rwlocks). */
#define RWLOCK_INITIALIZER(name)                                               \
    {.state = RWLOCK_STATE_UNLOCKED,                                           \
     .w_holder = RWLOCK_NONE_HOLDER,                                           \
     .name = name}

/** Alias for @ref RWLOCK_INITIALIZER (legacy compatibility). */
#define RWLOCK_INITIALIZED(name) RWLOCK_INITIALIZER(name)

#endif // __KERNEL_READ_WRITE_SPIN_LOCK_TYPES_H
