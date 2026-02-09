#ifndef __KERNEL_SPINLOCK_H
#define __KERNEL_SPINLOCK_H

#include "compiler.h"
#include "types.h"

// Mutual exclusion lock.
typedef struct spinlock {
  uint locked;       // Is the lock held?

  // For debugging:
  char *name;        // Name of lock.
  struct cpu_local *cpu;   // The cpu holding the lock.
} spinlock_t __ALIGNED_CACHELINE;

#define SPINLOCK_INITIALIZED(lock_name) { \
  .locked = 0,                            \
  .cpu    = NULL,                         \
  .name   = lock_name,                    \
}

/**
 * @brief Default sleep/wakeup callbacks for spinlock-protected waits.
 *
 * spin_sleep_cb releases the spinlock before yielding; spin_wake_cb
 * re-acquires it after wakeup.  Used by tq_wait_in_state() and
 * ttree_wait_in_state() as the default callbacks.
 *
 * Status convention: spin_sleep_cb returns 1 (lock released) or
 * 0 (data was NULL, no-op).  spin_wake_cb only re-acquires when
 * @c sleep_cb_status is non-zero.
 */
int spin_sleep_cb(void *data);
void spin_wake_cb(void *data, int sleep_cb_status);

#endif      /* __KERNEL_SPINLOCK_H */
