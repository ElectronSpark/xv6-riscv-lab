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


static inline int __reader_should_wait(rwsem_t *lock) {
    if (lock->readers == 0) {
        return lock->holder_pid != -1;  // -1 = no holder
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
    if (lock->holder_pid != -1) {  // -1 = no holder
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
        // If the lock is in write priority mode, first try to wake up the next writer
        if (tq_size(&lock->write_queue) > 0) {
            __wake_writer(lock);
        } else if (tq_size(&lock->read_queue) > 0) {
            __wake_readers(lock);
        }
    } else {
        // If the lock is in read priority mode, first try to wake up all readers
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
    lock->holder_pid = -1;  // -1 = no holder (0 is valid PID for idle)
    lock->flags = flags;

    return 0; // Success
}

int rwsem_acquire_read(rwsem_t *lock) {
    assert(current != NULL, "rwsem_acquire_read: no current thread");
    assert(mycpu()->spin_depth == 0, "rwsem_acquire_read called with spinlock held");
    assert(!CPU_IN_ITR(), "rwsem_acquire_read called in interrupt context");
    if (!lock) {
        return -1; // Invalid lock
    }

    int ret = 0;
    spin_lock(&lock->lock);
    // @TODO: signal handling (wait is still uninterruptible for now)
    while (__reader_should_wait(lock)) {
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

int rwsem_acquire_write(rwsem_t *lock) {
    assert(current != NULL, "rwsem_acquire_write: no current thread");
    assert(mycpu()->spin_depth == 0, "rwsem_acquire_write called with spinlock held");
    assert(!CPU_IN_ITR(), "rwsem_acquire_write called in interrupt context");
    if (!lock) {
        return -1; // Invalid lock
    }

    int ret = 0;
    spin_lock(&lock->lock);
    struct thread *self = current;
    int self_pid = self->pid;  // current != NULL asserted above
    assert(lock->holder_pid != self_pid, "rwsem_acquire_write: deadlock detected, thread already holds the write lock");
    // @TODO: signal handling (wait is still uninterruptible for now)
    while (__writer_should_wait(lock, self_pid)) {
        assert(lock->holder_pid != self_pid, "rwsem_acquire_write: deadlock detected, thread already holds the write lock");
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
