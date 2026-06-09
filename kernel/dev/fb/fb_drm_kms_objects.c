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

#define GPU_DRM_CONNECTOR_MODE_CAP 8

static void gpu_drm_connector_add_mode(
    struct drm_mode_modeinfo_compat modes[GPU_DRM_CONNECTOR_MODE_CAP],
    uint32 *count, uint32 width, uint32 height, uint32 refresh_millihz,
    int preferred)
{
    if (modes == NULL || count == NULL ||
        *count >= GPU_DRM_CONNECTOR_MODE_CAP ||
        width < 640 || width > 2560 || height < 400 || height > 1600)
        return;
    for (uint32 i = 0; i < *count; i++) {
        if (modes[i].hdisplay == width && modes[i].vdisplay == height)
            return;
    }
    gpu_drm_fill_mode_size(&modes[*count], width, height,
                           refresh_millihz, preferred);
    (*count)++;
}

static uint32 gpu_drm_connector_min_u32(uint32 a, uint32 b)
{
    if (a == 0)
        return b;
    if (b == 0)
        return a;
    return a < b ? a : b;
}

static uint32 gpu_drm_connector_max_u32(uint32 a, uint32 b)
{
    return a > b ? a : b;
}

static uint32 gpu_drm_build_connector_modes(
    struct drm_mode_modeinfo_compat modes[GPU_DRM_CONNECTOR_MODE_CAP])
{
    uint32 current_w = 0, current_h = 0;
    uint32 host_w = 0, host_h = 0;
    uint32 edid_w = 0, edid_h = 0, edid_refresh = 0;
    uint32 limit_w, limit_h;
    uint32 count = 0;

    gpu_drm_get_mode_size(&current_w, &current_h);
    virtio_gpu_probe_scanout(&host_w, &host_h);
    virtio_gpu_probe_edid_mode(&edid_w, &edid_h, &edid_refresh);

    gpu_drm_connector_add_mode(modes, &count, current_w, current_h,
                               60000, 1);
    if (edid_w != 0 && edid_h != 0)
        gpu_drm_connector_add_mode(modes, &count, edid_w, edid_h,
                                   edid_refresh, 0);
    if (host_w != 0 && host_h != 0)
        gpu_drm_connector_add_mode(modes, &count, host_w, host_h,
                                   60000, 0);

    limit_w = gpu_drm_connector_max_u32(current_w, host_w);
    limit_w = gpu_drm_connector_max_u32(limit_w, edid_w);
    limit_h = gpu_drm_connector_max_u32(current_h, host_h);
    limit_h = gpu_drm_connector_max_u32(limit_h, edid_h);

    if (limit_w == 0 || limit_h == 0) {
        limit_w = FB_DEFAULT_WIDTH;
        limit_h = FB_DEFAULT_HEIGHT;
    }

    if (gpu_drm_connector_min_u32(limit_w, 1920) == 1920 &&
        gpu_drm_connector_min_u32(limit_h, 1080) == 1080)
        gpu_drm_connector_add_mode(modes, &count, 1920, 1080, 60000, 0);
    if (gpu_drm_connector_min_u32(limit_w, 1280) == 1280 &&
        gpu_drm_connector_min_u32(limit_h, 800) == 800)
        gpu_drm_connector_add_mode(modes, &count, 1280, 800, 60000, 0);
    if (gpu_drm_connector_min_u32(limit_w, 1280) == 1280 &&
        gpu_drm_connector_min_u32(limit_h, 720) == 720)
        gpu_drm_connector_add_mode(modes, &count, 1280, 720, 60000, 0);
    if (gpu_drm_connector_min_u32(limit_w, 1024) == 1024 &&
        gpu_drm_connector_min_u32(limit_h, 768) == 768)
        gpu_drm_connector_add_mode(modes, &count, 1024, 768, 60000, 0);
    if (gpu_drm_connector_min_u32(limit_w, 800) == 800 &&
        gpu_drm_connector_min_u32(limit_h, 600) == 600)
        gpu_drm_connector_add_mode(modes, &count, 800, 600, 60000, 0);
    gpu_drm_connector_add_mode(modes, &count, 640, 480, 60000, 0);

    return count;
}

static int gpu_drm_mode_getconnector(uint64 arg)
{
    struct drm_mode_get_connector_compat req;
    struct drm_mode_modeinfo_compat modes[GPU_DRM_CONNECTOR_MODE_CAP];
    uint32 encoder = GPU_DRM_ENCODER_ID;
    uint32 props[2] = { GPU_DRM_PROP_CRTC_ID, GPU_DRM_PROP_MODE_ID };
    uint64 prop_values[2] = { GPU_DRM_CRTC_ID, GPU_DRM_MODE_BLOB_ID };
    uint32 mode_count;
    int ret;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.connector_id != GPU_DRM_CONNECTOR_ID)
        return -ENOENT;
    memset(modes, 0, sizeof(modes));
    mode_count = gpu_drm_build_connector_modes(modes);
    ret = gpu_drm_copyout_u32_array(req.encoders_ptr, req.count_encoders,
                                    &encoder, 1);
    if (ret != 0)
        return ret;
    ret = gpu_drm_copyout_mode_array(req.modes_ptr, req.count_modes, modes,
                                     mode_count);
    if (ret != 0)
        return ret;
    ret = gpu_drm_copyout_u32_array(req.props_ptr, req.count_props, props, 2);
    if (ret != 0)
        return ret;
    ret = gpu_drm_copyout_u64_array(req.prop_values_ptr, req.count_props,
                                    prop_values, 2);
    if (ret != 0)
        return ret;

    req.count_modes = mode_count;
    req.count_props = 2;
    req.count_encoders = 1;
    req.encoder_id = GPU_DRM_ENCODER_ID;
    req.connector_type = DRM_MODE_CONNECTOR_VIRTUAL;
    req.connector_type_id = 1;
    req.connection = DRM_MODE_CONNECTED;
    req.mm_width = modes[0].hdisplay / 4;
    req.mm_height = modes[0].vdisplay / 4;
    req.subpixel = DRM_MODE_SUBPIXEL_UNKNOWN;
    req.pad = 0;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}
