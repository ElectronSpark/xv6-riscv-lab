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
#include "signal.h"
#include "uabi/signo.h"
#include "lock/completion.h"
#include <mm/slab.h>
#include "proc/tq.h"
#include "proc/workqueue.h"

static slab_cache_t __workqueue_cache;
static slab_cache_t __work_struct_cache;

static int __create_manager(struct workqueue *wq);
static void __wakeup_manager(struct workqueue *wq);
static void __invoke_workqueue_ctor(struct workqueue *wq);
static void __invoke_workqueue_dtor(struct workqueue *wq);
static void __invoke_manager_ctor(struct workqueue *wq, struct thread *manager);
static void __invoke_manager_dtor(struct workqueue *wq, struct thread *manager);
static void __invoke_worker_ctor(struct workqueue *wq, struct thread *worker);
static void __invoke_worker_dtor(struct workqueue *wq, struct thread *worker);

static bool __work_flags_valid(uint32 flags) {
    return (flags & ~WORK_STRUCT_FLAG_VALID_MASK) == 0;
}

static bool __work_struct_valid(struct work_struct *work) {
    if (work == NULL) {
        return false;
    }
    if (!__work_flags_valid(work->flags)) {
        return false;
    }
    return work->func != NULL || work->fault != NULL;
}

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

static void __wq_lock(struct workqueue *wq) { spin_lock(&wq->lock); }

static void __wq_unlock(struct workqueue *wq) { spin_unlock(&wq->lock); }

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
                      void (*func)(struct work_struct *), uint64 data) {
    init_work_struct_ex(work, func, NULL, data, WORK_STRUCT_DEFAULT_FLAGS);
}

void init_work_struct_flags(struct work_struct *work,
                            void (*func)(struct work_struct *), uint64 data,
                            uint32 flags) {
    init_work_struct_ex(work, func, NULL, data, flags);
}

void init_work_struct_ex(struct work_struct *work,
                         void (*func)(struct work_struct *),
                         void (*fault)(struct work_struct *), uint64 data,
                         uint32 flags) {
    if (work == NULL) {
        return;
    }
    assert(__work_flags_valid(flags),
           "init_work_struct_ex: invalid work flags");
    list_entry_init(&work->entry);
    work->func = func;
    work->fault = fault;
    work->data = data;
    work->flags = flags;
}

// Dynamically allocate a work struct and initialize it with the given function
// and data
struct work_struct *create_work_struct(void (*func)(struct work_struct *),
                                       uint64 data) {
    return create_work_struct_flags(func, data, WORK_STRUCT_DEFAULT_FLAGS);
}

struct work_struct *create_work_struct_flags(void (*func)(struct work_struct *),
                                             uint64 data, uint32 flags) {
    return create_work_struct_ex(func, NULL, data, flags);
}

struct work_struct *create_work_struct_ex(void (*func)(struct work_struct *),
                                          void (*fault)(struct work_struct *),
                                          uint64 data, uint32 flags) {
    if (!__work_flags_valid(flags)) {
        return NULL;
    }
    struct work_struct *work = __alloc_work_struct();
    if (!work) {
        return NULL;
    }
    init_work_struct_ex(work, func, fault, data, flags);
    return work;
}

// Free a work struct
// This function can only be used to free work structs allocated by
// create_work_struct
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
    assert(LIST_NODE_IS_DETACHED(work, entry),
           "enqueue_work: work struct is already enqueued");
    list_node_push_back(&wq->work_list, work, entry);
    wq->pending_works++;
}

// Try to pop a work from a workqueue
// No validation checks to the parameters
// Caller should hold the lock of the wq
static struct work_struct *__dequeue_work(struct workqueue *wq) {
    struct work_struct *work =
        list_node_pop(&wq->work_list, struct work_struct, entry);
    if (work != NULL) {
        wq->pending_works--;
    }
    return work;
}

// exit routine for worker threads
static void __exit_worker_routine(uint64 exit_code) {
    tcb_lock(current);
    struct workqueue *wq = current->wq;
    tcb_unlock(current);
    if (wq != NULL) {
        __invoke_worker_dtor(wq, current);
        __wq_lock(wq);
        assert(wq->manager != current,
               "Manager thread try to exit using worker exit routine");
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
        assert(LIST_NODE_IS_DETACHED(current, wq_entry),
               "Worker thread not belong to a workqueue but attached\n");
        tcb_unlock(current);
    }
    exit((int)exit_code);
}

static void __exit_manager_routine(uint64 exit_code) {
    tcb_lock(current);
    struct workqueue *wq = current->wq;
    tcb_unlock(current);
    if (wq != NULL) {
        __invoke_manager_dtor(wq, current);
        __wq_lock(wq);
        if (wq->manager == current) {
            wq->manager = NULL;
        }
        __wq_unlock(wq);
    }
    exit((int)exit_code);
}

static bool __work_flag(struct work_struct *work, uint32 flag) {
    return !!(work->flags & flag);
}

static void __execute_work(struct work_struct *work, bool queue_active) {
    bool run_work =
        queue_active || __work_flag(work, WORK_STRUCT_FLAG_RUN_ON_DRAIN);
    if (run_work) {
        if (!queue_active && work->fault != NULL) {
            work->fault(work);
        } else if (work->func != NULL) {
            work->func(work);
        }
    }
    if (__work_flag(work, WORK_STRUCT_FLAG_FREE_AFTER_RUN)) {
        free_work_struct(work);
    }
}

static void __invoke_workqueue_ctor(struct workqueue *wq) {
    if (wq->callbacks.workqueue_ctor != NULL) {
        wq->callbacks.workqueue_ctor(wq);
    }
}

static void __invoke_workqueue_dtor(struct workqueue *wq) {
    if (wq->callbacks.workqueue_dtor != NULL) {
        wq->callbacks.workqueue_dtor(wq);
    }
}

static bool __mark_workqueue_dtor_once(struct workqueue *wq) {
    bool should_call = !wq->dtor_called;
    if (should_call) {
        wq->dtor_called = 1;
    }
    return should_call;
}

static void __invoke_manager_ctor(struct workqueue *wq,
                                  struct thread *manager) {
    if (wq->callbacks.manager_ctor != NULL) {
        wq->callbacks.manager_ctor(wq, manager);
    }
}

static void __invoke_manager_dtor(struct workqueue *wq,
                                  struct thread *manager) {
    if (wq->callbacks.manager_dtor != NULL) {
        wq->callbacks.manager_dtor(wq, manager);
    }
}

static void __invoke_worker_ctor(struct workqueue *wq, struct thread *worker) {
    if (wq->callbacks.worker_ctor != NULL) {
        wq->callbacks.worker_ctor(wq, worker);
    }
}

static void __invoke_worker_dtor(struct workqueue *wq, struct thread *worker) {
    if (wq->callbacks.worker_dtor != NULL) {
        wq->callbacks.worker_dtor(wq, worker);
    }
}

static void __wakeup_all_idle_workers(struct workqueue *wq) {
    int ret = tq_wakeup_all(&wq->idle_queue, 0, 0);
    if (ret < 0) {
        printf("warning: failed to wake idle workers for workqueue %s\n",
               wq->name);
    }
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
    __wq_unlock(wq);

    __invoke_worker_ctor(wq, current);

    for (;;) {
        __wq_lock(wq);
        if (THREAD_KILLED(current)) {
            __wq_unlock(wq);
            __exit_worker_routine(0);
        }
        if (!wq->active) {
            struct work_struct *work = __dequeue_work(wq);
            if (work == NULL) {
                __wq_unlock(wq);
                __exit_worker_routine(0);
            }
            __wq_unlock(wq);
            __execute_work(work, false);
            continue;
        }
        struct work_struct *work = __dequeue_work(wq);
        if (work == NULL) {
            // Otherwise wait for work to be assigned
            __thread_state_set(current, THREAD_INTERRUPTIBLE);
            tq_wait(&wq->idle_queue, &wq->lock, (uint64 *)&work);
            assert(spin_holding(&wq->lock),
                   "tq_wait should return with workqueue lock held");

            // If a work is assigned to the worker thread, it will be
            // passed via `work` variable.
            // If the worker thread is woken up but no work is assigned,
            // tq_wait has already re-acquired wq->lock (via spin_wake_cb),
            // so we must unlock before continuing to avoid lock reentry on
            // the next loop iteration.
            if (work == NULL) {
                __wq_unlock(wq);
                continue;
            }
        }
        // Found a work to do
        __wq_unlock(wq);
        __execute_work(work, true);
    }
}

// This function will only try to acquire the work thread lock
static int __create_worker(struct workqueue *wq) {
    struct thread *worker = kthread_create("worker_thread", __worker_routine,
                                           (uint64)wq, 0, KERNEL_STACK_ORDER);
    if (IS_ERR_OR_NULL(worker)) {
        return PTR_ERR_OR(worker, -ENOMEM);
    }
    tcb_lock(worker);
    worker->wq = wq;
    wq->nr_workers++;
    list_node_push(&wq->worker_list, worker, wq_entry);
    tcb_unlock(worker);
    wakeup(worker);
    return 0;
}

// Manager routine for managing worker threads
// Each workqueue has a manager thread that is responsible for creating and
// destroying worker threads
static void __manager_routine(void) {
    tcb_lock(current);
    struct workqueue *wq = current->wq;
    if (wq == NULL) {
        tcb_unlock(current);
        exit(-EINVAL);
    }
    tcb_unlock(current);

    __wq_lock(wq);
    wq->manager = current;
    __wq_unlock(wq);

    __invoke_manager_ctor(wq, current);

    __wq_lock(wq);
    for (;;) {
        if (THREAD_KILLED(current)) {
            if (wq->active) {
                if (__create_manager(wq) == 0) {
                    __wakeup_manager(wq);
                }
            } else {
                __wakeup_all_idle_workers(wq);
            }
            __wq_unlock(wq);
            __exit_manager_routine(0);
        }

        if (!wq->active) {
            __wakeup_all_idle_workers(wq);
            __wq_unlock(wq);
            __exit_manager_routine(0);
        }
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
    struct thread *manager = kthread_create("manager_thread", __manager_routine,
                                            (uint64)wq, 0, KERNEL_STACK_ORDER);
    if (IS_ERR_OR_NULL(manager)) {
        return PTR_ERR_OR(manager, -ENOMEM);
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
    if (wq == NULL || wq->manager == NULL) {
        return;
    }
    scheduler_wakeup(wq->manager);
}

void workqueue_init(void) {
    int ret = slab_cache_init(&__workqueue_cache, "workqueue",
                              sizeof(struct workqueue), SLAB_FLAG_EMBEDDED);
    assert(ret == 0, "Failed to initialize workqueue slab cache");
    ret = slab_cache_init(&__work_struct_cache, "work_struct",
                          sizeof(struct work_struct), SLAB_FLAG_EMBEDDED);
    assert(ret == 0, "Failed to initialize work_struct slab cache");
    printf("workqueue subsystem initialized\n");
}

struct workqueue *workqueue_create(const char *name, int max_active) {
    return workqueue_create_with_callbacks(name, max_active, NULL);
}

struct workqueue *
workqueue_create_with_callbacks(const char *name, int max_active,
                                const struct workqueue_callbacks *callbacks) {
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
    if (callbacks != NULL) {
        wq->callbacks = *callbacks;
    }
    wq->max_active = max_active;
    wq->min_active = max_active < WORKQUEUE_DEFAULT_MIN_ACTIVE
                         ? WORKQUEUE_DEFAULT_MIN_ACTIVE
                         : max_active;
    wq->nr_workers = 0;
    wq->active = 1;

    __invoke_workqueue_ctor(wq);

    __wq_lock(wq);
    if (__create_manager(wq) != 0) {
        bool should_call_dtor = __mark_workqueue_dtor_once(wq);
        __wq_unlock(wq);
        if (should_call_dtor) {
            __invoke_workqueue_dtor(wq);
        }
        __free_workqueue(wq);
        return NULL;
    }
    __wakeup_manager(wq);
    __wq_unlock(wq);

    return wq;
}

int workqueue_kill(struct workqueue *wq) {
    if (wq == NULL) {
        return -EINVAL;
    }

    __wq_lock(wq);
    if (wq->active == 0) {
        __wq_unlock(wq);
        return 0;
    }
    wq->active = 0;
    bool should_call_dtor = __mark_workqueue_dtor_once(wq);
    struct thread *manager = wq->manager;
    __wq_unlock(wq);

    if (should_call_dtor) {
        __invoke_workqueue_dtor(wq);
    }

    if (manager != NULL) {
        int ret = kill_thread(manager, SIGKILL);
        if (ret < 0) {
            THREAD_SET_KILLED(manager);
        }
        scheduler_wakeup(manager);
    }

    return 0;
}

bool queue_work(struct workqueue *wq, struct work_struct *work) {
    if (wq == NULL || !__work_struct_valid(work)) {
        return false;
    }
    if (!LIST_NODE_IS_DETACHED(work, entry)) {
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

#ifdef WORKQUEUE_RUNTIME_SMOKE_TEST
struct __wq_smoke_ctx {
    completion_t normal_done;
    completion_t fault_done;
    completion_t dtor_done;
    volatile int release_blockers;
    volatile int blocker_started;
    volatile int ctor_calls;
    volatile int dtor_calls;
    volatile int normal_calls;
    volatile int fault_calls;
    volatile int unexpected_normal_calls;
    volatile int stress_runs;
};

static struct __wq_smoke_ctx *__wq_smoke_ctx_ptr;

static void __wq_smoke_workqueue_ctor(struct workqueue *wq) {
    (void)wq;
    struct __wq_smoke_ctx *ctx = __wq_smoke_ctx_ptr;
    if (ctx != NULL) {
        __atomic_add_fetch(&ctx->ctor_calls, 1, __ATOMIC_SEQ_CST);
    }
}

static void __wq_smoke_workqueue_dtor(struct workqueue *wq) {
    (void)wq;
    struct __wq_smoke_ctx *ctx = __wq_smoke_ctx_ptr;
    if (ctx != NULL) {
        __atomic_add_fetch(&ctx->dtor_calls, 1, __ATOMIC_SEQ_CST);
        complete(&ctx->dtor_done);
    }
}

static void __wq_smoke_normal_work(struct work_struct *work) {
    struct __wq_smoke_ctx *ctx = (struct __wq_smoke_ctx *)work->data;
    __atomic_add_fetch(&ctx->normal_calls, 1, __ATOMIC_SEQ_CST);
    complete(&ctx->normal_done);
}

static void __wq_smoke_stress_work(struct work_struct *work) {
    struct __wq_smoke_ctx *ctx = (struct __wq_smoke_ctx *)work->data;
    __atomic_add_fetch(&ctx->stress_runs, 1, __ATOMIC_SEQ_CST);
}

static void __wq_smoke_blocker_work(struct work_struct *work) {
    struct __wq_smoke_ctx *ctx = (struct __wq_smoke_ctx *)work->data;
    __atomic_add_fetch(&ctx->blocker_started, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&ctx->release_blockers, __ATOMIC_SEQ_CST)) {
        sleep_ms(1);
    }
}

static void __wq_smoke_unexpected_normal(struct work_struct *work) {
    struct __wq_smoke_ctx *ctx = (struct __wq_smoke_ctx *)work->data;
    __atomic_add_fetch(&ctx->unexpected_normal_calls, 1, __ATOMIC_SEQ_CST);
}

static void __wq_smoke_fault_work(struct work_struct *work) {
    struct __wq_smoke_ctx *ctx = (struct __wq_smoke_ctx *)work->data;
    __atomic_add_fetch(&ctx->fault_calls, 1, __ATOMIC_SEQ_CST);
    complete(&ctx->fault_done);
}

void workqueue_runtime_smoke_test(void) {
    enum { WQ_SMOKE_STRESS_WORKS = 128 };

    struct __wq_smoke_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    completion_init(&ctx.normal_done);
    completion_init(&ctx.fault_done);
    completion_init(&ctx.dtor_done);
    __wq_smoke_ctx_ptr = &ctx;

    struct workqueue_callbacks callbacks = {
        .workqueue_ctor = __wq_smoke_workqueue_ctor,
        .workqueue_dtor = __wq_smoke_workqueue_dtor,
    };

    struct workqueue *wq =
        workqueue_create_with_callbacks("wq_smoke", 2, &callbacks);
    assert(wq != NULL, "workqueue smoke: create failed");
    assert(__atomic_load_n(&ctx.ctor_calls, __ATOMIC_SEQ_CST) == 1,
           "workqueue smoke: ctor callback not invoked once");

    for (int i = 0; i < WQ_SMOKE_STRESS_WORKS; i++) {
        struct work_struct *stress = create_work_struct_ex(
            __wq_smoke_stress_work, NULL, (uint64)&ctx,
            WORK_STRUCT_FLAG_RUN_ON_DRAIN | WORK_STRUCT_FLAG_FREE_AFTER_RUN);
        assert(stress != NULL, "workqueue smoke: stress work alloc failed");
        assert(queue_work(wq, stress), "workqueue smoke: stress queue failed");
    }

    int stress_wait = 0;
    while (__atomic_load_n(&ctx.stress_runs, __ATOMIC_SEQ_CST) <
               WQ_SMOKE_STRESS_WORKS &&
           stress_wait < 5000) {
        sleep_ms(1);
        stress_wait++;
    }
    assert(__atomic_load_n(&ctx.stress_runs, __ATOMIC_SEQ_CST) ==
               WQ_SMOKE_STRESS_WORKS,
           "workqueue smoke: stress work did not fully complete");

    struct work_struct *normal = create_work_struct_ex(
        __wq_smoke_normal_work, NULL, (uint64)&ctx,
        WORK_STRUCT_FLAG_RUN_ON_DRAIN | WORK_STRUCT_FLAG_FREE_AFTER_RUN);
    assert(normal != NULL, "workqueue smoke: normal work alloc failed");
    assert(queue_work(wq, normal), "workqueue smoke: normal queue failed");
    assert(wait_for_completion_timed(&ctx.normal_done, 3000) == 0,
           "workqueue smoke: normal work did not complete");

    struct work_struct *blocker1 = create_work_struct_ex(
        __wq_smoke_blocker_work, NULL, (uint64)&ctx,
        WORK_STRUCT_FLAG_RUN_ON_DRAIN | WORK_STRUCT_FLAG_FREE_AFTER_RUN);
    struct work_struct *blocker2 = create_work_struct_ex(
        __wq_smoke_blocker_work, NULL, (uint64)&ctx,
        WORK_STRUCT_FLAG_RUN_ON_DRAIN | WORK_STRUCT_FLAG_FREE_AFTER_RUN);
    assert(blocker1 != NULL && blocker2 != NULL,
           "workqueue smoke: blocker alloc failed");
    assert(queue_work(wq, blocker1), "workqueue smoke: blocker1 queue failed");
    assert(queue_work(wq, blocker2), "workqueue smoke: blocker2 queue failed");

    int wait_ms = 0;
    while (__atomic_load_n(&ctx.blocker_started, __ATOMIC_SEQ_CST) < 2 &&
           wait_ms < 3000) {
        sleep_ms(1);
        wait_ms++;
    }
    assert(__atomic_load_n(&ctx.blocker_started, __ATOMIC_SEQ_CST) >= 2,
           "workqueue smoke: blockers not started");

    struct work_struct *faulted = create_work_struct_ex(
        __wq_smoke_unexpected_normal, __wq_smoke_fault_work, (uint64)&ctx,
        WORK_STRUCT_FLAG_RUN_ON_DRAIN | WORK_STRUCT_FLAG_FREE_AFTER_RUN);
    assert(faulted != NULL, "workqueue smoke: fault work alloc failed");
    assert(queue_work(wq, faulted), "workqueue smoke: fault work queue failed");

    assert(workqueue_kill(wq) == 0, "workqueue smoke: kill failed");
    assert(workqueue_kill(wq) == 0, "workqueue smoke: second kill failed");
    __atomic_store_n(&ctx.release_blockers, 1, __ATOMIC_SEQ_CST);

    assert(wait_for_completion_timed(&ctx.fault_done, 5000) == 0,
           "workqueue smoke: fault callback did not run");
    assert(wait_for_completion_timed(&ctx.dtor_done, 3000) == 0,
           "workqueue smoke: dtor callback did not run");

    assert(__atomic_load_n(&ctx.unexpected_normal_calls, __ATOMIC_SEQ_CST) == 0,
           "workqueue smoke: normal callback ran during exit for faulted work");
    assert(__atomic_load_n(&ctx.fault_calls, __ATOMIC_SEQ_CST) == 1,
           "workqueue smoke: fault callback count mismatch");
    assert(__atomic_load_n(&ctx.dtor_calls, __ATOMIC_SEQ_CST) == 1,
           "workqueue smoke: dtor callback count mismatch");

    struct work_struct *rejected = create_work_struct_ex(
        __wq_smoke_stress_work, __wq_smoke_fault_work, (uint64)&ctx,
        WORK_STRUCT_FLAG_RUN_ON_DRAIN | WORK_STRUCT_FLAG_FREE_AFTER_RUN);
    assert(rejected != NULL, "workqueue smoke: rejected work alloc failed");
    assert(!queue_work(wq, rejected),
           "workqueue smoke: queue accepted work after kill");
    free_work_struct(rejected);

    __wq_smoke_ctx_ptr = NULL;
    printf("workqueue_smoke: PASS\n");
}

#else

void workqueue_runtime_smoke_test(void) {}

#endif
