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
#include "smp/ipi.h"

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

    int this_priority = myproc()->sched_entity->priority;
    int next_priority = se->priority;
    // @TODO: The following code is disabled before implementing EEVDF scheduling.
    // if (PROC_RUNNING(myproc()) 
    //     && this_priority < next_priority
    //     && (!IS_EEVDF_PRIORITY(this_priority)
    //         || !IS_EEVDF_PRIORITY(next_priority))) {
    //     // Current process has higher priority than the picked one
    //     // Do not switch
    //     return myproc();
    // }
    if (PROC_RUNNING(myproc()) && this_priority < next_priority) {
        // Current process has higher priority than the picked one
        // Do not switch
        return myproc();
    }
    
    struct proc *p = se->proc;
    assert(p != NULL, "__sched_pick_next: se->proc is NULL");
    
    // Prepare the task for running - this removes it from the scheduler's
    // internal list but keeps se->rq set, task_count unchanged, and on_rq=1.
    // This matches Linux behavior where on_rq stays 1 while running.
    rq_set_next_task(se);
    
    smp_store_release(&se->on_cpu, 1); // Mark the process as running on CPU
    enum procstate pstate = __proc_get_pstate(p);
    
    // Process can be RUNNING (normal case) or WAKENING (just woken up)
    // If WAKENING, transition to RUNNING now - the process wakes itself
    if (pstate == PSTATE_WAKENING) {
        smp_store_release(&p->state, PSTATE_RUNNING);
    } else if (pstate != PSTATE_RUNNING) {
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
    assert(PROC_RUNNING(proc), "Process state must be RUNNING after yield");
    mycpu()->intena = intena; // Restore interrupt state

    return prev;
}

// Yield the CPU to allow other processes to run.
// lk will not be re-acquired after yielding.
void scheduler_yield(void) {
    // Wake up processes with expired timers.
    // It may add processes to the run queue, so do it before acquiring rq_lock.
    __do_timer_tick();
    
    // Flush the wake list - atomically drain pending wakeups and enqueue them
    rq_flush_wake_list(cpuid());

    int intr = rq_lock_current_irqsave();
    struct proc *proc = myproc();
    struct proc *prev = NULL;

    assert(!CPU_IN_ITR(), "Cannot yield CPU in interrupt context");
    
    // Pick the next process to run
    struct proc *p = __sched_pick_next();
    
    // If our state changed back to RUNNING (woken before sleep completed),
    // and there's no other process to run (or only idle), stay on CPU
    if (p == myproc()) {
        // No other runnable process, just continue running
        // This is normal - process was woken but nothing else to switch to
        rq_unlock_current_irqrestore(intr);
        goto out;
    }
    
    if (!p) {
        if (proc == mycpu()->idle_proc) {
            // Already in idle process, just return
            rq_unlock_current_irqrestore(intr);
            goto out;
        }
        p = mycpu()->idle_proc;
        assert(p != NULL, "Idle process is NULL");
    }

    // prepare to switch
    context_switch_prepare(proc, p);
    
    prev = __switch_to(p);
    
    context_switch_finish(prev, myproc(), intr);

out:
    // The previous peocess may be in the wake list because it's on_cpu was set 
    // the last time we flush the wake list.
    rq_flush_wake_list(cpuid());
    // Note quiescent state for RCU - context switch is a quiescent state.
    // Callback processing is now handled by per-CPU RCU kthreads.
    rcu_check_callbacks();
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

static void __scheduler_wakeup_assertion(struct proc *p) {
    assert(p != NULL, "Cannot wake up a NULL process");
    // Note: pi_lock is now acquired inside __do_scheduler_wakeup, not by callers.
    // This avoids lock convoy when many processes wake the same target.
    assert(!spin_holding(&p->lock), "Process lock must not be held when waking up a process");
    assert(!sched_holding(), "Scheduler lock must not be held when waking up a process");
}

// Internal function to wake up a sleeping or stopped process.
// New pattern: CAS to WAKENING, select RQ, then either:
// - If not on_cpu: enqueue directly and set to RUNNING
// - If on_cpu: add to wake list and send IPI (state stays WAKENING)
// The 'from_stopped' flag indicates whether we're waking from PSTATE_STOPPED.
//
// Locking: Acquires pi_lock first, then rq_lock. Uses trylock with backoff
// to avoid lock convoy when many processes wake the same target.
static void __do_scheduler_wakeup(struct proc *p, bool from_stopped) {
    struct sched_entity *se = p->sched_entity;
    
    // Acquire pi_lock to serialize wakeup attempts on this process
    spin_lock(&se->pi_lock);
    
    // Special case: waking up the current process (p == myproc())
    // This happens when an interrupt wakes up a process that set its state to SLEEPING
    // but hasn't context-switched out yet. In this case, just change the state back
    // to RUNNING - the process will see this when it checks before context switching.
    // Note: Cannot wake self from STOPPED - that's done via scheduler_yield loop.
    if (p == myproc()) {
        smp_rmb();
        if (from_stopped) {
            spin_unlock(&se->pi_lock);
            return; // Cannot continue self
        }
        // The current process is still on CPU, just change state to abort the sleep
        enum procstate old_state = smp_load_acquire(&p->state);
        // Only wake from sleeping states
        if (!PSTATE_IS_SLEEPING(old_state)) {
            spin_unlock(&se->pi_lock);
            return;
        }
        // Set state to RUNNING - no CAS needed since we're the current process
        smp_store_release(&p->state, PSTATE_RUNNING);
        spin_unlock(&se->pi_lock);
        return;
    }
    
    // Check state - if not in expected state, nothing to do
    smp_rmb();
    enum procstate old_state = smp_load_acquire(&p->state);
    if (from_stopped) {
        if (old_state != PSTATE_STOPPED) {
            spin_unlock(&se->pi_lock);
            return;
        }
    } else {
        if (!PSTATE_IS_SLEEPING(old_state)) {
            spin_unlock(&se->pi_lock);
            return;
        }
    }
    
    // Now we hold pi_lock and the process is in a wakeable state.
    // We'll try to acquire rq_lock while holding pi_lock.
    // Use trylock to avoid lock convoy when many processes wake the same target.
    
retry:
    push_off();  // Disable interrupts to avoid CPU migration during rq selection
    struct rq *rq = rq_select_task_rq(se, se->affinity_mask);
    assert(!IS_ERR_OR_NULL(rq), "__do_scheduler_wakeup: rq_select_task_rq failed");
    // origin_cpuid: where the task currently is (to serialize with context_switch_finish)
    // target_cpu: where we want to enqueue the task
    // If cpu_id is -1 (task never ran), just use target_cpu for both.
    int origin_cpuid = smp_load_acquire(&se->cpu_id);
    int target_cpu = rq->cpu_id;
    if (origin_cpuid < 0) {
        origin_cpuid = target_cpu;  // New task, no origin rq to serialize with
    }
    
    // Try to lock both source and target rq to serialize with context_switch_finish.
    // Use trylock to avoid spinning while holding pi_lock (prevents lock convoy).
    if (!rq_trylock_two(origin_cpuid, target_cpu)) {
        pop_off();
        // Couldn't get rq locks - release pi_lock, backoff, re-check state
        spin_unlock(&se->pi_lock);
        for (volatile int i = 0; i < 10; i++) cpu_relax();
        spin_lock(&se->pi_lock);
        // Re-check state after reacquiring pi_lock
        old_state = smp_load_acquire(&p->state);
        if (from_stopped) {
            if (old_state != PSTATE_STOPPED) {
                spin_unlock(&se->pi_lock);
                return;
            }
        } else {
            if (!PSTATE_IS_SLEEPING(old_state)) {
                spin_unlock(&se->pi_lock);
                return;
            }
        }
        goto retry;
    }
    pop_off();
    
    // Re-check cpu_id after acquiring locks. If the task migrated between
    // our initial read and lock acquisition, we locked the wrong rq and
    // must retry. This prevents racing with context_switch_finish on the
    // actual CPU where the task is running.
    int current_cpuid = smp_load_acquire(&se->cpu_id);
    if (current_cpuid >= 0 && current_cpuid != origin_cpuid) {
        rq_unlock_two(origin_cpuid, target_cpu);
        // Release pi_lock, re-acquire and re-check state
        spin_unlock(&se->pi_lock);
        spin_lock(&se->pi_lock);
        old_state = smp_load_acquire(&p->state);
        if (from_stopped) {
            if (old_state != PSTATE_STOPPED) {
                spin_unlock(&se->pi_lock);
                return;
            }
        } else {
            if (!PSTATE_IS_SLEEPING(old_state)) {
                spin_unlock(&se->pi_lock);
                return;
            }
        }
        goto retry;
    }
    
    // We now hold both pi_lock and rq_lock - safe to transition state
    smp_store_release(&p->state, PSTATE_WAKENING);
    
    // Match Linux ttwu() behavior - check on_rq FIRST:
    // 1. If on_rq=1: task is already queued, just change state (ttwu_runnable path)
    // 2. If on_rq=0 and on_cpu=1: use wake_list (task is switching out)
    // 3. If on_rq=0 and on_cpu=0: enqueue directly
    
    // Check on_rq first - if already queued, nothing to do except change state
    if (smp_load_acquire(&se->on_rq)) {
        // Task is on rq - just change state to RUNNING.
        // The task is already in the run queue and will be scheduled.
        // This is true regardless of on_cpu (Linux: ttwu_runnable path).
        smp_store_release(&p->state, PSTATE_RUNNING);
        spin_unlock(&se->pi_lock);
        rq_unlock_two(origin_cpuid, target_cpu);
        return;
    }
    
    // on_rq=0: task is not on the run queue
    // Check if task is still on CPU (in the middle of context_switch)
    if (smp_load_acquire(&se->on_cpu)) {
        // Task is currently running on a CPU but not on_rq (switching out).
        // Use wake_list - the running CPU will enqueue after context_switch_finish.
        // Use origin_cpuid (already validated) rather than re-reading cpu_id.
        rq_add_wake_list(origin_cpuid, se);
        spin_unlock(&se->pi_lock);
        rq_unlock_two(origin_cpuid, target_cpu);
        // Send IPI to origin CPU to process the wake list
        ipi_send_single(origin_cpuid, IPI_REASON_RESCHEDULE);
        return;
    }

    // on_rq=0 and on_cpu=0: task is fully off CPU, enqueue directly
    rq_enqueue_task(rq, se);
    spin_unlock(&se->pi_lock);
    rq_unlock_two(origin_cpuid, target_cpu);
}

// unconditionally wake up a process from the sleep queue.
void scheduler_wakeup(struct proc *p) {
    __scheduler_wakeup_assertion(p);
    if (!PROC_SLEEPING(p)) {
        return; // Process is not sleeping, nothing to do
    }
    __do_scheduler_wakeup(p, false);
}

// Wake up a process sleeping in timer, timer_killable or interruptible state.
void scheduler_wakeup_timeout(struct proc *p) {
    __scheduler_wakeup_assertion(p);
    if (!PROC_TIMER(p)) {
        return; // Process is not in timer, timer_killable or interruptible state, nothing to do
    }
    __do_scheduler_wakeup(p, false);
}

void scheduler_wakeup_killable(struct proc *p) {
    __scheduler_wakeup_assertion(p);
    if (!PROC_KILLABLE(p)) {
        return; // Process is not in killable state, nothing to do
    }
    __do_scheduler_wakeup(p, false);
}

void scheduler_wakeup_interruptible(struct proc *p) {
    __scheduler_wakeup_assertion(p);
    if (!PROC_INTERRUPTIBLE(p)) {
        return; // Process is not in interruptible state, nothing to do
    }
    __do_scheduler_wakeup(p, false);
}

// Wake up a stopped process (continue from PSTATE_STOPPED).
void scheduler_wakeup_stopped(struct proc *p) {
    __scheduler_wakeup_assertion(p);
    if (!PROC_STOPPED(p)) {
        return; // Process is not stopped, nothing to do
    }
    __do_scheduler_wakeup(p, true);
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
// Note: pi_lock is not needed here - rq_lock_two in __do_scheduler_wakeup
// provides serialization for concurrent wakeups to the same target.
void wakeup_proc(struct proc *p)
{
    if (p == NULL) {
        return;
    }
    scheduler_wakeup(p);
}

void wakeup_timeout(struct proc *p) {
    if (p == NULL) {
        return;
    }
    scheduler_wakeup_timeout(p);
}

void wakeup_killable(struct proc *p) {
    if (p == NULL) {
        return;
    }
    scheduler_wakeup_killable(p);
}

// Wake up a process sleeping in interruptible state
// Note: pi_lock is not needed - rq_lock_two in __do_scheduler_wakeup
// handles concurrent wakeups. Removing pi_lock prevents lock convoy
// when many processes exit and wake the same parent.
void wakeup_interruptible(struct proc *p)
{
    if (p == NULL) {
        return;
    }
    scheduler_wakeup_interruptible(p);
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
// Key invariant (matching Linux):
//   - on_rq = 1 while task is running (task is logically "on the run queue")
//   - on_rq = 0 only when task is sleeping (dequeued via rq_dequeue_task)
//   - on_cpu = 1 while task is actively running on a CPU
//   - on_cpu = 0 after context_switch_finish completes
//
// Process sleep routine:
//   1. Process state changes to one of SLEEPING states
//   2. Add to sleep queue (if using channel-based sleep)
//   3. Call scheduler_yield() which:
//      a. context_switch_prepare(): mark next->on_cpu = 1
//      b. __switch_to(): perform actual context switch
//      c. context_switch_finish(): if SLEEPING, dequeue (on_rq=0); clear on_cpu=0
//
// Wakeup path (matching Linux ttwu pattern):
//   1. Read cpu_id to determine which rq to lock (origin CPU)
//   2. Lock both origin and target rq via rq_lock_two()
//   3. Re-check cpu_id: if changed (task migrated), unlock and retry
//   4. Under lock, check on_rq:
//      a. If on_rq=1: just change state to RUNNING (task already on rq)
//      b. If on_rq=0 and on_cpu=1: use wake_list (task switching out)
//      c. If on_rq=0 and on_cpu=0: enqueue directly
//
// The retry loop is critical: if we lock the wrong rq (task migrated between
// reading cpu_id and locking), we'd see stale on_rq/on_cpu and potentially
// set RUNNING without actually enqueuing the task.
//
// IMPORTANT: Parent wakeup for zombie processes is done in __exit_yield()
// BEFORE the zombie calls scheduler_yield(). This matches Linux's pattern
// where do_notify_parent() is called before do_task_dead().

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
    // All operations here are under rq_lock, which serializes with wakeup path.
    // @TODO: try not to treat idle process specially
    if (prev != mycpu()->idle_proc) {
        if (PSTATE_IS_RUNNING(pstate)) {
            // Process is still running, put it back to the scheduler's list.
            // Note: on_rq is still 1 (task was logically on rq while running).
            // Note: If affinity was changed to exclude current CPU, the task
            // stays here until it sleeps. On wakeup, rq_select_task_rq() will
            // place it on an allowed CPU.
            rq_put_prev_task(se);
        } else if (PSTATE_IS_SLEEPING(pstate) || PSTATE_IS_STOPPED(pstate)) {
            // Process entered sleeping/stopped state, properly dequeue it
            // so that wakeup/continue path can enqueue to a (potentially different) rq.
            // This sets on_rq = 0.
            if (se->rq != NULL) {
                rq_dequeue_task(se->rq, se);
            }
        }
        // Note: WAKENING cannot be seen here because wakeup and context_switch_finish
        // are serialized by rq_lock. If wakeup runs first (setting WAKENING/RUNNING),
        // we see RUNNING. If we run first, wakeup blocks on our rq_lock and sees
        // the final state after we release it.

        // If zombie, rq_task_dead was already called in context_switch_prepare
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

    // Note: Parent wakeup for zombie processes is handled in __exit_yield()
    // BEFORE the zombie calls scheduler_yield(). See the comment block above.
}
