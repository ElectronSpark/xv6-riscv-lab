#ifndef __KERNEL_DEV_DEV_TYPES_H
#define __KERNEL_DEV_DEV_TYPES_H

#include <compiler.h>
#include <param.h>
#include <types.h>

#define MAX_MAJOR_DEVICES 256 // Maximum number of major devices
#define MAX_MINOR_DEVICES 256 // Maximum number of minor devices per major device

typedef struct device_type device_type_t;
typedef struct device_instance device_t;
typedef struct device_ops device_ops_t;

typedef struct device_type {
    int major;                              // Major device number
    const char *name;                       // Device type name
    int num_minors;                         // Number of minor devices
    device_ops_t ops;                       // Device operations
    device_t **minors;                      // Array of pointers to minor device instances
} device_type_t;

typedef struct device_instance {
    int major;              // Major device number
    int minor;              // Minor device number
    struct {
        uint64 valid: 1;
    };
    int ref_count;          // Reference count for the device instance
    device_type_t *type;    // Pointer to the device type
} device_t;

typedef struct device_ops {
    int (*init)(device_t *dev);
    int (*exit)(device_t *dev);
} device_ops_t;


#endif // __KERNEL_DEV_DEV_TYPES_H
