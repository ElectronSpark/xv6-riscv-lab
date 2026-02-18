#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "param.h"
#include <mm/memlayout.h>
#include "lock/spinlock.h"
#include "lock/rwsem.h"
#include <smp/percpu.h>
#include "proc/thread.h"
#include "proc/tq.h"
#include "proc/sched.h"
#include "string.h"
#include "signal.h"
#include "timer/timer.h"

struct rwsem_timed_wait_ctx {
    rwsem_t *lock;
    struct timer_node timer;
    uint64 timeout_ms;
    bool timer_armed;
};

static int __rwsem_timed_sleep_cb(void *data) {
    struct rwsem_timed_wait_ctx *ctx = (struct rwsem_timed_wait_ctx *)data;
    if (ctx == NULL || ctx->lock == NULL) {
        return 0;
    }

    ctx->timer_armed = false;
    if (ctx->timeout_ms > 0 && sched_timer_set(&ctx->timer, ctx->timeout_ms) == 0) {
        ctx->timer_armed = true;
    }

    int status = spin_holding(&ctx->lock->lock);
    if (status) {
        spin_unlock(&ctx->lock->lock);
    }
    return status;
}

static void __rwsem_timed_wake_cb(void *data, int sleep_cb_status) {
    struct rwsem_timed_wait_ctx *ctx = (struct rwsem_timed_wait_ctx *)data;
    if (ctx == NULL || ctx->lock == NULL) {
        return;
    }

    if (ctx->timer_armed) {
        sched_timer_done(&ctx->timer);
        ctx->timer_armed = false;
    }

    if (sleep_cb_status) {
        spin_lock(&ctx->lock->lock);
    }
}

static inline int __reader_should_wait(rwsem_t *lock) {
    if (lock->readers == 0) {
        return lock->holder_pid != -1; // -1 = no holder
    }
    if ((lock->flags & RWLOCK_PRIO_WRITE) && tq_size(&lock->write_queue) > 0) {
        return 1;
    }
    return 0;
}

static inline int __writer_should_wait(rwsem_t *lock, int pid) {
    if (lock->holder_pid == pid) {
        // The caller already holds the write lock
        return 0;
    }
    if (lock->holder_pid != -1) { // -1 = no holder
        return 1;
    }
    if (lock->readers > 0) {
        return 1;
    }
    return 0;
}

static void __wake_readers(rwsem_t *lock) {
    int ret = tq_wakeup_all(&lock->read_queue, 0, 0);
    assert(ret >= 0, "rwsem: failed to wake readers");
}

static void __wake_writer(rwsem_t *lock) {
    struct thread *next = tq_wakeup(&lock->write_queue, 0, 0);
    assert(!IS_ERR_OR_NULL(next), "rwsem: failed to wake writer");
    lock->holder_pid = next->pid;
}

// wake up readers or a writer depending on the lock's priority.
static void __do_wake_up(rwsem_t *lock) {
    if (lock->flags & RWLOCK_PRIO_WRITE) {
        // If the lock is in write priority mode, first try to wake up the next
        // writer
        if (tq_size(&lock->write_queue) > 0) {
            __wake_writer(lock);
        } else if (tq_size(&lock->read_queue) > 0) {
            __wake_readers(lock);
        }
    } else {
        // If the lock is in read priority mode, first try to wake up all
        // readers
        if (tq_size(&lock->read_queue) > 0) {
            __wake_readers(lock);
        } else if (tq_size(&lock->write_queue) > 0) {
            __wake_writer(lock);
        }
    }
}

int rwsem_init(rwsem_t *lock, uint64 flags, const char *name) {
    if (!lock || !name) {
        return -1; // Invalid parameters
    }

    spin_init(&lock->lock, "rwsem spinlock");
    lock->readers = 0;
    tq_init(&lock->read_queue, "rwsem read queue", &lock->lock);
    tq_init(&lock->write_queue, "rwsem write queue", &lock->lock);
    lock->name = name;
    lock->holder_pid = -1; // -1 = no holder (0 is valid PID for idle)
    lock->flags = flags;

    return 0; // Success
}

int rwsem_acquire_read(rwsem_t *lock) {
    assert(current != NULL, "rwsem_acquire_read: no current thread");
    assert(mycpu()->spin_depth == 0,
           "rwsem_acquire_read called with spinlock held");
    assert(!CPU_IN_ITR(), "rwsem_acquire_read called in interrupt context");
    if (!lock) {
        return -1; // Invalid lock
    }

    int ret = 0;
    spin_lock(&lock->lock);
    // @TODO: signal handling (wait is still uninterruptible for now)
    while (__reader_should_wait(lock)) {
        __thread_state_set(current, THREAD_UNINTERRUPTIBLE);
        ret = tq_wait(&lock->read_queue, &lock->lock, NULL);
        if (ret != 0) {
            spin_unlock(&lock->lock);
            return ret;
        }
    }
    lock->readers++;
    spin_unlock(&lock->lock);
    return ret;
}

int rwsem_try_acquire_read(rwsem_t *lock) {
    assert(current != NULL, "rwsem_try_acquire_read: no current thread");
    assert(mycpu()->spin_depth == 0,
           "rwsem_try_acquire_read called with spinlock held");
    assert(!CPU_IN_ITR(),
           "rwsem_try_acquire_read called in interrupt context");
    if (!lock) {
        return -EINVAL;
    }

    int ret = -EAGAIN;
    spin_lock(&lock->lock);
    if (!__reader_should_wait(lock)) {
        lock->readers++;
        ret = 0;
    }
    spin_unlock(&lock->lock);
    return ret;
}

int rwsem_acquire_read_interruptible(rwsem_t *lock) {
    assert(current != NULL, "rwsem_acquire_read_interruptible: no current thread");
    assert(mycpu()->spin_depth == 0,
           "rwsem_acquire_read_interruptible called with spinlock held");
    assert(!CPU_IN_ITR(),
           "rwsem_acquire_read_interruptible called in interrupt context");
    if (!lock) {
        return -EINVAL;
    }

    spin_lock(&lock->lock);
    while (__reader_should_wait(lock)) {
        if (signal_pending(current)) {
            spin_unlock(&lock->lock);
            return -EINTR;
        }
        __thread_state_set(current, THREAD_INTERRUPTIBLE);
        int ret = tq_wait(&lock->read_queue, &lock->lock, NULL);
        if (ret != 0 && signal_pending(current)) {
            spin_unlock(&lock->lock);
            return -EINTR;
        }
    }

    lock->readers++;
    spin_unlock(&lock->lock);
    return 0;
}

int rwsem_acquire_read_timed(rwsem_t *lock, uint64 timeout_ms) {
    assert(current != NULL, "rwsem_acquire_read_timed: no current thread");
    assert(mycpu()->spin_depth == 0,
           "rwsem_acquire_read_timed called with spinlock held");
    assert(!CPU_IN_ITR(), "rwsem_acquire_read_timed called in interrupt context");
    if (!lock) {
        return -EINVAL;
    }

    if (timeout_ms == 0) {
        int ret = rwsem_try_acquire_read(lock);
        return ret == 0 ? 0 : -ETIMEDOUT;
    }

    uint64 start = r_time();
    uint64 timeout_ticks = MS_TO_RAWTICKS(timeout_ms);
    spin_lock(&lock->lock);

    while (__reader_should_wait(lock)) {
        if (signal_pending(current)) {
            spin_unlock(&lock->lock);
            return -EINTR;
        }

        uint64 elapsed = r_time() - start;
        if (elapsed >= timeout_ticks) {
            spin_unlock(&lock->lock);
            return -ETIMEDOUT;
        }

        uint64 remaining_ticks = timeout_ticks - elapsed;
        uint64 remaining_ms = (remaining_ticks + TICK_MS - 1) / TICK_MS;
        if (remaining_ms == 0) {
            remaining_ms = 1;
        }

        struct rwsem_timed_wait_ctx ctx = {
            .lock = lock,
            .timeout_ms = remaining_ms,
            .timer_armed = false,
        };

        __thread_state_set(current, THREAD_INTERRUPTIBLE);
        int ret = tq_wait_cb(&lock->read_queue, __rwsem_timed_sleep_cb,
                             __rwsem_timed_wake_cb, &ctx, NULL);
        if (ret != 0 && signal_pending(current)) {
            spin_unlock(&lock->lock);
            return -EINTR;
        }
    }

    lock->readers++;
    spin_unlock(&lock->lock);
    return 0;
}

int rwsem_acquire_write(rwsem_t *lock) {
    assert(current != NULL, "rwsem_acquire_write: no current thread");
    assert(mycpu()->spin_depth == 0,
           "rwsem_acquire_write called with spinlock held");
    assert(!CPU_IN_ITR(), "rwsem_acquire_write called in interrupt context");
    if (!lock) {
        return -1; // Invalid lock
    }

    int ret = 0;
    spin_lock(&lock->lock);
    struct thread *self = current;
    int self_pid = self->pid; // current != NULL asserted above
    assert(lock->holder_pid != self_pid,
           "rwsem_acquire_write: deadlock detected, thread already holds the "
           "write lock");
    // @TODO: signal handling (wait is still uninterruptible for now)
    while (__writer_should_wait(lock, self_pid)) {
        __thread_state_set(current, THREAD_UNINTERRUPTIBLE);
        assert(lock->holder_pid != self_pid,
               "rwsem_acquire_write: deadlock detected, thread already holds "
               "the write lock");
        ret = tq_wait(&lock->write_queue, &lock->lock, NULL);
        if (ret != 0) {
            spin_unlock(&lock->lock);
            return ret;
        }
    }
    lock->holder_pid = self_pid;
    spin_unlock(&lock->lock);
    return ret; // Success
}

int rwsem_try_acquire_write(rwsem_t *lock) {
    assert(current != NULL, "rwsem_try_acquire_write: no current thread");
    assert(mycpu()->spin_depth == 0,
           "rwsem_try_acquire_write called with spinlock held");
    assert(!CPU_IN_ITR(),
           "rwsem_try_acquire_write called in interrupt context");
    if (!lock) {
        return -EINVAL;
    }

    int ret = -EAGAIN;
    spin_lock(&lock->lock);
    int self_pid = current->pid;
    if (lock->holder_pid == self_pid) {
        spin_unlock(&lock->lock);
        return -EDEADLK;
    }
    if (!__writer_should_wait(lock, self_pid)) {
        lock->holder_pid = self_pid;
        ret = 0;
    }
    spin_unlock(&lock->lock);
    return ret;
}

int rwsem_acquire_write_interruptible(rwsem_t *lock) {
    assert(current != NULL,
           "rwsem_acquire_write_interruptible: no current thread");
    assert(mycpu()->spin_depth == 0,
           "rwsem_acquire_write_interruptible called with spinlock held");
    assert(!CPU_IN_ITR(),
           "rwsem_acquire_write_interruptible called in interrupt context");
    if (!lock) {
        return -EINVAL;
    }

    spin_lock(&lock->lock);
    int self_pid = current->pid;
    if (lock->holder_pid == self_pid) {
        spin_unlock(&lock->lock);
        return -EDEADLK;
    }

    while (__writer_should_wait(lock, self_pid)) {
        if (signal_pending(current)) {
            spin_unlock(&lock->lock);
            return -EINTR;
        }
        __thread_state_set(current, THREAD_INTERRUPTIBLE);
        int ret = tq_wait(&lock->write_queue, &lock->lock, NULL);
        if (ret != 0 && signal_pending(current)) {
            spin_unlock(&lock->lock);
            return -EINTR;
        }
        assert(lock->holder_pid != self_pid,
               "rwsem_acquire_write_interruptible: deadlock detected");
    }

    lock->holder_pid = self_pid;
    spin_unlock(&lock->lock);
    return 0;
}

int rwsem_acquire_write_timed(rwsem_t *lock, uint64 timeout_ms) {
    assert(current != NULL, "rwsem_acquire_write_timed: no current thread");
    assert(mycpu()->spin_depth == 0,
           "rwsem_acquire_write_timed called with spinlock held");
    assert(!CPU_IN_ITR(), "rwsem_acquire_write_timed called in interrupt context");
    if (!lock) {
        return -EINVAL;
    }

    if (timeout_ms == 0) {
        int ret = rwsem_try_acquire_write(lock);
        return ret == 0 ? 0 : -ETIMEDOUT;
    }

    uint64 start = r_time();
    uint64 timeout_ticks = MS_TO_RAWTICKS(timeout_ms);
    spin_lock(&lock->lock);
    int self_pid = current->pid;
    if (lock->holder_pid == self_pid) {
        spin_unlock(&lock->lock);
        return -EDEADLK;
    }

    while (__writer_should_wait(lock, self_pid)) {
        if (signal_pending(current)) {
            spin_unlock(&lock->lock);
            return -EINTR;
        }

        uint64 elapsed = r_time() - start;
        if (elapsed >= timeout_ticks) {
            spin_unlock(&lock->lock);
            return -ETIMEDOUT;
        }

        uint64 remaining_ticks = timeout_ticks - elapsed;
        uint64 remaining_ms = (remaining_ticks + TICK_MS - 1) / TICK_MS;
        if (remaining_ms == 0) {
            remaining_ms = 1;
        }

        struct rwsem_timed_wait_ctx ctx = {
            .lock = lock,
            .timeout_ms = remaining_ms,
            .timer_armed = false,
        };

        __thread_state_set(current, THREAD_INTERRUPTIBLE);
        int ret = tq_wait_cb(&lock->write_queue, __rwsem_timed_sleep_cb,
                             __rwsem_timed_wake_cb, &ctx, NULL);
        if (ret != 0 && signal_pending(current)) {
            spin_unlock(&lock->lock);
            return -EINTR;
        }
        assert(lock->holder_pid != self_pid,
               "rwsem_acquire_write_timed: deadlock detected");
    }

    lock->holder_pid = self_pid;
    spin_unlock(&lock->lock);
    return 0;
}

void rwsem_release(rwsem_t *lock) {
    if (!lock) {
        return; // Invalid lock
    }

    spin_lock(&lock->lock);
    struct thread *self = current;
    int self_pid = (self != NULL) ? self->pid : -1;
    if (lock->holder_pid == self_pid && self_pid != -1) {
        // When the current thread is the writer holding the lock
        // Then the current thread is holding the write lock
        lock->holder_pid = -1; // Clear the holder (-1 = no holder)
        __do_wake_up(lock);
    } else {
        assert(lock->readers > 0, "rwsem_release: no readers to release");
        lock->readers--;
        if (lock->readers == 0) {
            // If there are no more readers, wake up the next writer or readers
            __do_wake_up(lock);
        }
    }
    spin_unlock(&lock->lock);
}

bool rwsem_is_write_holding(rwsem_t *lock) {
    if (!lock) {
        return false; // Invalid lock
    }
    struct thread *self = current;
    if (self == NULL) {
        return false; // No thread context, can't be holding the lock
    }

    spin_lock(&lock->lock);
    bool is_locked = (lock->holder_pid == self->pid);
    spin_unlock(&lock->lock);
    return is_locked;
}
