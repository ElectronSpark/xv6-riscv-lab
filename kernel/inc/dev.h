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
// Mark a device as unregistering.
// After this call, device_get() and device_dup() will fail for this device.
// The actual cleanup (removal from table and release callback) happens
// when the refcount reaches 0.
int device_unregister(device_t *dev);

// Device table stress tests
void dev_table_test(void);


#endif // __KERNEL_DEV_DEV_H
