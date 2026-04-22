#ifndef __KERNEL_DEV_DEV_H
#define __KERNEL_DEV_DEV_H

#include <dev/dev_types.h>

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

int dev_ioctl(device_t *dev, uint64 cmd, void *arg);

/*
 * Iterate all registered devices.
 * The callback receives each device_t* with a held kobject ref.
 * Returning non-zero from the callback stops iteration early.
 * The caller must NOT hold the device table lock.
 */
typedef int (*dev_iter_cb_t)(device_t *dev, void *ctx);
int dev_for_each_device(dev_iter_cb_t cb, void *ctx);

#endif // __KERNEL_DEV_DEV_H
