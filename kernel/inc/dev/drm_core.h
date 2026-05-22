#ifndef KERNEL_DEV_DRM_CORE_H
#define KERNEL_DEV_DRM_CORE_H

#include <types.h>
#include <lock/spinlock.h>

enum drm_core_node_type {
    DRM_CORE_NODE_LEGACY = 0,
    DRM_CORE_NODE_PRIMARY,
    DRM_CORE_NODE_RENDER,
};

struct drm_core_driver {
    const char *driver_name;
    const char *desc;
    uint32 driver_major;
    uint32 driver_minor;
    uint32 driver_patchlevel;
};

struct drm_core_device {
    const struct drm_core_driver *driver;
    spinlock_t lock;
    uint32 next_magic;
    uint64 master_owner_cookie;
};

struct drm_core_file {
    struct drm_core_device *dev;
    enum drm_core_node_type node_type;
    uint32 magic;
    uint64 client_caps;
    uint64 ioctl_count;
    int authenticated;
    int is_master;
    uint64 owner_cookie;
    pid_t owner_tgid;
};

void drm_core_device_init(struct drm_core_device *dev,
                          const struct drm_core_driver *driver);
void drm_core_file_init(struct drm_core_device *dev, struct drm_core_file *file,
                        enum drm_core_node_type node_type,
                        uint64 owner_cookie, pid_t owner_tgid);
const char *drm_core_node_name(enum drm_core_node_type type);
int drm_core_is_primary_like(const struct drm_core_file *file);
int drm_core_get_magic(struct drm_core_file *file, uint64 arg);
int drm_core_auth_magic(struct drm_core_file *file, uint64 arg);
int drm_core_get_client(struct drm_core_file *file, uint64 arg);
int drm_core_set_client_cap(struct drm_core_file *file, uint64 arg);
int drm_core_set_master(struct drm_core_file *file);
int drm_core_drop_master(struct drm_core_file *file);
void drm_core_release_file(struct drm_core_file *file);

#endif
