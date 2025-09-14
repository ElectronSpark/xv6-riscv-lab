#ifndef __KERNEL_DEV_DEV_TYPES_H
#define __KERNEL_DEV_DEV_TYPES_H

#include <compiler.h>
#include <param.h>
#include <types.h>
#include <file_ops.h>

#define MAX_MAJOR_DEVICES 256 // Maximum number of major devices
#define MAX_MINOR_DEVICES 256 // Maximum number of minor devices per major device

typedef struct device_major device_major_t;
typedef struct device_instance device_t;
typedef struct device_ops device_ops_t;
typedef struct cdev cdev_t;

typedef struct device_major {
    int num_minors;                         // Number of minor devices
    device_t **minors;                      // Array of pointers to minor device instances
} device_major_t;

typedef struct device_ops {
    int (*open)(device_t *dev);
    int (*release)(device_t *dev);
} device_ops_t;

typedef enum {
    DEV_TYPE_UNKNOWN = 0,
    DEV_TYPE_BLOCK,
    DEV_TYPE_CHAR
} dev_type_e;

typedef struct device_instance {
    int major;              // Major device number
    int minor;              // Minor device number
    struct {
        uint64 valid: 1;
    };
    dev_type_e type;       // Device type (block, char, etc.)
    int ref_count;          // Reference count for the device instance
    device_ops_t ops;
} device_t;

typedef struct cdev_ops {
    int (*read)(cdev_t *cdev, bool user, void *buf, size_t count);
    int (*write)(cdev_t *cdev, bool user, const void *buf, size_t count);
    int (*open)(cdev_t *cdev);
    int (*release)(cdev_t *cdev);
} cdev_ops_t;

typedef struct cdev {
    device_t dev;          // Pointer to the device instance
    struct {
        uint64 readable: 1; // Is the device readable
        uint64 writable: 1; // Is the device writable
    };
    cdev_ops_t ops; // File operations for the character device
} cdev_t;


#endif // __KERNEL_DEV_DEV_TYPES_H
