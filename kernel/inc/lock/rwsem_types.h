#ifndef __KERNEL_RWLOCK_TYPES_H
#define __KERNEL_RWLOCK_TYPES_H

#include "proc/tq_type.h"
#include "spinlock.h"

typedef struct rwsem {
    spinlock_t lock;  // Spinlock to protect the rwsem structure
    int readers;      // Number of active readers
    pid_t holder_pid; // Thread holding write lock, if any
    uint64 holder_since_ms; // Time when the current writer acquired the lock
    char holder_name[16];   // Snapshot of the writer thread name
    void *holder_caller;    // Caller that acquired the writer lock
    uint64 reader_since_ms; // Time when the current reader batch began
    pid_t reader_first_pid; // First reader in the current batch
    char reader_first_name[16]; // Snapshot of the first reader thread name
    void *reader_first_caller;  // Caller that acquired the first read lock
    tq_t read_queue;  // Queue for threads waiting to read
    tq_t write_queue; // Queue for threads waiting to write
    const char *name; // Name of the rwsem
    uint64 flags;     // Additional flags for rwsem behavior
} rwsem_t;

#define RWLOCK_PRIO_READ 0x0  // Priority for readers (default)
#define RWLOCK_PRIO_WRITE 0x1 // Priority for writers

#endif // __KERNEL_RWLOCK_TYPES_H
