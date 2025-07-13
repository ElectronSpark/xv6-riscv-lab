#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "list.h"
#include "hlist.h"
#include "proc_queue.h"
#include "sched.h"


static proc_queue_t ready_queue;
static proc_queue_t sleep_queue;
static spinlock_t __sched_lock;   // ready_queue and sleep_queue share this lock

int sched_holding(void) {
    // Check if the scheduler lock is currently held
    return spin_holding(&__sched_lock);
}

/* Scheduler lock assertions */
static inline void __sched_assert_holding(void) {
    assert(spin_holding(&__sched_lock), "sched_lock must be held");
}

static inline void __sched_assert_unholding(void) {
    assert(!spin_holding(&__sched_lock), "sched_lock must not be held");
}

/* Scheduler lock functions */
// These functions are used to acquire and release the scheduler lock.
// To avoid deadlocks, locks must be held in the following order:
// - locks of each process -- proc.c
// - locks related to the process queue -- proc_queue.c
// - sched_lock -- sched.c
// - proc table lock -- proc.c
//
// The locks of task queues and the scheduler lock should not be held simultaneously,
void sched_lock(void) {
    spin_acquire(&__sched_lock);
}

void sched_unlock(void) {
    spin_release(&__sched_lock);
}

/* Scheduler functions */
void scheduler_init(void) {
    spin_init(&__sched_lock, "sched_lock");
    proc_queue_init(&ready_queue, "ready_queue");
    proc_queue_init(&sleep_queue, "sleep_queue");
}

// Pick the next process to run from the ready queue.
// Returns a RUNABLE state process or NULL if no process is ready.
// The process returned will be locked
static struct proc *__sched_pick_next(void) {
    struct proc *p = NULL;
    sched_lock();
    assert(proc_queue_pop(&ready_queue, &p) == 0, "Failed to pop process from ready queue");
    sched_unlock();
    
    if (p) {
        spin_acquire(&p->lock);
        assert(p->state != RUNNING, "found and running process in ready queue");
        assert(p->state != SLEEPING, "try to schedule a sleeping process");
        assert(p->state != UNUSED, "try to schedule an uninitialized process");
        assert(p->state != ZOMBIE, "found and zombie process in ready queue");
        assert(p->state == RUNNABLE, "try to schedule unknown process state");
        return p;
    }

    return NULL;
}

static void __idle(void) {
    // This function is called when no process is ready to run.
    // It can be used to put the CPU into a low-power state or
    // perform other idle tasks.
    // For now, we just yield the CPU.
    __sched_assert_unholding();
    asm volatile("wfi");  // Wait for interrupt
}

// Switch to the given process.
// The process will change its state to RUNNING afer the switching.
// Before calling this function, the caller must hold both the process's lock
//    and the scheduler lock.
// Returns the previous process that was running on this CPU.
static struct spinlock *__switch_to(struct proc *p) {
    __sched_assert_holding();
    assert(!intr_get(), "Interrupts must be disabled before switching to a process");
    assert(p != NULL, "Cannot switch to a NULL process");
    assert(p->state == RUNNABLE, "Cannot switch to a non-RUNNABLE process");
    assert(spin_holding(&p->lock), "Process lock must be held before switch");

    // Switch to the process's context
    mycpu()->proc = p; // Set the current process for this CPU
    p->state = RUNNING; // Set the process state to RUNNING

    struct spinlock * lk = (struct spinlock *)__swtch_context(&mycpu()->context, &p->context, 0);
    assert(spin_holding(&p->lock), "Process lock must be held after switch");
    assert(!intr_get(), "Interrupts must be disabled before switching to a process");

    mycpu()->proc = NULL; // Clear the current process for this CPU

    return lk;
}

void scheduler_run(void) {
    intr_off(); // Disable interrupts to keep the atomicity of the scheduling operation
    while (1) {
        struct proc *p = __sched_pick_next();
        if (!p) {
            intr_on(); // allow interrupts to be handled
            __idle();
            intr_off();
            continue;
        }
        // printf("CPU: %d -> %s (pid: %d)\n", cpuid(), p->name, p->pid);
        sched_lock();
        struct spinlock *lk = __switch_to(p);
        assert(!intr_get(), "Interrupts must be disabled after switching to a process");
        if (p->state == RUNNING) {
            p->state = RUNNABLE; // If we returned to a RUNNING process, set it to RUNNABLE
            if (proc_queue_push(&ready_queue, p) != 0) {
                panic("Failed to push process back to ready queue");
            }
        }
        sched_unlock();
        // printf("CPU: %d <- %s: %s (pid: %d)\n", cpuid(), p->name, procstate_to_str(p->state), p->pid);
        spin_release(&p->lock);
        if (lk != NULL) {
            spin_release(lk); // Release the lock returned by __swtch_context
        }
    }
}

// Yield the CPU to allow other processes to run.
int scheduler_yield(uint64 *ret_arg, struct spinlock *lk) {
    push_off();
    struct proc *proc = myproc();
    
    int lk_holding = (lk != NULL && spin_holding(lk));
    int proc_holding = spin_holding(&proc->lock);
    if (!proc_holding) {
        spin_acquire(&proc->lock);
    }
    pop_off();

    assert(!intr_get(), "Interrupts must be disabled after switching to a process");
    int intena = mycpu()->intena; // Save interrupt state
    assert(mycpu()->noff == 1 || (mycpu()->noff == 2 && lk_holding), 
           "Process must hold and only hold the proc lock when yielding. Current noff: %d", mycpu()->noff);
    sched_lock();
    uint64 ret_val = __swtch_context(&proc->context, &mycpu()->context, (uint64)lk);
    sched_unlock();

    assert(!intr_get(), "Interrupts must be disabled after switching to a process");
    assert(myproc() == proc, "Yield returned to a different process");
    assert(spin_holding(&proc->lock), "Process lock must be held after yield");
    assert(proc->state == RUNNING, "Process state must be RUNNING after yield");

    mycpu()->intena = intena; // Restore interrupt state

    // Because the process lock is acquired after lk, we need to release it before acquiring lk.
    // The process lock will be held after acquiring lk, if the process lock was held 
    // before the yield.
    spin_release(&proc->lock);

    if (lk_holding) {
        spin_acquire(lk);
    }

    if (proc_holding) {
        spin_acquire(&proc->lock);
    }
    
    if (ret_arg) {
        *ret_arg = ret_val; // Return the value from the context switch
    }

    return 0;
}

// Put the current process to sleep on the specified queue.
void scheduler_sleep(proc_queue_t *queue, struct spinlock *lk) {
    push_off(); // Increase noff counter to ensure interruptions are disabled
    struct proc *proc = myproc();
    assert(proc != NULL, "Cannot sleep with a NULL process");
    assert(queue != NULL, "Cannot sleep on a NULL queue");

    int holding = spin_holding(&proc->lock);
    if (!holding) {
        spin_acquire(&proc->lock);
    }
    pop_off();  // Safe to decrease noff counter after acquiring the process lock

    sched_lock();
    proc->state = SLEEPING;
    if (proc_queue_push(queue, proc) != 0) {
        panic("Failed to push process to sleep queue");
    }
    sched_unlock();

    scheduler_yield(NULL, lk); // Switch to the scheduler
    spin_release(&proc->lock);

    if (holding) {
        spin_acquire(&proc->lock);
    }
}

// Wake up a process from the sleep queue.
void scheduler_wakeup(struct proc *p) {
    push_off(); // Increase noff counter to ensure interruptions are disabled
    __sched_assert_holding();
    pop_off(); // Safe to decrease noff counter after acquiring the process lock
    // __sched_assert_unholding();
    assert(p != NULL, "Cannot wake up a NULL process");
    assert(p->state == SLEEPING, "Process must be SLEEPING to wake up");
    assert(!proc_in_queue(p, NULL), "Process must not be in any queue before waking up");
    assert(p != myproc(), "Cannot wake up the current process");

    p->state = RUNNABLE; // Change state to RUNNABLE
    p->chan = NULL; // Clear the channel

    if (proc_queue_push(&ready_queue, p) != 0) {
        panic("Failed to push process to ready queue");
    }
}

void scheduler_sleep_on_chan(void *chan, struct spinlock *lk) {
    push_off(); // Increase noff counter to ensure interruptions are disabled
    struct proc *proc = myproc();
    assert(proc != NULL, "Cannot sleep with a NULL process");
    assert(chan != NULL, "Cannot sleep on a NULL channel");
    
    int holding = spin_holding(&proc->lock);
    if (!holding) {
        spin_acquire(&proc->lock);
    } 
    pop_off();
    proc->chan = chan;
    scheduler_sleep(&sleep_queue, lk); // Put the process to sleep on the sleep queue

    if (!holding) {
        spin_release(&proc->lock);
    } 
}

void scheduler_wakeup_on_chan(void *chan) {
    proc_queue_t tmp_queue = { 0 };
    proc_queue_init(&tmp_queue, "tmp_queue");
    sched_lock();
    struct proc *p, *tmp;
    proc_queue_foreach_unlocked(&sleep_queue, p, tmp) {
        // process should not be accessible from any other place if it's inside of a proc queue
        if (p->chan == chan) {
            assert(proc_queue_remove(&sleep_queue, p) == 0, "Failed to remove process from sleep queue");
            assert(proc_queue_push(&tmp_queue, p) == 0, "Failed to push process to be woken up to temporary queue");
        }
    }
    sched_unlock();

    proc_queue_foreach_unlocked(&tmp_queue, p, tmp) {
        spin_acquire(&p->lock);
        assert(proc_queue_remove(&tmp_queue, p) == 0, "Failed to remove process from temporary queue");
        sched_lock();
        scheduler_wakeup(p);
        sched_unlock();
        spin_release(&p->lock);
    }
}
