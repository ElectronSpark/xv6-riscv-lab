#ifndef __KERNEL_MUTEX_TYPES_H
#define __KERNEL_MUTEX_TYPES_H

#include "compiler.h"
#include "proc/tq_type.h"

// Long-term locks for threads
typedef struct mutex {
  tq_t wait_queue; // Queue of threads waiting for the lock
  struct spinlock lk; // spinlock protecting this sleep lock
  
  // For debugging:
  char *name;        // Name of lock.
  pid_t holder;    // Thread holding lock
} mutex_t;

#endif        /* __KERNEL_MUTEX_TYPES_H */
