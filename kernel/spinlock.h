#ifndef __KERNEL_SPINLOCK_H
#define __KERNEL_SPINLOCK_H

#include "compiler.h"
#include "types.h"

// Mutual exclusion lock.
typedef struct spinlock {
  uint locked;       // Is the lock held?

  // For debugging:
  char *name;        // Name of lock.
  struct cpu *cpu;   // The cpu holding the lock.
} spinlock_t;

#define SPINLOCK_INITIALIZED(lock_name) { \
  .locked = 0,                            \
  .cpu    = NULL                          \
  .name   = lock_name,                    \
}

#endif      /* __KERNEL_SPINLOCK_H */
