#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc/proc.h"
#include "proc_private.h"
#include "defs.h"
#include "printf.h"
#include "list.h"
#include "slab.h"
#include "proc/rq.h"
#include "rbtree.h"
#include "atomic.h"
#include "string.h"
#include "bits.h"
#include "errno.h"

// Global run queue structure
// rqs structure:
//   rqs[PRIORITY_MAINLEVELS] (fixed size array, 64 priority levels)
//       |
//       v
//   +-------+-------+-------+-------+-------+-------+-------+-------+
//   |  [0]  |  [1]  |  [2]  | ...                           | [63]  |
//   +-------+-------+-------+-------+-------+-------+-------+-------+
//       |       |       |       ...
//       |       |       +---> NULL or pointer to dynamic array
//       |       +-----------> NULL or pointer to dynamic array
//       v
//   +-------+-------+-------+-------+-------+
//   |  *rq  |  *rq  |  *rq  |  *rq  | ....  |  (size: n-cpus)
//   +-------+-------+-------+-------+-------+
//       |       |       |       |
//       v       v       v       v
//     struct  struct  struct  struct
//       rq      rq      rq      rq
//
// Two-layer ready mask for O(1) lookup of highest priority ready queue:
//   ready_mask (8 bits): Each bit indicates if the corresponding group has any ready tasks.
//       Bit i is set if any priority level in group i (cls_id 8*i to 8*i+7) has tasks.
//   ready_mask_secondary (64 bits): Each bit indicates if that priority level has ready tasks.
//       Organized as 8 groups of 8 bits each, matching ready_mask groups.
//
// Lookup algorithm:
//   1. Find lowest set bit in ready_mask -> top_id (group 0-7)
//   2. Extract 8-bit group from ready_mask_secondary at (top_id * 8)
//   3. Find lowest set bit in that group -> cls_id = top_id*8 + bit_position
//
// sched_class: scheduling class for each main priority level.
//
// rq_lock: Each CPU has a global rq_lock to protect the rqs and ready_mask
static struct rq_global {
    struct rq **rqs[PRIORITY_MAINLEVELS];
    uint64 *ready_mask;
    uint64 *ready_mask_secondary;
    struct sched_class *sched_class[PRIORITY_MAINLEVELS];
    spinlock_t *rq_lock;
} rq_global;


void rq_set_ready(int cls_id, int cpu_id) {
    uint64 top_mask = (1ULL << (cls_id >> 3));
    uint64 secondary_mask = (1ULL << cls_id);
    rq_global.ready_mask[cpu_id] |= top_mask;
    rq_global.ready_mask_secondary[cpu_id] |= secondary_mask;
}

void rq_clear_ready(int cls_id, int cpu_id) {
    int top_id = cls_id >> 3;
    uint64 secondary_mask = (1ULL << cls_id);
    uint64 group_mask = 0xffULL << (top_id << 3);
    uint64 top_mask = (1ULL << top_id);

    // Clear the secondary bit
    rq_global.ready_mask_secondary[cpu_id] &= ~secondary_mask;
    
    // If the group is now empty, clear the top bit
    if ((rq_global.ready_mask_secondary[cpu_id] & group_mask) == 0) {
        rq_global.ready_mask[cpu_id] &= ~top_mask;
    }
}

// Find the lowest set bit index (used for two-layer mask lookup)
#define RQ_PICK_READY_ID(value)         bits_ctz8(value)

#define __sched_class_of_id(cls_id)    (rq_global.sched_class[cls_id])
#define __get_rq_for_cpu(cls_id, cpu_id)    (rq_global.rqs[cls_id] ? rq_global.rqs[cls_id][cpu_id] : NULL)
#define __rq_lock_held(cpu_id)    spin_holding(&rq_global.rq_lock[cpu_id])

struct rq *get_rq_for_cpu(int cls_id, int cpu_id) {
    if (cls_id < 0 || cls_id >= PRIORITY_MAINLEVELS) {
        return ERR_PTR(-EINVAL);
    }
    if (cpu_id < 0 || cpu_id >= NCPU) {
        return ERR_PTR(-EINVAL);
    }
    return __get_rq_for_cpu(cls_id, cpu_id);
}

// Pick the highest priority runnable rq across all CPUs and lock it
struct rq *pick_next_rq(void) {
    int cpu = cpuid();
    uint64 top_mask = rq_global.ready_mask[cpu];
    uint64 secondary_mask = rq_global.ready_mask_secondary[cpu];

    // Two-layer lookup: first find top-level group, then find class within group
    int top_id = RQ_PICK_READY_ID(top_mask);
    if (top_id < 0) {
        // No ready tasks at all - this shouldn't happen as idle should always be ready
        panic("pick_next_rq: no ready tasks on cpu %d\n", cpu);
    }
    uint8 group_bits = (secondary_mask >> (top_id << 3)) & 0xff;
    if (group_bits == 0) {
        // Race: top bit set but group is empty, retry with fresh read
        secondary_mask = rq_global.ready_mask_secondary[cpu];
        group_bits = (secondary_mask >> (top_id << 3)) & 0xff;
        if (group_bits == 0) {
            panic("pick_next_rq: inconsistent ready mask on cpu %d, top_id %d\n", cpu, top_id);
        }
    }
    int cls_id = (top_id << 3) + RQ_PICK_READY_ID(group_bits);

    struct rq *rq = get_rq_for_cpu(cls_id, cpu);
    if (IS_ERR_OR_NULL(rq)) {
        // There should be at least idle rq that are always ready
        panic("pick_next_rq: invalid rq for cls_id %d cpu_id %d\n", cls_id, cpu);
    }
    return rq;
}

void rq_global_init(void) {
    // Allocate and initialize rq spinlocks
    size_t size = sizeof(spinlock_t) * NCPU;
    rq_global.rq_lock = kmm_alloc(size);
    assert(rq_global.rq_lock != NULL, "rq_global_init: failed to allocate rq_lock array");
    for (int i = 0; i < NCPU; i++) {
        spin_init(&rq_global.rq_lock[i], "rq_global_lock");
    }

    // Allocate and initialize ready_mask
    size = sizeof(uint64) * NCPU;
    rq_global.ready_mask = kmm_alloc(size);
    assert(rq_global.ready_mask != NULL, "rq_global_init: failed to allocate ready_mask array");
    memset(rq_global.ready_mask, 0, size);
    rq_global.ready_mask_secondary = kmm_alloc(size);
    assert(rq_global.ready_mask_secondary != NULL, "rq_global_init: failed to allocate ready_mask_secondary array");
    memset(rq_global.ready_mask_secondary, 0, size);

    // Initialize sched class
    for (int i = 0; i < PRIORITY_MAINLEVELS; i++) {
        rq_global.sched_class[i] = NULL;
    }

    // Allocate and initialize rqs array
    size = sizeof(struct rq*) * NCPU;
    for (int i = 0; i < PRIORITY_MAINLEVELS; i++) {
        rq_global.rqs[i] = kmm_alloc(size);
        assert(rq_global.rqs[i] != NULL, "rq_global_init: failed to allocate rqs array");
        memset(rq_global.rqs[i], 0, size);
    }

    // Initialize each individual rq structure is done in rq_register
    init_idle_rq();
    init_fifo_rq();
}

void rq_init(struct rq* rq) {
    assert(rq != NULL, "rq_init: rq is NULL");
    memset(rq, 0, sizeof(struct rq));
    rq->task_count = 0;
}

void rq_register(struct rq* rq, int cls_id, int cpu_id) {
    assert(rq != NULL, "rq_register: rq is NULL");
    assert(cls_id >= 0 && cls_id < PRIORITY_MAINLEVELS, "rq_register: invalid cls_id %d", cls_id);
    assert(cpu_id >= 0 && cpu_id < NCPU, "rq_register: invalid cpu_id %d", cpu_id);
    assert(rq_global.rqs[cls_id][cpu_id] == NULL, "rq_register: rq for cls_id %d cpu_id %d already registered", cls_id, cpu_id);
    rq->class_id = cls_id;
    rq->cpu_id = cpu_id;
    rq->sched_class = __sched_class_of_id(cls_id);
    assert(rq->sched_class != NULL, "rq_init: sched_class is NULL");
    rq_global.rqs[cls_id][cpu_id] = rq;
}

void sched_entity_init(struct sched_entity* se, struct proc* p) {
    assert(se != NULL, "sched_entity_init: se is NULL");
    se->rq = NULL;
    se->priority = DEFAULT_PRIORITY;  // Set default priority for scheduling
    se->sched_class = NULL;
    spin_init(&se->pi_lock, "se_pi_lock");
    se->on_rq = 0;
    se->on_cpu = 0;
    se->cpu_id = -1;
    se->affinity_mask = (1ULL << NCPU) - 1; // all CPUs
    se->start_time = 0;
    se->exec_start = 0;
    se->exec_end = 0;
    se->proc = p;
}

void sched_class_register(int id, struct sched_class* cls) {
    if (id < 0 || id >= PRIORITY_MAINLEVELS) {
        panic("sched_class_register: invalid sched class id %d\n", id);
    }
    if (cls == NULL) {
        panic("sched_class_register: sched class id %d is NULL\n", id);
    }
    if (cls->pick_next_task == NULL) {
        panic("sched_class_register: sched class id %d has no pick_next_task\n", id);
    }
    rq_global.sched_class[id] = cls;
}

void rq_lock(int cpu_id) {
    assert(cpu_id >= 0 && cpu_id < NCPU, "rq_lock: invalid cpu_id %d", cpu_id);
    spin_acquire(&rq_global.rq_lock[cpu_id]);
}

void rq_unlock(int cpu_id) {
    assert(cpu_id >= 0 && cpu_id < NCPU, "rq_unlock: invalid cpu_id %d", cpu_id);
    assert(__rq_lock_held(cpu_id), "rq_unlock: lock not held for cpu_id %d", cpu_id);
    spin_release(&rq_global.rq_lock[cpu_id]);
}

void rq_lock_current(void) {
    push_off();
    rq_lock(cpuid());
    pop_off();
}

void rq_unlock_current(void) {
    push_off();
    int cpu = cpuid();
    pop_off();
    rq_unlock(cpu);
}

int rq_holding(int cpu_id) {
    if (cpu_id < 0 || cpu_id >= NCPU) {
        return 0;
    }
    return __rq_lock_held(cpu_id);
}

int rq_holding_current(void) {
    push_off();
    int holding = spin_holding(&rq_global.rq_lock[cpuid()]);
    pop_off();
    return holding;
}

// Select the appropriate run queue for the given sched_entity and cpumask
// Prefer the current CPU's rq if allowed by cpumask (for locality)
struct rq *rq_select_task_rq(struct sched_entity* se, cpumask_t cpumask) {
    if (se == NULL) {
        return ERR_PTR(-EINVAL);
    }

    int major_prio = se->priority >> PRIORITY_MAINLEVEL_SHIFT;
    if (major_prio < 0 || major_prio >= PRIORITY_MAINLEVELS) {
        return ERR_PTR(-EINVAL);
    }

    if (__sched_class_of_id(major_prio) == NULL) {
        return ERR_PTR(-EINVAL);
    }

    if (__sched_class_of_id(major_prio)->select_task_rq) {
        return rq_global.sched_class[major_prio]->select_task_rq(se->rq, se, cpumask);
    }

    if (cpumask == 0) {
        cpumask = (1ULL << NCPU) - 1; // all CPUs
    }

    // Prefer the current CPU's rq for locality
    int cur_cpu = cpuid();
    if (cpumask & (1ULL << cur_cpu)) {
        struct rq *rq = get_rq_for_cpu(major_prio, cur_cpu);
        if (!IS_ERR_OR_NULL(rq)) {
            return rq;
        }
    }

    // Fallback: find any allowed CPU's rq
    for (int cpu = 0; cpu < NCPU; cpu++) {
        cpumask_t mask = (1ULL << cpu);
        if (cpumask & mask) {
            struct rq *rq = get_rq_for_cpu(major_prio, cpu);
            if (!IS_ERR_OR_NULL(rq)) {
                return rq;
            }
        }
    }
    return NULL;
}

void rq_enqueue_task(struct rq *rq, struct sched_entity *se) {
    assert(__rq_lock_held(rq->cpu_id), "rq_enqueue_task: rq lock not held");
    assert(se->rq == NULL, "rq_enqueue_task: se rq is not NULL");
    if (rq->sched_class->enqueue_task) {
        rq->sched_class->enqueue_task(rq, se);
    }
    se->rq = rq;
    smp_store_release(&se->cpu_id, rq->cpu_id);
    se->sched_class = rq->sched_class;
    rq->task_count++;
    rq_set_ready(rq->class_id, rq->cpu_id);
}

void rq_dequeue_task(struct rq *rq, struct sched_entity* se) {
    assert(__rq_lock_held(rq->cpu_id), "rq_dequeue_task: rq lock not held");
    assert(se->rq == rq, "rq_dequeue_task: se->rq does not match rq");
    assert(rq->task_count > 0, "rq_dequeue_task: rq task_count is zero");
    assert(se->sched_class == se->rq->sched_class,
           "rq_dequeue_task: se->sched_class does not match rq's sched_class\n");
    if (se->sched_class->dequeue_task) {
        se->sched_class->dequeue_task(se->rq, se);
    }
    se->rq = NULL;
    se->sched_class = NULL;
    rq->task_count--;
    if (rq->task_count == 0) {
        rq_clear_ready(rq->class_id, rq->cpu_id);
    }
}

struct sched_entity *rq_pick_next_task(struct rq* rq) {
    assert(__rq_lock_held(rq->cpu_id), "rq_pick_next_task: rq lock not held");
    if (rq->sched_class->pick_next_task) {
        return rq->sched_class->pick_next_task(rq);
    }
    return NULL;
}

void rq_put_prev_task(struct sched_entity* se) {
    assert(se->rq != NULL, "rq_put_prev_task: se->rq is NULL");
    assert(__rq_lock_held(se->rq->cpu_id), "rq_put_prev_task: rq lock not held");
    assert(se->rq->task_count > 0, "rq_put_prev_task: rq task_count is zero");
    assert(se->sched_class == se->rq->sched_class,
           "rq_put_prev_task: se->sched_class does not match rq's sched_class\n");
    if (se->sched_class->put_prev_task) {
        se->sched_class->put_prev_task(se->rq, se);
    }
}

void rq_set_next_task(struct sched_entity* se) {
    assert(se->rq != NULL, "rq_set_next_task: se->rq is NULL");
    assert(__rq_lock_held(se->rq->cpu_id), "rq_set_next_task: rq lock not held");
    assert(se->rq->task_count > 0, "rq_set_next_task: rq task_count is zero");
    assert(se->sched_class == se->rq->sched_class,
           "rq_set_next_task: se->sched_class does not match rq's sched_class\n");
    if (se->sched_class->set_next_task) {
        se->sched_class->set_next_task(se->rq, se);
    }
}

// Check if the current CPU is allowed by the task's affinity mask.
// Returns true if current CPU is allowed, false if migration is needed.
// Note: Migration is handled lazily - when a process sleeps and wakes up,
// rq_select_task_rq() in the wakeup path respects affinity_mask and places
// the task on an allowed CPU. Processes that only yield() without sleeping
// will stay on their current CPU until they sleep.
bool rq_cpu_allowed(struct sched_entity *se, int cpu_id) {
    if (se == NULL) {
        return false;
    }
    return (se->affinity_mask & (1ULL << cpu_id)) != 0;
}

void rq_task_tick(struct sched_entity* se) {
    assert(se->sched_class != NULL, "rq_task_tick: se->sched_class is NULL");
    assert(se->rq != NULL, "rq_task_tick: se->rq is NULL");
    assert(__rq_lock_held(se->rq->cpu_id), "rq_task_tick: rq lock not held");
    assert(se->sched_class == se->rq->sched_class,
           "rq_task_tick: se->sched_class does not match rq's sched_class");
    if (se->sched_class->task_tick) {
        se->sched_class->task_tick(se->rq, se);
    }
}

void rq_task_fork(struct sched_entity* se) {
    // This is called by the parent process's rq when forking a new process
    // Thus the rq and se are of the parent process
    struct sched_entity* current_se = myproc()->sched_entity;

    if (current_se->sched_class && current_se->sched_class->task_fork) {
        current_se->sched_class->task_fork(se->rq, se);
    } else if (__sched_class_of_id(DEFAULT_MAJOR_PRIORITY) != NULL &&
               __sched_class_of_id(DEFAULT_MAJOR_PRIORITY)->task_fork != NULL) {
        // Otherwise, pick default scheduler class if it has task_fork
        __sched_class_of_id(DEFAULT_MAJOR_PRIORITY)->task_fork(se->rq, se);
    }
    // If no task_fork callback, just inherit the priority from default
    // The child will inherit the same sched_class as the parent when enqueued
}

void rq_task_dead(struct sched_entity* se) {
    // When a process is exiting (becoming zombie), it's still on the CPU
    // but not on any run queue. We need to clean up its sched_entity.
    // The process may or may not have an rq set depending on if it was ever scheduled.
    if (se->rq != NULL && se->sched_class != NULL && se->sched_class->task_dead) {
        se->sched_class->task_dead(se->rq, se);
    }
    // Dequeue the task from the run queue if it's still on one
    if (se->rq != NULL) {
        rq_dequeue_task(se->rq, se);
    }
    // Clear the sched_class to mark this process as dead
    se->sched_class = NULL;
}

void rq_yield_task(void) {
    struct rq *current_rq = myproc()->sched_entity->rq;
    assert(current_rq != NULL, "rq_yield_task: current_rq is NULL");
    assert(__rq_lock_held(current_rq->cpu_id), "rq_yield_task: rq lock not held");
    if (current_rq->sched_class->yield_task) {
        current_rq->sched_class->yield_task(current_rq);
    }
}

// Default time slice value (placeholder - not yet enforced by scheduler)
#define DEFAULT_TIME_SLICE 10

// Initialize a sched_attr structure with default values
void sched_attr_init(struct sched_attr *attr) {
    if (attr == NULL) {
        return;
    }
    attr->size = sizeof(struct sched_attr);
    attr->affinity_mask = (1ULL << NCPU) - 1;  // All CPUs
    attr->time_slice = DEFAULT_TIME_SLICE;     // Placeholder - not yet implemented
    attr->priority = DEFAULT_PRIORITY;
    attr->flags = 0;
}

// Get scheduling attributes for a sched_entity
// Returns 0 on success, negative errno on error.
// Caller must NOT hold se->pi_lock - this function acquires it internally.
int sched_getattr(struct sched_entity *se, struct sched_attr *attr) {
    if (se == NULL || attr == NULL) {
        return -EINVAL;
    }
    
    spin_acquire(&se->pi_lock);
    
    attr->size = sizeof(struct sched_attr);
    attr->affinity_mask = se->affinity_mask;
    attr->time_slice = DEFAULT_TIME_SLICE;  // Placeholder - not stored per-task yet
    attr->priority = se->priority;
    attr->flags = 0;
    
    spin_release(&se->pi_lock);
    
    return 0;
}

// Set scheduling attributes for a sched_entity
// Returns 0 on success, negative errno on error.
// Note: time_slice is currently a placeholder and will be ignored.
// Caller must NOT hold se->pi_lock - this function acquires it internally.
int sched_setattr(struct sched_entity *se, const struct sched_attr *attr) {
    if (se == NULL || attr == NULL) {
        return -EINVAL;
    }
    
    // Validate priority range (before acquiring lock)
    int major = MAJOR_PRIORITY(attr->priority);
    if (major < 0 || major >= PRIORITY_MAINLEVELS) {
        return -EINVAL;
    }
    
    // Validate affinity mask - must have at least one valid CPU
    cpumask_t valid_mask = (1ULL << NCPU) - 1;
    if ((attr->affinity_mask & valid_mask) == 0) {
        return -EINVAL;
    }
    
    spin_acquire(&se->pi_lock);
    
    // Apply the new attributes
    // Note: If the task is currently on a run queue, we may need to re-enqueue
    // it to reflect priority changes. For now, changes take effect on next enqueue.
    se->affinity_mask = attr->affinity_mask & valid_mask;
    se->priority = attr->priority;
    
    // time_slice is ignored for now (placeholder)
    // attr->time_slice would be stored when time slice support is implemented
    
    spin_release(&se->pi_lock);
    
    return 0;
}
