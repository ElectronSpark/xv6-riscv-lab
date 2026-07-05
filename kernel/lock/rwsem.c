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
#include "cmdline.h"

struct rwsem_trace_snapshot {
    int holder_pid;
    char holder_name[16];
    void *holder_caller;
    int readers;
    int readq;
    int writeq;
};

static uint64 rwsem_trace_parse_u64(const char *key, uint64 fallback) {
    char value[32];
    if (cmdline_get_param(key, value, sizeof(value)) != 0 || value[0] == '\0')
        return fallback;
    return strtoul(value, NULL, 10);
}

static int rwsem_vm_trace_enabled(void) {
    static int cached = -1;
    if (cached == -1) {
        char value[8];
        cached = cmdline_get_param("vm_rwsem_trace", value, sizeof(value)) == 0 &&
                 value[0] != '0';
    }
    return cached;
}

static uint64 rwsem_vm_trace_threshold_ms(void) {
    static uint64 threshold = 0;
    if (threshold == 0)
        threshold = rwsem_trace_parse_u64("vm_rwsem_trace_ms", 20);
    return threshold;
}

static uint64 rwsem_vm_trace_limit(void) {
    static uint64 limit = 0;
    if (limit == 0)
        limit = rwsem_trace_parse_u64("vm_rwsem_trace_limit", 256);
    return limit;
}

static int rwsem_vm_trace_lock(rwsem_t *lock) {
    return rwsem_vm_trace_enabled() && lock && lock->name &&
           strcmp(lock->name, "vm_rw_lock") == 0;
}

static int rwsem_vm_trace_take_slot(void) {
    static uint64 emitted = 0;
    uint64 limit = rwsem_vm_trace_limit();
    uint64 slot = __atomic_fetch_add(&emitted, 1, __ATOMIC_RELAXED);
    return slot < limit;
}

static void rwsem_trace_snapshot_locked(rwsem_t *lock,
                                        struct rwsem_trace_snapshot *snap) {
    snap->holder_pid = lock->holder_pid;
    safestrcpy(snap->holder_name,
               lock->holder_name[0] ? lock->holder_name : "-",
               sizeof(snap->holder_name));
    snap->holder_caller = lock->holder_caller;
    snap->readers = lock->readers;
    snap->readq = tq_size(&lock->read_queue);
    snap->writeq = tq_size(&lock->write_queue);
}

static void rwsem_trace_set_holder_locked(rwsem_t *lock, struct thread *holder,
                                          uint64 now_ms, void *caller) {
    lock->holder_pid = holder ? holder->pid : -1;
    lock->holder_since_ms = now_ms;
    lock->holder_caller = caller;
    if (holder)
        safestrcpy(lock->holder_name, holder->name, sizeof(lock->holder_name));
    else
        lock->holder_name[0] = '\0';
}

static void rwsem_trace_wait(const char *mode, rwsem_t *lock, uint64 wait_ms,
                             void *caller,
                             struct rwsem_trace_snapshot *first,
                             struct rwsem_trace_snapshot *now) {
    if (wait_ms < rwsem_vm_trace_threshold_ms() || !rwsem_vm_trace_take_slot())
        return;

    struct thread *self = current;
    const char *name = self ? self->name : "-";
    int pid = self ? self->pid : -1;
    int tgid = self ? self->tgid : -1;
    printf("vm-rwsem-trace: wait mode=%s lock=%s pid=%d tgid=%d name=%s "
           "wait_ms=%lu caller=%p first_holder=%d first_holder_name=%s "
           "first_holder_caller=%p "
           "first_readers=%d first_readq=%d first_writeq=%d readers=%d "
           "readq=%d writeq=%d\n",
           mode, lock->name, pid, tgid, name, wait_ms, caller,
           first->holder_pid, first->holder_name, first->holder_caller,
           first->readers, first->readq, first->writeq, now->readers,
           now->readq, now->writeq);
}

static void rwsem_trace_hold(rwsem_t *lock, int holder_pid,
                             const char *holder_name, void *holder_caller,
                             uint64 hold_ms,
                             struct rwsem_trace_snapshot *after) {
    if (hold_ms < rwsem_vm_trace_threshold_ms() || !rwsem_vm_trace_take_slot())
        return;

    printf("vm-rwsem-trace: release lock=%s holder=%d holder_name=%s "
           "holder_caller=%p hold_ms=%lu readers=%d readq=%d writeq=%d "
           "next_holder=%d next_holder_name=%s next_holder_caller=%p\n",
           lock->name, holder_pid, holder_name, holder_caller, hold_ms,
           after->readers, after->readq, after->writeq, after->holder_pid,
           after->holder_name, after->holder_caller);
}

static void rwsem_trace_read_hold(rwsem_t *lock, int first_pid,
                                  const char *first_name, void *first_caller,
                                  uint64 hold_ms,
                                  struct rwsem_trace_snapshot *after) {
    if (hold_ms < rwsem_vm_trace_threshold_ms() || !rwsem_vm_trace_take_slot())
        return;

    printf("vm-rwsem-trace: release-read lock=%s first_reader=%d "
           "first_reader_name=%s first_reader_caller=%p hold_ms=%lu "
           "readers=%d readq=%d writeq=%d next_holder=%d "
           "next_holder_name=%s next_holder_caller=%p\n",
           lock->name, first_pid, first_name, first_caller, hold_ms,
           after->readers, after->readq, after->writeq, after->holder_pid,
           after->holder_name, after->holder_caller);
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
    rwsem_trace_set_holder_locked(lock, next, sched_timer_now_ms(), NULL);
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
    lock->holder_since_ms = 0;
    lock->holder_name[0] = '\0';
    lock->holder_caller = NULL;
    lock->reader_since_ms = 0;
    lock->reader_first_pid = -1;
    lock->reader_first_name[0] = '\0';
    lock->reader_first_caller = NULL;
    lock->flags = flags;

    return 0; // Success
}

int rwsem_acquire_read_caller(rwsem_t *lock, void *caller) {
    assert(current != NULL, "rwsem_acquire_read: no current thread");
    assert(spin_depth_snapshot() == 0,
           "rwsem_acquire_read called with spinlock held");
    assert(!CPU_IN_ITR(), "rwsem_acquire_read called in interrupt context");
    if (!lock) {
        return -1; // Invalid lock
    }

    int ret = 0;
    int trace = rwsem_vm_trace_lock(lock);
    int waited = 0;
    uint64 wait_start_ms = 0;
    struct rwsem_trace_snapshot first = {0};
    struct rwsem_trace_snapshot now = {0};
    spin_lock(&lock->lock);
    // @TODO: signal handling (wait is still uninterruptible for now)
    while (__reader_should_wait(lock)) {
        if (trace && !waited) {
            waited = 1;
            wait_start_ms = sched_timer_now_ms();
            rwsem_trace_snapshot_locked(lock, &first);
        }
        ret = tq_wait(&lock->read_queue, &lock->lock, NULL);
        if (ret != 0) {
            ret = 0;
            continue;
        }
    }
    if (lock->readers == 0) {
        struct thread *self = current;
        lock->reader_since_ms = sched_timer_now_ms();
        lock->reader_first_pid = self ? self->pid : -1;
        lock->reader_first_caller = caller;
        if (self)
            safestrcpy(lock->reader_first_name, self->name,
                       sizeof(lock->reader_first_name));
        else
            lock->reader_first_name[0] = '\0';
    }
    lock->readers++;
    if (trace && waited)
        rwsem_trace_snapshot_locked(lock, &now);
    spin_unlock(&lock->lock);
    if (trace && waited)
        rwsem_trace_wait("read", lock, sched_timer_now_ms() - wait_start_ms,
                         caller,
                         &first, &now);
    return ret;
}

int rwsem_acquire_read(rwsem_t *lock) {
    return rwsem_acquire_read_caller(lock, NULL);
}

int rwsem_acquire_write_caller(rwsem_t *lock, void *caller) {
    assert(current != NULL, "rwsem_acquire_write: no current thread");
    assert(spin_depth_snapshot() == 0,
           "rwsem_acquire_write called with spinlock held");
    assert(!CPU_IN_ITR(), "rwsem_acquire_write called in interrupt context");
    if (!lock) {
        return -1; // Invalid lock
    }

    int ret = 0;
    int trace = rwsem_vm_trace_lock(lock);
    int waited = 0;
    uint64 wait_start_ms = 0;
    struct rwsem_trace_snapshot first = {0};
    struct rwsem_trace_snapshot now = {0};
    spin_lock(&lock->lock);
    struct thread *self = current;
    int self_pid = self->pid; // current != NULL asserted above
    assert(lock->holder_pid != self_pid,
           "rwsem_acquire_write: deadlock detected, thread already holds the "
           "write lock");
    // @TODO: signal handling (wait is still uninterruptible for now)
    while (__writer_should_wait(lock, self_pid)) {
        assert(lock->holder_pid != self_pid,
               "rwsem_acquire_write: deadlock detected, thread already holds "
               "the write lock");
        if (trace && !waited) {
            waited = 1;
            wait_start_ms = sched_timer_now_ms();
            rwsem_trace_snapshot_locked(lock, &first);
        }
        ret = tq_wait(&lock->write_queue, &lock->lock, NULL);
        if (ret != 0) {
            ret = 0;
            continue;
        }
    }
    rwsem_trace_set_holder_locked(lock, self, sched_timer_now_ms(), caller);
    if (trace && waited)
        rwsem_trace_snapshot_locked(lock, &now);
    spin_unlock(&lock->lock);
    if (trace && waited)
        rwsem_trace_wait("write", lock, sched_timer_now_ms() - wait_start_ms,
                         caller, &first, &now);
    return ret; // Success
}

int rwsem_acquire_write(rwsem_t *lock) {
    return rwsem_acquire_write_caller(lock, NULL);
}

void rwsem_release(rwsem_t *lock) {
    if (!lock) {
        return; // Invalid lock
    }

    spin_lock(&lock->lock);
    struct thread *self = current;
    int self_pid = (self != NULL) ? self->pid : -1;
    int trace_release = 0;
    int trace_read_release = 0;
    int released_pid = -1;
    int released_reader_pid = -1;
    char released_name[16] = "";
    char released_reader_name[16] = "";
    void *released_caller = NULL;
    void *released_reader_caller = NULL;
    uint64 hold_ms = 0;
    uint64 read_hold_ms = 0;
    struct rwsem_trace_snapshot after = {0};
    if (lock->holder_pid == self_pid && self_pid != -1) {
        // When the current thread is the writer holding the lock
        // Then the current thread is holding the write lock
        if (rwsem_vm_trace_lock(lock)) {
            trace_release = 1;
            released_pid = lock->holder_pid;
            safestrcpy(released_name,
                       lock->holder_name[0] ? lock->holder_name : "-",
                       sizeof(released_name));
            released_caller = lock->holder_caller;
            if (lock->holder_since_ms != 0)
                hold_ms = sched_timer_now_ms() - lock->holder_since_ms;
        }
        lock->holder_pid = -1; // Clear the holder (-1 = no holder)
        lock->holder_since_ms = 0;
        lock->holder_name[0] = '\0';
        lock->holder_caller = NULL;
        __do_wake_up(lock);
        if (trace_release)
            rwsem_trace_snapshot_locked(lock, &after);
    } else {
        assert(lock->readers > 0, "rwsem_release: no readers to release");
        lock->readers--;
        if (lock->readers == 0) {
            if (rwsem_vm_trace_lock(lock) && lock->reader_since_ms != 0) {
                trace_read_release = 1;
                released_reader_pid = lock->reader_first_pid;
                safestrcpy(released_reader_name,
                           lock->reader_first_name[0] ?
                               lock->reader_first_name : "-",
                           sizeof(released_reader_name));
                released_reader_caller = lock->reader_first_caller;
                read_hold_ms =
                    sched_timer_now_ms() - lock->reader_since_ms;
            }
            lock->reader_since_ms = 0;
            lock->reader_first_pid = -1;
            lock->reader_first_name[0] = '\0';
            lock->reader_first_caller = NULL;
            // If there are no more readers, wake up the next writer or readers
            __do_wake_up(lock);
            if (trace_read_release)
                rwsem_trace_snapshot_locked(lock, &after);
        }
    }
    spin_unlock(&lock->lock);
    if (trace_release)
        rwsem_trace_hold(lock, released_pid, released_name, released_caller,
                         hold_ms, &after);
    if (trace_read_release)
        rwsem_trace_read_hold(lock, released_reader_pid, released_reader_name,
                              released_reader_caller, read_hold_ms, &after);
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

/*
 * rwsem_acquire_read_interruptible - acquire read lock, interruptible by signals.
 *
 * Returns 0 on success, -EINTR if a signal is pending before or during sleep.
 */
int rwsem_acquire_read_interruptible_caller(rwsem_t *lock, void *caller) {
    assert(current != NULL, "rwsem_acquire_read_interruptible: no current thread");
    assert(spin_depth_snapshot() == 0,
           "rwsem_acquire_read_interruptible called with spinlock held");
    assert(!CPU_IN_ITR(),
           "rwsem_acquire_read_interruptible called in interrupt context");
    if (!lock) {
        return -EINVAL;
    }

    spin_lock(&lock->lock);
    int trace = rwsem_vm_trace_lock(lock);
    int waited = 0;
    uint64 wait_start_ms = 0;
    struct rwsem_trace_snapshot first = {0};
    struct rwsem_trace_snapshot now = {0};
    while (__reader_should_wait(lock)) {
        if (signal_pending(current)) {
            spin_unlock(&lock->lock);
            return -EINTR;
        }
        if (trace && !waited) {
            waited = 1;
            wait_start_ms = sched_timer_now_ms();
            rwsem_trace_snapshot_locked(lock, &first);
        }
        int ret = tq_wait_in_state(&lock->read_queue, &lock->lock, NULL,
                                   THREAD_INTERRUPTIBLE);
        if (ret != 0) {
            spin_unlock(&lock->lock);
            return -EINTR;
        }
    }
    if (lock->readers == 0) {
        struct thread *self = current;
        lock->reader_since_ms = sched_timer_now_ms();
        lock->reader_first_pid = self ? self->pid : -1;
        lock->reader_first_caller = caller;
        if (self)
            safestrcpy(lock->reader_first_name, self->name,
                       sizeof(lock->reader_first_name));
        else
            lock->reader_first_name[0] = '\0';
    }
    lock->readers++;
    if (trace && waited)
        rwsem_trace_snapshot_locked(lock, &now);
    spin_unlock(&lock->lock);
    if (trace && waited)
        rwsem_trace_wait("read-int", lock, sched_timer_now_ms() - wait_start_ms,
                         caller, &first, &now);
    return 0;
}

int rwsem_acquire_read_interruptible(rwsem_t *lock) {
    return rwsem_acquire_read_interruptible_caller(lock, NULL);
}

/*
 * rwsem_acquire_write_interruptible - acquire write lock, interruptible by signals.
 *
 * Returns 0 on success, -EINTR if a signal is pending before or during sleep.
 */
int rwsem_acquire_write_interruptible_caller(rwsem_t *lock, void *caller) {
    assert(current != NULL, "rwsem_acquire_write_interruptible: no current thread");
    assert(spin_depth_snapshot() == 0,
           "rwsem_acquire_write_interruptible called with spinlock held");
    assert(!CPU_IN_ITR(),
           "rwsem_acquire_write_interruptible called in interrupt context");
    if (!lock) {
        return -EINVAL;
    }

    spin_lock(&lock->lock);
    int trace = rwsem_vm_trace_lock(lock);
    int waited = 0;
    uint64 wait_start_ms = 0;
    struct rwsem_trace_snapshot first = {0};
    struct rwsem_trace_snapshot now = {0};
    struct thread *self = current;
    int self_pid = self->pid;
    assert(lock->holder_pid != self_pid,
           "rwsem_acquire_write_interruptible: deadlock detected, thread already "
           "holds the write lock");

    while (__writer_should_wait(lock, self_pid)) {
        assert(lock->holder_pid != self_pid,
               "rwsem_acquire_write_interruptible: deadlock detected");
        if (signal_pending(current)) {
            spin_unlock(&lock->lock);
            return -EINTR;
        }
        if (trace && !waited) {
            waited = 1;
            wait_start_ms = sched_timer_now_ms();
            rwsem_trace_snapshot_locked(lock, &first);
        }
        int ret = tq_wait_in_state(&lock->write_queue, &lock->lock, NULL,
                                   THREAD_INTERRUPTIBLE);
        if (ret != 0) {
            spin_unlock(&lock->lock);
            return -EINTR;
        }
    }
    rwsem_trace_set_holder_locked(lock, self, sched_timer_now_ms(), caller);
    if (trace && waited)
        rwsem_trace_snapshot_locked(lock, &now);
    spin_unlock(&lock->lock);
    if (trace && waited)
        rwsem_trace_wait("write-int", lock,
                         sched_timer_now_ms() - wait_start_ms, caller, &first,
                         &now);
    return 0;
}

int rwsem_acquire_write_interruptible(rwsem_t *lock) {
    return rwsem_acquire_write_interruptible_caller(lock, NULL);
}
