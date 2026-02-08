#include "types.h"
#include "string.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "proc/rq.h"
#include "proc_private.h"
#include "defs.h"
#include "printf.h"
#include "list.h"
#include <mm/slab.h>
#include <mm/page.h>
#include "bits.h"
#include "errno.h"
#include <smp/percpu.h>

#ifndef INT_MAX
#define INT_MAX 0x7FFFFFFF
#endif

// fifo_rqs[cls_id] -> array of fifo_rq per CPU
// Only entries for registered cls_ids are non-NULL
static struct fifo_rq *__fifo_rqs[PRIORITY_MAINLEVELS];

// Get minor priority index (0-3) from sched_entity
static inline int __fifo_minor_prio(struct sched_entity *se) {
    return MINOR_PRIORITY(se->priority);
}

// Get subqueue for a given minor priority
static inline struct fifo_subqueue *__fifo_get_subqueue(struct fifo_rq *fifo_rq, int minor_prio) {
    return &fifo_rq->subqueues[minor_prio];
}

static struct sched_entity *__fifo_pick_next_task(struct rq *rq) {
    struct fifo_rq *fifo_rq = container_of(rq, struct fifo_rq, rq);
    if (fifo_rq->ready_mask == 0) {
        return NULL;
    }
    // Find highest priority (lowest index) non-empty subqueue
    int idx = bits_ctz8(fifo_rq->ready_mask);
    struct fifo_subqueue *sq = __fifo_get_subqueue(fifo_rq, idx);
    return LIST_FIRST_NODE(&sq->head, struct sched_entity, list_entry);
}

static void __fifo_enqueue_task(struct rq *rq, struct sched_entity *se) {
    struct fifo_rq *fifo_rq = container_of(rq, struct fifo_rq, rq);
    int idx = __fifo_minor_prio(se);
    struct fifo_subqueue *sq = __fifo_get_subqueue(fifo_rq, idx);
    list_node_push(&sq->head, se, list_entry);
    sq->count++;
    fifo_rq->ready_mask |= (1 << idx);
}

static void __fifo_dequeue_task(struct rq *rq, struct sched_entity *se) {
    struct fifo_rq *fifo_rq = container_of(rq, struct fifo_rq, rq);
    int idx = __fifo_minor_prio(se);
    struct fifo_subqueue *sq = __fifo_get_subqueue(fifo_rq, idx);
    list_node_detach(se, list_entry);
    sq->count--;
    // Clear ready bit if subqueue is now empty
    if (sq->count == 0) {
        fifo_rq->ready_mask &= ~(1 << idx);
    }
}

static void __fifo_put_prev_task(struct rq *rq, struct sched_entity *se) {
    // put_prev_task re-adds the previously running task back to the list.
    // We update the list and masks to reflect the list state.
    // Counts (sq->count, rq->task_count) are NOT changed - task was always logically "on rq".
    struct fifo_rq *fifo_rq = container_of(rq, struct fifo_rq, rq);
    int idx = __fifo_minor_prio(se);
    struct fifo_subqueue *sq = __fifo_get_subqueue(fifo_rq, idx);
    
    list_node_push(&sq->head, se, list_entry);
    
    // Set the subqueue ready bit (subqueue now has at least one task in list)
    fifo_rq->ready_mask |= (1 << idx);
    
    // Set global ready masks (rq now has at least one task in list)
    rq_set_ready(rq->class_id, rq->cpu_id);
}

static void __fifo_set_next_task(struct rq *rq, struct sched_entity *se) {
    // set_next_task is called when picking a task to run.
    // We detach from the list and update masks to reflect the list state.
    // Counts (sq->count, rq->task_count) are NOT changed - task is still logically "on rq".
    struct fifo_rq *fifo_rq = container_of(rq, struct fifo_rq, rq);
    int idx = __fifo_minor_prio(se);
    struct fifo_subqueue *sq = __fifo_get_subqueue(fifo_rq, idx);
    
    list_node_detach(se, list_entry);
    
    // If this was the last task in the subqueue list, clear the subqueue ready bit
    if (sq->count == 1) {
        fifo_rq->ready_mask &= ~(1 << idx);
    }
    
    // If this was the last task in the rq list, clear global ready masks
    if (rq->task_count == 1) {
        rq_clear_ready(rq->class_id, rq->cpu_id);
    }
}

// Select the best CPU's run queue for a task
// Strategy: Pick the CPU with the lowest task count in the relevant subqueue
// to achieve load balancing across CPUs.
static struct rq *__fifo_select_task_rq(struct rq *prev_rq, struct sched_entity *se, cpumask_t cpumask) {
    int major_prio = MAJOR_PRIORITY(se->priority);
    int minor_prio = MINOR_PRIORITY(se->priority);
    
    if (cpumask == 0) {
        cpumask = (1ULL << NCPU) - 1;  // All CPUs allowed
    }
    
    // If the FIFO rqs for this major priority are not allocated, return error
    if (__fifo_rqs[major_prio] == NULL) {
        return ERR_PTR(-EINVAL);
    }
    
    // Prefer current CPU for cache locality if allowed
    int cur_cpu = cpuid();
    struct rq *best_rq = NULL;
    int best_count = INT_MAX;
    
    // First check current CPU (prefer for locality)
    if (cpumask & (1ULL << cur_cpu)) {
        struct fifo_rq *fifo_rq = &__fifo_rqs[major_prio][cur_cpu];
        struct fifo_subqueue *sq = __fifo_get_subqueue(fifo_rq, minor_prio);
        best_rq = &fifo_rq->rq;
        best_count = sq->count;
        
        // If current CPU's subqueue is empty, use it immediately (optimal locality)
        if (best_count == 0) {
            return best_rq;
        }
    }
    
    // Check all other allowed CPUs for a less loaded one
    for (int cpu = 0; cpu < NCPU; cpu++) {
        if (cpu == cur_cpu) {
            continue;  // Already checked
        }
        if (!(cpumask & (1ULL << cpu))) {
            continue;  // Not allowed
        }
        
        struct fifo_rq *fifo_rq = &__fifo_rqs[major_prio][cpu];
        struct fifo_subqueue *sq = __fifo_get_subqueue(fifo_rq, minor_prio);
        
        if (sq->count < best_count) {
            best_rq = &fifo_rq->rq;
            best_count = sq->count;
            
            // If we found an empty subqueue, use it immediately
            if (best_count == 0) {
                return best_rq;
            }
        }
    }
    
    if (best_rq == NULL) {
        return ERR_PTR(-ENOENT);
    }
    
    return best_rq;
}

static struct sched_class __fifo_sched_class = {
    .enqueue_task = __fifo_enqueue_task,
    .dequeue_task = __fifo_dequeue_task,
    .select_task_rq = __fifo_select_task_rq,
    .pick_next_task = __fifo_pick_next_task,
    .put_prev_task = __fifo_put_prev_task,
    .set_next_task = __fifo_set_next_task,
    .task_tick = NULL,
    .task_fork = NULL,
    .task_dead = NULL,
    .yield_task = NULL,
};

static void __fifo_subqueue_init(struct fifo_subqueue *sq) {
    list_entry_init(&sq->head);
    sq->count = 0;
}

static void __fifo_rq_init(struct fifo_rq *fifo_rq, int cls_id, int cpu_id) {
    for (int i = 0; i < FIFO_RQ_SUBLEVELS; i++) {
        __fifo_subqueue_init(&fifo_rq->subqueues[i]);
    }
    fifo_rq->ready_mask = 0;
    rq_init(&fifo_rq->rq);
    rq_clear_ready(cls_id, cpu_id);
}

// Allocate and register FIFO rqs for a single major priority level
static void __alloc_fifo_rqs_for_cls(int cls_id) {
    size_t fifo_rq_size = sizeof(struct fifo_rq) * NCPU;
    __fifo_rqs[cls_id] = (struct fifo_rq *)kmm_alloc(fifo_rq_size);
    if (!__fifo_rqs[cls_id]) {
        panic("alloc_fifo_rqs: failed to allocate fifo_rqs for cls_id %d\n", cls_id);
    }
    memset(__fifo_rqs[cls_id], 0, fifo_rq_size);
    for (int i = 0; i < NCPU; i++) {
        __fifo_rq_init(&__fifo_rqs[cls_id][i], cls_id, i);
        rq_register(&__fifo_rqs[cls_id][i].rq, cls_id, i);
    }
}

// Register FIFO scheduler for a range of major priority levels [start, end)
void init_fifo_rq_range(int start_cls_id, int end_cls_id) {
    for (int cls_id = start_cls_id; cls_id < end_cls_id; cls_id++) {
        sched_class_register(cls_id, &__fifo_sched_class);
        __alloc_fifo_rqs_for_cls(cls_id);
    }
}

void init_fifo_rq(void) {
    // Register FIFO scheduler for the default major priority level only
    init_fifo_rq_range(1, IDLE_MAJOR_PRIORITY);
}
