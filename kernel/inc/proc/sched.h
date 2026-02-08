#ifndef SCHED_H
#define SCHED_H

#include "proc/thread_types.h"
#include "proc/tq.h"
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
void scheduler_sleep(struct spinlock *lk, enum thread_state sleep_state);
void scheduler_wakeup(struct thread *p);
void scheduler_wakeup_timeout(struct thread *p);
void scheduler_wakeup_killable(struct thread *p);
void scheduler_wakeup_interruptible(struct thread *p);
void scheduler_wakeup_stopped(struct thread *p);
void sleep_on_chan(void *chan, struct spinlock *lk);
void wakeup_on_chan(void *chan);

void idle_thread_init(void);

// Context Switching Helpers
// Note context_switch_prepare will not acquire rq_lock,
// so caller must ensure rq_lock is held before calling it.
// And context_switch_finish will release the rq_lock of the target CPU
void context_switch_prepare(struct thread *prev, struct thread *next);
void context_switch_finish(struct thread *prev, struct thread *next, int intr);

// Timer related
void sched_timer_init(void);
void sched_timer_tick(void);
int sched_timer_set(struct timer_node *tn, uint64 ticks);
void sched_timer_done(struct timer_node *tn);
void sleep_ms(uint64 ms);
int sched_timer_add_deadline(void (*callback)(void *), void *data,
                             uint64 deadline);
int sched_timer_add(void (*callback)(void *), void *data, uint64 ticks);

// Wake up a sleeping threads
// This function will aquire the locks of the threads and the sched lock
void wakeup(struct thread *p);
void wakeup_timeout(struct thread *p);
void wakeup_killable(struct thread *p);
void wakeup_interruptible(struct thread *p);

#endif // SCHED_H
