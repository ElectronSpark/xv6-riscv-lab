#ifndef __KERNEL_SPINLOCK_H
#define __KERNEL_SPINLOCK_H

// Mutual exclusion lock.
struct spinlock {
  uint locked;       // Is the lock held?

  // For debugging:
  char *name;        // Name of lock.
  struct cpu *cpu;   // The cpu holding the lock.
};

#endif      /* __KERNEL_SPINLOCK_H */
