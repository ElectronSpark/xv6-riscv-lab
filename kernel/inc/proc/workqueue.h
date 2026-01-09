#ifndef __KERNEL_WORKQUEUE_H
#define __KERNEL_WORKQUEUE_H

#include "proc/workqueue_types.h"

#define MAX_WORKQUEUE_ACTIVE 64
#define WORKQUEUE_DEFAULT_MAX_ACTIVE 8
#define WORKQUEUE_DEFAULT_MIN_ACTIVE 2

void workqueue_init(void);
struct workqueue *workqueue_create(const char *name, int max_active);
bool queue_work(struct workqueue *wq, struct work_struct *work);

void init_work_struct(struct work_struct *work, void (*func)(struct work_struct*), uint64 data);
struct work_struct *create_work_struct(void (*func)(struct work_struct*), uint64 data);
void free_work_struct(struct work_struct *work);

#endif // __KERNEL_WORKQUEUE_H
