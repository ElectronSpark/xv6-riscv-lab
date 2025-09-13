#ifndef __KERNEL_DEV_DEV_H
#define __KERNEL_DEV_DEV_H

#include <dev_types.h>

void dev_table_init(void);
int device_get(int major, int minor, device_t **dev);
int device_dup(device_t *dev);
int device_put(device_t *dev);
int device_register(device_t *dev);
int device_unregister(device_t *dev);


#endif // __KERNEL_DEV_DEV_H
