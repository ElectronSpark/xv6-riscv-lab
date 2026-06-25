#include "types.h"
#include "string.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "proc/thread.h"
#include "defs.h"
#include "printf.h"
#include "proc/sched.h"
#include "lock/rcu.h"
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

/* Global monotonic millisecond counter for the sched_timer subsystem.
 * Refreshed from the architecture monotonic clock on any CPU.  Timed waits
 * can be armed by an arbitrary userspace thread, so depending on CPU 0 alone
 * to advance this counter can strand sleepers on hypervisors that deliver
 * periodic scheduling ticks to APs while the BSP is idle. */
static uint64 __sched_timer_ms;

static uint64 sched_timer_refresh_ms(void) {
    uint64 freq = __timebase_frequency ? __timebase_frequency : 10000000UL;
    uint64 now_ticks = r_time();
    uint64 now_ms;

    if (now_ticks <= (uint64)-1 / 1000ULL)
        now_ms = now_ticks * 1000ULL / freq;
    else
        now_ms = now_ticks / (freq / 1000ULL ? freq / 1000ULL : 1ULL);

    uint64 old = __atomic_load_n(&__sched_timer_ms, __ATOMIC_ACQUIRE);
    while (now_ms > old) {
        if (__atomic_compare_exchange_n(&__sched_timer_ms, &old, now_ms,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return now_ms;
    }
    return old;
}

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
    sched_timer_refresh_ms();
}

uint64 sched_timer_now_ms(void) {
    return sched_timer_refresh_ms();
}

void __do_timer_tick(void) {
    timer_tick(&__sched_timer, sched_timer_refresh_ms());
}

static void __sched_timer_callback(struct timer_node *tn) {
    // Wake up threads with expired timers.
    int pid = (int)(uint64)tn->data;
    if (pid <= 0)
        return;

    rcu_read_lock();
    struct thread *live = NULL;
    int valid = get_pid_thread(pid, &live) == 0 && live != NULL;
    if (valid && THREAD_SLEEPING(live)) {
        wakeup(live);
    }
    rcu_read_unlock();
}

int sched_timer_set(struct timer_node *tn, uint64 ms) {
    if (tn == NULL) {
        return -EINVAL; // Invalid timer node
    }
    uint64 expires = sched_timer_refresh_ms() + ms;
    /* Caller-owned timers back stack-based sleeps and timed waits. They must
     * fire once and be removed immediately; retrying a one-shot callback can
     * leave a stale timer node alive past the lifetime of its storage.
     */
    timer_node_init(tn, expires, __sched_timer_callback,
                    (void *)(uint64)current->pid, 1);
    int ret = timer_add(&__sched_timer, tn);
    return ret;
}

/**
 * sched_timer_set_cb - add a caller-owned timer node with a custom callback
 * @tn:       caller-allocated (or embedded) timer_node
 * @ms:       delay in milliseconds
 * @callback: function called when the timer fires (in soft-IRQ context)
 * @data:     opaque pointer passed as tn->data
 *
 * The caller may cancel the timer later with sched_timer_done(tn).
 */
int sched_timer_set_cb(struct timer_node *tn, uint64 ms,
                       void (*callback)(struct timer_node *), void *data) {
    if (tn == NULL || callback == NULL) {
        return -EINVAL;
    }
    uint64 expires = sched_timer_refresh_ms() + ms;
    /* Caller-managed callback timers are re-armed explicitly by their owner
     * when needed; repeated implicit retries create stale timer-node lifetime
     * hazards for contexts like timerfd and timed waits.
     */
    timer_node_init(tn, expires, callback, data, 1);
    return timer_add(&__sched_timer, tn);
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
    int ret = sched_timer_set(&tn, ms);
    if (ret != 0) {
        // Timer already expired or other transient failure — just yield
        // the CPU once instead of sleeping, which is the best approximation
        // for a very short sleep.
        __thread_state_set(p, THREAD_RUNNING);
        intr_restore(intr);
        scheduler_yield();
        return;
    }

    scheduler_yield();

    // After waking up, cancel the timer to avoid unnecessary callback
    sched_timer_done(&tn);
    intr_restore(intr);
}

/**
 * sleep_ms_interruptible - sleep for up to @ms milliseconds, interruptible
 *                          by pending signals.
 * @ms: requested sleep duration in milliseconds
 *
 * Returns the remaining time in milliseconds (0 if the full duration elapsed,
 * >0 if woken early by a signal).
 */
uint64 sleep_ms_interruptible(uint64 ms) {
    if (ms == 0) {
        return 0;
    }
    struct thread *p = current;
    assert(p != NULL, "Current thread must not be NULL");

    struct timer_node tn = {0};
    int intr = intr_off_save();

    __thread_state_set(p, THREAD_INTERRUPTIBLE);
    int ret = sched_timer_set(&tn, ms);
    if (ret != 0) {
        __thread_state_set(p, THREAD_RUNNING);
        intr_restore(intr);
        scheduler_yield();
        return 0;
    }

    scheduler_yield();

    // Compute remaining time before cancelling the timer
    uint64 remaining_ms = 0;
    uint64 now_ms = sched_timer_refresh_ms();
    if (tn.expires > now_ms) {
        remaining_ms = tn.expires - now_ms;
    }

    sched_timer_done(&tn);
    intr_restore(intr);
    return remaining_ms;
}

void sched_timer_init(void) {
    timer_init(&__sched_timer);
    sched_timer_refresh_ms();
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

int sched_timer_add(void (*callback)(void *), void *data, uint64 ms) {
    if (callback == NULL) {
        return -EINVAL; // Invalid callback
    }
    uint64 deadline = sched_timer_refresh_ms() + ms;
    return sched_timer_add_deadline(callback, data, deadline);
}
