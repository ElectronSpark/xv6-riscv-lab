#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "param.h"
#include "errno.h"
#include <mm/memlayout.h>
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "proc/tq.h"
#include "proc/sched.h"
#include "lock/completion.h"
#include "signal.h"
#include "timer/timer.h"

#define MAX_COMPLETIONS 65535

void completion_init(completion_t *c) {
    c->done = 0;
    spin_init(&c->lock, "completion_spin");
    tq_init(&c->wait_queue, "completion_queue", &c->lock);
}

void completion_reinit(completion_t *c) { c->done = 0; }

static bool __try_wait_for_completion(completion_t *c) {
    if (c->done <= 0) {
        return false;
    }
    if (c->done != MAX_COMPLETIONS) {
        c->done--;
    }
    return true;
}

static void __completion_do_wake(completion_t *c) {
    if (tq_size(&c->wait_queue) > 0) {
        struct thread *p = tq_wakeup(&c->wait_queue, 0, 0);
        (void)p; // @TODO: ignore interrupt by now
    }
}

struct completion_timeout_wait {
    spinlock_t *lock;
    struct timer_node *timer;
    uint64 timeout_ms;
    bool timer_armed;
};

static int __completion_timeout_sleep_cb(void *data) {
    struct completion_timeout_wait *ctx = data;

    ctx->timer_armed =
        ctx->timeout_ms > 0 && sched_timer_set(ctx->timer, ctx->timeout_ms) == 0;

    int status = spin_sleep_cb(ctx->lock);
    if (!ctx->timer_armed) {
        scheduler_wakeup(current);
    }
    return status;
}

static void __completion_timeout_wake_cb(void *data, int status) {
    struct completion_timeout_wait *ctx = data;

    if (ctx->timer_armed) {
        sched_timer_done(ctx->timer);
        ctx->timer_armed = false;
    }
    spin_wake_cb(ctx->lock, status);
}

bool try_wait_for_completion(completion_t *c) {
    if (c == NULL) {
        return false;
    }
    spin_lock(&c->lock);
    bool ret = __try_wait_for_completion(c);
    spin_unlock(&c->lock);
    return ret;
}

void wait_for_completion(completion_t *c) {
    assert(current != NULL,
           "wait_for_completion called from non-thread context");
    if (c == NULL) {
        return;
    }
    spin_lock(&c->lock);
    while (!__try_wait_for_completion(c)) {
        int ret = tq_wait(&c->wait_queue, &c->lock, NULL);
        (void)ret; // @TODO: ignore interrupt by now
    }
    if (c->done > 0) {
        __completion_do_wake(c);
    }
    spin_unlock(&c->lock);
}

int wait_for_completion_interruptible(completion_t *c) {
    assert(current != NULL,
           "wait_for_completion_interruptible called from non-thread context");
    if (c == NULL) {
        return -EINTR;
    }
    spin_lock(&c->lock);
    while (!__try_wait_for_completion(c)) {
        int ret = tq_wait_in_state(&c->wait_queue, &c->lock, NULL,
                                    THREAD_INTERRUPTIBLE);
        if (ret != 0) {
            spin_unlock(&c->lock);
            return -EINTR;
        }
    }
    if (c->done > 0) {
        __completion_do_wake(c);
    }
    spin_unlock(&c->lock);
    return 0;
}

void complete(completion_t *c) {
    if (c == NULL) {
        return;
    }
    spin_lock(&c->lock);
    if (c->done != MAX_COMPLETIONS) {
        c->done++;
    }
    __completion_do_wake(c);
    spin_unlock(&c->lock);
}

void complete_all(completion_t *c) {
    if (c == NULL) {
        return;
    }

    // Use a temporary queue to collect waiters, so we can release
    // the lock before waking them (avoiding lock convoy when woken
    // threads try to re-acquire c->lock in scheduler_sleep).
    tq_t temp_queue;
    tq_init(&temp_queue, "completion_temp", NULL);

    spin_lock(&c->lock);
    c->done = MAX_COMPLETIONS;
    // Move all waiters to temp queue
    tq_bulk_move(&temp_queue, &c->wait_queue);
    spin_unlock(&c->lock);

    // Wake all waiters outside the lock
    if (temp_queue.counter > 0) {
        tq_wakeup_all(&temp_queue, 0, 0);
    }
}

bool completion_done(completion_t *c) {
    if (c == NULL) {
        return false;
    }
    spin_lock(&c->lock);
    bool done = tq_size(&c->wait_queue) == 0;
    spin_unlock(&c->lock);
    return done;
}

/*
 * wait_for_completion_timeout - wait for completion with a millisecond deadline.
 *
 * Returns the remaining time in ms if the completion was signalled before the
 * deadline, or 0 on timeout.  Like Linux's wait_for_completion_timeout().
 */
uint64 wait_for_completion_timeout(completion_t *c, uint64 timeout_ms) {
    assert(current != NULL,
           "wait_for_completion_timeout called from non-thread context");
    if (c == NULL) {
        return 0;
    }

    uint64 timeout_ticks = MS_TO_RAWTICKS(timeout_ms);
    uint64 start = r_time();

    spin_lock(&c->lock);
    while (!__try_wait_for_completion(c)) {
        uint64 elapsed = r_time() - start;
        if (elapsed >= timeout_ticks) {
            spin_unlock(&c->lock);
            return 0; // timed out
        }
        uint64 remaining_ms = RAWTICKS_TO_MS(timeout_ticks - elapsed);
        if (remaining_ms == 0)
            remaining_ms = 1;

        struct timer_node tn = {0};
        struct completion_timeout_wait wait = {
            .lock = &c->lock,
            .timer = &tn,
            .timeout_ms = remaining_ms,
        };
        tq_wait_cb(&c->wait_queue, __completion_timeout_sleep_cb,
                   __completion_timeout_wake_cb, &wait, NULL);
    }
    if (c->done > 0) {
        __completion_do_wake(c);
    }
    spin_unlock(&c->lock);

    uint64 elapsed = r_time() - start;
    if (elapsed >= timeout_ticks)
        return 1; /* completed but at the boundary — return minimum 1 */
    return RAWTICKS_TO_MS(timeout_ticks - elapsed);
}

/*
 * wait_for_completion_interruptible_timeout - wait for completion, interruptible
 *                                             by signals, with a ms deadline.
 *
 * Returns remaining ms (>0) if completed, 0 on timeout, -EINTR on signal.
 */
int64 wait_for_completion_interruptible_timeout(completion_t *c,
                                                uint64 timeout_ms) {
    assert(current != NULL,
           "wait_for_completion_interruptible_timeout called from non-thread "
           "context");
    if (c == NULL) {
        return -EINTR;
    }

    uint64 timeout_ticks = MS_TO_RAWTICKS(timeout_ms);
    uint64 start = r_time();

    spin_lock(&c->lock);
    while (!__try_wait_for_completion(c)) {
        if (signal_pending(current)) {
            spin_unlock(&c->lock);
            return -EINTR;
        }
        uint64 elapsed = r_time() - start;
        if (elapsed >= timeout_ticks) {
            spin_unlock(&c->lock);
            return 0; // timed out
        }
        uint64 remaining_ms = RAWTICKS_TO_MS(timeout_ticks - elapsed);
        if (remaining_ms == 0)
            remaining_ms = 1;

        struct timer_node tn = {0};
        struct completion_timeout_wait wait = {
            .lock = &c->lock,
            .timer = &tn,
            .timeout_ms = remaining_ms,
        };
        int ret = tq_wait_in_state_cb(&c->wait_queue,
                                      __completion_timeout_sleep_cb,
                                      __completion_timeout_wake_cb, &wait, NULL,
                                      THREAD_INTERRUPTIBLE);
        if (ret != 0) {
            spin_unlock(&c->lock);
            return -EINTR;
        }
    }
    if (c->done > 0) {
        __completion_do_wake(c);
    }
    spin_unlock(&c->lock);

    uint64 elapsed = r_time() - start;
    if (elapsed >= timeout_ticks)
        return 1;
    return (int64)RAWTICKS_TO_MS(timeout_ticks - elapsed);
}
