// Read-Copy-Update (RCU) synchronization mechanism for xv6
//
// RCU is a synchronization mechanism that allows readers to access shared data
// structures without locks while writers can update them. It's particularly
// efficient for read-mostly workloads.
//
// KEY CONCEPTS:
//   - Read-side critical sections: Protected by rcu_read_lock/unlock, very lightweight
//   - Grace period: Time interval during which all pre-existing readers complete
//   - Quiescent state: Point where a CPU is not in RCU read-side critical section
//   - Callbacks: Functions invoked after a grace period completes
//   - Preemptible RCU: Processes can migrate CPUs while holding RCU read locks
//   - RCU GP kthread: Background kernel thread for grace period management
//
// GRACE PERIOD DETECTION:
//   A grace period completes when all CPUs have passed through a quiescent state.
//   Quiescent states include:
//     - Context switch (even if process holds RCU lock - safe due to per-process tracking)
//     - Idle loop
//     - User mode execution
//     - Explicit rcu_read_unlock() when nesting reaches 0
//   
//   The RCU GP kthread periodically:
//     - Forces quiescent states for idle/offline CPUs
//     - Advances grace periods when all CPUs report quiescent states
//     - Processes callbacks and wakes waiters
//
// NESTING COUNTERS (Hybrid Per-Process/Per-CPU):
//   - Per-process: Each process has rcu_read_lock_nesting counter that follows it
//     across CPU migrations, allowing safe preemption and context switches
//   - Per-CPU: Each CPU tracks total nesting for quiescent state detection
//   - When a process locks RCU: both process counter and current CPU counter increment
//   - When a process unlocks RCU: both counters decrement (even if on different CPU)
//   - No process context: Falls back to per-CPU tracking only
//
// IMPLEMENTATION STRATEGY:
//   - Per-CPU data structures minimize lock contention
//   - Grace periods tracked with sequence numbers
//   - Callbacks queued per-CPU and invoked after grace period
//   - Scheduler integration for quiescent state detection
//   - Background kernel thread for grace period management
//   - Wait queue support for efficient synchronize_rcu()
//

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "spinlock.h"
#include "rcu.h"
#include "proc.h"
#include "proc_queue.h"
#include "sched.h"

// Global RCU state
static rcu_state_t rcu_state;

// Lock protecting grace period state transitions
static spinlock_t rcu_gp_lock;

// RCU GP kthread state
static struct proc *rcu_gp_kthread = NULL;
static _Atomic int rcu_gp_kthread_should_run = 0;
static _Atomic int rcu_gp_kthread_running = 0;

// Wait queue for processes waiting on grace period completion
static proc_queue_t rcu_gp_waitq;
static spinlock_t rcu_gp_waitq_lock;

// Lock protecting callback processing
static spinlock_t rcu_cb_lock;

// Forward declarations
static void rcu_start_gp(void);
static int rcu_gp_completed(void);
static void rcu_advance_gp(void);
static void rcu_invoke_callbacks(rcu_head_t *list);
static void rcu_advance_cbs(rcu_cpu_data_t *rcp, uint64 gp_seq);
static rcu_head_t *rcu_cblist_dequeue(rcu_cpu_data_t *rcp);
static void rcu_cblist_enqueue(rcu_cpu_data_t *rcp, rcu_head_t *head);
static int rcu_cblist_ready_cbs(rcu_cpu_data_t *rcp);
static void rcu_expedited_gp(void);

// Configuration constants (Linux-inspired)
#define RCU_LAZY_GP_DELAY   100   // Callbacks to accumulate before starting GP
#define RCU_BATCH_SIZE      16    // Number of callbacks to invoke per batch
#define RCU_GP_KTHREAD_INTERVAL_MS  10  // GP kthread wake interval in ms

// ============================================================================
// RCU GP Kthread - Background Grace Period Processing
// ============================================================================

// Forward declaration for use in kthread
static void rcu_wakeup_gp_waiters(void);

// Force quiescent states for CPUs that aren't in RCU critical sections
// This is necessary because offline/idle CPUs may never report QS naturally
static void rcu_force_quiescent_states(void) {
    if (!__atomic_load_n(&rcu_state.gp_in_progress, __ATOMIC_ACQUIRE)) {
        return;
    }
    
    for (int i = 0; i < NCPU; i++) {
        rcu_cpu_data_t *rcp = &rcu_state.cpu_data[i];
        
        // If CPU is not in an RCU read-side critical section, force QS
        if (__atomic_load_n(&rcp->nesting, __ATOMIC_ACQUIRE) == 0) {
            // Clear this CPU's bit in the quiescent state mask
            uint64 cpu_mask = 1UL << i;
            __atomic_fetch_and(&rcu_state.qs_mask, ~cpu_mask, __ATOMIC_ACQ_REL);
            __atomic_store_n(&rcp->qs_pending, 0, __ATOMIC_RELEASE);
        }
    }
}

// RCU GP kthread main function
// This thread handles:
// 1. Periodic grace period advancement
// 2. Callback processing for all CPUs
// 3. Waking up processes waiting in synchronize_rcu()
static int rcu_gp_kthread_fn(uint64 arg1, uint64 arg2) {
    (void)arg1;
    (void)arg2;
    
    __atomic_store_n(&rcu_gp_kthread_running, 1, __ATOMIC_RELEASE);
    
    while (__atomic_load_n(&rcu_gp_kthread_should_run, __ATOMIC_ACQUIRE)) {
        // Check if there are pending callbacks that need a grace period
        int has_pending_cbs = 0;
        for (int i = 0; i < NCPU; i++) {
            if (__atomic_load_n(&rcu_state.cpu_data[i].cb_count, __ATOMIC_ACQUIRE) > 0) {
                has_pending_cbs = 1;
                break;
            }
        }
        
        // If there are pending callbacks or lazy threshold reached, start GP
        if (has_pending_cbs) {
            // Start a new grace period if none in progress
            if (!__atomic_load_n(&rcu_state.gp_in_progress, __ATOMIC_ACQUIRE)) {
                rcu_start_gp();
            }
        }
        
        // Force quiescent states for idle/offline CPUs
        rcu_force_quiescent_states();
        
        // Try to advance grace period
        rcu_advance_gp();
        
        // Process callbacks on all CPUs
        for (int i = 0; i < NCPU; i++) {
            rcu_cpu_data_t *rcp = &rcu_state.cpu_data[i];
            
            // Advance callback segments based on completed grace period
            spin_acquire(&rcu_cb_lock);
            uint64 gp_seq_completed = __atomic_load_n(&rcu_state.gp_seq_completed, __ATOMIC_ACQUIRE);
            rcu_advance_cbs(rcp, gp_seq_completed);
            
            // Check if there are ready callbacks
            if (rcu_cblist_ready_cbs(rcp)) {
                rcu_head_t *done_list = rcu_cblist_dequeue(rcp);
                if (done_list != NULL) {
                    // Count callbacks in the list
                    int count = 0;
                    for (rcu_head_t *p = done_list; p != NULL; p = p->next) {
                        count++;
                    }
                    __atomic_fetch_sub(&rcp->cb_count, count, __ATOMIC_RELEASE);
                    spin_release(&rcu_cb_lock);
                    
                    // Invoke callbacks outside the lock
                    rcu_invoke_callbacks(done_list);
                    __atomic_fetch_add(&rcp->cb_invoked, count, __ATOMIC_RELEASE);
                } else {
                    spin_release(&rcu_cb_lock);
                }
            } else {
                spin_release(&rcu_cb_lock);
            }
        }
        
        // Wake up any processes waiting for grace period completion
        rcu_wakeup_gp_waiters();
        
        // Sleep for a short interval before next check
        sleep_ms(RCU_GP_KTHREAD_INTERVAL_MS);
    }
    
    __atomic_store_n(&rcu_gp_kthread_running, 0, __ATOMIC_RELEASE);
    return 0;
}

// Wake up processes waiting in synchronize_rcu()
static void rcu_wakeup_gp_waiters(void) {
    spin_acquire(&rcu_gp_waitq_lock);
    // Wake up all waiters - they will check if their GP has completed
    proc_queue_wakeup_all(&rcu_gp_waitq, 0, 0);
    spin_release(&rcu_gp_waitq_lock);
}

// Start the RCU GP kthread
void rcu_gp_kthread_start(void) {
    if (rcu_gp_kthread != NULL) {
        return; // Already started
    }
    
    __atomic_store_n(&rcu_gp_kthread_should_run, 1, __ATOMIC_RELEASE);
    
    int ret = kernel_proc_create("rcu_gp", &rcu_gp_kthread, 
                                  rcu_gp_kthread_fn, 0, 0, KERNEL_STACK_ORDER);
    if (ret <= 0 || rcu_gp_kthread == NULL) {
        printf("rcu: failed to create RCU GP kthread\n");
        return;
    }
    
    // Wake up the kthread to start it
    wakeup_proc(rcu_gp_kthread);
    
    // Wait for kthread to actually start running
    int wait = 0;
    while (!__atomic_load_n(&rcu_gp_kthread_running, __ATOMIC_ACQUIRE) && wait < 1000) {
        yield();
        wait++;
    }
    
    printf("rcu: RCU GP kthread started (pid %d, running=%d)\n", 
           ret, __atomic_load_n(&rcu_gp_kthread_running, __ATOMIC_ACQUIRE));
}

// ============================================================================
// RCU Initialization
// ============================================================================

void rcu_init(void) {
    spin_init(&rcu_gp_lock, "rcu_gp");
    spin_init(&rcu_cb_lock, "rcu_cb");
    spin_init(&rcu_gp_waitq_lock, "rcu_gp_waitq");
    proc_queue_init(&rcu_gp_waitq, "rcu_gp_waitq", &rcu_gp_waitq_lock);

    __atomic_store_n(&rcu_state.gp_seq, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rcu_state.gp_seq_completed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rcu_state.qs_mask, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rcu_state.gp_in_progress, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rcu_state.gp_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rcu_state.cb_invoked, 0, __ATOMIC_RELEASE);

    // Initialize lazy GP and expedited GP support
    __atomic_store_n(&rcu_state.gp_lazy_start, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&rcu_state.lazy_cb_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rcu_state.expedited_in_progress, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rcu_state.expedited_seq, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rcu_state.expedited_count, 0, __ATOMIC_RELEASE);
    rcu_state.gp_wait_queue = NULL;

    // Initialize RCU GP kthread state
    rcu_gp_kthread = NULL;
    __atomic_store_n(&rcu_gp_kthread_should_run, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rcu_gp_kthread_running, 0, __ATOMIC_RELEASE);

    // Initialize per-CPU data
    for (int i = 0; i < NCPU; i++) {
        rcu_cpu_init(i);
    }
}

void rcu_cpu_init(int cpu) {
    if (cpu < 0 || cpu >= NCPU) {
        return;
    }

    rcu_cpu_data_t *rcp = &rcu_state.cpu_data[cpu];

    __atomic_store_n(&rcp->gp_seq, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rcp->nesting, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rcp->qs_pending, 0, __ATOMIC_RELEASE);

    // Initialize segmented callback list
    rcp->cb_head = NULL;
    for (int i = 0; i < RCU_CBLIST_NSEGS; i++) {
        rcp->cb_tail[i] = NULL;
        rcp->gp_seq_needed[i] = 0;
    }

    __atomic_store_n(&rcp->cb_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rcp->qs_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rcp->cb_invoked, 0, __ATOMIC_RELEASE);
}

// ============================================================================
// RCU Read-Side Critical Sections
// ============================================================================
//
// IMPLEMENTATION NOTE - Hybrid Per-Process/Per-CPU Nesting:
//
// This implementation uses BOTH per-process and per-CPU nesting counters to
// support preemptible RCU (processes can migrate CPUs while holding RCU locks).
//
// Example scenario showing why both counters are needed:
//   1. Process P on CPU 0: rcu_read_lock()  -> P.nesting=1, CPU0.nesting=1
//   2. Process P yields and migrates to CPU 1
//   3. Process P on CPU 1: rcu_read_unlock() -> P.nesting=0, CPU1.nesting=-1 (BUG!)
//
// Solution: Track nesting in BOTH process and CPU:
//   - Process counter: Follows process across CPUs (detects unbalanced locks)
//   - CPU counter: Tracks total locks on this CPU (for quiescent state detection)
//   - Only increment CPU counter on OUTERMOST lock (old_nesting == 0)
//   - Only decrement CPU counter when process nesting reaches 0
//

void rcu_read_lock(void) {
    // Disable interrupts to prevent context switches during RCU critical section
    push_off();

    struct proc *p = myproc();
    if (p == NULL) {
        // No process context (e.g., early boot or scheduler)
        // Fall back to per-CPU tracking only
        int cpu = cpuid();
        rcu_cpu_data_t *rcp = &rcu_state.cpu_data[cpu];
        __atomic_fetch_add(&rcp->nesting, 1, __ATOMIC_ACQUIRE);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (__atomic_load_n(&rcp->nesting, __ATOMIC_ACQUIRE) == 1) {
            __atomic_store_n(&rcp->qs_pending, 1, __ATOMIC_RELEASE);
        }
        return;
    }

    // Per-process nesting (allows migration across CPUs)
    int old_nesting = p->rcu_read_lock_nesting++;

    // Compiler barrier to prevent code motion
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    // If this is the outermost lock, increment CPU counter
    // This ensures CPU quiescent state tracking works correctly even if
    // the process migrates to another CPU before unlocking
    if (old_nesting == 0) {
        int cpu = cpuid();
        rcu_cpu_data_t *rcp = &rcu_state.cpu_data[cpu];
        __atomic_store_n(&rcp->qs_pending, 1, __ATOMIC_RELEASE);
        __atomic_fetch_add(&rcp->nesting, 1, __ATOMIC_ACQUIRE);
    }
}

void rcu_read_unlock(void) {
    struct proc *p = myproc();
    if (p == NULL) {
        // No process context (e.g., early boot or scheduler)
        // Fall back to per-CPU tracking only
        int cpu = cpuid();
        rcu_cpu_data_t *rcp = &rcu_state.cpu_data[cpu];
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        int new_nesting = __atomic_sub_fetch(&rcp->nesting, 1, __ATOMIC_RELEASE);
        if (new_nesting < 0) {
            panic("rcu_read_unlock: unbalanced unlock on CPU %d", cpu);
        }
        if (new_nesting == 0 && __atomic_load_n(&rcp->qs_pending, __ATOMIC_ACQUIRE)) {
            rcu_note_context_switch();
        }
        // Re-enable interrupts - matching the push_off() in rcu_read_lock()
        pop_off();
        return;
    }

    // Compiler barrier before decrementing
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    // Decrement per-process nesting counter
    p->rcu_read_lock_nesting--;

    if (p->rcu_read_lock_nesting < 0) {
        panic("rcu_read_unlock: unbalanced unlock in process %s (pid %d)", p->name, p->pid);
    }

    // If nesting reaches 0, we've exited all critical sections
    // Decrement the CPU counter (possibly on a different CPU than where we locked)
    if (p->rcu_read_lock_nesting == 0) {
        int cpu = cpuid();
        rcu_cpu_data_t *rcp = &rcu_state.cpu_data[cpu];

        // Decrement per-CPU nesting counter
        int cpu_nesting = __atomic_sub_fetch(&rcp->nesting, 1, __ATOMIC_RELEASE);

        // Check if we need to report quiescent state
        if (cpu_nesting == 0 && __atomic_load_n(&rcp->qs_pending, __ATOMIC_ACQUIRE)) {
            rcu_note_context_switch();
        }
    }

    // Re-enable interrupts - matching the push_off() in rcu_read_lock()
    pop_off();
}

int rcu_is_watching(void) {
    struct proc *p = myproc();
    if (p == NULL) {
        // No process context - check per-CPU
        int cpu = cpuid();
        rcu_cpu_data_t *rcp = &rcu_state.cpu_data[cpu];
        return __atomic_load_n(&rcp->nesting, __ATOMIC_ACQUIRE) > 0;
    }
    return p->rcu_read_lock_nesting > 0;
}

// ============================================================================
// Segmented Callback List Management (Linux-inspired)
// ============================================================================

// Enqueue a callback to the per-CPU callback list
static void rcu_cblist_enqueue(rcu_cpu_data_t *rcp, rcu_head_t *head) {
    head->next = NULL;

    if (rcp->cb_head == NULL) {
        // Empty list - initialize all segments to point to this callback
        rcp->cb_head = head;
        for (int i = 0; i < RCU_CBLIST_NSEGS; i++) {
            rcp->cb_tail[i] = &head->next;
        }
    } else {
        // Add to NEXT_TAIL segment (newest callbacks)
        *rcp->cb_tail[RCU_NEXT_TAIL] = head;
        rcp->cb_tail[RCU_NEXT_TAIL] = &head->next;
    }
}

// Dequeue callbacks that are ready to invoke
static rcu_head_t *rcu_cblist_dequeue(rcu_cpu_data_t *rcp) {
    // Extract callbacks from DONE_TAIL segment
    rcu_head_t *head = rcp->cb_head;

    if (head == NULL || rcp->cb_tail[RCU_DONE_TAIL] == (rcu_head_t **)&(rcp->cb_head)) {
        // No callbacks ready
        return NULL;
    }

    // Find the end of the DONE segment
    rcu_head_t **done_tail = rcp->cb_tail[RCU_DONE_TAIL];
    rcu_head_t *done_list = head;

    // Update head to point to remaining callbacks
    rcp->cb_head = *done_tail;

    // Terminate the done list
    *done_tail = NULL;

    // Shift segment pointers
    for (int i = RCU_DONE_TAIL; i < RCU_CBLIST_NSEGS - 1; i++) {
        rcp->cb_tail[i] = rcp->cb_tail[i + 1];
    }

    if (rcp->cb_head == NULL) {
        // List is now empty
        for (int i = 0; i < RCU_CBLIST_NSEGS; i++) {
            rcp->cb_tail[i] = NULL;
        }
    } else {
        rcp->cb_tail[RCU_NEXT_TAIL] = done_tail;
    }

    return done_list;
}

// Check if there are callbacks ready to invoke
static int rcu_cblist_ready_cbs(rcu_cpu_data_t *rcp) {
    if (rcp->cb_head == NULL) {
        return 0;
    }
    return rcp->cb_tail[RCU_DONE_TAIL] != (rcu_head_t **)&(rcp->cb_head);
}

// Advance callback segments based on completed grace period
static void rcu_advance_cbs(rcu_cpu_data_t *rcp, uint64 gp_seq) {
    if (rcp->cb_head == NULL) {
        return;
    }

    // Move segments forward as grace periods complete
    // WAIT_TAIL -> DONE_TAIL when their GP completes
    if (rcp->gp_seq_needed[RCU_WAIT_TAIL] <= gp_seq) {
        rcp->cb_tail[RCU_DONE_TAIL] = rcp->cb_tail[RCU_WAIT_TAIL];
        rcp->gp_seq_needed[RCU_DONE_TAIL] = rcp->gp_seq_needed[RCU_WAIT_TAIL];
    }

    // NEXT_READY_TAIL -> WAIT_TAIL when GP starts
    if (rcp->gp_seq_needed[RCU_NEXT_READY_TAIL] <= gp_seq) {
        rcp->cb_tail[RCU_WAIT_TAIL] = rcp->cb_tail[RCU_NEXT_READY_TAIL];
        rcp->gp_seq_needed[RCU_WAIT_TAIL] = gp_seq + 1;
    }

    // NEXT_TAIL -> NEXT_READY_TAIL
    rcp->cb_tail[RCU_NEXT_READY_TAIL] = rcp->cb_tail[RCU_NEXT_TAIL];
    rcp->gp_seq_needed[RCU_NEXT_READY_TAIL] = gp_seq + 2;
}

// ============================================================================
// Grace Period Management
// ============================================================================

// Start a new grace period
static void rcu_start_gp(void) {
    spin_acquire(&rcu_gp_lock);

    // Check if a grace period is already in progress
    if (__atomic_load_n(&rcu_state.gp_in_progress, __ATOMIC_ACQUIRE)) {
        spin_release(&rcu_gp_lock);
        return;
    }

    // Start new grace period
    uint64 new_gp_seq = __atomic_load_n(&rcu_state.gp_seq, __ATOMIC_ACQUIRE) + 1;
    __atomic_store_n(&rcu_state.gp_seq, new_gp_seq, __ATOMIC_RELEASE);
    __atomic_store_n(&rcu_state.gp_in_progress, 1, __ATOMIC_RELEASE);

    // Set quiescent state mask - need all CPUs to report
    uint64 qs_mask = 0;
    for (int i = 0; i < NCPU; i++) {
        qs_mask |= (1UL << i);
        __atomic_store_n(&rcu_state.cpu_data[i].qs_pending, 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&rcu_state.qs_mask, qs_mask, __ATOMIC_RELEASE);

    spin_release(&rcu_gp_lock);
}

// Check if current grace period has completed
static int rcu_gp_completed(void) {
    // Grace period completes when all CPUs have reported quiescent state
    uint64 qs_mask = __atomic_load_n(&rcu_state.qs_mask, __ATOMIC_ACQUIRE);
    return qs_mask == 0;
}

// Advance to next grace period if current one is complete
static void rcu_advance_gp(void) {
    if (!__atomic_load_n(&rcu_state.gp_in_progress, __ATOMIC_ACQUIRE)) {
        return;
    }

    if (!rcu_gp_completed()) {
        return;
    }

    spin_acquire(&rcu_gp_lock);

    // Double-check under lock
    if (!__atomic_load_n(&rcu_state.gp_in_progress, __ATOMIC_ACQUIRE) ||
        !rcu_gp_completed()) {
        spin_release(&rcu_gp_lock);
        return;
    }

    // Grace period complete - update completed sequence number
    uint64 gp_seq = __atomic_load_n(&rcu_state.gp_seq, __ATOMIC_ACQUIRE);
    __atomic_store_n(&rcu_state.gp_seq_completed, gp_seq, __ATOMIC_RELEASE);
    __atomic_store_n(&rcu_state.gp_in_progress, 0, __ATOMIC_RELEASE);
    __atomic_fetch_add(&rcu_state.gp_count, 1, __ATOMIC_RELEASE);

    spin_release(&rcu_gp_lock);

    // Advance callback segments based on completed grace period
    spin_acquire(&rcu_cb_lock);
    for (int i = 0; i < NCPU; i++) {
        rcu_cpu_data_t *rcp = &rcu_state.cpu_data[i];
        rcu_advance_cbs(rcp, gp_seq);
    }
    spin_release(&rcu_cb_lock);

    // Wake up any processes waiting in synchronize_rcu()
    // Note: In xv6, we don't have a proper wait queue, so they'll wake on next yield
}

// Note that current CPU has passed through a quiescent state
void rcu_note_context_switch(void) {
    int cpu = cpuid();
    rcu_cpu_data_t *rcp = &rcu_state.cpu_data[cpu];

    // Don't report if not in critical section
    if (__atomic_load_n(&rcp->nesting, __ATOMIC_ACQUIRE) > 0) {
        return;
    }

    // Check if we need to report quiescent state
    if (!__atomic_load_n(&rcp->qs_pending, __ATOMIC_ACQUIRE)) {
        return;
    }

    // Clear pending flag
    __atomic_store_n(&rcp->qs_pending, 0, __ATOMIC_RELEASE);

    // Update CPU's grace period sequence
    uint64 gp_seq = __atomic_load_n(&rcu_state.gp_seq, __ATOMIC_ACQUIRE);
    __atomic_store_n(&rcp->gp_seq, gp_seq, __ATOMIC_RELEASE);
    __atomic_fetch_add(&rcp->qs_count, 1, __ATOMIC_RELEASE);

    // Clear this CPU's bit in the quiescent state mask
    uint64 cpu_mask = 1UL << cpu;
    __atomic_fetch_and(&rcu_state.qs_mask, ~cpu_mask, __ATOMIC_ACQ_REL);

    // Try to advance grace period
    rcu_advance_gp();
}

// Called by scheduler to check for quiescent states
void rcu_check_callbacks(void) {
    // A context switch is a quiescent state
    rcu_note_context_switch();
}

// ============================================================================
// RCU Callback Management
// ============================================================================

void call_rcu(rcu_head_t *head, rcu_callback_t func, void *data) {
    if (head == NULL || func == NULL) {
        return;
    }

    int cpu = cpuid();
    rcu_cpu_data_t *rcp = &rcu_state.cpu_data[cpu];

    // Initialize callback
    head->next = NULL;
    head->func = func;
    head->data = data;

    // Add to per-CPU segmented callback list
    spin_acquire(&rcu_cb_lock);

    rcu_cblist_enqueue(rcp, head);
    __atomic_fetch_add(&rcp->cb_count, 1, __ATOMIC_RELEASE);

    // Update lazy callback counter
    int lazy_count = __atomic_fetch_add(&rcu_state.lazy_cb_count, 1, __ATOMIC_RELEASE);

    spin_release(&rcu_cb_lock);

    // Start a grace period based on lazy threshold (Linux-inspired batching)
    // This reduces overhead by accumulating callbacks before starting GP
    if (__atomic_load_n(&rcu_state.gp_lazy_start, __ATOMIC_ACQUIRE)) {
        if (lazy_count >= RCU_LAZY_GP_DELAY) {
            __atomic_store_n(&rcu_state.lazy_cb_count, 0, __ATOMIC_RELEASE);
            rcu_start_gp();
        }
    } else {
        // Non-lazy mode - start GP immediately
        rcu_start_gp();
    }
}

// Invoke callbacks that have completed their grace period
// Uses batching to limit the number of callbacks invoked per call (Linux-inspired)
static void rcu_invoke_callbacks(rcu_head_t *list) {
    rcu_head_t *cur = list;
    uint64 count = 0;

    while (cur != NULL) {
        rcu_head_t *next = cur->next;

        // Invoke the callback
        if (cur->func != NULL) {
            cur->func(cur->data);
            count++;
        }

        cur = next;

        // Batch limit - yield to prevent monopolizing CPU (Linux-inspired)
        if (count > 0 && count % RCU_BATCH_SIZE == 0) {
            yield();
        }
    }

    __atomic_fetch_add(&rcu_state.cb_invoked, count, __ATOMIC_RELEASE);
}

// Process completed RCU callbacks
void rcu_process_callbacks(void) {
    int cpu = cpuid();
    rcu_cpu_data_t *rcp = &rcu_state.cpu_data[cpu];

    // Check if there are ready callbacks using segmented list
    if (!rcu_cblist_ready_cbs(rcp)) {
        return;
    }

    // Get callbacks to process from DONE segment
    spin_acquire(&rcu_cb_lock);
    rcu_head_t *done_list = rcu_cblist_dequeue(rcp);

    // Update callback count
    if (done_list != NULL) {
        // Count callbacks in the list
        int count = 0;
        for (rcu_head_t *p = done_list; p != NULL; p = p->next) {
            count++;
        }
        __atomic_fetch_sub(&rcp->cb_count, count, __ATOMIC_RELEASE);
    }

    spin_release(&rcu_cb_lock);

    // Invoke callbacks outside the lock
    if (done_list != NULL) {
        rcu_invoke_callbacks(done_list);
        __atomic_fetch_add(&rcp->cb_invoked, 1, __ATOMIC_RELEASE);
    }
}

// ============================================================================
// RCU Synchronization Primitives
// ============================================================================

void synchronize_rcu(void) {
    uint64 start_gp = __atomic_load_n(&rcu_state.gp_seq_completed, __ATOMIC_ACQUIRE);

    // Disable lazy GP start for synchronous operations
    int old_lazy = __atomic_exchange_n(&rcu_state.gp_lazy_start, 0, __ATOMIC_ACQ_REL);

    // Start a new grace period immediately
    rcu_start_gp();

    // If the RCU GP kthread is running, we can sleep and be woken up
    // Otherwise, fall back to polling mode (always use polling for now - simpler)
    // int kthread_active = __atomic_load_n(&rcu_gp_kthread_running, __ATOMIC_ACQUIRE);
    int kthread_active = 0; // Force polling mode for debugging
    
    int max_wait = 100000;
    int wait_count = 0;

    while (wait_count < max_wait) {
        uint64 current_gp = __atomic_load_n(&rcu_state.gp_seq_completed, __ATOMIC_ACQUIRE);

        // Check if a grace period has completed since we started
        if (current_gp > start_gp) {
            // Restore lazy GP setting
            __atomic_store_n(&rcu_state.gp_lazy_start, old_lazy, __ATOMIC_RELEASE);
            return;
        }

        // Force quiescent states for idle CPUs
        rcu_force_quiescent_states();
        
        // Try to advance grace period ourselves
        rcu_advance_gp();

        if (kthread_active) {
            // Sleep on the wait queue - kthread will wake us
            spin_acquire(&rcu_gp_waitq_lock);
            // Double-check before sleeping
            current_gp = __atomic_load_n(&rcu_state.gp_seq_completed, __ATOMIC_ACQUIRE);
            if (current_gp > start_gp) {
                spin_release(&rcu_gp_waitq_lock);
                __atomic_store_n(&rcu_state.gp_lazy_start, old_lazy, __ATOMIC_RELEASE);
                return;
            }
            // Wait on the queue (will release lock while sleeping)
            proc_queue_wait(&rcu_gp_waitq, &rcu_gp_waitq_lock, NULL);
            spin_release(&rcu_gp_waitq_lock);
        } else {
            // Fallback: polling mode when kthread not running
            rcu_force_quiescent_states();
            yield();
        }

        wait_count++;
    }

    // Restore lazy GP setting
    __atomic_store_n(&rcu_state.gp_lazy_start, old_lazy, __ATOMIC_RELEASE);

    // If we got here, something is wrong
    printf("synchronize_rcu: WARNING - grace period did not complete after %d iterations\n",
           max_wait);
}

void rcu_barrier(void) {
    // Wait for all pending callbacks to complete
    // First, ensure current grace period completes
    synchronize_rcu();

    // Process any remaining callbacks
    for (int i = 0; i < NCPU; i++) {
        rcu_cpu_data_t *rcp = &rcu_state.cpu_data[i];

        // Wait for callback count to reach zero
        int max_wait = 100000;
        int wait_count = 0;

        while (__atomic_load_n(&rcp->cb_count, __ATOMIC_ACQUIRE) > 0 &&
               wait_count < max_wait) {
            rcu_process_callbacks();
            yield();
            wait_count++;
        }
    }

    // One more grace period to ensure all callbacks are invoked
    synchronize_rcu();
}

// ============================================================================
// Expedited Grace Period (Linux-inspired)
// ============================================================================

// Expedited grace period - forces immediate quiescent states on all CPUs
// This is faster than normal GP but has higher overhead (Linux-inspired)
static void rcu_expedited_gp(void) {
    spin_acquire(&rcu_gp_lock);

    // Check if expedited GP already in progress
    if (__atomic_load_n(&rcu_state.expedited_in_progress, __ATOMIC_ACQUIRE)) {
        spin_release(&rcu_gp_lock);
        return;
    }

    // Mark expedited GP in progress
    __atomic_store_n(&rcu_state.expedited_in_progress, 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&rcu_state.expedited_seq, 1, __ATOMIC_ACQ_REL);

    // Set quiescent state mask for expedited GP
    uint64 qs_mask = 0;
    for (int i = 0; i < NCPU; i++) {
        qs_mask |= (1UL << i);
    }
    __atomic_store_n(&rcu_state.qs_mask, qs_mask, __ATOMIC_RELEASE);

    spin_release(&rcu_gp_lock);

    // Force quiescent states on all CPUs by sending IPIs (simulated)
    // In a real system, this would send IPIs to all CPUs
    // For xv6, we aggressively call rcu_check_callbacks on all CPUs
    for (int i = 0; i < NCPU; i++) {
        // This simulates forcing a context switch / quiescent state check
        rcu_cpu_data_t *rcp = &rcu_state.cpu_data[i];
        if (__atomic_load_n(&rcp->nesting, __ATOMIC_ACQUIRE) == 0) {
            // CPU not in RCU read-side critical section - report QS
            uint64 cpu_mask = 1UL << i;
            __atomic_fetch_and(&rcu_state.qs_mask, ~cpu_mask, __ATOMIC_ACQ_REL);
        }
    }

    // Wait for all CPUs to report quiescent states (with timeout)
    int max_wait = 10000;
    int wait_count = 0;
    while (__atomic_load_n(&rcu_state.qs_mask, __ATOMIC_ACQUIRE) != 0 &&
           wait_count < max_wait) {
        // Try to advance
        for (int i = 0; i < NCPU; i++) {
            rcu_cpu_data_t *rcp = &rcu_state.cpu_data[i];
            if (__atomic_load_n(&rcp->nesting, __ATOMIC_ACQUIRE) == 0) {
                uint64 cpu_mask = 1UL << i;
                __atomic_fetch_and(&rcu_state.qs_mask, ~cpu_mask, __ATOMIC_ACQ_REL);
            }
        }
        yield();
        wait_count++;
    }

    // Complete expedited GP
    spin_acquire(&rcu_gp_lock);
    __atomic_store_n(&rcu_state.expedited_in_progress, 0, __ATOMIC_RELEASE);
    __atomic_fetch_add(&rcu_state.expedited_count, 1, __ATOMIC_RELEASE);
    spin_release(&rcu_gp_lock);
}

void synchronize_rcu_expedited(void) {
    uint64 start_exp = __atomic_load_n(&rcu_state.expedited_seq, __ATOMIC_ACQUIRE);

    // Disable lazy GP start
    int old_lazy = __atomic_exchange_n(&rcu_state.gp_lazy_start, 0, __ATOMIC_ACQ_REL);

    // Run expedited grace period
    rcu_expedited_gp();

    // Start and wait for normal GP to complete (handles callback advancement)
    rcu_start_gp();

    int max_wait = 50000;
    int wait_count = 0;

    while (wait_count < max_wait) {
        uint64 current_exp = __atomic_load_n(&rcu_state.expedited_seq, __ATOMIC_ACQUIRE);

        if (current_exp > start_exp) {
            // Expedited GP completed
            __atomic_store_n(&rcu_state.gp_lazy_start, old_lazy, __ATOMIC_RELEASE);
            return;
        }

        rcu_advance_gp();
        yield();
        wait_count++;
    }

    // Restore lazy GP setting
    __atomic_store_n(&rcu_state.gp_lazy_start, old_lazy, __ATOMIC_RELEASE);

    printf("synchronize_rcu_expedited: WARNING - expedited GP did not complete\n");
}
