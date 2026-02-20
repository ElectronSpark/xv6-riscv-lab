#ifndef __KERNEL_WORKQUEUE_H
#define __KERNEL_WORKQUEUE_H

#include "proc/workqueue_types.h"

#define MAX_WORKQUEUE_ACTIVE 64
#define WORKQUEUE_DEFAULT_MAX_ACTIVE 8
#define WORKQUEUE_DEFAULT_MIN_ACTIVE 2

#define WORK_STRUCT_FLAG_RUN_ON_DRAIN (1U << 0)
#define WORK_STRUCT_FLAG_FREE_AFTER_RUN (1U << 1)

#define WORK_STRUCT_FLAG_VALID_MASK                                           \
    (WORK_STRUCT_FLAG_RUN_ON_DRAIN | WORK_STRUCT_FLAG_FREE_AFTER_RUN)

#define WORK_STRUCT_DEFAULT_FLAGS WORK_STRUCT_FLAG_RUN_ON_DRAIN

void workqueue_init(void);
void workqueue_runtime_smoke_test(void);
struct workqueue *workqueue_create(const char *name, int max_active);
struct workqueue *
workqueue_create_with_callbacks(const char *name, int max_active,
                                const struct workqueue_callbacks *callbacks);
int workqueue_kill(struct workqueue *wq);
bool queue_work(struct workqueue *wq, struct work_struct *work);

void init_work_struct(struct work_struct *work,
                      void (*func)(struct work_struct *), uint64 data);
void init_work_struct_flags(struct work_struct *work,
                            void (*func)(struct work_struct *), uint64 data,
                            uint32 flags);
void init_work_struct_ex(struct work_struct *work,
                         void (*func)(struct work_struct *),
                         void (*fault)(struct work_struct *), uint64 data,
                         uint32 flags);
struct work_struct *create_work_struct(void (*func)(struct work_struct *),
                                       uint64 data);
struct work_struct *create_work_struct_flags(void (*func)(struct work_struct *),
                                             uint64 data, uint32 flags);
struct work_struct *create_work_struct_ex(void (*func)(struct work_struct *),
                                          void (*fault)(struct work_struct *),
                                          uint64 data, uint32 flags);
void free_work_struct(struct work_struct *work);

#endif // __KERNEL_WORKQUEUE_H
