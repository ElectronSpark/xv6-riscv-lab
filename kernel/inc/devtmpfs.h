#ifndef __KERNEL_DEVTMPFS_H
#define __KERNEL_DEVTMPFS_H

#include "types.h"

/*
 * devtmpfs public API
 *
 * These functions may be called from device drivers (cdev, blkdev, tty,
 * pty, etc.) to create or remove device nodes in devtmpfs.  It is safe
 * to call devtmpfs_create_node() before devtmpfs is mounted — nodes are
 * recorded in a global registry and materialised on the first mount.
 */

/*
 * Create a device node entry.
 *
 * @name:  leaf name (no leading slash), e.g. "console", "pts/0"
 * @mode:  file type + permission bits, e.g. S_IFCHR | 0666
 * @dev:   device number from mkdev(major, minor)
 *
 * Returns 0 on success, negative errno on failure.
 */
int devtmpfs_create_node(const char *name, mode_t mode, dev_t dev);

/*
 * Remove a device node entry.
 *
 * @name:  leaf name matching a previous create call
 *
 * Returns 0 on success, -ENOENT if not found.
 */
int devtmpfs_remove_node(const char *name);

/*
 * Retroactively create devtmpfs nodes for all devices that were
 * registered before devtmpfs became available.  Called once by
 * devtmpfs_init() after the filesystem type is registered.
 */
void devtmpfs_populate_devices(void);

#endif /* __KERNEL_DEVTMPFS_H */
