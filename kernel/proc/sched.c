#include "types.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "lock/spinlock.h"
#include "lock/rcu.h"
#include "proc/proc.h"
#include "defs.h"
#include "printf.h"
#include "list.h"
#include "hlist.h"
#include "proc/proc_queue.h"
#include "proc/sched.h"
#include "proc/rq.h"
#include <mm/slab.h>
#include "rbtree.h"
#include "signal.h"
#include "errno.h"
#include "timer/sched_timer_private.h"
#include "timer/timer.h"

// Locking order:
// - sleep_lock
// - proc_lock
// - rq_lock (per-CPU run queue lock)

static proc_tree_t __chan_queue_root;

static spinlock_t __sleep_lock = SPINLOCK_INITIALIZED("sleep_lock"); // Lock for sleep queues

static void chan_queue_init(void) {
    proc_tree_init(&__chan_queue_root, "chan_queue_root", &__sleep_lock);
}

int chan_holding(void) {
    return spin_holding(&__sleep_lock);
}

void sleep_lock(void) {
    spin_lock(&__sleep_lock);
}

void sleep_unlock(void) {
    spin_unlock(&__sleep_lock);
}

int sleep_lock_irqsave(void) {
    return spin_lock_irqsave(&__sleep_lock);
}

void sleep_unlock_irqrestore(int state) {
    spin_unlock_irqrestore(&__sleep_lock, state);
}

/* Scheduler lock functions */
// These functions are used to acquire and release the scheduler lock.
// To avoid deadlocks, locks must be held in the following order:
// - locks of each process -- proc.c
// - locks related to the process queue -- proc_queue.c
// - rq_lock (per-CPU run queue lock) -- rq.c
// - proc table lock -- proc.c
//
// The locks of task queues and the rq_lock should not be held simultaneously,
int sched_holding(void) {
    // Check if the current CPU's rq lock is held
    return rq_holding_current();
}

/* Scheduler lock assertions */
static inline void __sched_assert_holding(void) {
    assert(rq_holding_current(), "rq_lock must be held");
}

static inline void __sched_assert_unholding(void) {
    assert(!rq_holding_current(), "rq_lock must not be held");
}

/* Scheduler functions */
void scheduler_init(void) {
    chan_queue_init();
    rq_global_init();
}

// Pick the next process to run from the run queue.
// Returns a RUNNABLE state process or NULL if no process is ready.
// Caller must hold rq_lock.
static struct proc *__sched_pick_next(void) {
    __sched_assert_holding();
    
    struct rq *rq = pick_next_rq();
    if (IS_ERR_OR_NULL(rq)) {
        return NULL;
    }
    
    struct sched_entity *se = rq_pick_next_task(rq);
    if (se == NULL) {
        return NULL;
    }
    
    struct proc *p = se->proc;
    assert(p != NULL, "__sched_pick_next: se->proc is NULL");
    
    // Prepare the task for running - this removes it from the queue
    rq_set_next_task(se);
    smp_store_release(&se->on_rq, 0); // Mark as off run queue (while holding rq_lock)
    
    smp_store_release(&se->on_cpu, 1); // Mark the process as running on CPU
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

struct proc *process_switch_to(struct proc *current, struct proc *target) {
    // Update RCU timestamp before context switch
    uint64 now = get_jiffs();
    __atomic_store_n(&mycpu()->rcu_timestamp, now, __ATOMIC_RELEASE);
    
    mycpu()->proc = target;
    struct context *prev_context = __swtch_context(&current->sched_entity->context, &target->sched_entity->context);
    return proc_from_context(prev_context);
}

// Switch to the given process.
// The process will change its state to RUNNING afer the switching.
// Before calling this function, the caller must hold the rq_lock.
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
    assert(mycpu()->noff == 0, 
           "Process must not hold any other locks when yielding. Current noff: %d", mycpu()->noff);
    assert(mycpu()->spin_depth == spin_depth_expected, 
           "Process must hold and only hold the rq_lock when yielding. Current spin_depth: %d", mycpu()->spin_depth);

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
    // Wake up processes with expired timers.
    // It may add processes to the run queue, so do it before acquiring rq_lock.
    __do_timer_tick();

    int intr = rq_lock_current_irqsave();
    struct proc *proc = myproc();
    struct proc *prev = NULL;

    assert(!CPU_IN_ITR(), "Cannot yield CPU in interrupt context");

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
            // This is normal - process was woken but nothing else to switch to
            rq_unlock_current_irqrestore(intr);
            return;
        }
        // There is another process, but we were woken up too.
        // Put ourselves back on the ready queue via context_switch_finish
    }
    
    if (!p) {
        if (proc == mycpu()->idle_proc) {
            // Already in idle process, just return
            rq_unlock_current_irqrestore(intr);
            return;
        }
        p = mycpu()->idle_proc;
        assert(p != NULL, "Idle process is NULL");
    }

    // prepare to switch
    context_switch_prepare(proc, p);
    
    prev = __switch_to(p);
    
    context_switch_finish(prev, myproc(), intr);
}

// Change the process state to SLEEPING and yield CPU
// This function will lock both the process and scheduler locks.
void scheduler_sleep(struct spinlock *lk, enum procstate sleep_state) {
    int intr = intr_off_save();
    struct proc *proc = myproc();
    assert(proc != NULL, "PCB is NULL");
    __proc_set_pstate(myproc(), sleep_state);
    assert(PROC_SLEEPING(proc), "Process must be in either INTERRUPTIBLE or UNINTERRUPTIBLE state to sleep");
    int lk_holding = (lk != NULL && spin_holding(lk));
    
    if (lk_holding) {
        spin_unlock(lk); // Release the lock returned by __swtch_context
    }
    scheduler_yield(); // Switch to the scheduler
    
    if (lk_holding) {
        spin_lock(lk);
    }
    intr_restore(intr);
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
    // Caller must hold p->sched_entity->pi_lock, must NOT hold proc_lock or rq_lock.
    assert(p != NULL, "Cannot continue a NULL process");
    assert(spin_holding(&p->sched_entity->pi_lock), "pi_lock must be held for scheduler_continue");
    assert(!spin_holding(&p->lock), "proc_lock must not be held for scheduler_continue");
    assert(!sched_holding(), "rq_lock must not be held for scheduler_continue");
    assert(p != myproc(), "Cannot continue the current process");

    if (!PROC_STOPPED(p)) {
        return; // Process is not stopped, nothing to do
    }

    // Wait for process to be off CPU before modifying state.
    // Like Linux's smp_cond_load_acquire in try_to_wake_up().
    // "One must be running (->on_cpu == 1) in order to remove oneself from the runqueue."
    smp_cond_load_acquire(&p->sched_entity->on_cpu, !VAL);

    PROC_CLEAR_STOPPED(p); // Clear the stopped flag

    // If process is RUNNING, add it back to the ready queue
    if (__proc_get_pstate(p) == PSTATE_RUNNING) {
        // Select which run queue to put the task into
        push_off();  // Disable interrupts to avoid CPU migration during rq selection
        struct sched_entity *se = p->sched_entity;
        struct rq *rq = rq_select_task_rq(se, se->affinity_mask);
        assert(!IS_ERR_OR_NULL(rq), "scheduler_continue: rq_select_task_rq failed");
        
        // Lock the target CPU's run queue
        rq_lock(rq->cpu_id);
        pop_off();  // Restore interrupt state (acquiring spinlock increases intr counter)
        
        // CAS on on_rq to serialize with context_switch_finish race fix path
        int expected = 0;
        if (__atomic_compare_exchange_n(&se->on_rq, &expected, 1,
                                        false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            rq_enqueue_task(rq, se);
        }
        
        rq_unlock(rq->cpu_id);
    }
}

static void __scheduler_wakeup_assertion(struct proc *p) {
    assert(p != NULL, "Cannot wake up a NULL process");
    assert(spin_holding(&p->sched_entity->pi_lock), "Process pi_lock must be held when waking up a process");
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
    if (smp_load_acquire(&p->sched_entity->on_rq)) {
        return; // Already on a run queue, nothing to do
    }
    
    // Ensure we load on_cpu after on_rq (see Linux comment above)
    // "One must be running (->on_cpu == 1) in order to remove oneself from the runqueue."
    smp_rmb();
    
    // Wait for the process to finish context switching out (on_cpu == 0)
    // This is critical: we must not add to run queue while still on a CPU
    // Linux: smp_cond_load_acquire(&p->sched_entity->on_cpu, !VAL)
    // "Pairs with the smp_store_release() in finish_task()."
    // 
    // No deadlock: context_switch_finish doesn't need pi_lock anymore,
    // it uses CAS on on_rq instead to serialize with us.
    smp_cond_load_acquire(&p->sched_entity->on_cpu, !VAL);
    
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
    
    // Set to RUNNING BEFORE enqueue so another CPU picking this process
    // from the run queue sees the correct state. The WAKENING state was just
    // to win the race of multiple wakers.
    smp_store_release(&p->state, PSTATE_RUNNING);
    
    if (!PROC_STOPPED(p)) {
        // Select which run queue to put the task into
        push_off();  // Disable interrupts to avoid CPU migration during rq selection
        struct sched_entity *se = p->sched_entity;
        struct rq *rq = rq_select_task_rq(se, se->affinity_mask);
        assert(!IS_ERR_OR_NULL(rq), "__do_scheduler_wakeup: rq_select_task_rq failed");
        
        // Lock the target CPU's run queue
        rq_lock(rq->cpu_id);
        pop_off();  // Restore interrupt state (acquiring spinlock increases intr counter)
        
        // CAS on on_rq to serialize with context_switch_finish race fix path
        int expected = 0;
        if (__atomic_compare_exchange_n(&se->on_rq, &expected, 1,
                                        false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            rq_enqueue_task(rq, se);
        }
        // If CAS failed, context_switch_finish already enqueued
        
        rq_unlock(rq->cpu_id);
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
    int intr = sleep_lock_irqsave();
    assert(myproc() != NULL, "PCB is NULL");
    assert(chan != NULL, "Cannot sleep on a NULL channel");

    myproc()->chan = chan;
    PROC_SET_ONCHAN(myproc());
    
    // Release caller's lock if held (we'll reacquire after waking)
    int lk_holding = (lk != NULL && spin_holding(lk));
    if (lk_holding) {
        spin_unlock(lk);
    }
    
    // proc_tree_wait will release sleep_lock via scheduler_sleep,
    // keeping the tree protected during add operation.
    // After waking, scheduler_sleep will reacquire sleep_lock.
    int ret = proc_tree_wait(&__chan_queue_root, (uint64)chan, NULL, NULL);
    
    // Re-acqiore sleep lock.
    // Discard saved interrupt state because it was saved before sleeping.
    sleep_lock_irqsave();
    PROC_CLEAR_ONCHAN(myproc());
    myproc()->chan = NULL;
    sleep_unlock_irqrestore(intr);
    
    // Reacquire caller's lock if it was held
    if (lk_holding) {
        spin_lock(lk);
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
    spin_lock(&p->sched_entity->pi_lock);
    scheduler_wakeup(p);
    spin_unlock(&p->sched_entity->pi_lock);
}

void wakeup_timeout(struct proc *p) {
    if (p == NULL) {
        return;
    }
    spin_lock(&p->sched_entity->pi_lock);
    scheduler_wakeup_timeout(p);
    spin_unlock(&p->sched_entity->pi_lock);
}

void wakeup_killable(struct proc *p) {
    if (p == NULL) {
        return;
    }
    spin_lock(&p->sched_entity->pi_lock);
    scheduler_wakeup_killable(p);
    spin_unlock(&p->sched_entity->pi_lock);
}

// Wake up a process sleeping in interruptible state
void wakeup_interruptible(struct proc *p)
{
    if (p == NULL) {
        return;
    }
    spin_lock(&p->sched_entity->pi_lock);
    scheduler_wakeup_interruptible(p);
    spin_unlock(&p->sched_entity->pi_lock);
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
//
// These functions handle the critical transitions when switching between processes.
// The implementation follows Linux's pattern for handling context switching and
// wakeup race conditions.
//
// Process sleep routine:
//   1. Process state changes to one of SLEEPING states
//   2. Add to sleep queue (if using channel-based sleep)
//   3. Call scheduler_yield() which:
//      a. context_switch_prepare(): mark next->on_cpu = 1
//      b. __switch_to(): perform actual context switch
//      c. context_switch_finish(): mark prev->on_cpu = 0
//
// Memory ordering invariant (Linux pattern):
//   Writer (context_switch_finish):
//     on_rq = 1          (if RUNNING, put back on queue)
//     rq_unlock()        (release barrier)
//     on_cpu = 0         (signal completion)
//
//   Reader (waker in __do_scheduler_wakeup):
//     read on_rq         (if set, already queued - return)
//     smp_rmb()
//     wait on_cpu == 0   (smp_cond_load_acquire)
//     CAS state          (claim the wakeup)
//     enqueue task
//
// IMPORTANT: Parent wakeup for zombie processes is done in __exit_yield()
// BEFORE the zombie calls scheduler_yield(). This matches Linux's pattern
// where do_notify_parent() is called before do_task_dead().
//
// Doing the wakeup in context_switch_finish (after context switch) caused a
// livelock on real hardware: the waker would spin on
// smp_cond_load_acquire(&parent->on_cpu, !VAL) waiting for the parent to
// finish its context switch, creating memory contention.

// Prepare for context switch from prev to next.
// Caller must hold the current CPU's rq_lock (asserted).
void context_switch_prepare(struct proc *prev, struct proc *next) {
    assert(prev != NULL, "Previous process is NULL");
    assert(next != NULL, "Next process is NULL");
    __sched_assert_holding();

    // Mark the next process as on the CPU
    smp_store_release(&next->sched_entity->on_cpu, 1);
    next->sched_entity->cpu_id = cpuid();
    if (PROC_ZOMBIE(prev)) {
        // Previous process is exiting, clean up scheduler resources
        rq_task_dead(prev->sched_entity);
    }
}

// Finish context switch - clean up prev task and release rq_lock.
// This function releases the current CPU's rq_lock.
void context_switch_finish(struct proc *prev, struct proc *next, int intr) {
    assert(prev != NULL, "Previous process is NULL");
    assert(next != NULL, "Next process is NULL");

    enum procstate pstate = __proc_get_pstate(prev);
    struct sched_entity *se = prev->sched_entity;

    // Handle prev task based on its state.
    // Follow Linux kernel order: first add back to queue (if needed), then clear on_cpu.
    // This is because wakeup path waits for on_cpu to be 0 before adding to queue.
    // @TODO: try not to treat idle process specially
    int did_enqueue = 0;
    if (prev != mycpu()->idle_proc) {
        if (pstate == PSTATE_RUNNING && !PROC_STOPPED(prev)) {
            // Process is still running, put it back to the queue.
            // Note: If affinity was changed to exclude current CPU, the task
            // stays here until it sleeps. On wakeup, rq_select_task_rq() will
            // place it on an allowed CPU.
            rq_put_prev_task(se);
            smp_store_release(&se->on_rq, 1);
            did_enqueue = 1;
        } else if (PSTATE_IS_SLEEPING(pstate) || PROC_STOPPED(prev)) {
            // Process entered sleeping/stopped state, properly dequeue it
            // so that wakeup path can enqueue to a (potentially different) rq.
            if (se->rq != NULL) {
                rq_dequeue_task(se->rq, se);
            }
        }
        // If zombie, rq_task_dead was already called in context_switch_prepare
    }
    
    // Race fix: If we didn't enqueue (saw SLEEPING state), but a waker already
    // set state to RUNNING via CAS, we must enqueue now.
    // Use CAS on on_rq to serialize with wakeup path - no pi_lock needed here.
    // This avoids deadlock where waker holds pi_lock spinning on on_cpu.
    if (prev != mycpu()->idle_proc && !did_enqueue && !PROC_ZOMBIE(prev) && !PROC_STOPPED(prev)) {
        enum procstate new_pstate = __proc_get_pstate(prev);
        if (new_pstate == PSTATE_RUNNING) {
            // State changed to RUNNING after our initial read - try to enqueue
            // CAS ensures only one path (us or waker) succeeds
            int expected = 0;
            if (__atomic_compare_exchange_n(&se->on_rq, &expected, 1,
                                            false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                push_off();
                struct rq *rq = rq_select_task_rq(se, se->affinity_mask);
                assert(!IS_ERR_OR_NULL(rq), "context_switch_finish: rq_select_task_rq failed");
                rq_lock(rq->cpu_id);
                pop_off();
                rq_enqueue_task(rq, se);
                rq_unlock(rq->cpu_id);
            }
            // If CAS failed, waker already set on_rq = 1 and will/did enqueue
        }
    }
    
    // Now safe to mark as not on CPU - wakeup path can proceed.
    // Pairs with smp_cond_load_acquire() in __do_scheduler_wakeup().
    smp_store_release(&prev->sched_entity->on_cpu, 0);
    
    // Release sleep_lock BEFORE rq_lock to avoid deadlock with interrupt handlers.
    // sleep_lock was acquired with sleep_lock_irqsave(), so use sleep_unlock_irqrestore().
    // Pass 0 to NOT enable interrupts yet - let rq_unlock_current_irqrestore handle that.
    if (chan_holding()) {
        sleep_unlock_irqrestore(0);
    }

    rq_unlock_current_irqrestore(intr);

    if (pstate == PSTATE_ZOMBIE && pparent != NULL && pparent != next) {
        // Wake up the parent only if it's not the current process
        wakeup_interruptible(pparent);
    }

    // Note quiescent state for RCU - context switch is a quiescent state.
    // Callback processing is now handled by per-CPU RCU kthreads.
    rcu_check_callbacks();
}
