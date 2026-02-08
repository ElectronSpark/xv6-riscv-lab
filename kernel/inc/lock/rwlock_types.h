#ifndef __KERNEL_RWLOCK_TYPES_H
#define __KERNEL_RWLOCK_TYPES_H

#include "proc/tq_type.h"
#include "spinlock.h"

typedef struct rwlock {
    struct spinlock lock; // Spinlock to protect the rwlock structure
    int readers;          // Number of active readers
    pid_t holder_pid; // Thread holding write lock, if any
    tq_t read_queue;  // Queue for threads waiting to read
    tq_t write_queue; // Queue for threads waiting to write
    const char *name; // Name of the rwlock
    uint64 flags; // Additional flags for rwlock behavior
} rwlock_t;

#define RWLOCK_PRIO_READ 0x0 // Priority for readers (default)
#define RWLOCK_PRIO_WRITE 0x1 // Priority for writers

#endif // __KERNEL_RWLOCK_TYPES_H
