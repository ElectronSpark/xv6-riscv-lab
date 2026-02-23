#ifndef __TEST_STUB_PROC_SCHED_H
#define __TEST_STUB_PROC_SCHED_H

#include "proc/thread_types.h"
#include "timer/timer_types.h"

typedef struct spinlock spinlock_t;

void scheduler_wakeup(struct thread *p);
void scheduler_sleep(spinlock_t *lk, enum thread_state state);

void wakeup(struct thread *p);
void wakeup_on_chan(void *chan);
void sleep_on_chan(void *chan, spinlock_t *lk);
int sleep_on_chan_interruptible(void *chan, spinlock_t *lk);

int sched_timer_set(struct timer_node *tn, uint64 ticks);
void sched_timer_done(struct timer_node *tn);
void sleep_ms(uint64 ms);

#endif