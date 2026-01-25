#ifndef __KERNEL_BLOCK_DRIVER_H
#define __KERNEL_BLOCK_DRIVER_H

#include <dev/dev_types.h>
#include <dev/bio.h>

#define blkdev_blk_size(dev) ((size_t)BLK_SIZE << (dev)->block_shift)

// Return blkdev on success, or ERR_PTR on error
blkdev_t *blkdev_get(int major, int minor);
int blkdev_dup(blkdev_t *dev);
int blkdev_put(blkdev_t *dev);
int blkdev_register(blkdev_t *dev);
int blkdev_unregister(blkdev_t *dev);
int blkdev_submit_bio(blkdev_t *blkdev, struct bio *bio);

#endif  // __KERNEL_BLOCK_DRIVER_H
