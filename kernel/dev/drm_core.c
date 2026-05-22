#include <types.h>
#include <defs.h>
#include <errno.h>
#include <string.h>
#include <dev/drm_core.h>
#include <mm/vm.h>
#include <uabi/drm.h>

void drm_core_device_init(struct drm_core_device *dev,
                          const struct drm_core_driver *driver)
{
    if (dev == NULL)
        return;
    memset(dev, 0, sizeof(*dev));
    dev->driver = driver;
    dev->next_magic = 1;
    spin_init(&dev->lock, "drm_core");
}

static uint32 drm_core_alloc_magic(struct drm_core_device *dev)
{
    uint32 magic;

    if (dev == NULL)
        return 1;
    spin_lock(&dev->lock);
    magic = dev->next_magic++;
    if (dev->next_magic == 0)
        dev->next_magic = 1;
    if (magic == 0)
        magic = 1;
    spin_unlock(&dev->lock);
    return magic;
}

void drm_core_file_init(struct drm_core_device *dev, struct drm_core_file *file,
                        enum drm_core_node_type node_type,
                        uint64 owner_cookie, pid_t owner_tgid)
{
    if (file == NULL)
        return;
    memset(file, 0, sizeof(*file));
    file->dev = dev;
    file->node_type = node_type;
    file->magic = drm_core_alloc_magic(dev);
    file->authenticated = node_type != DRM_CORE_NODE_PRIMARY;
    file->owner_cookie = owner_cookie;
    file->owner_tgid = owner_tgid;
}

const char *drm_core_node_name(enum drm_core_node_type type)
{
    switch (type) {
    case DRM_CORE_NODE_PRIMARY:
        return "primary";
    case DRM_CORE_NODE_RENDER:
        return "render";
    default:
        return "legacy";
    }
}

int drm_core_is_primary_like(const struct drm_core_file *file)
{
    return file != NULL &&
        (file->node_type == DRM_CORE_NODE_PRIMARY ||
         file->node_type == DRM_CORE_NODE_LEGACY);
}

int drm_core_get_magic(struct drm_core_file *file, uint64 arg)
{
    struct drm_auth_compat req;

    if (!drm_core_is_primary_like(file))
        return -EOPNOTSUPP;
    req.magic = file->magic;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

int drm_core_auth_magic(struct drm_core_file *file, uint64 arg)
{
    struct drm_auth_compat req;

    if (!drm_core_is_primary_like(file))
        return -EOPNOTSUPP;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.magic == 0 || req.magic != file->magic)
        return -EINVAL;
    file->authenticated = 1;
    return 0;
}

int drm_core_get_client(struct drm_core_file *file, uint64 arg)
{
    struct drm_client_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.idx != 0)
        return -EINVAL;
    req.auth = file ? file->authenticated : 0;
    req.pid = file ? (uint64)file->owner_tgid : 0;
    req.uid = 0;
    req.magic = file ? file->magic : 1;
    req.iocs = file ? file->ioctl_count : 0;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

int drm_core_set_client_cap(struct drm_core_file *file, uint64 arg)
{
    struct drm_set_client_cap_compat req;
    uint64 bit;

    if (file == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    switch (req.capability) {
    case DRM_CLIENT_CAP_STEREO_3D:
    case DRM_CLIENT_CAP_UNIVERSAL_PLANES:
    case DRM_CLIENT_CAP_ATOMIC:
    case DRM_CLIENT_CAP_ASPECT_RATIO:
    case DRM_CLIENT_CAP_WRITEBACK_CONNECTORS:
        break;
    default:
        return -EINVAL;
    }
    if (req.value > 1)
        return -EINVAL;
    bit = 1ULL << req.capability;
    if (req.value)
        file->client_caps |= bit;
    else
        file->client_caps &= ~bit;
    return 0;
}

int drm_core_set_master(struct drm_core_file *file)
{
    struct drm_core_device *dev;

    if (!drm_core_is_primary_like(file))
        return -EOPNOTSUPP;
    dev = file->dev;
    if (dev == NULL)
        return -ENODEV;

    spin_lock(&dev->lock);
    if (dev->master_owner_cookie != 0 &&
        dev->master_owner_cookie != file->owner_cookie) {
        spin_unlock(&dev->lock);
        return -EBUSY;
    }
    dev->master_owner_cookie = file->owner_cookie;
    file->is_master = 1;
    file->authenticated = 1;
    spin_unlock(&dev->lock);
    return 0;
}

int drm_core_drop_master(struct drm_core_file *file)
{
    struct drm_core_device *dev;

    if (!drm_core_is_primary_like(file))
        return -EOPNOTSUPP;
    dev = file->dev;
    if (dev == NULL)
        return -ENODEV;

    spin_lock(&dev->lock);
    if (dev->master_owner_cookie != file->owner_cookie ||
        !file->is_master) {
        spin_unlock(&dev->lock);
        return -EINVAL;
    }
    dev->master_owner_cookie = 0;
    file->is_master = 0;
    spin_unlock(&dev->lock);
    return 0;
}

static uint32 drm_core_file_node_mask(const struct drm_core_file *file)
{
    if (file == NULL)
        return 0;
    switch (file->node_type) {
    case DRM_CORE_NODE_PRIMARY:
        return DRM_CORE_IOCTL_PRIMARY;
    case DRM_CORE_NODE_RENDER:
        return DRM_CORE_IOCTL_RENDER;
    default:
        return DRM_CORE_IOCTL_LEGACY;
    }
}

int drm_core_dispatch_ioctl(struct drm_core_file *file, void *driver_file,
                            uint64 cmd, uint64 arg,
                            const struct drm_core_ioctl_desc *table,
                            uint32 count, const char **name_out,
                            int *known_out)
{
    uint32 mask;
    uint32 i;

    if (name_out != NULL)
        *name_out = NULL;
    if (known_out != NULL)
        *known_out = 0;
    if (file == NULL)
        return -EBADF;
    file->ioctl_count++;
    mask = drm_core_file_node_mask(file);
    for (i = 0; i < count; i++) {
        const struct drm_core_ioctl_desc *desc = &table[i];
        if (desc->cmd != cmd)
            continue;
        if (name_out != NULL)
            *name_out = desc->name;
        if (known_out != NULL)
            *known_out = 1;
        if ((desc->flags & mask) == 0) {
            if (known_out != NULL)
                *known_out = 2;
            return -EOPNOTSUPP;
        }
        if (desc->fn == NULL)
            return -EOPNOTSUPP;
        return desc->fn(file, driver_file, cmd, arg);
    }
    return -EINVAL;
}

void drm_core_release_file(struct drm_core_file *file)
{
    struct drm_core_device *dev;

    if (file == NULL || file->dev == NULL)
        return;
    dev = file->dev;
    spin_lock(&dev->lock);
    if (dev->master_owner_cookie == file->owner_cookie)
        dev->master_owner_cookie = 0;
    spin_unlock(&dev->lock);
}
