#ifndef __KERNEL_THREAD_RQ_TYPES_H
#define __KERNEL_THREAD_RQ_TYPES_H

#include "compiler.h"
#include "riscv.h"
#include "types.h"
#include "lock/spinlock.h"
#include "trapframe.h"
#include "list_type.h"
#include "bintree_type.h"

struct rq;
struct rq_percpu;
struct sched_entity;

// Priority level count - needed for struct rq_percpu definition
#define PRIORITY_MAINLEVELS 64

#define SCHED_FIXEDPOINT_SHIFT 10
#define SCHED_FIXEDPOINT_ONE (1 << SCHED_FIXEDPOINT_SHIFT)
struct load_weight {
    uint32 weight;     // Weight for scheduling
    uint32 inv_weight; // Inverse weight for calculations
};

// Scheduler attributes for configuring a task's scheduling parameters.
// Used with sched_getattr() and sched_setattr() APIs.
struct sched_attr {
    uint32 size;             // Size of this structure (for versioning)
    cpumask_t affinity_mask; // CPU affinity bitmask
    uint32 time_slice; // Time slice length in ticks (placeholder - not yet
                       // implemented)
    int priority;      // Scheduling priority (major + minor)
    uint32 flags;      // Reserved for future use
};

// I picked some callbacks from Linux's sched_class as examples.
// Note: se->on_rq and se->on_cpu should be managed out of rq layer
struct sched_class {
    // When a task is added to or removed from the run queue
    void (*enqueue_task)(struct rq *rq, struct sched_entity *se);
    void (*dequeue_task)(struct rq *rq, struct sched_entity *se);

    struct rq *(*select_task_rq)(struct rq *rq, struct sched_entity *se,
                                 cpumask_t cpumask);

    // Select the next task to run
    // Every sched class has to implement at least pick_next_task
    //
    // Task Switch Flow:
    // =================
    //
    //   Run Queue (data structure)          CPU (current task)
    //   ┌─────────────────────────┐         ┌─────────────────┐
    //   │  [A] [B] [C] [D] ...    │         │     prev        │
    //   └─────────────────────────┘         └─────────────────┘
    //             │                                  │
    //             │ pick_next_task(rq)               │
    //             │ (select next, keep in queue)     │
    //             ▼                                  │
    //        next = [A]                              │
    //             │                                  │
    //             │ set_next_task(rq, next)          │
    //             │ (remove next from queue,         │
    //             │  set as current)                 │
    //             ▼                                  ▼
    //   ┌─────────────────────────┐         ┌─────────────────┐
    //   │  [B] [C] [D] ...        │         │     next        │
    //   └─────────────────────────┘         └─────────────────┘
    //             │                                  │
    //             │         ~~~ context switch ~~~   │
    //             │         (now running as next)    │
    //             │                                  │
    //             │                    put_prev_task(rq, prev)
    //             │                    (insert prev back to queue,
    //             │                     unset as current)
    //             ▼                                  │
    //   ┌─────────────────────────┐         ┌─────────────────┐
    //   │  [B] [C] [D] [prev] ... │         │     next        │
    //   └─────────────────────────┘         └─────────────────┘
    //
    struct sched_entity *(*pick_next_task)(struct rq *rq);
    void (*put_prev_task)(struct rq *rq, struct sched_entity *se);
    void (*set_next_task)(struct rq *rq, struct sched_entity *se);

    // Called on each timer tick for the currently running task
    void (*task_tick)(struct rq *rq, struct sched_entity *se);

    // When creating or exiting a thread
    void (*task_fork)(struct rq *rq, struct sched_entity *se);
    void (*task_dead)(struct rq *rq, struct sched_entity *se);

    // When volunrarily yielding the CPU
    void (*yield_task)(struct rq *rq);
};

struct rq {
    struct sched_class *sched_class; // Scheduling class in use
    int class_id;                    // Scheduling class ID
    int task_count;                  // Number of threads in the run queue
    int cpu_id;                      // CPU ID this run queue belongs to
} __ALIGNED_CACHELINE;

/**
 * @brief Per-CPU run queue data, cache-line aligned to avoid false sharing.
 *
 * Each CPU has its own instance of this structure, containing all per-CPU
 * scheduler state. Use rq_percpu_lock_get() / rq_percpu_put_unlock() to access.
 */
struct rq_percpu {
    struct rq
        *rqs[PRIORITY_MAINLEVELS]; /**< Per-priority run queues for this CPU */
    uint64 ready_mask; /**< Top-level ready mask (8 groups of 8 priorities) */
    uint64 ready_mask_secondary; /**< Secondary ready mask (64 priority bits) */
    spinlock_t rq_lock; /**< Lock protecting this CPU's run queue data */
    struct sched_entity
        *wake_list_head; /**< Head of the wake list for this CPU */
    struct sched_entity
        *current_se; /**< Currently running sched_entity on this CPU */
} __ALIGNED_CACHELINE;

struct sched_entity {
    union {
        struct rb_node rb_entry; // For red-black tree (if needed)
        list_node_t list_entry;  // For linked list (if needed)
    };
    struct rq *rq;                   // Pointer to the run queue
    int priority;                    // Scheduling priority
    struct thread *thread;           // Back pointer to the thread
    struct sched_class *sched_class; // Scheduling class
    // Priority Inheritance lock is adopted from Linux kernel.
    // Although we don't have priority levels yet, we still need pi_lock to
    // protect wakening up thread.
    // pi_lock does not protect sleeping thread, it's role is to avoid
    // multiple wakeups to the same thread at the same time.
    // pi_lock should be acquired before sched lock
    spinlock_t pi_lock; // priority inheritance lock
    int on_rq;          // The thread is on a ready queue
    int on_cpu;         // The thread is running on a CPU
    int cpu_id;         // The CPU running this thread.
    // Next entity to wake up(entry in wake list)
    struct sched_entity *wake_next;
    cpumask_t affinity_mask; // CPU affinity mask

    uint64 start_time; // Time when the thread started running
    uint64 exec_start; // Last time the thread started executing
    uint64 exec_end;   // Last time the thread stopped executing

    struct context context; // swtch() here to run thread
};

// Helper to get sched_entity from context pointer (used after context switch)
static inline struct sched_entity *se_from_context(struct context *ctx) {
    return container_of(ctx, struct sched_entity, context);
}

// Helper to get thread from context pointer (used after context switch)
static inline struct thread *thread_from_context(struct context *ctx) {
    return se_from_context(ctx)->thread;
}

// Idle process rq
struct idle_rq {
    struct rq rq;
    struct thread *idle_thread; // Idle process for this CPU
};

#define FIFO_RQ_SUBLEVELS 4 // Number of minor priority levels (2 bits)

// Sublevel queue structure for FIFO scheduler
struct fifo_subqueue {
    list_node_t head; // FIFO run queue head
    int count;        // Number of tasks in this subqueue
};

struct fifo_rq {
    struct rq rq;
    struct fifo_subqueue subqueues[FIFO_RQ_SUBLEVELS]; // One per minor priority
    uint8 ready_mask; // Bitmask of non-empty subqueues
};

#endif // __KERNEL_THREAD_RQ_TYPES_H
