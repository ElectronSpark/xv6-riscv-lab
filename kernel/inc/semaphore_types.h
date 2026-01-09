#ifndef __KERNEL_SEMAPHORE_TYPES_H
#define __KERNEL_SEMAPHORE_TYPES_H

#include "compiler.h"
#include "proc/proc_queue_type.h"
#include "spinlock.h"

// Long-term locks for processes
typedef struct semaphore {
  proc_queue_t wait_queue; // Queue of processes waiting for the lock
  struct spinlock lk; // spinlock protecting this sleep lock
  int value; // Semaphore value

  // For debugging:
  const char *name;        // Name of lock.
} sem_t;

#endif        /* __KERNEL_SEMAPHORE_TYPES_H */