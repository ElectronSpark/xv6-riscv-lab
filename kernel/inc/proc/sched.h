#ifndef SCHED_H
#define SCHED_H

#include "proc/proc.h"
#include "proc/proc_queue.h"
#include "timer/timer_types.h"

void scheduler_init(void);
int sched_holding(void);
int chan_holding(void);
void sleep_lock(void);
void sleep_unlock(void);
int sleep_lock_irqsave(void);
void sleep_unlock_irqrestore(int state);
void scheduler_run(void);
void scheduler_yield(void);
void scheduler_pause(struct spinlock *lk);
void scheduler_stop(struct proc *p);
void scheduler_continue(struct proc *p);
void scheduler_sleep(struct spinlock *lk, enum procstate sleep_state);
void scheduler_wakeup(struct proc *p);
void scheduler_wakeup_timeout(struct proc *p);
void scheduler_wakeup_killable(struct proc *p);
void scheduler_wakeup_interruptible(struct proc *p);
void sleep_on_chan(void *chan, struct spinlock *lk);
void wakeup_on_chan(void *chan);

void idle_proc_init(void);

// Context Switching Helpers
// Note context_switch_prepare will not acquire rq_lock,
// so caller must ensure rq_lock is held before calling it.
// And context_switch_finish will release the rq_lock of the target CPU
void context_switch_prepare(struct proc *prev, struct proc *next);
void context_switch_finish(struct proc *prev, struct proc *next, int intr);

// Timer related
void sched_timer_init(void);
void sched_timer_tick(void);
int sched_timer_set(struct timer_node *tn, uint64 ticks);
void sched_timer_done(struct timer_node *tn);
void sleep_ms(uint64 ms);
int sched_timer_add_deadline(void (*callback)(void *), void *data, uint64 deadline);
int sched_timer_add(void (*callback)(void *), void *data, uint64 ticks);

// Wake up a sleeping process
// This function will aquire the locks of the process and the sched lock
void wakeup_proc(struct proc *p);
void wakeup_timeout(struct proc *p);
void wakeup_killable(struct proc *p);
void wakeup_interruptible(struct proc *p);

#endif // SCHED_H
