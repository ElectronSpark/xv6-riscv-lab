#include <param.h>
#include <riscv.h>
#include <types.h>
#include <dev/dev.h>
#include <defs.h>
#include <spinlock.h>
#include <sleeplock.h>
#include <slab.h>
#include <page.h>
#include <errno.h>

static struct sleeplock __dev_tab_sleeplock;
static slab_cache_t __dev_type_cache;
static device_type_t *__dev_table[MAX_MAJOR_DEVICES] = { NULL };

static void __dev_tab_lock_init(void) {
    sleeplock_init(&__dev_tab_sleeplock, "dev_tab_lock");
}

static void __dev_tab_lock(void) {
    sleeplock_acquire(&__dev_tab_sleeplock);
}

static void __dev_tab_unlock(void) {
    sleeplock_release(&__dev_tab_sleeplock);
}

static void __dev_tab_assert_held(void) {
    sleeplock_assert_held(&__dev_tab_sleeplock);
}

static void __dev_tab_slab_init(void) {
    int ret = slab_cache_init(&__dev_type_cache, 
                              "dev_type_cache", 
                              sizeof(device_type_t), 
                              SLAB_FLAG_EMBEDDED);
    assert(ret == 0, "Failed to initialize device type slab cache");
}

static void dev_type_free(device_type_t *dev_type) {
    if (dev_type) {
        if (dev_type->minors) {
            page_free((void *)dev_type->minors, 0);
        }
        slab_free(dev_type);
    }
}

static device_type_t *dev_type_alloc(void) {

    device_type_t *dev_type = slab_alloc(&__dev_type_cache);
    void *minors = page_alloc(0, PAGE_FLAG_ANON);
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

static void __dev_type_init(device_type_t *dev_type, device_ops_t *ops, const char *name) {
    dev_type->ops = *ops;
    dev_type->name = name;
    dev_type->num_minors = 0;
}

static bool __dev_type_opts_validate(device_ops_t *ops) {
    if (ops == NULL) {
        return false;
    }
    if (ops->init == NULL || ops->exit == NULL) {
        return false; // Both init and exit operations must be defined
    }
    return true;
}

int device_type_register(device_ops_t *opts, int major, const char *name) {
    if (opts == NULL || major < 0 || major >= MAX_MAJOR_DEVICES) {
        return -EINVAL; // Invalid device type
    }
    if (!__dev_type_opts_validate(opts)) {
        return -EINVAL; // Invalid device operations
    }
    __dev_tab_lock();
    if (__dev_table[major] != NULL) {
        __dev_tab_unlock();
        return -EBUSY; // Device already registered
    }
    device_type_t *dev_type = dev_type_alloc();
    if (dev_type == NULL) {
        __dev_tab_unlock();
        return -ENOMEM; // Out of memory
    }
    __dev_type_init(dev_type, opts, name);
    __dev_table[major] = dev_type;
    __dev_tab_unlock();
    return 0;
}

int device_type_unregister(int major) {
    if (major < 0 || major >= MAX_MAJOR_DEVICES) {
        // @TODO: Implement device_type_unregister
        return -ENOTSUP;
    }
    // @TODO: Implement device_type_unregister
    return -ENOTSUP;
}

// Get a device slot by its major and minor numbers
// Unlocked
static device_t **__dev_slot_get(int major, int minor) {
    device_type_t *dev_type = __dev_table[major];
    return &((device_t **)dev_type->minors)[minor];
        return NULL; // Device type not registered
    }
    return &dev_type->minors[minor];
}

// Get a device by its major and minor numbers
// And increment its reference count
int device_get(int major, int minor, device_t **dev) {
    if (major < 0 || major >= MAX_MAJOR_DEVICES || minor < 0 || minor >= MAX_MINOR_DEVICES) {
        return -EINVAL;
    }
    if (dev == NULL) {
        return -EINVAL; // Null pointer for device output
    }
    __dev_tab_lock();
    device_t **device = __dev_slot_get(major, minor);
    if (device == NULL || *device == NULL) {
        __dev_tab_unlock();
        return -ENODEV;
    }
    (*device)->ref_count++;
    *dev = *device;
    __dev_tab_unlock();
    return 0;
}

// Decrement the reference count of a device
int device_put(device_t *device) {
    if (device == NULL) {
        return -EINVAL; // Null pointer for device
    }
    __dev_tab_lock();
    if (device->ref_count <= 0) {
        __dev_tab_unlock();
        return -EINVAL; // Invalid reference count
    }
    device->ref_count--;
    __dev_tab_unlock();
    return 0;
}

int device_register(device_t *dev) {
    if (dev == NULL) {
        return -EINVAL; // Null pointer for device
    }
    if (dev->type != NULL || dev->valid) {
        return -EINVAL; // Device already registered
    }
    if (dev->major < 0 || dev->major >= MAX_MAJOR_DEVICES 
        || dev->minor < 0 || dev->minor >= MAX_MINOR_DEVICES) {
        return -EINVAL; // Invalid device numbers
    }
    __dev_tab_lock();
    device_t **device = __dev_slot_get(dev->major, dev->minor);
    if (device == NULL) {
        __dev_tab_unlock();
        return -ENODEV;
    }
    if (*device != NULL) {
        __dev_tab_unlock();
        return -EBUSY; // Device already registered
    }
    *device = dev;
    dev->ref_count = 0;
    dev->valid = 1;
    __dev_tab_unlock();
    return 0;
}

int device_unregister(device_t *dev) {
    if (dev == NULL) {
        return -EINVAL; // Null pointer for device
    }
    // @TODO: Implement device_unregister
    return -ENOTSUP;
}
