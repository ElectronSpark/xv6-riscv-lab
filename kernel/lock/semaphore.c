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

/*
 * sem_wait_interruptible - decrement semaphore, interruptible by signals.
 *
 * Like sem_wait() but uses THREAD_INTERRUPTIBLE sleep state so a pending
 * signal will wake the thread.  On interrupt the decrement is undone (via
 * __sem_do_post) and -EINTR is returned.
 *
 * Returns 0 on success, -EINTR if a signal arrived before acquisition.
 */
/*
 * sem_wait_timed - decrement semaphore with a millisecond deadline.
 *
 * Decrements atomically first (matching sem_post's wakeup condition), then
 * sleeps with a sched_timer deadline if the semaphore was not available.
 * On timer expiry the decrement is undone and -ETIMEDOUT is returned.
 *
 * Returns 0 on success, -ETIMEDOUT on timeout.
 */
int sem_wait_timed(sem_t *sem, uint64 timeout_ms) {
    assert(current != NULL, "sem_wait_timed called from non-thread context");
    assert(mycpu()->spin_depth == 0, "sem_wait_timed called with spinlock held");
    assert(!CPU_IN_ITR(), "sem_wait_timed called in interrupt context");
    if (sem == NULL) {
        return -EINVAL;
    }

    uint64 timeout_ticks = MS_TO_RAWTICKS(timeout_ms);
    uint64 start = r_time();

    spin_lock(&sem->lk);
    for (;;) {
        /* Decrement first: sem_post only calls tq_wakeup when value goes
         * from negative back toward 0, so we MUST decrement before sleeping
         * or a concurrent post will skip tq_wakeup entirely. */
        int val = __sem_value_dec(sem);
        if (val >= 0) {
            spin_unlock(&sem->lk);
            return 0; /* acquired without sleeping */
        }

        uint64 elapsed = r_time() - start;
        if (elapsed >= timeout_ticks) {
            __sem_value_inc(sem); /* undo the decrement */
            spin_unlock(&sem->lk);
            return -ETIMEDOUT;
        }

        uint64 remaining_ms = RAWTICKS_TO_MS(timeout_ticks - elapsed);
        if (remaining_ms == 0)
            remaining_ms = 1;

        struct timer_node tn = {0};
        sched_timer_set(&tn, remaining_ms);
        int ret = tq_wait(&sem->wait_queue, &sem->lk, NULL);
        sched_timer_done(&tn);
        if (ret != 0) {
            /* Timer fired before sem_post — undo the decrement */
            __sem_value_inc(sem);
            spin_unlock(&sem->lk);
            return -ETIMEDOUT;
        }
        /* Woken by sem_post: our decrement consumed the post */
        spin_unlock(&sem->lk);
        return 0;
    }
}

int sem_wait_interruptible(sem_t *sem) {
    assert(current != NULL, "sem_wait_interruptible called from non-thread context");
    assert(mycpu()->spin_depth == 0,
           "sem_wait_interruptible called with spinlock held");
    assert(!CPU_IN_ITR(), "sem_wait_interruptible called in interrupt context");
    if (sem == NULL) {
        return -EINVAL;
    }

    spin_lock(&sem->lk);

    if (signal_pending(current)) {
        spin_unlock(&sem->lk);
        return -EINTR;
    }

    int val = __sem_value_dec(sem);
    if (val < -SEM_VALUE_MAX) {
        __sem_value_inc(sem);
        spin_unlock(&sem->lk);
        return -EOVERFLOW;
    }
    if (val >= 0) {
        spin_unlock(&sem->lk);
        return 0; // acquired without sleeping
    }

    int ret = tq_wait_in_state(&sem->wait_queue, &sem->lk, NULL,
                               THREAD_INTERRUPTIBLE);
    if (ret != 0) {
        // Signal interrupted the sleep — undo the decrement
        int wake_ret = __sem_do_post(sem);
        if (wake_ret != 0 && wake_ret != -ENOENT) {
            printf(
                "sem_wait_interruptible: failed to post semaphore '%s' after "
                "interrupt\n",
                sem->name);
        }
        spin_unlock(&sem->lk);
        return -EINTR;
    }

    spin_unlock(&sem->lk);
    return 0;
}
