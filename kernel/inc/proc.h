#ifndef __KERNEL_PROC_H
#define __KERNEL_PROC_H

#include "compiler.h"
#include "list_type.h"
#include "hlist_type.h"
#include "proc_queue_type.h"
#include "trapframe.h"
#include "signal_types.h"
#include "vm_types.h"

struct vfs_inode;

extern struct cpu cpus[NCPU];

#define SET_NEEDS_RESCHED(p) \
  do { mycpu()->needs_resched = 1; } while(0)
#define CLEAR_NEEDS_RESCHED(p) \
  do { mycpu()->needs_resched = 0; } while(0)
#define NEEDS_RESCHED(p) \
  (!!(mycpu()->needs_resched))

enum procstate {
  PSTATE_UNUSED,
  PSTATE_USED,
  PSTATE_INTERRUPTIBLE,
  STATE_KILLABLE,
  STATE_TIMER,
  STATE_KILLABLE_TIMER,
  PSTATE_UNINTERRUPTIBLE,
  PSTATE_RUNNABLE,
  PSTATE_RUNNING,
  PSTATE_EXITING,
  PSTATE_ZOMBIE
};

#define PSTATE_IS_SLEEPING(state) ({    \
  (state) == PSTATE_INTERRUPTIBLE ||    \
  (state) == PSTATE_UNINTERRUPTIBLE ||  \
  (state) == STATE_KILLABLE ||          \
  (state) == STATE_TIMER ||             \
  (state) == STATE_KILLABLE_TIMER;      \
})

#define PSTATE_IS_KILLABLE(state) ({    \
  (state) == STATE_KILLABLE ||          \
  (state) == STATE_KILLABLE_TIMER ||    \
  (state) == PSTATE_INTERRUPTIBLE;      \
})

#define PSTATE_IS_TIMER(state) ({       \
  (state) == STATE_TIMER ||             \
  (state) == STATE_KILLABLE_TIMER ||    \
  (state) == PSTATE_INTERRUPTIBLE;      \
})

#define PSTATE_IS_INTERRUPTIBLE(state) ({ \
  (state) == PSTATE_INTERRUPTIBLE;        \
})

#define PSTATE_IS_AWOKEN(state) ({    \
  (state) == PSTATE_RUNNABLE ||       \
  (state) == PSTATE_RUNNING;          \
})

#define PSTATE_IS_ZOMBIE(state) ({    \
  (state) == PSTATE_ZOMBIE;           \
})

struct workqueue;
struct vfs_file;

// Per-process file descriptor table
// It is protected by proc lock when used within a process
struct vfs_fdtable {
    int fd_count;
    int next_fd;
    struct vfs_file *files[NOFILE];
};

// Per-process state
struct proc {
  struct spinlock lock;

  // both p->lock and the corresponding proc queue lock must be held
  // when using these. 
  // 
  // If the process is trying to yield as RUNNABLE, it must hold __sched_lock
  // after acquiring p->lock, and before switching to the scheduler.
  //
  // When the process is in SLEEPING state, these fields are managed by the scheduler,
  // and the process queue it's in.
  enum procstate state;        // Process state
  void *chan;                  // If non-zero, sleeping on chan
  list_node_t sched_entry;     // entry for ready queue
  struct workqueue *wq;        // work queue this process belongs to
  list_node_t wq_entry;        // link to work queue
  struct context context;      // swtch() here to run process
  uint64 flags;
#define PROC_FLAG_VALID             0x1
#define PROC_FLAG_KILLED            0x8   // Process is exiting or exited
#define PROC_FLAG_ONCHAN            0x10  // Process is sleeping on a channel
#define PROC_FLAG_STOPPED           0x20  // Process is stopped
#define PROC_FLAG_USER_SPACE        0x40  // Process has user space
  
  // proc table lock must be held before holding p->lock to use this:
  hlist_entry_t proctab_entry; // Entry to link the process hash table
  
  // p->lock must be held when using these:
  list_node_t dmp_list_entry;  // Entry in the dump list
  int xstate;                  // Exit status to be returned to parent's wait
  int pid;                     // Process ID

  // Signal related fields
  sigacts_t *sigacts;          // Signal actions for this process
  sigset_t sig_pending_mask;   // Mark none empty signal pending queue
  sigpending_t sig_pending[NSIG]; // Queue of pending signals
  // signal trap frames would be put at the user stack.
  // This is used to restore the user context when a signal is delivered.
  uint64 sig_ucontext;    // Address of the signal user context
  stack_t sig_stack;      // Alternate signal stack

  // both p->lock and p->parent->lock must be held when using this:
  list_node_t siblings;       // List of sibling processes
  list_node_t children;       // List of child processes
  int children_count;         // Number of children
  struct proc *parent;        // Parent process

  // these are private to the process, so p->lock need not be held.
  uint64 kstack;               // Virtual address of kernel stack
  int kstack_order;            // Kernel stack order, used for allocation
  uint64 ksp;
  vm_t *vm;                     // Virtual memory areas and page table
  struct utrapframe *trapframe; // data page for trampoline.S

  // both p->lock and __sched_lock must be held 
  int cpu_id;                  // The CPU running this process.
  uint64 kentry;               // Entry point for kernel process
  uint64 arg[2];               // Argument for kernel process
  // struct file *ofile[NOFILE];  // Open files
  // struct inode *cwd;           // Current directory
  // @TODO: replace the original XV6 fs subsystem with VFS
  struct {
    struct vfs_inode_ref rooti; // Root inode
    struct vfs_inode_ref cwd;   // Current working directory inode
    struct vfs_fdtable fdtable; // File descriptor table
  } fs;
  char name[16];               // Process name (debugging)

  // RCU read-side critical section nesting counter (per-process)
  // This counter follows the process across CPU migrations, enabling preemptible RCU.
  // It tracks how many times this process has called rcu_read_lock() without matching
  // rcu_read_unlock(). The process can safely yield and migrate CPUs while this is > 0.
  int rcu_read_lock_nesting;   // Number of nested rcu_read_lock() calls
};

BUILD_BUG_ON((sizeof(struct proc) + sizeof(struct utrapframe) + 16) >= PGSIZE);

static inline uint64 proc_flags(struct proc *p) {
  if (p == NULL) {
    return 0;
  }
  return __atomic_load_n(&p->flags, __ATOMIC_SEQ_CST);
}

static inline void proc_set_flags(struct proc *p, uint64 flags) {
  if (p == NULL) {
    return;
  }
  __atomic_or_fetch(&p->flags, flags, __ATOMIC_SEQ_CST);
}

static inline void proc_clear_flags(struct proc *p, uint64 flags) {
  if (p == NULL) {
    return;
  }
  __atomic_and_fetch(&p->flags, ~flags, __ATOMIC_SEQ_CST);
}

#define PROC_SET_USER_SPACE(p) \
  proc_set_flags(p, PROC_FLAG_USER_SPACE)
#define PROC_SET_VALID(p) \
  proc_set_flags(p, PROC_FLAG_VALID)
#define PROC_SET_KILLED(p) \
  proc_set_flags(p, PROC_FLAG_KILLED)
#define PROC_SET_ONCHAN(p) \
  proc_set_flags(p, PROC_FLAG_ONCHAN)
#define PROC_SET_STOPPED(p) \
  proc_set_flags(p, PROC_FLAG_STOPPED)

#define PROC_CLEAR_USER_SPACE(p) \
  proc_clear_flags(p, PROC_FLAG_USER_SPACE)
#define PROC_CLEAR_VALID(p) \
  proc_clear_flags(p, PROC_FLAG_VALID)
#define PROC_CLEAR_KILLED(p) \
  proc_clear_flags(p, PROC_FLAG_KILLED)
#define PROC_CLEAR_ONCHAN(p) \
  proc_clear_flags(p, PROC_FLAG_ONCHAN)
#define PROC_CLEAR_STOPPED(p) \
  proc_clear_flags(p, PROC_FLAG_STOPPED)

#define PROC_VALID(p) \
  (!!(proc_flags(p) & PROC_FLAG_VALID))
#define PROC_KILLED(p) \
  (!!(proc_flags(p) & PROC_FLAG_KILLED))
#define PROC_ONCHAN(p) \
  (!!(proc_flags(p) & PROC_FLAG_ONCHAN))
#define PROC_STOPPED(p) \
  (!!(proc_flags(p) & PROC_FLAG_STOPPED))
#define PROC_USER_SPACE(p) \
  (!!(proc_flags(p) & PROC_FLAG_USER_SPACE))

static inline const char *procstate_to_str(enum procstate state) {
  switch (state) {
    case PSTATE_UNUSED: return "unused";
    case PSTATE_USED: return "used";
    case PSTATE_INTERRUPTIBLE: return "interruptible";
    case PSTATE_UNINTERRUPTIBLE: return "uninterruptible";
    case PSTATE_RUNNABLE: return "runnable";
    case PSTATE_RUNNING: return "running";
    case PSTATE_EXITING: return "exiting";
    case PSTATE_ZOMBIE: return "zombie";
    default: return "*unknown";
  }
}

static inline enum procstate __proc_get_pstate(struct proc *p) {
  if (p == NULL) {
    return PSTATE_UNUSED;
  }
  return __atomic_load_n(&p->state, __ATOMIC_SEQ_CST);
}

static inline void __proc_set_pstate(struct proc *p, enum procstate state) {
  if (p == NULL) {
    return;
  }
  __atomic_store_n(&p->state, state, __ATOMIC_SEQ_CST);
}

#define PROC_AWOKEN(p) PSTATE_IS_AWOKEN(__proc_get_pstate(p))
#define PROC_SLEEPING(p) PSTATE_IS_SLEEPING(__proc_get_pstate(p))
#define PROC_ZOMBIE(p) PSTATE_IS_ZOMBIE(__proc_get_pstate(p))
#define PROC_KILLABLE(p) PSTATE_IS_KILLABLE(__proc_get_pstate(p))
#define PROC_TIMER(p) PSTATE_IS_TIMER(__proc_get_pstate(p))
#define PROC_INTERRUPTIBLE(p) PSTATE_IS_INTERRUPTIBLE(__proc_get_pstate(p))


#endif        /* __KERNEL_PROC_H */
