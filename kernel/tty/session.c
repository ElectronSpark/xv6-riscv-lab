/*
 * session.c - Terminal session management
 *
 * A session groups process groups and optionally binds them to a
 * controlling terminal.  The session leader (thread whose tgid ==
 * sid) creates the session via setsid().
 *
 * Session membership (thread->session) is protected by pid_lock.
 * Session-internal fields (ctrl_tty, fg_pgid) are protected by
 * the session's own spinlock.
 */

#include "types.h"
#include "param.h"
#include "errno.h"
#include "string.h"
#include "printf.h"
#include "riscv.h"
#include "defs.h"
#include "lock/spinlock.h"
#include "lock/rcu.h"
#include "mm/slab.h"
#include "proc/thread.h"
#include "proc/thread_group.h"
#include "proc/pgroup.h"
#include "tty/session.h"
#include "tty/tty.h"
#include "smp/percpu.h"

/* ------------------------------------------------------------------ */
/*  Slab cache                                                        */
/* ------------------------------------------------------------------ */

static slab_cache_t __session_cache = {0};

void session_init(void) {
    int ret = slab_cache_init(&__session_cache, "session_cache",
                              sizeof(struct session), SLAB_FLAG_STATIC);
    assert(ret == 0, "session_init: failed to init session_cache, errno=%d",
           ret);
}

/* ------------------------------------------------------------------ */
/*  Allocation / reference counting                                   */
/* ------------------------------------------------------------------ */

struct session *session_alloc(pid_t sid) {
    struct session *s = slab_alloc(&__session_cache);
    if (s == NULL)
        return NULL;

    s->sid = sid;
    s->ctrl_tty = NULL;
    s->fg_pgid = sid; /* initially the leader is also fg group */
    s->ref_count = 1;
    spin_init(&s->lock, "session");
    return s;
}

void session_ref(struct session *s) {
    if (s == NULL)
        return;
    __atomic_add_fetch(&s->ref_count, 1, __ATOMIC_SEQ_CST);
}

void session_unref(struct session *s) {
    if (s == NULL)
        return;
    int old = __atomic_sub_fetch(&s->ref_count, 1, __ATOMIC_SEQ_CST);
    if (old <= 0)
        slab_free(s);
}

/* ------------------------------------------------------------------ */
/*  Controlling terminal                                              */
/* ------------------------------------------------------------------ */

void session_set_ctrl_tty(struct session *s, struct tty *tty) {
    if (s == NULL)
        return;
    spin_lock(&s->lock);
    s->ctrl_tty = tty;
    /* Also set the tty's back-pointer */
    if (tty) {
        spin_lock(&tty->lock);
        tty->session = s;
        spin_unlock(&tty->lock);
    }
    spin_unlock(&s->lock);
}

struct tty *session_get_ctrl_tty(struct session *s) {
    if (s == NULL)
        return NULL;
    struct tty *t;
    spin_lock(&s->lock);
    t = s->ctrl_tty;
    spin_unlock(&s->lock);
    return t;
}

/* ------------------------------------------------------------------ */
/*  Foreground process group                                          */
/* ------------------------------------------------------------------ */

void session_set_fg_pgid(struct session *s, pid_t pgid) {
    if (s == NULL)
        return;
    spin_lock(&s->lock);
    s->fg_pgid = pgid;
    spin_unlock(&s->lock);
}

pid_t session_get_fg_pgid(struct session *s) {
    if (s == NULL)
        return -1;
    pid_t pgid;
    spin_lock(&s->lock);
    pgid = s->fg_pgid;
    spin_unlock(&s->lock);
    return pgid;
}

/* ------------------------------------------------------------------ */
/*  setsid / getsid                                                   */
/* ------------------------------------------------------------------ */

pid_t session_setsid(void) {
    struct thread *p = current;
    pid_t tgid = thread_tgid(p);

    pid_wlock();

    /* Must not be a process group leader */
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

    /* Release old session */
    if (p->session)
        session_unref(p->session);

    /* Become session leader and process group leader */
    p->sid = tgid;
    p->pgid = tgid;
    p->session = s;

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

/* ------------------------------------------------------------------ */
/*  Boot-time initialisation                                          */
/* ------------------------------------------------------------------ */

/*
 * session_init_first - create session 1 for the init process
 *
 * Called once during boot.  Init (pid 1) becomes its own session
 * leader and process group leader.
 */
void session_init_first(struct thread *initproc) {
    struct session *s = session_alloc(1);
    assert(s != NULL, "session_init_first: allocation failed");

    initproc->sid = 1;
    initproc->pgid = 1;
    initproc->session = s;
}
