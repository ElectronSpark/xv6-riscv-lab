/*
 * (User Original Comment)
 * ------------------------------------------------------------
 * I let Github Copilot to check if my rwlock has any critical data-integrity issues.
 * It says no. I will trust it this time.
 * The original summary was:
 * 
 * RWLock Integrity Review Summary
 * --------------------------------
 * GitHub Copilot
 *
 * Result: No critical data-integrity flaw found.
 *
 * Correctness Observations:
 * - Write exclusion: writers set holder != NULL only when readers == 0; readers enter when
 *   (readers > 0) or holder == NULL, so no reader runs concurrently with an exclusive holder.
 * - Reader wake logic (__wakeup_readers): increments 'readers' for each queued reader under
 *   the spinlock before waking them. Each reader later removes its own queue node; a second
 *   wake on the same entries cannot occur with current call sites.
 * - Writer wake logic (__wakeup_writer): sets holder before wakeup ensuring the woken writer
 *   owns the lock upon scheduling.
 * - Waiter lifetime: stack-allocated waiter nodes persist while sleeping; removed before
 *   the acquire function returns; safe.
 * - Memory ordering: spin_acquire/spin_release provide full barriers; shared state is only
 *   mutated while holding the spinlock.
 * - Test4 data ordering: version written first, checksum last; readers never see partial
 *   updates because writer exclusivity is enforced. (If lock semantics ever relax, consider
 *   writing version last to make the structure self-validating.)
 *
 * Potential (Non-Fatal) Issues / Improvements:
 * - Starvation: In read-priority mode a continuous stream of readers may starve writers.
 *   Enable RWLOCK_PRIO_WRITE or implement handoff logic for fairness.
 * - Accounting robustness: Could add asserts after a reader acquires
 *   (assert(holder == NULL)) and before granting write (assert(readers == 0)).
 * - Wake strategy: __wakeup_readers leaves nodes in the queue until each reader removes itself;
 *   acceptable, but popping during wake could simplify future changes.
 * - Upgrade/downgrade: Not supported (no read->write promotion or write->read downgrade).
 * - Recursive write: Guarded by assert; production could return an error (e.g., -EDEADLK).
 *
 * Optional Hardening Ideas:
 * - Write version last (after data + checksum) for self-consistency if unlocked readers appear.
 * - Add a wake generation counter to aid debugging missed wakeups.
 *
 * Conclusion: Implementation preserves data integrity under current usage; main practical risk
 * is writer starvation without write priority.
 */

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "rwlock.h"
#include "proc_queue.h"
#include "sched.h"


// Wake up all readers and increse the readers count according to the number of readers woken up.
static void __wakeup_readers(rwlock_t *lock) {
    proc_node_t *p = NULL;
    proc_node_t *tmp = NULL;
    proc_list_foreach_unlocked(&lock->read_queue, p, tmp) {
        assert(p->proc != NULL, "first_waiter process is NULL");
        lock->readers++;
        proc_lock(p->proc); // Lock the process that will hold the lock
        sched_lock();
        scheduler_wakeup(p->proc); // Wake up the first process in the read queue
        sched_unlock();
        proc_unlock(p->proc); // Unlock the process that will hold the lock
    }
}

// Wake up one writer and set it as the holder of the lock.
static void __wakeup_writer(rwlock_t *lock) {
    proc_node_t *first_waiter = NULL;
    assert(proc_queue_pop(&lock->write_queue, &first_waiter) == 0,
            "failed to pop from write queue");
    
    if (first_waiter != NULL) {
        assert(first_waiter->proc != NULL, "first_waiter process is NULL");
        lock->holder = first_waiter->proc;
        proc_lock(first_waiter->proc); // Lock the process that will hold the lock
        sched_lock();
        scheduler_wakeup(first_waiter->proc); // Wake up the first process in the read queue
        sched_unlock();
        proc_unlock(first_waiter->proc); // Unlock the process that will hold the lock
    }
}

// Wait for releasing the lock as a reader.
// @TODO: signal handling
static int __reader_wait_on(rwlock_t *lock) {
    proc_lock(myproc());
    for(;;) {
        proc_node_t waiter = { 0 };
        proc_node_init(&waiter);
        assert(proc_queue_push(&lock->read_queue, &waiter) == 0,
                "failed to push to read queue");
        scheduler_sleep(&lock->lock, PSTATE_INTERRUPTIBLE);
        if (lock->readers > 0) {
            assert(lock->holder == NULL, "lock is held by a writer");
            assert(proc_queue_remove(&lock->read_queue, &waiter) == 0,
                    "failed to remove from read queue");
            break; // Success: lock acquired
        }
    }
    proc_unlock(myproc());
    return 0;
}

// wake up readers or a writer depending on the lock's priority.
static void __do_wake_up(rwlock_t *lock) {
    if (lock->flags & RWLOCK_PRIO_WRITE) {
        // If the lock is in write priority mode, first try to wake up the next writer
        if (proc_queue_size(&lock->write_queue) > 0) {
            __wakeup_writer(lock);
        } else if (proc_queue_size(&lock->read_queue) > 0) {
            __wakeup_readers(lock);
        }
    } else {
        // If the lock is in read priority mode, first try to wake up all readers
        if (proc_queue_size(&lock->read_queue) > 0) {
            __wakeup_readers(lock);
        } else if (proc_queue_size(&lock->write_queue) > 0) {
            __wakeup_writer(lock);
        }
    }
}

// Wait for releasing the lock as a writer.
// @TODO: signal handling
static int __writer_wait_on(rwlock_t *lock) {
    proc_lock(myproc());
    while (lock->holder != myproc()) {
        proc_node_t waiter = { 0 };
        proc_node_init(&waiter);
        assert(proc_queue_push(&lock->write_queue, &waiter) == 0,
                "rwlock_acquire_write: failed to push to write queue");
        scheduler_sleep(&lock->lock, PSTATE_INTERRUPTIBLE);
    }
    proc_unlock(myproc());
    return 0;
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
    lock->holder = NULL;
    lock->flags = flags;

    return 0; // Success
}

int rwlock_acquire_read(rwlock_t *lock) {
    if (!lock) {
        return -1; // Invalid lock
    }

    int ret = 0;
    spin_acquire(&lock->lock);
    if (lock->readers == 0 && lock->holder != NULL) {
        // Sleep when no active readers and a writer is holding the lock
        // In this case, the one releasing the lock increases the readers count
        ret = __reader_wait_on(lock);
    } else if (lock->readers > 0 && (lock->flags & RWLOCK_PRIO_WRITE) && 
               proc_queue_size(&lock->write_queue) > 0) {
        // Sleep when there are writers waiting and the lock is in write priority mode
        ret = __reader_wait_on(lock);
    } else {
        lock->readers++;
    }
    spin_release(&lock->lock);
    return ret;
}

int rwlock_acquire_write(rwlock_t *lock) {
    if (!lock) {
        return -1; // Invalid lock
    }

    int ret = 0;
    spin_acquire(&lock->lock);
    if (lock->readers > 0 || lock->holder != NULL) {
        assert(lock->holder != myproc(), "rwlock_acquire_write: deadlock detected, process already holds the write lock");
        ret = __writer_wait_on(lock);
    } else {
        lock->holder = myproc();
    }
    spin_release(&lock->lock);
    return ret; // Success
}

void rwlock_release(rwlock_t *lock) {
    if (!lock) {
        return; // Invalid lock
    }

    spin_acquire(&lock->lock);
    if (lock->holder == myproc()) {
        // When the current process is the writer holding the lock
        // Then the current process is holding the write lock
        lock->holder = NULL; // Clear the holder if no writers are waiting
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
