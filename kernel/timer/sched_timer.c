#include "types.h"
#include "string.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "proc/thread.h"
#include "defs.h"
#include "printf.h"
#include "proc/sched.h"
#include <mm/slab.h>
#include "errno.h"
#include "timer/timer.h"
#include "proc/workqueue.h"
#include "timer/sched_timer_private.h"

struct sched_timer_work {
    struct timer_node tn;
    struct work_struct work;
    void (*callback)(void *);
    void *data;
};

static void __work_callback(struct work_struct *work);
static void __timer_callback(struct timer_node *tn);
static void __free_sched_timer_work(struct sched_timer_work *stw);
static struct sched_timer_work *
__alloc_sched_timer_work(uint64 deadline, void (*callback)(void *), void *data);

static slab_cache_t __sched_timer_work_slab = {0};
static struct workqueue *__sched_timer_wq = NULL;
static struct timer_root __sched_timer;
static bool __sched_tick_clear;

static void __work_callback(struct work_struct *work) {
    struct sched_timer_work *stw = (struct sched_timer_work *)work->data;
    if (stw == NULL || stw->callback == NULL) {
        printf("warning: __work_callback: Invalid work or callback\n");
        return;
    }
    timer_remove(&stw->tn);
    stw->callback(stw->data);
    __free_sched_timer_work(stw);
}

static void __timer_callback(struct timer_node *tn) {
    struct sched_timer_work *stw = (struct sched_timer_work *)tn->data;
    if (stw == NULL) {
        printf("warning: __timer_callback: Invalid work\n");
        return;
    }
    bool ret = queue_work(__sched_timer_wq, &stw->work);
    if (!ret) {
        printf("warning: __sched_timer_add_timer_callback: Failed to queue "
               "work\n");
        __free_sched_timer_work(stw);
    }
}

static struct sched_timer_work *
__alloc_sched_timer_work(uint64 deadline, void (*callback)(void *),
                         void *data) {
    struct sched_timer_work *stw = slab_alloc(&__sched_timer_work_slab);
    if (stw == NULL) {
        return NULL;
    }
    memset(stw, 0, sizeof(*stw));
    stw->callback = callback;
    stw->data = data;
    // Because no thread is waiting for the timer, we can set retry_limit to 1
    timer_node_init(&stw->tn, deadline, __timer_callback, stw, 1);
    init_work_struct(&stw->work, __work_callback, (uint64)stw);
    return stw;
}

static void __free_sched_timer_work(struct sched_timer_work *stw) {
    if (stw == NULL) {
        return;
    }
    slab_free(stw);
}

void sched_timer_tick(void) {
    __atomic_clear(&__sched_tick_clear, __ATOMIC_RELEASE);
}

void __do_timer_tick(void) {
    bool was_cleared =
        __atomic_test_and_set(&__sched_tick_clear, __ATOMIC_ACQUIRE);
    if (!was_cleared) {
        timer_tick(&__sched_timer, get_jiffs());
    }
}

static void __sched_timer_callback(struct timer_node *tn) {
    // Wake up threads with expired timers.
    struct thread *p = tn->data;
    if (THREAD_SLEEPING(p)) {
        wakeup(p);
    }
}

int sched_timer_set(struct timer_node *tn, uint64 ticks) {
    if (tn == NULL) {
        return -EINVAL; // Invalid timer node
    }
    uint64 expires = get_jiffs() + ticks;
    timer_node_init(tn, expires, __sched_timer_callback, current,
                    TIMER_DEFAULT_RETRY_LIMIT);
    int ret = timer_add(&__sched_timer, tn);
    return ret;
}

void sched_timer_done(struct timer_node *tn) {
    if (tn == NULL) {
        return;
    }
    timer_remove(tn);
}

void sleep_ms(uint64 ms) {
    if (ms == 0) {
        return;
    }
    struct thread *p = current;
    assert(p != NULL, "Current thread must not be NULL");

    struct timer_node tn = {0};
    // Disable interrupts for the entire sleep/wake sequence to prevent
    // the timer callback from racing with our state transitions.
    int intr = intr_off_save();

    // Set state to INTERRUPTIBLE so scheduler_yield dequeues us.
    __thread_state_set(p, THREAD_INTERRUPTIBLE);
    uint64 before = get_jiffs();
    int ret = sched_timer_set(&tn, ms);
    if (ret != 0) {
        __thread_state_set(p, THREAD_RUNNING);
        intr_restore(intr);
        printf("thread %s: ", p ? p->name : "unknown");
        printf("Failed to set timer - ret=%d, before=%lu\n", ret, before);
        return;
    }

    scheduler_yield();

    // After waking up, cancel the timer to avoid unnecessary callback
    sched_timer_done(&tn);
    intr_restore(intr);
}

void sched_timer_init(void) {
    timer_init(&__sched_timer);
    __sched_tick_clear = false;
    __sched_timer_wq =
        workqueue_create("sched_timer_wq", WORKQUEUE_DEFAULT_MAX_ACTIVE);
    assert(__sched_timer_wq != NULL,
           "Failed to create scheduler timer workqueue");
    int ret =
        slab_cache_init(&__sched_timer_work_slab, "sched_timer_work_slab",
                        sizeof(struct sched_timer_work), SLAB_FLAG_EMBEDDED);
    assert(ret == 0, "Failed to initialize sched_timer_work_slab");
}

int sched_timer_add_deadline(void (*callback)(void *), void *data,
                             uint64 deadline) {
    if (callback == NULL) {
        return -EINVAL; // Invalid callback
    }
    struct sched_timer_work *stw =
        __alloc_sched_timer_work(deadline, callback, data);
    if (stw == NULL) {
        return -ENOMEM; // Memory allocation failed
    }
    int ret = timer_add(&__sched_timer, &stw->tn);
    if (ret != 0) {
        __free_sched_timer_work(stw);
        return ret; // Failed to add timer
    }
    return 0;
}

int sched_timer_add(void (*callback)(void *), void *data, uint64 ticks) {
    if (callback == NULL) {
        return -EINVAL; // Invalid callback
    }
    uint64 deadline = get_jiffs() + ticks;
    return sched_timer_add_deadline(callback, data, deadline);
}
