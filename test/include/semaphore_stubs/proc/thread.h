#ifndef __TEST_STUB_PROC_THREAD_H
#define __TEST_STUB_PROC_THREAD_H

#include "proc/thread_types.h"

struct thread *kthread_create(const char *name, void *entry, uint64 arg1,
                              uint64 arg2, int stack_order);

#define THREAD_IS_SLEEPING(state)                                              \
    ((state) == THREAD_INTERRUPTIBLE || (state) == THREAD_UNINTERRUPTIBLE ||   \
     (state) == THREAD_KIILABLE || (state) == THREAD_TIMER ||                  \
     (state) == THREAD_KIILABLE_TIMER)

static inline void __thread_state_set(struct thread *p, enum thread_state state)
{
    if (p != NULL) {
        p->state = (int)state;
    }
}

#endif