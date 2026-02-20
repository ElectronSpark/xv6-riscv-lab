#ifndef __KERNEL_WORKQUEUE_TYPES_H
#define __KERNEL_WORKQUEUE_TYPES_H

#include "types.h"
#include "lock/spinlock.h"
#include "proc/tq_type.h"

#define WORKQUEUE_NAME_MAX 31

struct proc;
struct thread;
struct workqueue;

typedef void (*workqueue_lifecycle_cb_t)(struct workqueue *);
typedef void (*workqueue_thread_lifecycle_cb_t)(struct workqueue *,
                                                struct thread *);

struct workqueue_callbacks {
    workqueue_lifecycle_cb_t workqueue_ctor;
    workqueue_lifecycle_cb_t workqueue_dtor;
    workqueue_thread_lifecycle_cb_t manager_ctor;
    workqueue_thread_lifecycle_cb_t manager_dtor;
    workqueue_thread_lifecycle_cb_t worker_ctor;
    workqueue_thread_lifecycle_cb_t worker_dtor;
};

struct work_struct {
    list_node_t entry;
    void (*func)(struct work_struct *);
    void (*fault)(struct work_struct *);
    uint64 data;
    uint32 flags;
};

struct workqueue {
    spinlock_t lock;
    tq_t idle_queue;
    list_node_t worker_list;
    struct thread *manager;
    int pending_works;
    list_node_t work_list;
    char name[WORKQUEUE_NAME_MAX + 1];
    struct {
        uint64 active : 1;
        uint64 dtor_called : 1;
    };
    int nr_workers;
    int min_active;
    int max_active;
    struct workqueue_callbacks callbacks;
};

#endif // __KERNEL_WORKQUEUE_TYPES_H
