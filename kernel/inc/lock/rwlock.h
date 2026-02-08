/**
 * @file rwlock.h
 * @brief Read-write spin lock header.
 *
 * A Linux-inspired read-write spin lock whose entire state is encoded in a
 * single 64-bit atomic word.  The encoding allows readers and writers to be
 * arbitrated with a single compare-and-swap (CAS) per acquisition attempt,
 * avoiding an auxiliary spinlock.
 *
 * @section state_layout  State Layout (64-bit)
 *
 * @code
 *  63            9   8     7         0
 * +---------------+---+----------------+
 * | reader count  | W |  writer hold   |
 * +---------------+---+----------------+
 *        55 bits   1b       8 bits
 * @endcode
 *
 * - **Bits  0-7  (WRITER_HOLDING):** Set to 0xFF when a writer holds the lock,
 *   0x00 otherwise.  Using a full byte (rather than a single bit) is
 *   intentional so that a writer-held state is never confused with a non-zero
 *   reader count shifted into the same field.
 * - **Bit   8    (WRITER_WAITING):** A *soft hint* set by a spinning writer
 *   that has reached the expedite threshold.  When this bit is set, new
 *   non-expediting readers and non-expediting writers voluntarily back off,
 *   giving that writer priority.  The bit may be transiently lost on unlock
 *   (see @ref rwlock_writer_release) — this is acceptable because the waiting writer
 *   will re-set it on its next CAS-failure iteration.
 * - **Bits 9-63 (reader count):** Each reader adds @c RWLOCK_STATE_READER_BIAS
 *   (1 << 9) when acquiring, allowing up to 2^55 concurrent readers.
 *
 * @section write_recur  Write-to-Read Recursion
 *
 * A thread that already holds the write lock may additionally acquire a read
 * lock (@ref rwlock_try_rlock checks @c RWLOCK_W_HOLDING).  The reverse
 * (read → write) requires an explicit upgrade via @ref rwlock_try_update,
 * which succeeds only when the caller is the *sole* reader.
 *
 * @section expedite  Writer Starvation Prevention (Expedite)
 *
 * To avoid indefinite writer starvation under read-heavy workloads the
 * blocking @ref rwlock_wacquire path enables *expedite mode* after
 * @c RWLOCK_EXPEDITE_THRESHOLD ticks.  In expedite mode:
 *   -# The failure-hook atomically ORs @c WRITER_WAITING into the state.
 *   -# Subsequent @ref rwlock_can_rlock / @ref rwlock_can_wlock callers
 *      without expedite refuse to acquire, yielding to the waiting writer.
 *
 * Two pre-packaged variants skip the timeout:
 * - @ref rwlock_wacquire_expedited — always expedites (low latency, aggressive).
 * - @ref rwlock_graceful_wacquire  — never expedites (fair, may starve).
 */

#ifndef __KERNEL_READ_WRITE_SPIN_LOCK_H
#define __KERNEL_READ_WRITE_SPIN_LOCK_H

#include "rwlock_types.h"
#include "smp/atomic.h"
#include "timer/timer.h"
#include "smp/percpu.h"
#include "riscv.h"

/** @name State constants
 * @{ */

/** Fully-unlocked state — no readers, no writer, no WRITER_WAITING hint. */
#define RWLOCK_STATE_UNLOCKED 0

/**
 * @brief Bit 8 — "a writer is waiting" soft hint.
 *
 * Set by the expedite failure-hook; cleared implicitly when the writer
 * acquires (CAS replaces state with @c WRITER_HOLDING) or when
 * @ref rwlock_writer_release stores @c UNLOCKED.
 */
#define RWLOCK_STATE_WRITER_WAITING (1ULL << 8)

/**
 * @brief Bits 0-7 all set (0xFF) — "a writer holds the lock".
 *
 * The new-value operand of the CAS in @ref rwlock_try_wlock /
 * @ref rwlock_try_update.  Because the entire lower 9 bits become
 * 0xFF the WRITER_WAITING hint is implicitly cleared on acquisition.
 */
#define RWLOCK_STATE_WRITER_HOLDING ((1ULL << 8) - 1)

/** Mask covering both writer-holding and writer-waiting bits (bits 0-8). */
#define RWLOCK_STATE_WRITER_MASK                                               \
    (RWLOCK_STATE_WRITER_WAITING | RWLOCK_STATE_WRITER_HOLDING)

/** Bit position at which the reader count field begins (bit 9). */
#define RWLOCK_STATE_READER_BIAS_SHIFT 9

/** Value added/subtracted for each reader (1 << 9). */
#define RWLOCK_STATE_READER_BIAS (1ULL << RWLOCK_STATE_READER_BIAS_SHIFT)

/** @} */

/** @name State-extraction helpers (operate on a raw @c uint64 snapshot)
 * @{ */

/** Non-zero if a writer holds the lock (bits 0-7 == 0xFF). */
#define RWLOCK_STATE_W_HOLDING(state) ((state) & RWLOCK_STATE_WRITER_HOLDING)

/** Non-zero if the WRITER_WAITING hint is set (bit 8). */
#define RWLOCK_STATE_W_WAITING(state) ((state) & RWLOCK_STATE_WRITER_WAITING)

/** Number of readers currently holding the lock (bits 9-63). */
#define RWLOCK_STATE_R_COUNT(state) ((state) >> RWLOCK_STATE_READER_BIAS_SHIFT)

/**
 * True when no writer holds and no readers hold — ignoring the
 * WRITER_WAITING hint (which is merely advisory).
 */
#define RWLOCK_STATE_IS_UNLOCKED(state)                                        \
    (((state) & ~RWLOCK_STATE_WRITER_WAITING) == RWLOCK_STATE_UNLOCKED)

/** @} */

/** @name Live-lock queries (atomically load from @c struct rwlock *)
 * @{ */

/** Atomically load the full 64-bit state word with acquire semantics. */
#define RWLOCK_STATE(rw) smp_load_acquire(&(rw)->state)

/** Read the CPU-id of the current write holder (or @c RWLOCK_NONE_HOLDER). */
#define RWLOCK_W_HOLDER(rw) (smp_load_acquire(&(rw)->w_holder))

/** True if the *calling* CPU currently holds the write lock. */
#define RWLOCK_W_HOLDING(rw) (cpuid() == RWLOCK_W_HOLDER(rw))

/** True if the WRITER_WAITING hint is currently set. */
#define RWLOCK_W_WAITING(rw) RWLOCK_STATE_W_WAITING(RWLOCK_STATE(rw))

/** True if a writer currently holds the lock. */
#define RWLOCK_W_LOCKED(rw) RWLOCK_STATE_W_HOLDING(RWLOCK_STATE(rw))

/** True if no writer holds and no readers hold (ignores WRITER_WAITING). */
#define RWLOCK_UNLOCKED(rw) RWLOCK_STATE_IS_UNLOCKED(RWLOCK_STATE(rw))

/** Number of readers currently holding the lock. */
#define RWLOCK_R_COUNT(rw) RWLOCK_STATE_R_COUNT(RWLOCK_STATE(rw))

/** @} */

/**
 * @brief Threshold for expediting writers in rwlock_wacquire()
 * If a writer has been waiting for longer than this threshold, new readers and
 * writers not reaching this threshold will not acquire the lock, allowing the
 * waiting writer to acquire it sooner. This helps prevent writer starvation in
 * read-heavy workloads.
 * */
#define RWLOCK_EXPEDITE_THRESHOLD (TICK_MS << 2) // 4ms

/**
 * @brief Predicate: can the current thread acquire a read lock given @p state?
 *
 * Called inside the CAS loop of @ref rwlock_try_rlock where @p state is the
 * loop-local @c VAL (the most recent CAS-observed snapshot), avoiding a
 * second load and the associated TOCTOU window.
 *
 * @param rw     Pointer to the rwlock (used only to check write ownership).
 * @param state  A snapshot of @c rw->state obtained by the CAS loop.
 * @return @c true if a read lock may be acquired:
 *   - No writer bits set at all (common fast path), **or**
 *   - Writer bits are set *and* the calling CPU is the writer (write → read
 *     recursion is allowed).
 */
static inline bool rwlock_can_rlock(struct rwlock *rw, uint64 state) {
    if (state & RWLOCK_STATE_WRITER_MASK) {
        return RWLOCK_W_HOLDING(rw);
    }
    return true; // No writers — readers can acquire
}

/**
 * @brief Non-blocking read-lock acquisition.
 *
 * Performs a single CAS attempt:
 *   - **Condition:** @ref rwlock_can_rlock(rw, VAL)
 *   - **New value:** @c VAL + READER_BIAS  (increment reader count while
 *     preserving any existing writer bits — important for write → read
 *     recursion).
 *
 * @param rw  Pointer to the rwlock.
 * @return @c true if the read lock was acquired, @c false otherwise.
 */
static inline bool rwlock_try_rlock(struct rwlock *rw) {
    return atomic_oper_cond(&rw->state, (VAL + RWLOCK_STATE_READER_BIAS),
                            rwlock_can_rlock(rw, VAL));
}

/**
 * @brief Predicate: can the current thread acquire a write lock given @p state?
 *
 * Called inside the CAS loop of @ref rwlock_try_wlock.  @p state is the
 * loop-local @c VAL snapshot.
 *
 * @param state    CAS-observed snapshot of @c rw->state.
 * @param expedite If @c true, the caller ignores the WRITER_WAITING hint
 *                 set by another writer, allowing it to bypass the
 *                 soft-priority mechanism.
 * @return @c true if a write lock may be acquired:
 *   - No readers present, **and**
 *   - No writer currently holding, **and**
 *   - Either no WRITER_WAITING hint or @p expedite is set.
 */
static inline bool rwlock_can_wlock(uint64 state, bool expedite) {
    if (RWLOCK_STATE_R_COUNT(state) > 0) {
        return false; // Readers present — can't acquire write lock
    }
    if (RWLOCK_STATE_W_HOLDING(state)) {
        return false; // Another writer holds the lock
    } else if (RWLOCK_STATE_W_WAITING(state) && !expedite) {
        return false; // Another writer is waiting and we're not expediting
    }
    return true; // No readers or writers, or we're expediting past a waiting
                 // writer
}

/**
 * @brief CAS failure-hook: set the WRITER_WAITING hint if not already set.
 *
 * Invoked by @c atomic_oper_cond_hook on every failed CAS iteration inside
 * @ref rwlock_try_wlock.  If the caller is in expedite mode and the hint
 * bit is not yet set, this atomically ORs @c WRITER_WAITING into the state.
 * The @c atomic_or is idempotent; concurrent setting by multiple writers is
 * harmless.
 *
 * @note @p VAL here is the CAS-updated snapshot (the value that caused the
 *       CAS to fail), so the check is against fresh state.
 *
 * @param state_ptr  Pointer to @c rw->state.
 * @param VAL        Current snapshot of the state (from the CAS failure).
 * @param expedite   Whether the caller is in expedite mode.
 */
#define __rwlock_expedite_hook(state_ptr, VAL, expedite)                       \
    do {                                                                       \
        if (!RWLOCK_STATE_W_WAITING(VAL) && expedite) {                        \
            atomic_or(state_ptr, RWLOCK_STATE_WRITER_WAITING);                 \
        }                                                                      \
    } while (0)

/**
 * @brief Non-blocking write-lock acquisition.
 *
 * Performs a CAS-retry loop via @c atomic_oper_cond_hook:
 *   - **Condition:** @ref rwlock_can_wlock(VAL, expedite)
 *   - **New value:** @c RWLOCK_STATE_WRITER_HOLDING (constant 0xFF).
 *     This atomically replaces whatever was in bits 0-8 (either @c UNLOCKED
 *     or @c WRITER_WAITING), thereby clearing the hint on acquisition.
 *   - **Success hook:** (none — @c w_holder is set after the macro returns).
 *   - **Failure hook:** @ref __rwlock_expedite_hook — sets the
 *     WRITER_WAITING hint if @p expedite is @c true and it isn't set yet.
 *
 * On success, publishes @c cpuid() into @c rw->w_holder with release
 * semantics so that other cores see a consistent writer identity.
 *
 * @param rw        Pointer to the rwlock.
 * @param expedite  If @c true, the caller ignores (and sets) the
 *                  WRITER_WAITING hint, gaining soft priority over
 *                  non-expediting readers and writers.
 * @return @c true if the write lock was acquired, @c false otherwise.
 */
static inline bool rwlock_try_wlock(struct rwlock *rw, bool expedite) {
    bool success = atomic_oper_cond_hook(
        &rw->state, RWLOCK_STATE_WRITER_HOLDING,
        rwlock_can_wlock(VAL, expedite),
        /* no success hook */,
        __rwlock_expedite_hook(&rw->state, VAL, expedite));
    if (success) {
        smp_store_release(&rw->w_holder, cpuid());
    }
    return success;
}

/**
 * @brief Predicate: can the current reader upgrade to a writer given @p state?
 *
 * Upgrading is safe only when:
 *   - No writer currently holds the lock (W_HOLDING bits are clear).  This
 *     rejects the write→read→update recursion path where a writer with a
 *     recursive read lock mistakenly attempts an upgrade, which would
 *     silently consume the reader bias and risk a double-release.
 *   - The caller is the **sole** reader (count == 1), so replacing the
 *     state with @c WRITER_HOLDING doesn't orphan other readers.
 *   - No WRITER_WAITING hint is set, meaning no other writer has claimed
 *     soft priority.
 *
 * @param rw     Pointer to the rwlock (unused in current implementation,
 *               kept for signature consistency with other predicates).
 * @param state  CAS-observed snapshot of @c rw->state.
 * @return @c true if upgrade is permissible.
 */
static inline bool rwlock_can_update(struct rwlock *rw, uint64 state) {
    if (RWLOCK_STATE_W_HOLDING(state)) {
        return false; // A writer holds the lock (includes write→read→update)
    }
    if (RWLOCK_STATE_R_COUNT(state) != 1 || RWLOCK_STATE_W_WAITING(state)) {
        return false; // Not the sole reader, or another writer is waiting
    }
    return true; // Sole reader, no writer, no waiting — upgrade is possible
}

/**
 * @brief Non-blocking read → write upgrade.
 *
 * Atomically transitions from "1 reader, no writer" to "writer holding".
 * The CAS replaces the entire state word with @c RWLOCK_STATE_WRITER_HOLDING,
 * which simultaneously removes the caller's reader bias and sets the writer
 * field.
 *
 * The write→read→update recursion path (a writer that also acquired a
 * recursive read lock and then attempts an "upgrade") is rejected by
 * @ref rwlock_can_update, which checks @c W_HOLDING(state) on the VAL
 * snapshot inside the CAS loop.  This prevents silently consuming the
 * reader bias and avoids double-release bugs.
 *
 * @pre  The caller must currently hold a read lock.
 * @post On success the caller holds the write lock; the read lock is consumed.
 *       On failure the caller still holds the read lock.
 *
 * @param rw  Pointer to the rwlock.
 * @return @c true if the upgrade succeeded, @c false if:
 *         - A writer holds the lock (including write→read→update recursion),
 *         - Other readers are present (@c R_COUNT != 1), or
 *         - A WRITER_WAITING hint prevents the upgrade.
 *
 * @note Callers that fail should either release the read lock and fall back to
 *       @ref rwlock_wacquire, or accept reading only.
 */
static inline bool rwlock_try_update(struct rwlock *rw) {
    bool success = atomic_oper_cond(&rw->state, RWLOCK_STATE_WRITER_HOLDING,
                                    rwlock_can_update(rw, VAL));
    if (success) {
        smp_store_release(&rw->w_holder, cpuid());
    }
    return success;
}

/** Initialize @p rw to unlocked state with diagnostic @p name. */
void rwlock_init(struct rwlock *rw, const char *name);

/** Spin-acquire a read lock (calls @ref rwlock_try_rlock in a loop). */
void rwlock_racquire(struct rwlock *rw);

/** Release a read lock (atomically subtracts @c READER_BIAS). */
void rwlock_rrelease(struct rwlock *rw);

/**
 * @brief Spin-acquire a write lock with adaptive expedite.
 *
 * Starts in non-expedite mode; after @c RWLOCK_EXPEDITE_THRESHOLD ticks
 * the call switches to expedite, setting WRITER_WAITING to gain soft
 * priority over incoming readers and non-expediting writers.
 */
void rwlock_wacquire(struct rwlock *rw);

/**
 * @brief Spin-acquire a write lock, always in expedite mode.
 *
 * Immediately sets WRITER_WAITING and ignores the hint set by other
 * writers.  Use when write latency is more important than fairness.
 */
void rwlock_wacquire_expedited(struct rwlock *rw);

/**
 * @brief Spin-acquire a write lock, never expediting.
 *
 * Will wait behind any WRITER_WAITING hint.  Fair, but may starve if
 * another writer repeatedly claims expedite.
 */
void rwlock_graceful_wacquire(struct rwlock *rw);

/**
 * @brief Release the write lock.
 *
 * Clears @c w_holder then stores @c RWLOCK_STATE_UNLOCKED.  This
 * unconditionally zeros the state, which may transiently clear a
 * WRITER_WAITING hint set by a spinning writer — that writer will
 * re-set the hint on its next failure-hook iteration.
 */
void rwlock_writer_release(struct rwlock *rw);

/** @name push_off / pop_off wrappers — nestable interrupt-safe lock/unlock
 * @{ */
void rwlock_rlock(struct rwlock *rw);
void rwlock_runlock(struct rwlock *rw);
void rwlock_wlock(struct rwlock *rw);
void rwlock_wlock_expedited(struct rwlock *rw);
void rwlock_graceful_wlock(struct rwlock *rw);
void rwlock_wunlock(struct rwlock *rw);
/** @} */

/** @name irqsave / irqrestore wrappers — raw interrupt save/restore
 * @{ */
int rwlock_rlock_irqsave(struct rwlock *rw);
void rwlock_runlock_irqrestore(struct rwlock *rw, int state);
int rwlock_wlock_irqsave(struct rwlock *rw);
int rwlock_wlock_expedited_irqsave(struct rwlock *rw);
int rwlock_graceful_wlock_irqsave(struct rwlock *rw);
void rwlock_wunlock_irqrestore(struct rwlock *rw, int state);
/** @} */

#endif // __KERNEL_READ_WRITE_SPIN_LOCK_H
