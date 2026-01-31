#ifndef __KERNEL_PROC_RQ_H
#define __KERNEL_PROC_RQ_H

#include "proc/rq_types.h"

// The priority values are made of two parts:
// - 0bit-1bit: sub-priority (0-3), lower value means higher priority.
//      Managed by specific scheduling class (e.g., FIFO has 4 sub-queues).
// - 2bit-7bit: main priority (0-63), lower value means higher priority.
//      Managed by rq layer using two-layer bitmask.
//      Each main level corresponds to a sched class.
//      Always prefer lower main priority level when picking next task.
// PRIORITY_MAINLEVELS is defined in rq_types.h (needed for struct rq_percpu)
#define PRIORITY_SUBLEVEL_MASK      0x03
#define PRIORITY_MAINLEVEL_MASK     0xFC
#define PRIORITY_MAINLEVEL_SHIFT    2
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

#define EEVDF_MAJOR_PRIORITY_START  20
#define EEVDF_MAJOR_PRIORITY_LIMIT  30
#define EEVDF_PRIORITY_START  MAKE_PRIORITY(EEVDF_MAJOR_PRIORITY_START, 0)
#define EEVDF_PRIORITY_LIMIT  MAKE_PRIORITY(EEVDF_MAJOR_PRIORITY_LIMIT, 0)
// Check if a priority is within the EEVDF range
#define IS_EEVDF_PRIORITY(prio)  \
    (((prio) >= EEVDF_PRIORITY_START) && ((prio) < EEVDF_PRIORITY_LIMIT))

#define GET_RQ_FOR_CURRENT(cls_id)    get_rq_for_cpu((cls_id), cpuid())

struct rq *get_rq_for_cpu(int cls_id, int cpu_id);
struct rq *pick_next_rq(void);
void rq_global_init(void);
void rq_init(struct rq* rq);
void sched_entity_init(struct sched_entity* se, struct proc* p);
void sched_class_register(int id, struct sched_class* cls);
void rq_register(struct rq* rq, int cls_id, int cpu_id);
void rq_lock(int cpu_id);
int rq_trylock(int cpu_id);
void rq_unlock(int cpu_id);
void rq_lock_current(void);
void rq_unlock_current(void);
int rq_lock_irqsave(int cpu_id);
void rq_unlock_irqrestore(int cpu_id, int state);
int rq_lock_current_irqsave(void);
void rq_unlock_current_irqrestore(int state);
void rq_lock_two(int cpu_id1, int cpu_id2);
int rq_trylock_two(int cpu_id1, int cpu_id2);
void rq_unlock_two(int cpu_id1, int cpu_id2);
int rq_holding(int cpu_id);
int rq_holding_current(void);

// Per-CPU run queue lock_get/put_unlock accessors
struct rq_percpu *rq_percpu_lock_get(int cpu_id);
struct rq_percpu *rq_percpu_lock_get_current(void);
void rq_percpu_put_unlock(struct rq_percpu *rq_pc);

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

bool rq_cpu_is_idle(int cpu_id);
int rq_add_wake_list(int cpu_id, struct sched_entity *se);
struct sched_entity *rq_pop_all_wake_list(struct rq_percpu *rq_pc);
void rq_flush_wake_list(int cpu_id);

// Check if a CPU is allowed by the task's affinity mask
bool rq_cpu_allowed(struct sched_entity *se, int cpu_id);

// Scheduler attribute get/set APIs
// These allow getting and setting scheduling parameters for a task.
// Note: time_slice is currently a placeholder and not enforced by the scheduler.
int sched_getattr(struct sched_entity *se, struct sched_attr *attr);
int sched_setattr(struct sched_entity *se, const struct sched_attr *attr);

// Initialize a sched_attr structure with default values
void sched_attr_init(struct sched_attr *attr);

// Dump run queue info to console
void rq_dump(void);

// Mark a CPU as active in the rq subsystem
void rq_cpu_activate(int cpu);

// Get the bitmask of active CPUs
uint64 rq_get_active_cpu_mask(void);

#endif // __KERNEL_PROC_RQ_H
