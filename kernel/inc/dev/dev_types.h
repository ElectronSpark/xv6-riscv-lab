#ifndef __KERNEL_DEV_DEV_TYPES_H
#define __KERNEL_DEV_DEV_TYPES_H

#include <compiler.h>
#include <param.h>
#include <types.h>
#include <kobject.h>
#include <lock/rcu_type.h>

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
    int (*ioctl)(device_t *dev, uint64 cmd, uint64 arg);
} device_ops_t;

typedef enum { DEV_TYPE_UNKNOWN = 0, DEV_TYPE_BLOCK, DEV_TYPE_CHAR } dev_type_e;

typedef struct device_instance {
    struct kobject kobj;
    int major;         // Major device number
    int minor;         // Minor device number
    dev_type_e type;   // Device type (block, char, etc.)
    int unregistering; // Set to 1 when device is being unregistered
    device_ops_t ops;
} device_t;

typedef struct cdev_ops {
    int (*read)(cdev_t *cdev, bool user, void *buf, size_t count);
    int (*write)(cdev_t *cdev, bool user, const void *buf, size_t count);
    int (*open)(cdev_t *cdev);
    int (*release)(cdev_t *cdev);
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
} blkdev_t;

#endif // __KERNEL_DEV_DEV_TYPES_H
