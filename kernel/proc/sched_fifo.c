#include "types.h"
#include "string.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "proc/rq.h"
#include "proc_private.h"
#include "defs.h"
#include "printf.h"
#include "list.h"
#include "slab.h"
#include "page.h"

static struct fifo_rq *__fifo_rqs; // Array of fifo_rq structures, one per CPU

static struct sched_entity *__fifo_pick_next_task(struct rq *rq) {
    // The fifo rq only has the fifo process
    struct fifo_rq *fifo_rq = container_of(rq, struct fifo_rq, rq);
    return LIST_FIRST_NODE(&fifo_rq->run_queue, struct sched_entity, list_entry);
}

static void __fifo_enqueue_task(struct rq *rq, struct sched_entity *se) {
    struct fifo_rq *fifo_rq = container_of(rq, struct fifo_rq, rq);
    list_node_push(&fifo_rq->run_queue, se, list_entry);
}

static void __fifo_dequeue_task(struct rq *rq, struct sched_entity *se) {
    // struct fifo_rq *fifo_rq = container_of(rq, struct fifo_rq, rq);
    list_node_detach(se, list_entry);
}

static void __fifo_put_prev_task(struct rq *rq, struct sched_entity *se) {
    struct fifo_rq *fifo_rq = container_of(rq, struct fifo_rq, rq);
    list_node_push(&fifo_rq->run_queue, se, list_entry);
}

static void __fifo_set_next_task(struct rq *rq, struct sched_entity *se) {
    list_node_detach(se, list_entry);
}

// static void __fifo_fork_task(struct rq *rq, struct sched_entity *se) {
//     // No special handling needed for FIFO on fork
// }

static struct sched_class __fifo_sched_class = {
    .enqueue_task = __fifo_enqueue_task,
    .dequeue_task = __fifo_dequeue_task,
    .select_task_rq = NULL,
    .pick_next_task = __fifo_pick_next_task,
    .put_prev_task = __fifo_put_prev_task,
    .set_next_task = __fifo_set_next_task,
    .task_tick = NULL,
    .task_fork = NULL,
    .task_dead = NULL,
    .yield_task = NULL,
};

static void __fifo_rq_init(struct fifo_rq *fifo_rq, int cpu_id) {
    rq_init(&fifo_rq->rq);
    list_entry_init(&fifo_rq->run_queue);
    rq_clear_ready(FIFO_MAJOR_PRIORITY, cpu_id);
}

static void __alloc_fifo_rqs(int cls_id) {
    size_t fifo_rq_size = sizeof(struct fifo_rq) * NCPU;
    __fifo_rqs = (struct fifo_rq *)kmm_alloc(fifo_rq_size);
    if (!__fifo_rqs) {
        panic("alloc_fifo_rqs: failed to allocate fifo_rqs\n");
    }
    memset(__fifo_rqs, 0, fifo_rq_size);
    for (int i = 0; i < NCPU; i++) {
        __fifo_rq_init(&__fifo_rqs[i], i);
        rq_register(&__fifo_rqs[i].rq, cls_id, i);
        rq_clear_ready(cls_id, i);
    }
}

void init_fifo_rq(void) {
    sched_class_register(FIFO_MAJOR_PRIORITY, &__fifo_sched_class);
    __alloc_fifo_rqs(FIFO_MAJOR_PRIORITY);
}
