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
#include "signal.h"
#include "timer/timer.h"
#include "errno.h"
#include "smp/atomic.h"

struct mutex_timed_wait_ctx {
    mutex_t *lock;
    struct timer_node timer;
    uint64 timeout_ms;
    bool timer_armed;
};

static int __mutex_timed_sleep_cb(void *data) {
    struct mutex_timed_wait_ctx *ctx = (struct mutex_timed_wait_ctx *)data;
    if (ctx == NULL || ctx->lock == NULL) {
        return 0;
    }

    ctx->timer_armed = false;
    if (ctx->timeout_ms > 0 && sched_timer_set(&ctx->timer, ctx->timeout_ms) == 0) {
        ctx->timer_armed = true;
    }

    int status = spin_holding(&ctx->lock->lk);
    if (status) {
        spin_unlock(&ctx->lock->lk);
    }
    return status;
}

static void __mutex_timed_wake_cb(void *data, int sleep_cb_status) {
    struct mutex_timed_wait_ctx *ctx = (struct mutex_timed_wait_ctx *)data;
    if (ctx == NULL || ctx->lock == NULL) {
        return;
    }

    if (ctx->timer_armed) {
        sched_timer_done(&ctx->timer);
        ctx->timer_armed = false;
    }

    if (sleep_cb_status) {
        spin_lock(&ctx->lock->lk);
    }
}

#define __mutex_set_holder(lk, pid)                                            \
    smp_store_release(                                                         \
        &lk->holder,                                                           \
        pid) // Use atomic store with release semantics to set holder
#define __mutex_holder(lk) smp_load_acquire(&lk->holder)
#define __mutex_try_set_holder(lk, pid)                                        \
    atomic_cas(&lk->holder, -1, pid) // -1 = no holder

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

int mutex_lock(mutex_t *lk) {
    struct thread *self = current;
    assert(self != NULL, "mutex_lock: no current thread");
    assert(mycpu()->spin_depth == 0, "mutex_lock called with spinlock held");
    assert(!CPU_IN_ITR(), "mutex_lock called in interrupt context");

    // If the lock is not held, acquire it and return success.
    if (__mutex_try_set_holder(lk, self->pid)) {
        return 0;
    }

    // Slow path
    spin_lock(&lk->lk);
    if (__mutex_try_set_holder(lk, self->pid)) {
        // It's possible that someone releases the mutex just right before the
        // current thread tried to acquire the mutex without holding spinlock.
        // In that case, we just need to claim the mutex and return.
        spin_unlock(&lk->lk);
        return 0;
    }

    // Deadlock detection: If we failed to acquire the lock AND we're already
    // the holder, that's a programming error (trying to lock a mutex we already
    // hold)
    assert(__mutex_holder(lk) != self->pid,
           "mutex_lock: deadlock detected, thread already holds the lock");

    while (__mutex_holder(lk) != self->pid) {
        __thread_state_set(current, THREAD_UNINTERRUPTIBLE);
        int ret = tq_wait(&lk->wait_queue, &lk->lk, NULL);
        if (ret != 0) {
            // If tq_wait returns an error, and the thread has already
            // gotten the lock, we need to release the lock and return the error
            // code.
            if (__mutex_holder(lk) == self->pid) {
                assert(!IS_ERR_OR_NULL(__do_wakeup(lk)),
                       "mutex_lock: failed to wake up threads after interrupt");
            }
            spin_unlock(&lk->lk);
            return ret;
        }
    }
    spin_unlock(&lk->lk);

    return 0;
}

// @TODO: signal handling
void mutex_unlock(mutex_t *lk) {
    // First put all threads from the wait queue to a temporary queue,
    // so that we can detach them from the wait queue, and then wake them up.
    // This is to avoid deadlocks, as we cannot hold the lock while waking up
    // threads from the wait queue.
    spin_lock(&lk->lk);
    struct thread *self = current;
    assert(self != NULL, "mutex_unlock: no thread context");
    assert(__mutex_holder(lk) == self->pid,
           "mutex_unlock: thread does not hold the lock");
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

int mutex_lock_interruptible(mutex_t *lk) {
    assert(current != NULL,
           "mutex_lock_interruptible: no current thread context");
    assert(mycpu()->spin_depth == 0,
           "mutex_lock_interruptible called with spinlock held");
    assert(!CPU_IN_ITR(), "mutex_lock_interruptible called in interrupt context");

    if (lk == NULL) {
        return -EINVAL;
    }

    struct thread *self = current;

    if (__mutex_try_set_holder(lk, self->pid)) {
        return 0;
    }

    spin_lock(&lk->lk);
    if (__mutex_try_set_holder(lk, self->pid)) {
        spin_unlock(&lk->lk);
        return 0;
    }

    assert(__mutex_holder(lk) != self->pid,
           "mutex_lock_interruptible: deadlock detected, thread already holds the lock");

    while (__mutex_holder(lk) != self->pid) {
        if (signal_pending(current)) {
            spin_unlock(&lk->lk);
            return -EINTR;
        }

        __thread_state_set(current, THREAD_INTERRUPTIBLE);
        int ret = tq_wait(&lk->wait_queue, &lk->lk, NULL);
        if (ret != 0 && signal_pending(current) && __mutex_holder(lk) != self->pid) {
            spin_unlock(&lk->lk);
            return -EINTR;
        }
    }

    spin_unlock(&lk->lk);
    return 0;
}

int mutex_lock_timed(mutex_t *lk, uint64 timeout_ms) {
    assert(current != NULL, "mutex_lock_timed: no current thread context");
    assert(mycpu()->spin_depth == 0,
           "mutex_lock_timed called with spinlock held");
    assert(!CPU_IN_ITR(), "mutex_lock_timed called in interrupt context");

    if (lk == NULL) {
        return -EINVAL;
    }

    struct thread *self = current;

    if (__mutex_try_set_holder(lk, self->pid)) {
        return 0;
    }

    if (timeout_ms == 0) {
        return -ETIMEDOUT;
    }

    spin_lock(&lk->lk);
    if (__mutex_try_set_holder(lk, self->pid)) {
        spin_unlock(&lk->lk);
        return 0;
    }

    assert(__mutex_holder(lk) != self->pid,
           "mutex_lock_timed: deadlock detected, thread already holds the lock");

    uint64 timeout_ticks = MS_TO_RAWTICKS(timeout_ms);
    uint64 start = r_time();

    while (__mutex_holder(lk) != self->pid) {
        uint64 elapsed = r_time() - start;
        if (elapsed >= timeout_ticks) {
            spin_unlock(&lk->lk);
            return -ETIMEDOUT;
        }

        uint64 remaining_ticks = timeout_ticks - elapsed;
        uint64 remaining_ms = (remaining_ticks + TICK_MS - 1) / TICK_MS;
        if (remaining_ms == 0) {
            remaining_ms = 1;
        }

        struct mutex_timed_wait_ctx ctx = {
            .lock = lk,
            .timeout_ms = remaining_ms,
            .timer_armed = false,
        };

        __thread_state_set(current, THREAD_INTERRUPTIBLE);
        int ret = tq_wait_cb(&lk->wait_queue, __mutex_timed_sleep_cb,
                             __mutex_timed_wake_cb, &ctx, NULL);
        if (ret != 0 && signal_pending(current) && __mutex_holder(lk) != self->pid) {
            spin_unlock(&lk->lk);
            return -EINTR;
        }
    }

    spin_unlock(&lk->lk);
    return 0;
}
