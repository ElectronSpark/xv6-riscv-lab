#ifndef KERNEL_VIRTUAL_FILE_SYSTEM_DEVTMPFS_PRIVATE_H
#define KERNEL_VIRTUAL_FILE_SYSTEM_DEVTMPFS_PRIVATE_H

/*
 * devtmpfs - Device pseudo-filesystem
 *
 * devtmpfs is an in-memory filesystem (based on tmpfs internals) that
 * automatically provides device nodes under /dev.  Kernel subsystems
 * call devtmpfs_create_node() / devtmpfs_remove_node() when devices
 * are registered or unregistered, and the corresponding entries appear
 * or disappear in every mounted devtmpfs instance.
 *
 * Because the on-disk / in-memory representation is identical to tmpfs
 * (hash-list directories, slab-allocated inodes, no backing store),
 * devtmpfs shares the tmpfs inode/dentry helpers and data structures
 * defined in tmpfs_private.h.
 */

#include "../tmpfs/tmpfs_private.h"

/*
 * A devtmpfs_node records one device entry that should exist in every
 * mounted devtmpfs instance.  They are kept in a global linked list
 * protected by the devtmpfs spinlock.
 */
struct devtmpfs_node {
    list_node_t list_entry; /* entry in global devtmpfs_nodes list */
    char *name;             /* path relative to devtmpfs root, e.g. "console" */
    size_t name_len;
    mode_t mode;            /* e.g. S_IFCHR | 0666 */
    dev_t dev;              /* device number (mkdev(major, minor)) */
};

/* ---------- superblock.c ---------- */
extern struct vfs_superblock_ops devtmpfs_superblock_ops;
extern struct vfs_fs_type_ops devtmpfs_fs_type_ops;

/* Populate device nodes into a fully-mounted devtmpfs at /dev.
 * Must be called AFTER vfs_mount_path("devtmpfs", "/dev", ...) succeeds. */
int devtmpfs_post_mount_populate(void);

/*
 * Create a device node in all mounted devtmpfs instances.
 * Called by device registration code.
 *
 * @name:  entry name (e.g. "console", "null", "random")
 * @mode:  file mode bits including type (S_IFCHR | perms)
 * @dev:   device number from mkdev()
 *
 * Returns 0 on success, negative errno on failure.
 */
int devtmpfs_create_node(const char *name, mode_t mode, dev_t dev);

/*
 * Remove a device node from all mounted devtmpfs instances.
 *
 * @name:  entry name
 *
 * Returns 0 on success, negative errno on failure.
 */
int devtmpfs_remove_node(const char *name);

/* Initialise devtmpfs caches and register the filesystem type */
void devtmpfs_init(void);

#endif /* KERNEL_VIRTUAL_FILE_SYSTEM_DEVTMPFS_PRIVATE_H */
