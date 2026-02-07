#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "param.h"
#include "errno.h"
#include <mm/memlayout.h>
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "proc/proc_queue.h"
#include "proc/sched.h"
#include "lock/completion.h"

#define MAX_COMPLETIONS 65535

void completion_init(completion_t *c) {
    c->done = 0;
    spin_init(&c->lock, "completion_spin");
    proc_queue_init(&c->wait_queue, "completion_queue", &c->lock);
}

void completion_reinit(completion_t *c) {
    c->done = 0;
}

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
    if (proc_queue_size(&c->wait_queue) > 0) {
        struct proc *p = proc_queue_wakeup(&c->wait_queue, 0, 0);
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
    assert(myproc() != NULL, "wait_for_completion called from non-process context");
    if (c == NULL) {
        return;
    }
    spin_lock(&c->lock);
    while (!__try_wait_for_completion(c)) {
        int ret = proc_queue_wait(&c->wait_queue, &c->lock, NULL);
        (void)ret; // @TODO: ignore interrupt by now
    }
    if (c->done > 0) {
        __completion_do_wake(c);
    }
    spin_unlock(&c->lock);
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
    // processes try to re-acquire c->lock in scheduler_sleep).
    proc_queue_t temp_queue;
    proc_queue_init(&temp_queue, "completion_temp", NULL);
    
    spin_lock(&c->lock);
    c->done = MAX_COMPLETIONS;
    // Move all waiters to temp queue
    proc_queue_bulk_move(&temp_queue, &c->wait_queue);
    spin_unlock(&c->lock);
    
    // Wake all waiters outside the lock
    if (temp_queue.counter > 0) {
        proc_queue_wakeup_all(&temp_queue, 0, 0);
    }
}

bool completion_done(completion_t *c) {
    if (c == NULL) {
        return false;
    }
    spin_lock(&c->lock);
    bool done = proc_queue_size(&c->wait_queue) == 0;
    spin_unlock(&c->lock);
    return done;
}
