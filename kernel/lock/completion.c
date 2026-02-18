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

struct completion_timed_wait_ctx {
    completion_t *comp;
    struct timer_node timer;
    uint64 timeout_ms;
    bool timer_armed;
};

static int __completion_timed_sleep_cb(void *data) {
    struct completion_timed_wait_ctx *ctx =
        (struct completion_timed_wait_ctx *)data;
    if (ctx == NULL || ctx->comp == NULL) {
        return 0;
    }

    ctx->timer_armed = false;
    if (ctx->timeout_ms > 0 &&
        sched_timer_set(&ctx->timer, ctx->timeout_ms) == 0) {
        ctx->timer_armed = true;
    }

    int status = spin_holding(&ctx->comp->lock);
    if (status) {
        spin_unlock(&ctx->comp->lock);
    }
    return status;
}

static void __completion_timed_wake_cb(void *data, int sleep_cb_status) {
    struct completion_timed_wait_ctx *ctx =
        (struct completion_timed_wait_ctx *)data;
    if (ctx == NULL || ctx->comp == NULL) {
        return;
    }

    if (ctx->timer_armed) {
        sched_timer_done(&ctx->timer);
        ctx->timer_armed = false;
    }

    if (sleep_cb_status) {
        spin_lock(&ctx->comp->lock);
    }
}

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
        __thread_state_set(current, THREAD_UNINTERRUPTIBLE);
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
        return -EINVAL;
    }

    spin_lock(&c->lock);
    while (!__try_wait_for_completion(c)) {
        if (signal_pending(current)) {
            spin_unlock(&c->lock);
            return -EINTR;
        }

        __thread_state_set(current, THREAD_INTERRUPTIBLE);
        int ret = tq_wait(&c->wait_queue, &c->lock, NULL);
        if (ret != 0 && signal_pending(current) && !__try_wait_for_completion(c)) {
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

int wait_for_completion_timed(completion_t *c, uint64 timeout_ms) {
    assert(current != NULL,
           "wait_for_completion_timed called from non-thread context");
    if (c == NULL) {
        return -EINVAL;
    }

    if (timeout_ms == 0) {
        if (try_wait_for_completion(c)) {
            return 0;
        }
        return -ETIMEDOUT;
    }

    spin_lock(&c->lock);
    if (__try_wait_for_completion(c)) {
        if (c->done > 0) {
            __completion_do_wake(c);
        }
        spin_unlock(&c->lock);
        return 0;
    }

    uint64 timeout_ticks = MS_TO_RAWTICKS(timeout_ms);
    uint64 start = r_time();

    while (!__try_wait_for_completion(c)) {
        if (signal_pending(current)) {
            spin_unlock(&c->lock);
            return -EINTR;
        }

        uint64 elapsed = r_time() - start;
        if (elapsed >= timeout_ticks) {
            spin_unlock(&c->lock);
            return -ETIMEDOUT;
        }

        uint64 remaining_ticks = timeout_ticks - elapsed;
        uint64 remaining_ms = (remaining_ticks + TICK_MS - 1) / TICK_MS;
        if (remaining_ms == 0) {
            remaining_ms = 1;
        }

        struct completion_timed_wait_ctx ctx = {
            .comp = c,
            .timeout_ms = remaining_ms,
            .timer_armed = false,
        };

        __thread_state_set(current, THREAD_INTERRUPTIBLE);
        int ret = tq_wait_cb(&c->wait_queue, __completion_timed_sleep_cb,
                             __completion_timed_wake_cb, &ctx, NULL);
        if (ret != 0 && signal_pending(current) && !__try_wait_for_completion(c)) {
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
