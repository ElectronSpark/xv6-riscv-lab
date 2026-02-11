#ifndef __KERNEL_RCU_H
#define __KERNEL_RCU_H

#include "rcu_type.h"
#include "compiler.h"

// RCU Read-Side Critical Section API
// These functions mark the boundaries of RCU read-side critical sections.
// Inside a critical section, the reader holds a reference to RCU-protected data
// and prevents it from being reclaimed.

/**
 * rcu_read_lock() - Enter RCU read-side critical section
 *
 * Marks the beginning of an RCU read-side critical section. The reader can
 * safely access RCU-protected data structures. These critical sections may
 * nest, and the data structure will remain protected until the matching
 * rcu_read_unlock() is called.
 *
 * RCU read-side critical sections are very lightweight - just a counter
 * increment and a compiler barrier.
 */
void rcu_read_lock(void);

/**
 * rcu_read_unlock() - Exit RCU read-side critical section
 *
 * Marks the end of an RCU read-side critical section. Must be paired with
 * a preceding rcu_read_lock(). After this call, the reader no longer holds
 * a reference to the RCU-protected data.
 */
void rcu_read_unlock(void);

/**
 * rcu_dereference() - Safely dereference an RCU-protected pointer
 * @p: The pointer to dereference
 *
 * Returns the value of the pointer with proper memory ordering semantics
 * to ensure that any initialization of the pointed-to structure is visible
 * to the reader.
 *
 * Must be called within an RCU read-side critical section.
 */
#define rcu_dereference(p)                                                     \
    ({                                                                         \
        typeof(p) _p = __atomic_load_n(&(p), __ATOMIC_CONSUME);                \
        _p;                                                                    \
    })

/**
 * rcu_assign_pointer() - Assign to an RCU-protected pointer
 * @p: Pointer to assign to
 * @v: Value to assign
 *
 * Assigns a new value to an RCU-protected pointer with proper memory
 * ordering to ensure that any initialization of the pointed-to structure
 * is visible before the pointer is updated.
 */
#define rcu_assign_pointer(p, v) __atomic_store_n(&(p), (v), __ATOMIC_RELEASE)

// RCU Synchronization API

/**
 * synchronize_rcu() - Wait for grace period to complete
 *
 * Blocks until all pre-existing RCU read-side critical sections have
 * completed. After this function returns, it is safe to free or reclaim
 * memory that was previously protected by RCU.
 *
 * This is a blocking operation and must not be called from interrupt context
 * or with locks held that could cause deadlock.
 */
void synchronize_rcu(void);

/**
 * call_rcu() - Register callback to be invoked after grace period
 * @head: RCU callback structure
 * @func: Callback function to invoke
 * @data: Data to pass to callback function
 *
 * Registers a callback to be invoked after a grace period has elapsed.
 * This is the non-blocking alternative to synchronize_rcu(). The callback
 * will be invoked in thread context (not interrupt context).
 *
 * The callback function will be called with the provided data pointer.
 * It is the callback's responsibility to free any memory or perform
 * cleanup operations.
 */
void call_rcu(rcu_head_t *head, rcu_callback_t func, void *data);

/**
 * rcu_barrier() - Wait for all pending RCU callbacks to complete
 *
 * Blocks until all previously registered RCU callbacks have been invoked.
 * This is useful during shutdown or when you need to ensure that all
 * cleanup has completed.
 */
void rcu_barrier(void);

/**
 * synchronize_rcu_expedited() - Wait for expedited grace period
 *
 * Similar to synchronize_rcu() but uses expedited mechanisms to complete
 * the grace period faster. This is useful for latency-sensitive operations
 * but has higher overhead than normal grace periods.
 *
 * Use this sparingly - only when the latency of synchronize_rcu() is
 * unacceptable for your use case.
 */
void synchronize_rcu_expedited(void);

// RCU Grace Period Management (Internal API)

/**
 * rcu_check_callbacks() - Check if CPU has passed through quiescent state
 *
 * Called by the scheduler to check if the current CPU has passed through
 * a quiescent state (e.g., context switch, idle, user mode). This helps
 * advance grace period completion.
 *
 * This function is called from scheduler context.
 */
void rcu_check_callbacks(void);

/**
 * rcu_process_callbacks() - Process completed RCU callbacks
 *
 * Invokes callbacks that have completed their grace period. This should
 * be called periodically from a safe context (e.g., softirq or workqueue).
 */
void rcu_process_callbacks(void);

/**
 * rcu_note_context_switch() - Note that CPU has context switched
 *
 * This function should be called whenever a CPU performs a context switch.
 * A context switch is a quiescent state for RCU.
 */
void rcu_note_context_switch(void);

/**
 * rcu_is_watching() - Check if RCU is watching current CPU
 *
 * Returns true if the current CPU is in an RCU read-side critical section,
 * false otherwise.
 */
int rcu_is_watching(void);

// RCU Initialization

/**
 * rcu_init() - Initialize RCU subsystem
 *
 * Must be called during kernel initialization before any RCU functions
 * are used.
 */
void rcu_init(void);

/**
 * rcu_cpu_init() - Initialize RCU for a specific CPU
 * @cpu: CPU ID to initialize
 *
 * Called during per-CPU initialization to set up RCU state for the CPU.
 */
void rcu_cpu_init(int cpu);

/**
 * rcu_kthread_start() - Start per-CPU RCU callback processing threads
 *
 * Creates a kernel thread on each CPU to handle RCU callback processing.
 * These threads wake up when there are ready callbacks to process and
 * handle callback invocation separately from the scheduler path.
 *
 * Must be called after scheduler and thread subsystems are initialized.
 */
void rcu_kthread_start(void);

/**
 * rcu_kthread_start_cpu() - Start RCU callback kthread for a specific CPU
 * @cpu: CPU ID to start the kthread for
 *
 * Called from each CPU's init context (after rq_cpu_activate) to create
 * the RCU callback kthread for that CPU. Only active CPUs should call this.
 */
void rcu_kthread_start_cpu(int cpu);

/**
 * rcu_kthread_wakeup() - Wake up the RCU callback thread for current CPU
 *
 * Called when there are ready callbacks that need processing.
 * The RCU kthread will wake up and invoke the callbacks.
 */
void rcu_kthread_wakeup(void);

/**
 * rcu_run_tests() - Run comprehensive RCU test suite
 *
 * Executes all RCU tests including basic operations, pointer operations,
 * grace periods, callbacks, and concurrent readers. Should be called
 * after RCU initialization is complete.
 */
void rcu_run_tests(void);

// RCU Utility Macros

/**
 * rcu_access_pointer() - Access an RCU-protected pointer without dereferencing
 * @p: The pointer to access
 *
 * Returns the value of the pointer without the overhead of rcu_dereference().
 * Use this when you only need to check if the pointer is NULL or compare it,
 * not dereference it.
 */
#define rcu_access_pointer(p) __atomic_load_n(&(p), __ATOMIC_RELAXED)

/**
 * RCU_INIT_POINTER() - Initialize an RCU-protected pointer
 * @p: Pointer to initialize
 * @v: Value to initialize to
 *
 * Initialize an RCU-protected pointer. Unlike rcu_assign_pointer(), this
 * does not include memory barriers and should only be used during
 * initialization before the pointer is published.
 */
#define RCU_INIT_POINTER(p, v)                                                 \
    do {                                                                       \
        (p) = (v);                                                             \
    } while (0)

// RCU List Macros (for RCU-protected linked lists)

/**
 * list_for_each_entry_rcu() - Iterate over RCU-protected list
 * @pos: Loop cursor
 * @head: List head
 * @member: Name of list_node_t within the structure
 *
 * Iterate over an RCU-protected list. Must be called within an RCU
 * read-side critical section.
 */
#define list_for_each_entry_rcu(pos, head, member)                             \
    for (pos = container_of(rcu_dereference((head)->next), typeof(*pos),       \
                            member);                                           \
         &pos->member != (head);                                               \
         pos = container_of(rcu_dereference(pos->member.next), typeof(*pos),   \
                            member))

#endif /* __KERNEL_RCU_H */
