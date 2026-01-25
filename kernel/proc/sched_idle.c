#include "types.h"
#include "string.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "proc/rq.h"
#include "proc_private.h"
#include "defs.h"
#include "printf.h"
#include "list.h"
#include "slab.h"
#include "page.h"

static struct idle_rq *__idle_rqs; // Array of idle_rq structures, one per CPU

static struct sched_entity *__idle_pick_next_task(struct rq *rq) {
    // The idle rq only has the idle process
    struct idle_rq *idle_rq = container_of(rq, struct idle_rq, rq);
    return idle_rq->idle_proc->sched_entity;
}

static void __idle_enqueue_task(struct rq *rq, struct sched_entity *se) {
    struct idle_rq *idle_rq = container_of(rq, struct idle_rq, rq);
    assert(idle_rq->idle_proc == NULL, "idle_enqueue_task: idle rq already has a process\n");
    idle_rq->idle_proc = se->proc;
    se->rq = rq;
    se->priority = (IDLE_MAJOR_PRIORITY << PRIORITY_MAINLEVEL_SHIFT) | PRIORITY_SUBLEVEL_MASK;
}

static void __idle_dequeue_task(struct rq *rq, struct sched_entity *se) {
    // Idle rq should never have any other task dequeued
    panic("idle_dequeue_task: trying to dequeue task from idle rq\n");
}

static struct sched_class __idle_sched_class = {
    .enqueue_task = __idle_enqueue_task,
    .dequeue_task = __idle_dequeue_task,
    .select_task_rq = NULL,
    .pick_next_task = __idle_pick_next_task,
    .put_prev_task = NULL,
    .set_next_task = NULL,
    .task_tick = NULL,
    .task_fork = NULL,
    .task_dead = NULL,
    .yield_task = NULL,
};

static void __alloc_idle_rqs(void) {
    size_t idle_rq_size = sizeof(struct idle_rq) * NCPU;
    __idle_rqs = (struct idle_rq *)kmm_alloc(idle_rq_size);
    if (!__idle_rqs) {
        panic("alloc_idle_rqs: failed to allocate idle_rqs\n");
    }
    memset(__idle_rqs, 0, idle_rq_size);

    for (int i = 0; i < NCPU; i++) {
        rq_init(&__idle_rqs[i].rq);
        rq_register(&__idle_rqs[i].rq, IDLE_MAJOR_PRIORITY, i);
        rq_set_ready(IDLE_MAJOR_PRIORITY, i);
    }
}

void init_idle_rq(void) {
    sched_class_register(IDLE_MAJOR_PRIORITY, &__idle_sched_class);
    __alloc_idle_rqs();
}
