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

static struct idle_rq *__idle_rqs; // Array of idle_rq structures, one per CPU
static struct rq **__idle_rq_list; // Array of pointers to idle_rq structures

static struct sched_entity *__idle_pick_next_task(struct rq *rq) {
    // The idle rq only has the idle process
    struct idle_rq *idle_rq = container_of(rq, struct idle_rq, rq);
    return idle_rq->idle_proc->sched_entity;
}

static struct sched_class __idle_sched_class = {
    .enqueue_task = NULL,
    .dequeue_task = NULL,
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

    size_t idle_rq_list_size = sizeof(struct rq *) * NCPU;
    __idle_rq_list = (struct rq **)kmm_alloc(idle_rq_list_size);
    if (!__idle_rq_list) {
        panic("alloc_idle_rqs: failed to allocate idle_rq_list\n");
    }
    memset(__idle_rq_list, 0, idle_rq_list_size);

    for (int i = 0; i < NCPU; i++) {
        __idle_rq_list[i] = &__idle_rqs[i].rq;
        rq_init(&__idle_rqs[i].rq, &__idle_sched_class, i);
        __idle_rqs[i].rq.task_count = 0;
        rq_register(&__idle_rqs[i].rq, IDLE_MAJOR_PRIORITY, i);
        rq_set_ready(IDLE_MAJOR_PRIORITY, i);
    }
}

void init_idle_rq(void) {
    sched_class_register(IDLE_MAJOR_PRIORITY, &__idle_sched_class);
    __alloc_idle_rqs();
}
