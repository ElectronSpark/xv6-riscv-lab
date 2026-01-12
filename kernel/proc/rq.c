#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc/proc.h"
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
//   rqs[PRIORITY_MAINLEVELS] (fixed size array)
//       |
//       v
//   +-------+-------+-------+-------+-------+-------+-------+-------+
//   |  [0]  |  [1]  |  [2]  |  [3]  |  [4]  |  [5]  |  [6]  |  [7]  |
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
// ready_mask: indicates which main priority levels have runnable tasks.
//      Only 8 bits are used.
//
// sched_class: scheduling class for each main priority level.
//
// rq_lock: Each CPU has a global rq_lock to protect the rqs and ready_mask
static struct rq_global {
    struct rq **rqs[PRIORITY_MAINLEVELS];
    uint64 *ready_mask;
    struct sched_class *sched_class[PRIORITY_MAINLEVELS];
    spinlock_t *rq_lock;
} rq_global;


void rq_set_ready(int cls_id, int cpu_id) {
    atomic_or(&rq_global.ready_mask[cpu_id], (1 << (cls_id)));
}

void rq_clear_ready(int cls_id, int cpu_id) {
    atomic_and(&rq_global.ready_mask[cpu_id], ~(1 << (cls_id)));
}

#define RQ_PICK_READY_ID(value)         bits_ctz8(value)

#define __sched_class_of_id(cls_id)    (rq_global.sched_class[cls_id])
#define __get_rq_for_cpu(cls_id, cpu_id)    (rq_global.rqs[cls_id] ? rq_global.rqs[cls_id][cpu_id] : NULL)

struct rq *get_rq_for_cpu(int cls_id, int cpu_id) {
    if (cls_id < 0 || cls_id >= PRIORITY_MAINLEVELS) {
        return ERR_PTR(-EINVAL);
    }
    return __get_rq_for_cpu(cls_id, cpu_id);
}

// Pick the highest priority runnable rq across all CPUs and lock it
struct rq *pick_next_rq(void) {
    int idx = 0;

    idx = RQ_PICK_READY_ID(rq_global.ready_mask[cpuid()]);
    struct rq *rq = get_rq_for_cpu(idx, cpuid());
    if (IS_ERR_OR_NULL(rq)) {
        // There should be at least idle rq that are always ready
        panic("pick_next_rq: invalid rq for cls_id %d cpu_id %ld\n", idx, cpuid());
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
}

void rq_init(struct rq* rq, struct sched_class* sched_class) {
    assert(rq != NULL, "rq_init: rq is NULL");
    assert(sched_class != NULL, "rq_init: sched_class is NULL");
    rq->sched_class = sched_class;
    rq->task_count = 0;
}

void sched_entity_init(struct sched_entity* se, struct proc* p) {
    assert(se != NULL, "sched_entity_init: se is NULL");
    se->rq = NULL;
    se->priority = -1;
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
    spin_acquire(&rq_global.rq_lock[cpu_id]);
}

void rq_unlock(int cpu_id) {
    spin_release(&rq_global.rq_lock[cpu_id]);
}

void rq_lock_current(void) {
    push_off();
    rq_lock(cpuid());
    pop_off();
}

void rq_unlock_current(void) {
    // Assumes preemption is disabled
    rq_unlock(cpuid());
}

// Select the appropriate run queue for the given sched_entity and cpumask
// @TODO: by now it just returns the current rq, or the corresponding rq in cpumask
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

    struct rq *selected = NULL;

    for (int cpu = 0; cpu < NCPU; cpu++) {
        cpumask_t mask = (1ULL << cpu);
        if (mask > cpumask) {
            break;
        }
        if (cpumask & mask) {
            struct rq *rq = get_rq_for_cpu(major_prio, cpu);
            if (IS_ERR_OR_NULL(rq)) {
                continue;
            }
            if (selected == NULL || rq->task_count < selected->task_count) {
                selected = rq;
            }
        }
    }
    return selected;
}

void rq_enqueue_task(struct rq *rq, struct sched_entity *se) {
    if (rq->sched_class->enqueue_task) {
        rq->sched_class->enqueue_task(rq, se);
    }
}

void rq_dequeue_task(struct sched_entity* se) {
    if (se->rq->sched_class->dequeue_task) {
        se->rq->sched_class->dequeue_task(se->rq, se);
    }
}

struct sched_entity *rq_pick_next_task(struct rq* rq) {
    if (rq->sched_class->pick_next_task) {
        return rq->sched_class->pick_next_task(rq);
    }
    return NULL;
}

void rq_put_prev_task(struct sched_entity* se) {
    if (se->rq->sched_class->put_prev_task) {
        se->rq->sched_class->put_prev_task(se->rq, se);
    }
}

void rq_set_next_task(struct sched_entity* se) {
    if (se->rq->sched_class->set_next_task) {
        se->rq->sched_class->set_next_task(se->rq, se);
    }
}

void rq_task_tick(struct sched_entity* se) {
    if (se->rq->sched_class->task_tick) {
        se->rq->sched_class->task_tick(se->rq, se);
    }
}

void rq_task_fork(struct sched_entity* se) {
    // This is called by the parent process's rq when forking a new process
    // Thus the rq and se are of the parent process
    struct sched_entity* current_se = myproc()->sched_entity;

    if (current_se->sched_class->task_fork) {
        current_se->sched_class->task_fork(se->rq, se);
    } else {
        // Otherwise, pick default scheduler class
        assert(__sched_class_of_id(DEFAULT_MAJOR_PRIORITY) != NULL,
               "rq_task_fork: default sched class is NULL");
        __sched_class_of_id(DEFAULT_MAJOR_PRIORITY)->task_fork(se->rq, se);
    }
}

void rq_task_dead(struct sched_entity* se) {
    if (se->rq->sched_class->task_dead) {
        se->rq->sched_class->task_dead(se->rq, se);
    }
}

void rq_yield_task(void) {
    struct rq *current_rq = myproc()->sched_entity->rq;
    if (current_rq->sched_class->yield_task) {
        current_rq->sched_class->yield_task(current_rq);
    }
}
