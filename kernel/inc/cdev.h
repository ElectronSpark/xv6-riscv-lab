#ifndef __KERNEL_CHARACTER_DEVICE_H
#define __KERNEL_CHARACTER_DEVICE_H

#include <dev_types.h>

int cdev_get(int major, int minor, cdev_t **dev);
int cdev_dup(cdev_t *dev);
int cdev_put(cdev_t *dev);
int cdev_register(cdev_t *dev);
int cdev_unregister(cdev_t *dev);

int cdev_read(cdev_t *cdev, bool user, void *buf, size_t count);
int cdev_write(cdev_t *cdev, bool user, const void *buf, size_t count);

#endif // __KERNEL_CHARACTER_DEVICE_H
