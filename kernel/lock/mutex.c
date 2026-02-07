// Sleeping locks

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "param.h"
#include <mm/memlayout.h>
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "lock/mutex_types.h"
#include "proc/proc_queue.h"
#include "proc/sched.h"
#include "errno.h"
#include "smp/atomic.h"

#define __mutex_set_holder(lk, pid)                                            \
    smp_store_release(                                                         \
        &lk->holder,                                                           \
        pid) // Use atomic store with release semantics to set holder
#define __mutex_holder(lk) smp_load_acquire(&lk->holder)
#define __mutex_try_set_holder(lk, pid)                                        \
    atomic_cas(&lk->holder, -1, pid) // -1 = no holder

static struct proc *__do_wakeup(mutex_t *lk) {
    struct proc *next = proc_queue_wakeup(&lk->wait_queue, 0, 0);
    if (next == NULL) {
        __mutex_set_holder(lk, -1); // -1 = no holder
        return NULL;
    } else if (IS_ERR(next)) {
        return ERR_CAST(next); // Error: failed to wake up process
    }
    __mutex_set_holder(lk, next->pid);
    return next;
}

void mutex_init(mutex_t *lk, char *name) {
    spin_init(&lk->lk, "sleep lock");
    proc_queue_init(&lk->wait_queue, "sleep lock wait queue", &lk->lk);
    lk->name = name;
    __mutex_set_holder(lk, -1); // -1 = no holder (0 is valid PID for idle)
}

int mutex_lock(mutex_t *lk) {
    struct proc *self = myproc();
    assert(self != NULL, "mutex_lock: no current process");
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
        // current process tried to acquire the mutex without holding spinlock.
        // In that case, we just need to claim the mutex and return.
        spin_unlock(&lk->lk);
        return 0;
    }

    // Deadlock detection: If we failed to acquire the lock AND we're already
    // the holder, that's a programming error (trying to lock a mutex we already
    // hold)
    assert(__mutex_holder(lk) != self->pid,
           "mutex_lock: deadlock detected, process already holds the lock");

    while (__mutex_holder(lk) != self->pid) {
        int ret = proc_queue_wait(&lk->wait_queue, &lk->lk, NULL);
        if (ret != 0) {
            // If proc_queue_wait returns an error, and the process has already
            // gotten the lock, we need to release the lock and return the error
            // code.
            if (__mutex_holder(lk) == self->pid) {
                assert(
                    !IS_ERR_OR_NULL(__do_wakeup(lk)),
                    "mutex_lock: failed to wake up processes after interrupt");
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
    // First put all process from the wait queue to a temporary queue,
    // so that we can detach them from the wait queue, and then wake them up.
    // This is to avoid deadlocks, as we cannot hold the lock while waking up
    // processes from the wait queue.
    spin_lock(&lk->lk);
    struct proc *self = myproc();
    assert(self != NULL, "mutex_unlock: no process context");
    assert(__mutex_holder(lk) == self->pid,
           "mutex_unlock: process does not hold the lock");
    struct proc *next = __do_wakeup(lk);
    assert(!IS_ERR(next), "mutex_unlock: failed to wake up processes");
    spin_unlock(&lk->lk);
}

int holding_mutex(mutex_t *lk) {
    struct proc *self = myproc();
    if (self == NULL) {
        return 0; // No process context, can't be holding the lock
    }
    return __atomic_load_n(&lk->holder, __ATOMIC_SEQ_CST) == self->pid;
}

int mutex_trylock(mutex_t *lk) {
    struct proc *self = myproc();
    assert(self != NULL, "mutex_trylock: no current process");
    assert(mycpu()->spin_depth == 0, "mutex_trylock called with spinlock held");
    assert(!CPU_IN_ITR(), "mutex_trylock called in interrupt context");

    // Try to acquire the lock without blocking
    if (__mutex_try_set_holder(lk, self->pid)) {
        return 1; // Successfully acquired
    }
    return 0; // Failed to acquire
}
