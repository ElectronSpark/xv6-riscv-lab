}

static int gpu_drm_mode_addfb2_req(struct fb_gpu_render_owner *owner,
                                   struct drm_mode_fb_cmd2_compat *req)
{
    struct fb_gpu_bo_entry *bo;
    uint64 min_size;
    uint32 fb_id = 0;
    uint32 plane_count = 1;
    int ret = 0;

    if (!gpu_drm_is_primary_like(owner))
        return -EOPNOTSUPP;
    if (req == NULL)
        return -EINVAL;
    if (req->width == 0 || req->height == 0 || req->handles[0] == 0 ||
        req->pitches[0] == 0)
        return -EINVAL;
    if ((req->flags & ~(DRM_MODE_FB_MODIFIERS)) != 0)
        return -EINVAL;
    if (req->pixel_format != DRM_FORMAT_XRGB8888 &&
        req->pixel_format != DRM_FORMAT_ARGB8888 &&
        req->pixel_format != DRM_FORMAT_XBGR8888 &&
        req->pixel_format != DRM_FORMAT_ABGR8888 &&
        req->pixel_format != DRM_FORMAT_NV12)
        return -EINVAL;
    if (req->pixel_format == DRM_FORMAT_NV12) {
        plane_count = 2;
        if (req->pitches[0] < req->width ||
            req->pitches[1] < req->width ||
            req->offsets[1] == 0 ||
            (req->handles[1] != 0 &&
             req->handles[1] != req->handles[0]))
            return -EINVAL;
    } else if (req->pitches[0] < req->width * 4) {
        return -EINVAL;
    }
    for (uint32 i = plane_count; i < 4; i++) {
        if (req->handles[i] != 0 || req->pitches[i] != 0 ||
            req->offsets[i] != 0 || req->modifier[i] != 0)
            return -EINVAL;
    }
    if ((req->flags & DRM_MODE_FB_MODIFIERS) &&
        req->modifier[0] != DRM_FORMAT_MOD_LINEAR)
        return -EINVAL;
    for (uint32 i = 1; i < plane_count; i++) {
        if (req->modifier[i] != 0 &&
            req->modifier[i] != req->modifier[0])
            return -EINVAL;
    }
    min_size = (uint64)req->pitches[0] * req->height + req->offsets[0];
    if (req->pixel_format == DRM_FORMAT_NV12) {
        uint64 uv_size =
            (uint64)req->pitches[1] * ((req->height + 1) / 2) +
            req->offsets[1];
        if (uv_size > min_size)
            min_size = uv_size;
    }
    if (min_size < req->offsets[0])
        return -EINVAL;

    bo = fb_bo_get_owned(req->handles[0], owner->id, owner->tgid);
    if (bo == NULL)
        return -ENOENT;
    if (bo->size < min_size) {
        fb_bo_put(bo);
        return -EINVAL;
    }

    spin_lock(&fb_state.lock);
    for (uint32 i = 0; i < FB_GPU_MAX_KMS_FBS; i++) {
        struct fb_gpu_kms_fb_entry *fb = &fb_state.kms_fbs[i];

        if (fb->in_use)
            continue;
        ret = fb_ttm_pin_locked(bo, owner->id, owner->tgid);
        if (ret != 0)
            break;
        fb_ttm_resv_record_shared_locked(bo, FB_GPU_RESV_ATTACH_KMS_PIN,
                                         FB_GPU_DMABUF_TAG_NONE);
        fb_id = fb_state.next_kms_fb_id++;
        if (fb_state.next_kms_fb_id == 0)
            fb_state.next_kms_fb_id = 100;
        memset(fb, 0, sizeof(*fb));
        fb->in_use = 1;
        fb->fb_id = fb_id;
        fb->bo_handle = req->handles[0];
        for (uint32 p = 0; p < plane_count; p++) {
            fb->bo_handles[p] = req->handles[p] != 0 ?
                req->handles[p] : req->handles[0];
            fb->pitches[p] = req->pitches[p];
            fb->offsets[p] = req->offsets[p];
        }
        fb->owner_id = owner->id;
        fb->owner_tgid = owner->tgid;
        fb->width = req->width;
        fb->height = req->height;
        fb->pitch = req->pitches[0];
        fb->plane_count = plane_count;
        fb->pixel_format = req->pixel_format;
        fb->modifier = (req->flags & DRM_MODE_FB_MODIFIERS) ?
            req->modifier[0] : DRM_FORMAT_MOD_LINEAR;
        if (bo->gem != NULL) {
            bo->gem->metadata.format = req->pixel_format;
            bo->gem->metadata.modifier =
                (req->flags & DRM_MODE_FB_MODIFIERS) ?
                    req->modifier[0] : DRM_FORMAT_MOD_LINEAR;
            bo->gem->metadata.plane_count = plane_count;
            for (uint32 p = 0; p < plane_count; p++) {
                bo->gem->metadata.offsets[p] = req->offsets[p];
                bo->gem->metadata.strides[p] = req->pitches[p];
            }
        }
        fb_state.stats.kms_framebuffers++;
        break;
    }
    spin_unlock(&fb_state.lock);
    fb_bo_put(bo);
    if (ret != 0)
        return ret;
    if (fb_id == 0)
        return -ENOSPC;

    req->fb_id = fb_id;
    return ret;
}

static int gpu_drm_mode_addfb2(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_mode_fb_cmd2_compat req;
    int ret;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    ret = gpu_drm_mode_addfb2_req(owner, &req);
    if (ret != 0)
        return ret;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_mode_addfb(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_mode_fb_cmd_compat legacy;
    struct drm_mode_fb_cmd2_compat req;
    int ret;

    if (either_copyin(&legacy, 1, arg, sizeof(legacy)) < 0)
        return -EFAULT;
    if (legacy.bpp != 32)
        return -EINVAL;
    memset(&req, 0, sizeof(req));
    req.width = legacy.width;
    req.height = legacy.height;
    req.handles[0] = legacy.handle;
    req.pitches[0] = legacy.pitch;
    if (legacy.depth == 24)
        req.pixel_format = DRM_FORMAT_XRGB8888;
    else if (legacy.depth == 32)
        req.pixel_format = DRM_FORMAT_ARGB8888;
    else
        return -EINVAL;

    ret = gpu_drm_mode_addfb2_req(owner, &req);
    if (ret != 0)
        return ret;
    legacy.fb_id = req.fb_id;
    if (either_copyout(1, arg, &legacy, sizeof(legacy)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_mode_rmfb(struct fb_gpu_render_owner *owner, uint64 arg)
{
    uint32 fb_id;
    int ret = -ENOENT;

    if (!gpu_drm_is_primary_like(owner))
        return -EOPNOTSUPP;
    if (either_copyin(&fb_id, 1, arg, sizeof(fb_id)) < 0)
        return -EFAULT;

    spin_lock(&fb_state.lock);
    for (uint32 i = 0; i < FB_GPU_MAX_KMS_FBS; i++) {
        struct fb_gpu_kms_fb_entry *fb = &fb_state.kms_fbs[i];

        if (fb->fb_id != fb_id || !gpu_kms_fb_owner_matches(fb, owner))
            continue;
        if (fb_state.current_kms_fb_id == fb_id)
            fb_state.current_kms_fb_id = 0;
        if (fb_state.current_cursor_fb_id == fb_id) {
            fb_state.current_cursor_fb_id = 0;
            fb_state.current_cursor_visible = 0;
        }
        gpu_kms_unpin_bo_locked(fb->bo_handle, owner->id, owner->tgid);
        memset(fb, 0, sizeof(*fb));
        if (fb_state.stats.kms_framebuffers > 0)
            fb_state.stats.kms_framebuffers--;
        ret = 0;
        break;
    }
    spin_unlock(&fb_state.lock);
    return ret;
}

static int gpu_drm_mode_closefb(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_mode_closefb_compat req;
    int ret = -ENOENT;

    if (!gpu_drm_is_primary_like(owner))
        return -EOPNOTSUPP;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.pad != 0)
        return -EINVAL;

    spin_lock(&fb_state.lock);
    if (fb_state.current_kms_fb_id == req.fb_id) {
        spin_unlock(&fb_state.lock);
        return -EBUSY;
    }
    for (uint32 i = 0; i < FB_GPU_MAX_KMS_FBS; i++) {
        struct fb_gpu_kms_fb_entry *fb = &fb_state.kms_fbs[i];

        if (fb->fb_id != req.fb_id || !gpu_kms_fb_owner_matches(fb, owner))
            continue;
        if (fb_state.current_cursor_fb_id == req.fb_id) {
            fb_state.current_cursor_fb_id = 0;
            fb_state.current_cursor_visible = 0;
        }
        gpu_kms_unpin_bo_locked(fb->bo_handle, owner->id, owner->tgid);
        memset(fb, 0, sizeof(*fb));
        if (fb_state.stats.kms_framebuffers > 0)
            fb_state.stats.kms_framebuffers--;
        ret = 0;
        break;
    }
    spin_unlock(&fb_state.lock);
    return ret;
}

static void gpu_drm_lease_note_reject_locked(uint64 cmd,
                                             const void *req,
                                             enum drm_core_node_type node_type)
{
    fb_state.stats.drm_lease_ioctl_attempts++;
    if (node_type == DRM_CORE_NODE_RENDER)
        fb_state.stats.drm_lease_render_rejects++;

    switch (cmd) {
    case DRM_IOCTL_MODE_CREATE_LEASE: {
        const struct drm_mode_create_lease_compat *create = req;
        fb_state.stats.drm_lease_create_rejects++;
        if (create != NULL && create->object_count != 0)
            fb_state.stats.drm_lease_create_object_rejects++;
        else
            fb_state.stats.drm_lease_create_empty_rejects++;
        break;
    }
    case DRM_IOCTL_MODE_LIST_LESSEES:
        fb_state.stats.drm_lease_list_rejects++;
        break;
    case DRM_IOCTL_MODE_GET_LEASE:
        fb_state.stats.drm_lease_get_rejects++;
        break;
    case DRM_IOCTL_MODE_REVOKE_LEASE:
        fb_state.stats.drm_lease_revoke_rejects++;
        break;
    default:
        break;
    }
}

static int gpu_drm_mode_lease_fail_closed(struct fb_gpu_render_owner *owner,
                                          uint64 cmd, uint64 arg)
{
    struct drm_mode_create_lease_compat create;
    struct drm_mode_list_lessees_compat list;
    struct drm_mode_get_lease_compat get;
    struct drm_mode_revoke_lease_compat revoke;
    const void *diag_req = NULL;
    enum drm_core_node_type node_type;

    if (owner == NULL)
        return -EBADF;
    node_type = owner->drm.node_type;

    switch (cmd) {
    case DRM_IOCTL_MODE_CREATE_LEASE:
        if (either_copyin(&create, 1, arg, sizeof(create)) < 0)
            return -EFAULT;
        if (create.object_count != 0 && create.object_ids == 0)
            return -EINVAL;
        diag_req = &create;
        break;
    case DRM_IOCTL_MODE_LIST_LESSEES:
        if (either_copyin(&list, 1, arg, sizeof(list)) < 0)
            return -EFAULT;
        if (list.count_lessees != 0 && list.lessees_ptr == 0)
            return -EINVAL;
        diag_req = &list;
        break;
    case DRM_IOCTL_MODE_GET_LEASE:
        if (either_copyin(&get, 1, arg, sizeof(get)) < 0)
            return -EFAULT;
        if (get.count_objects != 0 && get.objects_ptr == 0)
            return -EINVAL;
        diag_req = &get;
        break;
    case DRM_IOCTL_MODE_REVOKE_LEASE:
        if (either_copyin(&revoke, 1, arg, sizeof(revoke)) < 0)
            return -EFAULT;
        diag_req = &revoke;
        break;
    default:
        return -EINVAL;
    }

    spin_lock(&fb_state.lock);
    gpu_drm_lease_note_reject_locked(cmd, diag_req, node_type);
    spin_unlock(&fb_state.lock);
    return -EOPNOTSUPP;
}

struct gpu_kms_mode_pixels {
    struct fb_gpu_bo_entry *bo;
    struct fb_gpu_kms_fb_entry fb;
};

static int gpu_kms_mode_fill(void *opaque, void *dst, uint32 pitch)
{
    struct gpu_kms_mode_pixels *pixels = opaque;
    struct fb_gpu_bo_entry *bo = pixels->bo;
    struct fb_gpu_kms_fb_entry *fb = &pixels->fb;
    int swap_rb = fb_scanout_format_needs_rb_swap(fb->pixel_format);

    /* Page presence, bounds and all formats were validated before the driver
     * changes anything. The BO reference pins the page array across this copy. */
    for (uint32 row = 0; row < fb->height; row++) {
        uint64 off = fb->offsets[0] + (uint64)row * fb->pitch;
        uint32 remain = fb->width * 4;
        uint8 *target = (uint8 *)dst + (uint64)row * pitch;

        while (remain != 0) {
            uint32 page_index = off / PGSIZE;
            uint32 page_offset = off & (PGSIZE - 1);
            uint32 chunk = PGSIZE - page_offset;
            uint8 *src;

            if (chunk > remain)
                chunk = remain;
            src = (uint8 *)PA2VA(__page_to_pa(bo->pages[page_index])) +
                  page_offset;
            fb_copy_scanout_chunk(target, src, chunk, swap_rb);
            target += chunk;
            off += chunk;
            remain -= chunk;
        }
    }
    return 0;
}

static int gpu_kms_mode_fb_get(struct fb_gpu_render_owner *owner,
                                uint32 fb_id,
                                const struct drm_mode_modeinfo_compat *mode,
                                struct gpu_kms_mode_pixels *pixels)
{
    struct fb_gpu_bo_entry *bo;
    uint64 end;
    int ret;

    memset(pixels, 0, sizeof(*pixels));
    if (!virtio_gpu_scanout_mode_supported(mode->hdisplay, mode->vdisplay))
        return -EOPNOTSUPP;
    ret = gpu_kms_copy_fb_for_owner(owner, fb_id, &pixels->fb);
    if (ret != 0)
        return ret;
    if (pixels->fb.width != mode->hdisplay ||
        pixels->fb.height != mode->vdisplay ||
        !gpu_kms_primary_scanout_format_supported(pixels->fb.pixel_format,
                                                   pixels->fb.modifier) ||
        pixels->fb.pitch < pixels->fb.width * 4 ||
        (pixels->fb.pitch & 3) != 0 || (pixels->fb.offsets[0] & 3) != 0)
        return -EINVAL;
    bo = fb_bo_get_owned(pixels->fb.bo_handle, owner->id, owner->tgid);
    if (bo == NULL)
        return -ENOENT;
    end = pixels->fb.offsets[0] +
          (uint64)(pixels->fb.height - 1) * pixels->fb.pitch +
          (uint64)pixels->fb.width * 4;
    if (end > bo->size) {
        ret = -EINVAL;
        goto fail;
    }
    if (bo->virtio_resource_id != 0) {
        if (bo->width != pixels->fb.width || bo->height != pixels->fb.height ||
            bo->pitch != pixels->fb.pitch || pixels->fb.offsets[0] != 0 ||
            fb_scanout_format_needs_rb_swap(pixels->fb.pixel_format)) {
            ret = -EOPNOTSUPP;
            goto fail;
        }
    } else {
        if (bo->pages == NULL || end > (uint64)bo->npages * PGSIZE) {
            ret = -EINVAL;
            goto fail;
        }
        for (uint64 i = pixels->fb.offsets[0] / PGSIZE;
             i < (end + PGSIZE - 1) / PGSIZE; i++) {
            if (bo->pages[i] == NULL) {
                ret = -EINVAL;
                goto fail;
            }
        }
    }
    pixels->bo = bo;
    return 0;
fail:
    fb_bo_put(bo);
    return ret;
}

static int gpu_kms_modeset_fb(struct fb_gpu_render_owner *owner, uint32 fb_id,
                               const struct drm_mode_modeinfo_compat *mode,
                               int test_only)
{
    struct gpu_kms_mode_pixels pixels;
    int ret = gpu_kms_mode_fb_get(owner, fb_id, mode, &pixels);

    if (ret != 0)
        return ret;
    if (!test_only) {
        ret = virtio_gpu_modeset_scanout(mode->hdisplay, mode->vdisplay,
                pixels.bo->virtio_resource_id,
                pixels.bo->virtio_resource_id != 0 ? NULL : gpu_kms_mode_fill,
                &pixels);
        if (ret == 0) {
            spin_lock(&fb_state.lock);
            fb_state.stats.bo_presents++;
            if (pixels.bo->virtio_resource_id != 0) {
                fb_state.stats.virgl_bo_presents++;
                fb_state.stats.virgl_bo_present_pixels +=
                    (uint64)mode->hdisplay * mode->vdisplay;
                fb_state.stats.virgl_bo_present_last_resource =
                    pixels.bo->virtio_resource_id;
            }
            fb_note_display_complete_locked();
            (void)fb_bo_signal_present_locked(pixels.bo);
            spin_unlock(&fb_state.lock);
        }
    }
    fb_bo_put(pixels.bo);
    return ret;
}

static void gpu_kms_primary_snapshot_locked(struct gpu_kms_primary_state *state)
{
    memset(state, 0, sizeof(*state));
    if (fb_state.kms_primary_valid)
        *state = fb_state.kms_primary;
    else {
        state->src_w = (uint64)fb_state.xres << 16;
        state->src_h = (uint64)fb_state.yres << 16;
        state->crtc_w = fb_state.xres;
        state->crtc_h = fb_state.yres;
    }
    state->fb_id = fb_state.current_kms_fb_id;
    state->crtc_id = state->fb_id != 0 ? GPU_DRM_CRTC_ID : 0;
    state->touched = 0;
}

static int gpu_kms_primary_validate(struct fb_gpu_render_owner *owner,
                                    struct gpu_kms_primary_state *state,
                                    const struct drm_mode_modeinfo_compat *mode,
                                    int changing_mode)
{
    struct fb_gpu_kms_fb_entry fb;
    int ret;

    if (state->fb_id == 0)
        return state->crtc_id == 0 ? 0 : -EINVAL;
    if (state->crtc_id != GPU_DRM_CRTC_ID)
        return -EINVAL;
    ret = gpu_kms_copy_fb_for_owner(owner, state->fb_id, &fb);
    if (ret != 0)
        return ret;
    /* Preserve the existing same-mode small-framebuffer compatibility path.
     * Real modesets need a complete frame; explicit hardware crop/scaling is
     * rejected because the backend does not implement it. */
    if (state->touched || changing_mode) {
        if (state->src_x != 0 || state->src_y != 0 ||
            state->crtc_x != 0 || state->crtc_y != 0 ||
            state->src_w != ((uint64)fb.width << 16) ||
            state->src_h != ((uint64)fb.height << 16) ||
            state->crtc_w != fb.width || state->crtc_h != fb.height ||
            fb.width > mode->hdisplay || fb.height > mode->vdisplay)
            return -EINVAL;
    }
    if (changing_mode &&
        (fb.width != mode->hdisplay || fb.height != mode->vdisplay))
        return -EINVAL;
    return 0;
}

static int gpu_drm_mode_setcrtc(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_mode_crtc_compat req;
    struct drm_mode_modeinfo_compat mode, active;
    struct gpu_kms_primary_state primary;
    uint32 connector;
    int changing_mode;
    int ret;

    if (!gpu_drm_is_primary_like(owner))
        return -EOPNOTSUPP;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.crtc_id != GPU_DRM_CRTC_ID)
        return -ENOENT;
    if (req.count_connectors > 1 || req.x != 0 || req.y != 0 ||
        req.mode_valid > 1)
        return -EINVAL;
    if (req.count_connectors == 1) {
        if (either_copyin(&connector, 1, req.set_connectors_ptr,
                          sizeof(connector)) < 0)
            return -EFAULT;
        if (connector != GPU_DRM_CONNECTOR_ID)
            return -EINVAL;
    }
    gpu_drm_fill_mode(&active);
    mode = active;
    if (req.mode_valid && gpu_drm_mode_supported(&req.mode, &mode) != 0)
        return -EINVAL;
    changing_mode = !gpu_drm_mode_timings_equal(&mode, &active);
    if (changing_mode && (req.fb_id == 0 || req.count_connectors != 1))
        return -EINVAL;
    if (gpu_kms_modeset_busy())
        return -EBUSY;
    ret = gpu_kms_fb_presentable_for_owner(owner, req.fb_id);
    if (ret != 0)
        return ret;
    if (changing_mode)
        ret = gpu_kms_modeset_fb(owner, req.fb_id, &mode, 0);
    else
        ret = gpu_kms_present_fb(owner, req.fb_id);
    if (ret != 0)
        return ret;
    memset(&primary, 0, sizeof(primary));
    primary.crtc_id = req.fb_id != 0 ? GPU_DRM_CRTC_ID : 0;
    primary.fb_id = req.fb_id;
    primary.src_w = (uint64)mode.hdisplay << 16;
    primary.src_h = (uint64)mode.vdisplay << 16;
    primary.crtc_w = mode.hdisplay;
    primary.crtc_h = mode.vdisplay;
    spin_lock(&fb_state.lock);
    fb_state.current_kms_fb_id = req.fb_id;
    fb_state.kms_mode = mode;
    fb_state.kms_mode_valid = 1;
    fb_state.kms_primary = primary;
    fb_state.kms_primary_valid = 1;
    spin_unlock(&fb_state.lock);
    return 0;
}

static int gpu_drm_mode_page_flip(struct fb_gpu_render_owner *owner,
                                  uint64 arg)
{
    struct drm_mode_crtc_page_flip_compat req;
    int ret;

    if (!gpu_drm_is_primary_like(owner))
        return -EOPNOTSUPP;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.crtc_id != GPU_DRM_CRTC_ID || req.reserved != 0 ||
        (req.flags & ~DRM_MODE_PAGE_FLIP_FLAGS) != 0 ||
        ((req.flags & DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE) &&
         (req.flags & DRM_MODE_PAGE_FLIP_TARGET_RELATIVE)))
        return -EINVAL;
    if ((req.flags & (DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE |
                      DRM_MODE_PAGE_FLIP_TARGET_RELATIVE)) != 0) {
        spin_lock(&fb_state.lock);
        fb_state.stats.kms_page_flip_target_rejects++;
        spin_unlock(&fb_state.lock);
        return -EOPNOTSUPP;
    }
    if ((req.flags & DRM_MODE_PAGE_FLIP_ASYNC) != 0) {
        spin_lock(&fb_state.lock);
        fb_state.stats.kms_page_flip_async_rejects++;
        spin_unlock(&fb_state.lock);
        return -EOPNOTSUPP;
    }
    ret = gpu_kms_fb_presentable_for_owner(owner, req.fb_id);
    if (ret != 0) {
        if (ret == -ENOENT) {
            spin_lock(&fb_state.lock);
            fb_state.stats.kms_page_flip_invalid_noevent_rejects++;
            spin_unlock(&fb_state.lock);
        }
        return ret;
    }

    if ((req.flags & DRM_MODE_PAGE_FLIP_EVENT) != 0) {
        spin_lock(&fb_state.lock);
        if (owner->drm_event_count >= FB_GPU_DRM_EVENT_QUEUE_CAPACITY) {
            fb_state.stats.drm_event_queue_overflows++;
            fb_state.stats.drm_event_queue_dropped++;
            spin_unlock(&fb_state.lock);
            return -EAGAIN;
        }
        spin_unlock(&fb_state.lock);
    }

    /*
     * Gate: virtio_gpu_async_present (DEFAULT OFF). Only event-carrying flips
     * go async (the completion event is the whole point); the -EAGAIN ring
     * guard above already ran, as legacy+U5 require. A return of 1 means the
     * present+completion is now owned by the async worker (return success); a
     * negative return (-EBUSY for an overlapping flip) propagates; 0 falls
     * through to the byte-for-byte synchronous present below (gate OFF path).
     */
    if ((req.flags & DRM_MODE_PAGE_FLIP_EVENT) != 0) {
        int async_ret =
            gpu_kms_page_flip_async_try(owner, req.user_data, req.fb_id);

        if (async_ret > 0)
            return 0;
        if (async_ret < 0)
            return async_ret;
    }

    ret = gpu_kms_present_fb(owner, req.fb_id);
    if (ret != 0)
        return ret;

    /*
     * Gate: virtio_gpu_vblank_paced_flip (DEFAULT OFF). When enabled, the
     * present has already happened above; defer only the DRM_MODE_PAGE_FLIP
     * completion event to the next synthetic vblank edge so kwin's legacy
     * RenderLoop gets a phase-accurate clock anchor. gpu_drm_page_flip_paced()
     * returns 0 once the event is handled; a negative return (allocation
     * failure) falls through to the unchanged synchronous path below.
     */
    if (gpu_kms_vblank_paced_flip_enabled() &&
        (req.flags & DRM_MODE_PAGE_FLIP_EVENT) != 0 &&
        gpu_drm_page_flip_paced(owner, req.user_data, req.fb_id) == 0)
        return 0;

    spin_lock(&fb_state.lock);
    {
        uint64 sequence;
        uint64 timestamp_ns;
        int queued_event = 0;

        gpu_kms_sample_vblank_locked(0, &sequence, &timestamp_ns);
        fb_state.stats.kms_vblank_page_flip_events++;
        if (fb_state.stats.kms_present_last_lane ==
            FB_GPU_KMS_PRESENT_LANE_NOUVEAU_HW)
            fb_state.stats.kms_page_flip_events_native_hw++;
        else
            fb_state.stats.kms_page_flip_events_software_blit++;

        if ((req.flags & DRM_MODE_PAGE_FLIP_EVENT) != 0)
            ret = gpu_drm_event_queue_locked(owner,
                                             DRM_EVENT_FLIP_COMPLETE,
                                             req.user_data, sequence,
                                             GPU_DRM_CRTC_ID,
                                             timestamp_ns);
        if (ret == 0 && (req.flags & DRM_MODE_PAGE_FLIP_EVENT) != 0)
            queued_event = 1;
        if (ret != 0) {
            spin_unlock(&fb_state.lock);
            return ret;
        }
        fb_state.current_kms_fb_id = req.fb_id;
        fb_state.stats.kms_page_flips++;
        if (queued_event) {
            spin_unlock(&fb_state.lock);
            gpu_drm_event_notify_read(owner);
            return 0;
        }
    }
    spin_unlock(&fb_state.lock);
    return 0;
}

static int gpu_drm_mode_atomic(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_mode_atomic_compat req;
    uint32 obj_ids[8];
    uint32 prop_counts[8];
    uint32 atomic_props[64];
    uint64 atomic_values[64];
    uint32 new_fb = 0;
    uint32 total_props = 0;
    uint64 out_fence_ptr = 0;
    int32 in_fence_fds[GPU_KMS_MAX_IN_FENCES];
    struct vfs_file *in_fence_files[GPU_KMS_MAX_IN_FENCES];
    uint32 in_fence_count = 0;
    uint32 in_fence_ref_count = 0;
    int has_new_fb = 0;
    int32 out_fence_fd = -1;
    struct gpu_kms_prepared_out_fence prepared_out_fence;
    struct gpu_kms_cursor_atomic_state cursor_state, old_cursor;
    struct gpu_kms_primary_state primary;
    struct drm_mode_modeinfo_compat active_mode, selected_mode;
    int changing_mode = 0;
    int cursor_applied = 0;
    uint64 proposed_mode_id;
    uint32 proposed_active;
    uint32 old_active;
    int primary_crtc_supplied = 0;
    uint32 proposed_primary_fb;
    int ret = 0;

    memset(in_fence_files, 0, sizeof(in_fence_files));
    gpu_kms_init_prepared_out_fence(&prepared_out_fence);
    memset(&cursor_state, 0, sizeof(cursor_state));
    if (!gpu_drm_is_primary_like(owner))
        return -EOPNOTSUPP;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if ((req.flags & ~DRM_MODE_ATOMIC_FLAGS) != 0 || req.reserved != 0)
        return -EINVAL;
    if (req.count_objs > sizeof(obj_ids) / sizeof(obj_ids[0]))
        return -EINVAL;
    if (req.count_objs != 0 &&
        (req.objs_ptr == 0 || req.count_props_ptr == 0 ||
         req.props_ptr == 0 || req.prop_values_ptr == 0))
        return -EINVAL;
    for (uint32 i = 0; i < req.count_objs; i++) {
        if (either_copyin(&obj_ids[i], 1,
                          req.objs_ptr + (uint64)i * sizeof(uint32),
                          sizeof(uint32)) < 0 ||
            either_copyin(&prop_counts[i], 1,
                          req.count_props_ptr + (uint64)i * sizeof(uint32),
                          sizeof(uint32)) < 0)
            return -EFAULT;
        if (prop_counts[i] > 16 || total_props + prop_counts[i] > 64)
            return -EINVAL;
        total_props += prop_counts[i];
    }
    for (uint32 i = 0; i < total_props; i++) {
        if (either_copyin(&atomic_props[i], 1,
                          req.props_ptr + (uint64)i * sizeof(uint32),
                          sizeof(atomic_props[i])) < 0 ||
            either_copyin(&atomic_values[i], 1,
                          req.prop_values_ptr + (uint64)i * sizeof(uint64),
                          sizeof(atomic_values[i])) < 0)
            return -EFAULT;
    }

    for (uint32 i = 0; i < total_props; i++) {
        if (atomic_props[i] == GPU_DRM_PROP_MODE_ID) {
            ret = gpu_drm_validate_mode_blob(atomic_values[i]);
            if (ret != 0)
                return ret;
        }
    }

    gpu_drm_fill_mode(&active_mode);
    selected_mode = active_mode;
    spin_lock(&fb_state.lock);
    gpu_kms_cursor_atomic_snapshot_locked(&cursor_state);
    old_cursor = cursor_state;
    old_cursor.touched = 1;
    old_cursor.fb_touched = 1;
    gpu_kms_primary_snapshot_locked(&primary);
    proposed_active = fb_state.current_kms_fb_id != 0;
    old_active = proposed_active;
    proposed_mode_id = proposed_active ? GPU_DRM_MODE_BLOB_ID : 0;
    proposed_primary_fb = fb_state.current_kms_fb_id;
    total_props = 0;
    for (uint32 i = 0; i < req.count_objs && ret == 0; i++) {
        for (uint32 j = 0; j < prop_counts[i]; j++) {
            uint32 prop = atomic_props[total_props + j];
            uint64 value = atomic_values[total_props + j];

            ret = gpu_kms_validate_prop_locked(owner, obj_ids[i],
                                               DRM_MODE_OBJECT_ANY,
                                               prop, value,
                                               &new_fb, &has_new_fb,
                                               &out_fence_ptr,
                                               in_fence_fds,
                                               &in_fence_count,
                                               GPU_KMS_MAX_IN_FENCES,
                                               &cursor_state);
            if (ret != 0)
                break;
            if (obj_ids[i] == GPU_DRM_CRTC_ID) {
                if (prop == GPU_DRM_PROP_ACTIVE)
                    proposed_active = (uint32)value;
                else if (prop == GPU_DRM_PROP_MODE_ID)
                    proposed_mode_id = value;
            } else if (obj_ids[i] == GPU_DRM_PRIMARY_PLANE_ID) {
                switch (prop) {
                case GPU_DRM_PROP_FB_ID:
                    proposed_primary_fb = (uint32)value;
                    primary.fb_id = (uint32)value;
                    break;
                case GPU_DRM_PROP_CRTC_ID:
                    primary.crtc_id = (uint32)value;
                    primary_crtc_supplied = 1;
                    break;
                case GPU_DRM_PROP_SRC_X:
                    primary.src_x = value;
                    primary.touched = 1;
                    break;
                case GPU_DRM_PROP_SRC_Y:
                    primary.src_y = value;
                    primary.touched = 1;
                    break;
                case GPU_DRM_PROP_SRC_W:
                    primary.src_w = value;
                    primary.touched = 1;
                    break;
                case GPU_DRM_PROP_SRC_H:
                    primary.src_h = value;
                    primary.touched = 1;
                    break;
                case GPU_DRM_PROP_CRTC_X:
                    primary.crtc_x = (int64)value;
                    primary.touched = 1;
                    break;
                case GPU_DRM_PROP_CRTC_Y:
                    primary.crtc_y = (int64)value;
                    primary.touched = 1;
                    break;
                case GPU_DRM_PROP_CRTC_W:
                    primary.crtc_w = value;
                    primary.touched = 1;
                    break;
                case GPU_DRM_PROP_CRTC_H:
                    primary.crtc_h = value;
                    primary.touched = 1;
                    break;
                default: break;
                }
            }
        }
        total_props += prop_counts[i];
    }
    if (ret == 0 &&
        ((proposed_active != 0) != (proposed_mode_id != 0) ||
         (proposed_active != 0) != (proposed_primary_fb != 0)))
        ret = -EINVAL;
    spin_unlock(&fb_state.lock);
    if (ret != 0)
        return ret;
    if (proposed_mode_id != 0) {
        ret = gpu_drm_resolve_mode_blob(proposed_mode_id, &selected_mode);
        if (ret != 0)
            return ret;
    }
    changing_mode = proposed_active != 0 &&
        !gpu_drm_mode_timings_equal(&selected_mode, &active_mode);
    if ((changing_mode || proposed_active != old_active) &&
        (req.flags & DRM_MODE_ATOMIC_ALLOW_MODESET) == 0)
        return -EINVAL;
    if (changing_mode && (req.flags & DRM_MODE_ATOMIC_TEST_ONLY) == 0 &&
        gpu_kms_modeset_busy())
        return -EBUSY;
    /* Preserve the existing unsupported-framebuffer failure contract before
     * the stricter geometry checks: invalidate the requested out-fence slot
     * without acquiring input fences or preparing/exporting an output fence. */
    if (has_new_fb) {
        ret = gpu_kms_fb_presentable_for_owner(owner, new_fb);
        if (ret != 0) {
            if (out_fence_ptr != 0) {
                int32 failed_fd = -1;

                (void)either_copyout(1, out_fence_ptr, &failed_fd,
                                     sizeof(failed_fd));
            }
            return ret;
        }
    }
    primary.fb_id = proposed_primary_fb;
    if (!proposed_active && !primary_crtc_supplied)
        primary.crtc_id = 0;
    /* The legacy small-buffer tests may enable a plane using only FB_ID.
     * An enabled CRTC is the sole possible routing on this device. */
    else if (proposed_active && primary.crtc_id == 0 &&
             !primary_crtc_supplied)
        primary.crtc_id = GPU_DRM_CRTC_ID;
    ret = gpu_kms_primary_validate(owner, &primary, &selected_mode,
                                   changing_mode);
    if (ret != 0)
        return ret;
    if (changing_mode) {
        ret = gpu_kms_modeset_fb(owner, proposed_primary_fb, &selected_mode, 1);
        if (ret != 0)
            return ret;
        new_fb = proposed_primary_fb;
        has_new_fb = 1;
    }

    for (uint32 i = 0; i < in_fence_count; i++) {
        ret = gpu_kms_get_in_fence_file_ref(in_fence_fds[i],
                                            &in_fence_files[i]);
        if (ret != 0) {
            spin_lock(&fb_state.lock);
            fb_state.stats.kms_atomic_in_fence_rejected++;
            spin_unlock(&fb_state.lock);
            gpu_kms_put_in_fence_file_refs(in_fence_files,
                                           in_fence_ref_count);
            return ret;
        }
        in_fence_ref_count++;
    }

    /*
     * Compatibility path: accept KWin's ordinary unfenced NONBLOCK flips, but
     * complete the software present synchronously below.  This is not true
     * asynchronous DRM NONBLOCK semantics; a real input fence could sleep and
     * remains rejected until a deferred atomic worker can retain the BO,
     * owner, event, and fence state through completion.
     */
    if ((req.flags & DRM_MODE_ATOMIC_NONBLOCK) != 0 &&
        (req.flags & DRM_MODE_ATOMIC_TEST_ONLY) == 0 &&
        in_fence_count != 0) {
        spin_lock(&fb_state.lock);
        fb_state.stats.kms_atomic_nonblock_rejects++;
        spin_unlock(&fb_state.lock);
        gpu_kms_put_in_fence_file_refs(in_fence_files,
                                       in_fence_ref_count);
        return -EOPNOTSUPP;
    }

    /*
     * Event-ring backpressure, mirroring the legacy page-flip path
     * (gpu_drm_mode_page_flip above): when the caller asked for a
     * flip-complete event but its per-file event ring is already full, fail
     * with -EAGAIN *before* any irreversible commit work (in-fence wait,
     * cursor apply, out-fence arm, present), so a full ring can never silently
     * drop the completion the client is blocking on. Only a real
     * (non-TEST_ONLY) commit that flips the sole CRTC's scanout (has_new_fb)
     * will emit an event, so the guard is scoped to that case; the ring
     * capacity is checked under fb_state.lock exactly as the legacy path does.
     */
    if ((req.flags & DRM_MODE_ATOMIC_TEST_ONLY) == 0 &&
        (req.flags & DRM_MODE_PAGE_FLIP_EVENT) != 0 && has_new_fb) {
        spin_lock(&fb_state.lock);
        if (owner->drm_event_count >= FB_GPU_DRM_EVENT_QUEUE_CAPACITY) {
            fb_state.stats.drm_event_queue_overflows++;
            fb_state.stats.drm_event_queue_dropped++;
            spin_unlock(&fb_state.lock);
            gpu_kms_put_in_fence_file_refs(in_fence_files, in_fence_ref_count);
            return -EAGAIN;
        }
        spin_unlock(&fb_state.lock);
    }

    if ((req.flags & DRM_MODE_ATOMIC_TEST_ONLY) != 0 && out_fence_ptr != 0) {
        int32 fence_fd = -1;

        if (either_copyout(1, out_fence_ptr, &fence_fd,
                           sizeof(fence_fd)) < 0) {
            gpu_kms_put_in_fence_file_refs(in_fence_files,
                                           in_fence_ref_count);
            return -EFAULT;
        }
        spin_lock(&fb_state.lock);
        fb_state.stats.kms_atomic_out_fence_test_only_placeholders++;
        spin_unlock(&fb_state.lock);
    }

    for (uint32 i = 0; i < in_fence_count; i++) {
        if ((req.flags & DRM_MODE_ATOMIC_TEST_ONLY) != 0)
            ret = gpu_kms_validate_test_only_in_fence_file(
                owner, in_fence_files[i]);
        else
            ret = gpu_kms_wait_in_fence_file(owner, in_fence_files[i]);
        if (ret != 0) {
            gpu_kms_put_in_fence_file_refs(in_fence_files,
                                           in_fence_ref_count);
            return ret;
        }
    }

    if ((req.flags & DRM_MODE_ATOMIC_TEST_ONLY) == 0 && out_fence_ptr != 0) {
        out_fence_fd = gpu_kms_export_out_fence_fd(&prepared_out_fence,
                                                   has_new_fb);
        if (out_fence_fd < 0) {
            gpu_kms_put_in_fence_file_refs(in_fence_files,
                                           in_fence_ref_count);
            return out_fence_fd;
        }
        spin_lock(&fb_state.lock);
        fb_state.stats.kms_atomic_out_fence_prepared++;
        spin_unlock(&fb_state.lock);
        if (either_copyout(1, out_fence_ptr, &out_fence_fd,
                           sizeof(out_fence_fd)) < 0) {
            gpu_kms_cleanup_prepared_out_fence(&prepared_out_fence);
            gpu_kms_put_in_fence_file_refs(in_fence_files,
                                           in_fence_ref_count);
            return -EFAULT;
        }
        ret = gpu_kms_arm_prepared_out_fence(&prepared_out_fence);
        if (ret != 0) {
            int32 failed_fd = -1;

            gpu_kms_cleanup_prepared_out_fence(&prepared_out_fence);
            gpu_kms_put_in_fence_file_refs(in_fence_files,
                                           in_fence_ref_count);
            (void)either_copyout(1, out_fence_ptr, &failed_fd,
                                 sizeof(failed_fd));
            return ret;
        }
    }

    spin_lock(&fb_state.lock);
    if ((req.flags & DRM_MODE_ATOMIC_TEST_ONLY) == 0) {
        if (has_new_fb) {
            if (new_fb != 0 &&
                !gpu_kms_fb_owner_matches(gpu_kms_fb_lookup_locked(new_fb),
                                          owner)) {
                spin_unlock(&fb_state.lock);
                gpu_kms_cancel_prepared_out_fence(&prepared_out_fence,
                                                  -ENOENT);
                gpu_kms_cleanup_prepared_out_fence(&prepared_out_fence);
                gpu_kms_put_in_fence_file_refs(in_fence_files,
                                               in_fence_ref_count);
                if (out_fence_ptr != 0) {
                    int32 failed_fd = -1;

                    (void)either_copyout(1, out_fence_ptr, &failed_fd,
                                         sizeof(failed_fd));
                }
                return -ENOENT;
            }
        }
    }
    spin_unlock(&fb_state.lock);
    if ((req.flags & DRM_MODE_ATOMIC_TEST_ONLY) == 0 &&
        cursor_state.touched) {
        ret = gpu_kms_apply_cursor_atomic_state(owner, &cursor_state);
        if (ret == 0)
            cursor_applied = 1;
    }
    if ((req.flags & DRM_MODE_ATOMIC_TEST_ONLY) == 0 &&
        ret == 0 && has_new_fb) {
        ret = changing_mode ?
            gpu_kms_modeset_fb(owner, new_fb, &selected_mode, 0) :
            gpu_kms_present_fb(owner, new_fb);
    }
    if (ret != 0) {
        if (changing_mode && cursor_applied) {
            int restore = gpu_kms_apply_cursor_atomic_state(owner, &old_cursor);
            if (restore != 0)
                printf("DRM: modeset cursor rollback failed: %d\n", restore);
        }
        gpu_kms_cancel_prepared_out_fence(&prepared_out_fence, ret);
        gpu_kms_cleanup_prepared_out_fence(&prepared_out_fence);
        if (out_fence_ptr != 0) {
            int32 failed_fd = -1;

            (void)either_copyout(1, out_fence_ptr, &failed_fd,
                                 sizeof(failed_fd));
        }
        gpu_kms_put_in_fence_file_refs(in_fence_files, in_fence_ref_count);
        return ret;
    }
    if ((req.flags & DRM_MODE_ATOMIC_TEST_ONLY) == 0) {
        spin_lock(&fb_state.lock);
        if (proposed_active) {
            fb_state.kms_mode = selected_mode;
            fb_state.kms_mode_valid = 1;
        }
        fb_state.stats.kms_atomic_commits++;
        fb_state.kms_primary = primary;
        fb_state.kms_primary.touched = 0;
        fb_state.kms_primary_valid = 1;
        spin_unlock(&fb_state.lock);
    }
    if ((req.flags & DRM_MODE_ATOMIC_TEST_ONLY) == 0 && has_new_fb) {
        int want_event = (req.flags & DRM_MODE_PAGE_FLIP_EVENT) != 0;

        /*
         * Flip-complete event delivery for the atomic commit, mirroring the
         * legacy page-flip path's semantics. This kernel exposes a single CRTC
         * (GPU_DRM_CRTC_ID); a real multi-CRTC atomic driver would loop over
         * every CRTC carrying changes in the commit and emit one completion
         * per CRTC, keyed by the ioctl's user_data. Here the sole CRTC is "in
         * the commit" exactly when its scanout changed (has_new_fb, tracked
         * from the primary plane FB_ID / CRTC ACTIVE props), so at most one
         * event is emitted no matter how many plane/CRTC props the commit
         * touched -- there is no per-plane double-emit. The present above has
         * already happened; ring backpressure was enforced with -EAGAIN before
         * it, so the only failure possible from here is a rare concurrent-flip
         * race on a shared fd, in which case the event is dropped (accounted by
         * gpu_drm_event_queue_locked) rather than the commit being unwound.
         */
        if (want_event && gpu_kms_vblank_paced_flip_enabled() &&
            gpu_drm_page_flip_paced(owner, req.user_data, new_fb) == 0) {
            /*
             * Gate: virtio_gpu_vblank_paced_flip. The paced helper performed
             * the present-completion accounting, updated current_kms_fb_id and
             * deferred the completion event to the next synthetic vblank edge,
             * exactly as the legacy path does; a negative return (kvmalloc
             * failure) falls through to the synchronous delivery below.
             */
        } else {
            int queued_event = 0;

            spin_lock(&fb_state.lock);
            if (want_event) {
                uint64 sequence;
                uint64 timestamp_ns;

                gpu_kms_sample_vblank_locked(0, &sequence, &timestamp_ns);
                queued_event =
                    gpu_drm_event_queue_locked(owner, DRM_EVENT_FLIP_COMPLETE,
                                               req.user_data, sequence,
                                               GPU_DRM_CRTC_ID,
                                               timestamp_ns) == 0;
            }
            fb_state.current_kms_fb_id = new_fb;
            spin_unlock(&fb_state.lock);
            if (queued_event)
                gpu_drm_event_notify_read(owner);
        }
    }
    if (out_fence_fd >= 0) {
        spin_lock(&fb_state.lock);
        fb_state.stats.kms_atomic_out_fence_fd_exports++;
        if (!prepared_out_fence.display_correlated)
            fb_state.stats.
                kms_atomic_out_fence_software_scanout_correlated++;
        spin_unlock(&fb_state.lock);
        prepared_out_fence.fd = -1;
        out_fence_fd = -1;
    }
    gpu_kms_release_prepared_out_fence(&prepared_out_fence);
    gpu_kms_put_in_fence_file_refs(in_fence_files, in_fence_ref_count);
    return 0;
}

static struct fb_gpu_syncobj_entry *
gpu_syncobj_lookup_locked(uint32 handle, struct fb_gpu_render_owner *owner)
{
    if (handle == 0 || owner == NULL)
        return NULL;
    for (uint32 i = 0; i < FB_GPU_MAX_SYNCOBJS; i++) {
        struct fb_gpu_syncobj_entry *obj = &fb_state.syncobjs[i];

        if (obj->in_use && obj->handle == handle &&
            obj->owner_id == owner->id && obj->owner_tgid == owner->tgid)
            return obj;
    }
    return NULL;
}

static struct fb_gpu_syncobj_state_entry *
gpu_syncobj_state_locked(uint32 state_index)
{
    if (state_index == 0 || state_index > FB_GPU_MAX_SYNCOBJ_STATES)
