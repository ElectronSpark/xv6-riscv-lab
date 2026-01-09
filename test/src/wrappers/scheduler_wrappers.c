/*
 * Scheduler wrappers for unit tests
 */
#include "types.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "spinlock.h"

void __wrap_sched_lock(void) {
    // No-op for tests
}

void __wrap_sched_unlock(void) {
    // No-op for tests
}

void __wrap_scheduler_wakeup(struct proc *p) {
    (void)p;
    // No-op for tests
}

void __wrap_scheduler_sleep(struct spinlock *lk, enum procstate state) {
    (void)lk;
    (void)state;
    // No-op for tests
}
