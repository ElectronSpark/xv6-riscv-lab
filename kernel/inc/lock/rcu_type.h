#ifndef __KERNEL_RCU_TYPE_H
#define __KERNEL_RCU_TYPE_H

#include "compiler.h"
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
    uint64              timestamp;      // Timestamp when callback was registered
} rcu_head_t;

// Simplified callback list approach:
// Use a single pending list per CPU. Callbacks record their registration timestamp.
// A callback is ready when: callback.timestamp <= min(all other CPUs' rcu_timestamp)
// This avoids complex segment pointer management and ready/not-ready list separation.

// Per-CPU RCU data structure
// Aligned to cache line to prevent false sharing between CPUs
typedef struct {
    // Timestamp when this CPU last context switched (stored in cpu_local->rcu_timestamp)
    // Used for grace period detection - all CPUs must have context switched after grace period start
    // We don't store timestamp here, we read it from mycpu()->rcu_timestamp
    
    // Pending callbacks list
    // Callbacks are checked for readiness based on their timestamp vs other CPUs' timestamps
    // Access is protected by push_off()/pop_off() to ensure CPU-local exclusivity
    rcu_head_t * _Atomic pending_head;
    rcu_head_t * _Atomic pending_tail;

    // Statistics
    _Atomic uint64      cb_count;       // Number of callbacks pending
    _Atomic uint64      qs_count;       // Number of quiescent states reported
    _Atomic uint64      cb_invoked;     // Number of callbacks invoked on this CPU
} __ALIGNED_CACHELINE rcu_cpu_data_t;

// Global RCU state structure
typedef struct {
    // Grace period start timestamp
    _Atomic uint64      gp_start_timestamp;

    // Completed grace period count
    _Atomic uint64      gp_seq_completed;

    // Grace period in progress flag
    _Atomic int         gp_in_progress;

    // Grace period lazy start - accumulate callbacks before starting GP
    _Atomic int         gp_lazy_start;
    _Atomic int         lazy_cb_count;  // Number of callbacks waiting for lazy GP

    // Expedited grace period support (Linux-inspired)
    _Atomic int         expedited_in_progress;
    _Atomic uint64      expedited_seq;

    // Global statistics
    _Atomic uint64      gp_count;       // Total grace periods completed
    _Atomic uint64      cb_invoked;     // Total callbacks invoked
    _Atomic uint64      expedited_count; // Number of expedited GPs
} rcu_state_t;

// Per-CPU RCU data - declared separately to ensure cache-line alignment per CPU
// Each CPU's data is in its own cache line to prevent false sharing
extern rcu_cpu_data_t rcu_cpu_data[NCPU];

#endif /* __KERNEL_RCU_TYPE_H */
