/*
 * sysfs_private.h - Minimal Linux-compatible sysfs for device discovery.
 *
 * The tree is intentionally read-only and deterministic.  It exposes the
 * kernel device metadata that libdrm/Mesa probe through /sys/dev/char.
 */

#ifndef KERNEL_VFS_SYSFS_PRIVATE_H
#define KERNEL_VFS_SYSFS_PRIVATE_H

#include "vfs/vfs_types.h"

#define SYSFS_INO_ROOT                 1ULL
#define SYSFS_INO_DEV                  2ULL
#define SYSFS_INO_DEV_CHAR             3ULL
#define SYSFS_INO_DRM_PRIMARY          4ULL
#define SYSFS_INO_DRM_PRIMARY_DEVICE   5ULL
#define SYSFS_INO_DRM_RENDER           6ULL
#define SYSFS_INO_DRM_RENDER_DEVICE    7ULL
#define SYSFS_INO_BUS                  8ULL
#define SYSFS_INO_BUS_PCI              9ULL
#define SYSFS_INO_CLASS                10ULL
#define SYSFS_INO_CLASS_DRM            11ULL
#define SYSFS_INO_CLASS_DRM_CARD0      12ULL
#define SYSFS_INO_CLASS_DRM_RENDER     13ULL
#define SYSFS_INO_DEVICES              14ULL
#define SYSFS_INO_DEVICES_PCI_ROOT     15ULL
#define SYSFS_INO_PCI_DEVICE           16ULL
#define SYSFS_INO_PCI_DRM              17ULL
#define SYSFS_INO_BUS_PCI_DEVICES      18ULL
#define SYSFS_INO_BUS_PCI_DEVICE_LINK  19ULL
#define SYSFS_INO_BUS_PCI_DRIVERS      20ULL
#define SYSFS_INO_BUS_PCI_DRIVER_VIRTIO 21ULL
#define SYSFS_INO_BUS_PCI_DRIVER_DEVICE_LINK 22ULL
#define SYSFS_INO_DRM_PRIMARY_DEVICE_DRM 23ULL
#define SYSFS_INO_DRM_RENDER_DEVICE_DRM 24ULL
#define SYSFS_INO_DEVICES_SYSTEM       25ULL
#define SYSFS_INO_DEVICES_SYSTEM_CPU   26ULL
#define SYSFS_INO_CPU_ONLINE           27ULL
#define SYSFS_INO_CPU_PRESENT          28ULL
#define SYSFS_INO_CPU_POSSIBLE         29ULL
#define SYSFS_INO_CPU_KERNEL_MAX       30ULL

#define SYSFS_INO_DRM_ATTR_BASE        100ULL
#define SYSFS_INO_PRIMARY_ATTR_BASE    SYSFS_INO_DRM_ATTR_BASE
#define SYSFS_INO_RENDER_ATTR_BASE     (SYSFS_INO_DRM_ATTR_BASE + 32ULL)
#define SYSFS_INO_CLASS_ATTR_BASE      200ULL
#define SYSFS_INO_CLASS_CARD0_ATTR_BASE SYSFS_INO_CLASS_ATTR_BASE
#define SYSFS_INO_CLASS_RENDER_ATTR_BASE (SYSFS_INO_CLASS_ATTR_BASE + 32ULL)

enum sysfs_entry_type {
    SYSFS_ROOT = 0,
    SYSFS_DIR,
    SYSFS_FILE,
    SYSFS_SYMLINK,
};

enum sysfs_device_kind {
    SYSFS_DEV_NONE = 0,
    SYSFS_DEV_DRM_PRIMARY,
    SYSFS_DEV_DRM_RENDER,
};

enum sysfs_attr_type {
    SYSFS_ATTR_NONE = 0,
    SYSFS_ATTR_VENDOR,
    SYSFS_ATTR_DEVICE,
    SYSFS_ATTR_SUBSYSTEM_VENDOR,
    SYSFS_ATTR_SUBSYSTEM_DEVICE,
    SYSFS_ATTR_REVISION,
    SYSFS_ATTR_UEVENT,
    SYSFS_ATTR_SUBSYSTEM,
    SYSFS_ATTR_DEV,
    SYSFS_ATTR_DEVICE_LINK,
    SYSFS_ATTR_MODALIAS,
    SYSFS_ATTR_CLASS,
    SYSFS_ATTR_DRIVER_LINK,
    SYSFS_ATTR_CPU_ONLINE,
    SYSFS_ATTR_CPU_PRESENT,
    SYSFS_ATTR_CPU_POSSIBLE,
    SYSFS_ATTR_CPU_KERNEL_MAX,
};

struct sysfs_inode {
    struct vfs_inode vfs_inode;
    enum sysfs_entry_type type;
    enum sysfs_device_kind dev_kind;
    enum sysfs_attr_type attr;
};

static inline struct sysfs_inode *sysfs_i(struct vfs_inode *inode)
{
    return container_of(inode, struct sysfs_inode, vfs_inode);
}

extern struct vfs_superblock_ops sysfs_sb_ops;
extern struct vfs_fs_type_ops sysfs_fs_type_ops;
extern struct vfs_inode_ops sysfs_inode_ops;
extern struct vfs_file_ops sysfs_reg_file_ops;
extern struct vfs_file_ops sysfs_dir_file_ops;

struct vfs_inode *sysfs_alloc_inode(struct vfs_superblock *sb);
void sysfs_free_inode(struct vfs_inode *inode);
void sysfs_init(void);

#endif /* KERNEL_VFS_SYSFS_PRIVATE_H */
