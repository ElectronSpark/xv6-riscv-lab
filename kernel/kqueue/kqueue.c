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
    kn->status |= KN_QUEUED;
    list_node_push(&kq->ready, kn, ready_entry);
    kq->nready++;
    spin_unlock(&kq->lock);

    /* Wake one waiter outside the lock */
    tq_wakeup(&kq->waitq, 0, 0);
}

/*
 * knote_enqueue_with_data - enqueue with updated data and fflags
 */
void knote_enqueue_with_data(struct knote *kn, int64 data, uint32 fflags) {
    if (kn == NULL)
        return;
    struct kqueue *kq = kn->kq;
    if (kq == NULL || kq->closed)
        return;

    spin_lock(&kq->lock);
    if ((kn->status & KN_DISABLED) || (kn->status & KN_DETACHED)) {
        spin_unlock(&kq->lock);
        return;
    }
    kn->data = data;
    kn->fflags |= fflags;
    if (!(kn->status & KN_QUEUED)) {
        kn->status |= KN_QUEUED;
        list_node_push(&kq->ready, kn, ready_entry);
        kq->nready++;
    }
    spin_unlock(&kq->lock);
    tq_wakeup(&kq->waitq, 0, 0);
}

/*
 * vfs_file_knote_notify - walk a file's knote list and enqueue matching knotes
 *
 * @file: the vfs_file with attached knotes
 * @filter: EVFILT_READ or EVFILT_WRITE
 * @data: filter-specific data (e.g. bytes available)
 */
void vfs_file_knote_notify(struct vfs_file *file, int filter, int64 data) {
    if (file == NULL)
        return;
    spin_lock(&file->knote_lock);
    struct knote *kn = NULL;
    struct knote *tmp = NULL;
    list_foreach_node_safe(&file->knote_list, kn, tmp, source_entry) {
        if (kn->filter == filter) {
            spin_unlock(&file->knote_lock);
            knote_enqueue_with_data(kn, data, 0);
            spin_lock(&file->knote_lock);
        }
    }
    spin_unlock(&file->knote_lock);
}

/*
 * vfs_inode_knote_notify - walk an inode's knote list and enqueue for vnode events
 */
void vfs_inode_knote_notify(struct vfs_inode *inode, uint32 fflags) {
    if (inode == NULL)
        return;
    spin_lock(&inode->knote_lock);
    struct knote *kn = NULL;
    struct knote *tmp = NULL;
    list_foreach_node_safe(&inode->knote_list, kn, tmp, source_entry) {
        if (kn->sfflags & fflags) {
            spin_unlock(&inode->knote_lock);
            knote_enqueue_with_data(kn, 0, fflags);
            spin_lock(&inode->knote_lock);
        }
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
        if (kn->sfflags & fflags) {
            spin_unlock(&p->kqueue_proc_lock);
            knote_enqueue_with_data(kn, data, fflags);
            spin_lock(&p->kqueue_proc_lock);
        }
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
    /* Walk kqueue_signal_knotes[signo-1] under the sigacts lock (already held
     * by caller in __signal_send) — but we use our own dedicated lock to avoid
     * lock ordering issues. The signal knotes list has its own spinlock. */
    spin_lock(&sa->kqueue_signal_lock);
    struct knote *kn = NULL;
    struct knote *tmp = NULL;
    list_foreach_node_safe(&sa->kqueue_signal_knotes[signo - 1], kn, tmp,
                           source_entry) {
        spin_unlock(&sa->kqueue_signal_lock);
        knote_enqueue_with_data(kn, kn->data + 1, 0);
        spin_lock(&sa->kqueue_signal_lock);
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
    struct kqueue *kq = slab_alloc(&__kqueue_cache);
    if (kq == NULL)
        return -ENOMEM;

    memset(kq, 0, sizeof(*kq));
    spin_init(&kq->lock, "kqueue");
    tq_init(&kq->waitq, "kqueue_waitq", NULL);
    list_entry_init(&kq->registered);
    list_entry_init(&kq->ready);
    kq->nregistered = 0;
    kq->nready = 0;
    kq->closed = 0;

    int fd = vfs_custom_fd_alloc(&kqueue_file_ops, kq, 0);
    if (fd < 0) {
        slab_free(kq);
        return fd;
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

/*
 * __kqueue_detach_knote - detach a knote from source and remove from kqueue
 * Caller must hold kq->lock.
 */
static void __kqueue_detach_knote(struct kqueue *kq, struct knote *kn) {
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
    int timer_armed = 0;

    spin_lock(&kq->lock);

    while (total == 0) {
        /* Drain ready list */
        while (total < nevents && !LIST_IS_EMPTY(&kq->ready)) {
            struct knote *kn = LIST_FIRST_NODE(&kq->ready, struct knote,
                                               ready_entry);
            list_node_detach(kn, ready_entry);
            kq->nready--;
            kn->status &= ~KN_QUEUED;

            /* Fill in kevent for user */
            eventlist[total].ident = kn->ident;
            eventlist[total].filter = kn->filter;
            eventlist[total].flags = kn->flags & ~(EV_ADD | EV_DELETE |
                                                    EV_ENABLE | EV_DISABLE);
            eventlist[total].fflags = kn->fflags;
            eventlist[total].data = kn->data;
            eventlist[total].udata = kn->udata;
            total++;

            /* Handle EV_ONESHOT: auto-delete after delivery */
            if (kn->flags & EV_ONESHOT) {
                __kqueue_detach_knote(kq, kn);
            }

            /* Handle EV_CLEAR: reset fflags and data after delivery */
            if (kn->flags & EV_CLEAR) {
                kn->fflags = 0;
                kn->data = 0;
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

        /* Set up timeout timer if needed */
        if (timeout_ms > 0 && !timer_armed) {
            /* Use sched_timer_set for timeout - it puts the current thread
             * into a TIMER state. Instead, we use a simpler approach:
             * arm a deadline timer that wakes our wait queue. */
            timer_armed = 1;
            /* We'll use tq_wait_in_state with THREAD_INTERRUPTIBLE and
             * rely on a separate timeout mechanism below */
        }

        /* Sleep on kqueue wait queue */
        if (timeout_ms > 0) {
            /* For timed wait: use a timer callback that wakes us */
            int ret = sched_timer_add(
                (void (*)(void *))tq_wakeup_all, &kq->waitq, timeout_ms);
            if (ret < 0) {
                total = ret;
                break;
            }
            tq_wait_in_state(&kq->waitq, &kq->lock, NULL,
                             THREAD_INTERRUPTIBLE);
            /* After wakeup, timeout or event or signal — drain the
             * ready list on the next iteration.  If it's still empty
             * (pure timeout), exit the loop. */
            if (LIST_IS_EMPTY(&kq->ready))
                break;
        } else {
            /* timeout_ms == -1: block indefinitely */
            tq_wait_in_state(&kq->waitq, &kq->lock, NULL,
                             THREAD_INTERRUPTIBLE);
            if (signal_pending(current)) {
                total = -EINTR;
                break;
            }
        }
    }

    spin_unlock(&kq->lock);
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
    spin_unlock(&kq->lock);

    slab_free(kq);
}

static int kqueue_file_release(struct vfs_inode *inode, struct vfs_file *file) {
    (void)inode;
    struct kqueue *kq = (struct kqueue *)file->private_data;
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
    if ((events & (POLLIN | POLLRDNORM)) && kq->nready > 0)
        revents |= (events & (POLLIN | POLLRDNORM));
    spin_unlock(&kq->lock);
    return revents;
}
