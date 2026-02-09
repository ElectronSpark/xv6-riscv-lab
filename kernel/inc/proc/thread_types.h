#ifndef __KERNEL_THREAD_TYPES_H
#define __KERNEL_THREAD_TYPES_H

#include "compiler.h"
#include "list_type.h"
#include "hlist_type.h"
#include "proc/tq_type.h"
#include "proc/rq_types.h"
#include "trapframe.h"
#include "signal_types.h"
#include "mm/vm_types.h"
#include "vfs/vfs_types.h"
#include <smp/atomic.h>
#include <smp/percpu.h>
#include "lock/rcu_type.h"

struct vfs_inode;

// @TODO: stop signal may miss
enum thread_state {
    THREAD_UNUSED,
    THREAD_USED,
    THREAD_INTERRUPTIBLE,
    THREAD_KIILABLE,
    THREAD_TIMER,
    THREAD_KIILABLE_TIMER,
    THREAD_UNINTERRUPTIBLE,
    THREAD_WAKENING,
    THREAD_RUNNING,
    THREAD_STOPPED,
    THREAD_EXITING,
    THREAD_ZOMBIE
};

struct workqueue;

// Per-thread state
struct thread {
    struct spinlock lock;

    // both p->lock and the corresponding thread queue lock must be held
    // when using these.
    //
    // If the thread is trying to yield as RUNNABLE, it must hold __sched_lock
    // after acquiring p->lock, and before switching to the scheduler.
    //
    // When the thread is in SLEEPING state, these fields are managed by the
    // scheduler, and the thread queue it's in.
    enum thread_state state; // Thread state
    void *chan;              // If non-zero, sleeping on chan
    list_node_t sched_entry; // entry for ready queue
    struct workqueue *wq;    // work queue this thread belongs to
    list_node_t wq_entry;    // link to work queue
    uint64 flags;
#define THREAD_FLAG_VALID 1
#define THREAD_FLAG_KILLED 2     // Thread is exiting or exited
#define THREAD_FLAG_ONCHAN 3     // Thread is sleeping on a channel
#define THREAD_FLAG_SIGPENDING 4 // Thread has pending deliverable signals
#define THREAD_FLAG_USER_SPACE 5 // Thread has user space
    uint64 clone_flags;          // flags used during clone

    // process table lock must be held before holding p->lock to use this:
    hlist_entry_t proctab_entry; // Entry to link the pid hash table

    // p->lock must be held when using these:
    list_node_t dmp_list_entry; // Entry in the dump list
    int xstate;                 // Exit status to be returned to parent's wait
    int pid;                    // Thread ID

    // Signal related fields
    sigacts_t *sigacts; // Signal actions for this thread
    // Thread-local signal state
    thread_signal_t signal; // Per-thread signal state
    struct thread
        *vfork_parent; // Parent waiting for vfork child (NULL if not vfork)

    // both p->lock and p->parent->lock must be held when using this:
    list_node_t siblings;  // List of sibling threads
    list_node_t children;  // List of child threads
    int children_count;    // Number of children
    struct thread *parent; // Parent thread

    // these are private to the thread, so p->lock need not be held.
    uint64 kstack;    // Virtual address of kernel stack
    int kstack_order; // Kernel stack order, used for allocation
    uint64 ksp;
    vm_t *vm;                     // Virtual memory areas and page table
    struct utrapframe *trapframe; // data page for trampoline.S
    uint64 trapframe_vbase;       // Base virtual address of the trapframe

    // Priority Inheritance lock, on_rq, on_cpu, cpu_id, and context are now
    // stored in sched_entity. Access them via p->sched_entity-><field>.
    struct sched_entity *sched_entity;
    uint64 kentry; // Entry point for kernel thread
    uint64 arg[2]; // Argument for kernel thread

    struct fs_struct *fs; // Filesystem state (on kernel stack below utrapframe)
    struct vfs_fdtable
        *fdtable;  // File descriptor table (on kernel stack below fs)
    char name[16]; // Thread name (debugging)

    // RCU read-side critical section nesting counter (per-thread)
    // This counter follows the thread across CPU migrations, enabling
    // preemptible RCU. It tracks how many times this thread has called
    // rcu_read_lock() without matching rcu_read_unlock(). The thread can
    // safely yield and migrate CPUs while this is > 0.
    int rcu_read_lock_nesting; // Number of nested rcu_read_lock() calls

    // RCU deferred freeing
    rcu_head_t rcu_head; // RCU callback head (must be last)
};

BUILD_BUG_ON(((sizeof(struct thread) + sizeof(struct utrapframe) +
               sizeof(struct fs_struct) + sizeof(struct vfs_fdtable) +
               sizeof(struct sched_entity) + 80 + CACHELINE_SIZE) &
              ~CACHELINE_MASK) >= PGSIZE);

#endif /* __KERNEL_THREAD_TYPES_H */
