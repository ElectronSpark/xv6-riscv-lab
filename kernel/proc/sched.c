#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc/proc.h"
#include "defs.h"
#include "printf.h"
#include "list.h"
#include "hlist.h"
#include "proc/proc_queue.h"
#include "proc/sched.h"
#include "slab.h"
#include "rbtree.h"
#include "signal.h"
#include "errno.h"
#include "sched_timer_private.h"
#include "rcu.h"
#include "timer.h"

// Locking order:
// - sleep_lock
// - proc_lock
// - sched_lock

static proc_tree_t __chan_queue_root;

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
}

void __scheduler_add_ready(struct proc *p) {
    assert(p != NULL, "Cannot add NULL process to ready queue");
    __sched_assert_holding();
    enum procstate pstate = __proc_get_pstate(p);
    assert(pstate == PSTATE_RUNNING, 
           "Process must be in RUNNING state to be added to ready queue");
    
    // Check on_rq flag - if already on a queue, skip (like Linux)
    if (smp_load_acquire(&p->on_rq)) {
        return; // Already on a run queue, nothing to do
    }
    
    // Check if already in a list (prev and next point to self when detached)
    if (p->sched_entry.prev != &p->sched_entry || p->sched_entry.next != &p->sched_entry) {
        panic("__scheduler_add_ready: process %s (pid %d) already in a queue!", 
              p->name, p->pid);
    }

    smp_store_release(&p->on_rq, 1); // Mark as on run queue
    list_node_push(&ready_queue, p, sched_entry);
}

// Pick the next process to run from the ready queue.
// Returns a RUNABLE state process or NULL if no process is ready.
// The process returned will be locked
static struct proc *__sched_pick_next(void) {
    sched_lock();
    struct proc *p = list_node_pop_back(&ready_queue, struct proc, sched_entry);
    if (p) {
        smp_store_release(&p->on_rq, 0); // Mark as off run queue (while holding sched_lock)
    }
    sched_unlock();
    
    if (p) {
        smp_store_release(&p->on_cpu, 1); // Mark the process as running on CPU
        enum procstate pstate = __proc_get_pstate(p);
        if (pstate != PSTATE_RUNNING) {
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

struct proc *process_switch_to(struct proc *current, struct proc *target) {
    // Update RCU timestamp before context switch
    uint64 now = get_jiffs();
    __atomic_store_n(&mycpu()->rcu_timestamp, now, __ATOMIC_RELEASE);
    
    mycpu()->proc = target;
    struct context *prev_context = __swtch_context(&current->context, &target->context);
    return container_of(prev_context, struct proc, context);
}

// Switch to the given process.
// The process will change its state to RUNNING afer the switching.
// Before calling this function, the caller must hold both the process's lock
//    and the scheduler lock.
// Returns the previous process that was running on this CPU.
static struct proc *__switch_to(struct proc *p) {
    __sched_assert_holding();
    assert(!intr_get(), "Interrupts must be disabled before switching to a process");
    struct proc *proc = myproc();
    int intena = mycpu()->intena; // Save interrupt state
    int spin_depth_expected = 1;
    if (chan_holding()) {
        spin_depth_expected++;
    }
    assert(mycpu()->spin_depth == spin_depth_expected, 
           "Process must hold and only hold the proc lock and sched lock when yielding. Current spin_depth: %d", mycpu()->spin_depth);

    // Switch to the process's context
    struct proc *prev = process_switch_to(proc, p);

    // After switching, the current process is now 'p'
    assert(!intr_get(), "Interrupts must be disabled before switching to a process");
    assert(myproc() == proc, "Yield returned to a different process");
    assert(__proc_get_pstate(proc) == PSTATE_RUNNING, "Process state must be RUNNING after yield");
    mycpu()->intena = intena; // Restore interrupt state

    return prev;
}

// Yield the CPU to allow other processes to run.
// lk will not be re-acquired after yielding.
void scheduler_yield(void) {
    push_off();
    struct proc *proc = myproc();
    struct proc *prev = NULL;

    assert(!CPU_IN_ITR(), "Cannot yield CPU in interrupt context");

    // Wake up processes with expired timers.
    __do_timer_tick();
    
    // Check if we should abort a pending sleep.
    // If state was changed back to RUNNING (e.g., by an interrupt waking us),
    // and we're not the idle process, we should abort and stay on CPU.
    // Note: PSTATE_RUNNING processes that call yield() for preemption will have
    // picked a next process, so this check passes through for them.
    enum procstate cur_state = __proc_get_pstate(proc);
    
    // Pick the next process to run
    struct proc *p = __sched_pick_next();
    
    // If our state changed back to RUNNING (woken before sleep completed),
    // and there's no other process to run (or only idle), stay on CPU
    if (cur_state == PSTATE_RUNNING && proc != mycpu()->idle_proc) {
        if (p == NULL || p == mycpu()->idle_proc) {
            // No other runnable process, just continue running
            pop_off();
            return;
        }
        // There is another process, but we were woken up too.
        // Put ourselves back on the ready queue via context_switch_finish
    }
    
    if (!p) {
        if (proc == mycpu()->idle_proc) {
            // Already in idle process, just return
            pop_off();
            return;
        }
        p = mycpu()->idle_proc;
        assert(p != NULL, "Idle process is NULL");
    }

    // prepare to switch
    context_switch_prepare(proc, p);
    
    
    prev = __switch_to(p);
    
    
    context_switch_finish(prev, myproc());
    pop_off();
}

// Change the process state to SLEEPING and yield CPU
// This function will lock both the process and scheduler locks.
void scheduler_sleep(struct spinlock *lk, enum procstate sleep_state) {
    struct proc *proc = myproc();
    assert(proc != NULL, "PCB is NULL");
    __proc_set_pstate(myproc(), sleep_state);
    assert(PROC_SLEEPING(proc), "Process must be in either INTERRUPTIBLE or UNINTERRUPTIBLE state to sleep");
    int lk_holding = (lk != NULL && spin_holding(lk));
    
    if (lk_holding) {
        spin_release(lk); // Release the lock returned by __swtch_context
    }
    scheduler_yield(); // Switch to the scheduler
    
    if (lk_holding) {
        spin_acquire(lk);
    }
}

// Put the current process to sleep in interruptible state,
// so it would pause until receiving a signal.
void scheduler_pause(struct spinlock *lk) {
    assert(myproc() != NULL, "Cannot pause a NULL process");
    scheduler_sleep(lk, PSTATE_INTERRUPTIBLE); // Sleep with the specified lock
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
    // Uses pi_lock protocol like __do_scheduler_wakeup.
    // Caller must hold p->pi_lock, must NOT hold proc_lock or sched_lock.
    assert(p != NULL, "Cannot continue a NULL process");
    assert(spin_holding(&p->pi_lock), "pi_lock must be held for scheduler_continue");
    assert(!spin_holding(&p->lock), "proc_lock must not be held for scheduler_continue");
    assert(!sched_holding(), "sched_lock must not be held for scheduler_continue");
    assert(p != myproc(), "Cannot continue the current process");

    if (!PROC_STOPPED(p)) {
        return; // Process is not stopped, nothing to do
    }

    // Wait for process to be off CPU before modifying state.
    // Like Linux's smp_cond_load_acquire in try_to_wake_up().
    // "One must be running (->on_cpu == 1) in order to remove oneself from the runqueue."
    smp_cond_load_acquire(&p->on_cpu, !VAL);

    PROC_CLEAR_STOPPED(p); // Clear the stopped flag

    // If process is RUNNING, add it back to the ready queue
    if (__proc_get_pstate(p) == PSTATE_RUNNING) {
        sched_lock();
        __scheduler_add_ready(p);
        sched_unlock();
    }
}

static void __scheduler_wakeup_assertion(struct proc *p) {
    assert(p != NULL, "Cannot wake up a NULL process");
    // Self-wakeup (p == myproc()) is only valid from interrupt context.
    // This happens when an interrupt completes I/O for a process that has set
    // its state to SLEEPING but hasn't context-switched out yet.
    if (p == myproc()) {
        assert(CPU_IN_ITR(), "Cannot wake up current process outside interrupt context");
    }
    assert(spin_holding(&p->pi_lock), "Process pi_lock must be held when waking up a process");
    assert(!spin_holding(&p->lock), "Process lock must not be held when waking up a process");
    assert(!sched_holding(), "Scheduler lock must not be held when waking up a process");
}

// Internal function to wake up a sleeping process.
// Modeled after Linux's try_to_wake_up() in kernel/sched/core.c
static void __do_scheduler_wakeup(struct proc *p) {
    // Special case: waking up the current process (p == myproc())
    // This happens when an interrupt wakes up a process that set its state to SLEEPING
    // but hasn't context-switched out yet. In this case, just change the state back
    // to RUNNING - the process will see this when it checks before context switching.
    // Linux: "We're waking current, this means 'p->on_rq'... we can special case"
    if (p == myproc()) {
        // The current process is still on CPU, just change state to abort the sleep
        enum procstate old_state = smp_load_acquire(&p->state);
        if (old_state == PSTATE_WAKENING || old_state == PSTATE_RUNNING) {
            return; // Already awake
        }
        // Only wake from sleeping states
        if (!PSTATE_IS_SLEEPING(old_state)) {
            return;
        }
        // Try to change state back to RUNNING - this will abort the pending sleep
        __atomic_compare_exchange_n(&p->state, &old_state, PSTATE_RUNNING,
                                    false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return;
    }
    
    // Linux pattern: check on_rq first, then smp_rmb, then on_cpu
    // "Ensure we load p->on_cpu _after_ p->on_rq, otherwise it would be
    //  possible to, falsely, observe p->on_cpu == 0."
    //
    // If on_rq is set, the task is already queued - no need for full wakeup
    // (In Linux this would call ttwu_runnable() to potentially preempt)
    smp_rmb();
    if (smp_load_acquire(&p->on_rq)) {
        return; // Already on a run queue, nothing to do
    }
    
    // Ensure we load on_cpu after on_rq (see Linux comment above)
    // "One must be running (->on_cpu == 1) in order to remove oneself from the runqueue."
    smp_rmb();
    
    // Wait for the process to finish context switching out (on_cpu == 0)
    // This is critical: we must not add to run queue while still on a CPU
    // Linux: smp_cond_load_acquire(&p->on_cpu, !VAL)
    // "Pairs with the smp_store_release() in finish_task()."
    smp_cond_load_acquire(&p->on_cpu, !VAL);
    
    // Atomically transition state from SLEEPING to WAKENING
    // This prevents multiple wakers from racing to add the task to the run queue
    enum procstate old_state = smp_load_acquire(&p->state);
    // Only wake up processes that are actually sleeping
    if (!PSTATE_IS_SLEEPING(old_state)) {
        return; // Process is not in a sleepable state (could be RUNNING, WAKENING, ZOMBIE, etc.)
    }
    bool success = __atomic_compare_exchange_n(&p->state, &old_state, PSTATE_WAKENING, 
                                               false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    if (!success) {
        // State changed - either someone else woke it, or it changed to a non-sleeping state
        return;
    }
    
    // Now enqueue the task
    // Linux: "We're doing the wakeup (@success == 1), they did a dequeue (p->on_rq == 0),
    //         which means we need to do an enqueue"
    // 
    // Set to RUNNING BEFORE enqueue so another CPU picking this process
    // from the run queue sees the correct state. The WAKENING state was just
    // to win the race of multiple wakers.
    smp_store_release(&p->state, PSTATE_RUNNING);
    
    if (!PROC_STOPPED(p)) {
        sched_lock();
        // __scheduler_add_ready checks on_rq and skips if already queued
        __scheduler_add_ready(p);
        sched_unlock();
    }
}

// unconditionally wake up a process from the sleep queue.
void scheduler_wakeup(struct proc *p) {
    __scheduler_wakeup_assertion(p);
    if (!PROC_SLEEPING(p)) {
        return; // Process is not sleeping, nothing to do
    }
    __do_scheduler_wakeup(p);
}

// Wake up a process sleeping in timer, timer_killable or interruptible state.
void scheduler_wakeup_timeout(struct proc *p) {
    __scheduler_wakeup_assertion(p);
    if (!PROC_TIMER(p)) {
        return; // Process is not in timer, timer_killable or interruptible state, nothing to do
    }
    __do_scheduler_wakeup(p);
}

void scheduler_wakeup_killable(struct proc *p) {
    __scheduler_wakeup_assertion(p);
    if (!PROC_KILLABLE(p)) {
        return; // Process is not in killable state, nothing to do
    }
    __do_scheduler_wakeup(p);
}

void scheduler_wakeup_interruptible(struct proc *p) {
    __scheduler_wakeup_assertion(p);
    if (!PROC_INTERRUPTIBLE(p)) {
        return; // Process is not in interruptible state, nothing to do
    }
    __do_scheduler_wakeup(p);
}

void sleep_on_chan(void *chan, struct spinlock *lk) {
    sleep_lock();
    assert(myproc() != NULL, "PCB is NULL");
    assert(chan != NULL, "Cannot sleep on a NULL channel");

    myproc()->chan = chan;
    PROC_SET_ONCHAN(myproc());
    
    // Release caller's lock if held (we'll reacquire after waking)
    int lk_holding = (lk != NULL && spin_holding(lk));
    if (lk_holding) {
        spin_release(lk);
    }
    
    // proc_tree_wait will release sleep_lock via scheduler_sleep,
    // keeping the tree protected during add operation.
    // After waking, scheduler_sleep will reacquire sleep_lock.
    int ret = proc_tree_wait(&__chan_queue_root, (uint64)chan, &__sleep_lock, NULL);
    
    // sleep_lock is already held here (reacquired by scheduler_sleep)
    PROC_CLEAR_ONCHAN(myproc());
    myproc()->chan = NULL;
    sleep_unlock();
    
    // Reacquire caller's lock if it was held
    if (lk_holding) {
        spin_acquire(lk);
    }
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

// Unconditionally wake up process
void wakeup_proc(struct proc *p)
{
    if (p == NULL) {
        return;
    }
    spin_acquire(&p->pi_lock);
    scheduler_wakeup(p);
    spin_release(&p->pi_lock);
}

void wakeup_timeout(struct proc *p) {
    if (p == NULL) {
        return;
    }
    spin_acquire(&p->pi_lock);
    scheduler_wakeup_timeout(p);
    spin_release(&p->pi_lock);
}

void wakeup_killable(struct proc *p) {
    if (p == NULL) {
        return;
    }
    spin_acquire(&p->pi_lock);
    scheduler_wakeup_killable(p);
    spin_release(&p->pi_lock);
}

// Wake up a process sleeping in interruptible state
void wakeup_interruptible(struct proc *p)
{
    if (p == NULL) {
        return;
    }
    spin_acquire(&p->pi_lock);
    scheduler_wakeup_interruptible(p);
    spin_release(&p->pi_lock);
}

uint64 sys_dumpchan(void) {
    // This function is called from the dumpchan user program.
    // It dumps the channel queue to the console.
    sleep_lock();
    scheduler_dump_chan_queue();
    sleep_unlock();
    return 0;
}

// Context Switching Helpers
// Refer to how Linux handle context switching and wakeup race conditions

// Process sleep routine:
// - process state change to one of SLEEPING states
// - add sleep queue
// - mark on_rq = false in context_switch_prepare
// - context switch out
// - mark on_cpu = false in context_switch_finish

void context_switch_prepare(struct proc *prev, struct proc *next) {
    assert(prev != NULL, "Previous process is NULL");
    assert(next != NULL, "Next process is NULL");

    // Mark the next process as on the CPU
    smp_store_release(&next->on_cpu, 1);
    sched_lock();
    next->cpu_id = cpuid();
}

void context_switch_finish(struct proc *prev, struct proc *next) {
    assert(prev != NULL, "Previous process is NULL");
    assert(next != NULL, "Next process is NULL");

    enum procstate pstate = __proc_get_pstate(prev);
    struct proc *pparent = prev->parent;

    // In Linux kernel order: first add back to queue (if needed), then clear on_cpu
    // This is because wakeup path waits for on_cpu to be 0 before adding to queue
    if (pstate == PSTATE_RUNNING && prev != mycpu()->idle_proc) {
        if (!PROC_STOPPED(prev)) {
            // __scheduler_add_ready will check on_rq and skip if already queued
            __scheduler_add_ready(prev);
        }
    }
    sched_unlock();
    
    // Now safe to mark as not on CPU - wakeup path can proceed
    smp_store_release(&prev->on_cpu, 0);
    
    if (chan_holding()) {
        sleep_unlock();
    }

    if (pstate == PSTATE_ZOMBIE && pparent != NULL && pparent != next) {
        // Wake up the parent only if it's not the current process
        wakeup_interruptible(pparent);
    }

    // Check RCU callbacks - context switch is a quiescent state
    rcu_check_callbacks();
    rcu_process_callbacks();
}
