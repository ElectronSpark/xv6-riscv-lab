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
#include "proc/thread_group_types.h"

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
//
// Cache-line-optimized layout: fields are grouped by access frequency and
// co-access patterns. Each hot group is separated by __STRUCT_CACHELINE_PADDING
// to prevent false sharing.
//
// CL0: lock         — isolated; bounces between CPUs on signal/fork/wait
// CL1: sched hot    — state, flags, sched_entity, ksp, chan, sched_entry, wq
// CL2: trap hot     — trapframe, vm, pid, kstack, fs, fdtable, rcu_nesting
// CL3: family tree  — sigacts, parent, children, siblings, xstate
// cold: init/debug  — clone_flags, kentry, arg, name, thread_group, proctab...
// signal (large)    — thread_signal_t (560B, pushed to end)
// rcu_head          — must be last
struct thread {
    // ===== Cache line 0: Lock (isolated to prevent false sharing) =====
    spinlock_t lock;
    __STRUCT_CACHELINE_PADDING;

    // ===== Cache line 1: Scheduler hot path =====
    // Accessed on every context switch / wakeup. state and flags use atomics.
    // p->lock and the corresponding thread queue lock must be held when using
    // state, chan, and sched_entry.
    enum thread_state state; // Thread state
    uint64 flags;
#define THREAD_FLAG_VALID 1
#define THREAD_FLAG_KILLED 2           // Thread is exiting or exited
#define THREAD_FLAG_ONCHAN 3           // Thread is sleeping on a channel
#define THREAD_FLAG_SIGPENDING 4       // Thread has pending deliverable signals
#define THREAD_FLAG_USER_SPACE 5       // Thread has user space
#define THREAD_FLAG_SELF_REAP 6        // Non-leader CLONE_THREAD: self-cleanup on exit
    struct sched_entity *sched_entity; // PI lock, on_rq, context, etc.
    uint64 ksp;
    void *chan;              // If non-zero, sleeping on chan
    list_node_t sched_entry; // entry for ready queue
    struct workqueue *wq;    // work queue this thread belongs to
    __STRUCT_CACHELINE_PADDING;

    // ===== Cache line 2: Trap / syscall hot path =====
    // Accessed on every trap entry/exit and most syscalls.
    // These are private to the thread or stable read-mostly pointers.
    struct utrapframe *trapframe; // data page for trampoline.S
    vm_t *vm;                     // Virtual memory areas and page table
    // ===== Cold: Initialization / rarely changed =====
    uint64 clone_flags;     // flags used during clone
    uint64 kentry;          // Entry point for kernel thread
    uint64 arg[2];          // Argument for kernel thread
    char name[16];          // Thread name (debugging)
    int kstack_order;       // Kernel stack order, used for allocation
    uint64 kstack;          // Virtual address of kernel stack
    uint64 trapframe_vbase; // Base virtual address of the trapframe
    struct fs_struct *fs; // Filesystem state (on kernel stack below utrapframe)
    struct vfs_fdtable
        *fdtable; // File descriptor table (on kernel stack below fs)
    int rcu_read_lock_nesting; // Number of nested rcu_read_lock() calls
    __STRUCT_CACHELINE_PADDING;

    // ===== Cache line 3: Family tree (fork / exit / wait) =====
    // p->lock must be held when using these.
    // Both p->lock and p->parent->lock for siblings.
    sigacts_t *sigacts;    // Signal actions for this thread
    struct thread *parent; // Parent thread
    // Parent waiting for vfork child (NULL if not vfork)
    struct thread *vfork_parent;
    // Thread group (POSIX process abstraction).
    // All threads created with CLONE_THREAD share the same thread_group.
    // Threads created with fork/clone (no CLONE_THREAD) get their own group.
    struct thread_group *thread_group; // Thread group this thread belongs to
    pid_t sid;            // Session ID (for job control, not implemented)
    pid_t pgid;           // Process group ID (for job control, not implemented)
    pid_t tgid;           // Thread group ID (process ID)
    pid_t pid;            // Thread ID
    list_node_t tg_entry; // Link in thread_group->thread_list
    list_node_t pg_entry; // Link in process group list (not implemented)
    list_node_t sid_entry; // Link in session list (not implemented)
    list_node_t wq_entry;  // link to work queue
    list_node_t children;  // List of child threads
    int children_count;    // Number of children
    int xstate;            // Exit status to be returned to parent's wait
    list_node_t siblings;  // List of sibling threads
    __STRUCT_CACHELINE_PADDING;

    // ===== Cold: Registration / debug =====
    // process table lock must be held before holding p->lock to use this:
    hlist_entry_t proctab_entry; // Entry to link the pid hash table
    list_node_t dmp_list_entry;  // Entry in the dump list

    // ===== Signal (warm, large — pushed to end) =====
    thread_signal_t signal; // Per-thread signal state

    // ===== RCU deferred freeing (must be last) =====
    rcu_head_t rcu_head; // RCU callback head (must be last)
};

BUILD_BUG_ON(((sizeof(struct thread) + sizeof(struct utrapframe) +
               sizeof(struct fs_struct) + sizeof(struct vfs_fdtable) +
               sizeof(struct sched_entity) + 80 + CACHELINE_SIZE) &
              ~CACHELINE_MASK) >= PGSIZE);

#endif /* __KERNEL_THREAD_TYPES_H */
