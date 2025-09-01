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
#include "slab.h"
#include "rbtree.h"
#include "signal.h"
#include "errno.h"
#include "timer.h"

// Locking order:
// - sleep_lock
// - proc_lock
// - sched_lock

static proc_tree_t __chan_queue_root;
static struct timer_root __sched_timer;

list_node_t ready_queue;
static spinlock_t __sched_lock;   // ready_queue and sleep_queue share this lock
static spinlock_t __sleep_lock; // Lock for sleep queues

static void chan_queue_init(void) {
    spin_init(&__sleep_lock, "sleep_lock");
    proc_tree_init(&__chan_queue_root, "chan_queue_root", &__sleep_lock);
}

int chan_holding(void) {
    return spin_holding(&__sleep_lock);
}

void sleep_lock(void) {
    spin_acquire(&__sleep_lock);
}

void sleep_unlock(void) {
    spin_release(&__sleep_lock);
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

void sched_lock(void) {
    spin_acquire(&__sched_lock);
}

void sched_unlock(void) {
    spin_release(&__sched_lock);
}

/* Scheduler functions */
void scheduler_init(void) {
    spin_init(&__sched_lock, "sched_lock");
    list_entry_init(&ready_queue);
    chan_queue_init();
    timer_init(&__sched_timer);
}

void __scheduler_add_ready(struct proc *p) {
    assert(p != NULL, "Cannot add NULL process to ready queue");
    __sched_assert_holding();
    proc_assert_holding(p);
    enum procstate pstate = __proc_get_pstate(p);
    assert(pstate == PSTATE_RUNNABLE, "Process must be in RUNNABLE state to be added to ready queue");

    list_node_push(&ready_queue, p, sched_entry);
}

// Pick the next process to run from the ready queue.
// Returns a RUNABLE state process or NULL if no process is ready.
// The process returned will be locked
static struct proc *__sched_pick_next(void) {
    sched_lock();
    struct proc *p = list_node_pop_back(&ready_queue, struct proc, sched_entry);
    sched_unlock();
    
    if (p) {
        proc_lock(p);
        enum procstate pstate = __proc_get_pstate(p);
        if (pstate != PSTATE_RUNNABLE) {
            assert(pstate != PSTATE_RUNNING, "found and running process in ready queue");
            assert(pstate != PSTATE_INTERRUPTIBLE, "try to schedule an interruptible process");
            assert(pstate != PSTATE_UNINTERRUPTIBLE, "try to schedule an uninterruptible process");
            assert(pstate != PSTATE_UNUSED, "try to schedule an uninitialized process");
            assert(pstate != PSTATE_ZOMBIE, "found and zombie process in ready queue");
            panic("try to schedule unknown process state");
        }
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
    assert(__proc_get_pstate(p) == PSTATE_RUNNABLE, "Cannot switch to a non-RUNNABLE process");
    proc_assert_holding(p);

    // Switch to the process's context
    mycpu()->proc = p; // Set the current process for this CPU
    __proc_set_pstate(p, PSTATE_RUNNING); // Set the process state to RUNNING

    struct spinlock * lk = (struct spinlock *)__swtch_context(&mycpu()->context, &p->context, 0);
    proc_assert_holding(p);
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
        enum procstate pstate = __proc_get_pstate(p);
        struct proc *pparent = p->parent;

        if (pstate == PSTATE_RUNNING) {
            __proc_set_pstate(p, PSTATE_RUNNABLE); // If we returned to a RUNNING process, set it to RUNNABLE
            if (!PROC_STOPPED(p)) {
                __scheduler_add_ready(p); // Add the process back to the ready queue
            }
        }
        sched_unlock();
        // printf("CPU: %d <- %s: %s (pid: %d)\n", cpuid(), p->name, procstate_to_str(p->state), p->pid);
        proc_unlock(p);
        if (chan_holding()) {
            sleep_unlock();
        }
        if (lk != NULL) {
            spin_release(lk); // Release the lock returned by __swtch_context
        }

        if (pstate == PSTATE_ZOMBIE) {
            wakeup_on_chan(pparent);
        }
    }
}

// Yield the CPU to allow other processes to run.
// lk will not be re-acquired after yielding.
void scheduler_yield(struct spinlock *lk) {
    push_off();
    struct proc *proc = myproc();
    proc_assert_holding(proc);
    __sched_assert_holding();
    int lk_holding = (lk != NULL && spin_holding(lk));
    pop_off();

    assert(!intr_get(), "Interrupts must be disabled after switching to a process");
    int intena = mycpu()->intena; // Save interrupt state
    int noff_expected = 2;
    if (lk_holding) {
        noff_expected++;
    }
    if (chan_holding()) {
        noff_expected++;
    }
    assert(mycpu()->noff == noff_expected, 
           "Process must hold and only hold the proc lock and sched lock when yielding. Current noff: %d", mycpu()->noff);
    PROC_CLEAR_NEEDS_RESCHED(proc);
    __swtch_context(&proc->context, &mycpu()->context, (uint64)lk);

    assert(!intr_get(), "Interrupts must be disabled after switching to a process");
    assert(myproc() == proc, "Yield returned to a different process");
    proc_assert_holding(proc);
    assert(__proc_get_pstate(proc) == PSTATE_RUNNING, "Process state must be RUNNING after yield");

    mycpu()->intena = intena; // Restore interrupt state
}

// Change the process state to SLEEPING and yield CPU
// This function will lock both the process and scheduler locks.
void scheduler_sleep(struct spinlock *lk) {
    push_off(); // Increase noff counter to ensure interruptions are disabled
    struct proc *proc = myproc();
    assert(proc != NULL, "PCB is NULL");
    proc_assert_holding(proc);
    assert(PROC_SLEEPING(proc), "Process must be in either INTERRUPTIBLE or UNINTERRUPTIBLE state to sleep");
    int lk_holding = (lk != NULL && spin_holding(lk));
    pop_off();  // Safe to decrease noff counter after acquiring the process lock

    sched_lock();
    scheduler_yield(lk); // Switch to the scheduler
    sched_unlock();

    // Because the process lock is acquired after lk, we need to release it before acquiring lk.
    // The process lock will be held after acquiring lk, if the process lock was held 
    // before the yield.
    proc_unlock(proc);
    if (lk_holding) {
        spin_acquire(lk);
    }
    proc_lock(proc); // Reacquire the process lock after yielding
}

// Put the current process to sleep in interruptible state,
// so it would pause until receiving a signal.
void scheduler_pause(struct spinlock *lk) {
    struct proc *current = myproc();
    assert(current != NULL, "Cannot pause a NULL process");
    proc_assert_holding(current);
    __proc_set_pstate(current, PSTATE_INTERRUPTIBLE);
    scheduler_sleep(lk); // Sleep with the specified lock
}

void scheduler_stop(struct proc *p) {
    if (!p) {
        return; // Invalid process
    }
    proc_assert_holding(p);
    if (!PROC_STOPPED(p)) {
        PROC_SET_STOPPED(p); // Set the process as stopped
    }
}

void scheduler_continue(struct proc *p) {
    push_off(); // Increase noff counter to ensure interruptions are disabled
    proc_assert_holding(p);
    if (!PROC_STOPPED(p)) {
        pop_off(); // Balance push_off before early return
        return; // Process is not stopped, nothing to do
    }
    __sched_assert_holding();
    pop_off(); // Safe to decrease noff counter after acquiring the process lock
    // __sched_assert_unholding();
    assert(p != NULL, "Cannot wake up a NULL process");
    assert(p != myproc(), "Cannot wake up the current process");

    PROC_CLEAR_STOPPED(p); // Clear the stopped flag
    if (__proc_get_pstate(p) == PSTATE_RUNNABLE) {
        __scheduler_add_ready(p); // Add the process back to the ready queue
    }
}

// Wake up a process from the sleep queue.
void scheduler_wakeup(struct proc *p) {
    push_off(); // Increase noff counter to ensure interruptions are disabled
    __sched_assert_holding();
    proc_assert_holding(p);
    pop_off(); // Safe to decrease noff counter after acquiring the process lock
    // __sched_assert_unholding();
    assert(p != NULL, "Cannot wake up a NULL process");
    assert(p != myproc(), "Cannot wake up the current process");
    if (p->chan != NULL) {
        PROC_CLEAR_ONCHAN(p); // Clear the ONCHAN flag if the process is sleeping on a channel
        p->chan = NULL; // Clear the channel
    }
    if (!PROC_SLEEPING(p)) {
        return; // Process is not sleeping, nothing to do
    }

    __proc_set_pstate(p, PSTATE_RUNNABLE);

    if (!PROC_STOPPED(p)) {
        __scheduler_add_ready(p); // Add the process back to the ready queue
    }
}

void sleep_on_chan(void *chan, struct spinlock *lk) {
    sleep_lock();
    assert(myproc() != NULL, "PCB is NULL");
    assert(chan != NULL, "Cannot sleep on a NULL channel");

    myproc()->chan = chan;
    PROC_SET_ONCHAN(myproc());
    int ret = proc_tree_wait(&__chan_queue_root, (uint64)chan, lk, NULL);
    myproc()->chan = NULL;
    PROC_CLEAR_ONCHAN(myproc());
    // @TODO: process return value
    (void)ret;
}

void wakeup_on_chan(void *chan) {
    sleep_lock();
    int ret = proc_tree_wakeup_key(&__chan_queue_root, (uint64)chan, 0, 0);
    // @TODO: process return value
    (void)ret;
    sleep_unlock();
}

void scheduler_dump_chan_queue(void) {
    struct proc_node *node = NULL;
    struct proc_node *tmp = NULL;

    printf("Channel Queue Dump:\n");
    rb_foreach_entry_safe(&__chan_queue_root.root, node, tmp, tree.entry) {
        struct proc *proc = proc_node_get_proc(node);
        if (proc == NULL) {
            printf("  Process: NULL\n");
            continue;
        }
        printf("Chan: %p,  Proc: %s (PID: %d, State: %s)\n", proc->chan, proc->name, proc->pid, procstate_to_str(__proc_get_pstate(proc)));
    }
}

uint64 sys_dumpchan(void) {
    // This function is called from the dumpchan user program.
    // It dumps the channel queue to the console.
    sleep_lock();
    scheduler_dump_chan_queue();
    sleep_unlock();
    return 0;
}
