#ifndef __KERNEL_WORKQUEUE_TYPES_H
#define __KERNEL_WORKQUEUE_TYPES_H

#include "types.h"
#include "lock/spinlock.h"
#include "proc/tq_type.h"

#define WORKQUEUE_NAME_MAX 31

struct proc;

struct work_struct {
    list_node_t entry;
    void (*func)(struct work_struct*);
    uint64 data;
};

struct workqueue {
    struct spinlock lock;
    tq_t idle_queue;
    list_node_t worker_list;
    struct proc *manager;
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
