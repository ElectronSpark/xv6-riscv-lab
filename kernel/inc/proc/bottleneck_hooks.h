#ifndef __KERNEL_BOTTLENECK_HOOKS_H
#define __KERNEL_BOTTLENECK_HOOKS_H

#include "proc/thread.h"
#include "proc/bottleneck_trace.h"

struct bt_cause_scope {
    uint64 id;
    uint64 generation;
    int depth;
    bool active;
};

/* Scope writes/restores are IRQ-atomic, but the scope does not keep IRQs
 * disabled. A nested IRQ may establish its own scope and then restore this
 * one; scheduler hooks accept a cause only at the recorded interrupt depth.
 * The caller must leave the scope on the same thread and nesting level. */
static inline struct bt_cause_scope bt_cause_enter(uint64 id,
                                                   uint64 generation)
{
    struct bt_cause_scope saved = {0};

    if (id == 0 || !bt_enabled())
        return saved;
    push_off();
    struct cpu_local *cpu = mycpu();
    struct thread *p = cpu->proc;
    if (p != NULL) {
        saved.id = p->bt_cause_id;
        saved.generation = p->bt_cause_generation;
        saved.depth = p->bt_cause_depth;
        saved.active = true;
        p->bt_cause_id = id;
        p->bt_cause_generation = generation;
        p->bt_cause_depth = cpu->intr_depth;
    }
    pop_off();
    return saved;
}

static inline void bt_cause_leave(struct bt_cause_scope saved)
{
    if (!saved.active)
        return;
    push_off();
    struct thread *p = mycpu()->proc;
    if (p != NULL) {
        p->bt_cause_id = saved.id;
        p->bt_cause_generation = saved.generation;
        p->bt_cause_depth = saved.depth;
    }
    pop_off();
}

/* BT_WAKE_COMMIT: d low 32 bits is the thread state; high 32 bits is this
 * path. Wake-list handoff does not establish final run-queue admission.
 * BT_WAKE has no path bits and records the attempt before state guards. */
enum bt_wake_commit_path {
    BT_WAKE_COMMIT_DIRECT = 1,
    BT_WAKE_COMMIT_WAKELIST = 2,
    BT_WAKE_COMMIT_QUEUED = 3,
    BT_WAKE_COMMIT_SELF = 4,
};

#endif
