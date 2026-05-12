/*
 * kqueue_filters.c - Per-filter attach/detach/event operations for kqueue
 *
 * Implements EVFILT_READ, EVFILT_WRITE, EVFILT_TIMER, EVFILT_SIGNAL,
 * EVFILT_PROC, and EVFILT_VNODE.
 */

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "errno.h"
#include "printf.h"
#include "string.h"
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "vfs/file.h"
#include "vfs/vfs_types.h"
#include "vfs/fs.h"
#include "vfs/poll.h"
#include "dev/cdev.h"
#include "kqueue.h"
#include "kqueue_types.h"
#include "list.h"
#include "signal.h"
#include "timer/timer.h"

/* ========================================================================
 * EVFILT_READ
 * ======================================================================== */

static int knote_read_attach(struct knote *kn) {
    struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, (int)kn->ident);
    if (f == NULL)
        return -EBADF;
    kn->attached_file = f;

    spin_lock(&f->knote_lock);
    list_node_push(&f->knote_list, kn, source_entry);
    spin_unlock(&f->knote_lock);

    return 0;
}

static void knote_read_detach(struct knote *kn) {
    struct vfs_file *f = kn->attached_file;
    if (f == NULL)
        return;
    spin_lock(&f->knote_lock);
    list_node_detach(kn, source_entry);
    list_entry_init(&kn->source_entry);
    spin_unlock(&f->knote_lock);
    vfs_fput(f);
    kn->attached_file = NULL;
}

static int knote_read_event(struct knote *kn, long hint) {
    (void)hint;
    struct vfs_file *f = kn->attached_file;
    if (f == NULL)
        return 0;
    if (f->ops && f->ops->poll) {
        assert(f->ref_count > 0,
               "knote_read_event: stale file %p (ref=%d, ops=%p, ident=%ld)",
               f, f->ref_count, f->ops, kn->ident);
        int revents = f->ops->poll(f, POLLIN | POLLPRI | POLLRDNORM |
                                       POLLRDBAND | POLLRDHUP);
        if (revents & (POLLIN | POLLPRI | POLLRDNORM | POLLRDBAND | POLLRDHUP |
                       POLLHUP | POLLERR))
            return 1;
    }
    /* Chardev files (e.g. /dev/console) have f->ops == NULL;
     * dispatch through the device's poll callback instead. */
    if (f->f_kind == VFS_FILE_KIND_CDEV && f->cdev != NULL &&
        f->cdev->ops.poll != NULL) {
        int revents = f->cdev->ops.poll(f->cdev, POLLIN | POLLPRI |
                                                 POLLRDNORM | POLLRDBAND |
                                                 POLLRDHUP);
        if (revents & (POLLIN | POLLPRI | POLLRDNORM | POLLRDBAND | POLLRDHUP |
                       POLLHUP | POLLERR))
            return 1;
    }
    return 0;
}

struct knote_ops knote_read_ops = {
    .attach = knote_read_attach,
    .detach = knote_read_detach,
    .event = knote_read_event,
};

/* ========================================================================
 * EVFILT_WRITE
 * ======================================================================== */

static int knote_write_attach(struct knote *kn) {
    struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, (int)kn->ident);
    if (f == NULL)
        return -EBADF;
    kn->attached_file = f;

    spin_lock(&f->knote_lock);
    list_node_push(&f->knote_list, kn, source_entry);
    spin_unlock(&f->knote_lock);

    return 0;
}

static void knote_write_detach(struct knote *kn) {
    struct vfs_file *f = kn->attached_file;
    if (f == NULL)
        return;
    spin_lock(&f->knote_lock);
    list_node_detach(kn, source_entry);
    list_entry_init(&kn->source_entry);
    spin_unlock(&f->knote_lock);
    vfs_fput(f);
    kn->attached_file = NULL;
}

static int knote_write_event(struct knote *kn, long hint) {
    (void)hint;
    struct vfs_file *f = kn->attached_file;
    if (f == NULL)
        return 0;
    if (f->ops && f->ops->poll) {
        assert(f->ref_count > 0,
               "knote_write_event: stale file %p (ref=%d, ops=%p, ident=%ld)",
               f, f->ref_count, f->ops, kn->ident);
        int revents = f->ops->poll(f, POLLOUT | POLLWRNORM | POLLWRBAND);
        if (revents & (POLLOUT | POLLWRNORM | POLLWRBAND | POLLERR))
            return 1;
    }
    /* Chardev fallback */
    if (f->f_kind == VFS_FILE_KIND_CDEV && f->cdev != NULL &&
        f->cdev->ops.poll != NULL) {
        int revents = f->cdev->ops.poll(f->cdev, POLLOUT | POLLWRNORM |
                                                 POLLWRBAND);
        if (revents & (POLLOUT | POLLWRNORM | POLLWRBAND | POLLERR))
            return 1;
    }
    return 0;
}

struct knote_ops knote_write_ops = {
    .attach = knote_write_attach,
    .detach = knote_write_detach,
    .event = knote_write_event,
};

/* ========================================================================
 * EVFILT_TIMER
 * ======================================================================== */

/*
 * Timer callback: fires when the timer expires.
 * Called from workqueue context (via sched_timer_add).
 */
static void kqueue_timer_callback(void *arg) {
    struct knote *kn = (struct knote *)arg;
    if (kn == NULL || (kn->status & KN_DETACHED))
        return;

    knote_enqueue(kn);

    /* Re-arm for periodic timers (unless EV_ONESHOT).
     * Use kn->timer_ms (saved at attach time) instead of kn->data,
     * because EV_CLEAR zeroes kn->data after delivery. */
    if (!(kn->flags & EV_ONESHOT) && kn->timer_ms > 0) {
        sched_timer_add(kqueue_timer_callback, kn, (uint64)kn->timer_ms);
    }
}

static int knote_timer_attach(struct knote *kn) {
    if (kn->data <= 0)
        return -EINVAL; /* timer interval must be positive */

    kn->timer_ms = kn->data; /* save interval before EV_CLEAR can zero data */
    kn->status |= KN_TIMER;

    int ret = sched_timer_add(kqueue_timer_callback, kn, (uint64)kn->timer_ms);
    if (ret < 0) {
        kn->status &= ~KN_TIMER;
        return ret;
    }

    return 0;
}

static void knote_timer_detach(struct knote *kn) {
    if (kn->status & KN_TIMER) {
        /* Note: sched_timer_add allocates internally; we can't cancel it.
         * The KN_DETACHED flag prevents the callback from enqueuing. */
        kn->status &= ~KN_TIMER;
    }
}

static int knote_timer_event(struct knote *kn, long hint) {
    (void)hint;
    /* Timers are always edge-triggered via the callback; never "always ready" */
    return 0;
}

struct knote_ops knote_timer_ops = {
    .attach = knote_timer_attach,
    .detach = knote_timer_detach,
    .event = knote_timer_event,
};

/* ========================================================================
 * EVFILT_SIGNAL
 * ======================================================================== */

static int knote_signal_attach(struct knote *kn) {
    int signo = (int)kn->ident;
    if (signo < 1 || signo > NSIG)
        return -EINVAL;

    struct thread *p = current;
    sigacts_t *sa = p->sigacts;
    if (sa == NULL)
        return -EINVAL;

    kn->data = 0; /* count of signals received */

    spin_lock(&sa->kqueue_signal_lock);
    list_node_push(&sa->kqueue_signal_knotes[signo - 1], kn, source_entry);
    spin_unlock(&sa->kqueue_signal_lock);

    return 0;
}

static void knote_signal_detach(struct knote *kn) {
    struct thread *p = current;
    sigacts_t *sa = p->sigacts;
    if (sa == NULL)
        return;

    int signo = (int)kn->ident;
    if (signo < 1 || signo > NSIG)
        return;

    spin_lock(&sa->kqueue_signal_lock);
    list_node_detach(kn, source_entry);
    list_entry_init(&kn->source_entry);
    spin_unlock(&sa->kqueue_signal_lock);
}

static int knote_signal_event(struct knote *kn, long hint) {
    (void)hint;
    /* Signal events are edge-triggered; never "always ready" */
    return 0;
}

struct knote_ops knote_signal_ops = {
    .attach = knote_signal_attach,
    .detach = knote_signal_detach,
    .event = knote_signal_event,
};

/* ========================================================================
 * EVFILT_PROC
 * ======================================================================== */

static int knote_proc_attach(struct knote *kn) {
    pid_t pid = (pid_t)kn->ident;
    struct thread *target = NULL;

    rcu_read_lock();
    if (get_pid_thread(pid, &target) != 0 || target == NULL) {
        rcu_read_unlock();
        return -ESRCH;
    }
    kn->attached_pid = pid;

    spin_lock(&target->kqueue_proc_lock);
    list_node_push(&target->kqueue_proc_knotes, kn, source_entry);
    spin_unlock(&target->kqueue_proc_lock);

    rcu_read_unlock();
    return 0;
}

static void knote_proc_detach(struct knote *kn) {
    pid_t pid = kn->attached_pid;
    struct thread *target = NULL;

    rcu_read_lock();
    if (get_pid_thread(pid, &target) == 0 && target != NULL) {
        spin_lock(&target->kqueue_proc_lock);
        list_node_detach(kn, source_entry);
        list_entry_init(&kn->source_entry);
        spin_unlock(&target->kqueue_proc_lock);
    }
    rcu_read_unlock();
}

static int knote_proc_event(struct knote *kn, long hint) {
    (void)hint;
    /* Process events are edge-triggered */
    return 0;
}

struct knote_ops knote_proc_ops = {
    .attach = knote_proc_attach,
    .detach = knote_proc_detach,
    .event = knote_proc_event,
};

/* ========================================================================
 * EVFILT_VNODE
 * ======================================================================== */

static int knote_vnode_attach(struct knote *kn) {
    struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, (int)kn->ident);
    if (f == NULL)
        return -EBADF;

    struct vfs_inode *inode = vfs_inode_deref(&f->inode);
    if (inode == NULL) {
        vfs_fput(f);
        return -EBADF; /* not a file-backed fd */
    }

    kn->attached_file = f; /* hold reference so inode stays valid */
    kn->attached_inode = inode;

    spin_lock(&inode->knote_lock);
    list_node_push(&inode->knote_list, kn, source_entry);
    spin_unlock(&inode->knote_lock);

    return 0;
}

static void knote_vnode_detach(struct knote *kn) {
    struct vfs_inode *inode = kn->attached_inode;
    if (inode != NULL) {
        spin_lock(&inode->knote_lock);
        list_node_detach(kn, source_entry);
        list_entry_init(&kn->source_entry);
        spin_unlock(&inode->knote_lock);
        kn->attached_inode = NULL;
    }
    if (kn->attached_file != NULL) {
        vfs_fput(kn->attached_file);
        kn->attached_file = NULL;
    }
}

static int knote_vnode_event(struct knote *kn, long hint) {
    (void)hint;
    /* Vnode events are edge-triggered; never "always ready" */
    return 0;
}

struct knote_ops knote_vnode_ops = {
    .attach = knote_vnode_attach,
    .detach = knote_vnode_detach,
    .event = knote_vnode_event,
};
