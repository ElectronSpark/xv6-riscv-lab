#ifndef KERNEL_RWLOCK_H
#define KERNEL_RWLOCK_H

#include "rwsem_types.h"

int rwsem_init(rwsem_t *lock, uint64 flags, const char *name);
int rwsem_acquire_read(rwsem_t *lock);
int rwsem_acquire_write(rwsem_t *lock);
void rwsem_release(rwsem_t *lock);
bool rwsem_is_write_holding(rwsem_t *lock);

#endif // KERNEL_RWLOCK_H
