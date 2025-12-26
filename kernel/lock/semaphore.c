#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "param.h"
#include "errno.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "proc_queue.h"
#include "sched.h"
#include "semaphore.h"

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
    proc_queue_init(&sem->wait_queue, "semaphore wait queue", &sem->lk);
    return 0;
}

static int __sem_do_post(sem_t *sem) {
    int val = __sem_value_inc(sem);
    if (val <= 0) {
        // If the semaphore value was or is negative, wake up one waiting process
        return proc_queue_wakeup(&sem->wait_queue, 0, 0, NULL);
    }
    return 0;
}

int sem_wait(sem_t *sem) {
    if (sem == NULL) {
        return -EINVAL;
    }

    spin_acquire(&sem->lk);
    int val = __sem_value_dec(sem); // Decrement the semaphore value
    if (val < -SEM_VALUE_MAX) {
        // Prevent semaphore value from going below -SEM_VALUE_MAX
        __sem_value_inc(sem); // Revert the decrement
        spin_release(&sem->lk);
        return -EOVERFLOW;
    }
    if (val >= 0) {
        spin_release(&sem->lk);
        return 0; // Semaphore acquired successfully
    }

    int ret = proc_queue_wait(&sem->wait_queue, &sem->lk, NULL);
    if (ret != 0) {
        int wake_ret = __sem_do_post(sem);
        if (wake_ret != 0 && wake_ret != -ENOENT) {
            printf("Failed to post semaphore '%s' when process was interrupted\n", sem->name);
        }
    }

    spin_release(&sem->lk);
    return ret;
}

int sem_trywait(sem_t *sem) {
    if (sem == NULL) {
        return -EINVAL;
    }
    spin_acquire(&sem->lk);
    if (__sem_value_get(sem) > 0) {
        __sem_value_dec(sem);
        spin_release(&sem->lk);
        return 0;
    }
    spin_release(&sem->lk);
    return -EAGAIN;
}

int sem_post(sem_t *sem) {
    if (sem == NULL) {
        return -EINVAL;
    }

    spin_acquire(&sem->lk);
    if (__sem_value_get(sem) == SEM_VALUE_MAX) {
        spin_release(&sem->lk);
        return -EOVERFLOW; // Prevent semaphore value from exceeding SEM_VALUE_MAX
    }
    int ret = __sem_do_post(sem);
    spin_release(&sem->lk);
    if (ret == -ENOENT) {
        // No process to wake up, not an error
        return 0;
    }
    return ret;
}

int sem_getvalue(sem_t *sem, int *value) {
    if (sem == NULL || value == NULL) {
        return -EINVAL;
    }

    spin_acquire(&sem->lk);
    *value = __sem_value_get(sem);
    spin_release(&sem->lk);
    return 0;
}
