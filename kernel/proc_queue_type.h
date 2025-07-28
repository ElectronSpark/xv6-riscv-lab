#ifndef KERNEL_PROC_QUEUE_TYPE_H
#define KERNEL_PROC_QUEUE_TYPE_H

#include "types.h"
#include "list_type.h"
#include "spinlock.h"

typedef struct proc_queue proc_queue_t;

typedef struct proc_node {
    list_node_t list_entry;
    proc_queue_t *queue; // Pointer to the queue this entry belongs to
    struct proc *proc;   // Pointer to the process this node represents
} proc_node_t;

typedef struct proc_queue {
    list_node_t head; // List of processes in the queue
    int counter; // Number of processes in the queue
    const char *name; // Name of the queue
    spinlock_t *lock; // Spinlock for the queue, it's optional
    uint64 flags;
} proc_queue_t;

#define PROC_QUEUE_FLAG_VALID   (1UL << 0) // indicates that the queue is valid
#define PROC_QUEUE_FLAG_LOCK    (1UL << 1) // will try to acquire spinlock before using the queue

#endif // KERNEL_PROC_QUEUE_TYPE_H