/*
 * pgroup.c - Process group operations (POSIX)
 *
 * A process group is a collection of thread groups (processes) used for
 * job control.  Each process group belongs to exactly one session.
 *
 * Hierarchy:  session → pgroup → thread_group → thread
 *
 * All hierarchy pointers (pgroup, pgid, session, sid) are protected by
 * the global pid_lock (rwlock):
 *   - pid_wlock for mutations (setpgid, add/remove)
 *   - pid_rlock for reads and signal delivery
 *
 * Lock ordering:  pid_lock > sigacts.lock > tcb_lock
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
#include "mm/slab.h"
#include "string.h"
#include "list.h"

static slab_cache_t __pgroup_slab_cache = {0};

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

/*
 * Called each time a thread group is removed from a process group.
 * If the process group is empty (no thread groups), remove it from
 * its session and free it.
 * Caller must hold pid_wlock.
 */
static void __pgroup_cleanup(struct pgroup *pg) {
    if (pg == NULL)
        return;
    if (pg->p_cnt > 0)
        return; /* Still has thread groups — keep alive */
    if (pg->is_kernel)
        return;

    pg->exited = 1;

    /* No thread groups left — detach from session and free */
    if (pg->session != NULL) {
        session_remove_pg(pg->session, pg);
    }
    if (!LIST_ENTRY_IS_DETACHED(&pg->list_entry)) {
        list_entry_detach(&pg->list_entry);
    }
    slab_free(pg);
}

/* ------------------------------------------------------------------ */
/*  Allocation                                                         */
/* ------------------------------------------------------------------ */

struct pgroup *pgroup_alloc(pid_t pgid, struct thread_group *leader) {
    struct pgroup *pg = slab_alloc(&__pgroup_slab_cache);
    if (pg == NULL)
        return NULL;
    memset(pg, 0, sizeof(*pg));
    pg->pgid = pgid;
    pg->leader = leader;
    list_entry_init(&pg->list_entry);
    list_entry_init(&pg->threads);
    list_entry_init(&pg->thread_groups);
    pg->t_cnt = 0;
    pg->p_cnt = 0;
    pg->exited = 0;
    pg->session = NULL;
    return pg;
}

void pgroup_init(struct thread *initproc) {
    int ret = slab_cache_init(&__pgroup_slab_cache, "pgroup_cache",
                              sizeof(struct pgroup), SLAB_FLAG_EMBEDDED);
    assert(ret == 0, "Failed to initialize pgroup slab cache");

    /* Create process group 1 for the init process */
    struct thread_group *tg = initproc->thread_group;
    assert(tg != NULL, "pgroup_init: initproc has no thread_group");

    struct pgroup *pg = pgroup_alloc(initproc->pid, tg);
    assert(pg != NULL, "pgroup_init: pgroup_alloc failed");

    pgroup_add_tg(pg, tg);
    pgroup_add_thread(pg, initproc);
}

/* ------------------------------------------------------------------ */
/*  Thread membership                                                  */
/* ------------------------------------------------------------------ */

/*
 * Add a thread to a process group's thread list.
 * The thread's thread_group must already be associated with this pgroup.
 * Caller must hold pid_wlock.
 */
int pgroup_add_thread(struct pgroup *pg, struct thread *t) {
    pid_assert_wholding();
    if (!pg || !t)
        return -EINVAL;
    if (pg->exited)
        return -ESRCH;
    if (t->pgroup != NULL)
        return -EEXIST;
    t->pgroup = pg;
    t->pgid = pg->pgid;
    list_entry_init(&t->pg_entry);
    list_node_push(&pg->threads, t, pg_entry);
    pg->t_cnt++;
    return 0;
}

/*
 * Remove a thread from its process group.
 * Caller must hold pid_wlock.
 */
void pgroup_remove_thread(struct thread *t) {
    pid_assert_wholding();
    if (t == NULL)
        return;
    struct pgroup *pg = t->pgroup;
    if (pg == NULL)
        return;
    assert(pg->t_cnt > 0, "Process group thread count went negative");
    assert(!LIST_ENTRY_IS_DETACHED(&t->pg_entry),
           "Thread is not in the process group list");
    list_node_detach(t, pg_entry);
    pg->t_cnt--;
    t->pgroup = NULL;
    t->pgid = 0;
    /* Don't cleanup here — cleanup is driven by thread_group removal */
}

/* ------------------------------------------------------------------ */
/*  Thread group membership                                            */
/* ------------------------------------------------------------------ */

/*
 * Add a thread group (process) to a process group.
 * Caller must hold pid_wlock.
 */
int pgroup_add_tg(struct pgroup *pg, struct thread_group *tg) {
    pid_assert_wholding();
    if (!pg || !tg)
        return -EINVAL;
    if (pg->exited)
        return -ESRCH;
    if (tg->pgroup != NULL)
        return -EEXIST;
    list_entry_init(&tg->list_entry);
    list_node_push(&pg->thread_groups, tg, list_entry);
    tg->pgroup = pg;
    pg->p_cnt++;
    return 0;
}

/*
 * Remove a thread group from its process group.
 * Triggers cleanup if the group becomes empty.
 * Caller must hold pid_wlock.
 */
void pgroup_remove_tg(struct thread_group *tg) {
    pid_assert_wholding();
    if (tg == NULL)
        return;
    struct pgroup *pg = tg->pgroup;
    if (pg == NULL)
        return;
    assert(pg->p_cnt > 0, "Process group thread group count went negative");
    if (!LIST_ENTRY_IS_DETACHED(&tg->list_entry)) {
        list_node_detach(tg, list_entry);
    }
    pg->p_cnt--;
    tg->pgroup = NULL;
    __pgroup_cleanup(pg);
}

/*
 * Migrate a thread group and all its member threads from their current
 * process group to @new_pg.
 *
 * Removes the thread_group from its old pgroup (which may trigger
 * __pgroup_cleanup if it becomes empty), adds it to @new_pg, then
 * moves every thread in the thread_group to @new_pg.
 *
 * Caller must hold pid_wlock.
 */
void pgroup_migrate_tg(struct thread_group *tg, struct pgroup *new_pg) {
    pid_assert_wholding();
    assert(tg != NULL, "pgroup_migrate_tg: NULL tg");
    assert(new_pg != NULL, "pgroup_migrate_tg: NULL new_pg");

    /* Move the thread group itself */
    if (tg->pgroup != NULL) {
        pgroup_remove_tg(tg);
    }
    pgroup_add_tg(new_pg, tg);

    /* Move all member threads to the new pgroup */
    struct thread *t;
    struct thread *tmp;
    list_foreach_node_safe(&tg->thread_list, t, tmp, tg_entry) {
        if (t->pgroup != new_pg) {
            pgroup_remove_thread(t);
        }
        if (t->pgroup == NULL) {
            pgroup_add_thread(new_pg, t);
        }
    }
}

/*
 * Look up a process group by its PGID.
 *
 * Finds the thread whose pid == pgid (the process group leader's
 * thread-group leader) using get_pid_thread(), then returns its
 * pgroup pointer.
 *
 * Returns the pgroup pointer, or NULL if not found.
 * The caller must hold pid_lock or be inside an rcu_read_lock().
 */
struct pgroup *get_pgroup(pid_t pgid) {
    struct session *s = NULL;
    struct session *s_tmp = NULL;

    session_for_each(s, s_tmp) {
        struct pgroup *pg = NULL;
        struct pgroup *pg_tmp = NULL;

        list_foreach_node_safe(&s->pgrps, pg, pg_tmp, list_entry) {
            if (pg->pgid == pgid && !pg->exited)
                return pg;
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  setpgid / getpgid  (POSIX)                                        */
/* ------------------------------------------------------------------ */

/*
 * setpgid - set the process group ID of a process
 *
 * POSIX rules:
 *   - pid==0 → self (caller's tgid)
 *   - pgid==0 → use target's tgid as new pgid
 *   - May only change self or a child that hasn't exec'd
 *   - Target must be in the same session as the caller
 *   - Cannot change a session leader's pgid
 *   - If joining an existing group, it must be in the same session
 */
int pgroup_setpgid(pid_t pid, pid_t pgid) {
    struct thread *p = current;
    struct thread *target = NULL;
    pid_t tgid;

    tgid = thread_tgid(p);
    if (pid == 0)
        pid = tgid;
    if (pgid == 0)
        pgid = pid;
    if (pgid < 0)
        return -EINVAL;

    pid_wlock();

    /* Find the target thread (group leader) */
    if (pid == p->pid || pid == tgid) {
        target = p;
    } else {
        int err = get_pid_thread(pid, &target);
        if (err < 0 || target == NULL) {
            pid_wunlock();
            return -ESRCH;
        }
    }

    /* Must be self or a child of self */
    if (target != p && target->parent != p) {
        pid_wunlock();
        return -ESRCH;
    }

    /* Cannot change session leader's pgid (sid == tgid means session leader) */
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

    /* Already in the right group? */
    if (target->pgid == pgid) {
        pid_wunlock();
        return 0;
    }

    struct thread_group *tg = target->thread_group;
    struct pgroup *new_pg = NULL;

    if (pgid == target_tgid) {
        /* Creating a new process group with target as leader */
        new_pg = pgroup_alloc(pgid, tg);
        if (new_pg == NULL) {
            pid_wunlock();
            return -ENOMEM;
        }
        /* Add to the same session */
        if (target->session != NULL) {
            session_add_pg(target->session, new_pg);
        }
    } else {
        /* Joining an existing process group — look it up by pgid */
        new_pg = get_pgroup(pgid);
        if (new_pg == NULL) {
            pid_wunlock();
            return -EPERM; /* No such process group */
        }
        /* Must not be exited */
        if (new_pg->exited) {
            pid_wunlock();
            return -EPERM;
        }
        /* Must be in the same session */
        if (new_pg->session != target->session) {
            pid_wunlock();
            return -EPERM;
        }
    }

    /* Migrate the thread group (and all its threads) to the new pgroup */
    pgroup_migrate_tg(tg, new_pg);

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
/*  Process-group-wide signal delivery                                 */
/* ------------------------------------------------------------------ */

/*
 * pgroup_kill - send a signal to every process in a process group
 *
 * Iterates the process group's thread_groups list under pid_rlock,
 * sending the signal to each thread group via tg_signal_send().
 *
 * Returns 0 on success, -ESRCH if no processes were found.
 */
int pgroup_kill(pid_t pgid, int signum) {
    int count = 0;
    struct pgroup *pg = NULL;

    pid_rlock();
    pg = get_pgroup(pgid);
    if (pg == NULL || pg->exited) {
        pid_runlock();
        return -ESRCH;
    }
    if (pg->is_kernel) {
        pid_runlock();
        return -EPERM;
    }

    /* Signal each thread group in the process group */
    struct thread_group *tg;
    struct thread_group *tg_tmp;
    list_foreach_node_safe(&pg->thread_groups, tg, tg_tmp, list_entry) {
        struct ksiginfo info;
        memset(&info, 0, sizeof(info));
        info.signo = signum;
        tg_signal_send(tg, &info);
        count++;
    }
    pid_runlock();

    return count > 0 ? 0 : -ESRCH;
}
