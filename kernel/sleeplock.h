#ifndef __KERNEL_SLEEPLOCK_H
#define __KERNEL_SLEEPLOCK_H

#include "compiler.h"
#include "proc_queue_type.h"

// Long-term locks for processes
typedef struct mutex {
  struct proc_queue wait_queue; // Queue of processes waiting for the lock
  struct spinlock lk; // spinlock protecting this sleep lock
  
  // For debugging:
  char *name;        // Name of lock.
  struct proc *holder;    // Process holding lock
} mutex_t;

#endif        /* __KERNEL_SLEEPLOCK_H */
