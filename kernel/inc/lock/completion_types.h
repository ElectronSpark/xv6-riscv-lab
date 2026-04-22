#ifndef __KERNEL_COMPLETION_TYPES_H
#define __KERNEL_COMPLETION_TYPES_H

#include <types.h>
#include <proc/tq_type.h>

typedef struct {
    spinlock_t lock;
    int done;
    tq_t wait_queue;
} completion_t;

#endif // __KERNEL_COMPLETION_TYPES_H