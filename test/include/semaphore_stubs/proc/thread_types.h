#ifndef __TEST_STUB_PROC_THREAD_TYPES_H
#define __TEST_STUB_PROC_THREAD_TYPES_H

#include "types.h"

typedef struct spinlock spinlock_t;

struct thread {
    pid_t pid;
    int state;
};

enum thread_state {
    THREAD_UNUSED = 0,
    THREAD_USED,
    THREAD_INTERRUPTIBLE,
    THREAD_KIILABLE,
    THREAD_TIMER,
    THREAD_KIILABLE_TIMER,
    THREAD_UNINTERRUPTIBLE,
    THREAD_WAKENING,
    THREAD_RUNNING,
    THREAD_STOPPED,
    THREAD_EXITING,
    THREAD_ZOMBIE,
};

#endif