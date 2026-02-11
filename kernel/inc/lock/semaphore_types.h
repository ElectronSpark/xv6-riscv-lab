#ifndef __KERNEL_SEMAPHORE_TYPES_H
#define __KERNEL_SEMAPHORE_TYPES_H

#include "compiler.h"
#include "proc/tq_type.h"
#include "spinlock.h"

// Long-term locks for threads
typedef struct semaphore {
  spinlock_t lk; // spinlock protecting this sleep lock
  tq_t wait_queue; // Queue of threads waiting for the lock
  int value; // Semaphore value

  // For debugging:
  const char *name;        // Name of lock.
} sem_t;

#endif        /* __KERNEL_SEMAPHORE_TYPES_H */