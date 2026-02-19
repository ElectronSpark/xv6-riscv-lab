/*
 * kqueue.h - BSD-inspired kqueue event notification (public API)
 *
 * Defines the user-visible ABI: struct kevent, filter and flag constants.
 */
#ifndef __KERNEL_KQUEUE_H
#define __KERNEL_KQUEUE_H

#include "types.h"

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
#define EV_ADD          0x0001  /* add event to kqueue */
#define EV_DELETE       0x0002  /* delete event from kqueue */
#define EV_ENABLE       0x0004  /* enable event */
#define EV_DISABLE      0x0008  /* disable event */
#define EV_ONESHOT      0x0010  /* only report one occurrence */
#define EV_CLEAR        0x0020  /* clear event state after retrieval */
#define EV_EOF          0x8000  /* EOF detected */
#define EV_ERROR        0x4000  /* error, data contains errno */

/*
 * EVFILT_PROC filter-specific flags (fflags)
 */
#define NOTE_EXIT       0x80000000
#define NOTE_FORK       0x40000000
#define NOTE_EXEC       0x20000000
#define NOTE_TRACK      0x00000001

/*
 * EVFILT_VNODE filter-specific flags (fflags)
 */
#define NOTE_DELETE     0x00000001
#define NOTE_WRITE      0x00000002
#define NOTE_EXTEND     0x00000004
#define NOTE_ATTRIB     0x00000008
#define NOTE_LINK       0x00000010
#define NOTE_RENAME     0x00000020

/*
 * struct kevent - user-space event structure (the ABI)
 *
 * BSD-compatible layout.
 */
struct kevent {
    uint64 ident;           /* identifier for this event (fd, signal, pid) */
    int16 filter;           /* filter for event (EVFILT_*) */
    uint16 flags;           /* action flags (EV_*) */
    uint32 fflags;          /* filter-specific flags (NOTE_*) */
    int64 data;             /* filter-specific data */
    uint64 udata;           /* opaque user data */
};

#endif /* __KERNEL_KQUEUE_H */
