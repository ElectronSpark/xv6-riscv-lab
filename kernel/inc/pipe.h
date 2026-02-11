#ifndef __KERNEL_PIPE_H
#define __KERNEL_PIPE_H

#include "lock/spinlock.h"
#include "proc/tq_type.h"
#include <smp/atomic.h>

#define PIPESIZE 512

#define PIPE_FLAGS_READABLE 1
#define PIPE_FLAGS_WRITABLE 2
#define PIPE_FLAGS_RW ((1 << PIPE_FLAGS_READABLE) | (1 << PIPE_FLAGS_WRITABLE))

#define PIPE_WRITABLE(pi)                                                      \
    (!!(smp_load_acquire(&(pi)->flags) & (1 << PIPE_FLAGS_WRITABLE)))
#define PIPE_READABLE(pi)                                                      \
    (!!(smp_load_acquire(&(pi)->flags) & (1 << PIPE_FLAGS_READABLE)))
// Clear the writable flag; returns true if both readable and writable are cleared
// after the operation
#define PIPE_CLEAR_WRITABLE(pi)                                                \
    ({                                                                         \
        uint64 __flags;                                                        \
        __flags = __atomic_fetch_and(                                          \
            &(pi)->flags, ~(1 << PIPE_FLAGS_WRITABLE), __ATOMIC_SEQ_CST);      \
        !!(__flags == (1 << PIPE_FLAGS_WRITABLE));                             \
    })
#define PIPE_CLEAR_READABLE(pi)                                                \
    ({                                                                         \
        uint64 __flags;                                                        \
        __flags = __atomic_fetch_and(                                          \
            &(pi)->flags, ~(1 << PIPE_FLAGS_READABLE), __ATOMIC_SEQ_CST);      \
        !!(__flags == (1 << PIPE_FLAGS_READABLE));                             \
    })

struct pipe {
    spinlock_t reader_lock;
    uint nread; // number of bytes read
    tq_t nread_queue;
    spinlock_t writer_lock;
    uint nwrite; // number of bytes written
    tq_t nwrite_queue;
    int flags;
    char data[PIPESIZE];
};

#endif // __KERNEL_PIPE_H
