#ifndef __KERNEL_PIPE_TYPES_H
#define __KERNEL_PIPE_TYPES_H

#include "lock/spinlock.h"
#include "proc/tq_type.h"
#include "smp/atomic.h"

struct vfs_file;

struct pipe {
    spinlock_t reader_lock;
    uint nread; // number of bytes read
    tq_t nread_queue;
    spinlock_t writer_lock;
    uint nwrite; // number of bytes written
    tq_t nwrite_queue;
    int flags;
    char *data;
    /* kqueue: back-references to VFS file endpoints for cross-notification */
    struct vfs_file *read_file;
    struct vfs_file *write_file;
};

#endif // __KERNEL_PIPE_TYPES_H
