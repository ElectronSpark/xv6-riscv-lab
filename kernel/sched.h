#ifndef SCHED_H
#define SCHED_H

#include "proc.h"
#include "proc_queue.h"

void scheduler_init(void);
int sched_holding(void);
void sched_lock(void);
void sched_unlock(void);
void scheduler_run(void);
int scheduler_yield(uint64 *ret_arg, struct spinlock *lk);
void scheduler_sleep(struct spinlock *lk);
void scheduler_wakeup(struct proc *p);
void scheduler_sleep_on_chan(void *chan, struct spinlock *lk);
void scheduler_wakeup_on_chan(void *chan);

#endif // SCHED_H
