
int snprintf(char *buf, size_t size, const char *fmt, ...);

static int chrome_drm_ioctl_trace_enabled(void);
static int chrome_drm_trace_owner(const struct fb_gpu_render_owner *owner);

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

static void gpu_virtio_pci_unique(char unique[32])
{
    struct virtio_pci_discovery *vd = pci_get_virtio_gpu(0);

    if (vd != NULL && vd->found) {
        snprintf(unique, 32, "pci:0000:%02x:%02x.%u", vd->bus, vd->dev,
                 vd->func & 0x7);
        return;
    }
    snprintf(unique, 32, "pci:0000:00:04.0");
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

static void gpu_drm_trace_safe_name(const char *src, char dst[64])
{
    uint32 i;

    if (dst == NULL)
        return;
    if (src == NULL || src[0] == 0) {
        memcpy(dst, "?", sizeof("?"));
        return;
    }

    for (i = 0; i < 63 && src[i] != 0; i++) {
        char c = src[i];

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' ||
            c == '.' || c == '/' || c == ':' || c == '+')
            dst[i] = c;
        else
            dst[i] = '_';
    }
    if (i == 0)
        dst[i++] = '?';
    dst[i] = 0;
}

static void gpu_drm_trace_context_attrib(struct fb_gpu_render_owner *owner,
                                         const char *phase,
                                         uint32 ctx_id,
                                         uint32 submit_flags,
                                         uint32 ioctl_flags,
                                         uint32 words,
                                         uint32 resource_count,
                                         uint32 first_dw,
                                         uint64 fence,
                                         uint64 signaled,
                                         int ret)
{
    char debug_name[64];
    char proc_name[64];

    if (!chrome_drm_trace_owner(owner))
        return;

    gpu_drm_trace_safe_name(owner->debug_name, debug_name);
    gpu_drm_trace_safe_name(current ? current->name : NULL, proc_name);
    printf("chrome-drm-detail: virtgpu-context-%s pid=%d tgid=%d "
           "proc=%s owner=%lu:%d node=%s ctx=%u capset=%u "
           "context_init=0x%x num_rings=%u ring_idx_mask=0x%lx "
           "explicit_debug_name=%d debug_name=%s submit_flags=0x%x "
           "ioctl_flags=0x%x words=%u bo_count=%u first_dw=0x%x "
           "fence=%lu signaled=%lu ret=%d\n",
           phase, current ? current->pid : -1,
           current ? current->tgid : -1, proc_name,
           owner ? owner->id : 0, owner ? owner->tgid : -1,
           owner ? drm_core_node_name(owner->drm.node_type) : "?",
           ctx_id, owner ? owner->capset_id : 0,
           owner ? owner->context_init : 0,
           owner ? owner->num_rings : 0,
           owner ? owner->ring_idx_mask : 0,
           owner ? owner->explicit_debug_name : 0, debug_name,
           submit_flags, ioctl_flags, words, resource_count, first_dw,
           fence, signaled, ret);
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
    case GPU_DRM_PROP_DPMS:
        *name = "DPMS";
        *flags = DRM_MODE_PROP_ENUM;
        enums[0].value = DRM_MODE_DPMS_ON;
        gpu_drm_copy_prop_name(enums[0].name, "On");
        enums[1].value = DRM_MODE_DPMS_STANDBY;
        gpu_drm_copy_prop_name(enums[1].name, "Standby");
        enums[2].value = DRM_MODE_DPMS_SUSPEND;
        gpu_drm_copy_prop_name(enums[2].name, "Suspend");
        enums[3].value = DRM_MODE_DPMS_OFF;
        gpu_drm_copy_prop_name(enums[3].name, "Off");
        *count_enums = 4;
        break;
    default:
        return -ENOENT;
    }
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

static int gpu_owner_context_created(struct fb_gpu_render_owner *owner)
{
    return owner != NULL &&
        (owner->context_created || owner->default_ctx_id != 0);
}

static int gpu_owner_create_context(struct fb_gpu_render_owner *owner,
                                    const char *name, uint32 capset_id,
                                    uint32 context_init, uint32 num_rings,
                                    uint64 ring_idx_mask,
                                    int explicit_debug_name,
                                    int fail_if_exists)
{
    int ret;
    uint32 actual_capset_id = capset_id;

    if (owner == NULL)
        return -EBADF;
    /*
     * There is no narrow sleepable per-owner lock here. fb_state.lock is a
     * short critical-section spinlock and cannot be held while virtio creates
     * the context, so concurrent duplicate context-init ioctls remain a
     * future per-owner mutex cleanup rather than a broad lock in this patch.
     */
    if (gpu_owner_context_created(owner))
        return fail_if_exists ? -EEXIST : 0;
    ret = virtio_gpu_user_context_create(owner->id, owner->tgid, capset_id,
                                         context_init,
                                         name ? name : "xv6-drm",
                                         &owner->default_ctx_id,
                                         &actual_capset_id);
    if (ret == 0) {
        owner->context_created = 1;
        owner->capset_id = actual_capset_id;
        owner->context_init = context_init;
        owner->num_rings = num_rings != 0 ? num_rings : 1;
        owner->ring_idx_mask = ring_idx_mask;
        owner->explicit_debug_name = explicit_debug_name != 0;
        memset(owner->debug_name, 0, sizeof(owner->debug_name));
        if (name != NULL)
            memmove(owner->debug_name, name,
                    strlen(name) < sizeof(owner->debug_name) ?
                    strlen(name) + 1 : sizeof(owner->debug_name));
        else
            memcpy(owner->debug_name, "xv6-drm", sizeof("xv6-drm"));
        owner->debug_name[sizeof(owner->debug_name) - 1] = 0;
        gpu_drm_trace_context_attrib(owner, "create",
                                     owner->default_ctx_id, 0, 0, 0, 0, 0,
                                     0, 0, ret);
    }
    return ret;
}

static int gpu_owner_ensure_context(struct fb_gpu_render_owner *owner)
{
    if (gpu_owner_context_created(owner))
        return 0;
    return gpu_owner_create_context(owner, "xv6-drm", 0, 0, 1, 0, 0, 0);
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
    if (backend.backend == FB_GPU_BACKEND_HYPERV_DXG) {
        if (gpu_copyout_string(req.unique, &req.unique_len,
                               "vmbus:hyperv-dxg") != 0)
            return -EFAULT;
    } else {
        gpu_virtio_pci_unique(pci_unique);
        if (gpu_copyout_string(req.unique, &req.unique_len,
                               pci_unique) != 0)
            return -EFAULT;
    }
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_get_cap(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_get_cap_compat req;
    uint64 capability;
    int ret = 0;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    capability = req.capability;
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
        ret = -EINVAL;
        break;
    }
    if (chrome_drm_trace_owner(owner))
        printf("chrome-drm-detail: get-cap owner=%lu:%d cap=%lu "
               "value=%lu ret=%d\n",
               owner ? owner->id : 0, owner ? owner->tgid : -1,
               capability, req.value, ret);
    if (ret != 0)
        return ret;
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

#define DRM_VBLANK_RELATIVE_COMPAT       0x00000001U
#define DRM_VBLANK_HIGH_CRTC_MASK_COMPAT 0x0000003eU
#define DRM_VBLANK_EVENT_COMPAT          0x04000000U
#define DRM_VBLANK_SIGNAL_COMPAT         0x40000000U
#define DRM_VBLANK_SECONDARY_COMPAT      0x20000000U
#define DRM_VBLANK_NEXTONMISS_COMPAT     0x10000000U

static int gpu_drm_event_queue_locked(struct fb_gpu_render_owner *owner,
                                      uint32 type, uint64 user_data,
                                      uint64 sequence, uint32 crtc_id,
                                      uint64 timestamp_ns);
static void gpu_drm_event_notify_read(struct fb_gpu_render_owner *owner);

static uint64 gpu_kms_vblank_period_ns(void)
{
    return 16666667ULL; /* 60 Hz modes advertised by KMS */
}

static uint64 gpu_kms_predict_vblank_timestamp(uint64 current_sequence,
                                               uint64 current_timestamp_ns,
                                               uint64 target_sequence)
{
    uint64 delta;

    if (current_timestamp_ns == 0 || target_sequence <= current_sequence)
        return current_timestamp_ns;
    delta = target_sequence - current_sequence;
    return current_timestamp_ns + delta * gpu_kms_vblank_period_ns();
}

static int gpu_drm_wait_vblank(struct fb_gpu_render_owner *owner, uint64 arg)
{
    union drm_wait_vblank_compat req;
    uint32 type;
    uint64 current_sequence;
    uint64 current_timestamp_ns;
    uint64 sequence;
    uint64 timestamp_ns;
    uint64 target;
    int ret;

    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    type = req.request.type;
    if ((type & (DRM_VBLANK_SIGNAL_COMPAT | DRM_VBLANK_SECONDARY_COMPAT |
                 DRM_VBLANK_HIGH_CRTC_MASK_COMPAT)) != 0)
        return -EINVAL;

    spin_lock(&fb_state.lock);
    gpu_kms_sample_vblank_locked(0, &current_sequence, &current_timestamp_ns);
    spin_unlock(&fb_state.lock);

    if ((type & DRM_VBLANK_RELATIVE_COMPAT) != 0)
        target = current_sequence + req.request.sequence;
    else
        target = req.request.sequence;
    if (target <= current_sequence) {
        if ((type & DRM_VBLANK_NEXTONMISS_COMPAT) != 0)
            target = current_sequence + 1;
        else
            target = current_sequence;
    }
    timestamp_ns = gpu_kms_predict_vblank_timestamp(current_sequence,
                                                    current_timestamp_ns,
                                                    target);

    if ((type & DRM_VBLANK_EVENT_COMPAT) != 0) {
        spin_lock(&fb_state.lock);
        ret = gpu_drm_event_queue_locked(owner, DRM_EVENT_VBLANK,
                                         req.request.signal, target,
                                         GPU_DRM_CRTC_ID, timestamp_ns);
        spin_unlock(&fb_state.lock);
        if (ret != 0)
            return ret;
        gpu_drm_event_notify_read(owner);
        req.reply.sequence = (uint32)target;
        req.reply.tval_sec = (int64)(timestamp_ns / 1000000000ULL);
        req.reply.tval_usec =
            (int64)((timestamp_ns % 1000000000ULL) / 1000ULL);
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }

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

static void gpu_drm_event_notify_file(struct vfs_file *file)
{
    if (file == NULL)
        return;
    vfs_file_knote_notify(file, EVFILT_READ, 0);
    vfs_fput(file);
}

static void gpu_drm_event_notify_read(struct fb_gpu_render_owner *owner)
{
    struct vfs_file *file = owner ? vfs_fdup(owner->drm_event_file) : NULL;

    gpu_drm_event_notify_file(file);
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

/*
 * ── Async-present completion-event reservation (gate virtio_gpu_async_present)
 *
 * SHOULD-FIX #3: the accept-time ring check does NOT reserve, so between accept
 * and the worker's completion delivery a burst of concurrent VBLANK /
 * QUEUE_SEQUENCE events could fill the owner's 16-deep ring and force the
 * flip-complete to be dropped -- which freezes kwin (it blocks forever on an
 * event that never arrives). Fix: reserve one ring slot per in-flight async
 * completion at accept time and keep it free until that completion is enqueued.
 *
 * MODEL: a single global reservation record holds a COUNT of ring slots owed to
 * pending async completions for one owner. This suffices because the async path
 * keeps a single present in flight per the sole CRTC and kwin is the one
 * compositor owner that flips; only in the paced-composition mode (where the
 * in-flight slot is cleared before the deferred event fires) can more than one
 * completion be owed at once, and those are all for the same owner. An accept
 * by a *second* owner while the first still holds a reservation simply proceeds
 * UNRESERVED (acquire returns 0) -- degrading to pre-fix behavior for that rare
 * case rather than corrupting the count. The count also cannot be a per-owner
 * field here: the owner struct lives outside this slice's editable files.
 *
 * gpu_drm_event_queue_locked() honors the reservation as headroom: any enqueue
 * that is NOT itself a DRM_EVENT_FLIP_COMPLETE (VBLANK / QUEUE_SEQUENCE) is
 * rejected with -EAGAIN once (used + reserved) would exceed capacity, so the
 * reserved slots stay free for the completions. A FLIP_COMPLETE always uses the
 * full ring -- that is the ONLY completion class produced for the flipping
 * owner while the async gate is on (legacy flips all go async; the sync/atomic/
 * non-async-paced FLIP_COMPLETE producers are not reached for that owner), so
 * it consumes a genuinely-reserved slot rather than stealing one.
 *
 * LIFETIME: the reservation is released by whoever finally disposes of the
 * completion -- the worker (non-paced), the paced timer callback, or the
 * immediate-deliver fallback -- so it is freed on EVERY path, including a
 * mid-present fd close (the worker/timer always run; sched_timer timers are not
 * cancelable). Because it is not stored in the owner, owner teardown needs no
 * special handling. Guarded by fb_state.lock.
 */
static struct {
    int active;
    uint64 owner_id;
    pid_t owner_tgid;
    uint32 count;
} gpu_drm_async_present_resv;

/* Reserve one ring slot for a pending async completion. Returns 1 if reserved
 * (caller must arrange exactly one matching release), 0 if skipped because a
 * different owner already holds the reservation. Caller holds fb_state.lock. */
static int gpu_drm_async_present_reserve_locked(uint64 owner_id,
                                                pid_t owner_tgid)
{
    if (gpu_drm_async_present_resv.active &&
        (gpu_drm_async_present_resv.owner_id != owner_id ||
         gpu_drm_async_present_resv.owner_tgid != owner_tgid))
        return 0;
    gpu_drm_async_present_resv.active = 1;
    gpu_drm_async_present_resv.owner_id = owner_id;
    gpu_drm_async_present_resv.owner_tgid = owner_tgid;
    gpu_drm_async_present_resv.count++;
    return 1;
}

/* Release one previously-acquired reservation. Idempotent past zero (a stray
 * double-release is a no-op). Caller holds fb_state.lock. */
static void gpu_drm_async_present_unreserve_locked(void)
{
    if (gpu_drm_async_present_resv.count > 0 &&
        --gpu_drm_async_present_resv.count == 0)
        gpu_drm_async_present_resv.active = 0;
}

/* Ring slots currently reserved for pending async completions on this owner (0
 * unless an async present is in flight for it). Caller holds fb_state.lock. */
static uint32 gpu_drm_async_present_reserved_for_locked(
    const struct fb_gpu_render_owner *owner)
{
    if (owner != NULL && gpu_drm_async_present_resv.active &&
        gpu_drm_async_present_resv.owner_id == owner->id &&
        gpu_drm_async_present_resv.owner_tgid == owner->tgid)
        return gpu_drm_async_present_resv.count;
    return 0;
}

static int gpu_drm_event_queue_locked(struct fb_gpu_render_owner *owner,
                                      uint32 type, uint64 user_data,
                                      uint64 sequence, uint32 crtc_id,
                                      uint64 timestamp_ns)
{
    struct drm_event_vblank_compat *ev;
    uint32 reserved;
    uint32 effective_capacity;

    if (owner == NULL)
        return -EBADF;
    /*
     * Keep the ring slot(s) held for pending async flip-completions free for
     * every enqueue except a FLIP_COMPLETE (the reserved event class), so a
     * burst of VBLANK / QUEUE_SEQUENCE events can never displace a completion
     * kwin is blocking on (SHOULD-FIX #3).
     */
    reserved = (type != DRM_EVENT_FLIP_COMPLETE)
                   ? gpu_drm_async_present_reserved_for_locked(owner)
                   : 0;
    effective_capacity = reserved < FB_GPU_DRM_EVENT_QUEUE_CAPACITY
                             ? FB_GPU_DRM_EVENT_QUEUE_CAPACITY - reserved
                             : 0;
    if (owner->drm_event_count >= effective_capacity) {
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

static int gpu_kms_synthetic_vblank_enabled(void)
{
    static int initialized;
    static int enabled = 1;
    char value[8];

    if (!initialized) {
        if (cmdline_get_param("kms_synthetic_vblank", value,
                              sizeof(value)) == 0)
            enabled = value[0] != '0' && value[0] != 'n' &&
                      value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static void gpu_kms_sample_vblank_locked(uint64 min_sequence,
                                         uint64 *sequence_out,
                                         uint64 *timestamp_ns_out)
{
    uint64 display_seq = fb_state.stats.display_last_complete;
    uint64 seq = fb_state.stats.kms_vblank_sequence;
    uint64 timestamp_ns = fb_state.stats.kms_vblank_timestamp_ns;
    uint64 now_ns = gpu_kms_monotonic_ns();
    const uint64 period_ns = 16666667ULL; /* 60 Hz modes advertised by KMS */

    (void)min_sequence;

    if (!gpu_kms_synthetic_vblank_enabled()) {
        (void)min_sequence;
    } else if (seq == 0) {
        seq = 1;
        timestamp_ns = now_ns;
        fb_state.stats.kms_vblank_sequence = seq;
        fb_state.stats.kms_vblank_timestamp_ns = timestamp_ns;
        fb_state.stats.kms_vblank_last_tick = r_time();
        fb_state.stats.kms_vblank_synthetic = 1;
        fb_state.stats.kms_vblank_display_correlated = 0;
        fb_state.stats.kms_vblank_source_synthetic = 1;
        fb_state.stats.kms_vblank_source_software_display = 0;
        fb_state.stats.kms_vblank_source_nouveau_hw = 0;
    } else if (timestamp_ns != 0 && now_ns > timestamp_ns) {
        uint64 elapsed = (now_ns - timestamp_ns) / period_ns;

        if (elapsed != 0) {
            seq += elapsed;
            timestamp_ns += elapsed * period_ns;
            fb_state.stats.kms_vblank_sequence = seq;
            fb_state.stats.kms_vblank_timestamp_ns = timestamp_ns;
            fb_state.stats.kms_vblank_last_tick = r_time();
            fb_state.stats.kms_vblank_synthetic = 1;
            fb_state.stats.kms_vblank_display_correlated = 0;
            fb_state.stats.kms_vblank_source_synthetic = 1;
            fb_state.stats.kms_vblank_source_software_display = 0;
            fb_state.stats.kms_vblank_source_nouveau_hw = 0;
        }
    }

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

/*
 * Vblank-paced legacy page-flip completion (gate:
 * virtio_gpu_vblank_paced_flip, DEFAULT OFF).
 *
 * kwin 5.27's legacy (non-atomic) RenderLoop paces itself off the
 * DRM_MODE_PAGE_FLIP_EVENT completion. When that event is delivered
 * synchronously from inside the page-flip ioctl its timestamp is grid-snapped
 * and up to a refresh interval stale, so the RenderLoop has no real phase and
 * free-runs. With the gate on, the flip's present work still happens in the
 * ioctl exactly as before -- only the completion-event queue+notify is deferred
 * to the next synthetic vblank edge, giving kwin a phase-accurate clock anchor.
 *
 * KNOWN TRADEOFF: pacing flip-complete to the vblank edge caps flip-event
 * cadence at the synthetic refresh rate (~60 Hz). The M6 mesakmsgl direct-KMS
 * benchmark (118-125 FPS unthrottled) will read ~60 under the gate -- that is
 * the expected vsync semantic, not a regression; interpret the M6 A/B run
 * accordingly.
 */
static int gpu_kms_vblank_paced_flip_enabled(void)
{
    static int initialized;
    static int enabled; /* DEFAULT OFF: synchronous flip-complete delivery */
    char value[8];

    if (!initialized) {
        if (cmdline_get_param("virtio_gpu_vblank_paced_flip", value,
                              sizeof(value)) == 0)
            enabled = value[0] != '0' && value[0] != 'n' &&
                      value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

/*
 * Deferred flip-complete descriptor. Deliberately holds the owner *id* (which
 * is allocated monotonically and never reused, see gpu_alloc_render_owner_id)
 * rather than a raw owner pointer: sched_timer timers are not cancelable, so
 * the callback may still run after the drm file closes and kvfree()s its owner.
 * The callback re-resolves the owner by id under fb_state.lock and drops the
 * event if the file has since gone away, so there is no use-after-free.
 */
struct gpu_drm_paced_flip {
    uint64 owner_id;
    pid_t owner_tgid;
    uint64 user_data;
    uint64 sequence;
    uint64 timestamp_ns;
    uint32 crtc_id;
    /* Set only for an async-present completion (gate virtio_gpu_async_present)
     * that carries a ring-slot reservation; the timer callback releases it. */
    int owns_async_reservation;
};

/*
 * Timer callback for a deferred flip-complete event.
 *
 * CONTEXT: sched_timer_add() fires this from workqueue (kthread) context (the
 * timer soft-IRQ only queue_work()s onto __sched_timer_wq, see
 * timer/sched_timer.c __timer_callback -> __work_callback), so taking the
 * fb_state.lock spinlock here is safe -- it is never a sleeping lock and no
 * hard-IRQ handler takes it.
 *
 * LIFETIME: the owner is re-resolved by id under fb_state.lock. While the owner
 * is registered and we hold that lock it cannot be freed, because
 * gpu_fops_release() clears drm_event_file and unregisters (and only then
 * kvfree()s) the owner under the very same lock. We fdup() the event file while
 * still holding the lock so the notify below keeps the file alive independently
 * of the owner.
 *
 * DROP OBSERVABILITY: once the flip ioctl has returned 0, a silently dropped
 * completion event freezes the client's render loop with nothing to diagnose,
 * so an undelivered event on a still-open file bumps kms_paced_dropped_total
 * (an owner that already closed is a benign drop and is not counted). One
 * residual silent path remains outside this file: sched_timer's
 * __timer_callback drops the work item when queue_work() on __sched_timer_wq
 * fails (timer/sched_timer.c:79-84). That workqueue is created at boot and
 * never destroyed, so the path is unreachable today, but it is undefended;
 * hardening it belongs to sched_timer core, not this slice.
 */
static void gpu_drm_paced_flip_timer_cb(void *arg)
{
    struct gpu_drm_paced_flip *pf = arg;
    struct fb_gpu_render_owner *owner;
    struct vfs_file *file = NULL;

    if (pf == NULL)
        return;

    spin_lock(&fb_state.lock);
    owner = fb_gpu_render_owner_lookup_locked(pf->owner_id, pf->owner_tgid);
    if (owner != NULL && owner->drm_event_file != NULL) {
        int ret = gpu_drm_event_queue_locked(owner, DRM_EVENT_FLIP_COMPLETE,
                                             pf->user_data, pf->sequence,
                                             pf->crtc_id, pf->timestamp_ns);
        if (ret == 0)
            file = vfs_fdup(owner->drm_event_file);
        else
            fb_state.stats.kms_paced_dropped_total++; /* ring full (-EAGAIN) */
    }
    /* Release the async-present ring-slot reservation this deferred completion
     * carried (this is its sole terminal point, so it frees on every branch --
     * owner-gone drop and ring-full drop included). */
    if (pf->owns_async_reservation)
        gpu_drm_async_present_unreserve_locked();
    spin_unlock(&fb_state.lock);

    if (file != NULL)
        gpu_drm_event_notify_file(file);
    kvfree(pf);
}

/*
 * Arm a deferred flip-complete event at the next synthetic vblank edge.
 *
 * Called from the page-flip ioctl (process context, holding a reference to the
 * drm file) after the present work has already completed. Returns 0 when the
 * event has been handled (either armed for the vblank edge or, on a rare arm
 * failure, delivered synchronously so the event is never dropped), or a
 * negative errno when the caller should fall back to the synchronous delivery
 * path (only on allocation failure, before any accounting is touched).
 */
static int gpu_drm_page_flip_paced(struct fb_gpu_render_owner *owner,
                                   uint64 user_data, uint32 fb_id)
{
    struct gpu_drm_paced_flip *pf;
    uint64 sequence;
    uint64 timestamp_ns;
    uint64 next_seq;
    uint64 next_ts;
    uint64 now_ns;
    uint64 delay_ns;
    uint64 delay_ms;
    int ret;

    if (owner == NULL)
        return -EBADF;
    pf = kvmalloc(sizeof(*pf));
    if (pf == NULL)
        return -ENOMEM; /* caller falls back to synchronous delivery */

    spin_lock(&fb_state.lock);
    gpu_kms_sample_vblank_locked(0, &sequence, &timestamp_ns);
    next_seq = sequence + 1;
    next_ts = gpu_kms_predict_vblank_timestamp(sequence, timestamp_ns,
                                               next_seq);
    now_ns = gpu_kms_monotonic_ns();
    delay_ns = next_ts > now_ns ? next_ts - now_ns : 0;

    pf->owner_id = owner->id;
    pf->owner_tgid = owner->tgid;
    pf->user_data = user_data;
    pf->sequence = next_seq;
    pf->timestamp_ns = next_ts;
    pf->crtc_id = GPU_DRM_CRTC_ID;
    pf->owns_async_reservation = 0; /* non-async path holds no reservation */

    /* Present-completion accounting mirrors the synchronous path exactly. */
    fb_state.stats.kms_vblank_page_flip_events++;
    if (fb_state.stats.kms_present_last_lane ==
        FB_GPU_KMS_PRESENT_LANE_NOUVEAU_HW)
        fb_state.stats.kms_page_flip_events_native_hw++;
    else
        fb_state.stats.kms_page_flip_events_software_blit++;
    fb_state.current_kms_fb_id = fb_id;
    fb_state.stats.kms_page_flips++;
    spin_unlock(&fb_state.lock);

    delay_ms = (delay_ns + 999999ULL) / 1000000ULL;
    if (delay_ms == 0)
        delay_ms = 1; /* always cross at least one tick to the edge */

    ret = sched_timer_add(gpu_drm_paced_flip_timer_cb, pf, delay_ms);
    if (ret != 0) {
        /*
         * Could not arm the deferral: deliver the event synchronously right
         * now (never drop it) and free the descriptor. Safe to touch owner
         * directly: we are still inside the ioctl with a file reference held.
         */
        struct vfs_file *file = NULL;

        spin_lock(&fb_state.lock);
        ret = gpu_drm_event_queue_locked(owner, DRM_EVENT_FLIP_COMPLETE,
                                         pf->user_data, pf->sequence,
                                         pf->crtc_id, pf->timestamp_ns);
        if (ret == 0)
            file = vfs_fdup(owner->drm_event_file);
        else
            fb_state.stats.kms_paced_dropped_total++; /* ring full (-EAGAIN) */
        spin_unlock(&fb_state.lock);
        kvfree(pf);
        if (file != NULL)
            gpu_drm_event_notify_file(file);
        return 0;
    }

    spin_lock(&fb_state.lock);
    fb_state.stats.kms_flips_paced_total++;
    fb_state.stats.kms_paced_delay_us_total += delay_ns / 1000ULL;
    spin_unlock(&fb_state.lock);
    return 0;
}

/*
 * Deliver a DRM_EVENT_FLIP_COMPLETE by owner *id* (not pointer).
 *
 * Used by the async-present worker (gate virtio_gpu_async_present) to hand the
 * completion to a drm file that may have closed while the present was in
 * flight. The owner is re-resolved under fb_state.lock exactly as
 * gpu_drm_paced_flip_timer_cb does: while the owner is registered and the lock
 * is held it cannot be freed (gpu_fops_release clears drm_event_file and
 * unregisters under the same lock before kvfree), and the event file is fdup'd
 * under the lock so the notify keeps it alive independently of the owner. If
 * the owner has gone the completion is a benign drop; a full ring is accounted
 * by gpu_drm_event_queue_locked (drm_event_queue_dropped).
 */
static void gpu_drm_deliver_flip_complete_by_id(uint64 owner_id,
                                                pid_t owner_tgid,
                                                uint64 user_data,
                                                uint64 sequence,
                                                uint64 timestamp_ns,
                                                uint32 crtc_id)
{
    struct fb_gpu_render_owner *owner;
    struct vfs_file *file = NULL;

    spin_lock(&fb_state.lock);
    owner = fb_gpu_render_owner_lookup_locked(owner_id, owner_tgid);
    if (owner != NULL && owner->drm_event_file != NULL) {
        if (gpu_drm_event_queue_locked(owner, DRM_EVENT_FLIP_COMPLETE,
                                       user_data, sequence, crtc_id,
                                       timestamp_ns) == 0)
            file = vfs_fdup(owner->drm_event_file);
    }
    spin_unlock(&fb_state.lock);

    if (file != NULL)
        gpu_drm_event_notify_file(file);
}

/*
 * Compose virtio_gpu_async_present with virtio_gpu_vblank_paced_flip: when both
 * gates are on the async worker has already run the *present* to completion and
 * sampled the real completion-time vblank (base_seq/base_ts). Pace only the
 * completion *event* to the next synthetic vblank edge PAST that real
 * completion, so kwin gets a phase-accurate FUTURE anchor whose cost has
 * already been paid. Mirrors gpu_drm_page_flip_paced's arming, minus the
 * present-completion accounting (the worker already did it), and addresses the
 * owner by id so it is safe after a file close. Never drops the event: on a
 * kvmalloc / arm failure it is delivered immediately.
 */
static void gpu_drm_pace_completed_flip_by_id(uint64 owner_id,
                                              pid_t owner_tgid,
                                              uint64 user_data,
                                              uint64 base_seq,
                                              uint64 base_ts,
                                              int reserved)
{
    struct gpu_drm_paced_flip *pf;
    uint64 next_seq = base_seq + 1;
    uint64 next_ts = gpu_kms_predict_vblank_timestamp(base_seq, base_ts,
                                                      next_seq);
    uint64 now_ns = gpu_kms_monotonic_ns();
    uint64 delay_ns = next_ts > now_ns ? next_ts - now_ns : 0;
    uint64 delay_ms;
    int ret;

    pf = kvmalloc(sizeof(*pf));
    if (pf == NULL) {
        /* Immediate delivery is this reservation's terminal point too. */
        gpu_drm_deliver_flip_complete_by_id(owner_id, owner_tgid, user_data,
                                            base_seq, base_ts,
                                            GPU_DRM_CRTC_ID);
        if (reserved) {
            spin_lock(&fb_state.lock);
            gpu_drm_async_present_unreserve_locked();
            spin_unlock(&fb_state.lock);
        }
        return;
    }
    pf->owner_id = owner_id;
    pf->owner_tgid = owner_tgid;
    pf->user_data = user_data;
    pf->sequence = next_seq;
    pf->timestamp_ns = next_ts;
    pf->crtc_id = GPU_DRM_CRTC_ID;
    /* The reservation now rides the timer; gpu_drm_paced_flip_timer_cb frees
     * it. */
    pf->owns_async_reservation = reserved;

    delay_ms = (delay_ns + 999999ULL) / 1000000ULL;
    if (delay_ms == 0)
        delay_ms = 1; /* always cross at least one tick to the edge */

    ret = sched_timer_add(gpu_drm_paced_flip_timer_cb, pf, delay_ms);
    if (ret != 0) {
        kvfree(pf);
        gpu_drm_deliver_flip_complete_by_id(owner_id, owner_tgid, user_data,
                                            base_seq, base_ts,
                                            GPU_DRM_CRTC_ID);
        if (reserved) {
            spin_lock(&fb_state.lock);
            gpu_drm_async_present_unreserve_locked();
            spin_unlock(&fb_state.lock);
        }
        return;
    }

    spin_lock(&fb_state.lock);
    fb_state.stats.kms_flips_paced_total++;
    fb_state.stats.kms_paced_delay_us_total += delay_ns / 1000ULL;
    spin_unlock(&fb_state.lock);
}

/*
 * Free-running phase-locked 60 Hz present clock (gate:
 * virtio_gpu_present_clock_60hz, DEFAULT OFF; composes with — and requires —
 * virtio_gpu_async_present; no effect while async-present is off, so the
 * synchronous flip path stays byte-identical).
 *
 * WHY: kwin 5.27's legacy RenderLoop schedules its next repaint at
 * lastPresentationTimestamp + vblankInterval. On xv6 the flip-complete event
 * carries the *real* present-completion time (the display-correlated vblank —
 * see gpu_kms_sample_vblank_locked's display_last_complete overwrite), which
 * lags the 60 Hz grid by the accumulated per-frame processing (present
 * round-trip ~3 ms + repaint + wakeup). That lag is ADDED to every 16.67 ms
 * interval, so the loop slides to ~19.6 ms/frame = ~51 fps and never reaches
 * 60: kwin has no free-running vsync reference (the recorded M4/M7/U5
 * "kwin has no vsync/present clock" root cause). Neither async-present (removes
 * the flip *ioctl* cost) nor virtio_gpu_vblank_paced_flip (paces the event to
 * the next edge PAST real completion — still chained off real completion) breaks
 * this; both measured M7-null (U2 kill-criterion, U7 A/B presentedFPS 51.2->51.0).
 *
 * FIX: hand kwin a completion timestamp/sequence taken from a wall-clock-anchored
 * 60 Hz grid that is INDEPENDENT of when the present physically completed, and
 * deliver it at that grid edge. The async worker has already run the blit to
 * completion and dropped the R5-critical BO ref BEFORE this is armed
 * (gpu_kms_async_present_worker: blit -> fb_bo_put -> [arm completion]), so the
 * event is never delivered before the host finished reading the buffer — this
 * changes only the event's *clock*, not its ordering vs the blit, and therefore
 * carries NO new tearing/UAF risk over async-present. Grid slides are absorbed
 * as a stable wait instead of accumulating, so the loop grid-locks at 16.67 ms
 * = 60 fps.
 */
static int gpu_kms_present_clock_60hz_enabled(void)
{
    static int initialized;
    static int enabled; /* DEFAULT OFF */
    char value[8];

    if (!initialized) {
        if (cmdline_get_param("virtio_gpu_present_clock_60hz", value,
                              sizeof(value)) == 0)
            enabled = value[0] != '0' && value[0] != 'n' &&
                      value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

/*
 * Compute the next free-running 60 Hz grid point. Returns BASE values
 * (grid_seq-1, grid_ts-period) so gpu_drm_pace_completed_flip_by_id's internal
 * +1 lands the delivered event exactly on the target grid edge (grid_seq,
 * grid_ts), reusing that path's tested timer / reservation / never-drop
 * machinery verbatim. MUST be called under fb_state.lock (guards the grid
 * statics). Timestamps are wall-clock-anchored so the grid self-heals if the
 * worker runs late. A missed edge advances to the latest edge that has already
 * elapsed (sequence gaps still expose dropped frames), but does not wait for
 * another future edge and turn one miss into two.
 */
static void gpu_kms_present_clock_next_locked(uint64 *base_seq_out,
                                              uint64 *base_ts_out)
{
    static uint64 anchor_ns;   /* wall-clock phase anchor (first present) */
    static uint64 grid_seq;    /* free-running 60 Hz frame counter */
    const uint64 period_ns = gpu_kms_vblank_period_ns(); /* 16666667 */
    uint64 now_ns = gpu_kms_monotonic_ns();
    uint64 target_seq;
    uint64 target_ts;

    if (anchor_ns == 0) {
        anchor_ns = now_ns != 0 ? now_ns : 1;
        grid_seq = 0;
    }

    target_seq = grid_seq + 1;
    target_ts = anchor_ns + target_seq * period_ns;

    /* If the worker fell behind (long blit / host stall), the naive next edge
     * is already in the past. Snap to the most recent elapsed grid edge, not
     * the next future one. The paced-delivery helper then uses its minimum
     * one-tick path instead of sleeping for almost a second vblank. Skipped
     * sequence numbers still tell kwin that frames were missed, while the
     * elapsed-edge timestamp lets its next repaint catch up to the original
     * wall-clock phase. */
    if (now_ns >= target_ts) {
        uint64 elapsed = (now_ns > anchor_ns) ?
                         (now_ns - anchor_ns) / period_ns : 0;
        target_seq = elapsed;
        target_ts = anchor_ns + target_seq * period_ns;
        fb_state.stats.present_clock60_snap_total++;
    }

    grid_seq = target_seq;
    fb_state.stats.present_clock60_events_total++;

    /* Previous grid point; pace_completed(+1) delivers on the target edge. */
    *base_seq_out = target_seq - 1;
    *base_ts_out = target_ts - period_ns;
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
    target_sequence = req.sequence;
    if ((req.flags & DRM_CRTC_SEQUENCE_RELATIVE) != 0)
        target_sequence = current_sequence + req.sequence;
    if (target_sequence <= current_sequence &&
        (req.flags & DRM_CRTC_SEQUENCE_NEXT_ON_MISS) != 0)
        target_sequence = current_sequence + 1;
    if (target_sequence <= current_sequence &&
        (req.flags & DRM_CRTC_SEQUENCE_NEXT_ON_MISS) == 0)
        target_sequence = current_sequence;
    timestamp_ns = gpu_kms_predict_vblank_timestamp(current_sequence,
                                                    timestamp_ns,
                                                    target_sequence);
    ret = gpu_drm_event_queue_locked(owner, DRM_EVENT_VBLANK, req.user_data,
                                     target_sequence, GPU_DRM_CRTC_ID,
                                     timestamp_ns);
    spin_unlock(&fb_state.lock);
    if (ret != 0)
        return ret;
    gpu_drm_event_notify_read(owner);

    req.sequence = target_sequence;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

#define GPU_DRM_GAMMA_LUT_SIZE 256

static uint16 gpu_drm_gamma_identity(uint32 index)
{
    if (GPU_DRM_GAMMA_LUT_SIZE <= 1)
        return 0xffff;
    return (uint16)((index * 0xffffu) / (GPU_DRM_GAMMA_LUT_SIZE - 1));
}

static int gpu_drm_mode_gamma(uint64 arg, int set)
{
    struct drm_mode_crtc_lut_compat req;
    uint16 red[GPU_DRM_GAMMA_LUT_SIZE];
    uint16 green[GPU_DRM_GAMMA_LUT_SIZE];
    uint16 blue[GPU_DRM_GAMMA_LUT_SIZE];

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.crtc_id != GPU_DRM_CRTC_ID)
        return -EINVAL;
    if (req.gamma_size > GPU_DRM_GAMMA_LUT_SIZE)
        return -EINVAL;
    if (req.gamma_size != 0 &&
        (req.red == 0 || req.green == 0 || req.blue == 0))
        return -EINVAL;
    if (req.gamma_size != 0) {
        uint64 bytes = (uint64)req.gamma_size * sizeof(uint16);

        if (set) {
            if (either_copyin(red, 1, req.red, bytes) < 0 ||
                either_copyin(green, 1, req.green, bytes) < 0 ||
                either_copyin(blue, 1, req.blue, bytes) < 0)
                return -EFAULT;
        } else {
            for (uint32 i = 0; i < req.gamma_size; i++) {
                uint16 value = gpu_drm_gamma_identity(i);

                red[i] = value;
                green[i] = value;
                blue[i] = value;
            }
            if (either_copyout(1, req.red, red, bytes) < 0 ||
                either_copyout(1, req.green, green, bytes) < 0 ||
                either_copyout(1, req.blue, blue, bytes) < 0)
                return -EFAULT;
        }
    }
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_kms_fb_owner_matches(const struct fb_gpu_kms_fb_entry *fb,
                                    struct fb_gpu_render_owner *owner);

#include "fb_drm_kms_objects.c"
#include "fb_drm_kms_properties.c"
#include "fb_drm_kms_atomic_props.c"
