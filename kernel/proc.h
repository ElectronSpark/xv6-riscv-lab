#ifndef __KERNEL_PROC_H
#define __KERNEL_PROC_H

#include "compiler.h"
#include "list_type.h"
#include "hlist_type.h"
#include "proc_queue_type.h"
#include "trapframe.h"
#include "signal_types.h"
#include "vm_types.h"

// Saved registers for kernel context switches.
struct context {
  uint64 ra;
  uint64 sp;

  // callee-saved
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

// Per-CPU state.
struct cpu {
  struct proc *proc;          // The process running on this cpu, or null.
  struct context context;     // swtch() here to enter scheduler().
  int noff;                   // Depth of push_off() nesting.
  int intena;                 // Were interrupts enabled before push_off()?
};

extern struct cpu cpus[NCPU];

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, EXITING, ZOMBIE };

static inline const char *procstate_to_str(enum procstate state) {
  switch (state) {
    case UNUSED: return "UNUSED";
    case USED: return "USED";
    case SLEEPING: return "SLEEPING";
    case RUNNABLE: return "RUNNABLE";
    case RUNNING: return "RUNNING";
    case ZOMBIE: return "ZOMBIE";
    default: return "UNKNOWN";
  }
}

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
  proc_queue_entry_t queue_entry;     // Entry in a process queue
  
  // proc table lock must be held before holding p->lock to use this:
  hlist_entry_t proctab_entry; // Entry to link the process hash table
  
  // p->lock must be held when using these:
  list_node_t dmp_list_entry;  // Entry in the dump list
  int killed;                  // If non-zero, have been killed
  int needs_resched;           // If non-zero, process needs to be rescheduled
  int xstate;                  // Exit status to be returned to parent's wait
  int pid;                     // Process ID

  // Signal related fields
  sigacts_t *sigacts;          // Signal actions for this process
  // signal trap frames would be put at the user stack.
  // This is used to restore the user context when a signal is delivered.
  uint64 sig_ucontext;    // Address of the signal user context
  stack_t sig_stack;      // Alternate signal stack
  sigpending_t sigqueue;    // Queue of pending signals

  // both p->lock and p->parent->lock must be held when using this:
  list_node_t siblings;       // List of sibling processes
  list_node_t children;       // List of child processes
  int children_count;         // Number of children
  struct proc *parent;        // Parent process

  // these are private to the process, so p->lock need not be held.
  uint64 kstack;               // Virtual address of kernel stack
  vm_t *vm;                     // Virtual memory areas and page table
  struct trapframe *trapframe; // data page for trampoline.S

  // both p->lock and __sched_lock must be held 
  struct context context;      // swtch() here to run process
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
};

#endif        /* __KERNEL_PROC_H */
