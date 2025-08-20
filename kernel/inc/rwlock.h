#ifndef KERNEL_RWLOCK_H
#define KERNEL_RWLOCK_H

#include "rwlock_types.h"

int rwlock_init(rwlock_t *lock, uint64 flags, const char *name);
int rwlock_acquire_read(rwlock_t *lock);
int rwlock_acquire_write(rwlock_t *lock);
void rwlock_release(rwlock_t *lock);

#endif // KERNEL_RWLOCK_H
