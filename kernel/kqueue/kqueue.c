/*
 * kqueue.c - BSD kqueue event notification: core implementation
 *
 * Provides kqueue_create, kqueue_register, kqueue_wait, kqueue_close,
 * and the knote_enqueue notification primitive.
 */

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "errno.h"
#include "printf.h"
#include "string.h"
#include "param.h"
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "proc/tq.h"
#include "proc/sched.h"
#include "mm/slab.h"
#include "vfs/file.h"
#include "vfs/vfs_types.h"
#include "vfs/poll.h"
#include "kqueue.h"
#include "kqueue_types.h"
#include "list.h"
#include "signal.h"
#include "timer/timer.h"
#include <mm/vm.h>

/* Slab caches */
static slab_cache_t __kqueue_cache = {0};
static slab_cache_t __knote_cache = {0};

/* Forward declarations */
static int kqueue_file_release(struct vfs_inode *inode, struct vfs_file *file);
static int kqueue_file_poll(struct vfs_file *file, short events);

/* External filter ops (defined in kqueue_filters.c) */
extern struct knote_ops knote_read_ops;
extern struct knote_ops knote_write_ops;
extern struct knote_ops knote_timer_ops;
extern struct knote_ops knote_signal_ops;
extern struct knote_ops knote_proc_ops;
extern struct knote_ops knote_vnode_ops;

/* File operations for a kqueue file descriptor */
static struct vfs_file_ops kqueue_file_ops = {
    .read = NULL,
    .write = NULL,
    .llseek = NULL,
    .release = kqueue_file_release,
    .fsync = NULL,
    .poll = kqueue_file_poll,
    .ioctl = NULL,
    .fault = NULL,
};

static int knote_is_file_filter(struct knote *kn)
{
    return kn->filter == EVFILT_READ || kn->filter == EVFILT_WRITE;
}

static int knote_is_clear_file_filter(struct knote *kn)
{
    return (kn->flags & EV_CLEAR) && knote_is_file_filter(kn);
}

static int knote_poll_active(struct knote *kn)
{
    return kn->ops != NULL && kn->ops->event != NULL &&
           kn->ops->event(kn, 0);
}

static int knote_file_poll_revents(struct knote *kn)
{
    if (!knote_is_file_filter(kn) || kn->attached_file == NULL)
        return 0;

    struct vfs_file *f = kn->attached_file;
    short events = kn->filter == EVFILT_READ
        ? (POLLIN | POLLPRI | POLLRDNORM | POLLRDBAND | POLLRDHUP)
        : (POLLOUT | POLLWRNORM | POLLWRBAND);
    int revents = 0;

    if (f->ops && f->ops->poll) {
        assert(f->ref_count > 0,
               "knote_file_poll_revents: stale file %p (ref=%d, ops=%p, ident=%ld)",
               f, f->ref_count, f->ops, kn->ident);
        revents = f->ops->poll(f, events);
    } else if (f->f_kind == VFS_FILE_KIND_CDEV && f->cdev != NULL &&
               f->cdev->ops.poll != NULL) {
        revents = f->cdev->ops.poll(f->cdev, events);
    }

    if (kn->kq != NULL && (kn->kq->flags & KQ_EPOLL_COMPAT)) {
        int always = revents & (POLLERR | POLLHUP | POLLNVAL);

        if (kn->filter == EVFILT_READ) {
            int requested = 0;
            if (kn->sfflags & (POLLIN | POLLPRI | POLLRDNORM | POLLRDBAND))
                requested |= revents & (POLLIN | POLLPRI | POLLRDNORM |
                                        POLLRDBAND);
            if (kn->sfflags & POLLRDHUP)
                requested |= revents & POLLRDHUP;
            revents = requested | always;
        } else {
            int requested = 0;
            if (kn->sfflags & (POLLOUT | POLLWRNORM | POLLWRBAND))
                requested |= revents & (POLLOUT | POLLWRNORM | POLLWRBAND);
            revents = requested | always;
        }
    }

    return revents;
}

static void kqueue_rescan_registered_locked(struct kqueue *kq) {
    struct knote *kn = NULL;
    struct knote *tmp = NULL;

    list_foreach_node_safe(&kq->registered, kn, tmp, kq_entry) {
        if ((kn->status & (KN_DISABLED | KN_QUEUED | KN_DETACHED)) ||
            kn->ops == NULL || kn->ops->event == NULL) {
            continue;
        }
        /*
         * EV_CLEAR backs Linux EPOLLET in the epoll shim.  Epoll-created
         * queues keep Linux level-triggered behavior and redeliver unread
         * pipes/sockets on each wait.  Raw kqueue coalesces synthetic level
         * rescans after one delivery so a caller with a small event buffer can
         * still observe edge-like timers, signals, process, and vnode events.
         */
        if (knote_is_file_filter(kn)) {
            int revents = knote_file_poll_revents(kn);

            if (revents == 0) {
                kn->status &= ~KN_EDGE_ACTIVE;
                continue;
            }

            if (knote_is_clear_file_filter(kn)) {
                if (kn->status & KN_EDGE_ACTIVE)
                    continue;
                kn->status |= KN_EDGE_ACTIVE;
            } else if (!(kq->flags & KQ_EPOLL_COMPAT)) {
                if (kn->status & KN_LEVEL_SEEN)
                    continue;
            }
        } else if (!knote_poll_active(kn)) {
            continue;
        }

        kn->status |= KN_QUEUED;
        list_node_push(&kq->ready, kn, ready_entry);
        kq->nready++;
    }
}

static struct knote *kqueue_pick_ready_locked(struct kqueue *kq) {
    struct knote *kn = NULL;
    struct knote *tmp = NULL;

    /*
     * Timers, signals, process, and vnode notifications are edge-like.  Return
     * them before level file readiness so an unread pipe/socket cannot starve
     * a one-shot timer when the caller asks for a small result set.
     */
    list_foreach_node_safe(&kq->ready, kn, tmp, ready_entry) {
        if (!knote_is_file_filter(kn))
            return kn;
    }

    /*
     * Prefer read-side readiness when both read and write filters are queued.
     * Linux epoll users often monitor sockets for both directions, while
     * POLLOUT is effectively level-ready for most connected TCP sockets.  If
     * write knotes dominate a small event buffer, GLib/WebKit can churn on
     * writable events and fail to make progress on readable network data.
     */
    list_foreach_node_safe(&kq->ready, kn, tmp, ready_entry) {
        if (kn->filter == EVFILT_READ)
            return kn;
    }

    return LIST_FIRST_NODE(&kq->ready, struct knote, ready_entry);
}

/*
 * kqueue_init - initialize the kqueue subsystem
 */
void kqueue_init(void) {
    int ret;
    ret = slab_cache_init(&__kqueue_cache, "kqueue_cache",
                          sizeof(struct kqueue), SLAB_FLAG_STATIC);
    assert(ret == 0, "kqueue_init: failed to init kqueue_cache, errno=%d", ret);

    ret = slab_cache_init(&__knote_cache, "knote_cache",
                          sizeof(struct knote), SLAB_FLAG_STATIC);
    assert(ret == 0, "kqueue_init: failed to init knote_cache, errno=%d", ret);
}

/*
 * knote_alloc - allocate a zeroed knote
 */
static struct knote *knote_alloc(void) {
    struct knote *kn = slab_alloc(&__knote_cache);
    if (kn == NULL)
        return NULL;
    memset(kn, 0, sizeof(*kn));
    list_entry_init(&kn->kq_entry);
    list_entry_init(&kn->ready_entry);
    list_entry_init(&kn->source_entry);
    return kn;
}

/*
 * knote_free - free a knote back to the slab cache
 */
static void knote_free(struct knote *kn) {
    slab_free(kn);
}

struct kqueue *kqueue_alloc_private(void) {
    struct kqueue *kq = slab_alloc(&__kqueue_cache);
    if (kq == NULL)
        return NULL;

    memset(kq, 0, sizeof(*kq));
    spin_init(&kq->lock, "kqueue");
    tq_init(&kq->waitq, "kqueue_waitq", NULL);
    list_entry_init(&kq->registered);
    list_entry_init(&kq->ready);
    return kq;
}

struct kqueue *kqueue_from_file(struct vfs_file *file)
{
    if (file == NULL || file->ops != &kqueue_file_ops)
        return NULL;
    return (struct kqueue *)file->private_data;
}

int kqueue_file_is_epoll(struct vfs_file *file)
{
    struct kqueue *kq = kqueue_from_file(file);
    return kq != NULL && (kq->flags & KQ_EPOLL_COMPAT);
}

int kqueue_epoll_contains_kqueue(struct kqueue *root, struct kqueue *needle,
                                 int depth_limit)
{
    if (root == NULL || needle == NULL || depth_limit < 0)
        return 0;

    if (root == needle)
        return 1;

    struct vfs_file *children[16];
    int nchildren = 0;

    spin_lock(&root->lock);
    struct knote *kn = NULL;
    struct knote *tmp = NULL;
    list_foreach_node_safe(&root->registered, kn, tmp, kq_entry) {
        if (!knote_is_file_filter(kn) || kn->attached_file == NULL)
            continue;

        struct kqueue *child = kqueue_from_file(kn->attached_file);
        if (child == NULL || !(child->flags & KQ_EPOLL_COMPAT))
            continue;

        if (child == needle) {
            spin_unlock(&root->lock);
            for (int i = 0; i < nchildren; i++)
                vfs_fput(children[i]);
            return 1;
        }

        if (depth_limit > 0 && nchildren < (int)(sizeof(children) /
                                                 sizeof(children[0])))
            children[nchildren++] = vfs_fdup(kn->attached_file);
    }
    spin_unlock(&root->lock);

    for (int i = 0; i < nchildren; i++) {
        struct kqueue *child = kqueue_from_file(children[i]);
        if (child != NULL &&
            kqueue_epoll_contains_kqueue(child, needle, depth_limit - 1)) {
            for (int j = i; j < nchildren; j++)
                vfs_fput(children[j]);
            return 1;
        }
        vfs_fput(children[i]);
    }

    return 0;
}

/*
 * knote_get_ops - return the ops for a given filter type
 */
static struct knote_ops *knote_get_ops(int16 filter) {
    switch (filter) {
    case EVFILT_READ:   return &knote_read_ops;
    case EVFILT_WRITE:  return &knote_write_ops;
    case EVFILT_TIMER:  return &knote_timer_ops;
    case EVFILT_SIGNAL: return &knote_signal_ops;
    case EVFILT_PROC:   return &knote_proc_ops;
    case EVFILT_VNODE:  return &knote_vnode_ops;
    default:            return NULL;
    }
}

/*
 * knote_enqueue - add a knote to the kqueue's ready list and wake a waiter
 *
 * Called by event sources (pipes, sockets, timers, etc.) when an event fires.
 * May be called from interrupt context (timer callbacks) or process context.
 *
 * The caller should NOT hold kq->lock.
 */
void knote_enqueue(struct knote *kn) {
    if (kn == NULL)
        return;
    struct kqueue *kq = kn->kq;
    if (kq == NULL || kq->closed)
        return;

    spin_lock(&kq->lock);
    /* Skip if disabled or already queued */
    if ((kn->status & KN_DISABLED) || (kn->status & KN_QUEUED) ||
        (kn->status & KN_DETACHED)) {
        spin_unlock(&kq->lock);
        return;
    }
    if (knote_is_clear_file_filter(kn))
        kn->status |= KN_EDGE_ACTIVE;
    kn->status |= KN_QUEUED;
    list_node_push(&kq->ready, kn, ready_entry);
    kq->nready++;
    tq_wakeup(&kq->waitq, 0, 0);
    struct vfs_file *kq_file = kq->file;
    if (kq_file)
        kq_file = vfs_fdup(kq_file);
    spin_unlock(&kq->lock);

    /* Propagate readiness to any outer kqueue/poll monitoring this kqueue fd */
    if (kq_file) {
        vfs_file_knote_notify(kq_file, EVFILT_READ, 0);
        vfs_fput(kq_file);
    }
}

/*
 * __knote_enqueue_core - enqueue a knote without recursive propagation
 *
 * Performs the actual enqueue work and returns the kqueue's backing file
 * (with bumped refcount) if propagation to an outer kqueue/poll is needed.
 * The caller is responsible for calling vfs_file_knote_notify(kq_file,
 * EVFILT_READ, 0) and vfs_fput(kq_file) AFTER releasing any knote_lock.
 *
 * Returns: kq_file needing propagation (caller must vfs_fput), or NULL.
 */
static struct vfs_file *__knote_enqueue_core(struct knote *kn, int64 data,
                                             uint32 fflags) {
    if (kn == NULL)
        return NULL;
    struct kqueue *kq = kn->kq;
    if (kq == NULL || kq->closed)
        return NULL;

    spin_lock(&kq->lock);
    if ((kn->status & KN_DISABLED) || (kn->status & KN_DETACHED)) {
        spin_unlock(&kq->lock);
        return NULL;
    }
    kn->data = data;
    kn->fflags |= fflags;
    if (knote_is_clear_file_filter(kn))
        kn->status |= KN_EDGE_ACTIVE;
    if (!(kn->status & KN_QUEUED)) {
        kn->status |= KN_QUEUED;
        list_node_push(&kq->ready, kn, ready_entry);
        kq->nready++;
    }
    tq_wakeup(&kq->waitq, 0, 0);
    /*
     * Propagate readiness to an outer kqueue even when this knote was already
     * queued.  Nested epoll users such as GLib can monitor an epoll fd through
     * another poll source; if the inner queue is already readable, another
     * source notification should still wake the outer waiter.
     */
    struct vfs_file *kq_file = kq->file ? vfs_fdup(kq->file) : NULL;
    spin_unlock(&kq->lock);
    return kq_file;
}

/*
 * knote_enqueue_with_data - enqueue with updated data and fflags
 *
 * Safe to call when no file->knote_lock is held by the caller.
 * For contexts that already hold a knote_lock (e.g. vfs_file_knote_notify),
 * use __knote_enqueue_core directly to avoid recursive lock acquisition.
 */
void knote_enqueue_with_data(struct knote *kn, int64 data, uint32 fflags) {
    struct vfs_file *kq_file = __knote_enqueue_core(kn, data, fflags);

    /* Propagate readiness to any outer kqueue/poll monitoring this kqueue fd */
    if (kq_file) {
        vfs_file_knote_notify(kq_file, EVFILT_READ, 0);
        vfs_fput(kq_file);
    }
}

/*
 * vfs_file_knote_notify - walk a file's knote list and enqueue matching knotes
 *
 * @file: the vfs_file with attached knotes
 * @filter: EVFILT_READ or EVFILT_WRITE
 * @data: filter-specific data (e.g. bytes available)
 *
 * Uses __knote_enqueue_core (no propagation) while holding knote_lock,
 * then propagates to outer kqueues AFTER releasing the lock.  This avoids
 * recursive vfs_file_knote_notify → knote_enqueue_with_data →
 * vfs_file_knote_notify chains that can re-enter the same knote_lock if
 * the kqueue propagation path loops back to the original file.
 */
#define MAX_KNOTE_PROPAGATE 16
void vfs_file_knote_notify(struct vfs_file *file, int filter, int64 data) {
    if (file == NULL)
        return;

    struct vfs_file *propagate[MAX_KNOTE_PROPAGATE];
    int nprop = 0;

    /*
     * Hold knote_lock for the entire iteration.  This prevents a
     * concurrent __kqueue_detach_knote (from a deferred kqueue close)
     * from freeing a knote while we call __knote_enqueue_core on it.
     *
     * Lock ordering: file->knote_lock → kq->lock (inside
     * __knote_enqueue_core).  No other code path holds kq->lock
     * while acquiring file->knote_lock (kqueue_register and
     * __kqueue_detach_knote always release kq->lock first), so this
     * nesting is deadlock-free.
     */
    spin_lock(&file->knote_lock);
    struct knote *kn = NULL;
    struct knote *tmp = NULL;
    list_foreach_node_safe(&file->knote_list, kn, tmp, source_entry) {
        if (kn->filter == filter) {
            struct vfs_file *kq_file =
                __knote_enqueue_core(kn, data, 0);
            if (kq_file) {
                if (nprop < MAX_KNOTE_PROPAGATE)
                    propagate[nprop++] = kq_file;
                else
                    vfs_fput(kq_file);
            }
        }
    }
    spin_unlock(&file->knote_lock);

    /* Propagate to outer kqueues/polls without holding any knote_lock */
    for (int i = 0; i < nprop; i++) {
        vfs_file_knote_notify(propagate[i], EVFILT_READ, 0);
        vfs_fput(propagate[i]);
    }
}

/*
 * vfs_inode_knote_notify - walk an inode's knote list and enqueue for vnode events
 */
void vfs_inode_knote_notify(struct vfs_inode *inode, uint32 fflags) {
    if (inode == NULL)
        return;
    /* Fast path: skip spinlock when no watchers are registered */
    if (LIST_IS_EMPTY(&inode->knote_list))
        return;
    spin_lock(&inode->knote_lock);
    struct knote *kn = NULL;
    struct knote *tmp = NULL;
    list_foreach_node_safe(&inode->knote_list, kn, tmp, source_entry) {
        if (kn->sfflags & fflags)
            knote_enqueue_with_data(kn, 0, fflags);
    }
    spin_unlock(&inode->knote_lock);
}

/*
 * kqueue_proc_notify - notify all EVFILT_PROC knotes on a thread
 */
void kqueue_proc_notify(struct thread *p, uint32 fflags, int64 data) {
    if (p == NULL)
        return;
    spin_lock(&p->kqueue_proc_lock);
    struct knote *kn = NULL;
    struct knote *tmp = NULL;
    list_foreach_node_safe(&p->kqueue_proc_knotes, kn, tmp, source_entry) {
        if (kn->sfflags & fflags)
            knote_enqueue_with_data(kn, data, fflags);
    }
    spin_unlock(&p->kqueue_proc_lock);
}

/*
 * kqueue_signal_notify - notify EVFILT_SIGNAL knotes when signal is delivered
 */
void kqueue_signal_notify(struct thread *p, int signo) {
    if (p == NULL || p->sigacts == NULL)
        return;
    if (signo < 1 || signo > NSIG)
        return;
    sigacts_t *sa = p->sigacts;
    spin_lock(&sa->kqueue_signal_lock);
    struct knote *kn = NULL;
    struct knote *tmp = NULL;
    list_foreach_node_safe(&sa->kqueue_signal_knotes[signo - 1], kn, tmp,
                           source_entry) {
        knote_enqueue_with_data(kn, kn->data + 1, 0);
    }
    spin_unlock(&sa->kqueue_signal_lock);
}

/* ========================================================================
 * kqueue fd creation
 * ======================================================================== */

/*
 * kqueue_create - allocate a new kqueue and return its file descriptor
 */
int kqueue_create(void) {
    struct kqueue *kq = kqueue_alloc_private();
    if (kq == NULL)
        return -ENOMEM;

    int fd = vfs_custom_fd_alloc(&kqueue_file_ops, kq, 0);
    if (fd < 0) {
        slab_free(kq);
        return fd;
    }

    /* Store back-pointer to the file so nested epoll can propagate. */
    struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, fd);
    if (f) {
        kq->file = f;
        vfs_fput(f);  /* drop lookup ref; fdtable still holds it */
    }
    return fd;
}

/* ========================================================================
 * kqueue_register - process a changelist of kevent operations
 * ======================================================================== */

/*
 * __kqueue_find_knote - find a registered knote by ident + filter
 */
static struct knote *__kqueue_find_knote(struct kqueue *kq, uint64 ident,
                                         int16 filter) {
    struct knote *kn = NULL;
    struct knote *tmp = NULL;
    list_foreach_node_safe(&kq->registered, kn, tmp, kq_entry) {
        if (kn->ident == ident && kn->filter == filter)
            return kn;
    }
    return NULL;
}

int kqueue_epoll_has_ident(struct kqueue *kq, uint64 ident)
{
    if (kq == NULL)
        return 0;

    int found = 0;
    spin_lock(&kq->lock);
    struct knote *kn = NULL;
    struct knote *tmp = NULL;
    list_foreach_node_safe(&kq->registered, kn, tmp, kq_entry) {
        if (kn->ident == ident &&
            (kn->filter == EVFILT_READ || kn->filter == EVFILT_WRITE)) {
            found = 1;
            break;
        }
    }
    spin_unlock(&kq->lock);
    return found;
}

static void __kqueue_epoll_disable_oneshot_ident(struct kqueue *kq,
                                                 uint64 ident)
{
    struct knote *kn = NULL;
    struct knote *tmp = NULL;

    list_foreach_node_safe(&kq->registered, kn, tmp, kq_entry) {
        if (kn->ident != ident ||
            (kn->filter != EVFILT_READ && kn->filter != EVFILT_WRITE))
            continue;

        kn->status |= KN_DISABLED;
        if (kn->status & KN_QUEUED) {
            list_node_detach(kn, ready_entry);
            kq->nready--;
            kn->status &= ~KN_QUEUED;
        }
        kn->status &= ~(KN_EDGE_ACTIVE | KN_LEVEL_SEEN);
    }
}

/*
 * __kqueue_detach_knote - detach a knote from source and remove from kqueue
 * Caller must hold kq->lock.
 */
static void __kqueue_detach_knote(struct kqueue *kq, struct knote *kn) {
    bool defer_free = kn->filter == EVFILT_TIMER;

    /* Remove from ready list if queued */
    if (kn->status & KN_QUEUED) {
        list_node_detach(kn, ready_entry);
        kq->nready--;
        kn->status &= ~KN_QUEUED;
    }
    /* Remove from registered list */
    list_node_detach(kn, kq_entry);
    kq->nregistered--;
    kn->status |= KN_DETACHED;

    /* Detach from event source (outside kq lock) */
    spin_unlock(&kq->lock);
    if (kn->ops && kn->ops->detach)
        kn->ops->detach(kn);
    spin_lock(&kq->lock);

    /* EVFILT_TIMER uses sched_timer_add(), which allocates uncancelable timer
     * work items that may still callback with this knote pointer after detach.
     * Keeping detached timer knotes alive avoids a use-after-free until that
     * path is made cancelable.
     */
    if (defer_free)
        return;

    knote_free(kn);
}

/*
 * kqueue_register - process an array of kevent change operations
 */
int kqueue_register(struct kqueue *kq, struct kevent *changelist,
                    int nchanges) {
    if (kq == NULL)
        return -EINVAL;

    for (int i = 0; i < nchanges; i++) {
        struct kevent *kev = &changelist[i];
        struct knote_ops *ops = knote_get_ops(kev->filter);
        if (ops == NULL) {
            /* Report error for this event */
            kev->flags = EV_ERROR;
            kev->data = -EINVAL;
            continue;
        }

        spin_lock(&kq->lock);

        if (kev->flags & EV_ADD) {
            struct knote *kn = __kqueue_find_knote(kq, kev->ident,
                                                    kev->filter);
            if (kn != NULL) {
                /* Update existing knote */
                kn->flags = kev->flags;
                kn->fflags = kev->fflags;
                kn->sfflags = kev->fflags;
                kn->data = kev->data;
                kn->udata = kev->udata;
                if (kev->flags & EV_DISABLE)
                    kn->status |= KN_DISABLED;
                else
                    kn->status &= ~KN_DISABLED;
                spin_unlock(&kq->lock);

                /*
                 * EV_ADD also acts as a modify operation for an existing
                 * registration.  Linux epoll users commonly change masks after
                 * partially handling buffered IPC.  If readiness became true
                 * before the MOD, no new edge may arrive, so re-check the
                 * source after updating the knote.
                 */
                if (!(kn->status & KN_DISABLED) &&
                    kn->ops != NULL && kn->ops->event != NULL &&
                    kn->ops->event(kn, 0)) {
                    knote_enqueue(kn);
                }
            } else {
                /* Create new knote */
                spin_unlock(&kq->lock);

                kn = knote_alloc();
                if (kn == NULL) {
                    kev->flags = EV_ERROR;
                    kev->data = -ENOMEM;
                    continue;
                }
                kn->kq = kq;
                kn->ident = kev->ident;
                kn->filter = kev->filter;
                kn->flags = kev->flags;
                kn->fflags = kev->fflags;
                kn->sfflags = kev->fflags;
                kn->data = kev->data;
                kn->udata = kev->udata;
                kn->ops = ops;
                kn->status = KN_ACTIVE;

                if (kev->flags & EV_DISABLE)
                    kn->status |= KN_DISABLED;

                /* Attach to event source */
                int ret = 0;
                if (ops->attach)
                    ret = ops->attach(kn);
                if (ret < 0) {
                    kev->flags = EV_ERROR;
                    kev->data = ret;
                    knote_free(kn);
                    continue;
                }

                /* Add to kqueue's registered list */
                spin_lock(&kq->lock);
                list_node_push(&kq->registered, kn, kq_entry);
                kq->nregistered++;
                spin_unlock(&kq->lock);

                /* Check if event is already active (e.g. pipe already has data) */
                if (ops->event) {
                    int active = ops->event(kn, 0);
                    if (active)
                        knote_enqueue(kn);
                }
            }
        } else if (kev->flags & EV_DELETE) {
            struct knote *kn = __kqueue_find_knote(kq, kev->ident,
                                                    kev->filter);
            if (kn == NULL) {
                spin_unlock(&kq->lock);
                kev->flags = EV_ERROR;
                kev->data = -ENOENT;
                continue;
            }
            __kqueue_detach_knote(kq, kn);
            spin_unlock(&kq->lock);
        } else if (kev->flags & EV_ENABLE) {
            struct knote *kn = __kqueue_find_knote(kq, kev->ident,
                                                    kev->filter);
            if (kn != NULL) {
                kn->status &= ~KN_DISABLED;
                /* Re-check if event is currently active */
                spin_unlock(&kq->lock);
                if (kn->ops && kn->ops->event) {
                    int active = kn->ops->event(kn, 0);
                    if (active)
                        knote_enqueue(kn);
                }
            } else {
                spin_unlock(&kq->lock);
                kev->flags = EV_ERROR;
                kev->data = -ENOENT;
            }
        } else if (kev->flags & EV_DISABLE) {
            struct knote *kn = __kqueue_find_knote(kq, kev->ident,
                                                    kev->filter);
            if (kn != NULL) {
                kn->status |= KN_DISABLED;
            } else {
                kev->flags = EV_ERROR;
                kev->data = -ENOENT;
            }
            spin_unlock(&kq->lock);
        } else {
            spin_unlock(&kq->lock);
            kev->flags = EV_ERROR;
            kev->data = -EINVAL;
        }
    }
    return 0;
}

/* ========================================================================
 * kqueue_wait - wait for events and copy them out
 * ======================================================================== */

/* --- Timed-wait helpers ------------------------------------------------- */

struct __kq_timed_data {
    spinlock_t *lock;
    struct timer_node *tn;
    uint64 timeout_ms;
    bool timer_armed;
};

/*
 * Sleep callback: arm a one-shot timer that wakes the *current* thread
 * (via __sched_timer_callback / wakeup()), then release the kqueue lock.
 * Called inside tq_wait_in_state_cb with interrupts already disabled and
 * the thread already on the wait queue.
 */
static int __kq_timed_sleep_cb(void *data) {
    struct __kq_timed_data *d = (struct __kq_timed_data *)data;
    d->timer_armed = sched_timer_set(d->tn, d->timeout_ms) == 0;
    int status = spin_sleep_cb(d->lock);
    if (!d->timer_armed)
        scheduler_wakeup(current);
    return status;
}

/*
 * Wakeup callback: cancel the timer (no-op if already fired), then
 * re-acquire the kqueue lock.
 */
static void __kq_timed_wakeup_cb(void *data, int sleep_cb_status) {
    struct __kq_timed_data *d = (struct __kq_timed_data *)data;
    if (d->timer_armed)
        sched_timer_done(d->tn);
    spin_wake_cb(d->lock, sleep_cb_status);
}

/*
 * kqueue_wait - block until events are available, then return them
 *
 * @kq: the kqueue
 * @eventlist: output array of kevent structs
 * @nevents: max events to return
 * @timeout_ms: -1 = block forever, 0 = poll, >0 = timeout in ms
 *
 * Returns number of events delivered, or negative errno.
 */
int kqueue_wait(struct kqueue *kq, struct kevent *eventlist, int nevents,
                int timeout_ms) {
    if (kq == NULL || eventlist == NULL || nevents <= 0)
        return -EINVAL;

    int total = 0;
    uint64 *epoll_oneshot_idents = NULL;
    int epoll_oneshot_count = 0;

    if (kq->flags & KQ_EPOLL_COMPAT) {
        epoll_oneshot_idents = kvmalloc((size_t)nevents *
                                        sizeof(*epoll_oneshot_idents));
        if (epoll_oneshot_idents == NULL)
            return -ENOMEM;
    }

    spin_lock(&kq->lock);
    kq->waiters++;

    while (total == 0) {
        /* Check if kqueue was closed (e.g. process exiting) */
        if (kq->closed) {
            total = -EBADF;
            break;
        }

        kqueue_rescan_registered_locked(kq);

        /* Drain ready list */
        while (total < nevents && !LIST_IS_EMPTY(&kq->ready)) {
            struct knote *kn = kqueue_pick_ready_locked(kq);
            list_node_detach(kn, ready_entry);
            kq->nready--;
            kn->status &= ~KN_QUEUED;

            /*
             * EVFILT_READ/WRITE are level-triggered by default.  A readiness
             * edge can become stale while the knote sits on the ready list
             * (for example, epoll_ctl() can queue an unconnected socket error,
             * then a nonblocking connect moves the socket to EINPROGRESS).
             * Re-check before reporting level-triggered file readiness.
             */
            int poll_revents = 0;
            if (knote_is_file_filter(kn)) {
                poll_revents = knote_file_poll_revents(kn);
                if (poll_revents == 0) {
                    kn->status &= ~(KN_EDGE_ACTIVE | KN_LEVEL_SEEN);
                    if (!knote_is_clear_file_filter(kn))
                        continue;
                } else if (!knote_is_clear_file_filter(kn) &&
                           !(kq->flags & KQ_EPOLL_COMPAT)) {
                    kn->status |= KN_LEVEL_SEEN;
                }
            }

            /* Fill in kevent for user */
            eventlist[total].ident = kn->ident;
            eventlist[total].filter = kn->filter;
            eventlist[total].flags = kn->flags & ~(EV_ADD | EV_DELETE |
                                                    EV_ENABLE | EV_DISABLE);
            if (poll_revents & (POLLHUP | POLLRDHUP))
                eventlist[total].flags |= EV_EOF;
            if (poll_revents & (POLLERR | POLLNVAL))
                eventlist[total].flags |= EV_ERROR;
            eventlist[total].fflags =
                ((kq->flags & KQ_EPOLL_COMPAT) && knote_is_file_filter(kn))
                    ? kn->sfflags
                    : kn->fflags;
            eventlist[total].data =
                ((kq->flags & KQ_EPOLL_COMPAT) && knote_is_file_filter(kn))
                    ? poll_revents
                    : kn->data;
            eventlist[total].udata = kn->udata;
            total++;

            /* Handle EV_CLEAR: reset fflags and data after delivery */
            if (kn->flags & EV_CLEAR) {
                kn->fflags = 0;
                kn->data = 0;
                if (knote_is_clear_file_filter(kn)) {
                    if (knote_file_poll_revents(kn) != 0)
                        kn->status |= KN_EDGE_ACTIVE;
                    else
                        kn->status &= ~KN_EDGE_ACTIVE;
                }
            }

            /* Handle EV_ONESHOT.  BSD kqueue deletes one-shot knotes after
             * delivery.  Linux epoll keeps EPOLLONESHOT registrations and
             * disables the whole fd until epoll_ctl(MOD) rearms it. */
            if (kn->flags & EV_ONESHOT) {
                if (kq->flags & KQ_EPOLL_COMPAT) {
                    int seen = 0;
                    for (int i = 0; i < epoll_oneshot_count; i++) {
                        if (epoll_oneshot_idents[i] == kn->ident) {
                            seen = 1;
                            break;
                        }
                    }
                    if (!seen && epoll_oneshot_count < nevents)
                        epoll_oneshot_idents[epoll_oneshot_count++] =
                            kn->ident;
                } else {
                    __kqueue_detach_knote(kq, kn);
                }
            }
        }

        if (total > 0)
            break;

        /* Non-blocking poll */
        if (timeout_ms == 0)
            break;

        /* Check for pending signals */
        if (signal_pending(current)) {
            total = -EINTR;
            break;
        }

        /* Sleep on kqueue wait queue */
        if (timeout_ms > 0) {
            /* For timed wait: arm a per-thread timer that wakes us
             * directly, then sleep on the kqueue wait queue.  The
             * timer is cancelled on wakeup so there is no stale
             * reference to the kqueue after close/free. */
            struct timer_node __tn = {0};
            struct __kq_timed_data __td = {
                .lock = &kq->lock,
                .tn = &__tn,
                .timeout_ms = timeout_ms,
            };
            tq_wait_in_state_cb(&kq->waitq, __kq_timed_sleep_cb,
                                __kq_timed_wakeup_cb, &__td, NULL,
                                THREAD_INTERRUPTIBLE);
            /* After wakeup, timeout or event or signal — drain the
             * ready list on the next iteration.  If it's still empty
             * (pure timeout), exit the loop. */
            if (kq->closed) {
                total = -EBADF;
                break;
            }
            if (LIST_IS_EMPTY(&kq->ready))
                break;
        } else {
            /* timeout_ms == -1: block indefinitely */
            tq_wait_in_state(&kq->waitq, &kq->lock, NULL,
                             THREAD_INTERRUPTIBLE);
            if (kq->closed) {
                total = -EBADF;
                break;
            }
            if (signal_pending(current)) {
                total = -EINTR;
                break;
            }
        }
    }

    if (epoll_oneshot_count > 0) {
        for (int i = 0; i < epoll_oneshot_count; i++)
            __kqueue_epoll_disable_oneshot_ident(kq, epoll_oneshot_idents[i]);
    }

    kq->waiters--;
    int should_free = (kq->closed && kq->waiters == 0);
    spin_unlock(&kq->lock);
    if (should_free)
        slab_free(kq);
    kvfree(epoll_oneshot_idents);
    return total;
}

/* ========================================================================
 * kqueue close / file ops
 * ======================================================================== */

/*
 * kqueue_close - tear down a kqueue: detach all knotes, free resources
 */
static void kqueue_close(struct kqueue *kq) {
    if (kq == NULL)
        return;

    spin_lock(&kq->lock);
    kq->closed = 1;

    /* Detach and free all registered knotes */
    while (!LIST_IS_EMPTY(&kq->registered)) {
        struct knote *kn = LIST_FIRST_NODE(&kq->registered, struct knote,
                                           kq_entry);
        __kqueue_detach_knote(kq, kn);
    }

    /* Wake all waiters so they see the close */
    tq_wakeup_all(&kq->waitq, -EBADF, 0);
    int should_free = (kq->waiters == 0);
    spin_unlock(&kq->lock);

    /* Only free if no threads are inside kqueue_wait().
     * Otherwise the last waiter to leave kqueue_wait() will free it. */
    if (should_free)
        slab_free(kq);
}

void kqueue_close_private(struct kqueue *kq) {
    kqueue_close(kq);
}

static int kqueue_file_release(struct vfs_inode *inode, struct vfs_file *file) {
    (void)inode;
    struct kqueue *kq = (struct kqueue *)file->private_data;
    kq->file = NULL;   /* break back-pointer before teardown */
    kqueue_close(kq);
    file->private_data = NULL;
    return 0;
}

static int kqueue_file_poll(struct vfs_file *file, short events) {
    struct kqueue *kq = (struct kqueue *)file->private_data;
    short revents = 0;
    if (kq == NULL)
        return POLLNVAL;
    spin_lock(&kq->lock);
    if (events & (POLLIN | POLLRDNORM | POLLRDBAND | POLLRDHUP))
        kqueue_rescan_registered_locked(kq);
    if ((events & (POLLIN | POLLRDNORM | POLLRDBAND | POLLRDHUP)) &&
        kq->nready > 0)
        revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND | POLLRDHUP));
    spin_unlock(&kq->lock);
    return revents;
}
