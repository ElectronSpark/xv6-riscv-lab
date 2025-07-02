#ifndef KERNEL_PROC_QUEUE_TYPE_H
#define KERNEL_PROC_QUEUE_TYPE_H

#include "types.h"
#include "list_type.h"
#include "spinlock.h"

typedef struct proc_queue proc_queue_t;

typedef struct proc_queue_entry {
    list_node_t list_entry;
    proc_queue_t *queue; // Pointer to the queue this entry belongs to
} proc_queue_entry_t;

typedef struct proc_queue {
    list_node_t head; // List of processes in the queue
    int counter; // Number of processes in the queue
    const char *name; // Name of the queue
    uint64 flags;
} proc_queue_t;

#define PROC_QUEUE_FLAG_VALID   (1UL << 0) // indicates that the queue is valid
#define PROC_QUEUE_FLAG_LOCK    (1UL << 1) // will try to acquire spinlock before using the queue

#endif // KERNEL_PROC_QUEUE_TYPE_H