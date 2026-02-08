#ifndef SCHED_H
#define SCHED_H

#include "proc/thread_types.h"

struct spinlock;

void scheduler_wakeup(struct thread *p);
void scheduler_sleep(struct spinlock *lk, enum thread_state state);

#endif /* SCHED_H */
