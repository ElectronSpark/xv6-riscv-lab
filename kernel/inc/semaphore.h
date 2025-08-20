#ifndef __KERNEL_SEMAPHORE_H
#define __KERNEL_SEMAPHORE_H

#include "proc_queue_type.h"
#include "semaphore_types.h"

int sem_init(sem_t *sem, const char *name, int value);
int sem_wait(sem_t *sem);
int sem_trywait(sem_t *sem);
int sem_post(sem_t *sem);
int sem_getvalue(sem_t *sem, int *value);

#endif /* __KERNEL_SEMAPHORE_H */
