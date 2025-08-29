#ifndef SCHED_H
#define SCHED_H

#include "proc.h"
#include "proc_queue.h"

void scheduler_init(void);
int sched_holding(void);
void sched_lock(void);
void sched_unlock(void);
int chan_holding(void);
void sleep_lock(void);
void sleep_unlock(void);
void scheduler_run(void);
void scheduler_yield(struct spinlock *lk);
void scheduler_pause(struct spinlock *lk);
void scheduler_stop(struct proc *p);
void scheduler_continue(struct proc *p);
void scheduler_sleep(struct spinlock *lk);
void scheduler_wakeup(struct proc *p);
void scheduler_sleep_on_chan(void *chan, struct spinlock *lk);
void scheduler_wakeup_on_chan(void *chan);

// Wake up a sleeping process
// This function will aquire the locks of the process and the sched lock
void wakeup_proc(struct proc *p);

#endif // SCHED_H
