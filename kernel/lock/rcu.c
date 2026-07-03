// Read-Copy-Update (RCU) synchronization mechanism for xv6
//
// RCU is a synchronization mechanism that allows readers to access shared data
// structures without locks while writers can update them. It's particularly
// efficient for read-mostly workloads.
//
// KEY CONCEPTS:
//   - Read-side critical sections: Protected by rcu_read_lock/unlock, very
//   lightweight
//   - Grace period: Time interval during which all pre-existing readers
//   complete
//   - Quiescent state: Point where a CPU is not in RCU read-side critical
//   section
//   - Callbacks: Functions invoked after a grace period completes
//   - Timestamp-based RCU: Grace period detection based on context switch
//   timestamps
//   - Per-CPU RCU kthreads: Background kernel threads for callback processing
//
// GRACE PERIOD DETECTION (Timestamp-based):
//   A grace period completes when all CPUs have context switched after the
//   grace period start timestamp. Each CPU records its last context switch
//   timestamp in mycpu()->rcu_timestamp, which is updated on every context
//   switch.
//
//   Algorithm:
//     1. When call_rcu() is called, callback records timestamp = get_jiffs()
//     2. Each CPU updates its rcu_timestamp on context switch
//     3. Callback ready when: callback.timestamp <= min(other CPUs'
//     rcu_timestamp)
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
//     - Increment / decrement per-thread nesting counter
//   No per-CPU nesting counters needed - grace period detection relies solely
//   on context switch timestamps, not on tracking nested read locks.
//
// PER-CPU CALLBACK LIST SYNCHRONIZATION:
//   Both call_rcu() and the kthread access the same CPU's callback list.  The
//   list also needs safe inspection from barrier paths, so each CPU callback
//   list has a real spinlock.  Callback invocation remains outside the lock.
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
#include "proc/thread.h"
#include "proc/tq.h"
#include "proc/sched.h"
#include "proc/rq.h"
#include "timer/timer.h"
#include "mm/slab.h"
#include "mm/vm.h"
#include "string.h"
#include "cmdline.h"

// Slab cache for rcu_head_t structures
static slab_cache_t rcu_head_slab = {0};

// Global RCU state
static rcu_state_t rcu_state;

// Per-CPU RCU data, sized from runtime CPU topology.
rcu_cpu_data_t *rcu_cpu_data;
static _Atomic int rcu_initialized;

// Lock protecting grace period state transitions
static spinlock_t rcu_gp_lock = SPINLOCK_INITIALIZED("rcu_gp_lock");

// Wait queue for threads waiting on grace period completion
static tq_t rcu_gp_waitq;
static spinlock_t rcu_gp_waitq_lock = SPINLOCK_INITIALIZED("rcu_gp_waitq_lock");

// Per-CPU RCU kthread state (forward declaration for rcu_barrier)
struct rcu_kthread_state {
    struct thread *kthread;      // The kthread
    volatile int wakeup_pending; // Flag to signal wakeup
};
static struct rcu_kthread_state *rcu_kthread;

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
#define RCU_LAZY_GP_DELAY 100 // Callbacks to accumulate before starting GP

enum rcu_head_trace_state {
    RCU_HEAD_TRACE_EMPTY = 0,
    RCU_HEAD_TRACE_ALLOCATED,
    RCU_HEAD_TRACE_QUEUED,
    RCU_HEAD_TRACE_INVOKING,
    RCU_HEAD_TRACE_FREED,
};

struct rcu_head_trace_slot {
    rcu_head_t *head;
    rcu_callback_t func;
    void *data;
    void *caller;
    uint64 seq;
    uint64 alloc_time;
    uint64 enqueue_time;
    uint64 invoke_time;
    uint64 free_time;
    int alloc_cpu;
    int enqueue_cpu;
    int invoke_cpu;
    int free_cpu;
    int embedded;
    int state;
    uint64 free_count;
};

#define RCU_HEAD_TRACE_BITS 12
#define RCU_HEAD_TRACE_SIZE (1UL << RCU_HEAD_TRACE_BITS)
#define RCU_HEAD_TRACE_MASK (RCU_HEAD_TRACE_SIZE - 1UL)

static spinlock_t rcu_head_trace_lock = SPINLOCK_INITIALIZED("rcu_head_trace");
static struct rcu_head_trace_slot rcu_head_trace_slots[RCU_HEAD_TRACE_SIZE];
static uint64 rcu_head_trace_seq;
static int rcu_head_trace_full_logged;

// Maximum value for uint64 type (defined locally to avoid stdint.h dependency)
#define RCU_UINT64_MAX ((uint64) - 1)

static int rcu_head_trace_enabled(void) {
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("rcu_head_trace", value, sizeof(value)) == 0 &&
                  cmdline_value_is_true(value);
        initialized = 1;
    }

    return enabled;
}

static const char *rcu_head_trace_state_name(int state) {
    switch (state) {
    case RCU_HEAD_TRACE_EMPTY:
        return "empty";
    case RCU_HEAD_TRACE_ALLOCATED:
        return "allocated";
    case RCU_HEAD_TRACE_QUEUED:
        return "queued";
    case RCU_HEAD_TRACE_INVOKING:
        return "invoking";
    case RCU_HEAD_TRACE_FREED:
        return "freed";
    default:
        return "unknown";
    }
}

static int rcu_head_trace_cpu(void) {
    push_off();
    int cpu = cpuid();
    pop_off();
    return cpu;
}

static uint64 rcu_head_trace_hash(rcu_head_t *head) {
    uint64 key = (uint64)head >> 4;
    key ^= key >> 12;
    key ^= key >> 24;
    return key & RCU_HEAD_TRACE_MASK;
}

static struct rcu_head_trace_slot *
rcu_head_trace_lookup_locked(rcu_head_t *head, int create) {
    uint64 idx = rcu_head_trace_hash(head);

    for (uint64 probe = 0; probe < RCU_HEAD_TRACE_SIZE; probe++) {
        struct rcu_head_trace_slot *slot = &rcu_head_trace_slots[idx];
        if (slot->head == head) {
            return slot;
        }
        if (slot->head == NULL) {
            if (!create) {
                return NULL;
            }
            slot->head = head;
            return slot;
        }
        idx = (idx + 1) & RCU_HEAD_TRACE_MASK;
    }

    return NULL;
}

static void rcu_head_trace_print_slot(const char *event,
                                      struct rcu_head_trace_slot *slot,
                                      rcu_head_t *head,
                                      rcu_callback_t func,
                                      void *data,
                                      void *caller,
                                      int cpu) {
    printf("rcu_head_trace: event=%s head=%p state=%s seq=%lu "
           "embedded=%d free_count=%lu func=%p data=%p caller=%p cpu=%d "
           "last_func=%p last_data=%p last_caller=%p alloc_cpu=%d "
           "enqueue_cpu=%d invoke_cpu=%d free_cpu=%d alloc_time=%lu "
           "enqueue_time=%lu invoke_time=%lu free_time=%lu\n",
           event, head, rcu_head_trace_state_name(slot->state), slot->seq,
           slot->embedded, slot->free_count, (void *)func, data, caller, cpu,
           (void *)slot->func, slot->data, slot->caller, slot->alloc_cpu,
           slot->enqueue_cpu, slot->invoke_cpu, slot->free_cpu,
           slot->alloc_time, slot->enqueue_time, slot->invoke_time,
           slot->free_time);
}

static void rcu_head_trace_note_alloc(rcu_head_t *head,
                                      rcu_callback_t func,
                                      void *data,
                                      void *caller) {
    if (head == NULL || !rcu_head_trace_enabled()) {
        return;
    }

    int cpu = rcu_head_trace_cpu();
    int log_event = 0;
    int log_full = 0;
    struct rcu_head_trace_slot snapshot = {0};

    spin_lock(&rcu_head_trace_lock);
    struct rcu_head_trace_slot *slot = rcu_head_trace_lookup_locked(head, 1);
    if (slot == NULL) {
        if (!rcu_head_trace_full_logged) {
            rcu_head_trace_full_logged = 1;
            log_full = 1;
        }
    } else {
        if (slot->seq != 0 && slot->state != RCU_HEAD_TRACE_FREED) {
            log_event = 1;
            snapshot = *slot;
        }
        slot->seq = ++rcu_head_trace_seq;
        slot->func = func;
        slot->data = data;
        slot->caller = caller;
        slot->alloc_time = r_time();
        slot->enqueue_time = 0;
        slot->invoke_time = 0;
        slot->free_time = 0;
        slot->alloc_cpu = cpu;
        slot->enqueue_cpu = -1;
        slot->invoke_cpu = -1;
        slot->free_cpu = -1;
        slot->embedded = 0;
        slot->state = RCU_HEAD_TRACE_ALLOCATED;
        slot->free_count = 0;
    }
    spin_unlock(&rcu_head_trace_lock);

    if (log_full) {
        printf("rcu_head_trace: owner table full, further head history may be "
               "missing\n");
    }
    if (log_event) {
        rcu_head_trace_print_slot("alloc_reuse_while_live", &snapshot, head,
                                  func, data, caller, cpu);
    }
}

static void rcu_head_trace_note_enqueue(rcu_head_t *head,
                                        rcu_callback_t func,
                                        void *data,
                                        void *caller,
                                        int embedded) {
    if (head == NULL || !rcu_head_trace_enabled()) {
        return;
    }

    int cpu = rcu_head_trace_cpu();
    int log_event = 0;
    int log_full = 0;
    struct rcu_head_trace_slot snapshot = {0};

    spin_lock(&rcu_head_trace_lock);
    struct rcu_head_trace_slot *slot = rcu_head_trace_lookup_locked(head, 1);
    if (slot == NULL) {
        if (!rcu_head_trace_full_logged) {
            rcu_head_trace_full_logged = 1;
            log_full = 1;
        }
    } else {
        if (slot->seq == 0) {
            slot->seq = ++rcu_head_trace_seq;
            slot->alloc_time = 0;
            slot->alloc_cpu = -1;
            slot->free_count = 0;
        } else if (slot->state == RCU_HEAD_TRACE_QUEUED ||
                   slot->state == RCU_HEAD_TRACE_INVOKING) {
            log_event = 1;
            snapshot = *slot;
        }
        slot->func = func;
        slot->data = data;
        slot->caller = caller;
        slot->enqueue_time = r_time();
        slot->enqueue_cpu = cpu;
        slot->invoke_time = 0;
        slot->free_time = 0;
        slot->invoke_cpu = -1;
        slot->free_cpu = -1;
        slot->embedded = embedded;
        slot->state = RCU_HEAD_TRACE_QUEUED;
        slot->free_count = 0;
    }
    spin_unlock(&rcu_head_trace_lock);

    if (log_full) {
        printf("rcu_head_trace: owner table full, further head history may be "
               "missing\n");
    }
    if (log_event) {
        rcu_head_trace_print_slot("duplicate_enqueue", &snapshot, head, func,
                                  data, caller, cpu);
    }
}

static void rcu_head_trace_note_invoke(rcu_head_t *head,
                                       rcu_callback_t func,
                                       void *data,
                                       int embedded) {
    if (head == NULL || !rcu_head_trace_enabled()) {
        return;
    }

    int cpu = rcu_head_trace_cpu();
    int log_event = 0;
    struct rcu_head_trace_slot snapshot = {0};

    spin_lock(&rcu_head_trace_lock);
    struct rcu_head_trace_slot *slot = rcu_head_trace_lookup_locked(head, 0);
    if (slot == NULL || slot->state != RCU_HEAD_TRACE_QUEUED) {
        log_event = 1;
        if (slot != NULL) {
            snapshot = *slot;
        }
    }
    if (slot != NULL) {
        slot->func = func;
        slot->data = data;
        slot->invoke_time = r_time();
        slot->invoke_cpu = cpu;
        slot->embedded = embedded;
        slot->state = RCU_HEAD_TRACE_INVOKING;
    }
    spin_unlock(&rcu_head_trace_lock);

    if (log_event) {
        rcu_head_trace_print_slot("invoke_without_queued", &snapshot, head,
                                  func, data, __builtin_return_address(0),
                                  cpu);
    }
}

static void rcu_head_trace_note_free(rcu_head_t *head,
                                     rcu_callback_t func,
                                     void *data,
                                     void *caller) {
    if (head == NULL || !rcu_head_trace_enabled()) {
        return;
    }

    int cpu = rcu_head_trace_cpu();
    int log_event = 0;
    struct rcu_head_trace_slot snapshot = {0};

    spin_lock(&rcu_head_trace_lock);
    struct rcu_head_trace_slot *slot = rcu_head_trace_lookup_locked(head, 0);
    if (slot == NULL || slot->state == RCU_HEAD_TRACE_FREED) {
        log_event = 1;
        if (slot != NULL) {
            snapshot = *slot;
        }
    }
    if (slot != NULL) {
        slot->func = func;
        slot->data = data;
        slot->caller = caller;
        slot->free_time = r_time();
        slot->free_cpu = cpu;
        slot->state = RCU_HEAD_TRACE_FREED;
        slot->free_count++;
    }
    spin_unlock(&rcu_head_trace_lock);

    if (log_event) {
        rcu_head_trace_print_slot("duplicate_free", &snapshot, head, func,
                                  data, caller, cpu);
    }
}

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
// or early boot), returns RCU_UINT64_MAX. This means all callbacks are
// considered ready, which is correct because there are no other CPUs that could
// be in RCU read-side critical sections.
static uint64 rcu_get_min_other_cpu_timestamp(int exclude_cpu) {
    uint64 min_ts = RCU_UINT64_MAX;
    for (int i = 0; i < cpu_possible_count(); i++) {
        if (i == exclude_cpu)
            continue; // Skip the excluded CPU
        struct cpu_local *cpu = &cpus[i];
        uint64 cpu_ts = __atomic_load_n(&cpu->rcu_timestamp, __ATOMIC_ACQUIRE);
        if (cpu_ts == 0)
            continue; // Skip uninitialized CPUs
        if (cpu_ts < min_ts) {
            min_ts = cpu_ts;
        }
    }
    // If no other CPUs have initialized timestamps, min_ts remains
    // RCU_UINT64_MAX, which means all callbacks are ready (no other CPUs to
    // wait for)
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

// Check if grace period has completed by verifying all CPUs have context
// switched Returns 1 if all CPUs have switched since GP start, 0 otherwise
//
// Algorithm: Compare each CPU's rcu_timestamp against gp_start_timestamp.
// A CPU has passed through a quiescent state if its timestamp >= gp_start.
static int rcu_gp_completed(void) {
    uint64 gp_start =
        __atomic_load_n(&rcu_state.gp_start_timestamp, __ATOMIC_ACQUIRE);

    // If no grace period has been started yet (gp_start == 0), it cannot be
    // complete
    if (gp_start == 0) {
        return 0;
    }

    // A grace period completes when all CPUs have timestamps >= gp_start
    // This means they have all context switched at or after the GP began

    for (int i = 0; i < cpu_possible_count(); i++) {
        struct cpu_local *cpu = &cpus[i];
        uint64 cpu_timestamp =
            __atomic_load_n(&cpu->rcu_timestamp, __ATOMIC_ACQUIRE);

        // If CPU timestamp is 0, it's uninitialized - skip it
        if (cpu_timestamp == 0) {
            continue;
        }

        // If CPU timestamp is less than GP start, this CPU hasn't context
        // switched yet
        if (cpu_timestamp < gp_start) {
            return 0;
        }
    }

    return 1; // All CPUs have switched
}

// Force quiescent states - not needed in timestamp-based RCU
// Removed as it's unused in the new implementation

// Wake up threads waiting in synchronize_rcu()
static void rcu_wakeup_gp_waiters(void) {
    spin_lock(&rcu_gp_waitq_lock);
    // Wake up all waiters - they will check if their GP has completed
    tq_wakeup_all(&rcu_gp_waitq, 0, 0);
    spin_unlock(&rcu_gp_waitq_lock);
}

// ============================================================================
// RCU Initialization
// ============================================================================

void rcu_init(void) {
    tq_init(&rcu_gp_waitq, "rcu_gp_waitq", &rcu_gp_waitq_lock);

    int ret =
        slab_cache_init(&rcu_head_slab, "rcu_head_cache", sizeof(rcu_head_t),
                        SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
    assert(ret == 0, "Failed to initialize rcu_head_cache slab cache, errno=%d",
           ret);

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

    int ncpu = cpu_possible_count();
    rcu_cpu_data = (rcu_cpu_data_t *)kvmalloc(sizeof(rcu_cpu_data_t) * ncpu);
    rcu_kthread = (struct rcu_kthread_state *)
        kvmalloc(sizeof(struct rcu_kthread_state) * ncpu);
    assert(rcu_cpu_data != NULL && rcu_kthread != NULL,
           "rcu_init: per-CPU allocation failed");
    memset(rcu_cpu_data, 0, sizeof(rcu_cpu_data_t) * ncpu);
    memset(rcu_kthread, 0, sizeof(struct rcu_kthread_state) * ncpu);

    // Initialize per-CPU data and timestamps
    for (int i = 0; i < ncpu; i++) {
        rcu_cpu_init(i);
        // Initialize CPU timestamp
        cpus[i].rcu_timestamp = 0;
    }

    __atomic_store_n(&rcu_initialized, 1, __ATOMIC_RELEASE);
}

void rcu_cpu_init(int cpu) {
    if (cpu < 0 || cpu >= cpu_possible_count()) {
        return;
    }

    rcu_cpu_data_t *rcp = &rcu_cpu_data[cpu];

    // Initialize pending callback list
    spin_init(&rcp->lock, "rcu_cblist");
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
// IMPLEMENTATION NOTE - Simplified Per-Thread Nesting:
//
// This implementation uses only per-thread nesting counters.
// Grace period detection is based on context switch timestamps, not nesting.
//
// rcu_read_lock() and rcu_read_unlock() only do:
//   - push_off() / pop_off() to prevent preemption
//   - increment / decrement thread nesting counter
//
// No per-CPU counters are needed since we rely on timestamps.
//

void rcu_read_lock(void) {
    // Disable interrupts to prevent context switches during RCU critical
    // section
    push_off();

    mycpu()->rcu_read_lock_nesting++;

    struct thread *t = current;
    if (t != NULL) {
        // Per-thread mirror for diagnostics/tests in normal thread context.
        t->rcu_read_lock_nesting++;
    }
}

void rcu_read_unlock(void) {
    struct cpu_local *c = mycpu();
    c->rcu_read_lock_nesting--;
    if (c->rcu_read_lock_nesting < 0) {
        panic("rcu_read_unlock: unbalanced CPU unlock on cpu %d", cpuid());
    }

    struct thread *t = current;
    if (t != NULL) {
        /*
         * Thread nesting is only a debug mirror.  IRQ/early paths are
         * correctly paired by the CPU counter above, but may observe a
         * different current thread than the one active at lock time.
         */
        if (t->rcu_read_lock_nesting > 0)
            t->rcu_read_lock_nesting--;
    }

    // Re-enable interrupts - matching the push_off() in rcu_read_lock()
    pop_off();
}

int rcu_is_watching(void) {
    push_off();
    int watching = mycpu()->rcu_read_lock_nesting > 0;
    pop_off();
    return watching;
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

    spin_lock(&rcp->lock);

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

    spin_unlock(&rcp->lock);
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
    uint64 gp_completed =
        __atomic_fetch_add(&rcu_state.gp_seq_completed, 1, __ATOMIC_ACQ_REL) +
        1;
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

    void *caller = __builtin_return_address(0);

    if (!__atomic_load_n(&rcu_initialized, __ATOMIC_ACQUIRE)) {
        /*
         * Early boot uses RCU-tagged structures before per-CPU RCU storage and
         * callback kthreads exist.  There is only one running CPU at that
         * point, so there are no concurrent RCU readers to wait for.
         */
        func(data);
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
        rcu_head_trace_note_alloc(head, func, data, caller);
    } else {
        head->embedded_head = 1;
    }

    // Initialize callback before disabling preemption
    head->next = NULL;
    head->func = func;
    head->data = data;
    head->timestamp = r_time(); // Record when callback was registered
    if (!head->embedded_head) {
        rcu_head_trace_note_enqueue(head, func, data, caller, 0);
    }

    // Disable preemption to ensure we stay on the same CPU
    push_off();

    int cpu = cpuid();
    rcu_cpu_data_t *rcp = &rcu_cpu_data[cpu];

    // Add to per-CPU pending callback list
    rcu_cblist_enqueue(rcp, head);
    __atomic_fetch_add(&rcp->cb_count, 1, __ATOMIC_RELEASE);

    // Update lazy callback counter
    int lazy_count =
        __atomic_fetch_add(&rcu_state.lazy_cb_count, 1, __ATOMIC_RELEASE);

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
// Uses batching to limit the number of callbacks invoked per call
// (Linux-inspired) Returns the number of callbacks invoked
static int rcu_invoke_callbacks(rcu_head_t *list) {
    rcu_head_t *cur = list;
    int count = 0;

    while (cur != NULL) {
        // Copy callback info BEFORE invoking, since callback may free the
        // rcu_head
        rcu_head_t *next = cur->next;
        rcu_callback_t func = cur->func;
        void *data = cur->data;
        int embedded = cur->embedded_head;
        if (!embedded) {
            rcu_head_trace_note_invoke(cur, func, data, embedded);
        }

        // Detach this node from the list before invoking callback
        cur->next = NULL;

        // Invoke the callback - after this, cur may be freed by user if
        // embedded
        if (func != NULL) {
            func(data);
            count++;
        }

        // Free the rcu_head if it was allocated by call_rcu() (not embedded)
        if (!embedded) {
            rcu_head_trace_note_free(cur, func, data,
                                     __builtin_return_address(0));
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

// Process completed RCU callbacks for a specific CPU using timestamp-based
// readiness IMPORTANT: This must only be called for the CURRENT CPU. The
// pending list is protected by the per-CPU callback-list lock; callback
// invocation happens after ready callbacks have been detached.
static void rcu_process_callbacks_for_cpu(int cpu) {
    rcu_cpu_data_t *rcp = &rcu_cpu_data[cpu];

    // Get the minimum timestamp among CPUs OTHER than the target CPU
    // A callback is safe to invoke only if ALL other CPUs have context
    // switched after the callback was registered
    uint64 min_other_cpu_ts = rcu_get_min_other_cpu_timestamp(cpu);

    // Take the entire pending list under the per-CPU callback lock.
    spin_lock(&rcp->lock);
    rcu_head_t *pending =
        __atomic_exchange_n(&rcp->pending_head, NULL, __ATOMIC_ACQ_REL);
    __atomic_store_n(&rcp->pending_tail, NULL, __ATOMIC_RELEASE);
    spin_unlock(&rcp->lock);

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

        // Check if this callback is ready (all other CPUs have switched at or
        // after it)
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

    // Invoke ready callbacks (preemption enabled - callbacks may need to
    // sleep/yield)
    if (ready_head != NULL) {
        int count = rcu_invoke_callbacks(ready_head);
        __atomic_fetch_sub(&rcp->cb_count, count, __ATOMIC_RELEASE);
        __atomic_fetch_add(&rcp->cb_invoked, count, __ATOMIC_RELEASE);
    }

    // Put callbacks back under the per-CPU callback lock.
    if (notready_head != NULL) {
        spin_lock(&rcp->lock);
        rcu_head_t *old_head =
            __atomic_load_n(&rcp->pending_head, __ATOMIC_ACQUIRE);
        notready_tail->next = old_head;
        __atomic_store_n(&rcp->pending_head, notready_head, __ATOMIC_RELEASE);
        if (old_head == NULL) {
            __atomic_store_n(&rcp->pending_tail, notready_tail,
                             __ATOMIC_RELEASE);
        }
        spin_unlock(&rcp->lock);
    }
}

// Process completed RCU callbacks for current CPU using timestamp-based
// readiness
void rcu_process_callbacks(void) {
    // Get current CPU with preemption disabled
    push_off();
    int cpu = cpuid();
    pop_off();

    // Process callbacks - function manages its own push_off()/pop_off()
    // internally
    rcu_process_callbacks_for_cpu(cpu);
}

// ============================================================================
// RCU Synchronization Primitives
// ============================================================================

void synchronize_rcu(void) {
    // Record the timestamp when synchronize_rcu was called
    // All CPUs must context-switch after this time for the grace period to
    // complete
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
            for (int i = 0; i < cpu_possible_count(); i++) {
                if (rcu_kthread[i].kthread != NULL) {
                    wakeup_interruptible(rcu_kthread[i].kthread);
                }
            }
            return;
        }

        // Yield to allow other CPUs to context switch
        scheduler_yield();
        wait_count++;
    }

    printf("synchronize_rcu: WARNING - not all CPUs passed quiescent state "
           "after %d iterations\n",
           max_wait);
}

void rcu_barrier(void) {
    // Wait for all pending callbacks that existed BEFORE this call to complete.
    //
    // Timestamp-based strategy:
    // 1. Record barrier_timestamp = r_time()
    // 2. All callbacks registered before this have timestamp <=
    // barrier_timestamp
    // 3. A callback is ready when callback.timestamp <= min_other_cpu_ts
    // 4. After synchronize_rcu(), all CPUs have rcu_timestamp >=
    // barrier_timestamp
    // 5. So all pre-barrier callbacks become ready (their timestamp <=
    // barrier_timestamp <= min_ts)
    // 6. Wake kthreads to process ready callbacks and wait until all are
    // invoked

    // Record the barrier timestamp - all callbacks we care about have timestamp
    // <= this
    uint64 barrier_timestamp = r_time();

    // First synchronize_rcu() ensures all CPUs have timestamp >=
    // barrier_timestamp This makes all pre-barrier callbacks ready for
    // invocation
    synchronize_rcu();

    // Now all callbacks with timestamp <= barrier_timestamp are ready to invoke
    // Keep processing until all CPUs have no callbacks with timestamp <=
    // barrier_timestamp
    int max_wait = 100000;
    int wait_count = 0;
    int all_done = 0;

    while (!all_done && wait_count < max_wait) {
        // Wake up all RCU kthreads to process callbacks
        for (int i = 0; i < cpu_possible_count(); i++) {
            if (rcu_kthread[i].kthread != NULL) {
                wakeup_interruptible(rcu_kthread[i].kthread);
            }
        }

        // Process our own CPU's callbacks
        rcu_process_callbacks();

        // Check if all pre-barrier callbacks have been processed
        // We check each CPU's pending list for callbacks with timestamp <=
        // barrier_timestamp
        all_done = 1;
        for (int i = 0; i < cpu_possible_count(); i++) {
            rcu_cpu_data_t *rcp = &rcu_cpu_data[i];

            // Quick check: if cb_count is 0, no callbacks pending
            if (__atomic_load_n(&rcp->cb_count, __ATOMIC_ACQUIRE) == 0) {
                continue;
            }

            // Scan the pending list for old callbacks under the callback-list
            // lock so detach/requeue on the owner CPU cannot invalidate the
            // traversal.
            spin_lock(&rcp->lock);
            rcu_head_t *cb =
                __atomic_load_n(&rcp->pending_head, __ATOMIC_ACQUIRE);
            while (cb != NULL) {
                if (cb->timestamp <= barrier_timestamp) {
                    // Found an old callback that hasn't been processed yet
                    all_done = 0;
                    break;
                }
                cb = cb->next;
            }
            spin_unlock(&rcp->lock);

            if (!all_done) {
                break;
            }
        }

        if (!all_done) {
            // Do another synchronize to advance timestamps and make more
            // callbacks ready
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

        for (int i = 0; i < cpu_possible_count(); i++) {
            struct cpu_local *cpu = &cpus[i];
            uint64 cpu_timestamp =
                __atomic_load_n(&cpu->rcu_timestamp, __ATOMIC_ACQUIRE);

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
    uint64 start_exp =
        __atomic_load_n(&rcu_state.expedited_seq, __ATOMIC_ACQUIRE);

    // Disable lazy GP start
    int old_lazy =
        __atomic_exchange_n(&rcu_state.gp_lazy_start, 0, __ATOMIC_ACQ_REL);

    // Run expedited grace period
    rcu_expedited_gp();

    // Start and wait for normal GP to complete (handles callback advancement)
    rcu_start_gp();

    int max_wait = 50000;
    int wait_count = 0;

    while (wait_count < max_wait) {
        uint64 current_exp =
            __atomic_load_n(&rcu_state.expedited_seq, __ATOMIC_ACQUIRE);

        if (current_exp > start_exp) {
            // Expedited GP completed
            __atomic_store_n(&rcu_state.gp_lazy_start, old_lazy,
                             __ATOMIC_RELEASE);
            return;
        }

        rcu_advance_gp();
        scheduler_yield();
        wait_count++;
    }

    // Restore lazy GP setting
    __atomic_store_n(&rcu_state.gp_lazy_start, old_lazy, __ATOMIC_RELEASE);

    printf(
        "synchronize_rcu_expedited: WARNING - expedited GP did not complete\n");
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

    // Affinity is set by rcu_kthread_start_cpu() before wakeup, and
    // rq_flush_wake_list() respects affinity, so we must be on the right CPU.
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

        // Process callbacks from the pending list under the per-CPU callback
        // lock.  Callback invocation happens after the list is detached.
        spin_lock(&rcp->lock);
        rcu_head_t *pending =
            __atomic_exchange_n(&rcp->pending_head, NULL, __ATOMIC_ACQ_REL);
        __atomic_store_n(&rcp->pending_tail, NULL, __ATOMIC_RELEASE);
        spin_unlock(&rcp->lock);

        // Separate into ready (timestamp < min) and not-ready (timestamp >=
        // min)
        rcu_head_t *ready_head = NULL;
        rcu_head_t *ready_tail = NULL;
        rcu_head_t *notready_head = NULL;
        rcu_head_t *notready_tail = NULL;
        int ready_count = 0;

        while (pending != NULL) {
            rcu_head_t *cur = pending;
            pending = pending->next;
            cur->next = NULL;

            // Check if this callback is ready (all other CPUs have switched at
            // or after it)
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
        __atomic_store_n(&rcu_kthread[cpu_id].wakeup_pending, 0,
                         __ATOMIC_RELEASE);

        // Put not-ready callbacks back to the pending list.
        if (notready_head != NULL) {
            spin_lock(&rcp->lock);
            rcu_head_t *old_head =
                __atomic_load_n(&rcp->pending_head, __ATOMIC_ACQUIRE);
            notready_tail->next = old_head;
            __atomic_store_n(&rcp->pending_head, notready_head,
                             __ATOMIC_RELEASE);
            if (old_head == NULL) {
                __atomic_store_n(&rcp->pending_tail, notready_tail,
                                 __ATOMIC_RELEASE);
            }
            spin_unlock(&rcp->lock);

            // There are still pending callbacks - take a nap before next
            // iteration
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
        return; // Kthreads not started yet
    }

    push_off();
    int cpu = cpuid();
    pop_off();

    struct thread *p = rcu_kthread[cpu].kthread;
    if (p != NULL) {
        // Set wakeup flag and wake the thread
        __atomic_store_n(&rcu_kthread[cpu].wakeup_pending, 1, __ATOMIC_RELEASE);
        wakeup_interruptible(p);
    }
}

// Names for RCU kthreads - simple static strings
static const char *rcu_names[] = {"rcu_cb/0", "rcu_cb/1", "rcu_cb/2",
                                  "rcu_cb/3", "rcu_cb/4", "rcu_cb/5",
                                  "rcu_cb/6", "rcu_cb/7"};

// Start RCU callback processing thread for a specific CPU
// Called from each CPU's init context (after rq_cpu_activate)
void rcu_kthread_start_cpu(int cpu) {
    if (cpu < 0 || cpu >= cpu_possible_count()) {
        return;
    }

    // Initialize the kthread entry for this CPU
    rcu_kthread[cpu].kthread = NULL;
    rcu_kthread[cpu].wakeup_pending = 0;

    struct thread *p = NULL;
    int name_count = sizeof(rcu_names) / sizeof(rcu_names[0]);
    const char *name = (cpu < name_count) ? rcu_names[cpu] : "rcu_cb";

    p = kthread_create(name, rcu_cb_kthread, cpu, 0, KERNEL_STACK_ORDER);
    if (IS_ERR_OR_NULL(p)) {
        printf("Failed to create RCU kthread for CPU %d\n", cpu);
        return;
    }

    // Set CPU affinity and priority BEFORE waking the kthread
    struct sched_attr attr;
    sched_attr_init(&attr);
    attr.affinity_mask = (1ULL << cpu);
    attr.priority = MAKE_PRIORITY(1, 0);
    sched_setattr(p->sched_entity, &attr);

    rcu_kthread[cpu].kthread = p;

    // Wake the kthread - it will start on the correct CPU
    wakeup(p);

    // Mark that at least one kthread is started
    __atomic_store_n(&rcu_kthreads_started, 1, __ATOMIC_RELEASE);
}

// Legacy function - kthreads are now started per-CPU in start_kernel()
// This is kept for compatibility but does nothing.
void rcu_kthread_start(void) {
    // Each CPU calls rcu_kthread_start_cpu() before entering idle loop
    // No global initialization needed here
}
