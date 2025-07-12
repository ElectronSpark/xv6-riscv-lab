#ifndef __KERNEL_SLEEPLOCK_H
#define __KERNEL_SLEEPLOCK_H

#include "compiler.h"
#include "proc_queue_type.h"

// Long-term locks for processes
struct sleeplock {
  struct proc_queue wait_queue; // Queue of processes waiting for the lock
  uint locked;       // Is the lock held?
  struct spinlock lk; // spinlock protecting this sleep lock
  
  // For debugging:
  char *name;        // Name of lock.
  int pid;           // Process holding lock
};

#endif        /* __KERNEL_SLEEPLOCK_H */
