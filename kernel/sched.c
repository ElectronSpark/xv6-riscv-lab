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
static inline void __sched_assert_locked(void) {
    assert(spin_holding(&__sched_lock), "sched_lock must be held");
}

static inline void __sched_assert_unlocked(void) {
    assert(!spin_holding(&__sched_lock), "sched_lock must not be held");
}

/* Scheduler lock functions */
// These functions are used to acquire and release the scheduler lock.
// To avoid deadlocks, locks must be held in the following order:
// - proc table lock -- proc.c
// - sched_lock -- sched.c
// - locks of each process -- proc.c
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
static struct proc *__sched_pick_next(void) {
    __sched_assert_locked();

    struct proc *p = NULL; 
    assert(proc_queue_pop(&ready_queue, &p) == 0, "Failed to pop process from ready queue");
    if (p) {
        assert(p->state != SLEEPING, "try to schedule a sleeping process");
        assert(p->state != UNUSED, "try to schedule an uninitialized process");
        assert(p->state != RUNNING, "found and running process in ready queue");
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
    __sched_assert_unlocked();
    asm volatile("wfi");  // Wait for interrupt
}

// Switch to the given process.
// The process will change its state to RUNNING afer the switching.
// Before calling this function, the caller must hold both the process's lock
//    and the scheduler lock.
// Returns the previous process that was running on this CPU.
static struct proc *__switch_to(struct proc *p) {
    __sched_assert_locked();
    
    assert(p != NULL, "Cannot switch to a NULL process");
    assert(p->state == RUNNABLE, "Cannot switch to a non-RUNNABLE process");
    assert(spin_holding(&p->lock), "Process lock must be held before switch");

    // Switch to the process's context
    mycpu()->proc = p; // Set the current process for this CPU
    p->state = RUNNING; // Set the process state to RUNNING

    struct proc * prev = (struct proc *)__swtch_context(&mycpu()->context, &p->context, 0);
    __sched_assert_locked();
    assert(prev != NULL, "Returned from a NULL process");
    assert(spin_holding(&prev->lock), "Process lock must be held after switch");

    mycpu()->proc = NULL; // Clear the current process for this CPU

    return prev;
}

void scheduler_run(void) {
    while (1) {
        intr_off(); // Disable interrupts to keep the atomicity of the scheduling operation
        sched_lock();
        struct proc *p = __sched_pick_next();
        if (!p) {
            sched_unlock();
            intr_on(); // allow interrupts to be handled
            __idle();
            continue;
        }

        spin_acquire(&p->lock);
        // printf("Switching to process: %s (pid: %d)\n", p->name, p->pid);
        assert(!intr_get(), "Interrupts must be disabled before switching to a process");
        struct proc *prev = __switch_to(p);
        assert(!intr_get(), "Interrupts must be disabled after switching to a process");
        if (prev->state == RUNNING) {
            prev->state = RUNNABLE; // If we returned to a RUNNING process, set it to RUNNABLE
            if (proc_queue_push(&ready_queue, prev) != 0) {
                panic("Failed to push process back to ready queue");
            }
        } else if (prev->state == SLEEPING && prev->chan != NULL) {
            if (proc_queue_push(&sleep_queue, prev) != 0) {
                panic("Failed to push process back to sleep queue");
            }
        }
        // printf("Scheduler returned to a %s process: %s (pid: %d)\n", procstate_to_str(prev->state), prev->name, prev->pid);
        spin_release(&p->lock);
        sched_unlock();
        intr_on();  // Enable interrupts after the scheduling operation
    }
}

// Yield the CPU to allow other processes to run.
// To avoid deadlocks, this function must be called with the process lock unlocked.
int scheduler_yield(uint64 *ret_arg) {
    struct proc *proc = myproc();
    assert(!spin_holding(&proc->lock), "Yield called with process lock held");

    push_off(); // Disable interrupts to keep the atomicity of the scheduling operation
    sched_lock();
    spin_acquire(&proc->lock);

    int noff = mycpu()->noff; // Save the noff state
    int intena = mycpu()->intena; // Save interrupt state
    uint64 ret_val = __swtch_context(&proc->context, &mycpu()->context, (uint64)proc);

    __sched_assert_locked();
    assert(!intr_get(), "Interrupts must be disabled after switching to a process");
    assert(myproc() == proc, "Yield returned to a different process");
    assert(spin_holding(&proc->lock), "Process lock must be held after yield");
    assert(proc->state == RUNNING, "Process state must be RUNNING after yield");

    mycpu()->intena = intena; // Restore interrupt state
    mycpu()->noff = noff; // Restore noff state

    spin_release(&proc->lock);
    sched_unlock();
    pop_off();
    
    if (ret_arg) {
        *ret_arg = ret_val; // Return the value from the context switch
    }

    return 0;
}

// Put the current process to sleep on the specified queue.
void scheduler_sleep(proc_queue_t *queue) {
    struct proc *proc = myproc();
    assert(proc != NULL, "Cannot sleep with a NULL process");
    assert(!spin_holding(&proc->lock), "Cannot sleep with process lock held");
    assert(queue != NULL, "Cannot sleep on a NULL queue");

    spin_acquire(&proc->lock);
    proc->state = SLEEPING;
    proc->chan = NULL;
    if (proc_queue_push(queue, proc) != 0) {
        panic("Failed to push process to sleep queue");
    }
    spin_release(&proc->lock);

    scheduler_yield(NULL); // Switch to the scheduler
}

// Wake up a process from the sleep queue.
static void __scheduler_wakeup_locked(struct proc *p) {
    __sched_assert_locked();
    assert(p != NULL, "Cannot wake up a NULL process");
    assert(!spin_holding(&p->lock), "Process lock must not be held before waking up");
    push_off(); // Disable interrupts to keep the atomicity of the scheduling operation
    spin_acquire(&p->lock);
    assert(p->state == SLEEPING || p->state == PROC_INITIALIZED, 
           "Process must be SLEEPING or PROC_INITIALIZED to wake up");
    assert(!proc_in_queue(p, NULL), "Process must not be in any queue before waking up");
    assert(p != myproc(), "Cannot wake up the current process");

    p->state = RUNNABLE; // Change state to RUNNABLE
    p->chan = NULL; // Clear the channel
    if (proc_queue_push(&ready_queue, p) != 0) {
        panic("Failed to push process to ready queue");
    }
    
    spin_release(&p->lock);
    pop_off(); // Re-enable interrupts
}

void scheduler_wakeup(struct proc *p) {
    __sched_assert_unlocked();
    sched_lock();
    __scheduler_wakeup_locked(p);
    sched_unlock();
}

void scheduler_sleep_on_chan(void *chan, struct spinlock *lk) {
    struct proc *proc = myproc();
    assert(proc != NULL, "Cannot sleep with a NULL process");
    assert(!spin_holding(&proc->lock), "Cannot sleep with process lock held");
    assert(chan != NULL, "Cannot sleep on a NULL channel");

    push_off(); // Disable interrupts to keep the atomicity of the scheduling operation
    spin_acquire(&proc->lock);
    int lk_holding = 0;
    if (lk != NULL) {
        lk_holding = spin_holding(lk);
        if (lk_holding) {
            spin_release(lk); // Release the lock if provided
        }
    }

    proc->state = SLEEPING;
    proc->chan = chan;
    spin_release(&proc->lock);

    scheduler_yield(NULL); // Switch to the scheduler
    pop_off(); // Re-enable interrupts

    if (lk != NULL && lk_holding) {
        spin_acquire(lk); // Acquire the lock if it was held
    }
}

void scheduler_wakeup_on_chan(void *chan) {
    push_off();
    sched_lock();

    struct proc *p, *tmp;
    proc_queue_foreach_unlocked(&sleep_queue, p, tmp) {
        spin_acquire(&p->lock);
        if (p->chan == chan) {
            assert(proc_queue_remove(&sleep_queue, p) == 0, "Failed to remove process from sleep queue");
            spin_release(&p->lock);
            __scheduler_wakeup_locked(p);
        } else {
            spin_release(&p->lock);
        }
    }

    sched_unlock();
    pop_off();
}
