#include <param.h>
#include <types.h>
#include <riscv.h>
#include <dev.h>
#include <defs.h>
#include <spinlock.h>
#include <mutex_types.h>
#include <slab.h>
#include <page.h>
#include <errno.h>

static mutex_t __dev_tab_sleeplock;
static slab_cache_t __dev_type_cache;
static device_major_t *__dev_table[MAX_MAJOR_DEVICES] = { NULL };

static void __dev_tab_lock_init(void) {
    mutex_init(&__dev_tab_sleeplock, "dev_tab_lock");
}

static void __dev_tab_lock(void) {
    mutex_lock(&__dev_tab_sleeplock);
}

static void __dev_tab_unlock(void) {
    mutex_unlock(&__dev_tab_sleeplock);
}

static void __dev_tab_assert_held(void) {
    holding_mutex(&__dev_tab_sleeplock);
}

static void __dev_tab_slab_init(void) {
    int ret = slab_cache_init(&__dev_type_cache, 
                              "dev_type_cache", 
                              sizeof(device_major_t), 
                              SLAB_FLAG_EMBEDDED);
    assert(ret == 0, "Failed to initialize device type slab cache");
}

static void dev_type_free(device_major_t *dev_type) {
    if (dev_type) {
        if (dev_type->minors) {
            page_free((void *)dev_type->minors, 0);
            dev_type->minors = NULL;
        }
        slab_free(dev_type);
    }
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
// Unlocked
static int __dev_slot_get(int major, int minor, device_major_t ***ret_major, int *ret_minor, device_t ***ret_dev, bool alloc) {
    __dev_tab_assert_held();
    if (major <= 0 || major >= MAX_MAJOR_DEVICES || minor < 0 || minor >= MAX_MINOR_DEVICES) {
        return -EINVAL;
    }
    
    device_major_t *dmajor = __dev_table[major];
    if (dmajor == NULL) {
        if (!alloc) {
            return -ENODEV; // Device type not found
        }
        dmajor = dev_type_alloc();
        if (dmajor == NULL) {
            return -ENOMEM;
        }
        __dev_table[major] = dmajor;
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

// Unregister a device
// 
static void __device_unregister(device_t *dev) {
    __dev_tab_lock();
    device_t **dev_slot = NULL;
    device_major_t **dmajor_slot = NULL;
    int ret = __dev_slot_get(dev->major, dev->minor, &dmajor_slot, NULL, &dev_slot, true);
    assert(ret == 0, "Device slot must exist during unregister");
    assert(*dev_slot == dev, "Device mismatch during unregister");
    *dev_slot = NULL;
    device_major_t *dmajor = *dmajor_slot;
    bool needs_free = false;
    dmajor->num_minors--;
    if (dmajor->num_minors == 0) {
        // No more minors, free the device type
        needs_free = true;
        *dmajor_slot = NULL;
    }
    __dev_tab_unlock();

    if (needs_free) {
        dev_type_free(dmajor);
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
int device_get(int major, int minor, device_t **dev) {
    if (dev == NULL) {
        return -EINVAL; // Null pointer for device output
    }
    __dev_tab_lock();
    device_t **dev_slot = NULL;
    int ret = __dev_slot_get(major, minor, NULL, NULL, &dev_slot, false);
    if (ret != 0) {
        __dev_tab_unlock();
        return ret;
    }
    device_t *device = *dev_slot;
    if (device == NULL) {
        __dev_tab_unlock();
        return -ENODEV; // Device not found or not valid
    }
    kobject_get(&device->kobj);
    *dev = device;
    __dev_tab_unlock();
    return 0;
}

// Increment the reference count of a device
int device_dup(device_t *dev) {
    if (dev == NULL) {
        return -EINVAL; // Null pointer for device
    }
    __dev_tab_lock();
    kobject_get(&dev->kobj);
    __dev_tab_unlock();
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
    *dev_slot = dev;
    (*dmajor_slot)->num_minors++;
    dev->kobj.name = "device";
    dev->kobj.refcount = 0;
    dev->kobj.ops.release = __underlying_kobject_release;
    kobject_init(&dev->kobj);
    __dev_tab_unlock();
    return __dev_call_open(dev);
}
