/*
 * Scheduler wrappers for unit tests
 */
#include "types.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "spinlock.h"

void __wrap_scheduler_wakeup(struct thread *p) {
    (void)p;
    // No-op for tests
}

void __wrap_scheduler_sleep(spinlock_t *lk, enum thread_state state) {
    (void)lk;
    (void)state;
    // No-op for tests
}
