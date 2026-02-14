#ifndef __KERNEL_PIPE_TYPES_H
#define __KERNEL_PIPE_TYPES_H

#include "lock/spinlock.h"
#include "proc/tq_type.h"
#include "smp/atomic.h"

struct pipe {
    spinlock_t reader_lock;
    uint nread; // number of bytes read
    tq_t nread_queue;
    spinlock_t writer_lock;
    uint nwrite; // number of bytes written
    tq_t nwrite_queue;
    int flags;
    char *data;
};

#endif // __KERNEL_PIPE_TYPES_H
