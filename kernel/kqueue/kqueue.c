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
#include "cmdline.h"
#include "param.h"
#include "lock/spinlock.h"
#include "lock/rcu.h"
#include "proc/thread.h"
#include "proc/tq.h"
#include "proc/sched.h"
#include "mm/slab.h"
#include "dev/cdev.h"
#include "vfs/file.h"
#include "vfs/vfs_types.h"
#include "vfs/poll.h"
#include "vfs/unix_socket.h"
#include "kqueue.h"
#include "kqueue_types.h"
#include "kqueue_graph_walk.h"
#include "kde_ready_trace.h"
#include "ksymbols.h"
#include "list.h"
#include "signal.h"
#include "timer/timer.h"
#include <mm/vm.h>

/* Slab caches */
static slab_cache_t __kqueue_cache = {0};
static slab_cache_t __knote_cache = {0};
static spinlock_t __kqueue_graph_lock;

/* Forward declarations */
static int kqueue_file_release(struct vfs_inode *inode, struct vfs_file *file);
static int kqueue_file_poll(struct vfs_file *file, short events);
static ssize_t kqueue_file_readlink(struct vfs_file *file, char *buf,
                                    size_t buflen);
static void knote_free(struct knote *kn);

static void konsole_prepty_trace_file(struct vfs_file *file, int filter,
                                      int64 data, int matched,
                                      int enqueued_new, int already_queued,
                                      int propagated, uint64 first_ident,
                                      uint64 last_ident, uint64 first_udata,
                                      uint64 last_udata,
                                      struct kqueue *first_kq,
                                      struct kqueue *last_kq,
                                      void *origin, int eventfd_op,
                                      uint64 eventfd_counter_before,
                                      uint64 eventfd_counter_after,
                                      uint64 eventfd_value,
                                      uint64 eventfd_read_value,
                                      int eventfd_ret,
                                      void *eventfd_caller);

/* External filter ops (defined in kqueue_filters.c) */
extern struct knote_ops knote_read_ops;
extern struct knote_ops knote_write_ops;
extern struct knote_ops knote_timer_ops;
extern struct knote_ops knote_signal_ops;
extern struct knote_ops knote_proc_ops;
extern struct knote_ops knote_vnode_ops;

/* File operations for a kqueue file descriptor */
static struct vfs_file_ops kqueue_file_ops = {
    .flags = VFS_FILE_OPS_F_POLL_NOTIFY_BACKED,
    .read = NULL,
    .write = NULL,
    .llseek = NULL,
    .release = kqueue_file_release,
    .fsync = NULL,
    .readlink = kqueue_file_readlink,
    .poll = kqueue_file_poll,
    .ioctl = NULL,
    .fault = NULL,
};

static ssize_t kqueue_file_readlink(struct vfs_file *file, char *buf,
                                    size_t buflen)
{
    static const char target[] = "anon_inode:[kqueue]";
    size_t len = sizeof(target) - 1;

    (void)file;
    if (buflen != 0) {
        size_t copy = len < buflen - 1 ? len : buflen - 1;
        memmove(buf, target, copy);
        buf[copy] = '\0';
    }
    return (ssize_t)len;
}

static int knote_is_file_filter(struct knote *kn)
{
    return kn->filter == EVFILT_READ || kn->filter == EVFILT_WRITE;
}

static void konsole_prepty_trace_file(struct vfs_file *file, int filter,
                                      int64 data, int matched,
                                      int enqueued_new, int already_queued,
                                      int propagated, uint64 first_ident,
                                      uint64 last_ident, uint64 first_udata,
                                      uint64 last_udata,
                                      struct kqueue *first_kq,
                                      struct kqueue *last_kq,
                                      void *origin, int eventfd_op,
                                      uint64 eventfd_counter_before,
                                      uint64 eventfd_counter_after,
                                      uint64 eventfd_value,
                                      uint64 eventfd_read_value,
                                      int eventfd_ret,
                                      void *eventfd_caller)
{
    kde_konsole_prepty_ring_record_notify_eventfd(
        file, filter, data, matched, enqueued_new, already_queued,
        propagated, first_ident, last_ident, first_udata, last_udata,
        first_kq, last_kq, origin, eventfd_op, eventfd_counter_before,
        eventfd_counter_after, eventfd_value, eventfd_read_value,
        eventfd_ret, eventfd_caller);
}

static int knote_is_clear_file_filter(struct knote *kn)
{
    return (kn->flags & EV_CLEAR) && knote_is_file_filter(kn);
}

static int knote_nonfile_event_active(struct knote *kn)
{
    assert(!knote_is_file_filter(kn),
           "knote_nonfile_event_active: file filter dispatched under kq lock");
    return kn->ops != NULL && kn->ops->event != NULL &&
           kn->ops->event(kn, 0);
}

struct knote_poll_snapshot {
    struct knote *kn;
    struct vfs_file *file_ref;
    struct vfs_file *file_identity;
    uint64 ident;
    int16 filter;
    uint16 flags;
    uint32 sfflags;
    uint32 kq_flags;
    uint64 generation;
    bool pinned;
};

static void kqueue_assert_poll_unlocked(struct kqueue *kq)
{
    push_off();
    int held = spin_holding(&kq->lock);
    pop_off();
    assert(!held, "kqueue: file poll dispatched while owning kq->lock");
}

static int knote_file_poll_dispatch(struct kqueue *kq,
                                    struct knote_poll_snapshot *snapshot)
{
    kqueue_assert_poll_unlocked(kq);

    struct vfs_file *f = snapshot->file_ref;
    if (f == NULL)
        return 0;

    if ((snapshot->kq_flags & KQ_EPOLL_COMPAT) &&
        __atomic_load_n(&f->visible_fd_refs, __ATOMIC_ACQUIRE) == 0) {
        return 0;
    }

    short events = snapshot->filter == EVFILT_READ
        ? (POLLIN | POLLPRI | POLLRDNORM | POLLRDBAND | POLLRDHUP)
        : (POLLOUT | POLLWRNORM | POLLWRBAND);
    int revents = 0;

    if (f->ops && f->ops->poll) {
        assert(f->ref_count > 0,
               "knote_file_poll_dispatch: stale file %p (ref=%d, ops=%p, ident=%ld)",
               f, f->ref_count, f->ops, snapshot->ident);
        revents = f->ops->poll(f, events);
    } else if (f->f_kind == VFS_FILE_KIND_CDEV && f->cdev != NULL &&
               f->cdev->ops.poll != NULL) {
        revents = f->cdev->ops.poll(f->cdev, events);
    }

    if (snapshot->kq_flags & KQ_EPOLL_COMPAT) {
        int always = revents & (POLLERR | POLLHUP | POLLNVAL);

        if (snapshot->filter == EVFILT_READ) {
            int requested = 0;
            if (snapshot->sfflags &
                (POLLIN | POLLPRI | POLLRDNORM | POLLRDBAND))
                requested |= revents & (POLLIN | POLLPRI | POLLRDNORM |
                                        POLLRDBAND);
            if (snapshot->sfflags & POLLRDHUP)
                requested |= revents & POLLRDHUP;
            revents = requested | always;
        } else {
            int requested = 0;
            if (snapshot->sfflags &
                (POLLOUT | POLLWRNORM | POLLWRBAND))
                requested |= revents & (POLLOUT | POLLWRNORM | POLLWRBAND);
            revents = requested | always;
        }
    }

    return revents;
}

static void knote_generation_advance_locked(struct kqueue *kq,
                                             struct knote *kn)
{
    assert(spin_holding(&kq->lock),
           "knote_generation_advance_locked: kq lock not held");
    kn->registration_generation++;
    if (kn->registration_generation == 0)
        kn->registration_generation = 1;
}

static bool knote_is_registered_locked(struct kqueue *kq, struct knote *target)
{
    struct knote *kn = NULL;
    struct knote *tmp = NULL;

    list_foreach_node_safe(&kq->registered, kn, tmp, kq_entry) {
        if (kn == target)
            return true;
    }
    return false;
}

static bool knote_poll_snapshot_begin_locked(
    struct kqueue *kq, struct knote *kn, struct knote_poll_snapshot *snapshot)
{
    assert(spin_holding(&kq->lock),
           "knote_poll_snapshot_begin_locked: kq lock not held");
    memset(snapshot, 0, sizeof(*snapshot));

    if (!knote_is_file_filter(kn) || kn->attached_file == NULL ||
        (kn->status & (KN_DISABLED | KN_DETACHED))) {
        return false;
    }

    struct vfs_file *file_ref = vfs_fdup(kn->attached_file);
    if (file_ref == NULL)
        return false;

    assert(kn->poll_refs != ~(uint32)0,
           "knote poll ref overflow ident=%lu filter=%d", kn->ident,
           kn->filter);
    kn->poll_refs++;

    snapshot->kn = kn;
    snapshot->file_ref = file_ref;
    snapshot->file_identity = kn->attached_file;
    snapshot->ident = kn->ident;
    snapshot->filter = kn->filter;
    snapshot->flags = kn->flags;
    snapshot->sfflags = kn->sfflags;
    snapshot->kq_flags = kq->flags;
    snapshot->generation = kn->registration_generation;
    snapshot->pinned = true;
    return true;
}

static bool knote_poll_snapshot_matches_locked(
    struct kqueue *kq, const struct knote_poll_snapshot *snapshot)
{
    struct knote *kn = snapshot->kn;

    assert(spin_holding(&kq->lock),
           "knote_poll_snapshot_matches_locked: kq lock not held");
    return snapshot->pinned && kn->kq == kq && !kq->closed &&
           kq->flags == snapshot->kq_flags &&
           !(kn->status & (KN_DISABLED | KN_DETACHED)) &&
           kn->attached_file == snapshot->file_identity &&
           kn->ident == snapshot->ident && kn->filter == snapshot->filter &&
           kn->flags == snapshot->flags && kn->sfflags == snapshot->sfflags &&
           kn->registration_generation == snapshot->generation &&
           knote_is_registered_locked(kq, kn) &&
           (!(snapshot->kq_flags & KQ_EPOLL_COMPAT) ||
            __atomic_load_n(&kn->attached_file->visible_fd_refs,
                            __ATOMIC_ACQUIRE) != 0);
}

static void knote_poll_snapshot_release_locked(
    struct kqueue *kq, struct knote_poll_snapshot *snapshot)
{
    if (!snapshot->pinned)
        return;

    assert(spin_holding(&kq->lock),
           "knote_poll_snapshot_release_locked: kq lock not held");
    struct knote *kn = snapshot->kn;
    assert(kn->poll_refs > 0, "knote poll ref underflow");
    kn->poll_refs--;
    bool free_now = kn->poll_refs == 0 && kn->poll_free_pending;
    snapshot->pinned = false;
    if (free_now)
        knote_free(kn);
}

/*
 * Poll one file registration.  The caller enters and returns with kq->lock
 * held.  The snapshot pins both objects while the arbitrary file/cdev poll
 * callback runs without the kqueue lock; stale results are rejected after
 * DEL, close, disable, MOD, fd close/reuse, or delete/re-add.
 */
static int knote_file_poll_locked(struct kqueue *kq, struct knote *kn,
                                  bool *valid, bool delivery_poll)
{
    struct knote_poll_snapshot snapshot;
    *valid = false;

    if (!knote_poll_snapshot_begin_locked(kq, kn, &snapshot)) {
        if (delivery_poll && !(kn->status & KN_DETACHED) &&
            knote_is_registered_locked(kq, kn))
            kn->status &= ~KN_DELIVERING;
        return 0;
    }

    spin_unlock(&kq->lock);
    int revents = knote_file_poll_dispatch(kq, &snapshot);
    vfs_fput(snapshot.file_ref);
    snapshot.file_ref = NULL;
    spin_lock(&kq->lock);

    *valid = knote_poll_snapshot_matches_locked(kq, &snapshot);
    if (!*valid && delivery_poll && !(kn->status & KN_DETACHED) &&
        knote_is_registered_locked(kq, kn)) {
        /* MOD/disable can invalidate a delivery poll without deleting the
         * registration.  Do not strand its coalescing state permanently. */
        kn->status &= ~KN_DELIVERING;
    }
    knote_poll_snapshot_release_locked(kq, &snapshot);
    return revents;
}

/*
 * Read/write filter vtables route here too.  This keeps every repository
 * file/cdev poll call behind the same pin/snapshot/unlocked-dispatch gate.
 */
int kqueue_file_filter_event_unlocked(struct knote *kn)
{
    if (kn == NULL || kn->kq == NULL || !knote_is_file_filter(kn))
        return 0;

    struct kqueue *kq = kn->kq;
    kqueue_assert_poll_unlocked(kq);
    spin_lock(&kq->lock);
    bool valid = false;
    int revents = knote_file_poll_locked(kq, kn, &valid, false);
    spin_unlock(&kq->lock);
    return valid && revents != 0;
}

static int kqueue_kde_spin_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("kde_kqueue_spin_trace", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static int kqueue_kde_spin_trace_konsole_only_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("kde_kqueue_spin_trace_konsole_only", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static int kqueue_kde_spin_trace_konsole_current(void)
{
    if (current == NULL)
        return 0;
    if (strncmp(current->name, "konsole", 7) == 0)
        return 1;
    if (current->thread_group != NULL &&
        strstr(current->thread_group->exec_path, "/konsole") != NULL)
        return 1;
    return 0;
}

static int kqueue_kde_spin_trace_current(void)
{
    if (kqueue_kde_spin_trace_konsole_only_enabled())
        return kqueue_kde_spin_trace_konsole_current();
    return current != NULL &&
        (kqueue_kde_spin_trace_konsole_current() ||
         strncmp(current->name, "QDBusConnection", 15) == 0 ||
         strncmp(current->name, "kwin_wayland", 12) == 0);
}

static int kqueue_count_registered_locked(struct kqueue *kq)
{
    int count = 0;
    struct knote *kn = NULL;
    struct knote *tmp = NULL;

    list_foreach_node_safe(&kq->registered, kn, tmp, kq_entry)
        count++;
    return count;
}

static void kqueue_rescan_registered_locked(struct kqueue *kq) {
    assert(spin_holding(&kq->lock),
           "kqueue_rescan_registered_locked: kq lock not held");

    uint64 high_water = kq->next_registration_id;
    uint64 cursor = 0;

    /*
     * A file poll drops kq->lock.  Never carry a list cursor across that
     * boundary.  Each rescan owns its cursor and start high-water snapshot;
     * after every relock it selects the lowest surviving registration id above
     * the cursor.  Concurrent scans cannot clobber one another, and ADD/re-add
     * after this pass began is intentionally left for the next pass.
     */
    for (;;) {
        struct knote *kn = NULL;
        struct knote *candidate = NULL;
        struct knote *tmp = NULL;
        list_foreach_node_safe(&kq->registered, kn, tmp, kq_entry) {
            if (kn->registration_id > cursor &&
                kn->registration_id <= high_water &&
                (candidate == NULL ||
                 kn->registration_id < candidate->registration_id)) {
                candidate = kn;
            }
        }
        if (candidate == NULL)
            break;

        kn = candidate;
        cursor = kn->registration_id;
        if ((kn->status &
             (KN_DISABLED | KN_QUEUED | KN_DETACHED | KN_DELIVERING)) ||
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
            bool valid = false;
            int revents = knote_file_poll_locked(kq, kn, &valid, false);

            if (!valid)
                continue;

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
        } else if (!knote_nonfile_event_active(kn)) {
            continue;
        }

        if (kn->status & (KN_DISABLED | KN_QUEUED | KN_DETACHED))
            continue;

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
    spin_init(&__kqueue_graph_lock, "kqueue_graph");
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

#define KQUEUE_GRAPH_MAX_NODES 1024

struct kqueue_graph_walk {
    struct kqueue_graph_walk_state state;
    void *references[KQUEUE_GRAPH_MAX_NODES];
    void *scratch[KQUEUE_GRAPH_MAX_NODES];
};

static void *kqueue_graph_file_identity(void *context, void *reference)
{
    (void)context;
    return kqueue_from_file((struct vfs_file *)reference);
}

static void kqueue_graph_file_release(void *context, void *reference)
{
    (void)context;
    vfs_fput((struct vfs_file *)reference);
}

static int kqueue_graph_snapshot_children(void *context, void *parent_reference,
                                          void **children, int capacity,
                                          int *count)
{
    (void)context;
    *count = 0;
    struct kqueue *node =
        kqueue_from_file((struct vfs_file *)parent_reference);
    if (node == NULL)
        return 0;

    spin_lock(&node->lock);
    struct knote *kn = NULL;
    struct knote *tmp = NULL;
    int error = 0;
    list_foreach_node_safe(&node->registered, kn, tmp, kq_entry) {
        if (!knote_is_file_filter(kn) || kn->attached_file == NULL ||
            (kn->status & KN_DETACHED) ||
            kqueue_from_file(kn->attached_file) == NULL)
            continue;
        if (*count == capacity) {
            error = -EOVERFLOW;
            break;
        }
        struct vfs_file *child_ref = vfs_fdup(kn->attached_file);
        if (child_ref == NULL) {
            error = -EBADF;
            break;
        }
        children[(*count)++] = child_ref;
    }
    spin_unlock(&node->lock);
    return error;
}

static const struct kqueue_graph_walk_ops kqueue_graph_file_ops = {
    .identity = kqueue_graph_file_identity,
    .snapshot_children = kqueue_graph_snapshot_children,
    .release = kqueue_graph_file_release,
};

static void kqueue_graph_walk_cleanup(struct kqueue_graph_walk *walk)
{
    kqueue_graph_walk_release_all(&kqueue_graph_file_ops, NULL, &walk->state);
    kvfree(walk);
}

/* __kqueue_graph_lock serializes every pollable-kqueue edge admission. */
static int kqueue_graph_reaches_locked(struct vfs_file *start_file,
                                       struct kqueue *needle,
                                       struct kqueue_graph_walk *walk)
{
    struct vfs_file *start_ref = vfs_fdup(start_file);
    if (start_ref == NULL)
        return -EBADF;
    walk->state.references = walk->references;
    walk->state.scratch = walk->scratch;
    walk->state.capacity = KQUEUE_GRAPH_MAX_NODES;
    return kqueue_graph_walk_reaches(&kqueue_graph_file_ops, NULL,
                                     &walk->state, start_ref, needle,
                                     -EOVERFLOW);
}

/*
 * Publish a newly attached registration.  Pollable kqueue-file edges are
 * serialized from reachability check through list insertion, so concurrent
 * A->B and B->A additions cannot both pass.  Traversal holds references and
 * never holds two kqueue locks at once; overflow fails closed.
 */
static int kqueue_admit_registration(struct kqueue *kq, struct knote *kn)
{
    struct kqueue *child = knote_is_file_filter(kn)
        ? kqueue_from_file(kn->attached_file)
        : NULL;

    if (child == NULL) {
        spin_lock(&kq->lock);
        if (kq->closed) {
            spin_unlock(&kq->lock);
            return -EBADF;
        }
        list_node_push(&kq->registered, kn, kq_entry);
        kq->nregistered++;
        spin_unlock(&kq->lock);
        return 0;
    }

    struct kqueue_graph_walk *walk = kvmalloc(sizeof(*walk));
    if (walk == NULL)
        return -ENOMEM;
    memset(walk, 0, sizeof(*walk));

    spin_lock(&__kqueue_graph_lock);
    int reaches = kqueue_graph_reaches_locked(kn->attached_file, kq, walk);
    int ret = 0;
    if (reaches > 0) {
        ret = -ELOOP;
    } else if (reaches < 0) {
        ret = reaches;
    } else {
        spin_lock(&kq->lock);
        if (kq->closed) {
            ret = -EBADF;
        } else {
            list_node_push(&kq->registered, kn, kq_entry);
            kq->nregistered++;
        }
        spin_unlock(&kq->lock);
    }
    spin_unlock(&__kqueue_graph_lock);

    kqueue_graph_walk_cleanup(walk);
    return ret;
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
        if (__atomic_load_n(&kn->attached_file->visible_fd_refs,
                            __ATOMIC_ACQUIRE) == 0)
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

/* Caller holds kq->lock.  Return a referenced backing file for propagation. */
static struct vfs_file *knote_enqueue_locked(struct knote *kn)
{
    struct kqueue *kq = kn->kq;

    assert(kq != NULL && spin_holding(&kq->lock),
           "knote_enqueue_locked: kq lock not held");
    if (kq->closed || (kn->status & (KN_DISABLED | KN_DETACHED))) {
        return NULL;
    }
    if (knote_is_clear_file_filter(kn))
        kn->status |= KN_EDGE_ACTIVE;
    if (kn->status & KN_DELIVERING) {
        kn->status |= KN_PENDING;
        tq_wakeup(&kq->waitq, 0, 0);
        return NULL;
    }
    if (kn->status & KN_QUEUED)
        return NULL;
    kn->status |= KN_QUEUED;
    list_node_push(&kq->ready, kn, ready_entry);
    kq->nready++;
    tq_wakeup(&kq->waitq, 0, 0);
    return kq->file ? vfs_fdup(kq->file) : NULL;
}

static void knote_propagate_file(struct vfs_file *kq_file)
{
    if (kq_file == NULL)
        return;
    vfs_file_knote_notify(kq_file, EVFILT_READ, 0);
    vfs_fput(kq_file);
}

/*
 * Move notifications observed while a knote was being delivered onto the
 * ready list only after the current drain is finished.  This coalesces any
 * number of synchronous callbacks into one next-wait event and prevents a
 * hot source from consuming maxevents repeatedly ahead of its peers.
 */
static struct vfs_file *kqueue_materialize_pending_locked(struct kqueue *kq,
                                                           bool take_ref)
{
    assert(spin_holding(&kq->lock),
           "kqueue_materialize_pending_locked: kq lock not held");
    bool added = false;
    struct knote *kn = NULL;
    struct knote *tmp = NULL;

    list_foreach_node_safe(&kq->registered, kn, tmp, kq_entry) {
        if (!(kn->status & KN_PENDING))
            continue;
        if (kn->status & (KN_DISABLED | KN_DETACHED)) {
            kn->status &= ~KN_PENDING;
            continue;
        }
        if (kn->status & KN_DELIVERING)
            continue;

        kn->status &= ~KN_PENDING;
        if (kn->status & KN_QUEUED)
            continue;
        kn->status |= KN_QUEUED;
        list_node_push(&kq->ready, kn, ready_entry);
        kq->nready++;
        added = true;
    }

    if (!added)
        return NULL;
    tq_wakeup(&kq->waitq, 0, 0);
    return take_ref && kq->file ? vfs_fdup(kq->file) : NULL;
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
    struct vfs_file *kq_file = knote_enqueue_locked(kn);
    spin_unlock(&kq->lock);

    /* Propagate readiness to any outer kqueue/poll monitoring this kqueue fd */
    knote_propagate_file(kq_file);
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
    if (kn->status & KN_DELIVERING) {
        kn->status |= KN_PENDING;
        tq_wakeup(&kq->waitq, 0, 0);
        spin_unlock(&kq->lock);
        return NULL;
    }
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
static void vfs_file_knote_notify_origin_eventfd(
    struct vfs_file *file, int filter, int64 data, void *origin,
    int eventfd_op, uint64 eventfd_counter_before,
    uint64 eventfd_counter_after, uint64 eventfd_value,
    uint64 eventfd_read_value, int eventfd_ret, void *eventfd_caller)
{
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
    int matched = 0;
    int enqueued_new = 0;
    int already_queued = 0;
    uint64 first_ident = 0;
    uint64 last_ident = 0;
    uint64 first_udata = 0;
    uint64 last_udata = 0;
    struct kqueue *first_kq = NULL;
    struct kqueue *last_kq = NULL;
    list_foreach_node_safe(&file->knote_list, kn, tmp, source_entry) {
        if (kn->filter == filter) {
            matched++;
            if (matched == 1) {
                first_ident = kn->ident;
                first_udata = kn->udata;
                first_kq = kn->kq;
            }
            last_ident = kn->ident;
            last_udata = kn->udata;
            last_kq = kn->kq;
            int was_queued = (kn->status & KN_QUEUED) != 0;
            struct vfs_file *kq_file =
                __knote_enqueue_core(kn, data, 0);
            if (was_queued)
                already_queued++;
            else if (kn->status & KN_QUEUED)
                enqueued_new++;
            if (kq_file) {
                if (nprop < MAX_KNOTE_PROPAGATE)
                    propagate[nprop++] = kq_file;
                else
                    vfs_fput(kq_file);
            }
        }
    }
    spin_unlock(&file->knote_lock);

    konsole_prepty_trace_file(file, filter, data, matched, enqueued_new,
                              already_queued, nprop, first_ident, last_ident,
                              first_udata, last_udata, first_kq, last_kq,
                              origin, eventfd_op, eventfd_counter_before,
                              eventfd_counter_after, eventfd_value,
                              eventfd_read_value, eventfd_ret,
                              eventfd_caller);

    /* Propagate to outer kqueues/polls without holding any knote_lock */
    for (int i = 0; i < nprop; i++) {
        vfs_file_knote_notify_origin_eventfd(
            propagate[i], EVFILT_READ, 0, origin, eventfd_op,
            eventfd_counter_before, eventfd_counter_after, eventfd_value,
            eventfd_read_value, eventfd_ret, eventfd_caller);
        vfs_fput(propagate[i]);
    }
}

void vfs_file_knote_notify(struct vfs_file *file, int filter, int64 data)
{
    vfs_file_knote_notify_origin_eventfd(
        file, filter, data, __builtin_return_address(0), 0, 0, 0, 0, 0,
        0, NULL);
}

void vfs_file_knote_notify_eventfd(
    struct vfs_file *file, int filter, int64 data, int eventfd_op,
    uint64 counter_before, uint64 counter_after, uint64 value,
    uint64 read_value, int ret, void *eventfd_caller)
{
    vfs_file_knote_notify_origin_eventfd(
        file, filter, data, __builtin_return_address(0), eventfd_op,
        counter_before, counter_after, value, read_value, ret,
        eventfd_caller);
}

/*
 * cdev_knote_notify - walk a character device's knote list and enqueue
 * matching knotes.
 *
 * Character device readiness is often shared device state rather than
 * per-open-file state.  /dev/kbd and /dev/mouse are examples: an input event
 * pushed by one fd must wake a compositor epolling a different fd for the
 * same device.
 */
void cdev_knote_notify(cdev_t *cdev, int filter, int64 data) {
    if (cdev == NULL)
        return;

    struct vfs_file *propagate[MAX_KNOTE_PROPAGATE];
    int nprop = 0;

    spin_lock(&cdev->knote_lock);
    struct knote *kn = NULL;
    struct knote *tmp = NULL;
    list_foreach_node_safe(&cdev->knote_list, kn, tmp, source_entry) {
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
    spin_unlock(&cdev->knote_lock);

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
            (kn->filter == EVFILT_READ || kn->filter == EVFILT_WRITE) &&
            (kn->attached_file == NULL ||
             __atomic_load_n(&kn->attached_file->visible_fd_refs,
                             __ATOMIC_ACQUIRE) != 0)) {
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

        knote_generation_advance_locked(kq, kn);
        kn->status |= KN_DISABLED;
        kn->status &= ~KN_PENDING;
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
    knote_generation_advance_locked(kq, kn);
    kn->status |= KN_DETACHED;
    kn->status &= ~(KN_PENDING | KN_DELIVERING);

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

    if (kn->poll_refs != 0) {
        kn->poll_free_pending = true;
        return;
    }
    knote_free(kn);
}

void kqueue_epoll_purge_closed_files(struct kqueue *kq)
{
    if (kq == NULL || !(kq->flags & KQ_EPOLL_COMPAT))
        return;

    for (;;) {
        struct knote *closed = NULL;

        spin_lock(&kq->lock);
        struct knote *kn = NULL;
        struct knote *tmp = NULL;
        list_foreach_node_safe(&kq->registered, kn, tmp, kq_entry) {
            if (!knote_is_file_filter(kn) || kn->attached_file == NULL)
                continue;
            if (__atomic_load_n(&kn->attached_file->visible_fd_refs,
                                __ATOMIC_ACQUIRE) == 0) {
                closed = kn;
                break;
            }
        }

        if (closed == NULL) {
            spin_unlock(&kq->lock);
            break;
        }

        __kqueue_detach_knote(kq, closed);
        spin_unlock(&kq->lock);
    }
}

/*
 * kqueue_register - process an array of kevent change operations
 */
int kqueue_register(struct kqueue *kq, struct kevent *changelist,
                    int nchanges) {
    if (kq == NULL)
        return -EINVAL;

    spin_lock(&kq->lock);
    if (kq->closed) {
        spin_unlock(&kq->lock);
        return -EBADF;
    }
    kq->registrars++;
    spin_unlock(&kq->lock);

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
                knote_generation_advance_locked(kq, kn);
                kn->flags = kev->flags;
                kn->fflags = kev->fflags;
                kn->sfflags = kev->fflags;
                kn->data = kev->data;
                kn->udata = kev->udata;
                if (kev->flags & EV_DISABLE)
                    kn->status |= KN_DISABLED;
                else
                    kn->status &= ~KN_DISABLED;
                if (kn->status & KN_DISABLED) {
                    kn->status &= ~KN_PENDING;
                    if (kn->status & KN_QUEUED) {
                        list_node_detach(kn, ready_entry);
                        kq->nready--;
                        kn->status &= ~KN_QUEUED;
                    }
                }

                /*
                 * EV_ADD also acts as a modify operation for an existing
                 * registration.  Linux epoll users commonly change masks after
                 * partially handling buffered IPC.  If readiness became true
                 * before the MOD, no new edge may arrive, so re-check the
                 * source after updating the knote.
                 */
                struct vfs_file *propagate = NULL;
                if (!(kn->status & KN_DISABLED) && kn->ops != NULL &&
                    kn->ops->event != NULL) {
                    bool valid = true;
                    int active = knote_is_file_filter(kn)
                        ? knote_file_poll_locked(kq, kn, &valid, false)
                        : knote_nonfile_event_active(kn);
                    if (valid && active)
                        propagate = knote_enqueue_locked(kn);
                }
                spin_unlock(&kq->lock);
                knote_propagate_file(propagate);
            } else {
                /* Create new knote */
                if (kq->next_registration_id == ~(uint64)0) {
                    spin_unlock(&kq->lock);
                    kev->flags = EV_ERROR;
                    kev->data = -ENOSPC;
                    continue;
                }
                uint64 registration_id = ++kq->next_registration_id;
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
                /* Keep source notifications inert until graph admission and
                 * registered-list publication are complete. */
                kn->status = KN_ACTIVE | KN_DISABLED;
                kn->registration_generation = 1;
                kn->registration_id = registration_id;
                kn->poll_refs = 1; /* publication pin across close/admission */

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

                ret = kqueue_admit_registration(kq, kn);
                if (ret < 0) {
                    if (ops->detach)
                        ops->detach(kn);
                    kev->flags = EV_ERROR;
                    kev->data = ret;
                    kn->poll_refs = 0;
                    knote_free(kn);
                    continue;
                }

                spin_lock(&kq->lock);
                if ((kn->status & KN_DETACHED) || kq->closed) {
                    assert(kn->poll_refs == 1,
                           "kqueue publication pin imbalance");
                    kn->poll_refs = 0;
                    bool free_now = kn->poll_free_pending;
                    spin_unlock(&kq->lock);
                    if (free_now)
                        knote_free(kn);
                    kev->flags = EV_ERROR;
                    kev->data = -EBADF;
                    continue;
                }
                if (!(kev->flags & EV_DISABLE))
                    kn->status &= ~KN_DISABLED;

                /* Check if event is already active (e.g. pipe already has data) */
                struct vfs_file *propagate = NULL;
                if (!(kn->status & KN_DISABLED) && ops->event) {
                    bool valid = true;
                    int active = knote_is_file_filter(kn)
                        ? knote_file_poll_locked(kq, kn, &valid, false)
                        : knote_nonfile_event_active(kn);
                    if (valid && active)
                        propagate = knote_enqueue_locked(kn);
                }
                assert(kn->poll_refs == 1,
                       "kqueue publication pin imbalance after poll");
                kn->poll_refs = 0;
                bool free_now = kn->poll_free_pending;
                spin_unlock(&kq->lock);
                knote_propagate_file(propagate);
                if (free_now)
                    knote_free(kn);
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
                knote_generation_advance_locked(kq, kn);
                kn->status &= ~KN_DISABLED;
                /* Re-check if event is currently active */
                struct vfs_file *propagate = NULL;
                if (kn->ops && kn->ops->event) {
                    bool valid = true;
                    int active = knote_is_file_filter(kn)
                        ? knote_file_poll_locked(kq, kn, &valid, false)
                        : knote_nonfile_event_active(kn);
                    if (valid && active)
                        propagate = knote_enqueue_locked(kn);
                }
                spin_unlock(&kq->lock);
                knote_propagate_file(propagate);
            } else {
                spin_unlock(&kq->lock);
                kev->flags = EV_ERROR;
                kev->data = -ENOENT;
            }
        } else if (kev->flags & EV_DISABLE) {
            struct knote *kn = __kqueue_find_knote(kq, kev->ident,
                                                    kev->filter);
            if (kn != NULL) {
                knote_generation_advance_locked(kq, kn);
                kn->status |= KN_DISABLED;
                kn->status &= ~KN_PENDING;
                if (kn->status & KN_QUEUED) {
                    list_node_detach(kn, ready_entry);
                    kq->nready--;
                    kn->status &= ~KN_QUEUED;
                }
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
    spin_lock(&kq->lock);
    kq->registrars--;
    int should_free = kq->closed && kq->waiters == 0 &&
                      kq->pollers == 0 && kq->registrars == 0;
    spin_unlock(&kq->lock);
    if (should_free)
        slab_free(kq);
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
    int pid;
    int trace;
    bool timer_armed;
    int timer_fired;
    int wake_valid;
    int wake_sleeping;
    uint64 fired_ms;
};

static void __kq_timed_timer_cb(struct timer_node *tn) {
    struct __kq_timed_data *d = (struct __kq_timed_data *)tn->data;
    if (d == NULL || d->pid <= 0)
        return;

    if (d->trace) {
        __atomic_store_n(&d->fired_ms, sched_timer_now_ms(),
                         __ATOMIC_RELAXED);
        __atomic_store_n(&d->timer_fired, 1, __ATOMIC_RELEASE);
    }

    rcu_read_lock();
    struct thread *live = NULL;
    int valid = get_pid_thread(d->pid, &live) == 0 && live != NULL;
    int sleeping = valid && THREAD_SLEEPING(live);
    if (d->trace) {
        __atomic_store_n(&d->wake_valid, valid, __ATOMIC_RELAXED);
        __atomic_store_n(&d->wake_sleeping, sleeping, __ATOMIC_RELAXED);
    }
    if (sleeping)
        wakeup(live);
    rcu_read_unlock();
}

/*
 * Sleep callback: arm a one-shot timer that wakes the *current* thread
 * (via __sched_timer_callback / wakeup()), then release the kqueue lock.
 * Called inside tq_wait_in_state_cb with interrupts already disabled and
 * the thread already on the wait queue.
 */
static int __kq_timed_sleep_cb(void *data) {
    struct __kq_timed_data *d = (struct __kq_timed_data *)data;
    if (d->trace) {
        d->timer_armed = sched_timer_set_cb(d->tn, d->timeout_ms,
                                            __kq_timed_timer_cb, d) == 0;
    } else {
        d->timer_armed = sched_timer_set(d->tn, d->timeout_ms) == 0;
    }
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
    struct vfs_file *pending_propagate = NULL;
    int trace_spin = kqueue_kde_spin_trace_enabled() &&
                     kqueue_kde_spin_trace_current();
    int trace_iter = 0;
    uint64 trace_start_ms = trace_spin ? sched_timer_now_ms() : 0;

    spin_lock(&kq->lock);
    kq->waiters++;

    while (total == 0) {
        trace_iter++;
        /* Check if kqueue was closed (e.g. process exiting) */
        if (kq->closed) {
            total = -EBADF;
            break;
        }

        kqueue_rescan_registered_locked(kq);
        if (trace_spin &&
            (trace_iter <= 16 || (trace_iter & 0x3ff) == 0)) {
            printf("kde-kqueue-wait: phase=rescan pid=%d name=%s "
                   "iter=%d timeout=%d nready=%d registered=%d "
                   "waiters=%d elapsed_ms=%lu\n",
                   current->pid, current->name, trace_iter, timeout_ms,
                   kq->nready, kqueue_count_registered_locked(kq),
                   kq->waiters, sched_timer_now_ms() - trace_start_ms);
        }

        /* Drain ready list */
        while (total < nevents && !LIST_IS_EMPTY(&kq->ready)) {
            struct knote *kn = kqueue_pick_ready_locked(kq);
            list_node_detach(kn, ready_entry);
            kq->nready--;
            kn->status &= ~KN_QUEUED;
            kn->status |= KN_DELIVERING;

            /*
             * EVFILT_READ/WRITE are level-triggered by default.  A readiness
             * edge can become stale while the knote sits on the ready list
             * (for example, epoll_ctl() can queue an unconnected socket error,
             * then a nonblocking connect moves the socket to EINPROGRESS).
             * Re-check before reporting level-triggered file readiness.
             */
            int poll_revents = 0;
            if (knote_is_file_filter(kn)) {
                bool valid = false;
                poll_revents =
                    knote_file_poll_locked(kq, kn, &valid, true);
                if (!valid)
                    continue;
                if (poll_revents == 0) {
                    kn->status &= ~(KN_EDGE_ACTIVE | KN_LEVEL_SEEN);
                    if (trace_spin &&
                        (trace_iter <= 16 || (trace_iter & 0x3ff) == 0)) {
                        printf("kde-kqueue-wait: stale pid=%d name=%s "
                               "iter=%d ident=%lu filter=%d flags=0x%x "
                               "status=0x%x\n",
                               current->pid, current->name, trace_iter,
                               kn->ident, kn->filter, kn->flags, kn->status);
                    }
                    if (!knote_is_clear_file_filter(kn)) {
                        kn->status &= ~KN_DELIVERING;
                        continue;
                    }
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
                    bool valid = false;
                    int clear_revents =
                        knote_file_poll_locked(kq, kn, &valid, true);
                    if (!valid)
                        continue;
                    if (clear_revents != 0)
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
                    /* Disable both read/write registrations before a callback
                     * deferred during delivery can be materialized. */
                    __kqueue_epoll_disable_oneshot_ident(kq, kn->ident);
                    kn->status &= ~KN_DELIVERING;
                } else {
                    __kqueue_detach_knote(kq, kn);
                }
            } else {
                kn->status &= ~KN_DELIVERING;
            }
        }

        struct vfs_file *new_propagate =
            kqueue_materialize_pending_locked(kq,
                                               pending_propagate == NULL);
        if (new_propagate != NULL)
            pending_propagate = new_propagate;

        if (total > 0)
            break;

        /* A stale event may have received a new synchronous notification
         * during its poll recheck.  Drain that deferred event in a fresh pass
         * instead of sleeping with a non-empty ready list. */
        if (!LIST_IS_EMPTY(&kq->ready))
            continue;

        /* A file poll above temporarily drops kq->lock. */
        if (kq->closed) {
            total = -EBADF;
            break;
        }

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
            if (trace_spin &&
                (trace_iter <= 16 || (trace_iter & 0x3ff) == 0)) {
                printf("kde-kqueue-wait: sleep pid=%d name=%s iter=%d "
                       "timeout=%d elapsed_ms=%lu\n",
                       current->pid, current->name, trace_iter, timeout_ms,
                       sched_timer_now_ms() - trace_start_ms);
            }
            /* For timed wait: arm a per-thread timer that wakes us
             * directly, then sleep on the kqueue wait queue.  The
             * timer is cancelled on wakeup so there is no stale
             * reference to the kqueue after close/free. */
            struct timer_node __tn = {0};
            struct __kq_timed_data __td = {
                .lock = &kq->lock,
                .tn = &__tn,
                .timeout_ms = timeout_ms,
                .pid = current->pid,
                .trace = trace_spin,
            };
            uint64 wait_start_ms = trace_spin ? sched_timer_now_ms() : 0;
            int wait_ret =
                tq_wait_in_state_cb(&kq->waitq, __kq_timed_sleep_cb,
                                    __kq_timed_wakeup_cb, &__td, NULL,
                                    THREAD_INTERRUPTIBLE);
            if (trace_spin &&
                (trace_iter <= 16 || (trace_iter & 0x3ff) == 0)) {
                uint64 wait_end_ms = sched_timer_now_ms();
                int timer_fired =
                    __atomic_load_n(&__td.timer_fired, __ATOMIC_ACQUIRE);
                uint64 fired_ms =
                    __atomic_load_n(&__td.fired_ms, __ATOMIC_RELAXED);
                int wake_valid =
                    __atomic_load_n(&__td.wake_valid, __ATOMIC_RELAXED);
                int wake_sleeping =
                    __atomic_load_n(&__td.wake_sleeping, __ATOMIC_RELAXED);
                uint64 dispatch_ms =
                    (timer_fired && fired_ms != 0 && wait_end_ms >= fired_ms)
                        ? wait_end_ms - fired_ms
                        : 0;
                uint64 overrun_ms =
                    (wait_end_ms >= wait_start_ms + (uint64)timeout_ms)
                        ? wait_end_ms - wait_start_ms - (uint64)timeout_ms
                        : 0;
                printf("kde-kqueue-wait: wake pid=%d name=%s iter=%d "
                       "timeout=%d wait_ret=%d slept_ms=%lu nready=%d closed=%d "
                       "elapsed_ms=%lu timer_armed=%d timer_fired=%d "
                       "wake_valid=%d wake_sleeping=%d dispatch_ms=%lu "
                       "overrun_ms=%lu\n",
                       current->pid, current->name, trace_iter, timeout_ms,
                       wait_ret, wait_end_ms - wait_start_ms, kq->nready,
                       kq->closed, wait_end_ms - trace_start_ms,
                       __td.timer_armed, timer_fired, wake_valid,
                       wake_sleeping, dispatch_ms, overrun_ms);
            }
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
            if (trace_spin &&
                (trace_iter <= 16 || (trace_iter & 0x3ff) == 0)) {
                printf("kde-kqueue-wait: sleep-inf pid=%d name=%s "
                       "iter=%d elapsed_ms=%lu\n",
                       current->pid, current->name, trace_iter,
                       sched_timer_now_ms() - trace_start_ms);
            }
            /* timeout_ms == -1: block indefinitely */
            uint64 wait_start_ms = trace_spin ? sched_timer_now_ms() : 0;
            tq_wait_in_state(&kq->waitq, &kq->lock, NULL,
                             THREAD_INTERRUPTIBLE);
            if (trace_spin &&
                (trace_iter <= 16 || (trace_iter & 0x3ff) == 0)) {
                uint64 wait_end_ms = sched_timer_now_ms();
                printf("kde-kqueue-wait: wake-inf pid=%d name=%s iter=%d "
                       "slept_ms=%lu nready=%d closed=%d elapsed_ms=%lu\n",
                       current->pid, current->name, trace_iter,
                       wait_end_ms - wait_start_ms, kq->nready, kq->closed,
                       wait_end_ms - trace_start_ms);
            }
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

    kq->waiters--;
    int should_free =
        (kq->closed && kq->waiters == 0 && kq->pollers == 0 &&
         kq->registrars == 0);
    spin_unlock(&kq->lock);
    knote_propagate_file(pending_propagate);
    if (should_free)
        slab_free(kq);
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
    int should_free =
        (kq->waiters == 0 && kq->pollers == 0 && kq->registrars == 0);
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
    spin_lock(&kq->lock);
    kq->file = NULL;   /* break back-pointer before teardown */
    spin_unlock(&kq->lock);
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
    kq->pollers++;
    if (events & (POLLIN | POLLRDNORM | POLLRDBAND | POLLRDHUP))
        kqueue_rescan_registered_locked(kq);
    if (kq->closed)
        revents = POLLNVAL;
    else if ((events & (POLLIN | POLLRDNORM | POLLRDBAND | POLLRDHUP)) &&
        kq->nready > 0)
        revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND | POLLRDHUP));
    kq->pollers--;
    int should_free =
        (kq->closed && kq->waiters == 0 && kq->pollers == 0 &&
         kq->registrars == 0);
    spin_unlock(&kq->lock);
    if (should_free)
        slab_free(kq);
    return revents;
}
