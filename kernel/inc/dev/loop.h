/*
 * Loopback block device — public API.
 */
#ifndef __KERNEL_LOOP_H
#define __KERNEL_LOOP_H

#include <dev/loop_types.h>

/* ioctl commands for loop device management */
#define LOOP_SET_FD     0x4C00  /* attach a file descriptor */
#define LOOP_CLR_FD     0x4C01  /* detach the backing file */
#define LOOP_GET_STATUS 0x4C02  /* query loop device status */

/* Status bits returned by LOOP_GET_STATUS */
#define LOOP_STATUS_ACTIVE 1

/*
 * Initialise the loop device subsystem.
 * Pre-registers /dev/loop0 .. /dev/loop<NLOOP-1> in devtmpfs.
 * Called once at boot from start_kernel_post_init().
 */
void loop_init(void);

/*
 * Attach a regular file to a loop device.
 *
 * @loop_num:  loop slot number (0 .. NLOOP-1)
 * @file:      an opened regular file (ref is duped internally)
 * @offset:    byte offset within @file where the device image starts
 *
 * After success the loop device is active and partition discovery
 * is attempted automatically (via gendisk_probe).
 *
 * Returns 0 on success, negative errno on failure.
 */
int loop_setup(int loop_num, struct vfs_file *file, uint64 offset);

/*
 * Detach the backing file from a loop device.
 * Removes any discovered partitions and marks the device inactive.
 *
 * Returns 0 on success, negative errno on failure.
 */
int loop_clear(int loop_num);

/*
 * Check whether a loop device slot is free (no backing file attached).
 * Returns 1 if free, 0 if in use.
 */
int loop_is_free(int loop_num);

/*
 * Get the blkdev_t pointer for a loop device.
 * Returns NULL if loop_num is invalid.
 */
blkdev_t *loop_get_blkdev(int loop_num);

#endif /* __KERNEL_LOOP_H */
