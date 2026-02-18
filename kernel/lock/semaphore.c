#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "param.h"
#include "errno.h"
#include <mm/memlayout.h>
#include "lock/spinlock.h"
#include "lock/semaphore.h"
#include <smp/percpu.h>
#include "proc/thread.h"
#include "proc/tq.h"
#include "proc/sched.h"
#include "signal.h"
#include "timer/timer.h"

struct sem_timed_wait_ctx {
    sem_t *sem;
    struct timer_node timer;
    uint64 timeout_ms;
    bool timer_armed;
};

static int __sem_timed_sleep_cb(void *data) {
    struct sem_timed_wait_ctx *ctx = (struct sem_timed_wait_ctx *)data;
    if (ctx == NULL || ctx->sem == NULL) {
        return 0;
    }

    ctx->timer_armed = false;
    if (ctx->timeout_ms > 0) {
        if (sched_timer_set(&ctx->timer, ctx->timeout_ms) == 0) {
            ctx->timer_armed = true;
        }
    }

    int status = spin_holding(&ctx->sem->lk);
    if (status) {
        spin_unlock(&ctx->sem->lk);
    }
    return status;
}

static void __sem_timed_wake_cb(void *data, int sleep_cb_status) {
    struct sem_timed_wait_ctx *ctx = (struct sem_timed_wait_ctx *)data;
    if (ctx == NULL || ctx->sem == NULL) {
        return;
    }

    if (ctx->timer_armed) {
        sched_timer_done(&ctx->timer);
        ctx->timer_armed = false;
    }

    if (sleep_cb_status) {
        spin_lock(&ctx->sem->lk);
    }
}

static int __sem_value_inc(sem_t *sem) {
    return __atomic_add_fetch(&sem->value, 1, __ATOMIC_SEQ_CST);
}

static int __sem_value_dec(sem_t *sem) {
    return __atomic_add_fetch(&sem->value, -1, __ATOMIC_SEQ_CST);
}

static int __sem_value_get(sem_t *sem) {
    return __atomic_load_n(&sem->value, __ATOMIC_SEQ_CST);
}

int sem_init(sem_t *sem, const char *name, int value) {
    if (sem == NULL) {
        return -EINVAL;
    }
    if (value < 0) {
        return -EINVAL; // Semaphore value cannot be negative
    }
    sem->name = name ? name : "unnamed";
    sem->value = value;
    spin_init(&sem->lk, "semaphore spinlock");
    tq_init(&sem->wait_queue, "semaphore wait queue", &sem->lk);
    return 0;
}

static int __sem_do_post(sem_t *sem) {
    int val = __sem_value_inc(sem);
    if (val <= 0) {
        // If the semaphore value was or is negative, wake up one waiting thread
        struct thread *t = tq_wakeup(&sem->wait_queue, 0, 0);
        if (t == NULL) {
            return -ENOENT; // No thread to wake up
        }
        if (IS_ERR(t)) {
            return PTR_ERR(t);
        }
    }
    return 0;
}

int sem_wait(sem_t *sem) {
    assert(current != NULL, "sem_wait called from non-thread context");
    assert(mycpu()->spin_depth == 0, "sem_wait called with spinlock held");
    assert(!CPU_IN_ITR(), "sem_wait called in interrupt context");
    if (sem == NULL) {
        return -EINVAL;
    }

    spin_lock(&sem->lk);
    int val = __sem_value_dec(sem); // Decrement the semaphore value
    if (val < -SEM_VALUE_MAX) {
        // Prevent semaphore value from going below -SEM_VALUE_MAX
        __sem_value_inc(sem); // Revert the decrement
        spin_unlock(&sem->lk);
        return -EOVERFLOW;
    }
    if (val >= 0) {
        spin_unlock(&sem->lk);
        return 0; // Semaphore acquired successfully
    }

    __thread_state_set(current, THREAD_UNINTERRUPTIBLE);
    int ret = tq_wait(&sem->wait_queue, &sem->lk, NULL);
    if (ret != 0) {
        int wake_ret = __sem_do_post(sem);
        if (wake_ret != 0 && wake_ret != -ENOENT) {
            printf(
                "Failed to post semaphore '%s' when thread was interrupted\n",
                sem->name);
        }
    }

    spin_unlock(&sem->lk);
    return ret;
}

int sem_wait_interruptible(sem_t *sem) {
    assert(current != NULL,
           "sem_wait_interruptible called from non-thread context");
    assert(mycpu()->spin_depth == 0,
           "sem_wait_interruptible called with spinlock held");
    assert(!CPU_IN_ITR(), "sem_wait_interruptible called in interrupt context");
    if (sem == NULL) {
        return -EINVAL;
    }

    while (1) {
        int ret = sem_trywait(sem);
        if (ret == 0) {
            return 0;
        }
        if (ret != -EAGAIN) {
            return ret;
        }
        if (signal_pending(current)) {
            return -EINTR;
        }
        sleep_ms(1);
    }
}

int sem_timedwait(sem_t *sem, uint64 timeout_ms) {
    assert(current != NULL, "sem_timedwait called from non-thread context");
    assert(mycpu()->spin_depth == 0,
           "sem_timedwait called with spinlock held");
    assert(!CPU_IN_ITR(), "sem_timedwait called in interrupt context");
    if (sem == NULL) {
        return -EINVAL;
    }

    if (timeout_ms == 0) {
        return sem_trywait(sem) == 0 ? 0 : -ETIMEDOUT;
    }

    spin_lock(&sem->lk);
    int val = __sem_value_dec(sem);
    if (val < -SEM_VALUE_MAX) {
        __sem_value_inc(sem);
        spin_unlock(&sem->lk);
        return -EOVERFLOW;
    }
    if (val >= 0) {
        spin_unlock(&sem->lk);
        return 0;
    }

    uint64 timeout_ticks = MS_TO_RAWTICKS(timeout_ms);
    uint64 start = r_time();
    struct sem_timed_wait_ctx ctx = {
        .sem = sem,
        .timeout_ms = timeout_ms,
        .timer_armed = false,
    };

    __thread_state_set(current, THREAD_INTERRUPTIBLE);
    int ret = tq_wait_cb(&sem->wait_queue, __sem_timed_sleep_cb,
                         __sem_timed_wake_cb, &ctx, NULL);

    if (ret != 0) {
        int wake_ret = __sem_do_post(sem);
        if (wake_ret != 0 && wake_ret != -ENOENT) {
            printf("Failed to post semaphore '%s' after timed wait wakeup\n",
                   sem->name);
        }

        if (signal_pending(current)) {
            spin_unlock(&sem->lk);
            return -EINTR;
        }
        if ((r_time() - start) >= timeout_ticks) {
            spin_unlock(&sem->lk);
            return -ETIMEDOUT;
        }
    }

    spin_unlock(&sem->lk);
    return ret;
}

int sem_trywait(sem_t *sem) {
    if (sem == NULL) {
        return -EINVAL;
    }
    spin_lock(&sem->lk);
    if (__sem_value_get(sem) > 0) {
        __sem_value_dec(sem);
        spin_unlock(&sem->lk);
        return 0;
    }
    spin_unlock(&sem->lk);
    return -EAGAIN;
}

int sem_post(sem_t *sem) {
    if (sem == NULL) {
        return -EINVAL;
    }

    spin_lock(&sem->lk);
    if (__sem_value_get(sem) == SEM_VALUE_MAX) {
        spin_unlock(&sem->lk);
        return -EOVERFLOW; // Prevent semaphore value from exceeding
                           // SEM_VALUE_MAX
    }
    int ret = __sem_do_post(sem);
    spin_unlock(&sem->lk);
    if (ret == -ENOENT) {
        // No thread to wake up, not an error
        return 0;
    }
    return ret;
}

int sem_getvalue(sem_t *sem, int *value) {
    if (sem == NULL || value == NULL) {
        return -EINVAL;
    }

    spin_lock(&sem->lk);
    *value = __sem_value_get(sem);
    spin_unlock(&sem->lk);
    return 0;
}
