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
//   - Per-CPU RCU kthreads: Background kernel threads for callback processing
//
// GRACE PERIOD DETECTION (Timestamp-based):
//   A grace period completes when all CPUs have context switched after the grace
//   period start timestamp. Each CPU records its last context switch timestamp
//   in mycpu()->rcu_timestamp, which is updated on every context switch.
//
//   Algorithm:
//     1. When call_rcu() is called, callback records timestamp = get_jiffs()
//     2. Each CPU updates its rcu_timestamp on context switch
//     3. Callback ready when: callback.timestamp <= min(other CPUs' rcu_timestamp)
//     4. Ready callbacks are invoked by per-CPU kthreads
//
//   The per-CPU RCU kthreads periodically:
//     - Check which callbacks are ready based on timestamps
//     - Invoke ready callbacks
//     - Wake synchronize_rcu() waiters
//
// READ-SIDE CRITICAL SECTIONS:
//   rcu_read_lock() and rcu_read_unlock() are very lightweight:
//     - push_off() / pop_off() to prevent preemption during critical section
//     - Increment / decrement per-process nesting counter
//   No per-CPU nesting counters needed - grace period detection relies solely
//   on context switch timestamps, not on tracking nested read locks.
//
// PER-CPU CALLBACK LIST SYNCHRONIZATION:
//   Both call_rcu() and the kthread access the same CPU's callback list.
//   To prevent races, both use push_off()/pop_off() during list manipulation.
//   Since push_off()/pop_off() are re-entrant, this is safe.
//
// IMPLEMENTATION STRATEGY:
//   - Per-CPU data structures minimize lock contention
//   - Callbacks queued per-CPU and invoked after grace period
//   - Context switch updates mycpu()->rcu_timestamp
//   - Per-CPU kernel threads for callback processing
//   - Wait queue support for efficient synchronize_rcu()
//

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "lock/spinlock.h"
#include "lock/rcu.h"
#include "proc/proc.h"
#include "proc/proc_queue.h"
#include "proc/sched.h"
#include "proc/rq.h"
#include "timer/timer.h"
#include "mm/slab.h"

// Slab cache for rcu_head_t structures
static slab_cache_t rcu_head_slab = {0};

// Global RCU state
static rcu_state_t rcu_state;

// Per-CPU RCU data - cache-line aligned to prevent false sharing
rcu_cpu_data_t rcu_cpu_data[NCPU] __ALIGNED_CACHELINE;

// Lock protecting grace period state transitions
static spinlock_t rcu_gp_lock = SPINLOCK_INITIALIZED("rcu_gp_lock");

// Wait queue for processes waiting on grace period completion
static proc_queue_t rcu_gp_waitq;
static spinlock_t rcu_gp_waitq_lock = SPINLOCK_INITIALIZED("rcu_gp_waitq_lock");

// Per-CPU RCU kthread state (forward declaration for rcu_barrier)
static struct {
    struct proc *proc;           // The kthread process
    volatile int wakeup_pending; // Flag to signal wakeup
} rcu_kthread[NCPU];

// Flag indicating if RCU kthreads have been started
static volatile int rcu_kthreads_started = 0;

// Forward declarations
static void rcu_start_gp(void);
static int rcu_gp_completed(void);
static void rcu_advance_gp(void);
static int rcu_invoke_callbacks(rcu_head_t *list);
static void rcu_cblist_enqueue(rcu_cpu_data_t *rcp, rcu_head_t *head);
static void rcu_expedited_gp(void);
static void rcu_check_timestamp_overflow(void);

// Configuration constants (Linux-inspired)
#define RCU_LAZY_GP_DELAY   100   // Callbacks to accumulate before starting GP

// Maximum value for uint64 type (defined locally to avoid stdint.h dependency)
#define RCU_UINT64_MAX      ((uint64)-1)

// Note on timestamp overflow:
// With a 64-bit timestamp counter incrementing at 10MHz (100ns per tick),
// overflow would take ~58,000 years. At 1GHz it would take ~584 years.
// Therefore, timestamp normalization is not needed and has been removed
// to avoid the complexity and race conditions it would introduce
// (particularly with callback timestamps that are harder to normalize safely).

// Calculate the minimum rcu_timestamp among all CPUs OTHER than exclude_cpu.
// Returns RCU_UINT64_MAX if no other CPUs are initialized (timestamp != 0).
// This is used to determine which callbacks are safe to invoke - a callback
// is ready when its registration timestamp is less than this minimum,
// meaning all other CPUs have context-switched after it was registered.
//
// Special case: If no other CPUs have initialized timestamps (single-CPU system
// or early boot), returns RCU_UINT64_MAX. This means all callbacks are considered
// ready, which is correct because there are no other CPUs that could be in
// RCU read-side critical sections.
static uint64 rcu_get_min_other_cpu_timestamp(int exclude_cpu) {
    uint64 min_ts = RCU_UINT64_MAX;
    for (int i = 0; i < NCPU; i++) {
        if (i == exclude_cpu) continue;  // Skip the excluded CPU
        struct cpu_local *cpu = &cpus[i];
        uint64 cpu_ts = __atomic_load_n(&cpu->rcu_timestamp, __ATOMIC_ACQUIRE);
        if (cpu_ts == 0) continue;  // Skip uninitialized CPUs
        if (cpu_ts < min_ts) {
            min_ts = cpu_ts;
        }
    }
    // If no other CPUs have initialized timestamps, min_ts remains RCU_UINT64_MAX,
    // which means all callbacks are ready (no other CPUs to wait for)
    return min_ts;
}

// ============================================================================
// RCU GP Kthread - Background Grace Period Processing
// ============================================================================

// Forward declaration for use in kthread
static void rcu_wakeup_gp_waiters(void);

// Timestamp overflow check - no longer needed
//
// With 64-bit timestamps, overflow is not a practical concern:
// - At 10MHz (100ns ticks): overflow in ~58,000 years
// - At 1GHz (1ns ticks): overflow in ~584 years
//
// The previous normalization approach had bugs:
// 1. Callback timestamps in rcu_head_t were never normalized
// 2. Race conditions when normalizing while other CPUs context-switch
//
// This function is kept as a no-op for compatibility but does nothing.
static void rcu_check_timestamp_overflow(void) {
    // No-op: timestamp overflow is not a practical concern with uint64
}

// Check if grace period has completed by verifying all CPUs have context switched
// Returns 1 if all CPUs have switched since GP start, 0 otherwise
//
// Algorithm: Compare each CPU's rcu_timestamp against gp_start_timestamp.
// A CPU has passed through a quiescent state if its timestamp >= gp_start.
static int rcu_gp_completed(void) {
    uint64 gp_start = __atomic_load_n(&rcu_state.gp_start_timestamp, __ATOMIC_ACQUIRE);
    
    // If no grace period has been started yet (gp_start == 0), it cannot be complete
    if (gp_start == 0) {
        return 0;
    }
    
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

// Wake up processes waiting in synchronize_rcu()
static void rcu_wakeup_gp_waiters(void) {
    spin_lock(&rcu_gp_waitq_lock);
    // Wake up all waiters - they will check if their GP has completed
    proc_queue_wakeup_all(&rcu_gp_waitq, 0, 0);
    spin_unlock(&rcu_gp_waitq_lock);
}

// ============================================================================
// RCU Initialization
// ============================================================================

void rcu_init(void) {
    proc_queue_init(&rcu_gp_waitq, "rcu_gp_waitq", &rcu_gp_waitq_lock);

    int ret = slab_cache_init(&rcu_head_slab, "rcu_head_cache",
                    sizeof(rcu_head_t),
                    SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
    assert(ret == 0,
           "Failed to initialize rcu_head_cache slab cache, errno=%d", ret);

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

    // Initialize pending callback list
    __atomic_store_n(&rcp->pending_head, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&rcp->pending_tail, NULL, __ATOMIC_RELEASE);

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


// ============================================================================
// Grace Period Management
// ============================================================================

// Start a new grace period
static void rcu_start_gp(void) {
    spin_lock(&rcu_gp_lock);

    // Check if a grace period is already in progress
    if (__atomic_load_n(&rcu_state.gp_in_progress, __ATOMIC_ACQUIRE)) {
        spin_unlock(&rcu_gp_lock);
        return;
    }

    // Start new grace period with current timestamp
    uint64 now = r_time();
    __atomic_store_n(&rcu_state.gp_start_timestamp, now, __ATOMIC_RELEASE);
    __atomic_store_n(&rcu_state.gp_in_progress, 1, __ATOMIC_RELEASE);

    spin_unlock(&rcu_gp_lock);
}

// Advance to next grace period if current one is complete
static void rcu_advance_gp(void) {
    if (!__atomic_load_n(&rcu_state.gp_in_progress, __ATOMIC_ACQUIRE)) {
        return;
    }

    if (!rcu_gp_completed()) {
        return;
    }

    spin_lock(&rcu_gp_lock);

    // Double-check under lock
    if (!__atomic_load_n(&rcu_state.gp_in_progress, __ATOMIC_ACQUIRE) ||
        !rcu_gp_completed()) {
        spin_unlock(&rcu_gp_lock);
        return;
    }

    // Grace period complete - update completed counter
    uint64 gp_completed = __atomic_fetch_add(&rcu_state.gp_seq_completed, 1, __ATOMIC_ACQ_REL) + 1;
    (void)gp_completed; // Used by per-CPU callback advancement
    __atomic_store_n(&rcu_state.gp_in_progress, 0, __ATOMIC_RELEASE);
    __atomic_fetch_add(&rcu_state.gp_count, 1, __ATOMIC_RELEASE);

    spin_unlock(&rcu_gp_lock);

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
    uint64 now = r_time();
    __atomic_store_n(&mycpu_ptr->rcu_timestamp, now, __ATOMIC_RELEASE);
    
    // Update statistics
    rcu_cpu_data_t *rcp = &rcu_cpu_data[cpu];
    __atomic_fetch_add(&rcp->qs_count, 1, __ATOMIC_RELEASE);

    // Try to advance grace period
    rcu_advance_gp();
    
    // Note: In timestamp-based RCU, callbacks are processed by checking
    // timestamps directly in rcu_process_callbacks_for_cpu(), not by
    // moving them between lists based on GP sequence numbers.
    
    pop_off();
}

// Called by scheduler to note that a context switch has occurred
// This is the main mechanism for tracking quiescent states in RCU
void rcu_check_callbacks(void) {
    // A context switch is a quiescent state - update the CPU's timestamp
    // This allows RCU to determine when grace periods have completed
    rcu_note_context_switch();
}

// ============================================================================
// RCU Callback Management
// ============================================================================

void call_rcu(rcu_head_t *head, rcu_callback_t func, void *data) {
    if (func == NULL) {
        return;
    }

    if (head == NULL) {
        // Allocate rcu_head_t from slab cache
        head = (rcu_head_t *)slab_alloc(&rcu_head_slab);
        if (head == NULL) {
            // Allocation failed, fall back to immediate invocation
            synchronize_rcu();
            func(data);
            return;
        }
        head->embedded_head = 0;
    } else {
        head->embedded_head = 1;
    }

    // Initialize callback before disabling preemption
    head->next = NULL;
    head->func = func;
    head->data = data;
    head->timestamp = r_time();  // Record when callback was registered

    // Disable preemption to ensure we stay on the same CPU
    push_off();
    
    int cpu = cpuid();
    rcu_cpu_data_t *rcp = &rcu_cpu_data[cpu];

    // Add to per-CPU pending callback list
    rcu_cblist_enqueue(rcp, head);
    __atomic_fetch_add(&rcp->cb_count, 1, __ATOMIC_RELEASE);

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
    
    // Wake up the RCU kthread to process callbacks
    rcu_kthread_wakeup();
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
        int embedded = cur->embedded_head;

        // Detach this node from the list before invoking callback
        cur->next = NULL;

        // Invoke the callback - after this, cur may be freed by user if embedded
        if (func != NULL) {
            func(data);
            count++;
        }

        // Free the rcu_head if it was allocated by call_rcu() (not embedded)
        if (!embedded) {
            slab_free(cur);
        }

        cur = next;

        // Note: We don't yield here because rcu_invoke_callbacks can be called
        // from various contexts (kthreads, synchronize_rcu, rcu_barrier) and
        // yielding could disrupt scheduler state in some callers.
    }

    __atomic_fetch_add(&rcu_state.cb_invoked, count, __ATOMIC_RELEASE);
    return count;
}

// Process completed RCU callbacks for a specific CPU using timestamp-based readiness
// IMPORTANT: This must only be called for the CURRENT CPU to maintain per-CPU exclusivity.
// This function manages its own push_off()/pop_off() calls around list manipulation.
static void rcu_process_callbacks_for_cpu(int cpu) {
    rcu_cpu_data_t *rcp = &rcu_cpu_data[cpu];

    // Get the minimum timestamp among CPUs OTHER than the target CPU
    // A callback is safe to invoke only if ALL other CPUs have context
    // switched after the callback was registered
    uint64 min_other_cpu_ts = rcu_get_min_other_cpu_timestamp(cpu);

    // Disable preemption while taking the list to prevent race with call_rcu()
    push_off();
    
    // Take the entire pending list
    rcu_head_t *pending = __atomic_exchange_n(&rcp->pending_head, NULL, __ATOMIC_ACQ_REL);
    __atomic_store_n(&rcp->pending_tail, NULL, __ATOMIC_RELEASE);
    
    pop_off();

    if (pending == NULL) {
        return;
    }

    // Separate into ready (timestamp <= min) and not-ready (timestamp > min)
    // This operates on local lists, no protection needed
    rcu_head_t *ready_head = NULL;
    rcu_head_t *ready_tail = NULL;
    rcu_head_t *notready_head = NULL;
    rcu_head_t *notready_tail = NULL;

    while (pending != NULL) {
        rcu_head_t *cur = pending;
        pending = pending->next;
        cur->next = NULL;

        // Check if this callback is ready (all other CPUs have switched at or after it)
        if (cur->timestamp <= min_other_cpu_ts) {
            // Ready to invoke
            if (ready_tail == NULL) {
                ready_head = ready_tail = cur;
            } else {
                ready_tail->next = cur;
                ready_tail = cur;
            }
        } else {
            // Not ready yet - put in temp list
            if (notready_tail == NULL) {
                notready_head = notready_tail = cur;
            } else {
                notready_tail->next = cur;
                notready_tail = cur;
            }
        }
    }

    // Invoke ready callbacks (preemption enabled - callbacks may need to sleep/yield)
    if (ready_head != NULL) {
        int count = rcu_invoke_callbacks(ready_head);
        __atomic_fetch_sub(&rcp->cb_count, count, __ATOMIC_RELEASE);
        __atomic_fetch_add(&rcp->cb_invoked, count, __ATOMIC_RELEASE);
    }

    // Disable preemption while putting callbacks back to prevent race with call_rcu()
    if (notready_head != NULL) {
        push_off();
        
        rcu_head_t *old_head = __atomic_load_n(&rcp->pending_head, __ATOMIC_ACQUIRE);
        notready_tail->next = old_head;
        __atomic_store_n(&rcp->pending_head, notready_head, __ATOMIC_RELEASE);
        if (old_head == NULL) {
            __atomic_store_n(&rcp->pending_tail, notready_tail, __ATOMIC_RELEASE);
        }
        
        pop_off();
    }
}

// Process completed RCU callbacks for current CPU using timestamp-based readiness
void rcu_process_callbacks(void) {
    // Get current CPU with preemption disabled
    push_off();
    int cpu = cpuid();
    pop_off();
    
    // Process callbacks - function manages its own push_off()/pop_off() internally
    rcu_process_callbacks_for_cpu(cpu);
}

// ============================================================================
// RCU Synchronization Primitives
// ============================================================================

void synchronize_rcu(void) {
    // Record the timestamp when synchronize_rcu was called
    // All CPUs must context-switch after this time for the grace period to complete
    uint64 sync_timestamp = r_time();
    
    // Update our own CPU's timestamp
    rcu_note_context_switch();

    // Wait for all OTHER CPUs to have timestamps >= sync_timestamp
    // This means they have all context-switched since we started
    int max_wait = 100000;
    int wait_count = 0;

    push_off();
    int my_cpu = cpuid();
    pop_off();

    while (wait_count < max_wait) {
        // Get minimum timestamp among other CPUs
        // If min >= sync_timestamp, all CPUs have passed quiescent state
        uint64 min_ts = rcu_get_min_other_cpu_timestamp(my_cpu);
        
        if (min_ts >= sync_timestamp) {
            // All CPUs have passed through a quiescent state
            // Wake up kthreads to process any ready callbacks
            for (int i = 0; i < NCPU; i++) {
                if (rcu_kthread[i].proc != NULL) {
                    wakeup_interruptible(rcu_kthread[i].proc);
                }
            }
            return;
        }
        
        // Yield to allow other CPUs to context switch
       scheduler_yield();
        wait_count++;
    }

    printf("synchronize_rcu: WARNING - not all CPUs passed quiescent state after %d iterations\n",
           max_wait);
}

void rcu_barrier(void) {
    // Wait for all pending callbacks that existed BEFORE this call to complete.
    // 
    // Timestamp-based strategy:
    // 1. Record barrier_timestamp = r_time()
    // 2. All callbacks registered before this have timestamp <= barrier_timestamp
    // 3. A callback is ready when callback.timestamp <= min_other_cpu_ts
    // 4. After synchronize_rcu(), all CPUs have rcu_timestamp >= barrier_timestamp
    // 5. So all pre-barrier callbacks become ready (their timestamp <= barrier_timestamp <= min_ts)
    // 6. Wake kthreads to process ready callbacks and wait until all are invoked
    
    // Record the barrier timestamp - all callbacks we care about have timestamp <= this
    uint64 barrier_timestamp = r_time();
    
    // First synchronize_rcu() ensures all CPUs have timestamp >= barrier_timestamp
    // This makes all pre-barrier callbacks ready for invocation
    synchronize_rcu();

    // Now all callbacks with timestamp <= barrier_timestamp are ready to invoke
    // Keep processing until all CPUs have no callbacks with timestamp <= barrier_timestamp
    int max_wait = 100000;
    int wait_count = 0;
    int all_done = 0;
    
    while (!all_done && wait_count < max_wait) {
        // Wake up all RCU kthreads to process callbacks
        for (int i = 0; i < NCPU; i++) {
            if (rcu_kthread[i].proc != NULL) {
                wakeup_interruptible(rcu_kthread[i].proc);
            }
        }
        
        // Process our own CPU's callbacks
        rcu_process_callbacks();
        
        // Check if all pre-barrier callbacks have been processed
        // We check each CPU's pending list for callbacks with timestamp <= barrier_timestamp
        all_done = 1;
        for (int i = 0; i < NCPU; i++) {
            rcu_cpu_data_t *rcp = &rcu_cpu_data[i];
            
            // Quick check: if cb_count is 0, no callbacks pending
            if (__atomic_load_n(&rcp->cb_count, __ATOMIC_ACQUIRE) == 0) {
                continue;
            }
            
            // Scan the pending list for old callbacks
            // Note: This is a read-only scan, safe to do from any CPU
            rcu_head_t *cb = __atomic_load_n(&rcp->pending_head, __ATOMIC_ACQUIRE);
            while (cb != NULL) {
                if (cb->timestamp <= barrier_timestamp) {
                    // Found an old callback that hasn't been processed yet
                    all_done = 0;
                    break;
                }
                cb = cb->next;
            }
            
            if (!all_done) {
                break;
            }
        }
        
        if (!all_done) {
            // Do another synchronize to advance timestamps and make more callbacks ready
            synchronize_rcu();
           scheduler_yield();
            wait_count++;
        }
    }
    
    // Final synchronize to ensure everything is flushed
    synchronize_rcu();
}

// ============================================================================
// Expedited Grace Period (Linux-inspired)
// ============================================================================

// Expedited grace period - forces immediate quiescent states on all CPUs
// This is faster than normal GP but has higher overhead (Linux-inspired)
// In timestamp-based RCU, we just wait for all CPUs to context switch
static void rcu_expedited_gp(void) {
    spin_lock(&rcu_gp_lock);

    // Check if expedited GP already in progress
    if (__atomic_load_n(&rcu_state.expedited_in_progress, __ATOMIC_ACQUIRE)) {
        spin_unlock(&rcu_gp_lock);
        return;
    }

    // Mark expedited GP in progress
    __atomic_store_n(&rcu_state.expedited_in_progress, 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&rcu_state.expedited_seq, 1, __ATOMIC_ACQ_REL);

    // Record start timestamp
    uint64 exp_start = r_time();
    
    spin_unlock(&rcu_gp_lock);

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
        
       scheduler_yield();
        wait_count++;
    }

    // Complete expedited GP
    spin_lock(&rcu_gp_lock);
    __atomic_store_n(&rcu_state.expedited_in_progress, 0, __ATOMIC_RELEASE);
    __atomic_fetch_add(&rcu_state.expedited_count, 1, __ATOMIC_RELEASE);
    spin_unlock(&rcu_gp_lock);
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
       scheduler_yield();
        wait_count++;
    }

    // Restore lazy GP setting
    __atomic_store_n(&rcu_state.gp_lazy_start, old_lazy, __ATOMIC_RELEASE);

    printf("synchronize_rcu_expedited: WARNING - expedited GP did not complete\n");
}

// ============================================================================
// Per-CPU RCU Callback Kernel Threads
// ============================================================================
//
// Each CPU has a dedicated kernel thread for processing RCU callbacks.
// This separates callback processing from the scheduler path, avoiding
// potential deadlocks and reducing latency in the context switch path.
//
// The kthreads:
// - Sleep when there are no ready callbacks
// - Wake up when rcu_kthread_wakeup() is called
// - Process callbacks in batches
// - Run at normal priority (not idle)
//

// RCU callback kthread main function
static int rcu_cb_kthread(uint64 cpu_id, uint64 arg2) {
    (void)arg2;
    
    // RCU kthreads should never migrate - affinity is set at creation
    assert(cpuid() == (int)cpu_id, "RCU kthread started on wrong CPU");
    
    printf("RCU callback kthread started on CPU %lu\n", cpu_id);
    
    while (1) {
        // Verify we're still on the correct CPU after each wakeup
        assert(cpuid() == (int)cpu_id, "RCU kthread running on wrong CPU");
        
        rcu_cpu_data_t *rcp = &rcu_cpu_data[cpu_id];
        
        // First, advance grace period state
        rcu_check_timestamp_overflow();
        rcu_advance_gp();
        
        // Get the minimum timestamp among OTHER CPUs
        // A callback is safe to invoke only if ALL other CPUs have context
        // switched after the callback was registered
        uint64 min_other_cpu_ts = rcu_get_min_other_cpu_timestamp((int)cpu_id);
        
        // Process callbacks from the pending list
        // Disable preemption while manipulating the list to prevent race with call_rcu()
        // which also runs on this CPU with preemption disabled.
        push_off();
        
        // Take the entire pending list atomically
        rcu_head_t *pending = __atomic_exchange_n(&rcp->pending_head, NULL, __ATOMIC_ACQ_REL);
        __atomic_store_n(&rcp->pending_tail, NULL, __ATOMIC_RELEASE);
        
        pop_off();
        
        // Separate into ready (timestamp < min) and not-ready (timestamp >= min)
        rcu_head_t *ready_head = NULL;
        rcu_head_t *ready_tail = NULL;
        rcu_head_t *notready_head = NULL;
        rcu_head_t *notready_tail = NULL;
        int ready_count = 0;
        
        while (pending != NULL) {
            rcu_head_t *cur = pending;
            pending = pending->next;
            cur->next = NULL;
            
            // Check if this callback is ready (all other CPUs have switched at or after it)
            if (cur->timestamp <= min_other_cpu_ts) {
                // Ready to invoke
                if (ready_tail == NULL) {
                    ready_head = ready_tail = cur;
                } else {
                    ready_tail->next = cur;
                    ready_tail = cur;
                }
                ready_count++;
            } else {
                // Not ready yet - put in temp list
                if (notready_tail == NULL) {
                    notready_head = notready_tail = cur;
                } else {
                    notready_tail->next = cur;
                    notready_tail = cur;
                }
            }
        }
        
        // Invoke ready callbacks
        if (ready_head != NULL) {
            int count = rcu_invoke_callbacks(ready_head);
            __atomic_fetch_sub(&rcp->cb_count, count, __ATOMIC_RELEASE);
            __atomic_fetch_add(&rcp->cb_invoked, count, __ATOMIC_RELEASE);
        }
        
        // Wake up any synchronize_rcu() waiters
        rcu_wakeup_gp_waiters();
        
        // Clear wakeup flag
        __atomic_store_n(&rcu_kthread[cpu_id].wakeup_pending, 0, __ATOMIC_RELEASE);
        
        // Put not-ready callbacks back to the pending list
        // Disable preemption to prevent race with call_rcu() during list manipulation
        if (notready_head != NULL) {
            push_off();
            
            // Prepend not-ready list to pending list
            // Since we hold push_off(), call_rcu() cannot interleave
            rcu_head_t *old_head = __atomic_load_n(&rcp->pending_head, __ATOMIC_ACQUIRE);
            notready_tail->next = old_head;
            __atomic_store_n(&rcp->pending_head, notready_head, __ATOMIC_RELEASE);
            if (old_head == NULL) {
                __atomic_store_n(&rcp->pending_tail, notready_tail, __ATOMIC_RELEASE);
            }
            
            pop_off();
            
            // There are still pending callbacks - take a nap before next iteration
           sleep_ms(50);
        } else {
            // No pending callbacks - can sleep longer
            sleep_ms(5000);
        }
    }
    
    return 0;
}

// Wake up the RCU callback thread for current CPU
void rcu_kthread_wakeup(void) {
    if (!__atomic_load_n(&rcu_kthreads_started, __ATOMIC_ACQUIRE)) {
        return;  // Kthreads not started yet
    }
    
    push_off();
    int cpu = cpuid();
    pop_off();
    
    struct proc *p = rcu_kthread[cpu].proc;
    if (p != NULL) {
        // Set wakeup flag and wake the thread
        __atomic_store_n(&rcu_kthread[cpu].wakeup_pending, 1, __ATOMIC_RELEASE);
        wakeup_interruptible(p);
    }
}

// Names for RCU kthreads - simple static strings
static const char *rcu_names[NCPU] = {
    "rcu_cb/0", "rcu_cb/1", "rcu_cb/2", "rcu_cb/3",
    "rcu_cb/4", "rcu_cb/5", "rcu_cb/6", "rcu_cb/7"
};

// Start RCU callback processing thread for a specific CPU
// Called from each CPU's init context (after rq_cpu_activate)
void rcu_kthread_start_cpu(int cpu) {
    if (cpu < 0 || cpu >= NCPU) {
        return;
    }
    
    // Initialize the kthread entry for this CPU
    rcu_kthread[cpu].proc = NULL;
    rcu_kthread[cpu].wakeup_pending = 0;
    
    struct proc *p = NULL;
    const char *name = (cpu < 8) ? rcu_names[cpu] : "rcu_cb";
    
    int pid = kernel_proc_create(name, &p, rcu_cb_kthread, cpu, 0, KERNEL_STACK_ORDER);
    if (pid < 0 || p == NULL) {
        printf("Failed to create RCU kthread for CPU %d\n", cpu);
        return;
    }
    
    // Set CPU affinity BEFORE waking the kthread
    struct sched_attr attr;
    sched_attr_init(&attr);
    attr.affinity_mask = (1ULL << cpu);
    sched_setattr(p->sched_entity, &attr);
    
    rcu_kthread[cpu].proc = p;
    
    // Wake the kthread - it will start on the correct CPU
    wakeup_proc(p);
    
    // Mark that at least one kthread is started
    __atomic_store_n(&rcu_kthreads_started, 1, __ATOMIC_RELEASE);
}

// Legacy function - kthreads are now started per-CPU in start_kernel()
// This is kept for compatibility but does nothing.
void rcu_kthread_start(void) {
    // Each CPU calls rcu_kthread_start_cpu() before entering idle loop
    // No global initialization needed here
}
