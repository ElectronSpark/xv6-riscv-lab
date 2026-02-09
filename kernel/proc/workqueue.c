#include "types.h"
#include "string.h"
#include "errno.h"
#include "param.h"
#include "printf.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "defs.h"
#include <mm/slab.h>
#include "proc/tq.h"
#include "proc/workqueue.h"

static slab_cache_t __workqueue_cache;
static slab_cache_t __work_struct_cache;

static void __free_workqueue(struct workqueue *wq) {
    if (wq == NULL) {
        return;
    }
    slab_free(wq);
}

static struct workqueue *__alloc_workqueue(void) {
    struct workqueue *wq = slab_alloc(&__workqueue_cache);
    if (!wq) {
        return NULL;
    }
    memset(wq, 0, sizeof(struct workqueue));
    return wq;
}

static void __workqueue_struct_init(struct workqueue *wq) {
    if (wq == NULL) {
        return;
    }
    memset(wq, 0, sizeof(struct workqueue));
    list_entry_init(&wq->worker_list);
    list_entry_init(&wq->work_list);
    spin_init(&wq->lock, "workqueue_lock");
    tq_init(&wq->idle_queue, "workqueue_idle", &wq->lock);
}

static void __wq_lock(struct workqueue *wq) {
    spin_lock(&wq->lock);
}

static void __wq_unlock(struct workqueue *wq) {
    spin_unlock(&wq->lock);
}

static struct work_struct *__alloc_work_struct(void) {
    struct work_struct *work = slab_alloc(&__work_struct_cache);
    if (!work) {
        return NULL;
    }
    memset(work, 0, sizeof(struct work_struct));
    return work;
}

static void __free_work_struct(struct work_struct *work) {
    if (!work) {
        return;
    }
    slab_free(work);
}

// Initialize a work item
void init_work_struct(struct work_struct *work, 
                      void (*func)(struct work_struct*), 
                      uint64 data) {
    list_entry_init(&work->entry);
    work->func = func;
    work->data = data;
}

// Dynamically allocate a work struct and initialize it with the given function and data
struct work_struct *create_work_struct(void (*func)(struct work_struct*), 
                                       uint64 data) {
    struct work_struct *work = __alloc_work_struct();
    if (!work) {
        return NULL;
    }
    init_work_struct(work, func, data);
    return work;
}

// Free a work struct
// This function can only be used to free work structs allocated by create_work_struct
void free_work_struct(struct work_struct *work) {
    if (!work) {
        return;
    }
    __free_work_struct(work);
}

// Push a work onto a workqueue
// No validation checks to the parameters
// Caller should hold the lock of the wq
static void __enqueue_work(struct workqueue *wq, struct work_struct *work) {
    list_node_push_back(&wq->work_list, work, entry);
    wq->pending_works++;
}

// Try to pop a work from a workqueue
// No validation checks to the parameters
// Caller should hold the lock of the wq
static struct work_struct *__dequeue_work(struct workqueue *wq) {
    struct work_struct *work = list_node_pop(&wq->work_list, 
                                             struct work_struct, entry);
    if (work != NULL) {
        wq->pending_works--;
    }
    return work;
}

// exit routine for worker threads
static void __exit_routine(uint64 exit_code) {
    tcb_lock(current);
    struct workqueue *wq = current->wq;
    tcb_unlock(current);
    if (wq != NULL) {
        __wq_lock(wq);
        assert(wq->manager != current, "Manager thread try to exit using worker exit routine");
        tcb_lock(current);
        if (!LIST_NODE_IS_DETACHED(current, wq_entry)) {
            list_node_detach(current, wq_entry);
        }
        tcb_unlock(current);
        wq->nr_workers--;
        assert(wq->nr_workers >= 0, "Worker thread count is invalid\n");
        __wq_unlock(wq);
    } else {
        tcb_lock(current);
        assert (LIST_NODE_IS_DETACHED(current, wq_entry),
                "Worker thread not belong to a workqueue but attached\n");
        tcb_unlock(current);
    }
    exit((int)exit_code);
}

// Worker routine for worker threads
// @TODO: exit after idling too long
static void __worker_routine(void) {
    tcb_lock(current);
    struct workqueue *wq = current->wq;
    if (wq == NULL) {
        tcb_unlock(current);
        exit(-EINVAL);
    }
    tcb_unlock(current);

    __wq_lock(wq);
    if (wq->manager == current) {
        __wq_unlock(wq);
        exit(-EINVAL);
    }
    for (;;) {
        struct work_struct *work = __dequeue_work(wq);
        if (work == NULL) {
            if (!wq->active) {
                // If not more works to do, and the workqueue is inactive, just
                // exit the worker thread
                __wq_unlock(wq);
                __exit_routine(0);
            }
            // Otherwise wait for work to be assigned
            int ret = tq_wait(&wq->idle_queue, &wq->lock, (uint64*)&work);
            if (ret != 0) {
                __wq_unlock(wq);
                __exit_routine((uint64)ret);
            }
            // If a work is assigned to the worker thread, it will be 
            // passed via `work` variable.
            // If the worker thread is waken up but no work is assigned,
            // Then enter the next loop and try to get a work from the queue.
            if (work == NULL) {
                continue;
            }
        }
        // Found a work to do
        __wq_unlock(wq);
        work->func(work);
        __wq_lock(wq);
    }
    __wq_unlock(wq);

    __exit_routine(0);
}

// This function will only try to acquire the work thread lock
static int __create_worker(struct workqueue *wq) {
    struct thread *worker = NULL;
    int ret = kthread_create("worker_thread", &worker, __worker_routine, (uint64)wq, 0, KERNEL_STACK_ORDER);
    if (ret <= 0) {
        return ret;
    }
    assert(worker != NULL, "Failed to create worker thread");
    tcb_lock(worker);
    worker->wq = wq;
    wq->nr_workers++;
    list_node_push(&wq->worker_list, worker, wq_entry);
    tcb_unlock(worker);
    wakeup(worker);
    return 0;
}

// Manager routine for managing worker threads
// Each workqueue has a manager thread that is responsible for creating and destroying worker threads
static void __manager_routine(void) {
    tcb_lock(current);
    struct workqueue *wq = current->wq;
    if (wq == NULL) {
        tcb_unlock(current);
        exit(-EINVAL);
    }
    tcb_unlock(current);

    __wq_lock(wq);
    if (wq->manager != current) {
        __wq_unlock(wq);
        exit(-EINVAL);
    }
    for (;;) {
        assert(wq->nr_workers >= 0, "Worker thread count is invalid\n");
        while (wq->nr_workers < wq->min_active || 
               (wq->pending_works > wq->nr_workers && 
                wq->nr_workers < wq->max_active)) {
            // Need to create more worker threads
            if (__create_worker(wq) != 0) {
                break;
            }
        }
        while (tq_size(&wq->idle_queue) && 
               wq->nr_workers - tq_size(&wq->idle_queue) < wq->pending_works) {
            // Wake up an idle worker if any
            struct thread *p = tq_wakeup(&wq->idle_queue, 0, 0);
            if (IS_ERR_OR_NULL(p)) {
                printf("warning: Failed to wake up idle worker\n");
            }
        }
        // Mark interruptible and release the lock before yielding so that
        // workers can acquire wq->lock to dequeue work items.  Re-acquire
        // on wakeup to re-evaluate the loop condition.
        __thread_state_set(current, THREAD_INTERRUPTIBLE);
        spin_unlock(&wq->lock);
        scheduler_yield();
        spin_lock(&wq->lock);
    }
    __wq_unlock(wq);
}

// Create a manager thread for a work queue
// Will be called at the creation process of a work queue,
// during which the work queue lock is being hold.
// Thus, it will only try to hold the manager thread lock
static int __create_manager(struct workqueue *wq) {
    struct thread *manager = NULL;
    int ret = kthread_create("manager_thread", &manager, __manager_routine, (uint64)wq, 0, KERNEL_STACK_ORDER);
    if (ret <= 0) {
        return ret;
    }
    assert(manager != NULL, "Failed to create manager thread");
    tcb_lock(manager);
    manager->wq = wq;
    tcb_unlock(manager);
    wq->manager = manager;
    return 0;
}

// Try to wake up the manager thread of a work queue
// Note: pi_lock is acquired internally by scheduler_wakeup
static void __wakeup_manager(struct workqueue *wq) {
    scheduler_wakeup(wq->manager);
}

void workqueue_init(void) {
    int ret = slab_cache_init(&__workqueue_cache, "workqueue", 
                              sizeof(struct workqueue), 
                              SLAB_FLAG_EMBEDDED);
    assert(ret == 0, "Failed to initialize workqueue slab cache");
    ret = slab_cache_init(&__work_struct_cache, "work_struct", 
                          sizeof(struct work_struct), 
                          SLAB_FLAG_EMBEDDED);
    assert(ret == 0, "Failed to initialize work_struct slab cache");
    printf("workqueue subsystem initialized\n");
}

struct workqueue *workqueue_create(const char *name, int max_active) {
    if (max_active < 0) {
        return NULL;
    }
    if (max_active == 0) {
        max_active = WORKQUEUE_DEFAULT_MAX_ACTIVE;
    } else if (max_active > MAX_WORKQUEUE_ACTIVE) {
        max_active = MAX_WORKQUEUE_ACTIVE;
    }
    if (name == NULL) {
        name = "unnamed";
    }

    struct workqueue *wq = __alloc_workqueue();
    if (!wq) {
        return NULL;
    }
    __workqueue_struct_init(wq);
    strncpy(wq->name, name, sizeof(wq->name) - 1);
    wq->max_active = max_active;
    wq->min_active = max_active < WORKQUEUE_DEFAULT_MIN_ACTIVE ? WORKQUEUE_DEFAULT_MIN_ACTIVE : max_active;
    wq->nr_workers = 0;
    wq->active = 1;

    __wq_lock(wq);
    if (__create_manager(wq) != 0) {
        __free_workqueue(wq);
        // __wq_unlock(wq);
        return NULL;
    }
    __wakeup_manager(wq);
    __wq_unlock(wq);

    return wq;
}

bool queue_work(struct workqueue *wq, struct work_struct *work) {
    if (wq == NULL || work == NULL || work->func == NULL) {
        return false;
    }
    __wq_lock(wq);
    if (wq->active == 0) {
        // Workqueue is inactive, reject new works
        __wq_unlock(wq);
        return false;
    }
    
    __enqueue_work(wq, work);
    __wakeup_manager(wq);
    __wq_unlock(wq);
    return true;
}
