/*
 * epoll.c - Linux epoll(7) compatibility layer over BSD kqueue
 *
 * Implements the three epoll syscalls as thin wrappers around the existing
 * kqueue infrastructure:
 *
 *   sys_epoll_create1(flags) → fd
 *   sys_epoll_ctl(epfd, op, fd, event) → 0 / -errno
 *   sys_epoll_pwait(epfd, events, maxevents, timeout, sigmask) → n / -errno
 *
 * The epoll fd IS a kqueue fd internally.  epoll_ctl translates
 * struct epoll_event into kevent operations, and epoll_pwait translates
 * kevent results back into struct epoll_event.
 *
 * Mapping:
 *   EPOLLIN     → EVFILT_READ
 *   EPOLLOUT    → EVFILT_WRITE
 *   EPOLLET     → EV_CLEAR  (edge-triggered)
 *   EPOLLONESHOT→ EV_ONESHOT
 *   EPOLLHUP    → EV_EOF    (reported)
 *   EPOLLERR    → EV_ERROR  (reported)
 *
 * The user's epoll_data (64-bit) is stored in kevent.udata, which
 * preserves it through the round-trip without extra bookkeeping.
 */

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "errno.h"
#include "param.h"
#include "printf.h"
#include "string.h"
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "vfs/file.h"
#include "vfs/vfs_types.h"
#include "vfs/fcntl.h"
#include "kqueue.h"
#include "kqueue_types.h"
#include <mm/vm.h>
#include "signal.h"

/* From irq/syscall.c — argument fetching */
extern void argint(int n, int *ip);
extern void argint64(int n, int64 *ip);
extern void argaddr(int n, uint64 *ip);

/* From kqueue.c */
extern int kqueue_create(void);
extern int kqueue_register(struct kqueue *kq, struct kevent *changelist,
                           int nchanges);
extern int kqueue_wait(struct kqueue *kq, struct kevent *eventlist,
                       int nevents, int timeout_ms);

/* ========================================================================== */
/* epoll_event ABI: differs between x86_64 (__packed__) and riscv64           */
/* ========================================================================== */

/*
 * On x86_64, Linux packs struct epoll_event to 12 bytes.
 * On riscv64 (and most other arches), natural alignment gives 16 bytes.
 */
struct k_epoll_event {
    uint32 events;
#if !defined(__x86_64__)
    uint32 __pad;
#endif
    uint64 data;
}
#if defined(__x86_64__)
__attribute__((packed))
#endif
;

#if defined(__x86_64__)
_Static_assert(sizeof(struct k_epoll_event) == 12,
               "x86_64 epoll_event must be 12 bytes (packed)");
#else
_Static_assert(sizeof(struct k_epoll_event) == 16,
               "riscv64 epoll_event must be 16 bytes");
#endif

/* ========================================================================== */
/* epoll constants (matching Linux uapi)                                      */
/* ========================================================================== */

#define EPOLLIN        0x001
#define EPOLLPRI       0x002
#define EPOLLOUT       0x004
#define EPOLLERR       0x008
#define EPOLLHUP       0x010
#define EPOLLRDNORM    0x040
#define EPOLLRDBAND    0x080
#define EPOLLWRNORM    0x100
#define EPOLLWRBAND    0x200
#define EPOLLMSG       0x400
#define EPOLLRDHUP     0x2000
#define EPOLLONESHOT   (1U << 30)
#define EPOLLET        (1U << 31)

#define EPOLL_CTL_ADD  1
#define EPOLL_CTL_DEL  2
#define EPOLL_CTL_MOD  3

#define EPOLL_CLOEXEC  O_CLOEXEC

/* Max events per epoll_pwait / epoll_ctl */
#define EPOLL_MAX_EVENTS 256

/* ========================================================================== */
/* Helper: resolve epfd → kqueue *                                            */
/* ========================================================================== */

static struct kqueue *epoll_get_kq(int epfd, struct vfs_file **fp_out)
{
    struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, epfd);
    if (f == NULL)
        return NULL;

    /* Validate: the fd must be a kqueue fd (private_data points to kqueue).
     * We rely on the file_ops matching the kqueue ops. */
    struct kqueue *kq = (struct kqueue *)f->private_data;
    if (kq == NULL) {
        vfs_fput(f);
        return NULL;
    }

    if (fp_out)
        *fp_out = f;
    else
        vfs_fput(f);

    return kq;
}

/* ========================================================================== */
/* sys_epoll_create1                                                          */
/* ========================================================================== */

/*
 * sys_epoll_create1(flags) → epoll fd
 *
 * flags: 0 or EPOLL_CLOEXEC.
 * Internally creates a kqueue fd.
 */
uint64 sys_epoll_create1(void)
{
    int flags;
    argint(0, &flags);

    int fd = kqueue_create();
    if (fd < 0)
        return (uint64)fd;

    if (flags & EPOLL_CLOEXEC) {
        spin_lock(&current->fdtable->lock);
        vfs_fdtable_set_fdflags(current->fdtable, fd, FD_CLOEXEC);
        spin_unlock(&current->fdtable->lock);
    }

    return (uint64)fd;
}

/* ========================================================================== */
/* sys_epoll_ctl                                                              */
/* ========================================================================== */

/*
 * sys_epoll_ctl(epfd, op, fd, event_ptr) → 0 / -errno
 *
 * Translates the epoll operation into one or two kevent register calls
 * on the underlying kqueue.
 */
uint64 sys_epoll_ctl(void)
{
    int epfd, op, fd;
    uint64 uevent;
    argint(0, &epfd);
    argint(1, &op);
    argint(2, &fd);
    argaddr(3, &uevent);

    struct vfs_file *fp = NULL;
    struct kqueue *kq = epoll_get_kq(epfd, &fp);
    if (kq == NULL)
        return (uint64)-EBADF;

    /* Copy the epoll_event from user space (NULL for EPOLL_CTL_DEL) */
    struct k_epoll_event ev;
    memset(&ev, 0, sizeof(ev));

    if (op != EPOLL_CTL_DEL) {
        if (uevent == 0) {
            vfs_fput(fp);
            return (uint64)-EFAULT;
        }
        if (vm_copyin(current->vm, &ev, uevent, sizeof(ev)) < 0) {
            vfs_fput(fp);
            return (uint64)-EFAULT;
        }
    }

    /* Build kevent change list — up to 2 entries (read + write) */
    struct kevent changes[2];
    int nchanges = 0;

    uint16 kev_flags = 0;
    if (ev.events & EPOLLET)
        kev_flags |= EV_CLEAR;
    if (ev.events & EPOLLONESHOT)
        kev_flags |= EV_ONESHOT;

    switch (op) {
    case EPOLL_CTL_ADD:
    case EPOLL_CTL_MOD: {
        uint16 add_flags = EV_ADD | EV_ENABLE | kev_flags;

        if (ev.events & (EPOLLIN | EPOLLRDNORM | EPOLLRDBAND | EPOLLPRI |
                         EPOLLRDHUP)) {
            changes[nchanges].ident = (uint64)fd;
            changes[nchanges].filter = EVFILT_READ;
            changes[nchanges].flags = add_flags;
            changes[nchanges].fflags = 0;
            changes[nchanges].data = 0;
            changes[nchanges].udata = ev.data;
            nchanges++;
        } else if (op == EPOLL_CTL_MOD) {
            /* MOD: if EPOLLIN was previously set but now removed, delete it */
            changes[nchanges].ident = (uint64)fd;
            changes[nchanges].filter = EVFILT_READ;
            changes[nchanges].flags = EV_DELETE;
            changes[nchanges].fflags = 0;
            changes[nchanges].data = 0;
            changes[nchanges].udata = 0;
            nchanges++;
        }

        if (ev.events & (EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND)) {
            changes[nchanges].ident = (uint64)fd;
            changes[nchanges].filter = EVFILT_WRITE;
            changes[nchanges].flags = add_flags;
            changes[nchanges].fflags = 0;
            changes[nchanges].data = 0;
            changes[nchanges].udata = ev.data;
            nchanges++;
        } else if (op == EPOLL_CTL_MOD) {
            /* MOD: if EPOLLOUT was previously set but now removed, delete it */
            changes[nchanges].ident = (uint64)fd;
            changes[nchanges].filter = EVFILT_WRITE;
            changes[nchanges].flags = EV_DELETE;
            changes[nchanges].fflags = 0;
            changes[nchanges].data = 0;
            changes[nchanges].udata = 0;
            nchanges++;
        }

        if (nchanges == 0) {
            /* No events to register — at least register read */
            changes[0].ident = (uint64)fd;
            changes[0].filter = EVFILT_READ;
            changes[0].flags = EV_ADD | EV_ENABLE | kev_flags;
            changes[0].fflags = 0;
            changes[0].data = 0;
            changes[0].udata = ev.data;
            nchanges = 1;
        }
        break;
    }
    case EPOLL_CTL_DEL:
        /* Delete both read and write knotes for this fd.
         * ENOENT on either is silently ignored. */
        changes[0].ident = (uint64)fd;
        changes[0].filter = EVFILT_READ;
        changes[0].flags = EV_DELETE;
        changes[0].fflags = 0;
        changes[0].data = 0;
        changes[0].udata = 0;
        nchanges = 1;

        changes[1].ident = (uint64)fd;
        changes[1].filter = EVFILT_WRITE;
        changes[1].flags = EV_DELETE;
        changes[1].fflags = 0;
        changes[1].data = 0;
        changes[1].udata = 0;
        nchanges = 2;
        break;
    default:
        vfs_fput(fp);
        return (uint64)-EINVAL;
    }

    int ret = kqueue_register(kq, changes, nchanges);

    /* Check for errors in the change results */
    if (ret == 0) {
        for (int i = 0; i < nchanges; i++) {
            if (changes[i].flags & EV_ERROR) {
                int err = (int)(-changes[i].data);
                /* Ignore ENOENT on delete (knote wasn't registered) */
                if (err == ENOENT && (op == EPOLL_CTL_DEL ||
                    (op == EPOLL_CTL_MOD &&
                     !(changes[i].flags & EV_ADD))))
                    continue;
                /* On CTL_ADD: if error is EEXIST-like, ignore for MOD */
                if (err != 0) {
                    vfs_fput(fp);
                    return (uint64)-err;
                }
            }
        }
    }

    vfs_fput(fp);
    return ret < 0 ? (uint64)ret : 0;
}

/* ========================================================================== */
/* sys_epoll_pwait                                                            */
/* ========================================================================== */

/*
 * sys_epoll_pwait(epfd, events, maxevents, timeout_ms, sigmask_ptr) → n
 *
 * Wait for events on the epoll fd.  Internally calls kqueue_wait and
 * converts kevent results to epoll_event.
 *
 * timeout: -1 = block, 0 = poll, >0 = milliseconds.
 */
uint64 sys_epoll_pwait(void)
{
    int epfd, maxevents, timeout;
    uint64 uevents, usigmask;

    argint(0, &epfd);
    argaddr(1, &uevents);
    argint(2, &maxevents);
    argint(3, &timeout);
    argaddr(4, &usigmask);

    if (maxevents <= 0 || maxevents > EPOLL_MAX_EVENTS)
        return (uint64)-EINVAL;

    struct vfs_file *fp = NULL;
    struct kqueue *kq = epoll_get_kq(epfd, &fp);
    if (kq == NULL)
        return (uint64)-EBADF;

    /*
     * Signal mask: temporarily replace the signal mask if provided.
     * TODO: Full sigprocmask save/restore.  For now, ignore sigmask
     * (matches common usage where sigmask is NULL).
     */
    (void)usigmask;

    /* Allocate kernel buffer for kevent results.
     * Request up to 2x maxevents since each epoll fd can produce
     * two kevents (read + write), but we cap at maxevents output. */
    int kev_max = maxevents * 2;
    if (kev_max > EPOLL_MAX_EVENTS)
        kev_max = EPOLL_MAX_EVENTS;

    size_t kev_bytes = (size_t)kev_max * sizeof(struct kevent);
    struct kevent *kevents = kvmalloc(kev_bytes);
    if (kevents == NULL) {
        vfs_fput(fp);
        return (uint64)-ENOMEM;
    }

    /*
     * WebKit/GLib commonly uses epoll as an edge-style async wake source.
     * If a driver or socket callback drops one wakeup, an infinite
     * kqueue_wait() can park the network process even though readiness is
     * visible to a fresh poll.  Keep Linux-visible semantics, but implement
     * blocking waits as short internal timed waits so each slice rescans all
     * registered fds before sleeping again.
     */
    const int rescan_slice_ms = 20;
    int remaining_ms = timeout;
    int nkev = 0;
    for (;;) {
        int wait_ms = timeout;
        if (timeout < 0) {
            wait_ms = rescan_slice_ms;
        } else if (timeout > rescan_slice_ms) {
            wait_ms = rescan_slice_ms;
        }

        nkev = kqueue_wait(kq, kevents, kev_max, wait_ms);
        if (nkev != 0 || timeout == 0)
            break;
        if (timeout < 0)
            continue;

        remaining_ms -= wait_ms;
        if (remaining_ms <= 0)
            break;
        timeout = remaining_ms;
    }

    if (nkev < 0) {
        kvfree(kevents);
        vfs_fput(fp);
        return (uint64)nkev;
    }

    /*
     * Convert kevent results → epoll_event.  Linux epoll reports one event
     * record per watched fd, with readable/writable/error bits ORed together.
     * GLib's main loop relies on that coalescing when a socket is monitored
     * for both directions; returning separate records can make dispatch churn
     * on the writable side and starve the read progress that drives WebKit's
     * network process.
     */
    struct k_epoll_event *out_events =
        kvmalloc((size_t)maxevents * sizeof(struct k_epoll_event));
    if (out_events == NULL) {
        kvfree(kevents);
        vfs_fput(fp);
        return (uint64)-ENOMEM;
    }
    uint64 *out_ident = kvmalloc((size_t)maxevents * sizeof(uint64));
    if (out_ident == NULL) {
        kvfree(out_events);
        kvfree(kevents);
        vfs_fput(fp);
        return (uint64)-ENOMEM;
    }

    int nout = 0;
    for (int i = 0; i < nkev; i++) {
        uint32 mapped = 0;

        /* Map kqueue filter → epoll events */
        switch (kevents[i].filter) {
        case EVFILT_READ:
            mapped = EPOLLIN | EPOLLRDNORM;
            break;
        case EVFILT_WRITE:
            mapped = EPOLLOUT | EPOLLWRNORM;
            break;
        default:
            continue; /* skip filters we don't map */
        }

        /* Map kqueue flags → epoll events */
        if (kevents[i].flags & EV_EOF)
            mapped |= EPOLLHUP;
        if (kevents[i].flags & EV_ERROR)
            mapped |= EPOLLERR;

        int slot = -1;
        for (int j = 0; j < nout; j++) {
            if (out_ident[j] == kevents[i].ident &&
                out_events[j].data == kevents[i].udata) {
                slot = j;
                break;
            }
        }

        if (slot < 0) {
            if (nout >= maxevents)
                break;
            slot = nout++;
            memset(&out_events[slot], 0, sizeof(out_events[slot]));
            out_ident[slot] = kevents[i].ident;
            out_events[slot].data = kevents[i].udata;
        }
        out_events[slot].events |= mapped;
    }

    for (int i = 0; i < nout; i++) {
        uint64 dest = uevents + (uint64)i * sizeof(struct k_epoll_event);
        if (vm_copyout(current->vm, dest, &out_events[i],
                       sizeof(out_events[i])) < 0) {
            kvfree(out_ident);
            kvfree(out_events);
            kvfree(kevents);
            vfs_fput(fp);
            return (uint64)-EFAULT;
        }
    }

    kvfree(out_ident);
    kvfree(out_events);
    kvfree(kevents);
    vfs_fput(fp);
    return (uint64)nout;
}
