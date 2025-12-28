#include <param.h>
#include <types.h>
#include <riscv.h>
#include <dev.h>
#include <cdev.h>
#include <defs.h>
#include "printf.h"
#include <spinlock.h>
#include <mutex_types.h>
#include <slab.h>
#include <page.h>
#include <errno.h>

static int __underlying_dev_open(device_t *dev) {
    if (dev == NULL) {
        return -EINVAL; // Invalid device
    }
    cdev_t *cdev = (cdev_t *)dev;
    return cdev->ops.open(cdev);
}

static int __underlying_dev_release(device_t *dev) {
    if (dev == NULL) {
        return -EINVAL; // Invalid device
    }
    cdev_t *cdev = (cdev_t *)dev;
    return cdev->ops.release(cdev);
}

static device_ops_t __cdev_underlying_ops = {
    .open = __underlying_dev_open,
    .release = __underlying_dev_release
};

static bool __cdev_opts_validate(cdev_ops_t *ops) {
    if (ops == NULL) {
        return false;
    }
    if (ops->open == NULL || ops->release == NULL) {
        return false;
    }
    // Open and release are optional, but if provided, should be valid
    return true;
}

cdev_t *cdev_get(int major, int minor) {
    device_t *device = device_get(major, minor);
    if (IS_ERR(device)) {
        return (cdev_t *)device; // Propagate error from device_get
    }
    if (device->type != DEV_TYPE_CHAR) {
        device_put(device); // Release the device reference
        return ERR_PTR(-ENODEV); // Not a character device
    }
    return (cdev_t *)device;
}

int cdev_dup(cdev_t *dev) {
    if (dev == NULL) {
        return -EINVAL; // Null pointer for device
    }
    device_t *device = (device_t *)dev;
    int ret = device_dup(device);
    if (ret != 0) {
        return ret; // Propagate error from device_dup
    }
    return 0;
}

int cdev_put(cdev_t *dev) {
    if (dev == NULL) {
        return -EINVAL; // Null pointer for device
    }
    device_t *device = (device_t *)dev;
    device_put(device);
    return 0;
}

int cdev_register(cdev_t *dev) {
    if (dev == NULL) {
        return -EINVAL; // Null pointer for device
    }
    if (!__cdev_opts_validate(&dev->ops)) {
        return -EINVAL; // Invalid character device operations
    }
    device_t *device = (device_t *)dev;
    device->type = DEV_TYPE_CHAR;
    device->ops = __cdev_underlying_ops; // Set underlying device operations
    return device_register(device);
}

int cdev_unregister(cdev_t *dev) {
    if (dev == NULL) {
        return -EINVAL;
    }
    return device_unregister((device_t *)dev);
}

int cdev_read(cdev_t *cdev, bool user, void *buf, size_t count) {
    if (cdev == NULL || buf == NULL || count == 0) {
        return -EINVAL; // Invalid arguments
    }
    if (cdev->dev.type != DEV_TYPE_CHAR) {
        return -ENODEV; // Not a character device
    }
    if (!cdev->readable || cdev->ops.read == NULL) {
        return -ENOSYS; // Read operation not supported
    }
    return cdev->ops.read(cdev, user, buf, count);
}

int cdev_write(cdev_t *cdev, bool user, const void *buf, size_t count) {
    if (cdev == NULL || buf == NULL || count == 0) {
        return -EINVAL; // Invalid arguments
    }
    if (cdev->dev.type != DEV_TYPE_CHAR) {
        return -ENODEV; // Not a character device
    }
    if (!cdev->writable || cdev->ops.write == NULL) {
        return -ENOSYS; // Write operation not supported
    }
    return cdev->ops.write(cdev, user, buf, count);
}
