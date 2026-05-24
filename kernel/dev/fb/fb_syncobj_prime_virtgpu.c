        return NULL;
    if (!fb_state.syncobj_states[state_index - 1].in_use)
        return NULL;
    return &fb_state.syncobj_states[state_index - 1];
}

static void gpu_syncobj_state_put_locked(uint32 state_index)
{
    struct fb_gpu_syncobj_state_entry *state =
        gpu_syncobj_state_locked(state_index);
    uint32 proxy_source_index = 0;

    if (state == NULL)
        return;
    if (state->refs > 0)
        state->refs--;
    if (state->refs == 0) {
        proxy_source_index = state->proxy_source_index;
        memset(state, 0, sizeof(*state));
    }
    if (proxy_source_index != 0)
        gpu_syncobj_state_put_locked(proxy_source_index);
}

static int gpu_syncobj_state_get_locked(uint32 state_index)
{
    struct fb_gpu_syncobj_state_entry *state =
        gpu_syncobj_state_locked(state_index);

    if (state == NULL)
        return -ENOENT;
    state->refs++;
    return 0;
}

static int gpu_syncobj_alloc_state_locked(int signaled,
                                          uint32 *state_index)
{
    if (state_index == NULL)
        return -EINVAL;
    for (uint32 i = 0; i < FB_GPU_MAX_SYNCOBJ_STATES; i++) {
        struct fb_gpu_syncobj_state_entry *state =
            &fb_state.syncobj_states[i];

        if (state->in_use)
            continue;
        memset(state, 0, sizeof(*state));
        state->in_use = 1;
        state->signaled = signaled;
        state->timeline_value = signaled ? 1 : 0;
        state->reservation_fence = state->timeline_value;
        *state_index = i + 1;
        return 0;
    }
    return -ENOSPC;
}

static int gpu_syncobj_state_ready_locked(
    struct fb_gpu_syncobj_state_entry *state, uint64 point)
{
    if (state == NULL)
        return 0;
    if (point == 0)
        point = 1;
    if (!state->signaled && state->proxy_source_index != 0) {
        struct fb_gpu_syncobj_state_entry *source =
            gpu_syncobj_state_locked(state->proxy_source_index);
        uint64 proxy_point = state->proxy_point != 0 ? state->proxy_point : 1;

        if (source != NULL && source->signaled &&
            source->timeline_value >= proxy_point) {
            state->signaled = 1;
            state->timeline_value = proxy_point;
            state->reservation_seq++;
            state->reservation_fence = source->reservation_fence;
            fb_state.stats.sync_file_pending_wakeups++;
        }
    }
    return state->signaled && state->timeline_value >= point;
}

static int gpu_syncobj_alloc_handle_locked(struct fb_gpu_render_owner *owner,
                                           uint32 state_index,
                                           uint32 *handle_out)
{
    struct fb_gpu_syncobj_state_entry *state;

    if (owner == NULL || handle_out == NULL)
        return -EINVAL;
    state = gpu_syncobj_state_locked(state_index);
    if (state == NULL)
        return -ENOENT;

    for (uint32 i = 0; i < FB_GPU_MAX_SYNCOBJS; i++) {
        struct fb_gpu_syncobj_entry *obj = &fb_state.syncobjs[i];
        uint32 handle;

        if (obj->in_use)
            continue;
        handle = fb_state.next_syncobj_handle++;
        if (fb_state.next_syncobj_handle == 0)
            fb_state.next_syncobj_handle = 1;
        memset(obj, 0, sizeof(*obj));
        obj->in_use = 1;
        obj->handle = handle;
        obj->owner_id = owner->id;
        obj->owner_tgid = owner->tgid;
        obj->state_index = state_index;
        state->refs++;
        fb_state.stats.syncobj_created++;
        fb_state.stats.syncobj_live++;
        *handle_out = handle;
        return 0;
    }
    return -ENOSPC;
}

static void gpu_syncobj_destroy_handle_locked(struct fb_gpu_syncobj_entry *obj)
{
    uint32 state_index;

    if (obj == NULL || !obj->in_use)
        return;
    state_index = obj->state_index;
    memset(obj, 0, sizeof(*obj));
    if (fb_state.stats.syncobj_live > 0)
        fb_state.stats.syncobj_live--;
    gpu_syncobj_state_put_locked(state_index);
}

static int gpu_syncobj_copy_handle(uint64 base, uint32 index, uint32 *handle)
{
    if (base == 0 || handle == NULL)
        return -EINVAL;
    if (either_copyin(handle, 1, base + (uint64)index * sizeof(uint32),
                      sizeof(*handle)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_syncobj_copy_point(uint64 base, uint32 index, uint64 *point)
{
    if (base == 0 || point == NULL)
        return -EINVAL;
    if (either_copyin(point, 1, base + (uint64)index * sizeof(uint64),
                      sizeof(*point)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_syncobj_write_point(uint64 base, uint32 index, uint64 point)
{
    if (base == 0)
        return -EINVAL;
    if (either_copyout(1, base + (uint64)index * sizeof(uint64),
                       &point, sizeof(point)) < 0)
        return -EFAULT;
    return 0;
}

static void gpu_syncobj_wakeup_locked(void)
{
    uint32 waiters = fb_state.syncobj_waiters;

    fb_state.syncobj_wakeup_seq++;
    if (waiters != 0) {
        fb_state.stats.syncobj_wait_wakeups += waiters;
        wakeup_on_chan(&fb_state.syncobj_wakeup_seq);
    }
}

static void gpu_syncobj_timeout_wakeup(void *data)
{
    if (data != NULL)
        wakeup_on_chan(data);
}

static void gpu_syncobj_signal_state_locked(
    struct fb_gpu_syncobj_state_entry *state, uint64 point)
{
    if (state == NULL)
        return;
    if (point == 0)
        point = 1;
    state->signaled = 1;
    if (state->timeline_value < point)
        state->timeline_value = point;
    state->reservation_seq++;
    state->reservation_fence = state->timeline_value;
    fb_state.stats.syncobj_signals++;
    if (state->pending_sync_file_callbacks != 0) {
        fb_state.stats.sync_file_pending_callbacks_fired +=
            state->pending_sync_file_callbacks;
        state->pending_sync_file_callbacks = 0;
        state->sync_file_callback_fired_generation =
            state->sync_file_callback_generation;
    }
    if (state->pending_wait_callbacks != 0) {
        fb_state.stats.syncobj_wait_callbacks_fired +=
            state->pending_wait_callbacks;
        state->pending_wait_callbacks = 0;
        state->wait_callback_fired_generation =
            state->wait_callback_generation;
    }
    gpu_syncobj_wakeup_locked();
}

static void gpu_syncobj_reset_state_locked(
    struct fb_gpu_syncobj_state_entry *state)
{
    if (state == NULL)
        return;
    state->signaled = 0;
    state->reservation_seq++;
    state->reservation_fence = 0;
    gpu_syncobj_wakeup_locked();
}

static uint64 gpu_syncobj_attach_owner_resv_locked(
    struct fb_gpu_render_owner *owner, uint32 attach_point)
{
    uint64 attached;

    if (owner == NULL)
        return 0;
    attached = fb_ttm_resv_record_owner_locked(owner->id, owner->tgid,
                                               attach_point,
                                               FB_GPU_DMABUF_TAG_NONE);
    if (attached == 0)
        fb_ttm_resv_note_attach_locked(attach_point);
    return attached;
}

static int gpu_syncobj_timeout_to_ms(int64 timeout_nsec, uint64 *ms_out,
                                     int *finite_out)
{
    if (ms_out == NULL || finite_out == NULL)
        return -EINVAL;
    *ms_out = 0;
    *finite_out = timeout_nsec >= 0;
    if (timeout_nsec < 0)
        return 0;
    *ms_out = ((uint64)timeout_nsec + 999999ULL) / 1000000ULL;
    return 0;
}

static int gpu_syncobj_wait_ready_locked(struct fb_gpu_render_owner *owner,
                                         uint32 *handles, uint64 *points,
                                         uint32 count, uint32 flags,
                                         int *first)
{
    int found = -1;

    if (owner == NULL || handles == NULL || points == NULL || first == NULL)
        return -EINVAL;
    for (uint32 i = 0; i < count; i++) {
        struct fb_gpu_syncobj_entry *obj;
        struct fb_gpu_syncobj_state_entry *state;
        int ready;

        obj = gpu_syncobj_lookup_locked(handles[i], owner);
        state = obj != NULL ? gpu_syncobj_state_locked(obj->state_index) :
            NULL;
        if (obj == NULL || state == NULL)
            return -ENOENT;
        ready = gpu_syncobj_state_ready_locked(state, points[i]);
        if (ready && found < 0)
            found = (int)i;
        if (!ready && (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL)) {
            *first = -1;
            return 0;
        }
    }
    *first = found;
    return 0;
}

static uint32 gpu_syncobj_wait_arm_callbacks_locked(
    struct fb_gpu_render_owner *owner, uint32 *handles, uint64 *points,
    uint32 count, uint32 *armed_states, uint64 *armed_generations)
{
    uint32 armed = 0;

    if (owner == NULL || handles == NULL || points == NULL ||
        armed_states == NULL || armed_generations == NULL)
        return 0;
    for (uint32 i = 0; i < count && armed < 64; i++) {
        struct fb_gpu_syncobj_entry *obj;
        struct fb_gpu_syncobj_state_entry *state;

        obj = gpu_syncobj_lookup_locked(handles[i], owner);
        state = obj != NULL ? gpu_syncobj_state_locked(obj->state_index) :
            NULL;
        if (obj == NULL || state == NULL ||
            gpu_syncobj_state_ready_locked(state, points[i]))
            continue;
        state->pending_wait_callbacks++;
        state->wait_callback_generation++;
        armed_states[armed] = obj->state_index;
        armed_generations[armed] = state->wait_callback_generation;
        armed++;
        fb_state.stats.syncobj_wait_callbacks_armed++;
    }
    return armed;
}

static void gpu_syncobj_wait_cancel_callbacks_locked(
    uint32 *armed_states, uint64 *armed_generations, uint32 armed_count)
{
    if (armed_states == NULL || armed_generations == NULL)
        return;
    for (uint32 i = 0; i < armed_count; i++) {
        struct fb_gpu_syncobj_state_entry *state =
            gpu_syncobj_state_locked(armed_states[i]);

        if (state == NULL)
            continue;
        if (state->wait_callback_fired_generation >= armed_generations[i])
            continue;
        if (state->pending_wait_callbacks > 0) {
            state->pending_wait_callbacks--;
            fb_state.stats.syncobj_wait_callbacks_cancelled++;
        } else {
            fb_state.stats.syncobj_wait_callback_late_fires++;
        }
    }
}

static int gpu_syncobj_create(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_syncobj_create_compat req;
    uint32 handle = 0;
    uint32 state_index = 0;
    int ret;

    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if ((req.flags & ~DRM_SYNCOBJ_CREATE_SIGNALED) != 0)
        return -EINVAL;

    spin_lock(&fb_state.lock);
    ret = gpu_syncobj_alloc_state_locked(
        (req.flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0, &state_index);
    if (ret == 0)
        ret = gpu_syncobj_alloc_handle_locked(owner, state_index, &handle);
    if (ret != 0 && state_index != 0)
        gpu_syncobj_state_put_locked(state_index);
    spin_unlock(&fb_state.lock);
    if (ret != 0)
        return ret;
    req.handle = handle;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0) {
        spin_lock(&fb_state.lock);
        struct fb_gpu_syncobj_entry *obj =
            gpu_syncobj_lookup_locked(handle, owner);
        if (obj != NULL)
            gpu_syncobj_destroy_handle_locked(obj);
        spin_unlock(&fb_state.lock);
        return -EFAULT;
    }
    return 0;
}

static int gpu_syncobj_destroy(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_syncobj_destroy_compat req;
    struct fb_gpu_syncobj_entry *obj;
    int ret = -ENOENT;

    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;

    spin_lock(&fb_state.lock);
    obj = gpu_syncobj_lookup_locked(req.handle, owner);
    if (obj != NULL) {
        gpu_syncobj_destroy_handle_locked(obj);
        ret = 0;
    }
    spin_unlock(&fb_state.lock);
    return ret;
}

struct fb_gpu_syncobj_handle_legacy {
    uint32 handle;
    uint32 flags;
    int32 fd;
    uint32 pad;
};

static int gpu_syncobj_handle_copyin(uint64 arg,
                                     struct drm_syncobj_handle_compat *req,
                                     int legacy)
{
    struct fb_gpu_syncobj_handle_legacy old;

    if (req == NULL)
        return -EINVAL;
    memset(req, 0, sizeof(*req));
    if (!legacy)
        return either_copyin(req, 1, arg, sizeof(*req)) < 0 ? -EFAULT : 0;
    if (either_copyin(&old, 1, arg, sizeof(old)) < 0)
        return -EFAULT;
    req->handle = old.handle;
    req->flags = old.flags;
    req->fd = old.fd;
    req->pad = old.pad;
    req->point = 0;
    return 0;
}

static int gpu_syncobj_handle_copyout(uint64 arg,
                                      const struct drm_syncobj_handle_compat *req,
                                      int legacy)
{
    struct fb_gpu_syncobj_handle_legacy old;

    if (req == NULL)
        return -EINVAL;
    if (!legacy)
        return either_copyout(1, arg, (void *)req, sizeof(*req)) < 0 ?
            -EFAULT : 0;
    memset(&old, 0, sizeof(old));
    old.handle = req->handle;
    old.flags = req->flags;
    old.fd = req->fd;
    old.pad = req->pad;
    return either_copyout(1, arg, &old, sizeof(old)) < 0 ? -EFAULT : 0;
}

static int gpu_syncobj_handle_to_fd(struct fb_gpu_render_owner *owner,
                                    uint64 arg, int legacy)
{
    struct drm_syncobj_handle_compat req;
    struct fb_gpu_syncobj_file *sync_file;
    struct fb_gpu_syncobj_entry *obj;
    int fd;
    int ret;

    if (owner == NULL)
        return -EBADF;
    ret = gpu_syncobj_handle_copyin(arg, &req, legacy);
    if (ret != 0)
        return ret;
    if (req.pad != 0 ||
        (req.flags & ~(DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE |
                       DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE)) != 0 ||
        (req.point != 0 &&
         ((req.flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE) == 0 ||
          (req.flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE) == 0)))
        return -EINVAL;

    sync_file = kvmalloc(sizeof(*sync_file));
    if (sync_file == NULL)
        return -ENOMEM;
    memset(sync_file, 0, sizeof(*sync_file));

    spin_lock(&fb_state.lock);
    obj = gpu_syncobj_lookup_locked(req.handle, owner);
    if (obj == NULL) {
        spin_unlock(&fb_state.lock);
        kvfree(sync_file);
        return -ENOENT;
    }
    if (req.flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE) {
        struct fb_gpu_syncobj_state_entry *state =
            gpu_syncobj_state_locked(obj->state_index);

        if (state == NULL) {
            ret = -ENOENT;
        } else {
            uint64 point = req.point != 0 ? req.point :
                (state->timeline_value != 0 ? state->timeline_value : 1);

            ret = gpu_syncobj_state_get_locked(obj->state_index);
            if (ret == 0) {
                sync_file->kind = FB_GPU_SYNCOBJ_FD_SYNC_FILE;
                sync_file->state_index = obj->state_index;
                sync_file->snapshot_signaled =
                    state->signaled && state->timeline_value >= point;
                sync_file->snapshot_timeline_value = point;
                sync_file->snapshot_reservation_fence =
                    state->reservation_fence;
                if (!sync_file->snapshot_signaled)
                    fb_state.stats.sync_file_pending_exports++;
            }
        }
        fb_state.stats.syncobj_sync_file_exports++;
        gpu_syncobj_attach_owner_resv_locked(
            owner, FB_GPU_RESV_ATTACH_SYNC_FILE_EXPORT);
    } else {
        ret = gpu_syncobj_state_get_locked(obj->state_index);
        if (ret == 0) {
            sync_file->kind = FB_GPU_SYNCOBJ_FD_OPAQUE;
            sync_file->state_index = obj->state_index;
        }
    }
    spin_unlock(&fb_state.lock);
    if (ret != 0) {
        kvfree(sync_file);
        return ret;
    }

    fd = vfs_custom_fd_alloc(&fb_syncobj_file_ops, sync_file, 0);
    if (fd < 0) {
        if (sync_file->state_index != 0) {
            spin_lock(&fb_state.lock);
            gpu_syncobj_state_put_locked(sync_file->state_index);
            spin_unlock(&fb_state.lock);
        }
        kvfree(sync_file);
        return fd;
    }
    req.fd = fd;
    ret = gpu_syncobj_handle_copyout(arg, &req, legacy);
    if (ret != 0)
        fb_gpu_close_exported_fd(fd);
    return ret;
}

static int gpu_syncobj_fd_to_handle(struct fb_gpu_render_owner *owner,
                                    uint64 arg, int legacy)
{
    struct drm_syncobj_handle_compat req;
    struct fb_gpu_syncobj_file *sync_file;
    struct vfs_file *file;
    uint32 handle = 0;
    int ret;

    if (owner == NULL)
        return -EBADF;
    ret = gpu_syncobj_handle_copyin(arg, &req, legacy);
    if (ret != 0)
        return ret;
    if (req.pad != 0 || req.fd < 0 ||
        (req.flags & ~(DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE |
                       DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE)) != 0 ||
        (req.point != 0 &&
         ((req.flags & DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE) == 0 ||
          (req.flags & DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE) == 0)))
        return -EINVAL;

    file = vfs_fdtable_get_file(current->fdtable, req.fd);
    if (file == NULL)
        return -EBADF;

    if ((req.flags & DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE) == 0) {
        if (file->ops != &fb_syncobj_file_ops ||
            file->private_data == NULL) {
            ret = -EINVAL;
        } else {
            sync_file = (struct fb_gpu_syncobj_file *)file->private_data;
            if (sync_file->kind != FB_GPU_SYNCOBJ_FD_OPAQUE) {
                ret = -EINVAL;
            } else {
                spin_lock(&fb_state.lock);
                ret = gpu_syncobj_alloc_handle_locked(
                    owner, sync_file->state_index, &handle);
                spin_unlock(&fb_state.lock);
            }
        }
    } else if (file->ops == &fb_syncobj_file_ops &&
               file->private_data != NULL) {
        uint32 state_index = 0;

        sync_file = (struct fb_gpu_syncobj_file *)file->private_data;
        if (sync_file->kind != FB_GPU_SYNCOBJ_FD_SYNC_FILE) {
            ret = -EINVAL;
        } else {
            struct fb_gpu_syncobj_state_entry *source_state;
            int ready;
            uint64 import_point = req.point != 0 ? req.point :
                sync_file->snapshot_timeline_value;

            spin_lock(&fb_state.lock);
            source_state = gpu_syncobj_state_locked(sync_file->state_index);
            ready = sync_file->snapshot_signaled ||
                gpu_syncobj_state_ready_locked(
                    source_state, sync_file->snapshot_timeline_value);
            if (!ready && source_state == NULL) {
                fb_state.stats.sync_file_pending_import_rejects++;
                ret = -ENOENT;
            } else {
                ret = gpu_syncobj_alloc_state_locked(ready, &state_index);
            }
            if (ret == 0) {
                struct fb_gpu_syncobj_state_entry *state =
                    gpu_syncobj_state_locked(state_index);

                if (state != NULL) {
                    state->timeline_value = import_point;
                    state->reservation_fence =
                        sync_file->snapshot_reservation_fence;
                    if (!ready) {
                        ret = gpu_syncobj_state_get_locked(
                            sync_file->state_index);
                        if (ret == 0) {
                            state->proxy_source_index =
                                sync_file->state_index;
                            state->proxy_point =
                                sync_file->snapshot_timeline_value;
                            fb_state.stats.sync_file_pending_imports++;
                        }
                    }
                }
                if (ret == 0)
                    ret = gpu_syncobj_alloc_handle_locked(owner, state_index,
                                                          &handle);
                if (ret != 0)
                    gpu_syncobj_state_put_locked(state_index);
            }
            if (ret == 0) {
                fb_state.stats.syncobj_sync_file_imports++;
                gpu_syncobj_attach_owner_resv_locked(
                    owner, FB_GPU_RESV_ATTACH_SYNC_FILE_IMPORT);
            }
            spin_unlock(&fb_state.lock);
        }
    } else if (file->ops == &fb_fence_file_ops &&
               file->private_data != NULL) {
        struct fb_gpu_fence_file *fence_file =
            (struct fb_gpu_fence_file *)file->private_data;
        uint32 state_index = 0;

        spin_lock(&fb_state.lock);
        ret = fb_gpu_fence_file_wait_locked(fence_file);
        if (ret == 0)
            ret = gpu_syncobj_alloc_state_locked(1, &state_index);
        if (ret == 0) {
            struct fb_gpu_syncobj_state_entry *state =
                gpu_syncobj_state_locked(state_index);

            if (state != NULL && fence_file->fence != 0) {
                state->timeline_value = fence_file->fence;
                state->reservation_fence = fence_file->fence;
            }
            ret = gpu_syncobj_alloc_handle_locked(owner, state_index,
                                                  &handle);
            if (ret != 0)
                gpu_syncobj_state_put_locked(state_index);
        }
        if (ret == 0) {
            fb_state.stats.syncobj_sync_file_imports++;
            gpu_syncobj_attach_owner_resv_locked(
                owner, FB_GPU_RESV_ATTACH_SYNC_FILE_IMPORT);
        }
        spin_unlock(&fb_state.lock);
    } else if (file->ops == &fb_virgl_fence_file_ops &&
               file->private_data != NULL) {
        struct fb_gpu_virgl_fence_file *fence_file =
            (struct fb_gpu_virgl_fence_file *)file->private_data;
        uint64 signaled = 0;
        uint32 state_index = 0;

        ret = virtio_gpu_user_fence(fence_file->fence, 1, &signaled);
        if (ret == 0 && signaled < fence_file->fence)
            ret = -EAGAIN;
        if (ret == 0) {
            spin_lock(&fb_state.lock);
            ret = gpu_syncobj_alloc_state_locked(1, &state_index);
            if (ret == 0) {
                struct fb_gpu_syncobj_state_entry *state =
                    gpu_syncobj_state_locked(state_index);

                if (state != NULL) {
                    state->timeline_value = fence_file->fence;
                    state->reservation_fence = fence_file->fence;
                }
                ret = gpu_syncobj_alloc_handle_locked(owner, state_index,
                                                      &handle);
                if (ret != 0)
                    gpu_syncobj_state_put_locked(state_index);
            }
            if (ret == 0) {
                fb_state.stats.syncobj_sync_file_imports++;
                gpu_syncobj_attach_owner_resv_locked(
                    owner, FB_GPU_RESV_ATTACH_SYNC_FILE_IMPORT);
            }
            spin_unlock(&fb_state.lock);
        }
    } else {
        ret = -EINVAL;
    }
    vfs_fput(file);
    if (ret != 0)
        return ret;

    req.handle = handle;
    ret = gpu_syncobj_handle_copyout(arg, &req, legacy);
    if (ret != 0) {
        spin_lock(&fb_state.lock);
        struct fb_gpu_syncobj_entry *obj =
            gpu_syncobj_lookup_locked(handle, owner);
        if (obj != NULL)
            gpu_syncobj_destroy_handle_locked(obj);
        spin_unlock(&fb_state.lock);
    }
    return ret;
}

static int gpu_syncobj_array_signal_reset(struct fb_gpu_render_owner *owner,
                                          uint64 arg, int signal,
                                          int timeline)
{
    struct drm_syncobj_array_compat arr;
    struct drm_syncobj_timeline_array_compat tl;
    uint32 count;
    uint64 handles_ptr;
    uint64 points_ptr = 0;

    if (owner == NULL)
        return -EBADF;
    if (timeline) {
        if (either_copyin(&tl, 1, arg, sizeof(tl)) < 0)
            return -EFAULT;
        if (tl.flags != 0)
            return -EINVAL;
        count = tl.count_handles;
        handles_ptr = tl.handles;
        points_ptr = tl.points;
    } else {
        if (either_copyin(&arr, 1, arg, sizeof(arr)) < 0)
            return -EFAULT;
        if (arr.pad != 0)
            return -EINVAL;
        count = arr.count_handles;
        handles_ptr = arr.handles;
    }
    if (count == 0 || count > 64)
        return -EINVAL;

    for (uint32 i = 0; i < count; i++) {
        uint32 handle;
        uint64 point = 1;
        int ret = gpu_syncobj_copy_handle(handles_ptr, i, &handle);
        if (ret != 0)
            return ret;
        if (timeline) {
            ret = gpu_syncobj_copy_point(points_ptr, i, &point);
            if (ret != 0)
                return ret;
        }
        struct fb_gpu_syncobj_entry *obj;
        struct fb_gpu_syncobj_state_entry *state;

        spin_lock(&fb_state.lock);
        obj = gpu_syncobj_lookup_locked(handle, owner);
        state = obj != NULL ? gpu_syncobj_state_locked(obj->state_index) :
            NULL;
        if (obj == NULL || state == NULL) {
            spin_unlock(&fb_state.lock);
            return -ENOENT;
        }
        if (signal) {
            gpu_syncobj_signal_state_locked(state, point);
            gpu_syncobj_attach_owner_resv_locked(
                owner, FB_GPU_RESV_ATTACH_SYNCOBJ_SIGNAL);
        } else {
            gpu_syncobj_reset_state_locked(state);
        }
        spin_unlock(&fb_state.lock);
    }
    return 0;
}

static int gpu_syncobj_wait_common(struct fb_gpu_render_owner *owner,
                                   uint64 arg, int timeline)
{
    struct drm_syncobj_wait_compat wait;
    struct drm_syncobj_timeline_wait_compat twait;
    uint32 count;
    uint64 handles_ptr;
    uint64 points_ptr = 0;
    int first = -1;
    uint32 flags;
    int64 timeout_nsec;
    uint32 handles[64];
    uint64 points[64];
    uint64 timeout_ms = 0;
    uint64 deadline_ms = 0;
    int finite_timeout = 0;
    int timer_armed = 0;
    uint32 armed_states[64];
    uint64 armed_generations[64];
    int ret;

    if (owner == NULL)
        return -EBADF;
    if (timeline) {
        if (either_copyin(&twait, 1, arg, sizeof(twait)) < 0)
            return -EFAULT;
        count = twait.count_handles;
        handles_ptr = twait.handles;
        points_ptr = twait.points;
        flags = twait.flags;
        timeout_nsec = twait.timeout_nsec;
    } else {
        if (either_copyin(&wait, 1, arg, sizeof(wait)) < 0)
            return -EFAULT;
        count = wait.count_handles;
        handles_ptr = wait.handles;
        flags = wait.flags;
        timeout_nsec = wait.timeout_nsec;
    }
    if (count == 0 || count > 64 ||
        (flags & ~(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
                   DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
                   DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE |
                   DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE)) != 0)
        return -EINVAL;

    for (uint32 i = 0; i < count; i++) {
        uint32 handle;
        uint64 point = 1;
        ret = gpu_syncobj_copy_handle(handles_ptr, i, &handle);
        if (ret != 0)
            return ret;
        if (timeline) {
            ret = gpu_syncobj_copy_point(points_ptr, i, &point);
            if (ret != 0)
                return ret;
        }
        handles[i] = handle;
        points[i] = point == 0 ? 1 : point;
    }

    ret = gpu_syncobj_timeout_to_ms(timeout_nsec, &timeout_ms,
                                    &finite_timeout);
    if (ret != 0)
        return ret;
    if (finite_timeout && timeout_ms != 0) {
        deadline_ms = sched_timer_now_ms() + timeout_ms;
        if (sched_timer_add(gpu_syncobj_timeout_wakeup,
                            &fb_state.syncobj_wakeup_seq,
                            timeout_ms) == 0) {
            timer_armed = 1;
            spin_lock(&fb_state.lock);
            fb_state.stats.syncobj_timeout_waits++;
            spin_unlock(&fb_state.lock);
        } else {
            timeout_ms = 0;
        }
    }

    spin_lock(&fb_state.lock);
    fb_state.stats.syncobj_waits++;
    gpu_syncobj_attach_owner_resv_locked(owner,
                                         FB_GPU_RESV_ATTACH_SYNCOBJ_WAIT);
    for (;;) {
        ret = gpu_syncobj_wait_ready_locked(owner, handles, points, count,
                                            flags, &first);
        if (ret != 0 || first >= 0 ||
            ((flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) && first == 0))
            break;
        if (finite_timeout &&
            (timeout_ms == 0 || sched_timer_now_ms() >= deadline_ms)) {
            fb_state.stats.syncobj_stale_wait_rejects++;
            ret = -ETIME;
            break;
        }
        uint32 armed_count = gpu_syncobj_wait_arm_callbacks_locked(
            owner, handles, points, count, armed_states, armed_generations);
        fb_state.stats.syncobj_wait_queued++;
        fb_state.syncobj_waiters++;
        ret = sleep_on_chan_interruptible(&fb_state.syncobj_wakeup_seq,
                                          &fb_state.lock);
        if (fb_state.syncobj_waiters > 0)
            fb_state.syncobj_waiters--;
        gpu_syncobj_wait_cancel_callbacks_locked(
            armed_states, armed_generations, armed_count);
        if (ret != 0)
            break;
    }
    if (ret == 0 && first < 0) {
        fb_state.stats.syncobj_stale_wait_rejects++;
        ret = -ETIME;
    }
    spin_unlock(&fb_state.lock);
    (void)timer_armed;
    if (ret != 0)
        return ret;
    if (timeline) {
        twait.first_signaled = (uint32)first;
        if (either_copyout(1, arg, &twait, sizeof(twait)) < 0)
            return -EFAULT;
    } else {
        wait.first_signaled = (uint32)first;
        if (either_copyout(1, arg, &wait, sizeof(wait)) < 0)
            return -EFAULT;
    }
    return 0;
}

static int gpu_syncobj_query(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_syncobj_timeline_array_compat req;

    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.count_handles == 0 || req.count_handles > 64 ||
        (req.flags & ~DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) != 0)
        return -EINVAL;
    for (uint32 i = 0; i < req.count_handles; i++) {
        uint32 handle;
        uint64 point;
        int ret = gpu_syncobj_copy_handle(req.handles, i, &handle);
        if (ret != 0)
            return ret;
        struct fb_gpu_syncobj_entry *obj;
        struct fb_gpu_syncobj_state_entry *state;

        spin_lock(&fb_state.lock);
        obj = gpu_syncobj_lookup_locked(handle, owner);
        state = obj != NULL ? gpu_syncobj_state_locked(obj->state_index) :
            NULL;
        if (obj == NULL || state == NULL) {
            spin_unlock(&fb_state.lock);
            return -ENOENT;
        }
        (void)gpu_syncobj_state_ready_locked(state, 1);
        point = state->timeline_value;
        spin_unlock(&fb_state.lock);
        ret = gpu_syncobj_write_point(req.points, i, point);
        if (ret != 0)
            return ret;
    }
    return 0;
}

static int gpu_syncobj_transfer(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_syncobj_transfer_compat req;
    struct fb_gpu_syncobj_entry *src;
    struct fb_gpu_syncobj_entry *dst;
    struct fb_gpu_syncobj_state_entry *src_state;
    struct fb_gpu_syncobj_state_entry *dst_state;
    int ret = 0;

    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.flags != 0 || req.pad != 0)
        return -EINVAL;
    spin_lock(&fb_state.lock);
    src = gpu_syncobj_lookup_locked(req.src_handle, owner);
    dst = gpu_syncobj_lookup_locked(req.dst_handle, owner);
    src_state = src != NULL ? gpu_syncobj_state_locked(src->state_index) :
        NULL;
    dst_state = dst != NULL ? gpu_syncobj_state_locked(dst->state_index) :
        NULL;
    if (src == NULL || dst == NULL || src_state == NULL ||
        dst_state == NULL) {
        ret = -ENOENT;
    } else if (!src_state->signaled ||
               src_state->timeline_value < req.src_point) {
        ret = -ETIME;
    } else {
        gpu_syncobj_signal_state_locked(dst_state, req.dst_point);
        dst_state->reservation_fence = src_state->reservation_fence;
        gpu_syncobj_attach_owner_resv_locked(
            owner, FB_GPU_RESV_ATTACH_SYNCOBJ_SIGNAL);
    }
    spin_unlock(&fb_state.lock);
    return ret;
}

static int gpu_syncobj_eventfd(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_syncobj_eventfd_compat req;
    struct fb_gpu_syncobj_entry *obj;
    int ret = 0;

    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if ((req.flags & ~DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) != 0 ||
        req.pad != 0 || req.fd < 0)
        return -EINVAL;
    spin_lock(&fb_state.lock);
    obj = gpu_syncobj_lookup_locked(req.handle, owner);
    if (obj == NULL)
        ret = -ENOENT;
    spin_unlock(&fb_state.lock);
    if (ret != 0)
        return ret;
    return -EOPNOTSUPP;
}

static uint32 gpu_syncobj_destroy_owner(struct fb_gpu_render_owner *owner)
{
    uint32 stale = 0;

    if (owner == NULL)
        return 0;
    spin_lock(&fb_state.lock);
    for (uint32 i = 0; i < FB_GPU_MAX_SYNCOBJS; i++) {
        struct fb_gpu_syncobj_entry *obj = &fb_state.syncobjs[i];
        if (!obj->in_use || obj->owner_id != owner->id ||
            obj->owner_tgid != owner->tgid)
            continue;
        gpu_syncobj_destroy_handle_locked(obj);
        stale++;
    }
    spin_unlock(&fb_state.lock);
    return stale;
}

static int gpu_drm_create_dumb(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_mode_create_dumb_compat req;
    uint64 size;
    uint32 npages;
    page_t **pages;
    uint32 handle;
    int ret;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.width == 0 || req.height == 0 || req.bpp == 0 ||
        req.bpp > 64 || req.flags != 0 || req.width > 8192 ||
        req.height > 8192)
        return -EINVAL;

    req.pitch = req.width * ((req.bpp + 7) / 8);
    if (req.pitch == 0 || (uint64)req.pitch > ((uint64)-1) / req.height)
        return -EINVAL;
    size = (uint64)req.pitch * req.height;
    if (size == 0 || size > 256ULL * 1024 * 1024)
        return -EINVAL;
    req.size = PGROUNDUP(size);
    npages = req.size / PGSIZE;

    ret = fb_bo_alloc_pages(npages, &pages);
    if (ret != 0)
        return ret;

    ret = fb_bo_register(owner->id, owner->tgid, req.width, req.height,
                         req.pitch, req.size, pages, npages, &handle);
    if (ret != 0) {
        fb_bo_release_pages(pages, npages);
        return ret;
    }
    req.handle = handle;
    spin_lock(&fb_state.lock);
    fb_state.stats.bo_allocs++;
    fb_state.stats.bo_bytes += req.size;
    spin_unlock(&fb_state.lock);

    if (either_copyout(1, arg, &req, sizeof(req)) < 0) {
        (void)fb_bo_destroy_owned(req.handle, owner->id, owner->tgid);
        return -EFAULT;
    }
    return 0;
}

static int gpu_drm_prime_handle_to_fd(struct fb_gpu_render_owner *owner,
                                      uint64 arg)
{
    struct drm_prime_handle_compat req;
    struct fb_gpu_bo_entry *bo;
    struct fb_gpu_dmabuf_object *dmabuf;
    int fd;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    bo = fb_bo_get_owned(req.handle, owner->id, owner->tgid);
    if (bo == NULL)
        return -ENOENT;
    dmabuf = fb_dmabuf_create_from_bo(bo, FB_GPU_DMABUF_TAG_DRM_PRIME);
    fb_bo_put(bo);
    if (dmabuf == NULL)
        return -ENOENT;
    fd = vfs_custom_fd_alloc(&fb_dmabuf_file_ops, dmabuf, 0);
    if (fd < 0) {
        fb_dmabuf_put(dmabuf);
        return fd;
    }
    req.fd = fd;
    fb_dmabuf_note_export(dmabuf);
    if (either_copyout(1, arg, &req, sizeof(req)) < 0) {
        fb_gpu_close_exported_fd(fd);
        return -EFAULT;
    }
    return 0;
}

static int gpu_drm_prime_fd_to_handle(struct fb_gpu_render_owner *owner,
                                      uint64 arg)
{
    struct drm_prime_handle_compat req;
    struct vfs_file *file;
    struct fb_gpu_bo_entry *bo;
    struct fb_gpu_dmabuf_object *dmabuf;
    uint32 handle;
    int ret;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.fd < 0) {
        fb_dmabuf_note_bad_fd_reject();
        return -EBADF;
    }
    file = vfs_fdtable_get_file(current->fdtable, req.fd);
    if (file == NULL) {
        fb_dmabuf_note_bad_fd_reject();
        return -EBADF;
    }
    if (file->ops != &fb_dmabuf_file_ops || file->private_data == NULL) {
        fb_dmabuf_note_foreign_fd_reject();
        vfs_fput(file);
        return -EINVAL;
    }
    dmabuf = (struct fb_gpu_dmabuf_object *)file->private_data;
    ret = fb_bo_register_gem(owner->id, owner->tgid, dmabuf->gem, &handle);
    if (ret != 0) {
        vfs_fput(file);
        return ret;
    }
    bo = fb_bo_get(handle);
    if (bo == NULL) {
        (void)fb_bo_destroy(handle);
        vfs_fput(file);
        return -ENOENT;
    }
    spin_lock(&fb_state.lock);
    fb_dmabuf_note_import_locked(dmabuf, bo, FB_GPU_DMABUF_TAG_DRM_PRIME);
    spin_unlock(&fb_state.lock);
    fb_bo_put(bo);
    vfs_fput(file);
    req.handle = handle;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0) {
        (void)fb_bo_destroy_owned(handle, owner->id, owner->tgid);
        return -EFAULT;
    }
    return 0;
}

static int gpu_drm_virtgpu_resource_create(struct fb_gpu_render_owner *owner,
                                           uint64 arg, int blob)
{
    struct fb_gpu_virgl_resource_create create;
    uint32 out_handle = 0;
    uint64 out_size = 0;
    uint32 out_stride = 0;
    int ret;

    ret = gpu_owner_ensure_context(owner);
    if (ret != 0)
        return ret;

    memset(&create, 0, sizeof(create));
    create.ctx_id = owner->default_ctx_id;
    if (blob) {
        struct drm_virtgpu_resource_create_blob_compat req;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.size == 0 ||
            (req.blob_mem != VIRTGPU_BLOB_MEM_GUEST &&
             req.blob_mem != VIRTGPU_BLOB_MEM_HOST3D &&
             req.blob_mem != VIRTGPU_BLOB_MEM_HOST3D_GUEST))
            return -EINVAL;
        create.width = req.size > 4096 ? 4096 : (uint32)req.size;
        create.height = (uint32)((req.size + create.width - 1) / create.width);
        create.depth = 1;
        create.array_size = 1;
        create.format = 1; /* B8G8R8A8_UNORM */
        create.bind = 0;
        create.size = req.size;
        ret = virtio_gpu_user_resource_create(owner->id, owner->tgid, &create);
        if (ret != 0)
            return ret;
        if (create.addr != 0 && create.size != 0)
            (void)vm_munmap(current->vm, create.addr, (size_t)create.size);
        req.bo_handle = create.resource_id;
        req.res_handle = create.resource_id;
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    } else {
        struct drm_virtgpu_resource_create_compat req;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        create.target = req.target;
        create.format = req.format;
        create.bind = req.bind;
        create.width = req.width;
        create.height = req.height;
        create.depth = req.depth;
        create.array_size = req.array_size;
        create.last_level = req.last_level;
        create.nr_samples = req.nr_samples;
        create.flags = req.flags;
        create.size = req.size;
        ret = virtio_gpu_user_resource_create(owner->id, owner->tgid, &create);
        if (ret != 0)
            return ret;
        if (create.addr != 0 && create.size != 0)
            (void)vm_munmap(current->vm, create.addr, (size_t)create.size);
        out_handle = create.resource_id;
        out_size = create.size;
        out_stride = req.stride ? req.stride : create.width * 4;
        req.bo_handle = out_handle;
        req.res_handle = out_handle;
        req.size = (uint32)out_size;
        req.stride = out_stride;
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }
}

static int gpu_drm_virtgpu_transfer(struct fb_gpu_render_owner *owner,
                                    uint64 arg, int from_host)
{
    struct drm_virtgpu_3d_transfer_compat req;
    struct fb_gpu_virgl_transfer transfer;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    memset(&transfer, 0, sizeof(transfer));
