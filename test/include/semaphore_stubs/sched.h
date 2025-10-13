#ifndef SCHED_H
#define SCHED_H

#include "proc.h"

struct spinlock;

void sched_lock(void);
void sched_unlock(void);
void scheduler_wakeup(struct proc *p);
void scheduler_sleep(struct spinlock *lk, enum procstate state);

#endif /* SCHED_H */
