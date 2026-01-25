#include <param.h>
#include <types.h>
#include <riscv.h>
#include <dev/dev.h>
#include <dev/blkdev.h>
#include <defs.h>
#include "printf.h"
#include <lock/spinlock.h>
#include <lock/mutex_types.h>
#include <mm/slab.h>
#include <mm/page.h>
#include <errno.h>

static int __underlying_dev_open(device_t *dev) {
    if (dev == NULL) {
        return -EINVAL;
    }
    blkdev_t *blkdev = (blkdev_t *)dev;
    return blkdev->ops.open(blkdev);
}

static int __underlying_dev_release(device_t *dev) {
    if (dev == NULL) {
        return -EINVAL;
    }
    blkdev_t *blkdev = (blkdev_t *)dev;
    return blkdev->ops.release(blkdev);
}

static device_ops_t __blkdev_underlying_ops = {
    .open = __underlying_dev_open,
    .release = __underlying_dev_release
};

static bool __blkdev_ops_validate(blkdev_ops_t *ops) {
    if (ops == NULL) {
        return false;
    }
    if (ops->open == NULL || ops->release == NULL || ops->submit_bio == NULL) {
        return false;
    }
    return true;
}

blkdev_t *blkdev_get(int major, int minor) {
    device_t *device = device_get(major, minor);
    if (IS_ERR(device)) {
        return (blkdev_t *)device;
    }
    if (device->type != DEV_TYPE_BLOCK) {
        device_put(device);
        return ERR_PTR(-ENODEV);
    }
    return (blkdev_t *)device;
}

int blkdev_dup(blkdev_t *dev) {
    if (dev == NULL) {
        return -EINVAL;
    }
    return device_dup((device_t *)dev);
}

int blkdev_put(blkdev_t *dev) {
    if (dev == NULL) {
        return -EINVAL;
    }
    return device_put((device_t *)dev);
}

int blkdev_register(blkdev_t *dev) {
    if (dev == NULL) {
        return -EINVAL;
    }
    if (!__blkdev_ops_validate(&dev->ops)) {
        return -EINVAL;
    }
    device_t *device = (device_t *)dev;
    device->type = DEV_TYPE_BLOCK;
    device->ops = __blkdev_underlying_ops;
    return device_register(device);
}

int blkdev_unregister(blkdev_t *dev) {
    if (dev == NULL) {
        return -EINVAL;
    }
    return device_unregister((device_t *)dev);
}

int blkdev_submit_bio(blkdev_t *blkdev, struct bio *bio) {
    if (blkdev == NULL || bio == NULL) {
        return -EINVAL;
    }
    if (blkdev->dev.type != DEV_TYPE_BLOCK) {
        return -ENODEV;
    }
    if (blkdev->ops.submit_bio == NULL) {
        return -ENOSYS;
    }
    if (bio->rw && !blkdev->writable) {
        return -EACCES;
    }
    if (!bio->rw && !blkdev->readable) {
        return -EACCES;
    }
    bio->block_shift = blkdev->block_shift;

    int ret = bio_validate(bio, blkdev);
    if (ret != 0) {
        return ret;
    }

    return blkdev->ops.submit_bio(blkdev, bio);
}
