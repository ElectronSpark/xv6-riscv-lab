#ifndef __KERNEL_PROC_RQ_H
#define __KERNEL_PROC_RQ_H

#include "proc/rq_types.h"

// The priority values are made of two parts:
// - 0bit-4bit: sub-priority (0-31), lower value means higher priority.
//      Managed by specific scheduling class.
// - 5bit-7bit: main priority (0-7), lower value means
//      Managed by rq layer.
//      Each main level corresponds to a sched class.
//      Always prefer lower main priority level when picking next task.
#define PRIORITY_SUBLEVEL_MASK      0x03
#define PRIORITY_MAINLEVEL_MASK     0xFC
#define PRIORITY_MAINLEVEL_SHIFT    2
#define PRIORITY_MAINLEVELS         64
#define MAJOR_PRIORITY(prio)    (((prio) & PRIORITY_MAINLEVEL_MASK) >> PRIORITY_MAINLEVEL_SHIFT)
#define MINOR_PRIORITY(prio)    ((prio) & PRIORITY_SUBLEVEL_MASK)

// #define DEFAULT_MAJOR_PRIORITY   4
#define DEFAULT_MAJOR_PRIORITY   17
#define DEFAULT_MINOR_PRIORITY   0

#define EXIT_MAJOR_PRIORITY      0
#define FIFO_MAJOR_PRIORITY      17
#define IDLE_MAJOR_PRIORITY      63

#define MAKE_PRIORITY(major, minor)  (((major) << PRIORITY_MAINLEVEL_SHIFT) | (minor))
#define IDLE_PRIORITY    MAKE_PRIORITY(IDLE_MAJOR_PRIORITY, DEFAULT_MINOR_PRIORITY)
#define DEFAULT_PRIORITY   MAKE_PRIORITY(DEFAULT_MAJOR_PRIORITY, DEFAULT_MINOR_PRIORITY)

#define GET_RQ_FOR_CURRENT(cls_id)    get_rq_for_cpu((cls_id), cpuid())

struct rq *get_rq_for_cpu(int cls_id, int cpu_id);
struct rq *pick_next_rq(void);
void rq_global_init(void);
void rq_init(struct rq* rq);
void sched_entity_init(struct sched_entity* se, struct proc* p);
void sched_class_register(int id, struct sched_class* cls);
void rq_register(struct rq* rq, int cls_id, int cpu_id);
void rq_lock(int cpu_id);
void rq_unlock(int cpu_id);
void rq_lock_current(void);
void rq_unlock_current(void);
int rq_holding(int cpu_id);
int rq_holding_current(void);

struct rq *rq_select_task_rq(struct sched_entity* se, cpumask_t cpumask);

// Set/Clear the ready status of a scheduling class on a CPU
void rq_set_ready(int cls_id, int cpu_id);
void rq_clear_ready(int cls_id, int cpu_id);

// The following are wrappers around the sched_class callbacks
// Will assume the validity of rq and se
void rq_enqueue_task(struct rq* rq, struct sched_entity* se);
void rq_dequeue_task(struct rq *rq, struct sched_entity* se);
struct sched_entity *rq_pick_next_task(struct rq* rq);
void rq_put_prev_task(struct sched_entity* se);
void rq_set_next_task(struct sched_entity* se);
void rq_task_tick(struct sched_entity* se);
void rq_task_fork(struct sched_entity* se);
void rq_task_dead(struct sched_entity* se);
void rq_yield_task(void);

// Check if a CPU is allowed by the task's affinity mask
bool rq_cpu_allowed(struct sched_entity *se, int cpu_id);

// Scheduler attribute get/set APIs
// These allow getting and setting scheduling parameters for a task.
// Note: time_slice is currently a placeholder and not enforced by the scheduler.
int sched_getattr(struct sched_entity *se, struct sched_attr *attr);
int sched_setattr(struct sched_entity *se, const struct sched_attr *attr);

// Initialize a sched_attr structure with default values
void sched_attr_init(struct sched_attr *attr);

#endif // __KERNEL_PROC_RQ_H
