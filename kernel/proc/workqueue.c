#include "types.h"
#include "string.h"
#include "errno.h"
#include "param.h"
#include "printf.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "defs.h"
#include <mm/slab.h>
#include "proc/proc_queue.h"
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
    proc_queue_init(&wq->idle_queue, "workqueue_idle", &wq->lock);
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

// exit routine for worker processes
static void __exit_routine(uint64 exit_code) {
    proc_lock(myproc());
    struct workqueue *wq = myproc()->wq;
    proc_unlock(myproc());
    if (wq != NULL) {
        __wq_lock(wq);
        assert(wq->manager != myproc(), "Manager process try to exit using worker exit routine");
        proc_lock(myproc());
        if (!LIST_NODE_IS_DETACHED(myproc(), wq_entry)) {
            list_node_detach(myproc(), wq_entry);
        }
        proc_unlock(myproc());
        wq->nr_workers--;
        assert(wq->nr_workers >= 0, "Worker process count is invalid\n");
        __wq_unlock(wq);
    } else {
        proc_lock(myproc());
        assert (LIST_NODE_IS_DETACHED(myproc(), wq_entry),
                "Worker process not belong to a workqueue but attached\n");
        proc_unlock(myproc());
    }
    exit((int)exit_code);
}

// Worker routine for worker processes
// @TODO: exit after idling too long
static void __worker_routine(void) {
    proc_lock(myproc());
    struct workqueue *wq = myproc()->wq;
    if (wq == NULL) {
        proc_unlock(myproc());
        exit(-EINVAL);
    }
    proc_unlock(myproc());

    __wq_lock(wq);
    if (wq->manager == myproc()) {
        __wq_unlock(wq);
        exit(-EINVAL);
    }
    for (;;) {
        struct work_struct *work = __dequeue_work(wq);
        if (work == NULL) {
            if (!wq->active) {
                // If not more works to do, and the workqueue is inactive, just
                // exit the worker process
                __wq_unlock(wq);
                __exit_routine(0);
            }
            // Otherwise wait for work to be assigned
            int ret = proc_queue_wait(&wq->idle_queue, &wq->lock, (uint64*)&work);
            if (ret != 0) {
                __wq_unlock(wq);
                __exit_routine((uint64)ret);
            }
            // If a work is assigned to the worker process, it will be 
            // passed via `work` variable.
            // If the worker process is waken up but no work is assigned,
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

// This function will only try to acquire the work process lock
static int __create_worker(struct workqueue *wq) {
    struct proc *worker = NULL;
    int ret = kernel_proc_create("worker_process", &worker, __worker_routine, (uint64)wq, 0, KERNEL_STACK_ORDER);
    if (ret <= 0) {
        return ret;
    }
    assert(worker != NULL, "Failed to create worker process");
    proc_lock(worker);
    worker->wq = wq;
    wq->nr_workers++;
    list_node_push(&wq->worker_list, worker, wq_entry);
    proc_unlock(worker);
    wakeup_proc(worker);
    return 0;
}

// Manager routine for managing worker processes
// Each workqueue has a manager process that is responsible for creating and destroying worker processes
static void __manager_routine(void) {
    proc_lock(myproc());
    struct workqueue *wq = myproc()->wq;
    if (wq == NULL) {
        proc_unlock(myproc());
        exit(-EINVAL);
    }
    proc_unlock(myproc());

    __wq_lock(wq);
    if (wq->manager != myproc()) {
        __wq_unlock(wq);
        exit(-EINVAL);
    }
    for (;;) {
        assert(wq->nr_workers >= 0, "Worker process count is invalid\n");
        while (wq->nr_workers < wq->min_active || 
               (wq->pending_works > wq->nr_workers && 
                wq->nr_workers < wq->max_active)) {
            // Need to create more worker processes
            if (__create_worker(wq) != 0) {
                break;
            }
        }
        while (proc_queue_size(&wq->idle_queue) && 
               wq->nr_workers - proc_queue_size(&wq->idle_queue) < wq->pending_works) {
            // Wake up an idle worker if any
            struct proc *p = proc_queue_wakeup(&wq->idle_queue, 0, 0);
            if (IS_ERR_OR_NULL(p)) {
                printf("warning: Failed to wake up idle worker\n");
            }
        }
        scheduler_sleep(&wq->lock, PSTATE_INTERRUPTIBLE);
        // @TODO: handle signals
    }
    __wq_unlock(wq);
}

// Create a manager process for a work queue
// Will be called at the creation process of a work queue,
// during which the work queue lock is being hold.
// Thus, it will only try to hold the manager process lock
static int __create_manager(struct workqueue *wq) {
    struct proc *manager = NULL;
    int ret = kernel_proc_create("manager_process", &manager, __manager_routine, (uint64)wq, 0, KERNEL_STACK_ORDER);
    if (ret <= 0) {
        return ret;
    }
    assert(manager != NULL, "Failed to create manager process");
    proc_lock(manager);
    manager->wq = wq;
    proc_unlock(manager);
    wq->manager = manager;
    return 0;
}

// Try to wake up the manager process of a work queue
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
