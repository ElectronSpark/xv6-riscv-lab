// Sleeping locks

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "param.h"
#include <mm/memlayout.h>
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "lock/mutex_types.h"
#include "proc/tq.h"
#include "proc/sched.h"
#include "errno.h"
#include "smp/atomic.h"
#include "signal.h"
#include "timer/timer.h"

#define __mutex_set_holder(lk, pid)                                            \
    smp_store_release(                                                         \
        &lk->holder,                                                           \
        pid) // Use atomic store with release semantics to set holder
#define __mutex_holder(lk) smp_load_acquire(&lk->holder)
#define __mutex_try_set_holder(lk, pid)                                        \
    atomic_cas(&lk->holder, -1, pid) // -1 = no holder

/* ---------------------------------------------------------------
 * Timed mutex sleep/wake callbacks used by mutex_lock_timed().
 * --------------------------------------------------------------- */

struct __mutex_timed_ctx {
    mutex_t *lk;          /* the mutex being acquired */
    uint64 timeout_ms;    /* remaining timeout for this iteration (ms) */
    bool timer_armed;     /* whether the deadline timer is active */
    struct timer_node timer;
};

/*
 * Called by tq_wait_in_state_cb just before scheduler_yield().
 * Releases the mutex spinlock and arms the deadline timer.
 */
static int __mutex_timed_sleep_cb(void *data) {
    struct __mutex_timed_ctx *ctx = (struct __mutex_timed_ctx *)data;
    spin_unlock(&ctx->lk->lk);

    ctx->timer_armed = false;
    if (ctx->timeout_ms > 0 &&
        sched_timer_set(&ctx->timer, ctx->timeout_ms) == 0) {
        ctx->timer_armed = true;
    }

    return 0;
}

/*
 * Called by tq_wait_in_state_cb after the thread is rescheduled.
 * Cancels the deadline timer and reacquires the mutex spinlock.
 */
static void __mutex_timed_wake_cb(void *data, int status) {
    struct __mutex_timed_ctx *ctx = (struct __mutex_timed_ctx *)data;
    if (ctx->timer_armed) {
        sched_timer_done(&ctx->timer);
        ctx->timer_armed = false;
    }
    spin_lock(&ctx->lk->lk);
}

static struct thread *__do_wakeup(mutex_t *lk) {
    struct thread *next = tq_wakeup(&lk->wait_queue, 0, 0);
    if (next == NULL) {
        __mutex_set_holder(lk, -1); // -1 = no holder
        return NULL;
    } else if (IS_ERR(next)) {
        return ERR_CAST(next); // Error: failed to wake up thread
    }
    __mutex_set_holder(lk, next->pid);
    return next;
}

void mutex_init(mutex_t *lk, char *name) {
    spin_init(&lk->lk, "sleep lock");
    tq_init(&lk->wait_queue, "sleep lock wait queue", &lk->lk);
    lk->name = name;
    __mutex_set_holder(lk, -1); // -1 = no holder (0 is valid PID for idle)
}

void mutex_lock(mutex_t *lk) {
    struct thread *self = current;
    assert(self != NULL, "mutex_lock: no current thread");
    assert(mycpu()->spin_depth == 0, "mutex_lock called with spinlock held");
    assert(!CPU_IN_ITR(), "mutex_lock called in interrupt context");

    // If the lock is not held, acquire it and return success.
    if (__mutex_try_set_holder(lk, self->pid)) {
        return;
    }

    // Slow path
    spin_lock(&lk->lk);
    if (__mutex_try_set_holder(lk, self->pid)) {
        // It's possible that someone releases the mutex just right before the
        // current thread tried to acquire the mutex without holding spinlock.
        // In that case, we just need to claim the mutex and return.
        spin_unlock(&lk->lk);
        return;
    }

    // Deadlock detection: If we failed to acquire the lock AND we're already
    // the holder, that's a programming error (trying to lock a mutex we already
    // hold)
    assert(__mutex_holder(lk) != self->pid,
           "mutex_lock: deadlock detected, thread already holds the lock");

    while (__mutex_holder(lk) != self->pid) {
        __thread_state_set(current, THREAD_UNINTERRUPTIBLE);
        int ret = tq_wait(&lk->wait_queue, &lk->lk, NULL);
        if (ret != 0 && __mutex_holder(lk) != self->pid) {
            continue;
        }
    }
    spin_unlock(&lk->lk);
}

// @TODO: signal handling
void mutex_unlock(mutex_t *lk) {
    struct thread *self = current;
    assert(self != NULL, "mutex_unlock: no thread context");
    assert(__mutex_holder(lk) == self->pid,
           "mutex_unlock: thread does not hold the lock");

    /*
     * Serialize the fast release with the slow acquire path.  A waiter holds
     * lk->lk while transitioning from "observed a holder" to "queued and
     * asleep"; clearing holder without synchronizing with that window can lose
     * the only wakeup and leave the mutex permanently idle with sleepers.
     */
    spin_lock(&lk->lk);
    if (__atomic_load_n(&lk->wait_queue.counter, __ATOMIC_RELAXED) == 0) {
        __mutex_set_holder(lk, -1);
        spin_unlock(&lk->lk);
        return;
    }

    // First put all threads from the wait queue to a temporary queue,
    // so that we can detach them from the wait queue, and then wake them up.
    // This is to avoid deadlocks, as we cannot hold the lock while waking up
    // threads from the wait queue.
    struct thread *next = __do_wakeup(lk);
    assert(!IS_ERR(next), "mutex_unlock: failed to wake up threads");
    spin_unlock(&lk->lk);
}

int holding_mutex(mutex_t *lk) {
    struct thread *self = current;
    if (self == NULL) {
        return 0; // No thread context, can't be holding the lock
    }
    return __atomic_load_n(&lk->holder, __ATOMIC_SEQ_CST) == self->pid;
}

int mutex_trylock(mutex_t *lk) {
    struct thread *self = current;
    assert(self != NULL, "mutex_trylock: no current thread");
    assert(mycpu()->spin_depth == 0, "mutex_trylock called with spinlock held");
    assert(!CPU_IN_ITR(), "mutex_trylock called in interrupt context");

    // Try to acquire the lock without blocking
    if (__mutex_try_set_holder(lk, self->pid)) {
        return 1; // Successfully acquired
    }
    return 0; // Failed to acquire
}

/*
 * mutex_lock_interruptible - acquire a mutex, interruptible by signals.
 *
 * Returns 0 on success, -EINTR if a signal is pending.
 */
int mutex_lock_interruptible(mutex_t *lk) {
    struct thread *self = current;
    assert(self != NULL,
           "mutex_lock_interruptible: no current thread context");
    assert(mycpu()->spin_depth == 0,
           "mutex_lock_interruptible called with spinlock held");
    assert(!CPU_IN_ITR(),
           "mutex_lock_interruptible called in interrupt context");

    if (lk == NULL) {
        return -EINVAL;
    }

    // Fast path: try to acquire without sleeping
    if (__mutex_try_set_holder(lk, self->pid)) {
        return 0;
    }

    spin_lock(&lk->lk);
    if (__mutex_try_set_holder(lk, self->pid)) {
        spin_unlock(&lk->lk);
        return 0;
    }

    assert(__mutex_holder(lk) != self->pid,
           "mutex_lock_interruptible: deadlock detected, thread already holds "
           "the lock");

    while (__mutex_holder(lk) != self->pid) {
        if (signal_pending(current)) {
            spin_unlock(&lk->lk);
            return -EINTR;
        }

        __thread_state_set(current, THREAD_INTERRUPTIBLE);
        int ret = tq_wait(&lk->wait_queue, &lk->lk, NULL);
        if (ret != 0 && signal_pending(current) &&
            __mutex_holder(lk) != self->pid) {
            spin_unlock(&lk->lk);
            return -EINTR;
        }
    }
    spin_unlock(&lk->lk);
    return 0;
}

/*
 * mutex_lock_timed - acquire a mutex with a millisecond deadline.
 *
 * Returns  0         on success,
 *         -ETIMEDOUT if the deadline expired before acquisition,
 *         -EINTR     if a signal arrived before acquisition.
 */
int mutex_lock_timed(mutex_t *lk, uint64 timeout_ms) {
    struct thread *self = current;
    assert(self != NULL, "mutex_lock_timed: no current thread");
    assert(mycpu()->spin_depth == 0,
           "mutex_lock_timed called with spinlock held");
    assert(!CPU_IN_ITR(), "mutex_lock_timed called in interrupt context");

    if (lk == NULL) {
        return -EINVAL;
    }

    // Fast path
    if (__mutex_try_set_holder(lk, self->pid)) {
        return 0;
    }

    spin_lock(&lk->lk);
    if (__mutex_try_set_holder(lk, self->pid)) {
        spin_unlock(&lk->lk);
        return 0;
    }

    assert(
        __mutex_holder(lk) != self->pid,
        "mutex_lock_timed: deadlock detected, thread already holds the lock");

    uint64 timeout_ticks = MS_TO_RAWTICKS(timeout_ms);
    uint64 start = r_time();

    while (__mutex_holder(lk) != self->pid) {
        uint64 elapsed = r_time() - start;
        if (elapsed >= timeout_ticks) {
            spin_unlock(&lk->lk);
            return -ETIMEDOUT;
        }

        uint64 remaining_ms = RAWTICKS_TO_MS(timeout_ticks - elapsed);
        if (remaining_ms == 0)
            remaining_ms = 1; /* at least 1 ms to arm timer */

        if (signal_pending(current)) {
            spin_unlock(&lk->lk);
            return -EINTR;
        }

        struct __mutex_timed_ctx ctx = {
            .lk = lk,
            .timeout_ms = remaining_ms,
            .timer_armed = false,
        };

        __thread_state_set(current, THREAD_INTERRUPTIBLE);
        int ret = tq_wait_cb(&lk->wait_queue, __mutex_timed_sleep_cb,
                             __mutex_timed_wake_cb, &ctx, NULL);
        if (ret != 0 && signal_pending(current) &&
            __mutex_holder(lk) != self->pid) {
            spin_unlock(&lk->lk);
            return -EINTR;
        }
    }
    spin_unlock(&lk->lk);
    return 0;
}
