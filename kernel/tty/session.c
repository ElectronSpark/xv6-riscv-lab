/*
 * session.c - Terminal session management (POSIX)
 *
 * A session groups process groups and optionally binds them to a
 * controlling terminal.  The session leader (thread whose tgid ==
 * sid) creates the session via setsid().
 *
 * Hierarchy:  session → pgroup → thread_group → thread
 *
 * Session membership (thread->session, pgroup→session) is protected
 * by the global pid_lock (rwlock):
 *   - pid_wlock for mutations (setsid, add/remove thread/pg)
 *   - pid_rlock for read-only access (getsid, fg_pgrp queries)
 *
 * Lock ordering:  pid_lock > sigacts.lock > tcb_lock
 */

#include "types.h"
#include "param.h"
#include "errno.h"
#include "string.h"
#include "printf.h"
#include "riscv.h"
#include "defs.h"
#include "list.h"
#include "lock/spinlock.h"
#include "lock/rcu.h"
#include "mm/slab.h"
#include "proc/thread.h"
#include "proc/thread_group.h"
#include "proc/pgroup.h"
#include "tty/session.h"
#include "tty/tty.h"
#include "signal.h"
#include "smp/percpu.h"

/* ------------------------------------------------------------------ */
/*  Slab cache                                                        */
/* ------------------------------------------------------------------ */

static slab_cache_t __session_cache = {0};
list_node_t session_list = {0};

void session_cache_init(void) {
    list_entry_init(&session_list);
    int ret = slab_cache_init(&__session_cache, "session_cache",
                              sizeof(struct session), SLAB_FLAG_STATIC);
    assert(ret == 0, "session_cache_init: failed to init session_cache, errno=%d",
           ret);
}

/* Session 0: kernel-thread session (no controlling tty). */
static struct session *__kernel_session;

struct session *session_get_kernel(void) {
    return __kernel_session;
}

void session_init(struct thread *initproc) {
    /* Create session 0 for kernel threads (no controlling tty,
     * no foreground process group). */
    __kernel_session = session_alloc(0);
    assert(__kernel_session != NULL,
           "session_init: failed to allocate kernel session 0");

    /* Create session 1 for the init process:
     * session 1 → pgroup 1 → thread_group(init) → init thread */
    struct thread_group *tg = initproc->thread_group;
    assert(tg != NULL, "session_init: initproc has no thread_group");

    struct pgroup *pg = initproc->pgroup;
    assert(pg != NULL, "session_init: initproc has no pgroup");

    struct session *s = session_alloc(initproc->pid);
    assert(s != NULL, "session_init: session_alloc failed");

    session_add_pg(s, pg);
    session_add_thread(s, initproc);
    s->fg_pgrp = pg;
}

static struct session *__session_alloc(void) {
    struct session *s = slab_alloc(&__session_cache);
    if (s == NULL)
        return NULL;

    memset(s, 0, sizeof(*s));
    list_entry_init(&s->global_entry);
    list_entry_init(&s->threads);
    list_entry_init(&s->pgrps);
    return s;
}

/* ------------------------------------------------------------------ */
/*  Allocation / reference counting                                   */
/* ------------------------------------------------------------------ */

struct session *session_alloc(pid_t sid) {
    struct session *s = __session_alloc();
    if (s == NULL)
        return NULL;

    s->sid = sid;
    s->ctrl_tty = NULL;
    s->fg_pgrp = NULL;
    s->ref_cnt = 1;
    s->t_cnt = 0;
    s->pg_cnt = 0;
    list_node_push(&session_list, s, global_entry);
    return s;
}

void session_ref(struct session *s) {
    if (s == NULL)
        return;
    __atomic_add_fetch(&s->ref_cnt, 1, __ATOMIC_SEQ_CST);
}

void session_unref(struct session *s) {
    if (s == NULL)
        return;
    int old = __atomic_sub_fetch(&s->ref_cnt, 1, __ATOMIC_SEQ_CST);
    if (old <= 0) {
        s->exited = 1;
        if (!LIST_ENTRY_IS_DETACHED(&s->global_entry))
            list_node_detach(s, global_entry);
        slab_free(s);
    }
}

/* ------------------------------------------------------------------ */
/*  Thread membership                                                  */
/* ------------------------------------------------------------------ */

/*
 * Add a thread to a session's thread list.
 * Sets thread->session and thread->sid.
 * Caller must hold pid_wlock.
 */
int session_add_thread(struct session *s, struct thread *t) {
    pid_assert_wholding();
    if (s == NULL || t == NULL)
        return -EINVAL;
    if (s->exited)
        return -ESRCH;
    if (t->session != NULL)
        return -EEXIST;
    t->session = s;
    t->sid = s->sid;
    list_entry_init(&t->sid_entry);
    list_node_push(&s->threads, t, sid_entry);
    s->t_cnt++;
    session_ref(s);
    return 0;
}

/*
 * Remove a thread from its session's thread list.
 * Clears thread->session and thread->sid.
 * Caller must hold pid_wlock.
 */
int session_remove_thread(struct session *s, struct thread *t) {
    pid_assert_wholding();
    if (s == NULL || t == NULL)
        return -EINVAL;
    if (t->session != s)
        return -EINVAL;
    if (!LIST_ENTRY_IS_DETACHED(&t->sid_entry)) {
        list_node_detach(t, sid_entry);
    }
    s->t_cnt--;
    t->session = NULL;
    t->sid = 0;
    session_unref(s);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Process group membership                                           */
/* ------------------------------------------------------------------ */

/*
 * Add a process group to a session.
 * Caller must hold pid_wlock.
 */
int session_add_pg(struct session *s, struct pgroup *pg) {
    pid_assert_wholding();
    if (s == NULL || pg == NULL)
        return -EINVAL;
    if (s->exited)
        return -ESRCH;
    if (pg->session != NULL)
        return -EEXIST;
    pg->session = s;
    list_entry_init(&pg->list_entry);
    list_entry_push(&s->pgrps, &pg->list_entry);
    s->pg_cnt++;
    session_ref(s);
    return 0;
}

/*
 * Remove a process group from its session.
 * Caller must hold pid_wlock.
 */
int session_remove_pg(struct session *s, struct pgroup *pg) {
    pid_assert_wholding();
    if (s == NULL || pg == NULL)
        return -EINVAL;
    if (pg->session != s)
        return -EINVAL;
    if (!LIST_ENTRY_IS_DETACHED(&pg->list_entry)) {
        list_entry_detach(&pg->list_entry);
    }
    s->pg_cnt--;
    /* If the foreground pgroup is being removed, clear it */
    if (s->fg_pgrp == pg)
        s->fg_pgrp = NULL;
    pg->session = NULL;
    session_unref(s);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Controlling terminal                                              */
/* ------------------------------------------------------------------ */

void session_set_ctrl_tty(struct session *s, struct tty *tty) {
    if (s == NULL || s->exited)
        return;
    s->ctrl_tty = tty;
    /* Also set the tty's back-pointer */
    if (tty) {
        spin_lock(&tty->lock);
        tty->session = s;
        spin_unlock(&tty->lock);
    }
}

struct tty *session_get_ctrl_tty(struct session *s) {
    if (s == NULL)
        return NULL;
    return s->ctrl_tty;
}

/* ------------------------------------------------------------------ */
/*  Foreground process group                                          */
/* ------------------------------------------------------------------ */

void session_set_fg_pgid(struct session *s, pid_t pgid) {
    if (s == NULL || s->exited)
        return;
    struct pgroup *pg = get_pgroup(pgid);
    if (pg == NULL)
        return;
    if (pg->exited)
        return; /* cannot set an exited pgroup as foreground */
    if (pg->session != s)
        return; /* pgroup must belong to this session */
    s->fg_pgrp = pg;
}

pid_t session_get_fg_pgid(struct session *s) {
    if (s == NULL)
        return -1;
    if (s->fg_pgrp == NULL)
        return -1;
    return s->fg_pgrp->pgid;
}

/* ------------------------------------------------------------------ */
/*  Session hangup                                                     */
/* ------------------------------------------------------------------ */

/*
 * Send SIGHUP + SIGCONT to a thread's thread group.
 */
static void __hangup_signal_tg(struct thread *t) {
    if (t->thread_group == NULL)
        return;
    struct ksiginfo info;
    memset(&info, 0, sizeof(info));
    info.signo = SIGHUP;
    tg_signal_send(t->thread_group, &info);
    info.signo = SIGCONT;
    tg_signal_send(t->thread_group, &info);
}

void session_hangup(struct session *s) {
    if (s == NULL || s->exited)
        return;

    pid_assert_wholding();

    s->exited = 1;

    /* POSIX: SIGHUP + SIGCONT to the foreground process group only. */
    if (s->fg_pgrp != NULL && !s->fg_pgrp->exited) {
        struct thread *t, *t_tmp;
        list_foreach_node_safe(&s->fg_pgrp->threads, t, t_tmp, pg_entry)
            __hangup_signal_tg(t);
    }

    /* Hang up the controlling terminal.  For a PTY slave this severs
     * the pipe link so the master side gets EOF / POLLHUP. */
    if (s->ctrl_tty != NULL)
        tty_hangup(s->ctrl_tty);

    /* Disassociate the controlling terminal */
    s->ctrl_tty = NULL;
    s->fg_pgrp = NULL;
}

/* ------------------------------------------------------------------ */
/*  setsid / getsid  (POSIX)                                          */
/* ------------------------------------------------------------------ */

/*
 * setsid - create a new session
 *
 * The calling process becomes the session leader of a new session
 * and the process group leader of a new process group.
 * The new session has no controlling terminal.
 *
 * POSIX: must not be called by a process group leader.
 */
pid_t session_setsid(void) {
    struct thread *p = current;
    pid_t tgid = thread_tgid(p);

    pid_wlock();

    /* Must not already be a process group leader */
    if (p->pgid == tgid) {
        pid_wunlock();
        return -EPERM;
    }

    /* Create new session */
    struct session *s = session_alloc(tgid);
    if (s == NULL) {
        pid_wunlock();
        return -ENOMEM;
    }

    /* Create new process group with self as leader */
    struct thread_group *tg = p->thread_group;
    struct pgroup *pg = pgroup_alloc(tgid, tg);
    if (pg == NULL) {
        slab_free(s);
        pid_wunlock();
        return -ENOMEM;
    }

    /* Remove from old session/pgroup hierarchy */
    if (tg != NULL) {
        pgroup_remove_tg(tg);
    }
    pgroup_remove_thread(p);
    if (p->session != NULL) {
        session_remove_thread(p->session, p);
    }

    /* Set up new hierarchy: session → pgroup → thread_group → thread */
    session_add_pg(s, pg);
    if (tg != NULL) {
        pgroup_add_tg(pg, tg);
    }
    pgroup_add_thread(pg, p);
    session_add_thread(s, p);
    s->fg_pgrp = pg; /* Becomes foreground group */

    /* Drop the creation ref from session_alloc().  The session is now
     * kept alive solely by its member refs (add_pg, add_thread).
     * When the last member is removed the session is freed. */
    session_unref(s);

    pid_wunlock();
    return tgid;
}

pid_t session_getsid(pid_t pid) {
    if (pid == 0)
        return current->sid;

    if (pid < 0)
        return -EINVAL;

    struct thread *target = NULL;
    pid_t sid;

    rcu_read_lock();
    int err = get_pid_thread(pid, &target);
    if (err < 0 || target == NULL) {
        rcu_read_unlock();
        return -ESRCH;
    }
    sid = target->sid;
    rcu_read_unlock();

    return sid;
}

/*
 * Look up a session by its SID.
 *
 * Finds the thread whose pid == sid (the session leader) using
 * get_pid_thread(), then returns its session pointer.
 *
 * Returns the session pointer, or NULL if not found.
 * The caller must be inside an rcu_read_lock() section or hold pid_lock.
 */
struct session *get_session(pid_t sid) {
    struct thread *t = NULL;
    int err = get_pid_thread(sid, &t);
    if (err < 0 || t == NULL)
        return NULL;
    return t->session;
}
