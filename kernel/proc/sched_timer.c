#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"
#include "sched.h"
#include "slab.h"
#include "errno.h"
#include "timer.h"
#include "sched_timer_private.h"

static struct timer_root __sched_timer;
static bool __sched_tick_clear;

void scheduler_timer_tick(void) {
    __atomic_clear(&__sched_tick_clear, __ATOMIC_RELEASE);
}

void __do_timer_tick(void) {
    bool was_cleared = __atomic_test_and_set(&__sched_tick_clear, __ATOMIC_ACQUIRE);
    if (!was_cleared) {
        timer_tick(&__sched_timer, get_jiffs());
    }
}

static void __sched_timer_callback(struct timer_node *tn) {
    // Wake up processes with expired timers.
    struct proc *p = tn->data;
    if (PROC_SLEEPING(p)) {
        wakeup_proc(p);
    }
}

int scheduler_timer_set(struct timer_node *tn, uint64 ticks) {
    if (tn == NULL) {
        return -EINVAL; // Invalid timer node
    }
    uint64 expires = get_jiffs() + ticks;
    timer_node_init(tn, expires, __sched_timer_callback, myproc());
    int ret = timer_add(&__sched_timer, tn);
    return ret;
}

void scheduler_timer_done(struct timer_node *tn) {
    if (tn == NULL) {
        return;
    }
    timer_remove(tn);
}

void sleep_ms(uint64 ms) {
    if (ms == 0) {
        return;
    }
    struct proc *p = myproc();
    assert(p != NULL, "Current process must not be NULL");

    struct timer_node tn = {0};

    int ret = scheduler_timer_set(&tn, ms);
    if (ret != 0) {
        printf("Failed to set timer\n");
        return;
    }

    proc_lock(p);
    __proc_set_pstate(p, PSTATE_UNINTERRUPTIBLE);
    scheduler_sleep(NULL);
    proc_unlock(p);

    // After waking up, cancel the timer to avoid unnecessary callback
    scheduler_timer_done(&tn);
}

void __sched_timer_init(void) {
    timer_init(&__sched_timer);
    __sched_tick_clear = false;
}