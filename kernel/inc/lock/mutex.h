#ifndef __KERNEL_MUTEX_H
#define __KERNEL_MUTEX_H

#include "lock/mutex_types.h"

void mutex_lock(mutex_t *);
int mutex_lock_interruptible(mutex_t *);
int mutex_lock_timed(mutex_t *, uint64 timeout_ms);
int mutex_trylock(mutex_t *);
void mutex_unlock(mutex_t *);
int holding_mutex(mutex_t *);
void mutex_init(mutex_t *, char *);

#endif /* __KERNEL_MUTEX_H */
