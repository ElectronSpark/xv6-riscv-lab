/*
 * kqueue_types.h - Internal kernel types for the kqueue subsystem
 */
#ifndef __KERNEL_KQUEUE_TYPES_H
#define __KERNEL_KQUEUE_TYPES_H

#include "types.h"
#include "list_type.h"
#include "lock/spinlock.h"
#include "proc/tq_type.h"
#include "kqueue.h"

struct vfs_file;
struct vfs_inode;
struct knote;  /* forward declaration for knote_ops */

/*
 * Knote status bits
 */
#define KN_ACTIVE       0x01    /* knote is active (not disabled) */
#define KN_QUEUED       0x02    /* knote is on the ready list */
#define KN_DISABLED     0x04    /* knote is disabled */
#define KN_DETACHED     0x08    /* knote has been detached from source */
#define KN_TIMER        0x10    /* knote has an active timer */
#define KN_EDGE_ACTIVE  0x20    /* EV_CLEAR file filter is currently ready */
#define KN_LEVEL_SEEN   0x40    /* raw kqueue level readiness delivered once */

#define KQ_EPOLL_COMPAT 0x01    /* descriptor was created by epoll_create* */

/*
 * struct knote_ops - per-filter operation vtable
 */
struct knote_ops {
    int  (*attach)(struct knote *kn);   /* attach to event source */
    void (*detach)(struct knote *kn);   /* detach from event source */
    int  (*event)(struct knote *kn, long hint); /* check if event is active */
};

/*
 * struct knote - one registered event filter
 *
 * Lives on two lists simultaneously:
 *   - kqueue::registered (via kq_entry) — all registered knotes
 *   - kqueue::ready      (via ready_entry) — triggered knotes awaiting delivery
 * And optionally on a source-specific list:
 *   - vfs_file::knote_list / vfs_inode::knote_list / thread::kqueue_proc_knotes
 *     / sigacts_t::kqueue_signal_knotes[]  (via source_entry)
 */
struct knote {
    /* Linkage on kqueue's registered list */
    list_node_t kq_entry;

    /* Linkage on kqueue's ready list */
    list_node_t ready_entry;

    /* Linkage on the event source's knote list */
    list_node_t source_entry;

    /* Back-pointer to owning kqueue */
    struct kqueue *kq;

    /* User-visible kevent fields */
    uint64 ident;
    int16 filter;
    uint16 flags;
    uint32 fflags;
    uint32 sfflags;     /* subscribed fflags (preserved across EV_CLEAR) */
    int64 data;
    uint64 udata;

    /* Internal state */
    uint32 status;

    /* Filter operations */
    struct knote_ops *ops;

    /* Filter-specific state */
    struct vfs_file *attached_file;     /* EVFILT_READ/WRITE: tracked file */
    struct vfs_inode *attached_inode;   /* EVFILT_VNODE: tracked inode */
    int attached_pid;                   /* EVFILT_PROC: tracked PID */
    int64 timer_ms;                     /* EVFILT_TIMER: saved interval (ms) */
};

/*
 * struct kqueue - the kqueue descriptor
 *
 * One per kqueue fd. Holds the registered knote list, the ready list,
 * and a wait queue for threads blocking in kevent_wait().
 */
struct kqueue {
    spinlock_t lock;                /* protects all kqueue state */
    tq_t waitq;                    /* threads blocking in kevent_wait */
    list_node_t registered;        /* all registered knotes (via kn->kq_entry) */
    list_node_t ready;             /* triggered knotes (via kn->ready_entry) */
    int nregistered;               /* count of registered knotes */
    int nready;                    /* count of ready knotes */
    int closed;                    /* set when kqueue is being torn down */
    int waiters;                   /* threads currently inside kqueue_wait() */
    uint32 flags;                  /* KQ_* compatibility flags */
    struct vfs_file *file;         /* back-pointer to owning vfs_file (for nested epoll) */
};

/* Forward declarations for kqueue core API (used by hooks) */
void knote_enqueue(struct knote *kn);
void knote_enqueue_with_data(struct knote *kn, int64 data, uint32 fflags);
void vfs_file_knote_notify(struct vfs_file *file, int filter, int64 data);
void vfs_inode_knote_notify(struct vfs_inode *inode, uint32 fflags);
void kqueue_proc_notify(struct thread *p, uint32 fflags, int64 data);
void kqueue_signal_notify(struct thread *p, int signo);

/* Core kqueue API (used by syscall layer) */
int kqueue_create(void);
struct kqueue *kqueue_alloc_private(void);
void kqueue_close_private(struct kqueue *kq);
struct kqueue *kqueue_from_file(struct vfs_file *file);
int kqueue_file_is_epoll(struct vfs_file *file);
int kqueue_epoll_contains_kqueue(struct kqueue *root, struct kqueue *needle,
                                 int depth_limit);
int kqueue_epoll_has_ident(struct kqueue *kq, uint64 ident);
int kqueue_register(struct kqueue *kq, struct kevent *changelist, int nchanges);
int kqueue_wait(struct kqueue *kq, struct kevent *eventlist, int nevents,
                int timeout_ms);

/* Initialization */
void kqueue_init(void);

#endif /* __KERNEL_KQUEUE_TYPES_H */
