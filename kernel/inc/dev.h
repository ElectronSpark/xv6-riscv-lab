#ifndef __KERNEL_DEV_DEV_H
#define __KERNEL_DEV_DEV_H

#include <dev_types.h>

void dev_table_init(void);
// Get a device by its major and minor numbers
// And increment its reference count
// Return device on success, or ERR_PTR on error
device_t *device_get(int major, int minor);
int device_dup(device_t *dev);
int device_put(device_t *dev);
int device_register(device_t *dev);


#endif // __KERNEL_DEV_DEV_H
