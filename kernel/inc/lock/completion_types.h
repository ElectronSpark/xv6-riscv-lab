#ifndef __KERNEL_COMPLETION_TYPES_H
#define __KERNEL_COMPLETION_TYPES_H

#include <types.h>
#include <proc/proc_queue_type.h>

typedef struct {
    int done;
    proc_queue_t wait_queue;
    struct spinlock lock;
} completion_t;

#endif // __KERNEL_COMPLETION_TYPES_H