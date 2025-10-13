#ifndef __KERNEL_PROC_H
#define __KERNEL_PROC_H

#include "types.h"

struct spinlock;

struct cpu {
    int dummy;
};

struct proc {
    int dummy;
};

enum procstate {
    PSTATE_UNUSED = 0,
    PSTATE_USED,
    PSTATE_INTERRUPTIBLE,
    STATE_KILLABLE,
    STATE_TIMER,
    STATE_KILLABLE_TIMER,
    PSTATE_UNINTERRUPTIBLE,
    PSTATE_RUNNABLE,
    PSTATE_RUNNING,
    PSTATE_EXITING,
    PSTATE_ZOMBIE,
};

#define PSTATE_IS_SLEEPING(state)                              \
    ((state) == PSTATE_INTERRUPTIBLE ||                        \
     (state) == PSTATE_UNINTERRUPTIBLE ||                      \
     (state) == STATE_KILLABLE ||                              \
     (state) == STATE_TIMER ||                                 \
     (state) == STATE_KILLABLE_TIMER)

#endif /* __KERNEL_PROC_H */
