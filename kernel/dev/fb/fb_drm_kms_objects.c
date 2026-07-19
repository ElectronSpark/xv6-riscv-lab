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
    req.gamma_size = GPU_DRM_GAMMA_LUT_SIZE;
    req.mode_valid = req.fb_id != 0;
    if (req.mode_valid)
        gpu_drm_fill_mode(&req.mode);
    else
        memset(&req.mode, 0, sizeof(req.mode));
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

static uint32 gpu_drm_build_connector_modes(
    struct drm_mode_modeinfo_compat modes[GPU_DRM_CONNECTOR_MODE_CAP])
{
    uint32 current_w = 0, current_h = 0;
    uint32 count = 0;

    /*
     * Every mode returned by GETCONNECTOR is a promise that an atomic client
     * may create a MODE_ID blob for it and successfully TEST_ONLY that state.
     * The backend cannot yet carry a mode change through resize and scanout
     * reconfiguration, so advertise only the active mode.  Listing EDID or
     * convenience modes here while rejecting them later recreates KWin's
     * opening-logo stall when a saved output configuration selects one.
     */
    gpu_drm_get_mode_size(&current_w, &current_h);
    gpu_drm_connector_add_mode(modes, &count, current_w, current_h,
                               60000, 1);
    return count;
}

static int gpu_drm_mode_getconnector(uint64 arg)
{
    struct drm_mode_get_connector_compat req;
    struct drm_mode_modeinfo_compat modes[GPU_DRM_CONNECTOR_MODE_CAP];
    uint32 encoder = GPU_DRM_ENCODER_ID;
    uint32 props[3] = {
        GPU_DRM_PROP_CRTC_ID,
        GPU_DRM_PROP_MODE_ID,
        GPU_DRM_PROP_DPMS,
    };
    uint64 prop_values[3] = {
        GPU_DRM_CRTC_ID,
        GPU_DRM_MODE_BLOB_ID,
        DRM_MODE_DPMS_ON,
    };
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
    ret = gpu_drm_copyout_u32_array(req.props_ptr, req.count_props, props, 3);
    if (ret != 0)
        return ret;
    ret = gpu_drm_copyout_u64_array(req.prop_values_ptr, req.count_props,
                                    prop_values, 3);
    if (ret != 0)
        return ret;

    req.count_modes = mode_count;
    req.count_props = 3;
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
