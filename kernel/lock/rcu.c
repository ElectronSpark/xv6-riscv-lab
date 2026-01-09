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
//   - Timestamp-based RCU: Grace period detection based on context switch timestamps
//   - RCU GP kthread: Background kernel thread for grace period management
//
// GRACE PERIOD DETECTION (Timestamp-based):
//   A grace period completes when all CPUs have context switched after the grace
//   period start timestamp. Each CPU records its last context switch timestamp
//   in mycpu()->rcu_timestamp, which is updated before every context switch.
//
//   Algorithm:
//     1. When a grace period starts, record the current jiffies as gp_start_timestamp
//     2. Poll each CPU's rcu_timestamp to check if it's >= gp_start_timestamp
//     3. When all CPUs have timestamps >= gp_start, the grace period is complete
//     4. Process callbacks that were waiting for this grace period
//
//   Timestamp overflow handling:
//     - Only check overflow risk using current_time from get_jiffs()
//     - When current_time >= RCU_UINT64_MAX/2, normalize ALL stored timestamps
//     - Normalization: subtract RCU_UINT64_MAX/4 from all CPU and GP timestamps
//     - After normalization, direct comparison (t1 > t2) is safe
//     - This is checked periodically by the RCU GP kthread
//
//   The RCU GP kthread periodically:
//     - Checks for timestamp overflow and normalizes if needed
//     - Checks if all CPUs have switched context since GP start
//     - Advances grace periods when all CPUs have newer timestamps
//     - Processes callbacks and wakes waiters
//
// READ-SIDE CRITICAL SECTIONS:
//   rcu_read_lock() and rcu_read_unlock() are very lightweight:
//     - push_off() / pop_off() to prevent preemption during critical section
//     - Increment / decrement per-process nesting counter
//   No per-CPU nesting counters needed - grace period detection relies solely
//   on context switch timestamps, not on tracking nested read locks.
//
// IMPLEMENTATION STRATEGY:
//   - Per-CPU data structures minimize lock contention
//   - Grace periods tracked with timestamps instead of sequence numbers
//   - Callbacks queued per-CPU and invoked after grace period
//   - Context switch in process_switch_to() updates mycpu()->rcu_timestamp
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
#include "timer.h"

// Global RCU state
static rcu_state_t rcu_state;

// Per-CPU RCU data - cache-line aligned to prevent false sharing
rcu_cpu_data_t rcu_cpu_data[NCPU] __attribute__((aligned(RCU_CACHE_LINE_SIZE)));

// Lock protecting grace period state transitions
static spinlock_t rcu_gp_lock;

// Wait queue for processes waiting on grace period completion
static proc_queue_t rcu_gp_waitq;
static spinlock_t rcu_gp_waitq_lock;

// Forward declarations
static void rcu_start_gp(void);
static int rcu_gp_completed(void);
static void rcu_advance_gp(void);
static int rcu_invoke_callbacks(rcu_head_t *list);
static void rcu_advance_cbs(rcu_cpu_data_t *rcp, uint64 gp_timestamp);
static rcu_head_t *rcu_cblist_dequeue(rcu_cpu_data_t *rcp);
static void rcu_cblist_enqueue(rcu_cpu_data_t *rcp, rcu_head_t *head);
static int rcu_cblist_ready_cbs(rcu_cpu_data_t *rcp);
static void rcu_expedited_gp(void);
static void rcu_check_timestamp_overflow(void);

// Configuration constants (Linux-inspired)
#define RCU_LAZY_GP_DELAY   100   // Callbacks to accumulate before starting GP
#define RCU_BATCH_SIZE      16    // Number of callbacks to invoke per batch
#define RCU_GP_KTHREAD_INTERVAL_MS  10  // GP kthread wake interval in ms

// Maximum value for uint64 type (defined locally to avoid stdint.h dependency)
#define RCU_UINT64_MAX      ((uint64)-1)

// Timestamp overflow handling constants:
//   THRESHOLD: When get_jiffs() reaches this value, trigger normalization
//   NORMALIZE_VALUE: Amount to subtract from all timestamps during normalization
// This ensures timestamps stay in a safe range for direct comparison.
#define TIMESTAMP_OVERFLOW_THRESHOLD  (RCU_UINT64_MAX / 2)
#define TIMESTAMP_NORMALIZE_VALUE     (RCU_UINT64_MAX / 4)

// Helper function to compare timestamps
// Returns 1 if t1 > t2, 0 otherwise
// Safe to use direct comparison since rcu_check_timestamp_overflow() ensures
// all stored timestamps are periodically normalized to prevent wraparound.
static inline int safe_timestamp_after(uint64 t1, uint64 t2) {
    return t1 > t2;
}

// ============================================================================
// RCU GP Kthread - Background Grace Period Processing
// ============================================================================

// Forward declaration for use in kthread
static void rcu_wakeup_gp_waiters(void);

// Check and normalize timestamps if needed (called periodically by GP kthread)
//
// Overflow prevention strategy:
//   1. Check current_time from get_jiffs() against THRESHOLD
//   2. If threshold reached, read each stored timestamp into a local variable
//   3. Subtract NORMALIZE_VALUE from local copy and store back
//   4. This keeps all timestamps in a safe range for direct comparison
//
// Note: Only current_time determines if normalization is needed. Individual
// timestamp values are never compared to the threshold - they are simply
// normalized when the global time crosses the threshold.
static void rcu_check_timestamp_overflow(void) {
    uint64 current_time = get_jiffs();
    
    // Only trigger normalization when current time reaches the threshold
    if (current_time >= TIMESTAMP_OVERFLOW_THRESHOLD) {
        // Normalize all CPU timestamps: read into local, subtract, store back
        for (int i = 0; i < NCPU; i++) {
            struct cpu_local *cpu = &cpus[i];
            uint64 cpu_ts = __atomic_load_n(&cpu->rcu_timestamp, __ATOMIC_ACQUIRE);
            if (cpu_ts >= TIMESTAMP_NORMALIZE_VALUE) {
                __atomic_store_n(&cpu->rcu_timestamp, cpu_ts - TIMESTAMP_NORMALIZE_VALUE, __ATOMIC_RELEASE);
            }
        }
        
        // Normalize grace period start timestamp
        uint64 gp_start = __atomic_load_n(&rcu_state.gp_start_timestamp, __ATOMIC_ACQUIRE);
        if (gp_start >= TIMESTAMP_NORMALIZE_VALUE) {
            __atomic_store_n(&rcu_state.gp_start_timestamp, gp_start - TIMESTAMP_NORMALIZE_VALUE, __ATOMIC_RELEASE);
        }
    }
}

// Check if grace period has completed by verifying all CPUs have context switched
// Returns 1 if all CPUs have switched since GP start, 0 otherwise
//
// Algorithm: Compare each CPU's rcu_timestamp against gp_start_timestamp.
// A CPU has passed through a quiescent state if its timestamp >= gp_start.
// Direct comparison is safe because rcu_check_timestamp_overflow() ensures
// all timestamps are normalized before they can wrap around.
static int rcu_gp_completed(void) {
    uint64 gp_start = __atomic_load_n(&rcu_state.gp_start_timestamp, __ATOMIC_ACQUIRE);
    
    // A grace period completes when all CPUs have timestamps >= gp_start
    // This means they have all context switched at or after the GP began
    
    for (int i = 0; i < NCPU; i++) {
        struct cpu_local *cpu = &cpus[i];
        uint64 cpu_timestamp = __atomic_load_n(&cpu->rcu_timestamp, __ATOMIC_ACQUIRE);
        
        // If CPU timestamp is 0, it's uninitialized - skip it
        if (cpu_timestamp == 0) {
            continue;
        }
        
        // If CPU timestamp is less than GP start, this CPU hasn't context switched yet
        if (cpu_timestamp < gp_start) {
            return 0;
        }
    }
    
    return 1; // All CPUs have switched
}

// Force quiescent states - not needed in timestamp-based RCU
// Removed as it's unused in the new implementation

// Per-CPU RCU processing function called from idle loop
// Each CPU handles its own RCU work:
// 1. Timestamp overflow checking (any CPU can do this)
// 2. Starting grace periods if pending callbacks
// 3. Advancing grace period state
// 4. Processing own callbacks
// 5. Waking up synchronize_rcu() waiters
void rcu_idle_enter(void) {
    // Mark that we're in the idle loop - provides RCU quiescent state
    rcu_note_context_switch();
    
    // Check for timestamp overflow and normalize if needed
    // Any CPU can do this - it's atomic and safe
    rcu_check_timestamp_overflow();
    
    // Check if this CPU has pending callbacks that need a grace period
    push_off();
    int cpu = cpuid();
    rcu_cpu_data_t *rcp = &rcu_cpu_data[cpu];
    int has_pending_cbs = __atomic_load_n(&rcp->cb_count, __ATOMIC_ACQUIRE) > 0;
    pop_off();
    
    // If there are pending callbacks, start GP if none in progress
    if (has_pending_cbs) {
        if (!__atomic_load_n(&rcu_state.gp_in_progress, __ATOMIC_ACQUIRE)) {
            rcu_start_gp();
        }
    }
    
    // Try to advance grace period (any CPU can do this)
    rcu_advance_gp();
    
    // Process this CPU's ready callbacks
    rcu_process_callbacks();
    
    // Wake up any processes waiting for grace period completion
    rcu_wakeup_gp_waiters();
}

// Wake up processes waiting in synchronize_rcu()
static void rcu_wakeup_gp_waiters(void) {
    spin_acquire(&rcu_gp_waitq_lock);
    // Wake up all waiters - they will check if their GP has completed
    proc_queue_wakeup_all(&rcu_gp_waitq, 0, 0);
    spin_release(&rcu_gp_waitq_lock);
}

// ============================================================================
// RCU Initialization
// ============================================================================

void rcu_init(void) {
    spin_init(&rcu_gp_lock, "rcu_gp");
    spin_init(&rcu_gp_waitq_lock, "rcu_gp_waitq");
    proc_queue_init(&rcu_gp_waitq, "rcu_gp_waitq", &rcu_gp_waitq_lock);

    __atomic_store_n(&rcu_state.gp_start_timestamp, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rcu_state.gp_seq_completed, 0, __ATOMIC_RELEASE);
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

    // Initialize per-CPU data and timestamps
    for (int i = 0; i < NCPU; i++) {
        rcu_cpu_init(i);
        // Initialize CPU timestamp
        cpus[i].rcu_timestamp = 0;
    }
}

void rcu_cpu_init(int cpu) {
    if (cpu < 0 || cpu >= NCPU) {
        return;
    }

    rcu_cpu_data_t *rcp = &rcu_cpu_data[cpu];

    // Initialize two-list callback structure
    __atomic_store_n(&rcp->pending_head, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&rcp->pending_tail, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&rcp->pending_gp, 0, __ATOMIC_RELEASE);
    
    __atomic_store_n(&rcp->ready_head, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&rcp->ready_tail, NULL, __ATOMIC_RELEASE);

    __atomic_store_n(&rcp->cb_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rcp->qs_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rcp->cb_invoked, 0, __ATOMIC_RELEASE);
}

// ============================================================================
// RCU Read-Side Critical Sections
// ============================================================================
//
// IMPLEMENTATION NOTE - Simplified Per-Process Nesting:
//
// This implementation uses only per-process nesting counters.
// Grace period detection is based on context switch timestamps, not nesting.
//
// rcu_read_lock() and rcu_read_unlock() only do:
//   - push_off() / pop_off() to prevent preemption
//   - increment / decrement process nesting counter
//
// No per-CPU counters are needed since we rely on timestamps.
//

void rcu_read_lock(void) {
    // Disable interrupts to prevent context switches during RCU critical section
    push_off();

    struct proc *p = myproc();
    if (p != NULL) {
        // Per-process nesting
        p->rcu_read_lock_nesting++;
    }
    // If no process context (early boot), just the push_off() is sufficient
}

void rcu_read_unlock(void) {
    struct proc *p = myproc();
    if (p != NULL) {
        // Decrement per-process nesting counter
        p->rcu_read_lock_nesting--;

        if (p->rcu_read_lock_nesting < 0) {
            panic("rcu_read_unlock: unbalanced unlock in process %s (pid %d)", 
                  p->name, p->pid);
        }
    }

    // Re-enable interrupts - matching the push_off() in rcu_read_lock()
    pop_off();
}

int rcu_is_watching(void) {
    struct proc *p = myproc();
    if (p == NULL) {
        // No process context - assume not watching
        return 0;
    }
    return p->rcu_read_lock_nesting > 0;
}

// ============================================================================
// Simple Two-List Callback Management
// ============================================================================
//
// Instead of the complex 4-segment approach, we use a simple two-list design:
// - pending list: callbacks waiting for a grace period to complete
// - ready list: callbacks ready to invoke (their GP completed)
//
// When a GP completes, pending callbacks are moved to the ready list.
// This avoids pointer-into-freed-memory bugs that the segment approach had.
//

// Enqueue a callback to the pending list
static void rcu_cblist_enqueue(rcu_cpu_data_t *rcp, rcu_head_t *head) {
    head->next = NULL;

    rcu_head_t *tail = __atomic_load_n(&rcp->pending_tail, __ATOMIC_ACQUIRE);
    if (tail == NULL) {
        // Empty list
        __atomic_store_n(&rcp->pending_head, head, __ATOMIC_RELEASE);
        __atomic_store_n(&rcp->pending_tail, head, __ATOMIC_RELEASE);
    } else {
        // Append to tail
        tail->next = head;
        __atomic_store_n(&rcp->pending_tail, head, __ATOMIC_RELEASE);
    }
}

// Move pending callbacks to ready list when GP completes
static void rcu_advance_cbs(rcu_cpu_data_t *rcp, uint64 gp_completed) {
    // Check if pending callbacks are waiting for a GP that has now completed
    uint64 pending_gp = __atomic_load_n(&rcp->pending_gp, __ATOMIC_ACQUIRE);
    rcu_head_t *pending_head = __atomic_load_n(&rcp->pending_head, __ATOMIC_ACQUIRE);
    
    if (pending_head == NULL) {
        return;  // No pending callbacks
    }
    
    if (pending_gp > gp_completed) {
        return;  // GP not yet completed for these callbacks
    }
    
    // GP completed - move all pending callbacks to ready list
    rcu_head_t *pending_tail = __atomic_load_n(&rcp->pending_tail, __ATOMIC_ACQUIRE);
    rcu_head_t *ready_tail = __atomic_load_n(&rcp->ready_tail, __ATOMIC_ACQUIRE);
    
    if (ready_tail == NULL) {
        // Ready list is empty - pending becomes ready
        __atomic_store_n(&rcp->ready_head, pending_head, __ATOMIC_RELEASE);
        __atomic_store_n(&rcp->ready_tail, pending_tail, __ATOMIC_RELEASE);
    } else {
        // Append pending to ready
        ready_tail->next = pending_head;
        __atomic_store_n(&rcp->ready_tail, pending_tail, __ATOMIC_RELEASE);
    }
    
    // Clear pending list
    __atomic_store_n(&rcp->pending_head, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&rcp->pending_tail, NULL, __ATOMIC_RELEASE);
}

// Check if there are callbacks ready to invoke
static int rcu_cblist_ready_cbs(rcu_cpu_data_t *rcp) {
    return __atomic_load_n(&rcp->ready_head, __ATOMIC_ACQUIRE) != NULL;
}

// Dequeue all ready callbacks for invocation
static rcu_head_t *rcu_cblist_dequeue(rcu_cpu_data_t *rcp) {
    rcu_head_t *head = __atomic_load_n(&rcp->ready_head, __ATOMIC_ACQUIRE);
    if (head == NULL) {
        return NULL;
    }
    
    // Take the entire ready list
    __atomic_store_n(&rcp->ready_head, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&rcp->ready_tail, NULL, __ATOMIC_RELEASE);
    
    return head;
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

    // Start new grace period with current timestamp
    uint64 now = get_jiffs();
    __atomic_store_n(&rcu_state.gp_start_timestamp, now, __ATOMIC_RELEASE);
    __atomic_store_n(&rcu_state.gp_in_progress, 1, __ATOMIC_RELEASE);

    spin_release(&rcu_gp_lock);
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

    // Grace period complete - update completed counter
    uint64 gp_completed = __atomic_fetch_add(&rcu_state.gp_seq_completed, 1, __ATOMIC_ACQ_REL) + 1;
    (void)gp_completed; // Used by per-CPU callback advancement
    __atomic_store_n(&rcu_state.gp_in_progress, 0, __ATOMIC_RELEASE);
    __atomic_fetch_add(&rcu_state.gp_count, 1, __ATOMIC_RELEASE);

    spin_release(&rcu_gp_lock);

    // Note: Each CPU advances its own callbacks during rcu_process_callbacks()
    // or rcu_note_context_switch(). We don't access other CPUs' data here.
}

// Note that current CPU has passed through a quiescent state
// In timestamp-based RCU, this is called during context switches
void rcu_note_context_switch(void) {
    // Disable preemption to ensure we stay on the same CPU
    push_off();
    
    // Update CPU timestamp to current time
    int cpu = cpuid();
    struct cpu_local *mycpu_ptr = &cpus[cpu];
    uint64 now = get_jiffs();
    __atomic_store_n(&mycpu_ptr->rcu_timestamp, now, __ATOMIC_RELEASE);
    
    // Update statistics
    rcu_cpu_data_t *rcp = &rcu_cpu_data[cpu];
    __atomic_fetch_add(&rcp->qs_count, 1, __ATOMIC_RELEASE);

    // Try to advance grace period
    rcu_advance_gp();
    
    // Advance our own CPU's callbacks (protected by push_off)
    uint64 gp_completed = __atomic_load_n(&rcu_state.gp_seq_completed, __ATOMIC_ACQUIRE);
    rcu_advance_cbs(rcp, gp_completed);
    
    pop_off();
}

// Called by scheduler to check for quiescent states
void rcu_check_callbacks(void) {
    // A context switch is a quiescent state - timestamp is updated elsewhere
    // This function is kept for compatibility but doesn't need to do much
}

// ============================================================================
// RCU Callback Management
// ============================================================================

void call_rcu(rcu_head_t *head, rcu_callback_t func, void *data) {
    if (head == NULL || func == NULL) {
        return;
    }

    // Initialize callback before disabling preemption
    head->next = NULL;
    head->func = func;
    head->data = data;

    // Disable preemption to ensure we stay on the same CPU
    push_off();
    
    int cpu = cpuid();
    rcu_cpu_data_t *rcp = &rcu_cpu_data[cpu];

    // Add to per-CPU pending callback list
    rcu_cblist_enqueue(rcp, head);
    __atomic_fetch_add(&rcp->cb_count, 1, __ATOMIC_RELEASE);
    
    // Set the GP sequence this callback is waiting for
    // Callbacks need to wait for the NEXT grace period to complete
    uint64 current_gp = __atomic_load_n(&rcu_state.gp_seq_completed, __ATOMIC_ACQUIRE);
    uint64 pending_gp = __atomic_load_n(&rcp->pending_gp, __ATOMIC_ACQUIRE);
    if (pending_gp == 0 || pending_gp <= current_gp) {
        // Set to wait for next GP
        __atomic_store_n(&rcp->pending_gp, current_gp + 1, __ATOMIC_RELEASE);
    }

    // Update lazy callback counter
    int lazy_count = __atomic_fetch_add(&rcu_state.lazy_cb_count, 1, __ATOMIC_RELEASE);

    pop_off();

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
// Returns the number of callbacks invoked
static int rcu_invoke_callbacks(rcu_head_t *list) {
    rcu_head_t *cur = list;
    int count = 0;

    while (cur != NULL) {
        // Copy callback info BEFORE invoking, since callback may free the rcu_head
        rcu_head_t *next = cur->next;
        rcu_callback_t func = cur->func;
        void *data = cur->data;

        // Detach this node from the list before invoking callback
        cur->next = NULL;

        // Invoke the callback - after this, cur may be freed
        if (func != NULL) {
            func(data);
            count++;
        }

        cur = next;

        // Batch limit - yield to prevent monopolizing CPU (Linux-inspired)
        if (count > 0 && count % RCU_BATCH_SIZE == 0) {
            yield();
        }
    }

    __atomic_fetch_add(&rcu_state.cb_invoked, count, __ATOMIC_RELEASE);
    return count;
}

// Process completed RCU callbacks
void rcu_process_callbacks(void) {
    // Disable preemption to ensure we stay on the same CPU
    push_off();
    
    int cpu = cpuid();
    rcu_cpu_data_t *rcp = &rcu_cpu_data[cpu];

    // First, advance our callback segments based on completed grace periods
    uint64 gp_completed = __atomic_load_n(&rcu_state.gp_seq_completed, __ATOMIC_ACQUIRE);
    rcu_advance_cbs(rcp, gp_completed);

    // Check if there are ready callbacks using segmented list
    if (!rcu_cblist_ready_cbs(rcp)) {
        pop_off();
        return;
    }

    // Get callbacks to process from DONE segment
    rcu_head_t *done_list = rcu_cblist_dequeue(rcp);
    
    pop_off();

    // Invoke callbacks outside the critical section
    if (done_list != NULL) {
        int count = rcu_invoke_callbacks(done_list);
        __atomic_fetch_sub(&rcp->cb_count, count, __ATOMIC_RELEASE);
        __atomic_fetch_add(&rcp->cb_invoked, 1, __ATOMIC_RELEASE);
    }
}

// ============================================================================
// RCU Synchronization Primitives
// ============================================================================

void synchronize_rcu(void) {
    uint64 start_gp_count = __atomic_load_n(&rcu_state.gp_seq_completed, __ATOMIC_ACQUIRE);

    // Disable lazy GP start for synchronous operations
    int old_lazy = __atomic_exchange_n(&rcu_state.gp_lazy_start, 0, __ATOMIC_ACQ_REL);

    // Start a new grace period immediately
    rcu_start_gp();
    
    // Mark the current CPU as having passed through a quiescent state
    // This is important for single-CPU or low-activity scenarios
    rcu_note_context_switch();

    // Poll mode - wait for grace period to complete
    int max_wait = 100000;
    int wait_count = 0;

    while (wait_count < max_wait) {
        uint64 current_gp = __atomic_load_n(&rcu_state.gp_seq_completed, __ATOMIC_ACQUIRE);

        // Check if a grace period has completed since we started
        if (current_gp > start_gp_count) {
            // Restore lazy GP setting
            __atomic_store_n(&rcu_state.gp_lazy_start, old_lazy, __ATOMIC_RELEASE);
            return;
        }

        // Try to advance grace period ourselves
        rcu_advance_gp();

        // Sleep on the wait queue - idle loops will wake us up
        // The idle loop on each CPU calls rcu_idle_enter() which advances
        // grace periods and wakes waiters
        spin_acquire(&rcu_gp_waitq_lock);
        // Double-check before sleeping
        current_gp = __atomic_load_n(&rcu_state.gp_seq_completed, __ATOMIC_ACQUIRE);
        if (current_gp > start_gp_count) {
            spin_release(&rcu_gp_waitq_lock);
            __atomic_store_n(&rcu_state.gp_lazy_start, old_lazy, __ATOMIC_RELEASE);
            return;
        }
        // Wait on the queue (will release lock while sleeping)
        proc_queue_wait(&rcu_gp_waitq, &rcu_gp_waitq_lock, NULL);
        spin_release(&rcu_gp_waitq_lock);

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
        rcu_cpu_data_t *rcp = &rcu_cpu_data[i];

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
// In timestamp-based RCU, we just wait for all CPUs to context switch
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

    // Record start timestamp
    uint64 exp_start = get_jiffs();
    
    spin_release(&rcu_gp_lock);

    // Wait for all CPUs to context switch (with timeout)
    int max_wait = 10000;
    int wait_count = 0;
    
    while (wait_count < max_wait) {
        int all_switched = 1;
        
        for (int i = 0; i < NCPU; i++) {
            struct cpu_local *cpu = &cpus[i];
            uint64 cpu_timestamp = __atomic_load_n(&cpu->rcu_timestamp, __ATOMIC_ACQUIRE);
            
            // Skip uninitialized timestamps
            if (cpu_timestamp == 0) {
                continue;
            }
            
            // Check if CPU has switched since expedited GP start
            if (cpu_timestamp <= exp_start) {
                all_switched = 0;
                break;
            }
        }
        
        if (all_switched) {
            break;
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
