#ifndef __KERNEL_SLEEPLOCK_H
#define __KERNEL_SLEEPLOCK_H

#include "compiler.h"

// Long-term locks for processes
struct sleeplock {
  uint locked;       // Is the lock held?
  struct spinlock lk; // spinlock protecting this sleep lock
  
  // For debugging:
  char *name;        // Name of lock.
  int pid;           // Process holding lock
};

#endif        /* __KERNEL_SLEEPLOCK_H */
