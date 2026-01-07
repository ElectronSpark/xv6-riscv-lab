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

// RCU GP kthread main function
// This thread handles:
// 1. Periodic grace period advancement
// 2. Callback processing for all CPUs
// 3. Waking up processes waiting in synchronize_rcu()
// 4. Timestamp overflow checking
static int rcu_gp_kthread_fn(uint64 arg1, uint64 arg2) {
    (void)arg1;
    (void)arg2;
    
    __atomic_store_n(&rcu_gp_kthread_running, 1, __ATOMIC_RELEASE);
    
    while (__atomic_load_n(&rcu_gp_kthread_should_run, __ATOMIC_ACQUIRE)) {
        // Check for timestamp overflow and normalize if needed
        rcu_check_timestamp_overflow();
        
        // Check if there are pending callbacks that need a grace period
        int has_pending_cbs = 0;
        for (int i = 0; i < NCPU; i++) {
            if (__atomic_load_n(&rcu_state.cpu_data[i].cb_count, __ATOMIC_ACQUIRE) > 0) {
                has_pending_cbs = 1;
                break;
            }
        }
        
        // If there are pending callbacks, start GP if none in progress
        if (has_pending_cbs) {
            if (!__atomic_load_n(&rcu_state.gp_in_progress, __ATOMIC_ACQUIRE)) {
                rcu_start_gp();
            }
        }
        
        // Try to advance grace period
        rcu_advance_gp();
        
        // Process callbacks on all CPUs
        for (int i = 0; i < NCPU; i++) {
            rcu_cpu_data_t *rcp = &rcu_state.cpu_data[i];
            
            // Advance callback segments based on completed grace period
            spin_acquire(&rcu_cb_lock);
            uint64 gp_completed = __atomic_load_n(&rcu_state.gp_seq_completed, __ATOMIC_ACQUIRE);
            rcu_advance_cbs(rcp, gp_completed);
            
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

    // Initialize RCU GP kthread state
    rcu_gp_kthread = NULL;
    __atomic_store_n(&rcu_gp_kthread_should_run, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rcu_gp_kthread_running, 0, __ATOMIC_RELEASE);

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

    rcu_cpu_data_t *rcp = &rcu_state.cpu_data[cpu];

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

// Advance callback segments based on completed grace period timestamp
static void rcu_advance_cbs(rcu_cpu_data_t *rcp, uint64 gp_completed) {
    if (rcp->cb_head == NULL) {
        return;
    }

    // Move segments forward as grace periods complete
    // WAIT_TAIL -> DONE_TAIL when their GP completes
    if (rcp->gp_seq_needed[RCU_WAIT_TAIL] <= gp_completed) {
        rcp->cb_tail[RCU_DONE_TAIL] = rcp->cb_tail[RCU_WAIT_TAIL];
        rcp->gp_seq_needed[RCU_DONE_TAIL] = rcp->gp_seq_needed[RCU_WAIT_TAIL];
    }

    // NEXT_READY_TAIL -> WAIT_TAIL when GP starts
    if (rcp->gp_seq_needed[RCU_NEXT_READY_TAIL] <= gp_completed) {
        rcp->cb_tail[RCU_WAIT_TAIL] = rcp->cb_tail[RCU_NEXT_READY_TAIL];
        rcp->gp_seq_needed[RCU_WAIT_TAIL] = gp_completed + 1;
    }

    // NEXT_TAIL -> NEXT_READY_TAIL
    rcp->cb_tail[RCU_NEXT_READY_TAIL] = rcp->cb_tail[RCU_NEXT_TAIL];
    rcp->gp_seq_needed[RCU_NEXT_READY_TAIL] = gp_completed + 2;
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
    __atomic_store_n(&rcu_state.gp_in_progress, 0, __ATOMIC_RELEASE);
    __atomic_fetch_add(&rcu_state.gp_count, 1, __ATOMIC_RELEASE);

    spin_release(&rcu_gp_lock);

    // Advance callback segments based on completed grace period
    spin_acquire(&rcu_cb_lock);
    for (int i = 0; i < NCPU; i++) {
        rcu_cpu_data_t *rcp = &rcu_state.cpu_data[i];
        rcu_advance_cbs(rcp, gp_completed);
    }
    spin_release(&rcu_cb_lock);
}

// Note that current CPU has passed through a quiescent state
// In timestamp-based RCU, this is called during context switches
void rcu_note_context_switch(void) {
    // Update CPU timestamp to current time
    int cpu = cpuid();
    struct cpu_local *mycpu_ptr = &cpus[cpu];
    uint64 now = get_jiffs();
    __atomic_store_n(&mycpu_ptr->rcu_timestamp, now, __ATOMIC_RELEASE);
    
    // Update statistics
    rcu_cpu_data_t *rcp = &rcu_state.cpu_data[cpu];
    __atomic_fetch_add(&rcp->qs_count, 1, __ATOMIC_RELEASE);

    // Try to advance grace period
    rcu_advance_gp();
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

        // Sleep on the wait queue if kthread is running, otherwise yield
        int kthread_active = __atomic_load_n(&rcu_gp_kthread_running, __ATOMIC_ACQUIRE);
        if (kthread_active) {
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
        } else {
            // In polling mode, update our timestamp and yield to allow other work
            rcu_note_context_switch();
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
