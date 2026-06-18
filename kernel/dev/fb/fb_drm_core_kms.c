
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

static void gpu_drm_fill_mode_size(struct drm_mode_modeinfo_compat *mode,
                                   uint32 w, uint32 h,
                                   uint32 refresh_millihz, int preferred)
{
    uint32 pos = 0;
    uint32 hblank, vblank;
    uint32 refresh_hz;

    memset(mode, 0, sizeof(*mode));

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
    refresh_hz = refresh_millihz != 0 ?
        (refresh_millihz + 500) / 1000 : 60;
    if (refresh_hz == 0)
        refresh_hz = 60;
    mode->vrefresh = refresh_hz;
    mode->clock = (uint32)(((uint64)mode->htotal * mode->vtotal *
                            (refresh_millihz != 0 ?
                             refresh_millihz : 60000)) / 1000000);
    if (mode->clock == 0)
        mode->clock = 40000;
    mode->flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
    mode->type = DRM_MODE_TYPE_DRIVER |
        (preferred ? DRM_MODE_TYPE_PREFERRED : 0);

    gpu_drm_mode_append_uint(mode->name, &pos, w);
    if (pos < 31)
        mode->name[pos++] = 'x';
    gpu_drm_mode_append_uint(mode->name, &pos, h);
    mode->name[pos < 32 ? pos : 31] = 0;
}

static void gpu_drm_fill_mode(struct drm_mode_modeinfo_compat *mode)
{
    uint32 w, h;

    gpu_drm_get_mode_size(&w, &h);
    gpu_drm_fill_mode_size(mode, w, h, 60000, 1);
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
                                      const struct drm_mode_modeinfo_compat *modes,
                                      uint32 count)
{
    uint32 n;

    if (ptr == 0 || capacity == 0 || modes == NULL || count == 0)
        return 0;
    n = capacity < count ? capacity : count;
    if (either_copyout(1, ptr, (void *)modes,
                       (uint64)n * sizeof(modes[0])) < 0)
        return -EFAULT;
    return 0;
}

static void gpu_drm_copy_prop_name(char dst[32], const char *src)
{
    memset(dst, 0, 32);
    if (src != NULL)
        memmove(dst, src, strlen(src) < 32 ? strlen(src) : 31);
}

struct gpu_drm_in_formats_blob {
    struct drm_format_modifier_blob_compat header;
    uint32 formats[4];
    struct drm_format_modifier_compat modifiers[1];
};

static const uint32 gpu_kms_primary_scanout_formats[] = {
    DRM_FORMAT_XRGB8888,
    DRM_FORMAT_ARGB8888,
    DRM_FORMAT_XBGR8888,
    DRM_FORMAT_ABGR8888,
};

static const uint32 gpu_kms_cursor_formats[] = {
    DRM_FORMAT_ARGB8888,
};

static void gpu_drm_fill_in_formats_blob(struct gpu_drm_in_formats_blob *blob)
{
    memset(blob, 0, sizeof(*blob));
    blob->header.version = 1;
    blob->header.count_formats =
        sizeof(gpu_kms_primary_scanout_formats) /
        sizeof(gpu_kms_primary_scanout_formats[0]);
    blob->header.formats_offset =
        (uint32)((char *)&blob->formats[0] - (char *)blob);
    blob->header.count_modifiers = 1;
    blob->header.modifiers_offset =
        (uint32)((char *)&blob->modifiers[0] - (char *)blob);
    memmove(blob->formats, gpu_kms_primary_scanout_formats,
            sizeof(gpu_kms_primary_scanout_formats));
    blob->modifiers[0].formats =
        (1ULL << blob->header.count_formats) - 1;
    blob->modifiers[0].offset = 0;
    blob->modifiers[0].modifier = DRM_FORMAT_MOD_LINEAR;
}

static int gpu_kms_primary_scanout_format_supported(uint32 pixel_format,
                                                    uint64 modifier)
{
    if (modifier != DRM_FORMAT_MOD_LINEAR)
        return 0;
    for (uint32 i = 0;
         i < sizeof(gpu_kms_primary_scanout_formats) /
                 sizeof(gpu_kms_primary_scanout_formats[0]);
         i++) {
        if (gpu_kms_primary_scanout_formats[i] == pixel_format)
            return 1;
    }
    return 0;
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
        values[0] = DRM_MODE_OBJECT_CRTC;
        *count_values = 1;
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
        enums[1].value = DRM_PLANE_TYPE_CURSOR;
        gpu_drm_copy_prop_name(enums[1].name, "Cursor");
        *count_enums = 2;
        break;
    case GPU_DRM_PROP_FB_ID:
        *name = "FB_ID";
        *flags = DRM_MODE_PROP_OBJECT | DRM_MODE_PROP_ATOMIC;
        values[0] = DRM_MODE_OBJECT_FB;
        *count_values = 1;
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
    case GPU_DRM_PROP_IN_FORMATS:
        *name = "IN_FORMATS";
        *flags = DRM_MODE_PROP_BLOB | DRM_MODE_PROP_IMMUTABLE;
        break;
    case GPU_DRM_PROP_HOTSPOT_X:
        *name = "HOTSPOT_X";
        *flags = DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC;
        values[1] = FB_GPU_CURSOR_MAX_DIM - 1;
        *count_values = 2;
        break;
    case GPU_DRM_PROP_HOTSPOT_Y:
        *name = "HOTSPOT_Y";
        *flags = DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC;
        values[1] = FB_GPU_CURSOR_MAX_DIM - 1;
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
            info->flags |= FB_GPU_BACKEND_F_D3DKMT |
                           FB_GPU_BACKEND_F_GPU_COMPUTE;
        info->dxg_global_status = dxg.global_open_status;
        info->dxg_vgpu_status = dxg.vgpu_open_status;
        info->dxg_global_rx = dxg.global_rx_packets;
        info->dxg_vgpu_rx = dxg.vgpu_rx_packets;
        gpu_backend_copy_string(info->name, sizeof(info->name),
                                "hyperv-dxg");
        gpu_backend_copy_string(info->renderer, sizeof(info->renderer),
                                info->dxg_d3dkmt ?
                                "Hyper-V GPU-PV D3DKMT/DXCore; GPU-compute ready, OpenGL-submit gated" :
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
    uint64 target;
    int ret;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    target = req.request.sequence + 1;
    ret = gpu_kms_wait_vblank_sequence(target, &sequence, &timestamp_ns);
    if (ret != 0)
        return ret;
    req.reply.sequence = (uint32)sequence;
    req.reply.tval_sec = (int64)(timestamp_ns / 1000000000ULL);
    req.reply.tval_usec =
        (int64)((timestamp_ns % 1000000000ULL) / 1000ULL);
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_kms_wait_vblank_sequence(uint64 min_sequence,
                                        uint64 *sequence_out,
                                        uint64 *timestamp_ns_out)
{
    uint64 sequence;
    uint64 timestamp_ns;

    for (int i = 0; i < 1000; i++) {
        spin_lock(&fb_state.lock);
        gpu_kms_sample_vblank_locked(min_sequence, &sequence, &timestamp_ns);
        spin_unlock(&fb_state.lock);
        if (min_sequence == 0 || sequence >= min_sequence) {
            if (sequence_out != NULL)
                *sequence_out = sequence;
            if (timestamp_ns_out != NULL)
                *timestamp_ns_out = timestamp_ns;
            return 0;
        }
        sleep_ms(1);
    }
    return -ETIMEDOUT;
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
        timestamp_ns = gpu_kms_monotonic_ns();
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
    uint64 display_seq = fb_state.stats.display_last_complete;
    uint64 seq = fb_state.stats.kms_vblank_sequence;
    uint64 timestamp_ns = fb_state.stats.kms_vblank_timestamp_ns;

    if (display_seq != 0 && display_seq > seq) {
        fb_state.stats.kms_vblank_sequence = display_seq;
        seq = display_seq;
        timestamp_ns = fb_state.stats.kms_vblank_timestamp_ns;
        fb_state.stats.kms_vblank_synthetic = 0;
        fb_state.stats.kms_vblank_display_correlated = 1;
        fb_state.stats.kms_vblank_source_synthetic = 0;
        fb_state.stats.kms_vblank_source_software_display = 1;
        fb_state.stats.kms_vblank_source_nouveau_hw = 0;
    }
    fb_state.stats.kms_vblank_samples++;
    if (sequence_out != NULL)
        *sequence_out = seq;
    if (timestamp_ns_out != NULL)
        *timestamp_ns_out = timestamp_ns;
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

static int gpu_drm_crtc_queue_sequence(struct fb_gpu_render_owner *owner,
                                       uint64 arg)
{
    struct drm_crtc_queue_sequence_compat req;
    uint64 current_sequence;
    uint64 target_sequence;
    uint64 timestamp_ns;
    int ret;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.crtc_id != GPU_DRM_CRTC_ID)
        return -EINVAL;
    spin_lock(&fb_state.lock);
    if ((req.flags & ~DRM_CRTC_SEQUENCE_FLAGS) != 0) {
        fb_state.stats.kms_crtc_queue_sequence_bad_flags++;
        spin_unlock(&fb_state.lock);
        return -EINVAL;
    }
    gpu_kms_sample_vblank_locked(0, &current_sequence, &timestamp_ns);
    spin_unlock(&fb_state.lock);

    target_sequence = req.sequence;
    if ((req.flags & DRM_CRTC_SEQUENCE_RELATIVE) != 0)
        target_sequence = current_sequence + req.sequence;
    if (target_sequence <= current_sequence &&
        (req.flags & DRM_CRTC_SEQUENCE_NEXT_ON_MISS) != 0)
        target_sequence = current_sequence + 1;

    ret = gpu_kms_wait_vblank_sequence(target_sequence, &current_sequence,
                                       &timestamp_ns);
    if (ret != 0) {
        spin_lock(&fb_state.lock);
        fb_state.stats.kms_crtc_queue_sequence_rejects++;
        spin_unlock(&fb_state.lock);
        return ret;
    }

    req.sequence = current_sequence;
    spin_lock(&fb_state.lock);
    ret = gpu_drm_event_queue_locked(owner, DRM_EVENT_VBLANK, req.user_data,
                                     current_sequence, GPU_DRM_CRTC_ID,
                                     timestamp_ns);
    spin_unlock(&fb_state.lock);
    if (ret != 0)
        return ret;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
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

#include "fb_drm_kms_objects.c"
#include "fb_drm_kms_properties.c"
#include "fb_drm_kms_atomic_props.c"
