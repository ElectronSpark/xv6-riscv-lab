#ifndef __KERNEL_RCU_TYPE_H
#define __KERNEL_RCU_TYPE_H

#include "types.h"
#include "param.h"
#include "list_type.h"

// RCU callback function type
typedef void (*rcu_callback_t)(void *data);

// RCU callback structure
typedef struct rcu_head {
    struct rcu_head     *next;          // Next callback in the list
    rcu_callback_t      func;           // Callback function
    void                *data;          // Data to pass to callback
} rcu_head_t;

// RCU callback segmentation (Linux-style 4-segment approach)
// Segments progress through grace period stages for efficient batching
#define RCU_NEXT_READY_TAIL     0  // Callbacks ready for next GP
#define RCU_NEXT_TAIL           1  // Callbacks for GP after next
#define RCU_WAIT_TAIL           2  // Callbacks waiting for current GP
#define RCU_DONE_TAIL           3  // Callbacks ready to invoke
#define RCU_CBLIST_NSEGS        4

// Per-CPU RCU data structure
typedef struct {
    // Current grace period number when this CPU last reported quiescent state
    _Atomic uint64      gp_seq;

    // Tracks total number of outermost RCU read locks held by processes on this CPU
    // Used for quiescent state detection (CPU is quiescent when nesting == 0)
    // Note: This is incremented only for OUTERMOST locks (when process nesting was 0)
    // Combined with per-process nesting (proc->rcu_read_lock_nesting) for preemptible RCU
    _Atomic int         nesting;

    // Indicates if this CPU needs to report a quiescent state
    _Atomic int         qs_pending;

    // Segmented callback lists (Linux-inspired 4-segment design)
    // All callbacks in a single list, with pointers to segment boundaries
    rcu_head_t          *cb_head;                        // Head of callback list
    rcu_head_t          **cb_tail[RCU_CBLIST_NSEGS];     // Tail pointers (ptr to ptr) for each segment
    uint64              gp_seq_needed[RCU_CBLIST_NSEGS]; // GP seq needed for each segment

    // Statistics
    _Atomic uint64      cb_count;       // Number of callbacks pending
    _Atomic uint64      qs_count;       // Number of quiescent states reported
    _Atomic uint64      cb_invoked;     // Number of callbacks invoked on this CPU
} rcu_cpu_data_t;

// Global RCU state structure
typedef struct {
    // Current grace period sequence number
    _Atomic uint64      gp_seq;

    // Completed grace period sequence number
    _Atomic uint64      gp_seq_completed;

    // Bitmap of CPUs that have passed through quiescent state
    _Atomic uint64      qs_mask;

    // Grace period in progress flag
    _Atomic int         gp_in_progress;

    // Grace period lazy start - accumulate callbacks before starting GP
    _Atomic int         gp_lazy_start;
    _Atomic int         lazy_cb_count;  // Number of callbacks waiting for lazy GP

    // Expedited grace period support (Linux-inspired)
    _Atomic int         expedited_in_progress;
    _Atomic uint64      expedited_seq;

    // Waiting processes for synchronize_rcu() (for sleep/wakeup optimization)
    void                *gp_wait_queue;  // Will be cast to appropriate wait structure

    // Per-CPU RCU data
    rcu_cpu_data_t      cpu_data[NCPU];

    // Global statistics
    _Atomic uint64      gp_count;       // Total grace periods completed
    _Atomic uint64      cb_invoked;     // Total callbacks invoked
    _Atomic uint64      expedited_count; // Number of expedited GPs
} rcu_state_t;

#endif /* __KERNEL_RCU_TYPE_H */
