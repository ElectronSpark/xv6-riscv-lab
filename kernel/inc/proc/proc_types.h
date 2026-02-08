#ifndef __KERNEL_PROC_TYPES_H
#define __KERNEL_PROC_TYPES_H

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

enum procstate {
    PSTATE_UNUSED,
    PSTATE_USED,
    PSTATE_INTERRUPTIBLE,
    STATE_KILLABLE,
    STATE_TIMER,
    STATE_KILLABLE_TIMER,
    PSTATE_UNINTERRUPTIBLE,
    PSTATE_WAKENING,
    PSTATE_RUNNING,
    PSTATE_STOPPED,
    PSTATE_EXITING,
    PSTATE_ZOMBIE
};

struct workqueue;

// Per-process state
struct proc {
    struct spinlock lock;

    // both p->lock and the corresponding proc queue lock must be held
    // when using these.
    //
    // If the process is trying to yield as RUNNABLE, it must hold __sched_lock
    // after acquiring p->lock, and before switching to the scheduler.
    //
    // When the process is in SLEEPING state, these fields are managed by the
    // scheduler, and the process queue it's in.
    enum procstate state;    // Process state
    void *chan;              // If non-zero, sleeping on chan
    list_node_t sched_entry; // entry for ready queue
    struct workqueue *wq;    // work queue this process belongs to
    list_node_t wq_entry;    // link to work queue
    uint64 flags;
#define PROC_FLAG_VALID 1
#define PROC_FLAG_KILLED 2     // Process is exiting or exited
#define PROC_FLAG_ONCHAN 3     // Process is sleeping on a channel
#define PROC_FLAG_SIGPENDING 4 // Process has pending deliverable signals
#define PROC_FLAG_USER_SPACE 5 // Process has user space
    uint64 clone_flags;         // flags used during clone

    // proc table lock must be held before holding p->lock to use this:
    hlist_entry_t proctab_entry; // Entry to link the process hash table

    // p->lock must be held when using these:
    list_node_t dmp_list_entry; // Entry in the dump list
    int xstate;                 // Exit status to be returned to parent's wait
    int pid;                    // Process ID

    // Signal related fields
    sigacts_t *sigacts;             // Signal actions for this process
    // Thread-local signal state
    thread_signal_t signal;   // Per-thread signal state
    struct proc *vfork_parent;  // Parent waiting for vfork child (NULL if not vfork)

    // both p->lock and p->parent->lock must be held when using this:
    list_node_t siblings; // List of sibling processes
    list_node_t children; // List of child processes
    int children_count;   // Number of children
    struct proc *parent;  // Parent process

    // these are private to the process, so p->lock need not be held.
    uint64 kstack;    // Virtual address of kernel stack
    int kstack_order; // Kernel stack order, used for allocation
    uint64 ksp;
    vm_t *vm;                     // Virtual memory areas and page table
    struct utrapframe *trapframe; // data page for trampoline.S
    uint64 trapframe_vbase;    // Base virtual address of the trapframe

    // Priority Inheritance lock, on_rq, on_cpu, cpu_id, and context are now
    // stored in sched_entity. Access them via p->sched_entity-><field>.
    struct sched_entity *sched_entity;
    uint64 kentry; // Entry point for kernel process
    uint64 arg[2]; // Argument for kernel process

    struct fs_struct *fs; // Filesystem state (on kernel stack below utrapframe)
    struct vfs_fdtable
        *fdtable;  // File descriptor table (on kernel stack below fs)
    char name[16]; // Process name (debugging)

    // RCU read-side critical section nesting counter (per-process)
    // This counter follows the process across CPU migrations, enabling
    // preemptible RCU. It tracks how many times this process has called
    // rcu_read_lock() without matching rcu_read_unlock(). The process can
    // safely yield and migrate CPUs while this is > 0.
    int rcu_read_lock_nesting; // Number of nested rcu_read_lock() calls

    // RCU deferred freeing
    rcu_head_t rcu_head; // RCU callback head (must be last)
};

BUILD_BUG_ON(((sizeof(struct proc) + sizeof(struct utrapframe) +
               sizeof(struct fs_struct) + sizeof(struct vfs_fdtable) +
               sizeof(struct sched_entity) + 80 + CACHELINE_SIZE) &
              ~CACHELINE_MASK) >= PGSIZE);

#endif /* __KERNEL_PROC_TYPES_H */
