/*
 * pgroup.c - Process group operations
 *
 * Provides setpgid/getpgid and process-group-wide signal delivery.
 * The pgid field in struct thread is protected by pid_lock (rwlock).
 */

#include "types.h"
#include "param.h"
#include "errno.h"
#include "printf.h"
#include "lock/spinlock.h"
#include "lock/rcu.h"
#include "proc/thread.h"
#include "proc/thread_group.h"
#include "proc/pgroup.h"
#include "tty/session.h"
#include "proc_private.h"
#include "signal.h"
#include "smp/percpu.h"

/* ------------------------------------------------------------------ */
/*  Initialisation                                                    */
/* ------------------------------------------------------------------ */

/*
 * Called from thread_clone() for the newly created child.
 * Inherits the parent's pgid and sid.
 * Caller must hold pid_wlock.
 */
void pgroup_init_thread(struct thread *child, struct thread *parent) {
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->session = parent->session;
    if (child->session)
        session_ref(child->session);
}

/* ------------------------------------------------------------------ */
/*  setpgid / getpgid                                                 */
/* ------------------------------------------------------------------ */

int pgroup_setpgid(pid_t pid, pid_t pgid) {
    struct thread *p = current;
    struct thread *target = NULL;
    pid_t tgid;

    /* pid == 0 means self */
    tgid = thread_tgid(p);
    if (pid == 0)
        pid = tgid;

    /* pgid == 0 means use the target's pid as the new pgid */
    if (pgid == 0)
        pgid = pid;

    if (pgid < 0)
        return -EINVAL;

    pid_wlock();

    /* Find the target thread */
    rcu_read_lock();
    int err = get_pid_thread(pid, &target);
    rcu_read_unlock();

    if (err < 0 || target == NULL) {
        pid_wunlock();
        return -ESRCH;
    }

    /* Must be self or a child of self */
    if (target != p && target->parent != p) {
        pid_wunlock();
        return -ESRCH;
    }

    /* Cannot change session leader's pgid */
    pid_t target_tgid = thread_tgid(target);
    if (target->sid == target_tgid) {
        pid_wunlock();
        return -EPERM;
    }

    /* Must be in the same session */
    if (target->sid != p->sid) {
        pid_wunlock();
        return -EPERM;
    }

    target->pgid = pgid;

    pid_wunlock();
    return 0;
}

pid_t pgroup_getpgid(pid_t pid) {
    if (pid == 0)
        return current->pgid;

    if (pid < 0)
        return -EINVAL;

    struct thread *target = NULL;
    pid_t pgid;

    rcu_read_lock();
    int err = get_pid_thread(pid, &target);
    if (err < 0 || target == NULL) {
        rcu_read_unlock();
        return -ESRCH;
    }
    pgid = target->pgid;
    rcu_read_unlock();

    return pgid;
}

/* ------------------------------------------------------------------ */
/*  Process-group-wide signal delivery                                */
/* ------------------------------------------------------------------ */

struct pgroup_kill_ctx {
    pid_t pgid;
    int signum;
    int count; /* number of processes signalled */
};

static void __pgroup_kill_visitor(struct thread *p, void *arg) {
    struct pgroup_kill_ctx *ctx = (struct pgroup_kill_ctx *)arg;

    /* Only signal thread-group leaders to avoid duplicates */
    if (!thread_is_group_leader(p))
        return;

    if (p->pgid != ctx->pgid)
        return;

    kill_proc(p, ctx->signum);
    ctx->count++;
}

int pgroup_kill(pid_t pgid, int signum) {
    struct pgroup_kill_ctx ctx = {
        .pgid = pgid,
        .signum = signum,
        .count = 0,
    };

    pid_rlock();
    proctab_for_each_rcu(__pgroup_kill_visitor, &ctx);
    pid_runlock();

    return ctx.count > 0 ? 0 : -ESRCH;
}
