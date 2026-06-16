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
    struct gpu_drm_in_formats_blob in_formats;
    void *user_copy = NULL;
    const void *blob_data = NULL;
    uint32 blob_size = 0;
    uint32 capacity;
    uint32 n;
    int found_user_blob = 0;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.blob_id == GPU_DRM_MODE_BLOB_ID) {
        gpu_drm_fill_mode(&mode);
        blob_data = &mode;
        blob_size = sizeof(mode);
    } else if (req.blob_id == GPU_DRM_IN_FORMATS_BLOB_ID) {
        gpu_drm_fill_in_formats_blob(&in_formats);
        blob_data = &in_formats;
        blob_size = sizeof(in_formats);
    } else {
        spin_lock(&fb_state.lock);
        for (int i = 0; i < FB_GPU_MAX_USER_BLOBS; i++) {
            if (fb_state.user_blobs[i].in_use &&
                fb_state.user_blobs[i].id == req.blob_id) {
                blob_size = fb_state.user_blobs[i].length;
                found_user_blob = 1;
                break;
            }
        }
        spin_unlock(&fb_state.lock);
        if (!found_user_blob)
            return -ENOENT;
        if (req.data != 0 && req.length != 0) {
            user_copy = kvmalloc(blob_size);
            if (user_copy == NULL)
                return -ENOMEM;
            found_user_blob = 0;
            spin_lock(&fb_state.lock);
            for (int i = 0; i < FB_GPU_MAX_USER_BLOBS; i++) {
                if (fb_state.user_blobs[i].in_use &&
                    fb_state.user_blobs[i].id == req.blob_id) {
                    if (fb_state.user_blobs[i].length == blob_size) {
                        memmove(user_copy, fb_state.user_blobs[i].data,
                                blob_size);
                        found_user_blob = 1;
                    }
                    break;
                }
            }
            spin_unlock(&fb_state.lock);
            if (!found_user_blob) {
                kvfree(user_copy);
                return -ENOENT;
            }
            blob_data = user_copy;
        }
    }
    capacity = req.length;
    req.length = blob_size;
    if (req.data != 0 && capacity != 0) {
        n = capacity < blob_size ? capacity : blob_size;
        if (either_copyout(1, req.data, (void *)blob_data, n) < 0) {
            if (user_copy != NULL)
                kvfree(user_copy);
            return -EFAULT;
        }
    }
    if (user_copy != NULL)
        kvfree(user_copy);
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static uint32 gpu_drm_alloc_user_blob_id_locked(void)
{
    uint32 start = fb_state.next_user_blob_id;
    uint32 id = start;

    if (start <= GPU_DRM_IN_FORMATS_BLOB_ID)
        start = GPU_DRM_IN_FORMATS_BLOB_ID + 1;
    id = start;
    do {
        int used = 0;

        for (int i = 0; i < FB_GPU_MAX_USER_BLOBS; i++) {
            if (fb_state.user_blobs[i].in_use &&
                fb_state.user_blobs[i].id == id) {
                used = 1;
                break;
            }
        }
        if (!used) {
            fb_state.next_user_blob_id = id + 1;
            if (fb_state.next_user_blob_id <= GPU_DRM_IN_FORMATS_BLOB_ID)
                fb_state.next_user_blob_id = GPU_DRM_IN_FORMATS_BLOB_ID + 1;
            return id;
        }
        id++;
        if (id <= GPU_DRM_IN_FORMATS_BLOB_ID)
            id = GPU_DRM_IN_FORMATS_BLOB_ID + 1;
    } while (id != start);
    return 0;
}

static int gpu_drm_mode_createblob(uint64 arg)
{
    struct drm_mode_create_blob_compat req;
    void *data = NULL;
    uint32 id;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.length > FB_GPU_MAX_USER_BLOB_SIZE)
        return -E2BIG;
    if (req.length != 0 && req.data == 0)
        return -EINVAL;
    if (req.length != 0) {
        data = kvmalloc(req.length);
        if (data == NULL)
            return -ENOMEM;
        if (either_copyin(data, 1, req.data, req.length) < 0) {
            kvfree(data);
            return -EFAULT;
        }
    }

    spin_lock(&fb_state.lock);
    for (int i = 0; i < FB_GPU_MAX_USER_BLOBS; i++) {
        if (!fb_state.user_blobs[i].in_use) {
            id = gpu_drm_alloc_user_blob_id_locked();
            if (id == 0) {
                spin_unlock(&fb_state.lock);
                kvfree(data);
                return -ENOSPC;
            }
            fb_state.user_blobs[i].in_use = 1;
            fb_state.user_blobs[i].id = id;
            fb_state.user_blobs[i].length = req.length;
            fb_state.user_blobs[i].data = data;
            req.blob_id = id;
            spin_unlock(&fb_state.lock);
            if (either_copyout(1, arg, &req, sizeof(req)) < 0) {
                spin_lock(&fb_state.lock);
                if (fb_state.user_blobs[i].in_use &&
                    fb_state.user_blobs[i].id == id)
                    memset(&fb_state.user_blobs[i], 0,
                           sizeof(fb_state.user_blobs[i]));
                spin_unlock(&fb_state.lock);
                kvfree(data);
                return -EFAULT;
            }
            return 0;
        }
    }
    spin_unlock(&fb_state.lock);
    kvfree(data);
    return -ENOSPC;
}

static int gpu_drm_mode_destroyblob(uint64 arg)
{
    struct drm_mode_destroy_blob_compat req;
    void *data = NULL;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.blob_id == 0 || req.blob_id == GPU_DRM_MODE_BLOB_ID ||
        req.blob_id == GPU_DRM_IN_FORMATS_BLOB_ID)
        return -EINVAL;

    spin_lock(&fb_state.lock);
    for (int i = 0; i < FB_GPU_MAX_USER_BLOBS; i++) {
        if (fb_state.user_blobs[i].in_use &&
            fb_state.user_blobs[i].id == req.blob_id) {
            data = fb_state.user_blobs[i].data;
            memset(&fb_state.user_blobs[i], 0,
                   sizeof(fb_state.user_blobs[i]));
            spin_unlock(&fb_state.lock);
            kvfree(data);
            return 0;
        }
    }
    spin_unlock(&fb_state.lock);
    return -ENOENT;
}

static int gpu_drm_mode_getplaneresources(uint64 arg)
{
    struct drm_mode_get_plane_res_compat req;
    uint32 plane_ids[] = {
        GPU_DRM_PRIMARY_PLANE_ID,
        GPU_DRM_CURSOR_PLANE_ID,
    };
    uint32 copy_count;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    copy_count = req.count_planes;
    if (copy_count > sizeof(plane_ids) / sizeof(plane_ids[0]))
        copy_count = sizeof(plane_ids) / sizeof(plane_ids[0]);
    if (req.plane_id_ptr != 0 && copy_count != 0) {
        if (either_copyout(1, req.plane_id_ptr, plane_ids,
                           copy_count * sizeof(plane_ids[0])) < 0)
            return -EFAULT;
    }
    req.count_planes = sizeof(plane_ids) / sizeof(plane_ids[0]);
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_mode_getplane(uint64 arg)
{
    struct drm_mode_get_plane_compat req;
    const uint32 *formats;
    uint32 current_fb;
    uint32 format_count;
    uint32 copy_count;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.plane_id == GPU_DRM_PRIMARY_PLANE_ID) {
        formats = gpu_kms_primary_scanout_formats;
        format_count =
            sizeof(gpu_kms_primary_scanout_formats) /
            sizeof(gpu_kms_primary_scanout_formats[0]);
        current_fb = fb_state.current_kms_fb_id;
    } else if (req.plane_id == GPU_DRM_CURSOR_PLANE_ID) {
        formats = gpu_kms_cursor_formats;
        format_count =
            sizeof(gpu_kms_cursor_formats) /
            sizeof(gpu_kms_cursor_formats[0]);
        current_fb = fb_state.current_cursor_fb_id;
    } else {
        return -ENOENT;
    }
    copy_count = req.count_format_types;
    if (copy_count > format_count)
        copy_count = format_count;
    if (req.format_type_ptr != 0 && copy_count != 0 &&
        either_copyout(1, req.format_type_ptr, (void *)formats,
                       copy_count * sizeof(formats[0])) < 0)
        return -EFAULT;
    req.crtc_id = req.plane_id == GPU_DRM_CURSOR_PLANE_ID &&
        !fb_state.current_cursor_visible ? 0 : GPU_DRM_CRTC_ID;
    req.fb_id = current_fb;
    req.possible_crtcs = 1;
    req.gamma_size = 0;
    req.count_format_types = format_count;
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

struct gpu_kms_cursor_atomic_state {
    int touched;
    uint32 fb_id;
    uint32 crtc_id;
    int32 crtc_x;
    int32 crtc_y;
    uint32 crtc_w;
    uint32 crtc_h;
    uint32 src_x;
    uint32 src_y;
    uint32 src_w;
    uint32 src_h;
};

static int gpu_kms_cursor_format_supported(uint32 pixel_format,
                                           uint64 modifier)
{
    return modifier == DRM_FORMAT_MOD_LINEAR &&
        pixel_format == DRM_FORMAT_ARGB8888;
}

static int gpu_kms_copy_bo_cursor_pixels(struct fb_gpu_bo_entry *bo,
                                         uint32 width, uint32 height,
                                         uint32 pitch, uint32 offset,
                                         uint32 *pixels)
{
    uint64 row_bytes;
    uint64 last_byte;

    if (bo == NULL || pixels == NULL || bo->pages == NULL)
        return -EINVAL;
    if (width == 0 || height == 0 ||
        width > FB_GPU_CURSOR_MAX_DIM || height > FB_GPU_CURSOR_MAX_DIM)
        return -EINVAL;
    row_bytes = (uint64)width * sizeof(uint32);
    if (pitch < row_bytes)
        return -EINVAL;
    last_byte = (uint64)offset + (uint64)(height - 1) * pitch + row_bytes;
    if (last_byte < offset || last_byte > bo->size)
        return -EINVAL;

    for (uint32 row = 0; row < height; row++) {
        uint64 src_off = (uint64)offset + (uint64)row * pitch;
        uint64 remaining = row_bytes;
        uint32 copied = 0;

        while (remaining != 0) {
            uint32 page_idx = src_off / PGSIZE;
            uint32 page_off = src_off & (PGSIZE - 1);
            uint32 chunk = PGSIZE - page_off;
            uint8 *src;

            if (page_idx >= bo->npages || bo->pages[page_idx] == NULL)
                return -EINVAL;
            if (chunk > remaining)
                chunk = remaining;
            src = (uint8 *)PA2VA(__page_to_pa(bo->pages[page_idx])) +
                page_off;
            memmove((uint8 *)&pixels[row * width] + copied, src, chunk);
            src_off += chunk;
            copied += chunk;
            remaining -= chunk;
        }
    }
    return 0;
}

static void gpu_kms_record_cursor_pixels(const uint32 *pixels, uint32 width,
                                         uint32 height, uint32 hot_x,
                                         uint32 hot_y)
{
    uint64 checksum = 1469598103934665603ULL;
    uint64 alpha_nonzero = 0;
    uint64 alpha_zero = 0;
    uint64 alpha_opaque = 0;
    uint64 rgb_nonzero = 0;
    uint64 count = (uint64)width * height;

    for (uint64 i = 0; i < count; i++) {
        uint32 pixel = pixels[i];
        uint32 alpha = pixel >> 24;

        checksum ^= pixel;
        checksum *= 1099511628211ULL;
        if (alpha == 0)
            alpha_zero++;
        else
            alpha_nonzero++;
        if (alpha == 0xff)
            alpha_opaque++;
        if ((pixel & 0x00ffffffU) != 0)
            rgb_nonzero++;
    }

    fb_state.stats.kms_cursor_uploads++;
    fb_state.stats.kms_cursor_last_width = width;
    fb_state.stats.kms_cursor_last_height = height;
    fb_state.stats.kms_cursor_last_hot_x = hot_x;
    fb_state.stats.kms_cursor_last_hot_y = hot_y;
    fb_state.stats.kms_cursor_last_checksum = checksum;
    fb_state.stats.kms_cursor_last_alpha_nonzero = alpha_nonzero;
    fb_state.stats.kms_cursor_last_alpha_zero = alpha_zero;
    fb_state.stats.kms_cursor_last_alpha_opaque = alpha_opaque;
    fb_state.stats.kms_cursor_last_rgb_nonzero = rgb_nonzero;
    fb_state.stats.kms_cursor_last_first_pixel = count != 0 ? pixels[0] : 0;
    fb_state.stats.kms_cursor_last_center_pixel =
        count != 0 ? pixels[(height / 2) * width + (width / 2)] : 0;
}

static int gpu_kms_upload_cursor_from_bo(struct fb_gpu_render_owner *owner,
                                         uint32 handle, uint32 width,
                                         uint32 height, uint32 pitch,
                                         uint32 offset, uint32 hot_x,
                                         uint32 hot_y)
{
    struct fb_gpu_bo_entry *bo;
    uint32 *pixels;
    uint64 size;
    int ret;

    if (owner == NULL || handle == 0)
        return -EINVAL;
    if (width == 0 || height == 0 ||
        width > FB_GPU_CURSOR_MAX_DIM || height > FB_GPU_CURSOR_MAX_DIM ||
        hot_x >= width || hot_y >= height)
        return -EINVAL;
    size = (uint64)width * height * sizeof(uint32);
    pixels = kvmalloc((size_t)size);
    if (pixels == NULL)
        return -ENOMEM;
    bo = fb_bo_get_owned(handle, owner->id, owner->tgid);
    if (bo == NULL) {
        kvfree(pixels);
        return -ENOENT;
    }
    ret = gpu_kms_copy_bo_cursor_pixels(bo, width, height, pitch, offset,
                                        pixels);
    fb_bo_put(bo);
    if (ret == 0) {
        gpu_kms_record_cursor_pixels(pixels, width, height, hot_x, hot_y);
        ret = virtio_gpu_user_set_cursor(pixels, width, height, hot_x,
                                         hot_y);
    }
    if (ret != 0)
        fb_state.stats.kms_cursor_upload_failures++;
    kvfree(pixels);
    return ret;
}

static int gpu_kms_cursor_fb_usable_locked(struct fb_gpu_kms_fb_entry *fb,
                                           struct fb_gpu_render_owner *owner)
{
    if (!gpu_kms_fb_owner_matches(fb, owner))
        return -ENOENT;
    if (fb->width == 0 || fb->height == 0 ||
        fb->width > FB_GPU_CURSOR_MAX_DIM ||
        fb->height > FB_GPU_CURSOR_MAX_DIM ||
        fb->plane_count != 1 ||
        !gpu_kms_cursor_format_supported(fb->pixel_format, fb->modifier))
        return -EINVAL;
    if (fb->pitches[0] < fb->width * sizeof(uint32))
        return -EINVAL;
    return 0;
}

static int gpu_kms_apply_cursor_fb(struct fb_gpu_render_owner *owner,
                                   uint32 fb_id, int32 x, int32 y,
                                   uint32 crtc_w, uint32 crtc_h,
                                   uint32 src_x, uint32 src_y,
                                   uint32 src_w, uint32 src_h,
                                   uint32 hot_x, uint32 hot_y)
{
    struct fb_gpu_kms_fb_entry fb;
    int image_changed;
    int ret;

    if (fb_id == 0) {
        ret = virtio_gpu_user_move_cursor(x, y, 0);
        if (ret != 0)
            return ret;
        spin_lock(&fb_state.lock);
        fb_state.current_cursor_fb_id = 0;
        fb_state.current_cursor_x = x;
        fb_state.current_cursor_y = y;
        fb_state.current_cursor_visible = 0;
        spin_unlock(&fb_state.lock);
        return 0;
    }

    spin_lock(&fb_state.lock);
    ret = gpu_kms_cursor_fb_usable_locked(
        gpu_kms_fb_lookup_locked(fb_id), owner);
    if (ret == 0)
        fb = *gpu_kms_fb_lookup_locked(fb_id);
    spin_unlock(&fb_state.lock);
    if (ret != 0)
        return ret;
    if (crtc_w != fb.width || crtc_h != fb.height ||
        src_x != 0 || src_y != 0 ||
        src_w != ((uint32)fb.width << 16) ||
        src_h != ((uint32)fb.height << 16))
        return -EINVAL;
    spin_lock(&fb_state.lock);
    image_changed = fb_state.current_cursor_fb_id != fb_id ||
        fb_state.current_cursor_w != fb.width ||
        fb_state.current_cursor_h != fb.height;
    spin_unlock(&fb_state.lock);
    if (image_changed) {
        ret = gpu_kms_upload_cursor_from_bo(owner, fb.bo_handle, fb.width,
                                            fb.height, fb.pitches[0],
                                            fb.offsets[0], hot_x, hot_y);
        if (ret != 0)
            return ret;
    }
    ret = virtio_gpu_user_move_cursor(x, y, 1);
    if (ret != 0)
        return ret;
    spin_lock(&fb_state.lock);
    fb_state.current_cursor_fb_id = fb_id;
    fb_state.current_cursor_x = x;
    fb_state.current_cursor_y = y;
    fb_state.current_cursor_w = fb.width;
    fb_state.current_cursor_h = fb.height;
    fb_state.current_cursor_visible = 1;
    spin_unlock(&fb_state.lock);
    return 0;
}

static int gpu_kms_apply_cursor_atomic_state(
    struct fb_gpu_render_owner *owner,
    const struct gpu_kms_cursor_atomic_state *cursor)
{
    if (cursor == NULL || !cursor->touched)
        return 0;
    if (cursor->fb_id == 0 || cursor->crtc_id == 0)
        return gpu_kms_apply_cursor_fb(owner, 0, cursor->crtc_x,
                                       cursor->crtc_y, 0, 0, 0, 0, 0, 0,
                                       0, 0);
    if (cursor->crtc_id != GPU_DRM_CRTC_ID)
        return -EINVAL;
    return gpu_kms_apply_cursor_fb(owner, cursor->fb_id, cursor->crtc_x,
                                   cursor->crtc_y, cursor->crtc_w,
                                   cursor->crtc_h, cursor->src_x,
                                   cursor->src_y, cursor->src_w,
                                   cursor->src_h, 0, 0);
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
    if (req.flags != 0)
        return -EINVAL;
    if (req.plane_id == GPU_DRM_CURSOR_PLANE_ID) {
        if ((req.crtc_id == 0) != (req.fb_id == 0))
            return -EINVAL;
        if (req.crtc_id != 0 && req.crtc_id != GPU_DRM_CRTC_ID)
            return -EINVAL;
        return gpu_kms_apply_cursor_fb(owner, req.fb_id, req.crtc_x,
                                       req.crtc_y, req.crtc_w,
                                       req.crtc_h, req.src_x,
                                       req.src_y, req.src_w, req.src_h,
                                       0, 0);
    }
    if (req.plane_id != GPU_DRM_PRIMARY_PLANE_ID)
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

static uint64 gpu_kms_native_present_reject_reasons_locked(void)
{
    uint64 reasons = 0;

    if (fb_state.stats.nouveau_native_display_ready == 0)
        reasons |= FB_GPU_KMS_PRESENT_REJECT_NO_NATIVE_DISPLAY;
    if (fb_state.stats.nouveau_dda_native_display_present == 0)
        reasons |= FB_GPU_KMS_PRESENT_REJECT_NO_NOUVEAU_DISPLAY;
    if (fb_state.stats.nouveau_display_create_successes == 0 ||
        fb_state.stats.nouveau_display_engine_object_created == 0 ||
        fb_state.stats.nouveau_display_mode_config_ready == 0)
        reasons |= FB_GPU_KMS_PRESENT_REJECT_NO_DISPLAY_CREATE;
    if (fb_state.stats.nouveau_display_heads == 0 ||
        fb_state.stats.nouveau_display_crtc_count == 0 ||
        fb_state.stats.nouveau_display_head_mask_seen == 0 ||
        fb_state.stats.nouveau_display_nvif_head_ctor_successes == 0)
        reasons |= FB_GPU_KMS_PRESENT_REJECT_NO_HEADS;
    if (fb_state.stats.nouveau_display_connectors == 0 ||
        fb_state.stats.nouveau_display_nonvirtual_connectors == 0 ||
        fb_state.stats.nouveau_display_encoder_count == 0 ||
        fb_state.stats.nouveau_display_outp_mask_seen == 0 ||
        fb_state.stats.nouveau_display_conn_mask_seen == 0 ||
        fb_state.stats.nouveau_display_hpd_event_registered == 0 ||
        fb_state.stats.nouveau_display_dp_irq_event_registered == 0)
        reasons |= FB_GPU_KMS_PRESENT_REJECT_NO_CONNECTORS;
    if (fb_state.stats.nouveau_display_vblank_supported == 0 ||
        fb_state.stats.nouveau_display_vblank_irq_supported == 0 ||
        fb_state.stats.nouveau_display_vblank_event_registered == 0 ||
        fb_state.stats.nouveau_display_vblank_source !=
            FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ)
        reasons |= FB_GPU_KMS_PRESENT_REJECT_NO_VBLANK;
    if (fb_state.stats.nouveau_display_page_flip_completion_ready == 0 ||
        fb_state.stats.nouveau_display_page_flip_completions == 0 ||
        fb_state.stats.nouveau_display_page_flip_event_source !=
            FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ)
        reasons |= FB_GPU_KMS_PRESENT_REJECT_NO_HW_COMPLETION;
    if (fb_state.stats.nouveau_display_atomic_pageflip_backend_missing != 0 ||
        fb_state.stats.nouveau_display_atomic_commit_tail_ready == 0 ||
        fb_state.stats.nouveau_display_primary_plane_count == 0 ||
        fb_state.stats.nouveau_display_primary_plane_linear_required == 0 ||
        fb_state.stats.nouveau_display_primary_plane_nonlinear_modifiers != 0)
        reasons |= FB_GPU_KMS_PRESENT_REJECT_NO_ATOMIC_PAGEFLIP_BACKEND;
    if (reasons == 0)
        reasons = FB_GPU_KMS_PRESENT_REJECT_NO_ATOMIC_PAGEFLIP_BACKEND;
    return reasons;
}

static int gpu_kms_try_native_present_fb_locked(uint32 fb_id)
{
    uint64 reasons;

    if (fb_id == 0)
        return 0;
    reasons = gpu_kms_native_present_reject_reasons_locked();
    fb_state.stats.kms_present_last_lane = FB_GPU_KMS_PRESENT_LANE_NONE;
    fb_state.stats.kms_present_rejects++;
    fb_state.stats.kms_present_reject_reasons |= reasons;
    fb_state.stats.nouveau_native_display_reject_reasons = reasons;
    if ((reasons & FB_GPU_KMS_PRESENT_REJECT_NO_NATIVE_DISPLAY) != 0)
        fb_state.stats.kms_present_reject_no_native_display++;
    if ((reasons & FB_GPU_KMS_PRESENT_REJECT_NO_NOUVEAU_DISPLAY) != 0)
        fb_state.stats.kms_present_reject_no_nouveau_display++;
    if ((reasons & FB_GPU_KMS_PRESENT_REJECT_NO_DISPLAY_CREATE) != 0)
        fb_state.stats.kms_present_reject_no_display_create++;
    if ((reasons & FB_GPU_KMS_PRESENT_REJECT_NO_HEADS) != 0)
        fb_state.stats.kms_present_reject_no_heads++;
    if ((reasons & FB_GPU_KMS_PRESENT_REJECT_NO_CONNECTORS) != 0)
        fb_state.stats.kms_present_reject_no_connectors++;
    if ((reasons & FB_GPU_KMS_PRESENT_REJECT_NO_VBLANK) != 0)
        fb_state.stats.kms_present_reject_no_vblank++;
    if ((reasons & FB_GPU_KMS_PRESENT_REJECT_NO_HW_COMPLETION) != 0)
        fb_state.stats.kms_present_reject_no_hw_completion++;
    if ((reasons &
         FB_GPU_KMS_PRESENT_REJECT_NO_ATOMIC_PAGEFLIP_BACKEND) != 0)
        fb_state.stats.kms_present_reject_no_atomic_pageflip_backend++;
    return -EOPNOTSUPP;
}

static int gpu_kms_present_fb(struct fb_gpu_render_owner *owner, uint32 fb_id)
{
    struct fb_gpu_bo_entry *bo;
    struct fb_gpu_bo_present present;
    uint32 bo_handle = 0;
    uint32 width = 0;
    uint32 height = 0;
    uint32 xres = 0;
    uint32 yres = 0;
    uint32 pixel_format = 0;
    uint64 modifier = DRM_FORMAT_MOD_LINEAR;
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
        modifier = fb->modifier;
        break;
    }
    xres = fb_state.xres;
    yres = fb_state.yres;
    spin_unlock(&fb_state.lock);
    if (bo_handle == 0)
        return -ENOENT;
    if (width == 0 || height == 0)
        return -EINVAL;
    if (!gpu_kms_primary_scanout_format_supported(pixel_format, modifier))
        return -EOPNOTSUPP;

    spin_lock(&fb_state.lock);
    (void)gpu_kms_try_native_present_fb_locked(fb_id);
    spin_unlock(&fb_state.lock);

    bo = fb_bo_get_owned(bo_handle, owner->id, owner->tgid);
    if (bo == NULL)
        return -ENOENT;
    memset(&present, 0, sizeof(present));
    present.w = width;
    present.h = height;
    if (width == xres && height == yres &&
        modifier == DRM_FORMAT_MOD_LINEAR &&
        !fb_scanout_format_needs_rb_swap(pixel_format) &&
        !fb_cmdline_enabled("virtio_gpu_no_kms_resource_scanout")) {
        /*
         * Alpine's smooth virgl/KMS path presents completed full-screen GBM
         * buffers by flipping scanout to that resource, then flushing it.  Use
         * that same shape for full-size KMS framebuffers; fb_blit_from_bo_format
         * falls back to the existing CPU/readback path when the BO is not
         * virtio-resource backed.
         */
        present.flags = FB_GPU_BO_PRESENT_F_VIRGL_SCANOUT;
    }
    ret = fb_blit_from_bo_format(bo, present, pixel_format, modifier,
                                 &fence, NULL);
    if (ret != 0 &&
        (present.flags & FB_GPU_BO_PRESENT_F_VIRGL_SCANOUT) != 0) {
        memset(&present, 0, sizeof(present));
        present.w = width;
        present.h = height;
        ret = fb_blit_from_bo_format(bo, present, pixel_format, modifier,
                                     &fence, NULL);
    }
    fb_bo_put(bo);
    return ret;
}

static int gpu_kms_present_fb_rect(struct fb_gpu_render_owner *owner,
                                   const struct fb_gpu_kms_fb_entry *fb,
                                   uint32 x, uint32 y, uint32 w, uint32 h)
{
    struct fb_gpu_bo_entry *bo;
    struct fb_gpu_bo_present present;
    uint64 fence = 0;
    int ret;

    if (owner == NULL || fb == NULL)
        return -EINVAL;
    if (w == 0 || h == 0)
        return 0;
    if ((uint64)x + w > fb->width || (uint64)y + h > fb->height)
        return -EINVAL;
    if (!gpu_kms_primary_scanout_format_supported(fb->pixel_format,
                                                  fb->modifier))
        return -EOPNOTSUPP;

    bo = fb_bo_get_owned(fb->bo_handle, owner->id, owner->tgid);
    if (bo == NULL)
        return -ENOENT;

    memset(&present, 0, sizeof(present));
    present.x = x;
    present.y = y;
    present.w = w;
    present.h = h;
    present.pixels = fb->offsets[0] + (uint64)y * fb->pitch + (uint64)x * 4;
    if (fb->modifier == DRM_FORMAT_MOD_LINEAR &&
        !fb_scanout_format_needs_rb_swap(fb->pixel_format) &&
        bo->virtio_resource_id != 0 &&
        !fb_cmdline_enabled("virtio_gpu_no_kms_resource_scanout"))
        present.flags = FB_GPU_BO_PRESENT_F_VIRGL_SCANOUT;

    ret = fb_blit_from_bo_format(bo, present, fb->pixel_format,
                                 fb->modifier, &fence, NULL);
    if (ret != 0 &&
        (present.flags & FB_GPU_BO_PRESENT_F_VIRGL_SCANOUT) != 0) {
        present.flags = 0;
        ret = fb_blit_from_bo_format(bo, present, fb->pixel_format,
                                     fb->modifier, &fence, NULL);
    }
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
    if (!gpu_kms_primary_scanout_format_supported(fb.pixel_format,
                                                  fb.modifier))
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
    req.depth = (fb.pixel_format == DRM_FORMAT_XRGB8888 ||
                 fb.pixel_format == DRM_FORMAT_XBGR8888) ? 24 : 32;
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

static int gpu_drm_mode_cursor(struct fb_gpu_render_owner *owner,
                               uint64 arg, int cursor2)
{
    struct drm_mode_cursor2_compat req;
    int ret = 0;
    int visible;
    int32 x;
    int32 y;

    if (owner == NULL)
        return -EBADF;
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
    if ((req.flags & DRM_MODE_CURSOR_BO) != 0 && req.handle != 0) {
        if (req.width == 0 || req.height == 0 ||
            req.width > FB_GPU_CURSOR_MAX_DIM ||
            req.height > FB_GPU_CURSOR_MAX_DIM ||
            req.hot_x < 0 || req.hot_y < 0 ||
            (uint32)req.hot_x >= req.width ||
            (uint32)req.hot_y >= req.height)
            return -EINVAL;
        ret = gpu_kms_upload_cursor_from_bo(owner, req.handle,
                                            req.width, req.height,
                                            req.width * sizeof(uint32), 0,
                                            (uint32)req.hot_x,
                                            (uint32)req.hot_y);
        if (ret != 0)
            return ret;
        spin_lock(&fb_state.lock);
        fb_state.current_cursor_fb_id = 0;
        fb_state.current_cursor_w = req.width;
        fb_state.current_cursor_h = req.height;
        fb_state.current_cursor_visible = 1;
        spin_unlock(&fb_state.lock);
    } else if ((req.flags & DRM_MODE_CURSOR_BO) != 0) {
        spin_lock(&fb_state.lock);
        x = fb_state.current_cursor_x;
        y = fb_state.current_cursor_y;
        if ((req.flags & DRM_MODE_CURSOR_MOVE) != 0) {
            x = req.x;
            y = req.y;
        }
        spin_unlock(&fb_state.lock);
        ret = virtio_gpu_user_move_cursor(x, y, 0);
        if (ret != 0)
            return ret;
        spin_lock(&fb_state.lock);
        fb_state.current_cursor_fb_id = 0;
        fb_state.current_cursor_x = x;
        fb_state.current_cursor_y = y;
        fb_state.current_cursor_visible = 0;
        spin_unlock(&fb_state.lock);
        return 0;
    }

    if ((req.flags & DRM_MODE_CURSOR_MOVE) != 0 ||
        ((req.flags & DRM_MODE_CURSOR_BO) != 0 && req.handle != 0)) {
        spin_lock(&fb_state.lock);
        x = (req.flags & DRM_MODE_CURSOR_MOVE) != 0 ?
            req.x : fb_state.current_cursor_x;
        y = (req.flags & DRM_MODE_CURSOR_MOVE) != 0 ?
            req.y : fb_state.current_cursor_y;
        visible = fb_state.current_cursor_visible ||
            ((req.flags & DRM_MODE_CURSOR_BO) != 0 && req.handle != 0);
        spin_unlock(&fb_state.lock);
        ret = virtio_gpu_user_move_cursor(x, y, visible);
        if (ret != 0)
            return ret;
        spin_lock(&fb_state.lock);
        fb_state.current_cursor_x = x;
        fb_state.current_cursor_y = y;
        fb_state.current_cursor_visible = visible;
        spin_unlock(&fb_state.lock);
    }
    return 0;
}

static int gpu_drm_mode_dirtyfb(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_mode_fb_dirty_cmd_compat req;
    struct fb_gpu_kms_fb_entry fb;
    int is_current;
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
    is_current = fb_state.current_kms_fb_id == req.fb_id;
    spin_unlock(&fb_state.lock);
    if (!is_current)
        return 0;
    if (req.num_clips == 0)
        return gpu_kms_present_fb(owner, req.fb_id);

    for (uint32 i = 0; i < req.num_clips; i++) {
        struct drm_clip_rect_compat clip;
        uint32 x, y, w, h;

        if (either_copyin(&clip, 1,
                          req.clips_ptr + i * sizeof(clip),
                          sizeof(clip)) < 0)
            return -EFAULT;
        if (clip.x2 <= clip.x1 || clip.y2 <= clip.y1)
            continue;
        x = clip.x1;
        y = clip.y1;
        if (x >= fb.width || y >= fb.height)
            continue;
        w = clip.x2 - clip.x1;
        h = clip.y2 - clip.y1;
        if ((uint64)x + w > fb.width)
            w = fb.width - x;
        if ((uint64)y + h > fb.height)
            h = fb.height - y;
        ret = gpu_kms_present_fb_rect(owner, &fb, x, y, w, h);
        if (ret != 0)
            return ret;
    }
    return 0;
}

struct gpu_kms_obj_props {
    uint32 props[16];
    uint64 values[16];
    uint32 count;
};

#define GPU_KMS_MAX_IN_FENCES 8
