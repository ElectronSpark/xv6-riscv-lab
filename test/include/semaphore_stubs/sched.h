#ifndef SCHED_H
#define SCHED_H

#include "proc/thread_types.h"

typedef struct spinlock spinlock_t;

void scheduler_wakeup(struct thread *p);
void scheduler_sleep(spinlock_t *lk, enum thread_state state);

#endif /* SCHED_H */
