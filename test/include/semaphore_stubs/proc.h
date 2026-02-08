#ifndef __KERNEL_THREAD_H
#define __KERNEL_THREAD_H

#include "types.h"

struct spinlock;

struct cpu {
    int dummy;
};

struct thread {
    pid_t pid;
    int dummy;
};

enum thread_state {
    THREAD_UNUSED = 0,
    THREAD_USED,
    THREAD_INTERRUPTIBLE,
    THREAD_KIILABLE,
    THREAD_TIMER,
    THREAD_KIILABLE_TIMER,
    THREAD_UNINTERRUPTIBLE,
    // THREAD_RUNNABLE,
    THREAD_RUNNING,
    THREAD_EXITING,
    THREAD_ZOMBIE,
};

#define THREAD_IS_SLEEPING(state)                              \
    ((state) == THREAD_INTERRUPTIBLE ||                        \
     (state) == THREAD_UNINTERRUPTIBLE ||                      \
     (state) == THREAD_KIILABLE ||                              \
     (state) == THREAD_TIMER ||                                 \
     (state) == THREAD_KIILABLE_TIMER)

#endif /* __KERNEL_THREAD_H */
