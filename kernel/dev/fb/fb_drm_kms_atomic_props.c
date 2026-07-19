static void gpu_kms_cursor_atomic_snapshot_locked(
    struct gpu_kms_cursor_atomic_state *cursor)
{
    if (cursor == NULL)
        return;
    memset(cursor, 0, sizeof(*cursor));
    cursor->fb_id = fb_state.current_cursor_fb_id;
    cursor->crtc_id =
        fb_state.current_cursor_visible ? GPU_DRM_CRTC_ID : 0;
    cursor->crtc_x = fb_state.current_cursor_x;
    cursor->crtc_y = fb_state.current_cursor_y;
    cursor->crtc_w = fb_state.current_cursor_w;
    cursor->crtc_h = fb_state.current_cursor_h;
    cursor->src_w = (uint32)fb_state.current_cursor_w << 16;
    cursor->src_h = (uint32)fb_state.current_cursor_h << 16;
    cursor->hot_x = fb_state.current_cursor_hot_x;
    cursor->hot_y = fb_state.current_cursor_hot_y;
}

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
            (obj_id == GPU_DRM_CURSOR_PLANE_ID &&
             gpu_kms_cursor_plane_available_locked()) ||
            obj_id == GPU_DRM_MODE_BLOB_ID ||
            obj_id == GPU_DRM_IN_FORMATS_BLOB_ID)
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
        return obj_id == GPU_DRM_PRIMARY_PLANE_ID ||
            (obj_id == GPU_DRM_CURSOR_PLANE_ID &&
             gpu_kms_cursor_plane_available_locked());
    case DRM_MODE_OBJECT_BLOB:
    case DRM_MODE_OBJECT_MODE:
        return obj_id == GPU_DRM_MODE_BLOB_ID ||
            obj_id == GPU_DRM_IN_FORMATS_BLOB_ID;
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
        else if (obj_id == GPU_DRM_CURSOR_PLANE_ID)
            obj_type = DRM_MODE_OBJECT_PLANE;
        else if (obj_id == GPU_DRM_ENCODER_ID)
            obj_type = DRM_MODE_OBJECT_ENCODER;
        else if (obj_id == GPU_DRM_MODE_BLOB_ID)
            obj_type = DRM_MODE_OBJECT_BLOB;
        else if (obj_id == GPU_DRM_IN_FORMATS_BLOB_ID)
            obj_type = DRM_MODE_OBJECT_BLOB;
        else
            obj_type = DRM_MODE_OBJECT_FB;
    }

    switch (obj_type) {
    case DRM_MODE_OBJECT_CRTC:
        gpu_kms_push_prop(out, GPU_DRM_PROP_ACTIVE,
                          current_fb != 0 ? 1 : 0);
        gpu_kms_push_prop(out, GPU_DRM_PROP_MODE_ID,
                          current_fb != 0 ? GPU_DRM_MODE_BLOB_ID : 0);
        gpu_kms_push_prop(out, GPU_DRM_PROP_OUT_FENCE_PTR, 0);
        break;
    case DRM_MODE_OBJECT_CONNECTOR:
        gpu_kms_push_prop(out, GPU_DRM_PROP_CRTC_ID, GPU_DRM_CRTC_ID);
        gpu_kms_push_prop(out, GPU_DRM_PROP_MODE_ID,
                          GPU_DRM_MODE_BLOB_ID);
        gpu_kms_push_prop(out, GPU_DRM_PROP_DPMS, DRM_MODE_DPMS_ON);
        break;
    case DRM_MODE_OBJECT_PLANE:
        if (obj_id == GPU_DRM_CURSOR_PLANE_ID) {
            gpu_kms_push_prop(out, GPU_DRM_PROP_PLANE_TYPE,
                              DRM_PLANE_TYPE_CURSOR);
            gpu_kms_push_prop(out, GPU_DRM_PROP_CRTC_ID,
                              fb_state.current_cursor_visible ?
                              GPU_DRM_CRTC_ID : 0);
            gpu_kms_push_prop(out, GPU_DRM_PROP_FB_ID,
                              fb_state.current_cursor_fb_id);
            gpu_kms_push_prop(out, GPU_DRM_PROP_SRC_X, 0);
            gpu_kms_push_prop(out, GPU_DRM_PROP_SRC_Y, 0);
            gpu_kms_push_prop(out, GPU_DRM_PROP_SRC_W,
                              (uint64)fb_state.current_cursor_w << 16);
            gpu_kms_push_prop(out, GPU_DRM_PROP_SRC_H,
                              (uint64)fb_state.current_cursor_h << 16);
            gpu_kms_push_prop(out, GPU_DRM_PROP_CRTC_X,
                              (uint64)fb_state.current_cursor_x);
            gpu_kms_push_prop(out, GPU_DRM_PROP_CRTC_Y,
                              (uint64)fb_state.current_cursor_y);
            gpu_kms_push_prop(out, GPU_DRM_PROP_CRTC_W,
                              fb_state.current_cursor_w);
            gpu_kms_push_prop(out, GPU_DRM_PROP_CRTC_H,
                              fb_state.current_cursor_h);
            gpu_kms_push_prop(out, GPU_DRM_PROP_IN_FENCE_FD,
                              (uint64)-1);
            if (owner != NULL &&
                drm_core_has_client_cap(&owner->drm,
                                        DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT)) {
                gpu_kms_push_prop(out, GPU_DRM_PROP_HOTSPOT_X,
                                  fb_state.current_cursor_hot_x);
                gpu_kms_push_prop(out, GPU_DRM_PROP_HOTSPOT_Y,
                                  fb_state.current_cursor_hot_y);
            }
        } else {
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
            gpu_kms_push_prop(out, GPU_DRM_PROP_IN_FORMATS,
                              GPU_DRM_IN_FORMATS_BLOB_ID);
        }
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
                                        uint32 in_fence_capacity,
                                        struct gpu_kms_cursor_atomic_state
                                            *cursor)
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
        if (obj_id == GPU_DRM_CURSOR_PLANE_ID && cursor != NULL) {
            cursor->crtc_id = (uint32)value;
            cursor->touched = 1;
        }
        break;
    case GPU_DRM_PROP_MODE_ID:
        if (value != 0 && value != GPU_DRM_MODE_BLOB_ID &&
            (value > 0xffffffffULL ||
             !gpu_drm_user_blob_has_size_locked(
                 (uint32)value,
                 sizeof(struct drm_mode_modeinfo_compat))))
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
    case GPU_DRM_PROP_DPMS:
        if (obj_id != GPU_DRM_CONNECTOR_ID ||
            value > DRM_MODE_DPMS_OFF)
            return -EINVAL;
        break;
    case GPU_DRM_PROP_PLANE_TYPE:
        if (obj_id == GPU_DRM_CURSOR_PLANE_ID) {
            if (value != DRM_PLANE_TYPE_CURSOR)
                return -EINVAL;
        } else if (value != DRM_PLANE_TYPE_PRIMARY) {
            return -EINVAL;
        }
        break;
    case GPU_DRM_PROP_FB_ID:
        if (value != 0) {
            struct fb_gpu_kms_fb_entry *fb =
                gpu_kms_fb_lookup_locked((uint32)value);

            if (obj_id == GPU_DRM_CURSOR_PLANE_ID) {
                ret = gpu_kms_cursor_fb_usable_locked(fb, owner);
                if (ret != 0)
                    return ret;
            } else if (!gpu_kms_fb_owner_matches(fb, owner)) {
                return -ENOENT;
            }
        }
        if (obj_id == GPU_DRM_CURSOR_PLANE_ID && cursor != NULL) {
            cursor->fb_id = (uint32)value;
            cursor->touched = 1;
            cursor->fb_touched = 1;
        } else if (new_fb_id != NULL && has_new_fb != NULL) {
            *new_fb_id = (uint32)value;
            *has_new_fb = 1;
        }
        break;
    case GPU_DRM_PROP_SRC_X:
        if (obj_id == GPU_DRM_CURSOR_PLANE_ID && cursor != NULL) {
            cursor->src_x = (uint32)value;
            cursor->touched = 1;
        }
        break;
    case GPU_DRM_PROP_SRC_Y:
        if (obj_id == GPU_DRM_CURSOR_PLANE_ID && cursor != NULL) {
            cursor->src_y = (uint32)value;
            cursor->touched = 1;
        }
        break;
    case GPU_DRM_PROP_SRC_W:
        if (obj_id == GPU_DRM_CURSOR_PLANE_ID && cursor != NULL) {
            cursor->src_w = (uint32)value;
            cursor->touched = 1;
        }
        break;
    case GPU_DRM_PROP_SRC_H:
        if (obj_id == GPU_DRM_CURSOR_PLANE_ID && cursor != NULL) {
            cursor->src_h = (uint32)value;
            cursor->touched = 1;
        }
        break;
    case GPU_DRM_PROP_CRTC_X:
        if (obj_id == GPU_DRM_CURSOR_PLANE_ID && cursor != NULL) {
            cursor->crtc_x = (int32)value;
            cursor->touched = 1;
        }
        break;
    case GPU_DRM_PROP_CRTC_Y:
        if (obj_id == GPU_DRM_CURSOR_PLANE_ID && cursor != NULL) {
            cursor->crtc_y = (int32)value;
            cursor->touched = 1;
        }
        break;
    case GPU_DRM_PROP_CRTC_W:
        if (obj_id == GPU_DRM_CURSOR_PLANE_ID && cursor != NULL) {
            cursor->crtc_w = (uint32)value;
            cursor->touched = 1;
        }
        break;
    case GPU_DRM_PROP_CRTC_H:
        if (obj_id == GPU_DRM_CURSOR_PLANE_ID && cursor != NULL) {
            cursor->crtc_h = (uint32)value;
            cursor->touched = 1;
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
    case GPU_DRM_PROP_IN_FORMATS:
        if (value != GPU_DRM_IN_FORMATS_BLOB_ID)
            return -EINVAL;
        break;
    case GPU_DRM_PROP_HOTSPOT_X:
        if (obj_id != GPU_DRM_CURSOR_PLANE_ID || owner == NULL ||
            !drm_core_has_client_cap(&owner->drm,
                                     DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT) ||
            value >= FB_GPU_CURSOR_MAX_DIM)
            return -EINVAL;
        if (cursor != NULL) {
            cursor->hot_x = (uint32)value;
            cursor->touched = 1;
            cursor->fb_touched = 1;
        }
        break;
    case GPU_DRM_PROP_HOTSPOT_Y:
        if (obj_id != GPU_DRM_CURSOR_PLANE_ID || owner == NULL ||
            !drm_core_has_client_cap(&owner->drm,
                                     DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT) ||
            value >= FB_GPU_CURSOR_MAX_DIM)
            return -EINVAL;
        if (cursor != NULL) {
            cursor->hot_y = (uint32)value;
            cursor->touched = 1;
            cursor->fb_touched = 1;
        }
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
    if (req.prop_id == GPU_DRM_PROP_MODE_ID) {
        ret = gpu_drm_validate_mode_blob(req.value);
        if (ret != 0)
            return ret;
    }
    spin_lock(&fb_state.lock);
    ret = gpu_kms_validate_prop_locked(owner, req.obj_id, req.obj_type,
                                       req.prop_id, req.value, &new_fb,
                                       &has_new_fb, NULL, NULL, NULL, 0,
                                       NULL);
    if (ret == 0 && req.obj_id == GPU_DRM_CRTC_ID &&
        req.prop_id == GPU_DRM_PROP_MODE_ID &&
        ((req.value != 0) != (fb_state.current_kms_fb_id != 0)))
        ret = -EINVAL;
    if (ret == 0 && req.obj_id == GPU_DRM_CRTC_ID &&
        req.prop_id == GPU_DRM_PROP_ACTIVE && req.value != 0 &&
        fb_state.current_kms_fb_id == 0)
        ret = -EINVAL;
    if (ret == 0 && has_new_fb) {
        struct fb_gpu_kms_fb_entry *fb =
            new_fb == 0 ? NULL : gpu_kms_fb_lookup_locked(new_fb);

        if (new_fb != 0 &&
            !gpu_kms_primary_scanout_format_supported(fb->pixel_format,
                                                      fb->modifier))
            ret = -EOPNOTSUPP;
        else
            fb_state.current_kms_fb_id = new_fb;
    }
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

        if (sync_file->kind != FB_GPU_SYNCOBJ_FD_SYNC_FILE) {
            ret = -EINVAL;
        } else if (sync_file->snapshot_signaled ||
                   dma_fence_is_signaled(sync_file->fence)) {
            ret = 0;
        } else if (sync_file->fence != NULL) {
            spin_lock(&fb_state.lock);
            fb_state.stats.kms_atomic_in_fence_sync_file_pending_waits++;
            spin_unlock(&fb_state.lock);
            ret = dma_fence_wait(sync_file->fence, -1);
            if (ret == 0) {
                spin_lock(&fb_state.lock);
                fb_state.stats.
                    kms_atomic_in_fence_sync_file_pending_wakeups++;
                spin_unlock(&fb_state.lock);
            }
        } else {
            ret = -EINVAL;
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
    struct dma_fence *fence;
    uint64 target_sequence;
    int display_correlated;
};

static void gpu_kms_init_prepared_out_fence(
    struct gpu_kms_prepared_out_fence *out)
{
    if (out == NULL)
        return;
    out->fd = -1;
    out->fence = NULL;
    out->target_sequence = 0;
    out->display_correlated = 0;
}

static void gpu_kms_ensure_out_fence_list_locked(void)
{
    if (fb_state.kms_pending_out_fences_ready)
        return;
    list_entry_init(&fb_state.kms_pending_out_fences);
    fb_state.kms_pending_out_fences_ready = 1;
}

static int gpu_kms_export_out_fence_fd(
    struct gpu_kms_prepared_out_fence *out,
    int display_correlated)
{
    struct fb_gpu_syncobj_file *sync_file;
    struct dma_fence *fence;
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

    fence = kvmalloc(sizeof(*fence));
    if (fence == NULL)
        return -ENOMEM;
    dma_fence_init(fence, 0, seqno);
    if (!display_correlated)
        (void)dma_fence_signal(fence, 0);

    sync_file = kvmalloc(sizeof(*sync_file));
    if (sync_file == NULL) {
        dma_fence_put(fence);
        return -ENOMEM;
    }
    memset(sync_file, 0, sizeof(*sync_file));
    sync_file->kind = FB_GPU_SYNCOBJ_FD_SYNC_FILE;
    sync_file->snapshot_signaled = !display_correlated;
    sync_file->snapshot_timeline_value = seqno;
    sync_file->fence = dma_fence_get(fence);
    if (sync_file->fence == NULL) {
        kvfree(sync_file);
        dma_fence_put(fence);
        return -ENOMEM;
    }

    fd = vfs_custom_fd_alloc(&fb_syncobj_file_ops, sync_file, 0);
    if (fd < 0) {
        dma_fence_put(sync_file->fence);
        kvfree(sync_file);
        dma_fence_put(fence);
        return fd;
    }

    spin_lock(&fb_state.lock);
    fb_state.stats.syncobj_sync_file_exports++;
    spin_unlock(&fb_state.lock);
    out->fd = fd;
    out->fence = fence;
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
    struct dma_fence *fence;

    if (out == NULL || out->fence == NULL)
        return;
    fence = out->fence;
    out->fence = NULL;
    dma_fence_put(fence);
}

static void gpu_kms_cleanup_prepared_out_fence(
    struct gpu_kms_prepared_out_fence *out)
{
    if (out == NULL)
        return;
    gpu_kms_cleanup_prepared_out_fence_fd(&out->fd);
    gpu_kms_release_prepared_out_fence(out);
}

static int gpu_kms_arm_prepared_out_fence(
    struct gpu_kms_prepared_out_fence *out)
{
    struct gpu_kms_pending_out_fence *pending;

    if (out == NULL || out->fence == NULL || !out->display_correlated)
        return 0;
    pending = kvmalloc(sizeof(*pending));
    if (pending == NULL)
        return -ENOMEM;
    memset(pending, 0, sizeof(*pending));
    list_entry_init(&pending->node);
    pending->fence = dma_fence_get(out->fence);
    if (pending->fence == NULL) {
        kvfree(pending);
        return -ENOMEM;
    }
    pending->target_sequence = out->target_sequence;

    spin_lock(&fb_state.lock);
    gpu_kms_ensure_out_fence_list_locked();
    list_node_push_back(&fb_state.kms_pending_out_fences, pending, node);
    spin_unlock(&fb_state.lock);
    return 0;
}

static void gpu_kms_cancel_prepared_out_fence(
    struct gpu_kms_prepared_out_fence *out, int error)
{
    struct gpu_kms_pending_out_fence *pending;
    struct gpu_kms_pending_out_fence *tmp;
    struct dma_fence *fence;

    if (out == NULL || out->fence == NULL)
        return;
    fence = out->fence;
    spin_lock(&fb_state.lock);
    if (fb_state.kms_pending_out_fences_ready) {
        list_foreach_node_safe(&fb_state.kms_pending_out_fences, pending,
                               tmp, node) {
            if (pending->fence != fence)
                continue;
            list_node_detach(pending, node);
            (void)dma_fence_signal(pending->fence, error);
            dma_fence_put(pending->fence);
            kvfree(pending);
            break;
        }
    }
    spin_unlock(&fb_state.lock);
}

static void gpu_kms_signal_pending_out_fences_locked(uint64 sequence)
{
    struct gpu_kms_pending_out_fence *pending;
    struct gpu_kms_pending_out_fence *tmp;

    if (!fb_state.kms_pending_out_fences_ready)
        return;
    list_foreach_node_safe(&fb_state.kms_pending_out_fences, pending, tmp,
                           node) {
        if (pending->target_sequence > sequence)
            continue;
        list_node_detach(pending, node);
        (void)dma_fence_signal(pending->fence, 0);
        dma_fence_put(pending->fence);
        kvfree(pending);
        fb_state.stats.kms_atomic_out_fence_display_correlated++;
    }
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
        if (fb_state.current_cursor_fb_id == fb->fb_id) {
            fb_state.current_cursor_fb_id = 0;
            fb_state.current_cursor_visible = 0;
        }
        gpu_kms_unpin_bo_locked(fb->bo_handle, owner->id, owner->tgid);
        memset(fb, 0, sizeof(*fb));
        if (fb_state.stats.kms_framebuffers > 0)
            fb_state.stats.kms_framebuffers--;
        stale++;
    }
    spin_unlock(&fb_state.lock);
    return stale;
