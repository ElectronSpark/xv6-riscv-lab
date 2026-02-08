/**
 * @file rwlock.c
 * @brief Read-write spin lock — blocking wrappers.
 *
 * This file contains the spin-wait wrappers that call the inline try-lock
 * primitives defined in rwlock.h.  Three flavours of write acquisition are
 * provided:
 *
 * | Function                     | Expedite behaviour |
 * |:-----------------------------|:--------------------------------------------|
 * | @ref rwlock_wacquire            | Adaptive — enables after timeout | | @ref
 * rwlock_wacquire_expedited  | Always expedites                             | |
 * @ref rwlock_graceful_wacquire   | Never expedites                              |
 */

#include "types.h"
#include "param.h"
#include <mm/memlayout.h>
#include "lock/spinlock.h"
#include "lock/rwlock.h"
#include "riscv.h"
#include "proc/thread.h"
#include <smp/percpu.h>
#include "defs.h"
#include "printf.h"
#include "timer/timer.h"

/**
 * @brief Initialise a read-write spinlock.
 *
 * Sets the state to @c UNLOCKED, clears the writer holder, and records a
 * diagnostic @p name.
 */
void rwlock_init(struct rwlock *rw, const char *name) {
    assert(rw != NULL, "rwlock_init: rwlock pointer is NULL");
    smp_store_release(&rw->state, RWLOCK_STATE_UNLOCKED); // Unlocked state
    smp_store_release(&rw->w_holder, RWLOCK_NONE_HOLDER); // No writer
    rw->name = name ? name : "unnamed";
}

/**
 * @brief Acquire a read lock, spinning until successful.
 *
 * Spins on @ref rwlock_try_rlock with @c cpu_relax() back-off.  Will
 * succeed immediately when no writer bits are set, or when the caller
 * already holds the write lock (write→read recursion).
 */
void rwlock_racquire(struct rwlock *rw) {
    assert(rw != NULL, "rwlock_racquire: rwlock pointer is NULL");
    while (!rwlock_try_rlock(rw)) {
        cpu_relax();
    }
}

/**
 * @brief Release a read lock.
 *
 * Atomically subtracts @c READER_BIAS from the state.  The caller must
 * hold at least one read lock; an assertion fires otherwise.
 */
void rwlock_rrelease(struct rwlock *rw) {
    uint64 prev_state = RWLOCK_STATE(rw);
    assert(RWLOCK_STATE_R_COUNT(prev_state) > 0,
           "rwlock_rrelease: no readers to unlock");
    atomic_sub(&rw->state, RWLOCK_STATE_READER_BIAS);
}

/**
 * @brief Acquire a write lock with adaptive expedite.
 *
 * Starts without expediting; if the lock is not obtained within
 * @c RWLOCK_EXPEDITE_THRESHOLD timer ticks, switches to expedite mode.
 * In expedite mode the failure-hook sets the @c WRITER_WAITING hint,
 * causing non-expediting readers and writers to voluntarily back off.
 */
void rwlock_wacquire(struct rwlock *rw) {
    assert(rw != NULL, "rwlock_wacquire: rwlock pointer is NULL");
    uint64 start_time = r_time();
    bool expedite = false;
    while (!rwlock_try_wlock(rw, expedite)) {
        cpu_relax();
        if (!expedite && r_time() - start_time >= RWLOCK_EXPEDITE_THRESHOLD) {
            // Allow writers to acquire lock even if there are waiting readers
            expedite = true;
        }
    }
}

/**
 * @brief Acquire a write lock, always in expedite mode.
 *
 * Immediately claims soft priority by setting @c WRITER_WAITING.
 * Suitable for latency-sensitive paths where writer starvation is the
 * primary concern.
 */
void rwlock_wacquire_expedited(struct rwlock *rw) {
    assert(rw != NULL, "rwlock_wacquire_expedited: rwlock pointer is NULL");
    while (!rwlock_try_wlock(rw, true)) {
        cpu_relax();
    }
}

/**
 * @brief Acquire a write lock without ever expediting.
 *
 * Will wait behind any @c WRITER_WAITING hint set by another writer.
 * Fair to other writers, but may starve under sustained read-heavy
 * or expedite-heavy workloads.
 */
void rwlock_graceful_wacquire(struct rwlock *rw) {
    assert(rw != NULL, "rwlock_graceful_wacquire: rwlock pointer is NULL");
    while (!rwlock_try_wlock(rw, false)) {
        cpu_relax();
    }
}

/**
 * @brief Release the write lock.
 *
 * Clears @c w_holder (release semantics) then unconditionally stores
 * @c RWLOCK_STATE_UNLOCKED into @c state.  This zeros the entire word,
 * which may transiently clear a @c WRITER_WAITING hint set by a spinning
 * writer — that writer will re-set the hint on its next CAS-failure
 * iteration via @ref __rwlock_expedite_hook.
 *
 * @pre The calling CPU must be the current write holder.
 */
void rwlock_writer_release(struct rwlock *rw) {
    assert(rw != NULL, "rwlock_writer_release: rwlock pointer is NULL");
    assert(RWLOCK_W_HOLDING(rw), "rwlock_writer_release: write lock not held");
    smp_store_release(&rw->w_holder, RWLOCK_NONE_HOLDER); // Clear writer holder
    smp_store_release(&rw->state,
                      RWLOCK_STATE_UNLOCKED); // Set to unlocked state
}

// ───────────────────────────────────────────────────────────────────────────
// push_off / pop_off wrappers — nestable interrupt-safe lock/unlock
// ───────────────────────────────────────────────────────────────────────────

/**
 * @brief Acquire a read lock with nestable interrupt disable.
 *
 * Disables interrupts via @c push_off before spinning on the lock,
 * preventing deadlocks when an interrupt handler also takes this lock.
 */
void rwlock_rlock(struct rwlock *rw) {
    push_off();
    rwlock_racquire(rw);
}

/**
 * @brief Release a read lock and restore interrupt state.
 */
void rwlock_runlock(struct rwlock *rw) {
    rwlock_rrelease(rw);
    pop_off();
}

/**
 * @brief Acquire a write lock (adaptive expedite) with nestable interrupt
 * disable.
 */
void rwlock_wlock(struct rwlock *rw) {
    push_off();
    rwlock_wacquire(rw);
}

/**
 * @brief Acquire a write lock (always expedite) with nestable interrupt
 * disable.
 */
void rwlock_wlock_expedited(struct rwlock *rw) {
    push_off();
    rwlock_wacquire_expedited(rw);
}

/**
 * @brief Acquire a write lock (never expedite) with nestable interrupt disable.
 */
void rwlock_graceful_wlock(struct rwlock *rw) {
    push_off();
    rwlock_graceful_wacquire(rw);
}

/**
 * @brief Release the write lock and restore interrupt state.
 */
void rwlock_wunlock(struct rwlock *rw) {
    rwlock_writer_release(rw);
    pop_off();
}

// ───────────────────────────────────────────────────────────────────────────
// irqsave / irqrestore wrappers — raw interrupt save/restore
// ───────────────────────────────────────────────────────────────────────────

/**
 * @brief Acquire a read lock, saving and disabling interrupts.
 * @return The previous interrupt-enable state (pass to
 *         @ref rwlock_runlock_irqrestore).
 */
int rwlock_rlock_irqsave(struct rwlock *rw) {
    int intena = intr_off_save();
    rwlock_racquire(rw);
    return intena;
}

/**
 * @brief Release a read lock and restore saved interrupt state.
 */
void rwlock_runlock_irqrestore(struct rwlock *rw, int intena) {
    rwlock_rrelease(rw);
    intr_restore(intena);
}

/**
 * @brief Acquire a write lock (adaptive expedite), saving and disabling
 *        interrupts.
 * @return The previous interrupt-enable state.
 */
int rwlock_wlock_irqsave(struct rwlock *rw) {
    int intena = intr_off_save();
    rwlock_wacquire(rw);
    return intena;
}

/**
 * @brief Acquire a write lock (always expedite), saving and disabling
 *        interrupts.
 * @return The previous interrupt-enable state.
 */
int rwlock_wlock_expedited_irqsave(struct rwlock *rw) {
    int intena = intr_off_save();
    rwlock_wacquire_expedited(rw);
    return intena;
}

/**
 * @brief Acquire a write lock (never expedite), saving and disabling
 *        interrupts.
 * @return The previous interrupt-enable state.
 */
int rwlock_graceful_wlock_irqsave(struct rwlock *rw) {
    int intena = intr_off_save();
    rwlock_graceful_wacquire(rw);
    return intena;
}

/**
 * @brief Release the write lock and restore saved interrupt state.
 */
void rwlock_wunlock_irqrestore(struct rwlock *rw, int intena) {
    rwlock_writer_release(rw);
    intr_restore(intena);
}
