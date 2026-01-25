#include <param.h>
#include <types.h>
#include "string.h"
#include <riscv.h>
#include <dev/dev.h>
#include <defs.h>
#include "printf.h"
#include <lock/spinlock.h>
#include <lock/mutex_types.h>
#include <mm/slab.h>
#include <mm/page.h>
#include <errno.h>
#include <smp/atomic.h>
#include "lock/rcu.h"

// RCU-protected device table
// Readers use rcu_read_lock/unlock + rcu_dereference
// Writers use spinlock for serialization + rcu_assign_pointer + call_rcu for deferred free
static spinlock_t __dev_tab_spinlock = SPINLOCK_INITIALIZED("dev_tab_lock");
static slab_cache_t __dev_type_cache;
static device_major_t *__dev_table[MAX_MAJOR_DEVICES] = { NULL };

static void __dev_tab_lock_init(void) {
}

static void __dev_tab_lock(void) {
    spin_lock(&__dev_tab_spinlock);
}

static void __dev_tab_unlock(void) {
    spin_unlock(&__dev_tab_spinlock);
}

static void __dev_tab_assert_held(void) {
    assert(spin_holding(&__dev_tab_spinlock), "dev_tab_lock not held");
}

static void __dev_tab_slab_init(void) {
    int ret = slab_cache_init(&__dev_type_cache, 
                              "dev_type_cache", 
                              sizeof(device_major_t), 
                              SLAB_FLAG_EMBEDDED | SLAB_FLAG_DEBUG_BITMAP);
    assert(ret == 0, "Failed to initialize device type slab cache");
}

// Free a device_major_t structure and its minors array
static void dev_type_free(device_major_t *dev_type) {
    if (dev_type) {
        if (dev_type->minors) {
            page_free((void *)dev_type->minors, 0);
            dev_type->minors = NULL;
        }
        slab_free(dev_type);
    }
}

// RCU callback to free device_major_t after grace period
static void dev_type_rcu_free(void *data) {
    device_major_t *dev_type = (device_major_t *)data;
    dev_type_free(dev_type);
}

static device_major_t *dev_type_alloc(void) {
    device_major_t *dev_type = slab_alloc(&__dev_type_cache);
    if (dev_type == NULL) {
        return NULL;
    }
    void *minors = page_alloc(0, PAGE_TYPE_ANON);
    if (minors == NULL) {
        slab_free(dev_type);
        return NULL;
    }
    memset(minors, 0, sizeof(device_t*) * MAX_MINOR_DEVICES);
    memset(dev_type, 0, sizeof(*dev_type));
    dev_type->minors = minors;
    return dev_type;
}

void dev_table_init(void) {
    __dev_tab_lock_init();
    __dev_tab_slab_init();
}

static bool __dev_opts_validate(device_ops_t *ops) {
    if (ops == NULL) {
        return false;
    }
    if (ops->open == NULL || ops->release == NULL) {
        return false; // Both open and release operations must be defined
    }
    return true;
}

static bool __dev_type_validate(dev_type_e type) {
    switch (type) {
        case DEV_TYPE_BLOCK:
        case DEV_TYPE_CHAR:
            return true;
        default:
            return false;
    }
}

// Get a device slot by its major and minor numbers
// Allocate device_major_t if not exist
// For writers (alloc=true): must hold __dev_tab_spinlock
// For readers (alloc=false): must be in RCU read-side critical section
static int __dev_slot_get(int major, int minor, device_major_t ***ret_major, int *ret_minor, device_t ***ret_dev, bool alloc) {
    if (alloc) {
        __dev_tab_assert_held();
    }
    if (major <= 0 || major >= MAX_MAJOR_DEVICES || minor < 0 || minor >= MAX_MINOR_DEVICES) {
        return -EINVAL;
    }
    
    device_major_t *dmajor;
    if (alloc) {
        // Writer path - direct access since we hold the lock
        dmajor = __dev_table[major];
    } else {
        // Reader path - use RCU dereference
        dmajor = rcu_dereference(__dev_table[major]);
    }
    
    if (dmajor == NULL) {
        if (!alloc) {
            return -ENODEV; // Device type not found
        }
        dmajor = dev_type_alloc();
        if (dmajor == NULL) {
            return -ENOMEM;
        }
        rcu_assign_pointer(__dev_table[major], dmajor);
    }

    // If minor is 0, return the slot with the lowest minor number
    if (minor == 0) {
        if (!alloc) {
            return -EINVAL; // Minor number 0 is invalid for lookup
        }
        for (int i = 1; i < MAX_MINOR_DEVICES; i++) {
            if (dmajor->minors[i] == NULL) {
                minor = i;
                break;
            }
        }
        if (minor == 0) {
            return -ENOSPC; // No available minor slots
        }
    }

    if (ret_major) {
        *ret_major = &__dev_table[major];
    }
    if (ret_dev) {
        *ret_dev = &dmajor->minors[minor];
    }
    if (ret_minor) {
        *ret_minor = minor;
    }
    return 0;
}

// Helper functions to call device ops
// Because only ops of valid device will be called
// Their validity is asserted to be true
// Open and release are called with the device table lock held
static int __dev_call_open(device_t *dev) {
    assert(dev && dev->ops.open, "Invalid device or open operation");
    return dev->ops.open(dev);
}

static int __dev_call_release(device_t *dev) {
    assert(dev && dev->ops.release, "Invalid device or release operation");
    return dev->ops.release(dev);
}

// Unregister a device from the device table
// Called either by device_unregister() or when refcount reaches 0
// Uses RCU for safe removal - old device_major_t freed after grace period
static void __device_unregister(device_t *dev) {
    __dev_tab_lock();
    device_t **dev_slot = NULL;
    device_major_t **dmajor_slot = NULL;
    int ret = __dev_slot_get(dev->major, dev->minor, &dmajor_slot, NULL, &dev_slot, true);
    if (ret != 0) {
        // Device already removed from table
        __dev_tab_unlock();
        return;
    }
    if (*dev_slot != dev) {
        // Device already removed or replaced
        __dev_tab_unlock();
        return;
    }
    // Use RCU-safe pointer assignment to clear the device slot
    rcu_assign_pointer(*dev_slot, NULL);
    device_major_t *dmajor = *dmajor_slot;
    device_major_t *to_free = NULL;
    dmajor->num_minors--;
    if (dmajor->num_minors == 0) {
        // No more minors, schedule the device type for RCU-deferred free
        to_free = dmajor;
        rcu_assign_pointer(*dmajor_slot, NULL);
    }
    __dev_tab_unlock();

    if (to_free) {
        // Defer freeing until after grace period so readers can finish
        call_rcu(&to_free->rcu_head, dev_type_rcu_free, to_free);
    }
}

// Release the underlying device
static void __underlying_kobject_release(struct kobject *obj) {
    device_t *dev = container_of(obj, device_t, kobj);
    __device_unregister(dev);
    __dev_call_release(dev);
}

// Get a device by its major and minor numbers
// And increment its reference count
// Uses RCU for lock-free read access
device_t *device_get(int major, int minor) {
    rcu_read_lock();
    
    // Validate parameters first
    if (major <= 0 || major >= MAX_MAJOR_DEVICES || minor <= 0 || minor >= MAX_MINOR_DEVICES) {
        rcu_read_unlock();
        return ERR_PTR(-EINVAL);
    }
    
    // RCU-safe dereference of the device major entry
    device_major_t *dmajor = rcu_dereference(__dev_table[major]);
    if (dmajor == NULL) {
        rcu_read_unlock();
        return ERR_PTR(-ENODEV);
    }
    
    // RCU-safe dereference of the device minor entry
    device_t *device = rcu_dereference(dmajor->minors[minor]);
    if (device == NULL) {
        rcu_read_unlock();
        return ERR_PTR(-ENODEV);
    }
    
    // Check if device is being unregistered
    if (__atomic_load_n(&device->unregistering, __ATOMIC_SEQ_CST)) {
        rcu_read_unlock();
        return ERR_PTR(-ENODEV);
    }
    
    // Use kobject_try_get to avoid racing with final put
    // This must succeed before we exit the RCU read-side critical section
    if (!kobject_try_get(&device->kobj)) {
        rcu_read_unlock();
        return ERR_PTR(-ENODEV);
    }
    
    rcu_read_unlock();
    return device;
}

// Increment the reference count of a device
// Returns -ENODEV if the device is being unregistered or refcount is 0
int device_dup(device_t *dev) {
    if (dev == NULL) {
        return -EINVAL; // Null pointer for device
    }
    // Check if device is being unregistered
    if (__atomic_load_n(&dev->unregistering, __ATOMIC_SEQ_CST)) {
        return -ENODEV; // Device is being unregistered
    }
    // Use try_get to avoid racing with final put
    if (!kobject_try_get(&dev->kobj)) {
        return -ENODEV; // Device refcount already reached 0
    }
    return 0;
}

// Decrement the reference count of a device
int device_put(device_t *device) {
    if (device == NULL) {
        return -EINVAL; // Null pointer for device
    }
    kobject_put(&device->kobj);
    return 0;
}

int device_register(device_t *dev) {
    if (dev == NULL) {
        return -EINVAL; // Null pointer for device
    }
    if (!__dev_type_validate(dev->type)) {
        return -EINVAL; // Invalid device type
    }
    if (!__dev_opts_validate(&dev->ops)) {
        return -EINVAL; // Invalid device operations
    }
    __dev_tab_lock();
    device_t **dev_slot = NULL;
    device_major_t **dmajor_slot = NULL;
    int ret_minor = 0;

    int ret = __dev_slot_get(dev->major, dev->minor, &dmajor_slot, &ret_minor, &dev_slot, true);
    if (ret != 0) {
        __dev_tab_unlock();
        return ret;
    }
    if (*dev_slot != NULL) {
        __dev_tab_unlock();
        return -EBUSY; // Device already registered
    }
    
    // Initialize kobject before making device visible to readers
    dev->kobj.name = "device";
    dev->kobj.refcount = 0;
    dev->unregistering = 0;
    dev->kobj.ops.release = __underlying_kobject_release;
    kobject_init(&dev->kobj);
    (*dmajor_slot)->num_minors++;
    
    // Use RCU-safe pointer assignment to publish the device
    rcu_assign_pointer(*dev_slot, dev);
    __dev_tab_unlock();
    return __dev_call_open(dev);
}

// Mark a device as unregistering.
// After this call, device_get() and device_dup() will fail for this device.
// The device is also removed from the lookup table immediately.
// The actual release callback happens when the refcount reaches 0.
int device_unregister(device_t *dev) {
    if (dev == NULL) {
        return -EINVAL;
    }
    // Mark device as unregistering atomically
    if (!atomic_cas(&dev->unregistering, 0, 1)) {
        return -EALREADY; // Already unregistering
    }
    // Remove from device table immediately so no new lookups find it
    __device_unregister(dev);
    // Drop the initial reference from registration
    // When refcount reaches 0, __underlying_kobject_release will be called
    kobject_put(&dev->kobj);
    return 0;
}
