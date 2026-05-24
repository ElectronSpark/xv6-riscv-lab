
static int gpu_drm_is_primary_like(struct fb_gpu_render_owner *owner)
{
    return owner != NULL && drm_core_is_primary_like(&owner->drm);
}

static cdev_t gpu_render_cdev;

static int gpu_copyout_string(uint64 dst, uint64 *len, const char *src)
{
    uint64 actual = strlen(src);
    uint64 n, cap;

    if (len == NULL)
        return -EINVAL;
    cap = *len;
    n = cap;
    *len = actual;
    if (dst == 0 || n == 0)
        return 0;
    if (n > actual)
        n = actual;
    if (either_copyout(1, dst, (void *)src, n) < 0)
        return -EFAULT;
    if (cap > n) {
        char nul = 0;
        if (either_copyout(1, dst + n, &nul, 1) < 0)
            return -EFAULT;
    }
    return 0;
}

static int gpu_user_debug_name(uint64 user_ptr, char name[64])
{
    uint32 i;

    if (name == NULL)
        return -EINVAL;
    if (user_ptr == 0) {
        memcpy(name, "xv6-drm", sizeof("xv6-drm"));
        return 0;
    }

    for (i = 0; i < 63; i++) {
        if (either_copyin(&name[i], 1, user_ptr + i, 1) < 0)
            return -EFAULT;
        if (name[i] == 0)
            return 0;
    }
    name[63] = 0;
    return 0;
}

static void gpu_drm_mode_append_uint(char *buf, uint32 *pos, uint32 value)
{
    char tmp[10];
    uint32 n = 0;

    if (buf == NULL || pos == NULL || *pos >= 31)
        return;
    if (value == 0) {
        buf[(*pos)++] = '0';
        return;
    }
    while (value != 0 && n < sizeof(tmp)) {
        tmp[n++] = '0' + (value % 10);
        value /= 10;
    }
    while (n != 0 && *pos < 31)
        buf[(*pos)++] = tmp[--n];
}

static void gpu_drm_get_mode_size(uint32 *width, uint32 *height)
{
    uint32 w, h;

    spin_lock(&fb_state.lock);
    w = fb_state.xres;
    h = fb_state.yres;
    spin_unlock(&fb_state.lock);
    if (w < 640)
        w = FB_DEFAULT_WIDTH;
    if (h < 480)
        h = FB_DEFAULT_HEIGHT;
    if (width)
        *width = w;
    if (height)
        *height = h;
}

static void gpu_drm_fill_mode(struct drm_mode_modeinfo_compat *mode)
{
    uint32 w, h, pos = 0;
    uint32 hblank, vblank;

    memset(mode, 0, sizeof(*mode));
    gpu_drm_get_mode_size(&w, &h);

    hblank = w / 5;
    if (hblank < 160)
        hblank = 160;
    vblank = h / 20;
    if (vblank < 30)
        vblank = 30;

    mode->hdisplay = (uint16)w;
    mode->hsync_start = (uint16)(w + hblank / 3);
    mode->hsync_end = (uint16)(w + (2 * hblank) / 3);
    mode->htotal = (uint16)(w + hblank);
    mode->vdisplay = (uint16)h;
    mode->vsync_start = (uint16)(h + vblank / 3);
    mode->vsync_end = (uint16)(h + (2 * vblank) / 3);
    mode->vtotal = (uint16)(h + vblank);
    mode->vrefresh = 60;
    mode->clock = (uint32)(((uint64)mode->htotal * mode->vtotal *
                            mode->vrefresh) / 1000);
    if (mode->clock == 0)
        mode->clock = 40000;
    mode->flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
    mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

    gpu_drm_mode_append_uint(mode->name, &pos, w);
    if (pos < 31)
        mode->name[pos++] = 'x';
    gpu_drm_mode_append_uint(mode->name, &pos, h);
    mode->name[pos < 32 ? pos : 31] = 0;
}

static int gpu_drm_copyout_u32_array(uint64 ptr, uint32 capacity,
                                     const uint32 *ids, uint32 count)
{
    uint32 n;

    if (ptr == 0 || capacity == 0 || ids == NULL || count == 0)
        return 0;
    n = capacity < count ? capacity : count;
    if (either_copyout(1, ptr, (void *)ids, (uint64)n * sizeof(uint32)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_copyout_u64_array(uint64 ptr, uint32 capacity,
                                     const uint64 *values, uint32 count)
{
    uint32 n;

    if (ptr == 0 || capacity == 0 || values == NULL || count == 0)
        return 0;
    n = capacity < count ? capacity : count;
    if (either_copyout(1, ptr, (void *)values, (uint64)n * sizeof(uint64)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_copyout_mode_array(uint64 ptr, uint32 capacity,
                                      const struct drm_mode_modeinfo_compat *mode)
{
    if (ptr == 0 || capacity == 0 || mode == NULL)
        return 0;
    if (either_copyout(1, ptr, (void *)mode, sizeof(*mode)) < 0)
        return -EFAULT;
    return 0;
}

static void gpu_drm_copy_prop_name(char dst[32], const char *src)
{
    memset(dst, 0, 32);
    if (src != NULL)
        memmove(dst, src, strlen(src) < 32 ? strlen(src) : 31);
}

static int gpu_drm_property_info(uint32 prop_id, const char **name,
                                 uint32 *flags, uint64 values[2],
                                 uint32 *count_values,
                                 struct drm_mode_property_enum_compat *enums,
                                 uint32 *count_enums)
{
    if (name == NULL || flags == NULL || values == NULL ||
        count_values == NULL || enums == NULL || count_enums == NULL)
        return -EINVAL;
    *count_values = 0;
    *count_enums = 0;
    values[0] = 0;
    values[1] = 0;
    switch (prop_id) {
    case GPU_DRM_PROP_CRTC_ID:
        *name = "CRTC_ID";
        *flags = DRM_MODE_PROP_OBJECT | DRM_MODE_PROP_ATOMIC;
        break;
    case GPU_DRM_PROP_MODE_ID:
        *name = "MODE_ID";
        *flags = DRM_MODE_PROP_BLOB | DRM_MODE_PROP_ATOMIC;
        break;
    case GPU_DRM_PROP_ACTIVE:
        *name = "ACTIVE";
        *flags = DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC;
        values[1] = 1;
        *count_values = 2;
        break;
    case GPU_DRM_PROP_PLANE_TYPE:
        *name = "type";
        *flags = DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE;
        enums[0].value = DRM_PLANE_TYPE_PRIMARY;
        gpu_drm_copy_prop_name(enums[0].name, "Primary");
        *count_enums = 1;
        break;
    case GPU_DRM_PROP_FB_ID:
        *name = "FB_ID";
        *flags = DRM_MODE_PROP_OBJECT | DRM_MODE_PROP_ATOMIC;
        break;
    case GPU_DRM_PROP_SRC_X:
        *name = "SRC_X";
        *flags = DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC;
        values[1] = 0xffffffffULL;
        *count_values = 2;
        break;
    case GPU_DRM_PROP_SRC_Y:
        *name = "SRC_Y";
        *flags = DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC;
        values[1] = 0xffffffffULL;
        *count_values = 2;
        break;
    case GPU_DRM_PROP_SRC_W:
        *name = "SRC_W";
        *flags = DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC;
        values[1] = 0xffffffffULL;
        *count_values = 2;
        break;
    case GPU_DRM_PROP_SRC_H:
        *name = "SRC_H";
        *flags = DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC;
        values[1] = 0xffffffffULL;
        *count_values = 2;
        break;
    case GPU_DRM_PROP_CRTC_X:
        *name = "CRTC_X";
        *flags = DRM_MODE_PROP_SIGNED_RANGE | DRM_MODE_PROP_ATOMIC;
        values[1] = 8192;
        *count_values = 2;
        break;
    case GPU_DRM_PROP_CRTC_Y:
        *name = "CRTC_Y";
        *flags = DRM_MODE_PROP_SIGNED_RANGE | DRM_MODE_PROP_ATOMIC;
        values[1] = 8192;
        *count_values = 2;
        break;
    case GPU_DRM_PROP_CRTC_W:
        *name = "CRTC_W";
        *flags = DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC;
        values[1] = 8192;
        *count_values = 2;
        break;
    case GPU_DRM_PROP_CRTC_H:
        *name = "CRTC_H";
        *flags = DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC;
        values[1] = 8192;
        *count_values = 2;
        break;
    case GPU_DRM_PROP_IN_FENCE_FD:
        *name = "IN_FENCE_FD";
        *flags = DRM_MODE_PROP_SIGNED_RANGE | DRM_MODE_PROP_ATOMIC;
        values[0] = (uint64)-1;
        values[1] = 0x7fffffffULL;
        *count_values = 2;
        break;
    case GPU_DRM_PROP_OUT_FENCE_PTR:
        *name = "OUT_FENCE_PTR";
        *flags = DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC;
        values[1] = (uint64)-1;
        *count_values = 2;
        break;
    default:
        return -ENOENT;
    }
    return 0;
}

static int gpu_drm_current_capset(uint32 *capset_id)
{
    uint32 id = 0;
    int ret;

    ret = virtio_gpu_user_get_caps(NULL, 0, &id, NULL, NULL);
    if (ret != 0)
        return ret;
    if (id == 0)
        return -ENODEV;
    if (capset_id != NULL)
        *capset_id = id;
    return 0;
}

static void gpu_backend_copy_string(char *dst, size_t dst_size,
                                    const char *src)
{
    size_t i;

    if (dst == NULL || dst_size == 0)
        return;
    for (i = 0; i + 1 < dst_size && src != NULL && src[i] != 0; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static void gpu_backend_fill(struct fb_gpu_backend_info *info)
{
    struct hyperv_dxg_status dxg;

    memset(info, 0, sizeof(*info));
    info->backend = FB_GPU_BACKEND_DUMB;
    info->flags = FB_GPU_BACKEND_F_RENDER_NODE | FB_GPU_BACKEND_F_DUMB_BO;
    if (gpu_nouveau_device() != NULL)
        info->flags |= FB_GPU_BACKEND_F_DDA_NOUVEAU;
    gpu_backend_copy_string(info->name, sizeof(info->name), "dumb");
    gpu_backend_copy_string(info->renderer, sizeof(info->renderer),
                            "xv6 software/dumb-buffer render node");

    if (virtio_gpu_has_virgl()) {
        uint32 capset_id = 0, capset_version = 0, capset_size = 0;

        (void)virtio_gpu_user_get_caps(NULL, 0, &capset_id,
                                       &capset_version, &capset_size);
        info->backend = FB_GPU_BACKEND_VIRGL;
        info->flags |= FB_GPU_BACKEND_F_VIRGL_OPENGL |
                       FB_GPU_BACKEND_F_OPENGL_SUBMIT;
        info->capset_id = capset_id;
        info->capset_version = capset_version;
        info->capset_size = capset_size;
        gpu_backend_copy_string(info->name, sizeof(info->name), "virgl");
        gpu_backend_copy_string(info->renderer, sizeof(info->renderer),
                                "OpenGL via virtio-gpu virgl");
        return;
    }

    if (hyperv_dxg_get_status(&dxg) == 0 &&
        (dxg.global_present || dxg.vgpu_present)) {
        info->backend = FB_GPU_BACKEND_HYPERV_DXG;
        if (dxg.global_open_ok && dxg.vgpu_open_ok)
            info->flags |= FB_GPU_BACKEND_F_DXG_TRANSPORT;
        info->dxg_global_open = dxg.global_open_ok != 0;
        info->dxg_vgpu_open = dxg.vgpu_open_ok != 0;
        info->dxg_d3dkmt = hyperv_dxg_d3dkmt_ready() != 0;
        if (info->dxg_d3dkmt)
            info->flags |= FB_GPU_BACKEND_F_D3DKMT;
        info->dxg_global_status = dxg.global_open_status;
        info->dxg_vgpu_status = dxg.vgpu_open_status;
        info->dxg_global_rx = dxg.global_rx_packets;
        info->dxg_vgpu_rx = dxg.vgpu_rx_packets;
        gpu_backend_copy_string(info->name, sizeof(info->name),
                                "hyperv-dxg");
        gpu_backend_copy_string(info->renderer, sizeof(info->renderer),
                                info->dxg_d3dkmt ?
                                "Hyper-V GPU-PV D3DKMT/DXCore; OpenGL-submit gated" :
                                "Hyper-V GPU-PV DXG transport; D3DKMT pending");
    }
}

static int gpu_backend_query(uint64 arg)
{
    struct fb_gpu_backend_info info;

    gpu_backend_fill(&info);
    if (either_copyout(1, arg, &info, sizeof(info)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_owner_create_context(struct fb_gpu_render_owner *owner,
                                    const char *name, uint32 capset_id)
{
    int ret;
    uint32 current_capset = 0;

    if (owner == NULL)
        return -EBADF;
    ret = gpu_drm_current_capset(&current_capset);
    if (ret != 0)
        return ret;
    if (capset_id == 0)
        capset_id = current_capset;
    ret = virtio_gpu_user_get_caps_for(capset_id, 0, NULL, 0, NULL, NULL,
                                       NULL);
    if (ret != 0)
        return ret;
    if (owner->capset_id != 0 && owner->capset_id != capset_id)
        return -EINVAL;
    if (owner->default_ctx_id != 0)
        return 0;
    ret = virtio_gpu_user_context_create(owner->id, owner->tgid, capset_id,
                                         name ? name : "xv6-drm",
                                         &owner->default_ctx_id);
    if (ret == 0)
        owner->capset_id = capset_id;
    return ret;
}

static int gpu_owner_ensure_context(struct fb_gpu_render_owner *owner)
{
    return gpu_owner_create_context(owner, "xv6-drm", 0);
}

static int gpu_drm_version(uint64 arg)
{
    struct drm_version_compat req;
    struct fb_gpu_backend_info backend;
    const struct drm_core_driver *core_driver = &fb_drm_driver;
    const char *driver;
    const char *desc;
    int ret;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    gpu_backend_fill(&backend);
    if (backend.backend == FB_GPU_BACKEND_VIRGL) {
        driver = "virtio_gpu";
        desc = backend.renderer[0] ? backend.renderer : "virtio GPU";
    } else if (gpu_nouveau_device() != NULL) {
        core_driver = &nouveau_drm_driver;
        driver = core_driver->driver_name;
        desc = core_driver->desc;
    } else {
        driver = core_driver->driver_name;
        desc = backend.renderer[0] ? backend.renderer : core_driver->desc;
    }
    req.version_major = (int)core_driver->driver_major;
    req.version_minor = (int)core_driver->driver_minor;
    req.version_patchlevel = (int)core_driver->driver_patchlevel;
    ret = gpu_copyout_string(req.name, &req.name_len, driver);
    if (ret != 0)
        return ret;
    ret = gpu_copyout_string(req.date, &req.date_len, "20260502");
    if (ret != 0)
        return ret;
    ret = gpu_copyout_string(req.desc, &req.desc_len, desc);
    if (ret != 0)
        return ret;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_get_unique(uint64 arg)
{
    struct drm_unique_compat req;
    struct fb_gpu_backend_info backend;
    struct pci_device_info *nvdev;
    char pci_unique[32];

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    gpu_backend_fill(&backend);
    nvdev = gpu_nouveau_device();
    if (nvdev != NULL) {
        memset(pci_unique, 0, sizeof(pci_unique));
        pci_unique[0] = 'p';
        pci_unique[1] = 'c';
        pci_unique[2] = 'i';
        pci_unique[3] = ':';
        pci_unique[4] = '0';
        pci_unique[5] = '0';
        pci_unique[6] = '0';
        pci_unique[7] = '0';
        pci_unique[8] = ':';
        pci_unique[9] = "0123456789abcdef"[(nvdev->bus >> 4) & 0xf];
        pci_unique[10] = "0123456789abcdef"[nvdev->bus & 0xf];
        pci_unique[11] = ':';
        pci_unique[12] = "0123456789abcdef"[(nvdev->dev >> 4) & 0xf];
        pci_unique[13] = "0123456789abcdef"[nvdev->dev & 0xf];
        pci_unique[14] = '.';
        pci_unique[15] = '0' + (nvdev->func & 0x7);
        if (gpu_copyout_string(req.unique, &req.unique_len, pci_unique) != 0)
            return -EFAULT;
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }
    if (gpu_copyout_string(req.unique, &req.unique_len,
                           backend.backend == FB_GPU_BACKEND_HYPERV_DXG ?
                               "vmbus:hyperv-dxg" : "pci:0000:00:04.0") != 0)
        return -EFAULT;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_get_cap(uint64 arg)
{
    struct drm_get_cap_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    switch (req.capability) {
    case DRM_CAP_DUMB_BUFFER:
    case DRM_CAP_VBLANK_HIGH_CRTC:
    case DRM_CAP_TIMESTAMP_MONOTONIC:
        req.value = 1;
        break;
    case DRM_CAP_DUMB_PREFERRED_DEPTH:
        req.value = 32;
        break;
    case DRM_CAP_DUMB_PREFER_SHADOW:
        req.value = 0;
        break;
    case DRM_CAP_PRIME:
        req.value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;
        break;
    case DRM_CAP_CURSOR_WIDTH:
    case DRM_CAP_CURSOR_HEIGHT:
        req.value = 64;
        break;
    case DRM_CAP_ADDFB2_MODIFIERS:
        req.value = 1;
        break;
    case DRM_CAP_ASYNC_PAGE_FLIP:
    case DRM_CAP_PAGE_FLIP_TARGET:
    case DRM_CAP_CRTC_IN_VBLANK_EVENT:
    case DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP:
        req.value = 0;
        break;
    case DRM_CAP_SYNCOBJ:
    case DRM_CAP_SYNCOBJ_TIMELINE:
        req.value = 1;
        break;
    default:
        return -EINVAL;
    }
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_get_magic(struct fb_gpu_render_owner *owner, uint64 arg)
{
    if (owner == NULL)
        return -EBADF;
    return drm_core_get_magic(&owner->drm, arg);
}

static int gpu_drm_auth_magic(struct fb_gpu_render_owner *owner, uint64 arg)
{
    int ret;

    if (owner == NULL)
        return -EBADF;
    ret = drm_core_auth_magic(&owner->drm, arg);
    if (ret == 0) {
        spin_lock(&fb_state.lock);
        fb_state.stats.drm_auths++;
        spin_unlock(&fb_state.lock);
    }
    return ret;
}

static int gpu_drm_get_client(struct fb_gpu_render_owner *owner, uint64 arg)
{
    if (owner == NULL)
        return -EBADF;
    return drm_core_get_client(&owner->drm, arg);
}

static int gpu_drm_set_client_cap(struct fb_gpu_render_owner *owner,
                                  uint64 arg)
{
    if (owner == NULL)
        return -EBADF;
    return drm_core_set_client_cap(&owner->drm, arg);
}

static int gpu_drm_set_client_name(struct fb_gpu_render_owner *owner,
                                   uint64 arg)
{
    struct drm_set_client_name_compat req;
    char name[DRM_CLIENT_NAME_MAX_LEN];

    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.name_len > DRM_CLIENT_NAME_MAX_LEN)
        return -EINVAL;
    if (req.name_len != 0 && req.name == 0)
        return -EINVAL;
    if (req.name_len != 0 &&
        either_copyin(name, 1, req.name, req.name_len) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_legacy_get_map(uint64 arg)
{
    struct drm_map_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    return -EINVAL;
}

static int gpu_drm_legacy_add_rm_map(uint64 arg, int remove)
{
    struct drm_map_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if ((!remove && req.size == 0) || (remove && req.handle == 0))
        return -EINVAL;
    return -EOPNOTSUPP;
}

static int gpu_drm_legacy_sarea_ctx(uint64 arg)
{
    struct drm_ctx_priv_map_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    return -EOPNOTSUPP;
}

static int gpu_drm_legacy_get_ctx(uint64 arg)
{
    struct drm_ctx_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.handle != 0)
        return -EINVAL;
    req.flags = 0;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_legacy_ctx_mutate(uint64 arg, int allow_zero_handle)
{
    struct drm_ctx_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (!allow_zero_handle && req.handle == 0)
        return -EINVAL;
    return -EOPNOTSUPP;
}

static int gpu_drm_legacy_res_ctx(uint64 arg)
{
    struct drm_ctx_res_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.count < 0)
        return -EINVAL;
    req.count = 0;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_legacy_lock(uint64 arg)
{
    struct drm_lock_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.context < 0)
        return -EINVAL;
    return -EOPNOTSUPP;
}

static int gpu_drm_legacy_buf_desc(uint64 arg)
{
    struct drm_buf_desc_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.count < 0 || req.size < 0)
        return -EINVAL;
    return -EOPNOTSUPP;
}

static int gpu_drm_legacy_info_bufs(uint64 arg)
{
    struct drm_buf_info_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.count < 0)
        return -EINVAL;
    req.count = 0;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_legacy_map_bufs(uint64 arg)
{
    struct drm_buf_map_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.count < 0)
        return -EINVAL;
    req.count = 0;
    req.virtual = 0;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_legacy_free_bufs(uint64 arg)
{
    struct drm_buf_free_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.count <= 0 || req.list == 0)
        return -EINVAL;
    return -EOPNOTSUPP;
}

static int gpu_drm_legacy_dma(uint64 arg)
{
    struct drm_dma_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.context < 0 || req.send_count < 0 ||
        req.request_count < 0 || req.request_size < 0)
        return -EINVAL;
    return -EOPNOTSUPP;
}

static int gpu_drm_legacy_agp_struct(uint64 arg, uint64 cmd)
{
    union {
        struct drm_agp_mode_compat mode;
        struct drm_agp_buffer_compat buffer;
        struct drm_agp_binding_compat binding;
        struct drm_scatter_gather_compat sg;
    } req;
    uint64 len = 0;

    switch (cmd) {
    case DRM_IOCTL_AGP_ENABLE:
        len = sizeof(req.mode);
        break;
    case DRM_IOCTL_AGP_ALLOC:
    case DRM_IOCTL_AGP_FREE:
        len = sizeof(req.buffer);
        break;
    case DRM_IOCTL_AGP_BIND:
    case DRM_IOCTL_AGP_UNBIND:
        len = sizeof(req.binding);
        break;
    case DRM_IOCTL_SG_ALLOC:
    case DRM_IOCTL_SG_FREE:
        len = sizeof(req.sg);
        break;
    default:
        break;
    }
    if (len != 0 && either_copyin(&req, 1, arg, len) < 0)
        return -EFAULT;
    if ((cmd == DRM_IOCTL_AGP_ALLOC && req.buffer.size == 0) ||
        (cmd == DRM_IOCTL_SG_ALLOC && req.sg.size == 0))
        return -EINVAL;
    return -EOPNOTSUPP;
}

static int gpu_drm_set_master(struct fb_gpu_render_owner *owner)
{
    int ret;

    if (owner == NULL)
        return -EBADF;
    ret = drm_core_set_master(&owner->drm);
    if (ret == 0) {
        spin_lock(&fb_state.lock);
        fb_state.stats.drm_master_sets++;
        spin_unlock(&fb_state.lock);
    }
    return ret;
}

static int gpu_drm_drop_master(struct fb_gpu_render_owner *owner)
{
    int ret;

    if (owner == NULL)
        return -EBADF;
    ret = drm_core_drop_master(&owner->drm);
    if (ret == 0) {
        spin_lock(&fb_state.lock);
        fb_state.stats.drm_master_drops++;
        spin_unlock(&fb_state.lock);
    }
    return ret;
}

static int gpu_drm_get_stats(uint64 arg)
{
    struct drm_stats_compat req;

    memset(&req, 0, sizeof(req));
    req.count = 6;
    spin_lock(&fb_state.lock);
    req.data[0].value = fb_state.stats.drm_ioctls;
    req.data[1].value = fb_state.stats.bo_allocs;
    req.data[2].value = fb_state.stats.bo_bytes / 1024;
    req.data[3].value = fb_state.stats.fence_fd_polls;
    req.data[4].value = fb_state.stats.drm_unknown_ioctls;
    req.data[5].value = fb_state.stats.drm_master_sets;
    spin_unlock(&fb_state.lock);
    req.data[0].type = 3; /* _DRM_STAT_IOCTLS */
    req.data[1].type = 8; /* _DRM_STAT_COUNT */
    req.data[2].type = 7; /* _DRM_STAT_BYTE */
    req.data[3].type = 8; /* _DRM_STAT_COUNT */
    req.data[4].type = 8; /* _DRM_STAT_COUNT */
    req.data[5].type = 8; /* _DRM_STAT_COUNT */
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_set_version(uint64 arg)
{
    struct drm_set_version_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    req.drm_di_major = 1;
    if (req.drm_di_minor < 4)
        req.drm_di_minor = 4;
    req.drm_dd_major = 0;
    req.drm_dd_minor = 1;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_wait_vblank(uint64 arg)
{
    union drm_wait_vblank_compat req;
    uint64 sequence;
    uint64 timestamp_ns;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    spin_lock(&fb_state.lock);
    gpu_kms_sample_vblank_locked(req.request.sequence + 1, &sequence,
                                 &timestamp_ns);
    spin_unlock(&fb_state.lock);
    req.reply.sequence = (uint32)sequence;
    req.reply.tval_sec = (int64)(timestamp_ns / 1000000000ULL);
    req.reply.tval_usec =
        (int64)((timestamp_ns % 1000000000ULL) / 1000ULL);
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_event_queue_locked(struct fb_gpu_render_owner *owner,
                                      uint32 type, uint64 user_data,
                                      uint64 sequence, uint32 crtc_id,
                                      uint64 timestamp_ns)
{
    struct drm_event_vblank_compat *ev;

    if (owner == NULL)
        return -EBADF;
    if (owner->drm_event_count >= FB_GPU_DRM_EVENT_QUEUE_CAPACITY) {
        fb_state.stats.drm_event_queue_overflows++;
        fb_state.stats.drm_event_queue_dropped++;
        return -EAGAIN;
    }

    if (timestamp_ns == 0)
        timestamp_ns = r_time() * 100ULL;
    ev = &owner->drm_events[owner->drm_event_tail];
    memset(ev, 0, sizeof(*ev));
    ev->base.type = type;
    ev->base.length = sizeof(*ev);
    ev->user_data = user_data;
    ev->tv_sec = (uint32)(timestamp_ns / 1000000000ULL);
    ev->tv_usec = (uint32)((timestamp_ns % 1000000000ULL) / 1000ULL);
    ev->sequence = (uint32)sequence;
    ev->crtc_id = crtc_id;

    owner->drm_event_tail =
        (owner->drm_event_tail + 1) % FB_GPU_DRM_EVENT_QUEUE_CAPACITY;
    owner->drm_event_count++;
    if (owner->drm_event_count > owner->drm_event_high_water)
        owner->drm_event_high_water = owner->drm_event_count;
    fb_state.stats.drm_events_queued++;
    fb_state.stats.drm_event_queue_depth++;
    if (fb_state.stats.drm_event_queue_depth >
        fb_state.stats.drm_event_queue_high_water)
        fb_state.stats.drm_event_queue_high_water =
            fb_state.stats.drm_event_queue_depth;
    if (owner->drm_event_high_water >
        fb_state.stats.drm_event_file_high_water)
        fb_state.stats.drm_event_file_high_water =
            owner->drm_event_high_water;
    return 0;
}

static uint32
gpu_drm_event_release_stale_locked(struct fb_gpu_render_owner *owner)
{
    uint32 stale;

    if (owner == NULL || owner->drm_event_count == 0)
        return 0;
    stale = owner->drm_event_count;
    fb_state.stats.drm_event_close_stale += stale;
    if (fb_state.stats.drm_event_queue_depth >= stale)
        fb_state.stats.drm_event_queue_depth -= stale;
    else
        fb_state.stats.drm_event_queue_depth = 0;
    owner->drm_event_head = 0;
    owner->drm_event_tail = 0;
    owner->drm_event_count = 0;
    return stale;
}

static void gpu_kms_sample_vblank_locked(uint64 min_sequence,
                                         uint64 *sequence_out,
                                         uint64 *timestamp_ns_out)
{
    uint64 now = r_time();
    uint64 display_seq = fb_state.stats.display_last_complete;
    uint64 last = fb_state.stats.kms_vblank_last_tick;
    uint64 seq = fb_state.stats.kms_vblank_sequence;

    if (display_seq != 0 &&
        (min_sequence == 0 || display_seq >= min_sequence)) {
        fb_state.stats.kms_vblank_sequence = display_seq;
        fb_state.stats.kms_vblank_last_tick = now;
        fb_state.stats.kms_vblank_timestamp_ns = now * 100ULL;
        fb_state.stats.kms_vblank_samples++;
        fb_state.stats.kms_vblank_synthetic = 0;
        fb_state.stats.kms_vblank_display_correlated = 1;
        if (sequence_out != NULL)
            *sequence_out = display_seq;
        if (timestamp_ns_out != NULL)
            *timestamp_ns_out = fb_state.stats.kms_vblank_timestamp_ns;
        return;
    }

    if (seq == 0)
        seq = 1;
    if (last == 0)
        last = now;
    if (now > last) {
        uint64 elapsed = now - last;
        uint64 steps = elapsed / FB_GPU_SYNTHETIC_VBLANK_PERIOD_TICKS;

        if (steps != 0) {
            seq += steps;
            last += steps * FB_GPU_SYNTHETIC_VBLANK_PERIOD_TICKS;
        }
    }
    if (min_sequence != 0 && seq < min_sequence) {
        seq = min_sequence;
        last = now;
    }

    fb_state.stats.kms_vblank_sequence = seq;
    fb_state.stats.kms_vblank_last_tick = last;
    fb_state.stats.kms_vblank_timestamp_ns = last * 100ULL;
    fb_state.stats.kms_vblank_samples++;
    fb_state.stats.kms_vblank_synthetic = 1;
    fb_state.stats.kms_vblank_display_correlated = 0;
    if (sequence_out != NULL)
        *sequence_out = seq;
    if (timestamp_ns_out != NULL)
        *timestamp_ns_out = fb_state.stats.kms_vblank_timestamp_ns;
}

static int gpu_drm_crtc_get_sequence(uint64 arg)
{
    struct drm_crtc_get_sequence_compat req;
    uint64 sequence;
    uint64 timestamp_ns;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.crtc_id != GPU_DRM_CRTC_ID)
        return -EINVAL;
    spin_lock(&fb_state.lock);
    req.active = fb_state.current_kms_fb_id != 0;
    gpu_kms_sample_vblank_locked(0, &sequence, &timestamp_ns);
    spin_unlock(&fb_state.lock);
    req.sequence = sequence;
    req.sequence_ns = (int64)timestamp_ns;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_crtc_queue_sequence(uint64 arg)
{
    struct drm_crtc_queue_sequence_compat req;
    int ret = -EOPNOTSUPP;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.crtc_id != GPU_DRM_CRTC_ID)
        return -EINVAL;
    spin_lock(&fb_state.lock);
    if ((req.flags & ~DRM_CRTC_SEQUENCE_FLAGS) != 0) {
        fb_state.stats.kms_crtc_queue_sequence_bad_flags++;
        ret = -EINVAL;
    } else {
        fb_state.stats.kms_crtc_queue_sequence_rejects++;
    }
    fb_state.stats.kms_crtc_queue_sequence_noevent_rejects++;
    spin_unlock(&fb_state.lock);
    return ret;
}

static int gpu_drm_mode_gamma(uint64 arg, int set)
{
    struct drm_mode_crtc_lut_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.crtc_id != GPU_DRM_CRTC_ID)
        return -EINVAL;
    if (req.gamma_size != 0 &&
        (req.red == 0 || req.green == 0 || req.blue == 0))
        return -EINVAL;
    if (set || req.gamma_size != 0)
        return -EOPNOTSUPP;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_kms_fb_owner_matches(const struct fb_gpu_kms_fb_entry *fb,
                                    struct fb_gpu_render_owner *owner);

static int gpu_drm_mode_getresources(struct fb_gpu_render_owner *owner,
                                     uint64 arg)
{
    struct drm_mode_card_res_compat req;
    uint32 ids[FB_GPU_MAX_KMS_FBS > 1 ? FB_GPU_MAX_KMS_FBS : 1];
    uint32 w, h;
    uint32 fb_count = 0;
    int ret;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    w = fb_state.xres;
    h = fb_state.yres;
    if (w < 640)
        w = FB_DEFAULT_WIDTH;
    if (h < 480)
        h = FB_DEFAULT_HEIGHT;

    spin_lock(&fb_state.lock);
    for (uint32 i = 0; i < FB_GPU_MAX_KMS_FBS; i++) {
        struct fb_gpu_kms_fb_entry *fb = &fb_state.kms_fbs[i];

        if (!fb->in_use || !gpu_kms_fb_owner_matches(fb, owner))
            continue;
        ids[fb_count++] = fb->fb_id;
    }
    spin_unlock(&fb_state.lock);
    ret = gpu_drm_copyout_u32_array(req.fb_id_ptr, req.count_fbs, ids,
                                    fb_count);
    if (ret != 0)
        return ret;

    ids[0] = GPU_DRM_CRTC_ID;
    ret = gpu_drm_copyout_u32_array(req.crtc_id_ptr, req.count_crtcs, ids, 1);
    if (ret != 0)
        return ret;
    ids[0] = GPU_DRM_CONNECTOR_ID;
    ret = gpu_drm_copyout_u32_array(req.connector_id_ptr,
                                    req.count_connectors, ids, 1);
    if (ret != 0)
        return ret;
    ids[0] = GPU_DRM_ENCODER_ID;
    ret = gpu_drm_copyout_u32_array(req.encoder_id_ptr,
                                    req.count_encoders, ids, 1);
    if (ret != 0)
        return ret;

    req.count_fbs = fb_count;
    req.count_crtcs = 1;
    req.count_connectors = 1;
    req.count_encoders = 1;
    req.min_width = 1;
    req.max_width = w > 8192 ? w : 8192;
    req.min_height = 1;
    req.max_height = h > 8192 ? h : 8192;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_mode_getcrtc(uint64 arg)
{
    struct drm_mode_crtc_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.crtc_id != GPU_DRM_CRTC_ID)
        return -ENOENT;
    spin_lock(&fb_state.lock);
    req.fb_id = fb_state.current_kms_fb_id;
    spin_unlock(&fb_state.lock);
    req.x = 0;
    req.y = 0;
    req.gamma_size = 0;
    req.mode_valid = 1;
    gpu_drm_fill_mode(&req.mode);
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_mode_getencoder(uint64 arg)
{
    struct drm_mode_get_encoder_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.encoder_id != GPU_DRM_ENCODER_ID)
        return -ENOENT;
    req.encoder_type = DRM_MODE_ENCODER_VIRTUAL;
    req.crtc_id = GPU_DRM_CRTC_ID;
    req.possible_crtcs = 1;
    req.possible_clones = 1;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_mode_getconnector(uint64 arg)
{
    struct drm_mode_get_connector_compat req;
    struct drm_mode_modeinfo_compat mode;
    uint32 encoder = GPU_DRM_ENCODER_ID;
    uint32 props[2] = { GPU_DRM_PROP_CRTC_ID, GPU_DRM_PROP_MODE_ID };
    uint64 prop_values[2] = { GPU_DRM_CRTC_ID, GPU_DRM_MODE_BLOB_ID };
    int ret;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.connector_id != GPU_DRM_CONNECTOR_ID)
        return -ENOENT;
    gpu_drm_fill_mode(&mode);
    ret = gpu_drm_copyout_u32_array(req.encoders_ptr, req.count_encoders,
                                    &encoder, 1);
    if (ret != 0)
        return ret;
    ret = gpu_drm_copyout_mode_array(req.modes_ptr, req.count_modes, &mode);
    if (ret != 0)
        return ret;
    ret = gpu_drm_copyout_u32_array(req.props_ptr, req.count_props, props, 2);
    if (ret != 0)
        return ret;
    ret = gpu_drm_copyout_u64_array(req.prop_values_ptr, req.count_props,
                                    prop_values, 2);
    if (ret != 0)
        return ret;

    req.count_modes = 1;
    req.count_props = 2;
    req.count_encoders = 1;
    req.encoder_id = GPU_DRM_ENCODER_ID;
    req.connector_type = DRM_MODE_CONNECTOR_VIRTUAL;
    req.connector_type_id = 1;
    req.connection = DRM_MODE_CONNECTED;
    req.mm_width = mode.hdisplay / 4;
    req.mm_height = mode.vdisplay / 4;
    req.subpixel = DRM_MODE_SUBPIXEL_UNKNOWN;
    req.pad = 0;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_mode_getproperty(uint64 arg)
{
    struct drm_mode_get_property_compat req;
    const char *name;
    uint64 values[2];
    struct drm_mode_property_enum_compat enums[3];
    uint32 count_values;
    uint32 count_enums;
    int ret;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    memset(values, 0, sizeof(values));
    memset(enums, 0, sizeof(enums));
    ret = gpu_drm_property_info(req.prop_id, &name, &req.flags, values,
                                &count_values, enums, &count_enums);
    if (ret != 0)
        return ret;
    ret = gpu_drm_copyout_u64_array(req.values_ptr, req.count_values,
                                    values, count_values);
    if (ret != 0)
        return ret;
    if (req.enum_blob_ptr != 0 && req.count_enum_blobs != 0 &&
        count_enums != 0) {
        uint32 n = req.count_enum_blobs < count_enums ?
            req.count_enum_blobs : count_enums;

        if (either_copyout(1, req.enum_blob_ptr, enums,
                           (uint64)n * sizeof(enums[0])) < 0)
            return -EFAULT;
    }
    gpu_drm_copy_prop_name(req.name, name);
    req.count_values = count_values;
    req.count_enum_blobs = count_enums;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_mode_getblob(uint64 arg)
{
    struct drm_mode_get_blob_compat req;
    struct drm_mode_modeinfo_compat mode;
    uint32 capacity;
    uint32 n;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.blob_id != GPU_DRM_MODE_BLOB_ID)
        return -ENOENT;
    gpu_drm_fill_mode(&mode);
    capacity = req.length;
    req.length = sizeof(mode);
    if (req.data != 0 && capacity != 0) {
        n = capacity < sizeof(mode) ? capacity : sizeof(mode);
        if (either_copyout(1, req.data, &mode, n) < 0)
            return -EFAULT;
    }
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_mode_getplaneresources(uint64 arg)
{
    struct drm_mode_get_plane_res_compat req;
    uint32 plane_id = GPU_DRM_PRIMARY_PLANE_ID;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.plane_id_ptr != 0 && req.count_planes != 0 &&
        either_copyout(1, req.plane_id_ptr, &plane_id,
                       sizeof(plane_id)) < 0)
        return -EFAULT;
    req.count_planes = 1;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_mode_getplane(uint64 arg)
{
    struct drm_mode_get_plane_compat req;
    uint32 formats[2] = {
        DRM_FORMAT_XRGB8888,
        DRM_FORMAT_ARGB8888,
    };
    uint32 copy_count;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.plane_id != GPU_DRM_PRIMARY_PLANE_ID)
        return -ENOENT;
    copy_count = req.count_format_types;
    if (copy_count > sizeof(formats) / sizeof(formats[0]))
        copy_count = sizeof(formats) / sizeof(formats[0]);
    if (req.format_type_ptr != 0 && copy_count != 0 &&
        either_copyout(1, req.format_type_ptr, formats,
                       copy_count * sizeof(formats[0])) < 0)
        return -EFAULT;
    req.crtc_id = GPU_DRM_CRTC_ID;
    req.fb_id = fb_state.current_kms_fb_id;
    req.possible_crtcs = 1;
    req.gamma_size = 0;
    req.count_format_types = sizeof(formats) / sizeof(formats[0]);
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static struct fb_gpu_kms_fb_entry *gpu_kms_fb_lookup_locked(uint32 fb_id)
{
    for (uint32 i = 0; i < FB_GPU_MAX_KMS_FBS; i++) {
        if (fb_state.kms_fbs[i].in_use && fb_state.kms_fbs[i].fb_id == fb_id)
            return &fb_state.kms_fbs[i];
    }
    return NULL;
}

static int gpu_kms_fb_owner_matches(const struct fb_gpu_kms_fb_entry *fb,
                                    struct fb_gpu_render_owner *owner)
{
    if (fb == NULL || owner == NULL)
        return 0;
    return fb->owner_id == owner->id && fb->owner_tgid == owner->tgid;
}

static int gpu_drm_mode_setplane(struct fb_gpu_render_owner *owner,
                                 uint64 arg)
{
    struct drm_mode_set_plane_compat req;
    struct fb_gpu_kms_fb_entry *fb;
    int owned = 0;

    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.flags != 0 || req.plane_id != GPU_DRM_PRIMARY_PLANE_ID)
        return -EINVAL;
    if ((req.crtc_id == 0) != (req.fb_id == 0))
        return -EINVAL;
    if (req.crtc_id != 0 && req.crtc_id != GPU_DRM_CRTC_ID)
        return -EINVAL;
    if (req.fb_id != 0) {
        spin_lock(&fb_state.lock);
        fb = gpu_kms_fb_lookup_locked(req.fb_id);
        owned = gpu_kms_fb_owner_matches(fb, owner);
        spin_unlock(&fb_state.lock);
        if (!owned)
            return -EINVAL;
    }
    return -EOPNOTSUPP;
}

static int gpu_kms_copy_fb_for_owner(struct fb_gpu_render_owner *owner,
                                     uint32 fb_id,
                                     struct fb_gpu_kms_fb_entry *out)
{
    int ret = -ENOENT;

    if (out == NULL)
        return -EINVAL;
    spin_lock(&fb_state.lock);
    for (uint32 i = 0; i < FB_GPU_MAX_KMS_FBS; i++) {
        struct fb_gpu_kms_fb_entry *fb = &fb_state.kms_fbs[i];

        if (!fb->in_use || fb->fb_id != fb_id ||
            !gpu_kms_fb_owner_matches(fb, owner))
            continue;
        *out = *fb;
        ret = 0;
        break;
    }
    spin_unlock(&fb_state.lock);
    return ret;
}

static int gpu_kms_present_fb(struct fb_gpu_render_owner *owner, uint32 fb_id)
{
    struct fb_gpu_bo_entry *bo;
    struct fb_gpu_bo_present present;
    uint32 bo_handle = 0;
    uint32 width = 0;
    uint32 height = 0;
    uint32 pixel_format = 0;
    uint64 fence = 0;
    int ret;

    if (fb_id == 0)
        return 0;
    spin_lock(&fb_state.lock);
    for (uint32 i = 0; i < FB_GPU_MAX_KMS_FBS; i++) {
        struct fb_gpu_kms_fb_entry *fb = &fb_state.kms_fbs[i];

        if (fb->fb_id != fb_id || !gpu_kms_fb_owner_matches(fb, owner))
            continue;
        bo_handle = fb->bo_handle;
        width = fb->width;
        height = fb->height;
        pixel_format = fb->pixel_format;
        break;
    }
    spin_unlock(&fb_state.lock);
    if (bo_handle == 0)
        return -ENOENT;
    if (width == 0 || height == 0)
        return -EINVAL;
    if (pixel_format != DRM_FORMAT_XRGB8888 &&
        pixel_format != DRM_FORMAT_ARGB8888)
        return -EOPNOTSUPP;

    bo = fb_bo_get_owned(bo_handle, owner->id, owner->tgid);
    if (bo == NULL)
        return -ENOENT;
    memset(&present, 0, sizeof(present));
    present.w = width;
    present.h = height;
    ret = fb_blit_from_bo(bo, present, &fence);
    fb_bo_put(bo);
    return ret;
}

static int gpu_kms_fb_presentable_for_owner(struct fb_gpu_render_owner *owner,
                                            uint32 fb_id)
{
    struct fb_gpu_kms_fb_entry fb;
    int ret;

    if (fb_id == 0)
        return 0;
    ret = gpu_kms_copy_fb_for_owner(owner, fb_id, &fb);
    if (ret != 0)
        return ret;
    if (fb.width == 0 || fb.height == 0)
        return -EINVAL;
    if (fb.pixel_format != DRM_FORMAT_XRGB8888 &&
        fb.pixel_format != DRM_FORMAT_ARGB8888)
        return -EOPNOTSUPP;
    return 0;
}

static int gpu_drm_mode_getfb(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_mode_fb_cmd_compat req;
    struct fb_gpu_kms_fb_entry fb;
    int ret;

    if (!gpu_drm_is_primary_like(owner))
        return -EOPNOTSUPP;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    ret = gpu_kms_copy_fb_for_owner(owner, req.fb_id, &fb);
    if (ret != 0)
        return ret;
    req.width = fb.width;
    req.height = fb.height;
    req.pitch = fb.pitch;
    req.bpp = 32;
    req.depth = fb.pixel_format == DRM_FORMAT_XRGB8888 ? 24 : 32;
    req.handle = fb.bo_handle;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_mode_getfb2(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_mode_fb_cmd2_compat req;
    struct fb_gpu_kms_fb_entry fb;
    int ret;

    if (!gpu_drm_is_primary_like(owner))
        return -EOPNOTSUPP;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    ret = gpu_kms_copy_fb_for_owner(owner, req.fb_id, &fb);
    if (ret != 0)
        return ret;
    memset(req.handles, 0, sizeof(req.handles));
    memset(req.pitches, 0, sizeof(req.pitches));
    memset(req.offsets, 0, sizeof(req.offsets));
    memset(req.modifier, 0, sizeof(req.modifier));
    req.width = fb.width;
    req.height = fb.height;
    req.pixel_format = fb.pixel_format;
    req.flags = DRM_MODE_FB_MODIFIERS;
    for (uint32 i = 0; i < fb.plane_count && i < 4; i++) {
        req.handles[i] = fb.bo_handles[i];
        req.pitches[i] = fb.pitches[i];
        req.offsets[i] = fb.offsets[i];
        req.modifier[i] = fb.modifier;
    }
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_mode_cursor(uint64 arg, int cursor2)
{
    struct drm_mode_cursor2_compat req;

    memset(&req, 0, sizeof(req));
    if (cursor2) {
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
    } else {
        struct drm_mode_cursor_compat old;
        if (either_copyin(&old, 1, arg, sizeof(old)) < 0)
            return -EFAULT;
        req.flags = old.flags;
        req.crtc_id = old.crtc_id;
        req.x = old.x;
        req.y = old.y;
        req.width = old.width;
        req.height = old.height;
        req.handle = old.handle;
    }
    if (req.crtc_id != GPU_DRM_CRTC_ID ||
        (req.flags & ~DRM_MODE_CURSOR_FLAGS) != 0)
        return -EINVAL;
    if ((req.flags & DRM_MODE_CURSOR_BO) != 0 && req.handle != 0)
        return -EOPNOTSUPP;
    return 0;
}

static int gpu_drm_mode_dirtyfb(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_mode_fb_dirty_cmd_compat req;
    struct fb_gpu_kms_fb_entry fb;
    int ret;

    if (!gpu_drm_is_primary_like(owner))
        return -EOPNOTSUPP;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if ((req.flags & ~DRM_MODE_FB_DIRTY_FLAGS) != 0 ||
        req.num_clips > DRM_MODE_FB_DIRTY_MAX_CLIPS ||
        (req.num_clips != 0 && req.clips_ptr == 0))
        return -EINVAL;
    ret = gpu_kms_copy_fb_for_owner(owner, req.fb_id, &fb);
    if (ret != 0)
        return ret;
    spin_lock(&fb_state.lock);
    ret = fb_state.current_kms_fb_id == req.fb_id ? 0 : 1;
    spin_unlock(&fb_state.lock);
    return ret == 0 ? gpu_kms_present_fb(owner, req.fb_id) : 0;
}

struct gpu_kms_obj_props {
    uint32 props[12];
    uint64 values[12];
    uint32 count;
};

#define GPU_KMS_MAX_IN_FENCES 8

static int gpu_kms_record_in_fence_locked(uint64 value, int32 *fds,
                                          uint32 *count, uint32 capacity)
{
    if (value == (uint64)-1)
        return 0;
    if (value > 0x7fffffffULL) {
        fb_state.stats.kms_atomic_in_fence_rejected++;
        return -EINVAL;
    }
    if (fds == NULL || count == NULL || *count >= capacity) {
        fb_state.stats.kms_atomic_in_fence_rejected++;
        return -EOPNOTSUPP;
    }
    if (*count != 0) {
        fb_state.stats.kms_atomic_in_fence_duplicate_rejects++;
        fb_state.stats.kms_atomic_in_fence_rejected++;
        return -EINVAL;
    }
    fds[*count] = (int32)value;
    *count = *count + 1;
    return 0;
}

static int gpu_kms_object_exists_locked(uint32 obj_id, uint32 obj_type,
                                        struct fb_gpu_render_owner *owner)
{
    if (obj_type == DRM_MODE_OBJECT_ANY) {
        if (obj_id == GPU_DRM_CRTC_ID || obj_id == GPU_DRM_CONNECTOR_ID ||
            obj_id == GPU_DRM_ENCODER_ID ||
            obj_id == GPU_DRM_PRIMARY_PLANE_ID ||
            obj_id == GPU_DRM_MODE_BLOB_ID)
            return 1;
        return gpu_kms_fb_owner_matches(gpu_kms_fb_lookup_locked(obj_id),
                                        owner);
    }
    switch (obj_type) {
    case DRM_MODE_OBJECT_CRTC:
        return obj_id == GPU_DRM_CRTC_ID;
    case DRM_MODE_OBJECT_CONNECTOR:
        return obj_id == GPU_DRM_CONNECTOR_ID;
    case DRM_MODE_OBJECT_ENCODER:
        return obj_id == GPU_DRM_ENCODER_ID;
    case DRM_MODE_OBJECT_PLANE:
        return obj_id == GPU_DRM_PRIMARY_PLANE_ID;
    case DRM_MODE_OBJECT_BLOB:
    case DRM_MODE_OBJECT_MODE:
        return obj_id == GPU_DRM_MODE_BLOB_ID;
    case DRM_MODE_OBJECT_FB:
        return gpu_kms_fb_owner_matches(gpu_kms_fb_lookup_locked(obj_id),
                                        owner);
    default:
        return 0;
    }
}

static void gpu_kms_push_prop(struct gpu_kms_obj_props *out, uint32 prop,
                              uint64 value)
{
    if (out == NULL || out->count >=
        sizeof(out->props) / sizeof(out->props[0]))
        return;
    out->props[out->count] = prop;
    out->values[out->count] = value;
    out->count++;
}

static int gpu_kms_collect_obj_props_locked(uint32 obj_id, uint32 obj_type,
                                            struct fb_gpu_render_owner *owner,
                                            struct gpu_kms_obj_props *out)
{
    uint32 w, h;
    uint32 current_fb = fb_state.current_kms_fb_id;

    if (out == NULL)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!gpu_kms_object_exists_locked(obj_id, obj_type, owner))
        return -ENOENT;
    w = fb_state.xres;
    h = fb_state.yres;
    if (w < 640)
        w = FB_DEFAULT_WIDTH;
    if (h < 480)
        h = FB_DEFAULT_HEIGHT;
    if (obj_type == DRM_MODE_OBJECT_ANY) {
        if (obj_id == GPU_DRM_CRTC_ID)
            obj_type = DRM_MODE_OBJECT_CRTC;
        else if (obj_id == GPU_DRM_CONNECTOR_ID)
            obj_type = DRM_MODE_OBJECT_CONNECTOR;
        else if (obj_id == GPU_DRM_PRIMARY_PLANE_ID)
            obj_type = DRM_MODE_OBJECT_PLANE;
        else if (obj_id == GPU_DRM_ENCODER_ID)
            obj_type = DRM_MODE_OBJECT_ENCODER;
        else if (obj_id == GPU_DRM_MODE_BLOB_ID)
            obj_type = DRM_MODE_OBJECT_BLOB;
        else
            obj_type = DRM_MODE_OBJECT_FB;
    }

    switch (obj_type) {
    case DRM_MODE_OBJECT_CRTC:
        gpu_kms_push_prop(out, GPU_DRM_PROP_ACTIVE,
                          current_fb != 0 ? 1 : 0);
        gpu_kms_push_prop(out, GPU_DRM_PROP_MODE_ID,
                          GPU_DRM_MODE_BLOB_ID);
        gpu_kms_push_prop(out, GPU_DRM_PROP_OUT_FENCE_PTR, 0);
        break;
    case DRM_MODE_OBJECT_CONNECTOR:
        gpu_kms_push_prop(out, GPU_DRM_PROP_CRTC_ID, GPU_DRM_CRTC_ID);
        gpu_kms_push_prop(out, GPU_DRM_PROP_MODE_ID,
                          GPU_DRM_MODE_BLOB_ID);
        break;
    case DRM_MODE_OBJECT_PLANE:
        gpu_kms_push_prop(out, GPU_DRM_PROP_PLANE_TYPE,
                          DRM_PLANE_TYPE_PRIMARY);
        gpu_kms_push_prop(out, GPU_DRM_PROP_CRTC_ID, GPU_DRM_CRTC_ID);
        gpu_kms_push_prop(out, GPU_DRM_PROP_FB_ID, current_fb);
        gpu_kms_push_prop(out, GPU_DRM_PROP_SRC_X, 0);
        gpu_kms_push_prop(out, GPU_DRM_PROP_SRC_Y, 0);
        gpu_kms_push_prop(out, GPU_DRM_PROP_SRC_W, (uint64)w << 16);
        gpu_kms_push_prop(out, GPU_DRM_PROP_SRC_H, (uint64)h << 16);
        gpu_kms_push_prop(out, GPU_DRM_PROP_CRTC_X, 0);
        gpu_kms_push_prop(out, GPU_DRM_PROP_CRTC_Y, 0);
        gpu_kms_push_prop(out, GPU_DRM_PROP_CRTC_W, w);
        gpu_kms_push_prop(out, GPU_DRM_PROP_CRTC_H, h);
        gpu_kms_push_prop(out, GPU_DRM_PROP_IN_FENCE_FD, (uint64)-1);
        break;
    default:
        break;
    }
    return 0;
}

static int gpu_drm_mode_obj_getproperties(struct fb_gpu_render_owner *owner,
                                          uint64 arg)
{
    struct drm_mode_obj_get_properties_compat req;
    struct gpu_kms_obj_props props;
    int ret;

    if (!gpu_drm_is_primary_like(owner))
        return -EOPNOTSUPP;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    spin_lock(&fb_state.lock);
    ret = gpu_kms_collect_obj_props_locked(req.obj_id, req.obj_type, owner,
                                           &props);
    spin_unlock(&fb_state.lock);
    if (ret != 0)
        return ret;
    ret = gpu_drm_copyout_u32_array(req.props_ptr, req.count_props,
                                    props.props, props.count);
    if (ret != 0)
        return ret;
    ret = gpu_drm_copyout_u64_array(req.prop_values_ptr, req.count_props,
                                    props.values, props.count);
    if (ret != 0)
        return ret;
    req.count_props = props.count;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_kms_validate_prop_locked(struct fb_gpu_render_owner *owner,
                                        uint32 obj_id, uint32 obj_type,
                                        uint32 prop, uint64 value,
                                        uint32 *new_fb_id, int *has_new_fb,
                                        uint64 *out_fence_ptr,
                                        int32 *in_fence_fds,
                                        uint32 *in_fence_count,
                                        uint32 in_fence_capacity)
{
    struct gpu_kms_obj_props props;
    int found = 0;
    int ret;

    ret = gpu_kms_collect_obj_props_locked(obj_id, obj_type, owner, &props);
    if (ret != 0)
        return ret;
    for (uint32 i = 0; i < props.count; i++) {
        if (props.props[i] == prop) {
            found = 1;
            break;
        }
    }
    if (!found)
        return -EINVAL;

    switch (prop) {
    case GPU_DRM_PROP_CRTC_ID:
        if (value != 0 && value != GPU_DRM_CRTC_ID)
            return -EINVAL;
        break;
    case GPU_DRM_PROP_MODE_ID:
        if (value != 0 && value != GPU_DRM_MODE_BLOB_ID)
            return -EINVAL;
        break;
    case GPU_DRM_PROP_ACTIVE:
        if (value > 1)
            return -EINVAL;
        if (value == 0 && new_fb_id != NULL && has_new_fb != NULL) {
            *new_fb_id = 0;
            *has_new_fb = 1;
        }
        break;
    case GPU_DRM_PROP_PLANE_TYPE:
        if (value != DRM_PLANE_TYPE_PRIMARY)
            return -EINVAL;
        break;
    case GPU_DRM_PROP_FB_ID:
        if (value != 0 &&
            !gpu_kms_fb_owner_matches(gpu_kms_fb_lookup_locked((uint32)value),
                                      owner))
            return -ENOENT;
        if (new_fb_id != NULL && has_new_fb != NULL) {
            *new_fb_id = (uint32)value;
            *has_new_fb = 1;
        }
        break;
    case GPU_DRM_PROP_IN_FENCE_FD:
        ret = gpu_kms_record_in_fence_locked(value, in_fence_fds,
                                             in_fence_count,
                                             in_fence_capacity);
        if (ret != 0)
            return ret;
        break;
    case GPU_DRM_PROP_OUT_FENCE_PTR:
        if (out_fence_ptr != NULL)
            *out_fence_ptr = value;
        break;
    default:
        break;
    }
    return 0;
}

static int gpu_drm_mode_obj_setproperty(struct fb_gpu_render_owner *owner,
                                        uint64 arg)
{
    struct drm_mode_obj_set_property_compat req;
    uint32 new_fb = 0;
    int has_new_fb = 0;
    int ret;

    if (!gpu_drm_is_primary_like(owner))
        return -EOPNOTSUPP;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    spin_lock(&fb_state.lock);
    ret = gpu_kms_validate_prop_locked(owner, req.obj_id, req.obj_type,
                                       req.prop_id, req.value, &new_fb,
                                       &has_new_fb, NULL, NULL, NULL, 0);
    if (ret == 0 && has_new_fb)
        fb_state.current_kms_fb_id = new_fb;
    spin_unlock(&fb_state.lock);
    return ret;
}

static int gpu_kms_in_fence_file_kind_ok(struct vfs_file *file)
{
    if (file == NULL || file->private_data == NULL)
        return -EINVAL;
    if (file->ops == &fb_fence_file_ops)
        return 0;
    if (file->ops == &fb_virgl_fence_file_ops)
        return 0;
    if (file->ops == &fb_syncobj_file_ops) {
        struct fb_gpu_syncobj_file *sync_file =
            (struct fb_gpu_syncobj_file *)file->private_data;

        return sync_file->kind == FB_GPU_SYNCOBJ_FD_SYNC_FILE ? 0 : -EINVAL;
    }
    return -EINVAL;
}

static int gpu_kms_get_in_fence_file_ref(int32 fd, struct vfs_file **out)
{
    struct vfs_file *file;
    int ret;

    if (out == NULL)
        return -EINVAL;
    *out = NULL;
    if (fd < 0)
        return -EINVAL;
    file = vfs_fdtable_get_file(current->fdtable, fd);
    if (file == NULL)
        return -EBADF;
    ret = gpu_kms_in_fence_file_kind_ok(file);
    if (ret != 0) {
        vfs_fput(file);
        return ret;
    }
    *out = file;
    spin_lock(&fb_state.lock);
    fb_state.stats.kms_atomic_in_fence_fd_refs++;
    spin_unlock(&fb_state.lock);
    return 0;
}

static void gpu_kms_put_in_fence_file_refs(struct vfs_file **files,
                                           uint32 count)
{
    if (files == NULL)
        return;
    for (uint32 i = 0; i < count; i++) {
        if (files[i] == NULL)
            continue;
        vfs_fput(files[i]);
        files[i] = NULL;
        spin_lock(&fb_state.lock);
        fb_state.stats.kms_atomic_in_fence_fd_ref_puts++;
        spin_unlock(&fb_state.lock);
    }
}

static int gpu_kms_wait_in_fence_file(struct fb_gpu_render_owner *owner,
                                      struct vfs_file *file)
{
    int ret = 0;

    (void)owner;
    if (file == NULL) {
        ret = -EBADF;
    } else if (file->ops == &fb_fence_file_ops &&
               file->private_data != NULL) {
        struct fb_gpu_fence_file *fence_file =
            (struct fb_gpu_fence_file *)file->private_data;

        spin_lock(&fb_state.lock);
        ret = fb_gpu_fence_file_wait_locked(fence_file);
        spin_unlock(&fb_state.lock);
    } else if (file->ops == &fb_virgl_fence_file_ops &&
               file->private_data != NULL) {
        struct fb_gpu_virgl_fence_file *fence_file =
            (struct fb_gpu_virgl_fence_file *)file->private_data;
        uint64 signaled = 0;

        ret = virtio_gpu_user_fence(fence_file->fence, 1, &signaled);
        if (ret == 0 && signaled < fence_file->fence)
            ret = -EAGAIN;
    } else if (file->ops == &fb_syncobj_file_ops &&
               file->private_data != NULL) {
        struct fb_gpu_syncobj_file *sync_file =
            (struct fb_gpu_syncobj_file *)file->private_data;
        struct fb_gpu_syncobj_state_entry *state;
        int ready;

        if (sync_file->kind != FB_GPU_SYNCOBJ_FD_SYNC_FILE) {
            ret = -EINVAL;
        } else if (sync_file->snapshot_signaled) {
            ret = 0;
        } else {
            spin_lock(&fb_state.lock);
            fb_state.stats.kms_atomic_in_fence_sync_file_pending_waits++;
            state = gpu_syncobj_state_locked(sync_file->state_index);
            ready = gpu_syncobj_state_ready_locked(
                state, sync_file->snapshot_timeline_value);
            if (ready)
                fb_state.stats.
                    kms_atomic_in_fence_sync_file_pending_wakeups++;
            spin_unlock(&fb_state.lock);
            ret = ready ? 0 : -EAGAIN;
        }
    } else {
        ret = -EINVAL;
    }

    spin_lock(&fb_state.lock);
    if (ret == 0)
        fb_state.stats.kms_atomic_in_fence_accepted++;
    else
        fb_state.stats.kms_atomic_in_fence_rejected++;
    spin_unlock(&fb_state.lock);
    return ret;
}

static int gpu_kms_validate_test_only_in_fence_file(
    struct fb_gpu_render_owner *owner, struct vfs_file *file)
{
    int ret = 0;

    (void)owner;
    if (file == NULL) {
        ret = -EBADF;
        goto out_note;
    }
    ret = gpu_kms_in_fence_file_kind_ok(file);

out_note:
    spin_lock(&fb_state.lock);
    if (ret == 0) {
        fb_state.stats.kms_atomic_in_fence_accepted++;
        fb_state.stats.kms_atomic_in_fence_test_only_validated++;
    } else {
        fb_state.stats.kms_atomic_in_fence_rejected++;
    }
    spin_unlock(&fb_state.lock);
    return ret;
}

struct gpu_kms_prepared_out_fence {
    int fd;
    struct fb_gpu_fence *fence_obj;
    uint64 target_sequence;
    int display_correlated;
};

static void gpu_kms_init_prepared_out_fence(
    struct gpu_kms_prepared_out_fence *out)
{
    if (out == NULL)
        return;
    out->fd = -1;
    out->fence_obj = NULL;
    out->target_sequence = 0;
    out->display_correlated = 0;
}

static int gpu_kms_export_out_fence_fd(
    struct gpu_kms_prepared_out_fence *out,
    int display_correlated)
{
    struct fb_gpu_fence_file *fence_file;
    struct fb_gpu_fence *fence_obj;
    int fd;
    uint64 seqno;

    if (out == NULL)
        return -EINVAL;
    gpu_kms_init_prepared_out_fence(out);

    spin_lock(&fb_state.lock);
    seqno = fb_state.stats.display_last_complete != 0 ?
        fb_state.stats.display_last_complete : 1;
    if (display_correlated)
        seqno++;
    spin_unlock(&fb_state.lock);

    fence_obj = fb_gpu_fence_create(0, seqno,
                                    display_correlated ? 0 : 1, 0);
    if (fence_obj == NULL)
        return -ENOMEM;

    fence_file = kvmalloc(sizeof(*fence_file));
    if (fence_file == NULL) {
        fb_gpu_fence_put(fence_obj);
        return -ENOMEM;
    }
    memset(fence_file, 0, sizeof(*fence_file));
    fence_file->fence = seqno;
    fence_file->fence_obj = fence_obj;

    fd = vfs_custom_fd_alloc(&fb_fence_file_ops, fence_file, 0);
    if (fd < 0) {
        fb_gpu_fence_put(fence_obj);
        kvfree(fence_file);
        return fd;
    }

    spin_lock(&fb_state.lock);
    fb_gpu_fence_get_locked(fence_obj);
    fb_state.stats.fence_fd_exports++;
    spin_unlock(&fb_state.lock);
    out->fd = fd;
    out->fence_obj = fence_obj;
    out->target_sequence = seqno;
    out->display_correlated = display_correlated != 0;
    return fd;
}

static void gpu_kms_close_exported_fd(int fd)
{
    fb_gpu_close_exported_fd(fd);
}

static void gpu_kms_cleanup_prepared_out_fence_fd(int *fdp)
{
    if (fdp == NULL || *fdp < 0)
        return;
    gpu_kms_close_exported_fd(*fdp);
    *fdp = -1;
    spin_lock(&fb_state.lock);
    fb_state.stats.kms_atomic_out_fence_cleanup_closes++;
    spin_unlock(&fb_state.lock);
}

static void gpu_kms_release_prepared_out_fence(
    struct gpu_kms_prepared_out_fence *out)
{
    struct fb_gpu_fence *fence_obj;

    if (out == NULL || out->fence_obj == NULL)
        return;
    fence_obj = out->fence_obj;
    out->fence_obj = NULL;
    fb_gpu_fence_put(fence_obj);
}

static void gpu_kms_cleanup_prepared_out_fence(
    struct gpu_kms_prepared_out_fence *out)
{
    if (out == NULL)
        return;
    gpu_kms_cleanup_prepared_out_fence_fd(&out->fd);
    gpu_kms_release_prepared_out_fence(out);
}

static void gpu_kms_complete_prepared_out_fence(
    struct gpu_kms_prepared_out_fence *out)
{
    if (out == NULL || out->fence_obj == NULL)
        return;

    spin_lock(&fb_state.lock);
    if (out->display_correlated) {
        out->fence_obj->seqno = fb_state.stats.display_last_complete != 0 ?
            fb_state.stats.display_last_complete : out->target_sequence;
        fb_state.stats.
            kms_atomic_out_fence_software_scanout_correlated++;
    }
    (void)fb_gpu_fence_signal_locked(out->fence_obj, 0);
    spin_unlock(&fb_state.lock);
    gpu_kms_release_prepared_out_fence(out);
}

static void gpu_kms_unpin_bo_locked(uint32 handle, uint64 owner_id,
                                    pid_t owner_tgid)
{
    struct fb_gpu_bo_entry *bo = fb_bo_lookup_locked(handle);

    if (bo == NULL || !fb_bo_owner_matches(bo, owner_id, owner_tgid) ||
        bo->ttm_pin_count == 0)
        return;
    if (fb_ttm_unpin_locked(bo, owner_id, owner_tgid) == 0)
        fb_ttm_resv_record_shared_locked(bo, FB_GPU_RESV_ATTACH_KMS_UNPIN,
                                         FB_GPU_DMABUF_TAG_NONE);
}

static uint32 gpu_kms_destroy_owner_fbs(struct fb_gpu_render_owner *owner)
{
    uint32 stale = 0;

    if (owner == NULL)
        return 0;

    spin_lock(&fb_state.lock);
    for (uint32 i = 0; i < FB_GPU_MAX_KMS_FBS; i++) {
        struct fb_gpu_kms_fb_entry *fb = &fb_state.kms_fbs[i];

        if (!gpu_kms_fb_owner_matches(fb, owner))
            continue;
        if (fb_state.current_kms_fb_id == fb->fb_id)
            fb_state.current_kms_fb_id = 0;
        gpu_kms_unpin_bo_locked(fb->bo_handle, owner->id, owner->tgid);
        memset(fb, 0, sizeof(*fb));
        if (fb_state.stats.kms_framebuffers > 0)
            fb_state.stats.kms_framebuffers--;
        stale++;
    }
    spin_unlock(&fb_state.lock);
    return stale;
