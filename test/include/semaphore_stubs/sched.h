#ifndef SCHED_H
#define SCHED_H

#include "proc/proc_types.h"

struct spinlock;

void scheduler_wakeup(struct proc *p);
void scheduler_sleep(struct spinlock *lk, enum procstate state);

#endif /* SCHED_H */
