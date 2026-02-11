#ifndef __KERNEL_TYPES_H
#define __KERNEL_TYPES_H

#include "compiler.h"

// Reference to an inode and its associated superblock
struct vfs_superblock;
struct vfs_inode;
struct vfs_inode_ref {
    struct vfs_superblock *sb;
    struct vfs_inode *inode;
};

typedef unsigned int uint;
typedef unsigned short ushort;
typedef unsigned char uchar;

typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int uint32;
typedef unsigned long uint64;

typedef signed char int8;
typedef signed short int16;
typedef signed int int32;
typedef signed long int64;

typedef uint64 pde_t;

typedef uint64 cpumask_t;

#ifndef size_t
typedef typeof(sizeof(0)) size_t;
#endif /* size_t */

#if !defined(mode_t)
typedef uint32 mode_t;
#endif /* mode_t */

#if !defined(pid_t)
typedef int pid_t;
#endif /* pid_t */

#if !defined(ON_HOST_OS)
#if !defined(ssize_t)
typedef typeof(-sizeof(int)) ssize_t;
#endif /* ssize_t */
#if !defined(loff_t)
typedef long long loff_t;
#endif /* loff_t */
#if !defined(dev_t)
typedef uint32 dev_t;
#endif /* dev_t */
#else  /* ON_HOST_OS */
#include <sys/types.h>
/* loff_t, ssize_t, dev_t are provided by sys/types.h on host */
#endif // ON_HOST_OS

#ifndef bool
#if __STDC_VERSION__ < 202311L // C23 or later
typedef enum { false = 0, true = 1 } bool;
#endif
#endif /* bool */

#ifndef NULL
#define NULL ((void *)0)
#endif /* NULL */

// get the offset of an entry in bytes from its parent type
#ifndef offsetof
#define offsetof(type, member)                                                 \
    ({                                                                         \
        const type *__pptr = NULL;                                             \
        (void *)&(__pptr->member) - NULL;                                      \
    })
#endif /* offsetof */

// given the address of an entry, get the address of its parent type
#ifndef container_of
#define container_of(ptr, type, member)                                        \
    ({                                                                         \
        const typeof(((type *)NULL)->member) *__mptr = (ptr);                  \
        (type *)((void *)__mptr - offsetof(type, member));                     \
    })
#endif /* container_of */

/**
 * Callback invoked before a thread yields the CPU during a wait.
 * Typically releases the caller's lock so that a waker can make progress.
 *
 * The return value is opaque: its meaning is defined by each
 * sleep/wakeup callback pair.  It is forwarded as the @c status
 * argument to the matching wakeup_callback_t.
 */
typedef int (*sleep_callback_t)(void *data);

/**
 * Callback invoked after a thread resumes from a wait.
 * Typically re-acquires the lock released by the matching sleep_callback_t.
 *
 * @param data    The same opaque pointer passed to sleep_callback_t.
 * @param status  The value returned by the matching sleep_callback_t.
 *                Its interpretation is defined by the callback pair.
 */
typedef void (*wakeup_callback_t)(void *data, int status);

#endif /* __KERNEL_TYPES_H */
