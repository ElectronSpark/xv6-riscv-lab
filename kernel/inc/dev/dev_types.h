#ifndef __KERNEL_DEV_DEV_TYPES_H
#define __KERNEL_DEV_DEV_TYPES_H

#include <compiler.h>
#include <param.h>
#include <types.h>
#include <kobject.h>
#include <lock/rcu_type.h>
#include <vfs/stat.h>
#include <dev/iosched_types.h>

#define MAX_MAJOR_DEVICES 256 // Maximum number of major devices
// Maximum number of minor devices per major device
#define MAX_MINOR_DEVICES 256

typedef struct device_major device_major_t;
typedef struct device_instance device_t;
typedef struct device_ops device_ops_t;
typedef struct cdev cdev_t;
typedef struct blkdev_ops blkdev_ops_t;
typedef struct blkdev blkdev_t;

struct bio;

typedef struct device_major {
    int num_minors;      // Number of minor devices
    device_t **minors;   // Array of pointers to minor device instances
    rcu_head_t rcu_head; // RCU callback head for deferred freeing
} device_major_t;

typedef struct device_ops {
    int (*open)(device_t *dev);
    int (*release)(device_t *dev);
    int (*ioctl)(device_t *dev, uint64 cmd, void *arg);
} device_ops_t;

typedef enum { DEV_TYPE_UNKNOWN = 0, DEV_TYPE_BLOCK, DEV_TYPE_CHAR } dev_type_e;

typedef struct device_instance {
    struct kobject kobj;
    int major;         // Major device number
    int minor;         // Minor device number
    dev_type_e type;   // Device type (block, char, etc.)
    int unregistering; // Set to 1 when device is being unregistered
    device_ops_t ops;

    /*
     * devtmpfs integration — optional.
     * When devname is non-NULL, device_register() automatically calls
     * devtmpfs_create_node() and device_unregister() removes it.
     * Safe to set before registration even if devtmpfs is not yet mounted;
     * nodes are buffered and materialised on first mount.
     */
    const char *devname; // devtmpfs path relative to /dev (e.g. "console")
    mode_t      devmode; // file type + perms (e.g. S_IFCHR | 0666)
} device_t;

struct vfs_file; /* forward declaration for open_file callback */

typedef struct cdev_ops {
    int (*read)(cdev_t *cdev, bool user, void *buf, size_t count);
    int (*write)(cdev_t *cdev, bool user, const void *buf, size_t count);
    int (*open)(cdev_t *cdev);
    int (*release)(cdev_t *cdev);
    int (*ioctl)(cdev_t *cdev, uint64 cmd, void *arg);
    int (*poll)(cdev_t *cdev, short events); // returns revents bitmask

    /*
     * open_file - optional file-level open callback (like Linux's fops->open).
     *
     * When non-NULL, __vfs_open_cdev calls this INSTEAD of the plain open().
     * The callback receives the vfs_file being opened so it can install
     * custom file->ops and file->private_data.  This is used by /dev/ptmx
     * to transform the opened file into a PTY master.
     *
     * Return 0 on success, negative errno on failure.
     */
    int (*open_file)(cdev_t *cdev, struct vfs_file *file);
} cdev_ops_t;

typedef struct cdev {
    device_t dev;
    struct {
        uint64 readable : 1; // Is the device readable
        uint64 writable : 1; // Is the device writable
    };
    cdev_ops_t ops; // File operations for the character device
} cdev_t;

typedef struct blkdev_ops {
    int (*open)(blkdev_t *blkdev);
    int (*release)(blkdev_t *blkdev);
    int (*submit_bio)(blkdev_t *blkdev, struct bio *bio);
    int (*flush)(blkdev_t *blkdev); // flush volatile write cache to stable storage
} blkdev_ops_t;

typedef struct blkdev {
    device_t dev;
    struct {
        uint64 readable : 1; // Is the device readable
        uint64 writable : 1; // Is the device writable
    };
    uint16 block_shift; // Block size shift relative to 512 bytes, typically
                        // 1(512) or 3(4096)
    blkdev_ops_t ops;
    struct iosched iosched; // per-device IO scheduler
} blkdev_t;

#endif // __KERNEL_DEV_DEV_TYPES_H
