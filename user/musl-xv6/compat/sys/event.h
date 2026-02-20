/**
 * @file sys/event.h
 * @brief BSD kqueue event notification interface for xv6/musl
 *
 * Provides the user-visible ABI: struct kevent, filter and flag constants,
 * and function declarations for kqueue(), kevent_register(), kevent_wait().
 *
 * musl does not ship sys/event.h (it is a BSD extension).  This compat
 * header is placed in user/musl-xv6/compat/ so that ports expecting it
 * (e.g. CPython) can find it.
 */

#ifndef _SYS_EVENT_H
#define _SYS_EVENT_H

#include <sys/types.h>
#include <stdint.h>
#include <time.h>

/*
 * Event filters (negative values, BSD convention)
 */
#define EVFILT_READ     (-1)
#define EVFILT_WRITE    (-2)
#define EVFILT_TIMER    (-3)
#define EVFILT_SIGNAL   (-4)
#define EVFILT_PROC     (-5)
#define EVFILT_VNODE    (-6)

/*
 * Event flags (bitfield in kevent.flags)
 */
#define EV_ADD          0x0001
#define EV_DELETE       0x0002
#define EV_ENABLE       0x0004
#define EV_DISABLE      0x0008
#define EV_ONESHOT      0x0010
#define EV_CLEAR        0x0020
#define EV_EOF          0x8000
#define EV_ERROR        0x4000

/*
 * EVFILT_PROC filter-specific flags (fflags)
 */
#define NOTE_EXIT       0x80000000
#define NOTE_FORK       0x40000000
#define NOTE_EXEC       0x20000000
#define NOTE_TRACK      0x00000001
#define NOTE_CHILD      0x00000004
#define NOTE_TRACKERR   0x00000002
#define NOTE_PCTRLMASK  0xf0000000
#define NOTE_PDATAMASK  0x000fffff

/*
 * EVFILT_VNODE filter-specific flags (fflags)
 */
#define NOTE_DELETE     0x00000001
#define NOTE_WRITE      0x00000002
#define NOTE_EXTEND     0x00000004
#define NOTE_ATTRIB     0x00000008
#define NOTE_LINK       0x00000010
#define NOTE_RENAME     0x00000020
#define NOTE_REVOKE     0x00000040

/*
 * struct kevent — user-space event structure (the ABI)
 *
 * BSD-compatible layout.  Must match the kernel's struct kevent exactly.
 */
struct kevent {
    uint64_t ident;
    int16_t  filter;
    uint16_t flags;
    uint32_t fflags;
    int64_t  data;
    uint64_t udata;
};

/*
 * Convenience macro à la BSD: initialise a struct kevent in-place.
 */
#define EV_SET(kevp, a, b, c, d, e, f) do { \
    struct kevent *__kevp = (kevp);          \
    __kevp->ident  = (a);                   \
    __kevp->filter = (b);                   \
    __kevp->flags  = (c);                   \
    __kevp->fflags = (d);                   \
    __kevp->data   = (e);                   \
    __kevp->udata  = (f);                   \
} while (0)

#ifdef __cplusplus
extern "C" {
#endif

int kqueue(void);
int kevent_register(int kqfd, struct kevent *changelist, int nchanges);
int kevent_wait(int kqfd, struct kevent *eventlist, int nevents,
                int timeout_ms);
int kevent(int kq, const struct kevent *changelist, int nchanges,
           struct kevent *eventlist, int nevents,
           const struct timespec *timeout);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_EVENT_H */
