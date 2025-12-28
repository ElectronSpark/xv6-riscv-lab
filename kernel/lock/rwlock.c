#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "rwlock.h"
#include "proc_queue.h"
#include "sched.h"
#include "string.h"


static inline int __reader_should_wait(rwlock_t *lock) {
    if (lock->readers == 0) {
        return lock->holder_pid != 0;
    }
    if ((lock->flags & RWLOCK_PRIO_WRITE) && proc_queue_size(&lock->write_queue) > 0) {
        return 1;
    }
    return 0;
}

static inline int __writer_should_wait(rwlock_t *lock, int pid) {
    if (lock->holder_pid == pid) {
        // The caller already holds the write lock
        return 0;
    }
    if (lock->holder_pid != 0) {
        return 1;
    }
    if (lock->readers > 0) {
        return 1;
    }
    return 0;
}

static void __wake_readers(rwlock_t *lock) {
    int ret = proc_queue_wakeup_all(&lock->read_queue, 0, 0);
    assert(ret == 0, "rwlock: failed to wake readers");
}

static void __wake_writer(rwlock_t *lock) {
    struct proc *next = NULL;
    int ret = proc_queue_wakeup(&lock->write_queue, 0, 0, &next);
    assert(ret == 0, "rwlock: failed to wake writer");
    assert(next != NULL, "rwlock: woke writer with NULL proc");
    lock->holder_pid = next->pid;
}

// wake up readers or a writer depending on the lock's priority.
static void __do_wake_up(rwlock_t *lock) {
    if (lock->flags & RWLOCK_PRIO_WRITE) {
        // If the lock is in write priority mode, first try to wake up the next writer
        if (proc_queue_size(&lock->write_queue) > 0) {
            __wake_writer(lock);
        } else if (proc_queue_size(&lock->read_queue) > 0) {
            __wake_readers(lock);
        }
    } else {
        // If the lock is in read priority mode, first try to wake up all readers
        if (proc_queue_size(&lock->read_queue) > 0) {
            __wake_readers(lock);
        } else if (proc_queue_size(&lock->write_queue) > 0) {
            __wake_writer(lock);
        }
    }
}


int rwlock_init(rwlock_t *lock, uint64 flags, const char *name) {
    if (!lock || !name) {
        return -1; // Invalid parameters
    }

    spin_init(&lock->lock, "rwlock spinlock");
    lock->readers = 0;
    proc_queue_init(&lock->read_queue, "rwlock read queue", &lock->lock);
    proc_queue_init(&lock->write_queue, "rwlock write queue", &lock->lock);
    lock->name = name;
    lock->holder_pid = 0;
    lock->flags = flags;

    return 0; // Success
}

int rwlock_acquire_read(rwlock_t *lock) {
    assert(myproc() != NULL, "rwlock_acquire_read: no current process");
    if (!lock) {
        return -1; // Invalid lock
    }

    int ret = 0;
    spin_acquire(&lock->lock);
    // @TODO: signal handling (wait is still uninterruptible for now)
    while (__reader_should_wait(lock)) {
        ret = proc_queue_wait(&lock->read_queue, &lock->lock, NULL);
        if (ret != 0) {
            spin_release(&lock->lock);
            return ret;
        }
    }
    lock->readers++;
    spin_release(&lock->lock);
    return ret;
}

int rwlock_acquire_write(rwlock_t *lock) {
    assert(myproc() != NULL, "rwlock_acquire_write: no current process");
    if (!lock) {
        return -1; // Invalid lock
    }

    int ret = 0;
    spin_acquire(&lock->lock);
    struct proc *self = myproc();
    int self_pid = (self != NULL) ? self->pid : 0;
    assert(lock->holder_pid != self_pid, "rwlock_acquire_write: deadlock detected, process already holds the write lock");
    // @TODO: signal handling (wait is still uninterruptible for now)
    while (__writer_should_wait(lock, self_pid)) {
        assert(lock->holder_pid != self_pid, "rwlock_acquire_write: deadlock detected, process already holds the write lock");
        ret = proc_queue_wait(&lock->write_queue, &lock->lock, NULL);
        if (ret != 0) {
            spin_release(&lock->lock);
            return ret;
        }
    }
    lock->holder_pid = self_pid;
    spin_release(&lock->lock);
    return ret; // Success
}

void rwlock_release(rwlock_t *lock) {
    if (!lock) {
        return; // Invalid lock
    }

    spin_acquire(&lock->lock);
    struct proc *self = myproc();
    int self_pid = (self != NULL) ? self->pid : 0;
    if (lock->holder_pid == self_pid) {
        // When the current process is the writer holding the lock
        // Then the current process is holding the write lock
        lock->holder_pid = 0; // Clear the holder if no writers are waiting
        __do_wake_up(lock);
    } else {
        assert(lock->readers > 0, "rwlock_release: no readers to release");
        lock->readers--;
        if (lock->readers == 0) {
            // If there are no more readers, wake up the next writer or readers
            __do_wake_up(lock);
        }
    }
    spin_release(&lock->lock);
}

bool rwlock_is_write_holding(rwlock_t *lock) {
    assert(myproc() != NULL, "rwlock_is_write_holding: no current process");
    if (!lock) {
        return false; // Invalid lock
    }

    spin_acquire(&lock->lock);
    struct proc *self = myproc();
    int self_pid = (self != NULL) ? self->pid : 0;
    bool is_locked = (lock->holder_pid == self_pid);
    spin_release(&lock->lock);
    return is_locked;
}
