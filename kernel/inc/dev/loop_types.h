/*
 * Loopback block device types.
 *
 * A loop device presents a regular file as a block device.
 * I/O goes through the backing file's per-inode page cache (i_data).
 */
#ifndef __KERNEL_LOOP_TYPES_H
#define __KERNEL_LOOP_TYPES_H

#include <types.h>
#include <lock/spinlock.h>
#include <dev/dev_types.h>

#define NLOOP 8     /* maximum number of loop devices */

struct vfs_file;
struct gendisk;

struct loop_dev {
    blkdev_t           blkdev;          /* first field — container_of works */
    struct vfs_file   *backing_file;    /* NULL when slot is free */
    uint64             file_offset;     /* byte start within backing file */
    uint64             size_bytes;      /* total loop device size in bytes */
    int                loop_num;        /* 0-based index: /dev/loop0 .. */
    spinlock_t         lock;
    struct gendisk    *gd;              /* gendisk for partition discovery */
    char               devname[16];     /* e.g. "loop0" */
};

#endif /* __KERNEL_LOOP_TYPES_H */
