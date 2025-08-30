#ifndef __KERNEL_WORKQUEUE_TYPES_H
#define __KERNEL_WORKQUEUE_TYPES_H

#include "types.h"
#include "spinlock.h"
#include "proc_queue_type.h"

#define WORKQUEUE_NAME_MAX 31

struct work_struct {
    list_node_t entry;
    void (*func)(void *);
    void *data;
};

struct workqueue {
    struct spinlock lock;
    proc_queue_t idle_queue;
    list_node_t worker_list;
    int pending_works;
    list_node_t work_list;
    char name[WORKQUEUE_NAME_MAX + 1];
    struct {
        uint64 active: 1;
    };
    int nr_workers;
    int min_active;
    int max_active;
};

#endif // __KERNEL_WORKQUEUE_TYPES_H
