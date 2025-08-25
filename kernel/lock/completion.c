#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "proc_queue.h"
#include "sched.h"
#include "completion.h"

#define MAX_COMPLETIONS 65535

void completion_init(completion_t *c) {
    c->done = 0;
    spin_init(&c->lock, "completion_spin");
    proc_queue_init(&c->wait_queue, "completion_queue", &c->lock);
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
        proc_queue_wakeup(&c->wait_queue, 0, NULL);
    }
}

static void __completion_do_wake_all(completion_t *c) {
    if (proc_queue_size(&c->wait_queue) > 0) {
        int ret = proc_queue_wakeup_all(&c->wait_queue, 0);
        (void)ret; // @TODO: ignore interrupt by now
    }
}

bool try_wait_for_completion(completion_t *c) {
    if (c == NULL) {
        return false;
    }
    spin_acquire(&c->lock);
    bool ret = __try_wait_for_completion(c);
    spin_release(&c->lock);
    return ret;
}

void wait_for_completion(completion_t *c) {
    if (c == NULL) {
        return;
    }
    spin_acquire(&c->lock);
    while (!__try_wait_for_completion(c)) {
        int ret = proc_queue_wait(&c->wait_queue, &c->lock);
        (void)ret; // @TODO: ignore interrupt by now
    }
    if (c->done > 0) {
        __completion_do_wake(c);
    }
    spin_release(&c->lock);
}

void complete(completion_t *c) {
    if (c == NULL) {
        return;
    }
    spin_acquire(&c->lock);
    if (c->done != MAX_COMPLETIONS) {
        c->done++;
    }
    __completion_do_wake(c);
    spin_release(&c->lock);
}

void complete_all(completion_t *c) {
    if (c == NULL) {
        return;
    }
    spin_acquire(&c->lock);
    c->done = MAX_COMPLETIONS;
    __completion_do_wake_all(c);
    spin_release(&c->lock);
}

bool completion_done(completion_t *c) {
    if (c == NULL) {
        return false;
    }
    spin_acquire(&c->lock);
    bool done = (c->done == 0);
    spin_release(&c->lock);
    return done;
}
