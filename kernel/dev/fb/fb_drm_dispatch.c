#include <signal.h>

#define GPU_DRM_FENCE_WAIT_TIMEOUT_MS 60000

static int chrome_drm_ioctl_trace_enabled(void);
static int chrome_drm_current_is_chrome(void);
static int chrome_drm_trace_owner(const struct fb_gpu_render_owner *owner);
int virtio_gpu_user_creatable_capset_ids(uint64 *ids);
int virtio_gpu_user_capset_query_only(uint32 capset_id);
int virtio_gpu_user_resource_blob_supported(void);

static uint64 gpu_drm_ticks_to_us(uint64 ticks)
{
    extern uint64 __timebase_frequency;
    uint64 freq = __timebase_frequency ? __timebase_frequency : 10000000UL;

    if (ticks <= (uint64)-1 / 1000000ULL)
        return ticks * 1000000ULL / freq;
    return ticks / (freq / 1000000ULL ? freq / 1000000ULL : 1ULL);
}

static int chrome_drm_fence_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("chrome_drm_fence_trace", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static int gpu_drm_gem_close(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_gem_close_compat req;
    struct fb_gpu_bo_entry *bo;
    uint32 virtio_resource_id = 0;
    uint64 virtio_owner_id = 0;
    pid_t virtio_owner_tgid = 0;
    int ret;

    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    bo = fb_bo_get_owned(req.handle, owner->id, owner->tgid);
    if (bo != NULL) {
        virtio_resource_id = bo->virtio_resource_id;
        virtio_owner_id = bo->virtio_resource_owner_id;
        virtio_owner_tgid = bo->virtio_resource_owner_tgid;
        fb_bo_put(bo);
    }
    ret = fb_bo_destroy_owned(req.handle, owner->id, owner->tgid);
    if (ret == 0 && virtio_resource_id != 0 &&
        virtio_owner_id == owner->id)
        (void)virtio_gpu_user_resource_destroy(virtio_owner_id,
                                               virtio_owner_tgid,
                                               virtio_resource_id);
    if (ret == -ENOENT)
        ret = virtio_gpu_user_resource_destroy(owner->id, owner->tgid,
                                               req.handle);
    return ret;
}

static int gpu_drm_execbuffer_async_allowed(void)
{
    if (!fb_cmdline_enabled("vgpu_async_pf") &&
        !fb_cmdline_enabled("virtio_gpu_async_submit_3d"))
        return 0;
    return 1;
}

static int gpu_drm_virtgpu_resource_for_bo_handle(
    struct fb_gpu_render_owner *owner, uint32 bo_handle, uint32 *resource_id,
    uint64 *resource_owner_id, pid_t *resource_owner_tgid, int *imported)
{
    struct fb_gpu_bo_entry *bo;
    uint32 id = 0;
    uint64 id_owner = 0;
    pid_t id_tgid = 0;
    int ret;

    if (owner == NULL)
        return -EBADF;
    if (resource_id == NULL || bo_handle == 0)
        return -EINVAL;

    bo = fb_bo_get_owned(bo_handle, owner->id, owner->tgid);
    if (bo == NULL)
        return -ENOENT;

    id = bo->virtio_resource_id;
    id_owner = bo->virtio_resource_owner_id;
    id_tgid = bo->virtio_resource_owner_tgid;
    if (id == 0 && bo->gem != NULL) {
        id = bo->gem->virtio_resource_id;
        id_owner = bo->gem->virtio_resource_owner_id;
        id_tgid = bo->gem->virtio_resource_owner_tgid;
    }
    fb_bo_put(bo);

    if (id == 0)
        return -ENOENT;
    if (id_owner == 0 && id_tgid <= 0) {
        id_owner = owner->id;
        id_tgid = owner->tgid;
    }
    ret = virtio_gpu_user_resource_info(id_owner, id_tgid, id,
                                        NULL, NULL, NULL, NULL);
    if (ret != 0)
        return ret;
    *resource_id = id;
    if (resource_owner_id != NULL)
        *resource_owner_id = id_owner;
    if (resource_owner_tgid != NULL)
        *resource_owner_tgid = id_tgid;
    if (imported != NULL)
        *imported = id_owner != owner->id;
    return 0;
}

static int gpu_drm_wait_virtio_fence(uint64 fence, uint64 *signaled_out)
{
    uint64 deadline_ms;
    uint64 signaled = 0;
    int ret;

    if (fence == 0)
        return -EINVAL;

    deadline_ms = sched_timer_now_ms() + GPU_DRM_FENCE_WAIT_TIMEOUT_MS;
    for (;;) {
        ret = virtio_gpu_user_fence(fence, 1, &signaled);
        if (signaled_out != NULL)
            *signaled_out = signaled;
        if (ret != 0 && ret != -EAGAIN)
            return ret;
        if (signaled >= fence)
            return 0;
        if (signal_pending(current))
            return -EINTR;
        if (sched_timer_now_ms() >= deadline_ms)
            return -ETIME;
        sleep_ms(1);
    }
}

static void gpu_drm_trace_execbuffer_fence_wait(
    const struct fb_gpu_render_owner *owner, int32 fd, const char *kind,
    int ret, uint64 target, uint64 signaled, uint64 virtio_fence,
    uint64 reservation_fence, uint64 snapshot_point, int snapshot_signaled)
{
    static _Atomic int printed;
    int slot;

    if (!chrome_drm_fence_trace_enabled() &&
        !chrome_drm_ioctl_trace_enabled())
        return;
    slot = __atomic_fetch_add(&printed, 1, __ATOMIC_RELAXED);
    if (slot >= 64)
        return;
    printf("chrome-drm-fence-wait: fd=%d kind=%s ret=%d target=%lu "
           "signaled=%lu virtio=%lu reservation=%lu point=%lu "
           "snapshot=%d owner=%lu:%d pid=%d name=%s\n",
           fd, kind ? kind : "?", ret, target, signaled, virtio_fence,
           reservation_fence, snapshot_point, snapshot_signaled,
           owner ? owner->id : 0, owner ? owner->tgid : -1,
           current ? current->pid : -1, current ? current->name : "?");
}

static int gpu_drm_execbuffer_wait_fence_fd(
    const struct fb_gpu_render_owner *owner, int32 fd)
{
    struct vfs_file *file = NULL;
    const char *kind = "none";
    uint64 start_ticks = r_time();
    int ret = 0;

    if (fd < 0) {
        ret = -EINVAL;
        kind = "invalid-fd";
        goto out;
    }
    if (current == NULL || current->fdtable == NULL) {
        ret = -EBADF;
        kind = "no-fdtable";
        goto out;
    }
    file = vfs_fdtable_get_file(current->fdtable, fd);
    if (file == NULL) {
        ret = -EBADF;
        kind = "bad-fd";
        goto out;
    }

    if (file->ops == &fb_fence_file_ops && file->private_data != NULL) {
        struct fb_gpu_fence_file *fence_file =
            (struct fb_gpu_fence_file *)file->private_data;
        uint64 signaled = 0;

        kind = "fb-fence";
        spin_lock(&fb_state.lock);
        ret = fb_gpu_fence_file_wait_locked(fence_file);
        signaled = fb_gpu_fence_file_signaled_locked(fence_file);
        spin_unlock(&fb_state.lock);
        if (ret != 0)
            gpu_drm_trace_execbuffer_fence_wait(
                owner, fd, "fb-fence", ret, fence_file->fence, signaled,
                0, 0, 0, fence_file->fence_obj != NULL &&
                fence_file->fence_obj->signaled);
    } else if (file->ops == &fb_virgl_fence_file_ops &&
               file->private_data != NULL) {
        struct fb_gpu_virgl_fence_file *fence_file =
            (struct fb_gpu_virgl_fence_file *)file->private_data;
        uint64 signaled = 0;

        kind = "virgl-fence";
        ret = gpu_drm_wait_virtio_fence(fence_file->fence, &signaled);
        if (ret != 0)
            gpu_drm_trace_execbuffer_fence_wait(
                owner, fd, "virgl-fence", ret, fence_file->fence,
                signaled, fence_file->fence, 0, 0, 0);
    } else if (file->ops == &fb_syncobj_file_ops &&
               file->private_data != NULL) {
        struct fb_gpu_syncobj_file *sync_file =
            (struct fb_gpu_syncobj_file *)file->private_data;

        kind = "sync-file";
        if (sync_file->kind != FB_GPU_SYNCOBJ_FD_SYNC_FILE) {
            ret = -EINVAL;
            kind = "sync-file-kind";
        } else if (sync_file->virtio_fence != 0 ||
                   (sync_file->fence != NULL &&
                    dma_fence_get_virtio_fence(sync_file->fence) != 0)) {
            uint64 signaled = 0;

            kind = "sync-file-virtio";
            if (sync_file->virtio_fence == 0)
                sync_file->virtio_fence =
                    dma_fence_get_virtio_fence(sync_file->fence);
            ret = gpu_drm_wait_virtio_fence(sync_file->virtio_fence,
                                            &signaled);
            if (ret == 0)
                (void)fb_syncobj_file_status(sync_file);
            else
                gpu_drm_trace_execbuffer_fence_wait(
                    owner, fd, "sync-file-virtio", ret,
                    sync_file->virtio_fence, signaled,
                    sync_file->virtio_fence,
                    sync_file->snapshot_reservation_fence,
                    sync_file->snapshot_timeline_value,
                    sync_file->snapshot_signaled);
        } else if (sync_file->reservation_gem != NULL &&
                   sync_file->reservation_fence != 0) {
            uint64 signaled = 0;

            kind = "sync-file-resv";
            spin_lock(&fb_state.lock);
            ret = fb_ttm_resv_wait_fence_locked(
                sync_file->reservation_gem, sync_file->reservation_fence);
            if (sync_file->reservation_gem->in_use &&
                !sync_file->reservation_gem->dead)
                signaled = sync_file->reservation_gem->signaled_fence;
            spin_unlock(&fb_state.lock);
            if (ret == 0)
                (void)fb_syncobj_file_status(sync_file);
            else
                gpu_drm_trace_execbuffer_fence_wait(
                    owner, fd, "sync-file-resv", ret,
                    sync_file->reservation_fence, signaled,
                    sync_file->virtio_fence,
                    sync_file->snapshot_reservation_fence,
                    sync_file->snapshot_timeline_value,
                    sync_file->snapshot_signaled);
        } else if (sync_file->snapshot_signaled ||
                   dma_fence_is_signaled(sync_file->fence)) {
            kind = "sync-file-signaled";
            ret = 0;
        } else if (sync_file->fence != NULL) {
            int status = fb_syncobj_file_status(sync_file);

            kind = "sync-file-dma";
            if (status > 0)
                ret = 0;
            else if (status < 0)
                ret = status;
            else
                ret = dma_fence_wait_uninterruptible(sync_file->fence);
            if (ret != 0)
                gpu_drm_trace_execbuffer_fence_wait(
                    owner, fd, "sync-file-dma", ret, 0, 0,
                    sync_file->virtio_fence,
                    sync_file->snapshot_reservation_fence,
                    sync_file->snapshot_timeline_value,
                    sync_file->snapshot_signaled);
        } else {
            ret = -EINVAL;
            kind = "sync-file-invalid";
            gpu_drm_trace_execbuffer_fence_wait(
                owner, fd, "sync-file-invalid", ret, 0, 0,
                sync_file->virtio_fence,
                sync_file->snapshot_reservation_fence,
                sync_file->snapshot_timeline_value,
                sync_file->snapshot_signaled);
        }
    } else {
        ret = -EINVAL;
        kind = "unknown";
        gpu_drm_trace_execbuffer_fence_wait(
            owner, fd, "unknown", ret, 0, 0, 0, 0, 0, 0);
    }

out:
    if (file != NULL)
        vfs_fput(file);
    if (chrome_drm_trace_owner(owner) || chrome_drm_fence_trace_enabled()) {
        uint64 total_ticks = r_time() - start_ticks;

        printf("chrome-drm-detail: execbuffer-fence-fd-wait-time "
               "owner=%lu:%d pid=%d proc=%s fd=%d kind=%s ret=%d "
               "total_us=%lu total_ticks=%lu\n",
               owner ? owner->id : 0, owner ? owner->tgid : -1,
               current ? current->pid : -1, current ? current->name : "?",
               fd, kind, ret, gpu_drm_ticks_to_us(total_ticks),
               total_ticks);
    }
    return ret;
}

static int gpu_drm_execbuffer_export_fence_fd(uint64 fence_id, int signaled,
                                              int *fd_out)
{
    struct fb_gpu_syncobj_file *sync_file;
    struct dma_fence *fence;
    int fd;

    if (fd_out == NULL || fence_id == 0)
        return -EINVAL;
    *fd_out = -1;

    fence = kvmalloc(sizeof(*fence));
    if (fence == NULL)
        return -ENOMEM;
    dma_fence_init(fence, 0, fence_id);
    dma_fence_set_virtio_fence(fence, fence_id);
    if (signaled)
        (void)dma_fence_signal(fence, 0);

    sync_file = kvmalloc(sizeof(*sync_file));
    if (sync_file == NULL) {
        dma_fence_put(fence);
        return -ENOMEM;
    }
    memset(sync_file, 0, sizeof(*sync_file));
    sync_file->kind = FB_GPU_SYNCOBJ_FD_SYNC_FILE;
    sync_file->snapshot_signaled = signaled != 0;
    sync_file->snapshot_timeline_value = fence_id;
    sync_file->snapshot_reservation_fence = fence_id;
    sync_file->virtio_fence = fence_id;
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
    dma_fence_put(fence);
    *fd_out = fd;
    return 0;
}

static int gpu_drm_ioctl_handle(struct drm_core_file *drm_file,
                                void *driver_file, uint64 cmd, uint64 arg)
{
    struct fb_gpu_render_owner *owner =
        (struct fb_gpu_render_owner *)driver_file;

    (void)drm_file;
    if (owner == NULL)
        return -EBADF;
    switch (cmd) {
    case DRM_IOCTL_VERSION:
        return gpu_drm_version(arg);
    case DRM_IOCTL_GET_UNIQUE:
        return gpu_drm_get_unique(arg);
    case DRM_IOCTL_GET_MAGIC:
        return gpu_drm_get_magic(owner, arg);
    case DRM_IOCTL_GET_MAP:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_get_map(arg);
    case DRM_IOCTL_AUTH_MAGIC:
        return gpu_drm_auth_magic(owner, arg);
    case DRM_IOCTL_GET_CLIENT:
        return gpu_drm_get_client(owner, arg);
    case DRM_IOCTL_GET_STATS:
        return gpu_drm_get_stats(arg);
    case DRM_IOCTL_SET_VERSION:
        return gpu_drm_set_version(arg);
    case DRM_IOCTL_GET_CAP:
        return gpu_drm_get_cap(owner, arg);
    case DRM_IOCTL_SET_CLIENT_CAP:
        return gpu_drm_set_client_cap(owner, arg);
    case DRM_IOCTL_SET_CLIENT_NAME:
        return gpu_drm_set_client_name(owner, arg);
    case DRM_IOCTL_ADD_MAP:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_add_rm_map(arg, 0);
    case DRM_IOCTL_RM_MAP:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_add_rm_map(arg, 1);
    case DRM_IOCTL_SET_SAREA_CTX:
    case DRM_IOCTL_GET_SAREA_CTX:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_sarea_ctx(arg);
    case DRM_IOCTL_ADD_BUFS:
    case DRM_IOCTL_MARK_BUFS:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_buf_desc(arg);
    case DRM_IOCTL_INFO_BUFS:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_info_bufs(arg);
    case DRM_IOCTL_MAP_BUFS:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_map_bufs(arg);
    case DRM_IOCTL_FREE_BUFS:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_free_bufs(arg);
    case DRM_IOCTL_SET_MASTER:
        return gpu_drm_set_master(owner);
    case DRM_IOCTL_DROP_MASTER:
        return gpu_drm_drop_master(owner);
    case DRM_IOCTL_ADD_CTX:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_ctx_mutate(arg, 1);
    case DRM_IOCTL_RM_CTX:
    case DRM_IOCTL_MOD_CTX:
    case DRM_IOCTL_SWITCH_CTX:
    case DRM_IOCTL_NEW_CTX:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_ctx_mutate(arg, 0);
    case DRM_IOCTL_GET_CTX:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_get_ctx(arg);
    case DRM_IOCTL_RES_CTX:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_res_ctx(arg);
    case DRM_IOCTL_DMA:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_dma(arg);
    case DRM_IOCTL_LOCK:
    case DRM_IOCTL_UNLOCK:
    case DRM_IOCTL_FINISH:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_lock(arg);
    case DRM_IOCTL_GEM_CLOSE:
        return gpu_drm_gem_close(owner, arg);
    case DRM_IOCTL_GEM_FLINK: {
        struct drm_gem_flink_compat req;
        int ret;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        ret = fb_gem_flink(req.handle, owner->id, owner->tgid, &req.name);
        if (ret != 0)
            return ret;
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }
    case DRM_IOCTL_GEM_OPEN: {
        struct drm_gem_open_compat req;
        int ret;
        int handle_created = 0;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.name == 0)
            return -EINVAL;
        ret = fb_gem_open_name(owner->id, owner->tgid, req.name,
                               &req.handle, &req.size, &handle_created);
        if (ret != 0)
            return ret;
        if (either_copyout(1, arg, &req, sizeof(req)) < 0) {
            if (handle_created)
                (void)fb_bo_destroy_owned(req.handle, owner->id,
                                          owner->tgid);
            return -EFAULT;
        }
        return 0;
    }
    case DRM_IOCTL_PRIME_HANDLE_TO_FD:
        return gpu_drm_prime_handle_to_fd(owner, arg);
    case DRM_IOCTL_PRIME_FD_TO_HANDLE:
        return gpu_drm_prime_fd_to_handle(owner, arg);
    case DRM_IOCTL_AGP_ACQUIRE:
    case DRM_IOCTL_AGP_RELEASE:
    case DRM_IOCTL_AGP_INFO:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return -EOPNOTSUPP;
    case DRM_IOCTL_AGP_ENABLE:
    case DRM_IOCTL_AGP_ALLOC:
    case DRM_IOCTL_AGP_FREE:
    case DRM_IOCTL_AGP_BIND:
    case DRM_IOCTL_AGP_UNBIND:
    case DRM_IOCTL_SG_ALLOC:
    case DRM_IOCTL_SG_FREE:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_agp_struct(arg, cmd);
    case DRM_IOCTL_WAIT_VBLANK:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_wait_vblank(owner, arg);
    case DRM_IOCTL_CRTC_GET_SEQUENCE:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_crtc_get_sequence(arg);
    case DRM_IOCTL_CRTC_QUEUE_SEQUENCE:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_crtc_queue_sequence(owner, arg);
    case DRM_IOCTL_MODE_GETRESOURCES:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getresources(owner, arg);
    case DRM_IOCTL_MODE_GETCRTC:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getcrtc(arg);
    case DRM_IOCTL_MODE_SETCRTC:
        return gpu_drm_mode_setcrtc(owner, arg);
    case DRM_IOCTL_MODE_CURSOR:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_cursor(owner, arg, 0);
    case DRM_IOCTL_MODE_GETGAMMA:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_gamma(arg, 0);
    case DRM_IOCTL_MODE_SETGAMMA:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_gamma(arg, 1);
    case DRM_IOCTL_MODE_GETENCODER:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getencoder(arg);
    case DRM_IOCTL_MODE_GETCONNECTOR:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getconnector(arg);
    case DRM_IOCTL_MODE_GETPROPERTY:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getproperty(arg);
    case DRM_IOCTL_MODE_GETPROPBLOB:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getblob(arg);
    case DRM_IOCTL_MODE_GETFB:
        return gpu_drm_mode_getfb(owner, arg);
    case DRM_IOCTL_MODE_ADDFB:
        return gpu_drm_mode_addfb(owner, arg);
    case DRM_IOCTL_MODE_GETPLANERESOURCES:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getplaneresources(arg);
    case DRM_IOCTL_MODE_GETPLANE:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getplane(arg);
    case DRM_IOCTL_MODE_SETPLANE:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_setplane(owner, arg);
    case DRM_IOCTL_MODE_ADDFB2:
        return gpu_drm_mode_addfb2(owner, arg);
    case DRM_IOCTL_MODE_OBJ_GETPROPERTIES:
        return gpu_drm_mode_obj_getproperties(owner, arg);
    case DRM_IOCTL_MODE_OBJ_SETPROPERTY:
        return gpu_drm_mode_obj_setproperty(owner, arg);
    case DRM_IOCTL_MODE_RMFB:
        return gpu_drm_mode_rmfb(owner, arg);
    case DRM_IOCTL_MODE_PAGE_FLIP:
        return gpu_drm_mode_page_flip(owner, arg);
    case DRM_IOCTL_MODE_DIRTYFB:
        return gpu_drm_mode_dirtyfb(owner, arg);
    case DRM_IOCTL_MODE_ATOMIC:
        return gpu_drm_mode_atomic(owner, arg);
    case DRM_IOCTL_MODE_CURSOR2:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_cursor(owner, arg, 1);
    case DRM_IOCTL_MODE_CREATEPROPBLOB: {
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_createblob(arg);
    }
    case DRM_IOCTL_MODE_DESTROYPROPBLOB: {
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_destroyblob(arg);
    }
    case DRM_IOCTL_MODE_CREATE_LEASE:
    case DRM_IOCTL_MODE_LIST_LESSEES:
    case DRM_IOCTL_MODE_GET_LEASE:
    case DRM_IOCTL_MODE_REVOKE_LEASE:
        return gpu_drm_mode_lease_fail_closed(owner, cmd, arg);
    case DRM_IOCTL_SYNCOBJ_CREATE:
        return gpu_syncobj_create(owner, arg);
    case DRM_IOCTL_SYNCOBJ_DESTROY:
        return gpu_syncobj_destroy(owner, arg);
    case DRM_IOCTL_XV6_SYNCOBJ_HANDLE_TO_FD_LEGACY:
        return gpu_syncobj_handle_to_fd(owner, arg, 1);
    case DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD:
        return gpu_syncobj_handle_to_fd(owner, arg, 0);
    case DRM_IOCTL_XV6_SYNCOBJ_FD_TO_HANDLE_LEGACY:
        return gpu_syncobj_fd_to_handle(owner, arg, 1);
    case DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE:
        return gpu_syncobj_fd_to_handle(owner, arg, 0);
    case DRM_IOCTL_SYNCOBJ_WAIT:
        return gpu_syncobj_wait_common(owner, arg, 0);
    case DRM_IOCTL_SYNCOBJ_RESET:
        return gpu_syncobj_array_signal_reset(owner, arg, 0, 0);
    case DRM_IOCTL_SYNCOBJ_SIGNAL:
        return gpu_syncobj_array_signal_reset(owner, arg, 1, 0);
    case DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT:
        return gpu_syncobj_wait_common(owner, arg, 1);
    case DRM_IOCTL_SYNCOBJ_QUERY:
        return gpu_syncobj_query(owner, arg);
    case DRM_IOCTL_SYNCOBJ_TRANSFER:
        return gpu_syncobj_transfer(owner, arg);
    case DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL:
        return gpu_syncobj_array_signal_reset(owner, arg, 1, 1);
    case DRM_IOCTL_MODE_GETFB2:
        return gpu_drm_mode_getfb2(owner, arg);
    case DRM_IOCTL_SYNCOBJ_EVENTFD:
        return gpu_syncobj_eventfd(owner, arg);
    case DRM_IOCTL_MODE_CLOSEFB: {
        return gpu_drm_mode_closefb(owner, arg);
    }
    case DRM_IOCTL_MODE_CREATE_DUMB:
        return gpu_drm_create_dumb(owner, arg);
    case DRM_IOCTL_MODE_MAP_DUMB:
    {
        struct drm_mode_map_dumb_compat req;
        struct fb_gpu_bo_entry *bo;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.handle == 0)
            return -EINVAL;
        bo = fb_bo_get_owned(req.handle, owner->id, owner->tgid);
        if (bo == NULL)
            return -ENOENT;
        req.offset = GPU_DRM_MMAP_OFFSET(req.handle);
        fb_bo_put(bo);
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_MAP: {
        struct drm_virtgpu_map_compat req;
        uint32 resource_id = 0;
        uint64 resource_owner_id = 0;
        pid_t resource_owner_tgid = 0;
        uint64 blob_offset = 0;
        uint32 blob_mem = 0;
        int ret;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.handle == 0)
            return -EINVAL;
        ret = gpu_drm_virtgpu_resource_for_bo_handle(owner, req.handle,
                                                     &resource_id,
                                                     &resource_owner_id,
                                                     &resource_owner_tgid,
                                                     NULL);
        if (ret != 0)
            return ret;
        ret = virtio_gpu_user_resource_map_offset(resource_owner_id,
                                                  resource_owner_tgid,
                                                  resource_id,
                                                  &blob_offset);
        if (ret == 0) {
            req.offset = blob_offset;
            if (either_copyout(1, arg, &req, sizeof(req)) < 0)
                return -EFAULT;
            return 0;
        }
        if (virtio_gpu_user_resource_blob_mem(resource_owner_id,
                                              resource_owner_tgid,
                                              resource_id, &blob_mem) == 0 &&
            blob_mem == VIRTGPU_BLOB_MEM_HOST3D)
            return ret;
        req.offset = GPU_DRM_MMAP_OFFSET(req.handle);
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }
    case DRM_IOCTL_MODE_DESTROY_DUMB: {
        struct drm_mode_destroy_dumb_compat req;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        return fb_bo_destroy_owned(req.handle, owner->id, owner->tgid);
    }
    case DRM_IOCTL_VIRTGPU_GETPARAM: {
        struct drm_virtgpu_getparam_compat req;
        uint64 value = 0;
        uint32 value32;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        switch (req.param) {
        case VIRTGPU_PARAM_3D_FEATURES:
            value = virtio_gpu_has_virgl() ? 1 : 0;
            break;
        case VIRTGPU_PARAM_CONTEXT_INIT:
        case VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME:
            value = virtio_gpu_has_context_init() ? 1 : 0;
            break;
        case VIRTGPU_PARAM_CAPSET_QUERY_FIX:
            value = 1;
            break;
        case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs:
            if (virtio_gpu_user_capset_ids(&value) != 0)
                value = 0;
            break;
        case VIRTGPU_PARAM_RESOURCE_BLOB:
            value = virtio_gpu_user_resource_blob_supported() ? 1 : 0;
            break;
        case VIRTGPU_PARAM_HOST_VISIBLE:
            value = virtio_gpu_has_host_visible() ? 1 : 0;
            break;
        case VIRTGPU_PARAM_CROSS_DEVICE:
            value = 0;
            break;
        default:
            return -EINVAL;
        }
        if (req.value == 0)
            return -EFAULT;
        /* Linux virtgpu GETPARAM writes only the low 32 bits. Mesa zeroes the
         * 64-bit destination before calling this ioctl. */
        value32 = (uint32)value;
        if (either_copyout(1, req.value, &value32, sizeof(value32)) < 0)
            return -EFAULT;
        if (chrome_drm_trace_owner(owner))
            printf("chrome-drm-detail: getparam owner=%lu:%d param=%lu "
                   "value=%lu value_ptr=0x%lx\n",
                   owner->id, owner->tgid, req.param, value, req.value);
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_CONTEXT_INIT: {
        struct drm_virtgpu_context_init_compat req;
        struct drm_virtgpu_context_set_param_compat param;
        char debug_name[64];
        uint32 capset_id = 0;
        uint32 context_init = 0;
        uint32 num_rings = 1;
        uint64 ring_idx_mask = 0;
        uint64 creatable_capsets = 0;
        uint32 seen_params = 0;
        int explicit_debug_name = 0;
        uint32 i;
        int ret;

        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.pad != 0 || req.num_params > 16)
            return -EINVAL;
        if (!virtio_gpu_has_context_init())
            return -EINVAL;
        if (gpu_owner_context_created(owner))
            return -EEXIST;
        ret = virtio_gpu_user_creatable_capset_ids(&creatable_capsets);
        if (ret != 0)
            return ret;
        memcpy(debug_name, "xv6-drm", sizeof("xv6-drm"));

        if (req.num_params != 0 && req.ctx_set_params == 0)
            return -EFAULT;
        if (chrome_drm_trace_owner(owner))
            printf("chrome-drm-detail: context-init begin owner=%lu:%d "
                   "num_params=%u params=0x%lx\n",
                   owner->id, owner->tgid, req.num_params,
                   req.ctx_set_params);
        for (i = 0; i < req.num_params; i++) {
            uint64 user_param = req.ctx_set_params +
                (uint64)i * sizeof(param);
            uint32 param_bit;

            if (either_copyin(&param, 1, user_param, sizeof(param)) < 0)
                return -EFAULT;
            if (chrome_drm_trace_owner(owner))
                printf("chrome-drm-detail: context-param owner=%lu:%d "
                       "index=%u param=%lu value=%lu\n",
                       owner->id, owner->tgid, i, param.param, param.value);

            if (param.param == 0 || param.param > 31)
                return -EINVAL;
            param_bit = 1U << (uint32)param.param;
            if ((seen_params & param_bit) != 0)
                return -EINVAL;
            seen_params |= param_bit;

            switch (param.param) {
            case VIRTGPU_CONTEXT_PARAM_CAPSET_ID:
                if (param.value == 0 || param.value >= 64 ||
                    (creatable_capsets & (1ULL << param.value)) == 0) {
                    const char *reason =
                        virtio_gpu_user_capset_query_only(param.value) ?
                        "query-only" : "unsupported";
                    if (chrome_drm_trace_owner(owner))
                        printf("chrome-drm-detail: context-init reject "
                               "owner=%lu:%d param=capset value=%lu "
                               "creatable=0x%lx reason=%s ret=%d\n",
                               owner->id, owner->tgid, param.value,
                               creatable_capsets, reason, -EINVAL);
                    return -EINVAL;
                }
                capset_id = (uint32)param.value;
                context_init = (context_init & ~0xffU) |
                    (capset_id & 0xffU);
                break;
            case VIRTGPU_CONTEXT_PARAM_DEBUG_NAME:
                ret = gpu_user_debug_name(param.value, debug_name);
                if (ret != 0)
                    return ret;
                explicit_debug_name = 1;
                break;
            case VIRTGPU_CONTEXT_PARAM_NUM_RINGS:
                if (param.value == 0 ||
                    param.value > FB_GPU_VIRTGPU_CONTEXT_MAX_RINGS) {
                    if (chrome_drm_trace_owner(owner))
                        printf("chrome-drm-detail: context-init reject "
                               "owner=%lu:%d param=num-rings value=%lu "
                               "ret=%d\n",
                               owner->id, owner->tgid, param.value,
                               -EINVAL);
                    return -EINVAL;
                }
                num_rings = (uint32)param.value;
                break;
            case VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK:
                ring_idx_mask = param.value;
                break;
            default:
                return -EINVAL;
            }
        }
        if ((ring_idx_mask & ~((1ULL << num_rings) - 1)) != 0)
            return -EINVAL;
        ret = gpu_owner_create_context(owner, debug_name, capset_id,
                                       context_init, num_rings,
                                       ring_idx_mask,
                                       explicit_debug_name, 1);
        if (chrome_drm_trace_owner(owner)) {
            char safe_debug_name[64];

            gpu_drm_trace_safe_name(debug_name, safe_debug_name);
            printf("chrome-drm-detail: context-init end owner=%lu:%d "
                   "capset=%u context_init=0x%x num_rings=%u "
                   "ring_idx_mask=0x%lx ret=%d debug_name=%s\n",
                   owner->id, owner->tgid, capset_id, context_init,
                   num_rings, ring_idx_mask, ret, safe_debug_name);
        }
        return ret;
    }
    case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE:
        return gpu_drm_virtgpu_resource_create(owner, arg, 0);
    case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB:
        return gpu_drm_virtgpu_resource_create(owner, arg, 1);
    case DRM_IOCTL_VIRTGPU_RESOURCE_INFO: {
        struct drm_virtgpu_resource_info_compat req;
        uint32 resource_id = 0;
        uint64 resource_owner_id = 0;
        pid_t resource_owner_tgid = 0;
        uint32 blob_mem = 0;
        uint64 size = 0;
        int ret;
        if (owner != NULL && owner->nouveau_channel != 0 &&
            gpu_nouveau_device() != NULL)
            return gpu_nouveau_notifier_alloc(owner, arg);
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        ret = gpu_drm_virtgpu_resource_for_bo_handle(owner, req.bo_handle,
                                                     &resource_id,
                                                     &resource_owner_id,
                                                     &resource_owner_tgid,
                                                     NULL);
        if (ret != 0)
            return ret;
        ret = virtio_gpu_user_resource_info(resource_owner_id,
                                            resource_owner_tgid,
                                            resource_id, NULL, NULL, NULL,
                                            &size);
        if (ret != 0)
            return ret;
        ret = virtio_gpu_user_resource_blob_mem(resource_owner_id,
                                                resource_owner_tgid,
                                                resource_id, &blob_mem);
        if (ret != 0)
            return ret;
        req.res_handle = resource_id;
        req.size = (uint32)size;
        req.blob_mem = blob_mem;
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST:
        return gpu_drm_virtgpu_transfer(owner, arg, 1);
    case DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST:
        return gpu_drm_virtgpu_transfer(owner, arg, 0);
    case DRM_IOCTL_VIRTGPU_WAIT: {
        struct drm_virtgpu_3d_wait_compat req = {0};
        uint32 resource_id = 0;
        uint64 resource_owner_id = 0;
        pid_t resource_owner_tgid = 0;
        uint64 wait_for = 0;
        uint64 signaled = 0;
        uint64 total_start = r_time();
        uint64 resolve_ticks = 0;
        uint64 fence_lookup_ticks = 0;
        uint64 fence_wait_ticks = 0;
        uint64 phase_start;
        int traced = chrome_drm_trace_owner(owner);
        int ret;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            goto virtgpu_wait_done;
        }
        if ((req.flags & ~VIRTGPU_WAIT_NOWAIT) != 0) {
            ret = -EINVAL;
            goto virtgpu_wait_done;
        }
        phase_start = r_time();
        ret = gpu_drm_virtgpu_resource_for_bo_handle(owner, req.handle,
                                                     &resource_id,
                                                     &resource_owner_id,
                                                     &resource_owner_tgid,
                                                     NULL);
        resolve_ticks = r_time() - phase_start;
        if (ret != 0)
            goto virtgpu_wait_done;
        phase_start = r_time();
        ret = virtio_gpu_user_resource_last_submit_fence(resource_owner_id,
                                                        resource_owner_tgid,
                                                        resource_id,
                                                        &wait_for);
        fence_lookup_ticks = r_time() - phase_start;
        if (ret != 0)
            goto virtgpu_wait_done;
        if (wait_for == 0) {
            ret = 0;
            goto virtgpu_wait_done;
        }
        phase_start = r_time();
        if (req.flags & VIRTGPU_WAIT_NOWAIT)
            ret = virtio_gpu_user_fence(wait_for, 0, &signaled);
        else
            ret = gpu_drm_wait_virtio_fence(wait_for, &signaled);
        fence_wait_ticks = r_time() - phase_start;
        if (ret != 0)
            goto virtgpu_wait_done;
        if (signaled < wait_for) {
            ret = (req.flags & VIRTGPU_WAIT_NOWAIT) ? -EBUSY : -EAGAIN;
            goto virtgpu_wait_done;
        }
        ret = 0;
virtgpu_wait_done:
        if (traced) {
            uint64 total_ticks = r_time() - total_start;

            printf("chrome-drm-detail: virtgpu-wait-time owner=%lu:%d "
                   "pid=%d proc=%s handle=%u flags=0x%x resource=%u "
                   "resource_owner=%lu:%d wait_for=%lu signaled=%lu ret=%d "
                   "total_us=%lu resolve_us=%lu fence_lookup_us=%lu "
                   "fence_wait_us=%lu total_ticks=%lu resolve_ticks=%lu "
                   "fence_lookup_ticks=%lu fence_wait_ticks=%lu\n",
                   owner ? owner->id : 0, owner ? owner->tgid : -1,
                   current ? current->pid : -1,
                   current ? current->name : "?", req.handle, req.flags,
                   resource_id, resource_owner_id, resource_owner_tgid,
                   wait_for, signaled, ret,
                   gpu_drm_ticks_to_us(total_ticks),
                   gpu_drm_ticks_to_us(resolve_ticks),
                   gpu_drm_ticks_to_us(fence_lookup_ticks),
                   gpu_drm_ticks_to_us(fence_wait_ticks),
                   total_ticks, resolve_ticks, fence_lookup_ticks,
                   fence_wait_ticks);
        }
        return ret;
    }
    case DRM_IOCTL_VIRTGPU_GET_CAPS: {
        struct drm_virtgpu_get_caps_compat req;
        uint32 requested_id;
        uint32 requested_ver;
        uint32 requested_size;
        uint64 requested_addr;
        uint32 capset_id = 0, capset_ver = 0, capset_size = 0;
        uint32 copy_size;
        void *caps = NULL;
        int ret;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        requested_id = req.cap_set_id;
        requested_ver = req.cap_set_ver;
        requested_size = req.size;
        requested_addr = req.addr;

        if (req.addr == 0 || req.size == 0) {
            if (chrome_drm_trace_owner(owner))
                printf("chrome-drm-detail: get-caps fail owner=%lu:%d "
                       "req_id=%u req_ver=%u req_size=%u req_addr=0x%lx "
                       "ret=%d\n",
                       owner->id, owner->tgid, requested_id, requested_ver,
                       requested_size, requested_addr, -EINVAL);
            return -EINVAL;
        }

        caps = kalloc();
        if (caps == NULL)
            return -ENOMEM;

        ret = virtio_gpu_user_get_caps_for(req.cap_set_id, req.cap_set_ver,
                                           caps, req.size, &capset_id,
                                           &capset_ver, &capset_size);
        copy_size = capset_size;
        if (copy_size > requested_size)
            copy_size = requested_size;
        if (ret == 0 && copy_size != 0 &&
            either_copyout(1, req.addr, caps, copy_size) < 0)
            ret = -EFAULT;
        kfree(caps);
        if (ret != 0) {
            if (chrome_drm_trace_owner(owner))
                printf("chrome-drm-detail: get-caps fail owner=%lu:%d "
                       "req_id=%u req_ver=%u req_size=%u req_addr=0x%lx "
                       "out_id=%u out_ver=%u out_size=%u ret=%d\n",
                       owner->id, owner->tgid, requested_id, requested_ver,
                       requested_size, requested_addr, capset_id, capset_ver,
                       capset_size, ret);
            return ret;
        }
        req.cap_set_id = capset_id;
        req.cap_set_ver = capset_ver;
        req.size = capset_size;
        if (chrome_drm_trace_owner(owner))
            printf("chrome-drm-detail: get-caps owner=%lu:%d "
                   "req_id=%u req_ver=%u req_size=%u req_addr=0x%lx "
                   "out_id=%u out_ver=%u out_size=%u copied=%u\n",
                   owner->id, owner->tgid, requested_id, requested_ver,
                   requested_size, requested_addr, capset_id, capset_ver,
                   capset_size, copy_size);
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_EXECBUFFER: {
        struct drm_virtgpu_execbuffer_compat req = {0};
        uint32 *cmds = NULL;
        uint32 *handles = NULL;
        uint32 *resources = NULL;
        uint32 alloc_len = PGSIZE;
        uint32 submit_flags = 0;
        uint32 first_submit = 0;
        uint32 first_word = 0;
        struct virtio_gpu_submit_trace_shape trace_shape = {0};
        int order = 0;
        uint64 fence = 0, signaled = 0;
        uint64 total_start = r_time();
        uint64 ensure_ticks = 0;
        uint64 in_fence_ticks = 0;
        uint64 cmd_copy_ticks = 0;
        uint64 bo_resolve_ticks = 0;
        uint64 submit_ticks = 0;
        uint64 out_fence_ticks = 0;
        uint64 total_work_ticks = 0;
        uint64 trace_log_ticks = 0;
        uint64 phase_start;
        uint64 trace_start;
        int out_fence_fd = -1;
        int submit_ret = 0;
        int submit_attempted = 0;
        int traced = chrome_drm_trace_owner(owner);
        int ret;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            goto execbuffer_done;
        }
        phase_start = r_time();
        ret = gpu_owner_ensure_context(owner);
        ensure_ticks = r_time() - phase_start;
        if (ret != 0)
            goto execbuffer_done;
        if ((req.flags & ~VIRTGPU_EXECBUF_SUPPORTED_FLAGS) != 0) {
            ret = -EINVAL;
            goto execbuffer_done;
        }
        if ((req.flags & VIRTGPU_EXECBUF_RING_IDX) != 0 && req.ring_idx != 0) {
            ret = -EOPNOTSUPP;
            goto execbuffer_done;
        }
        if (req.syncobj_stride != 0 || req.num_in_syncobjs != 0 ||
            req.num_out_syncobjs != 0 || req.in_syncobjs != 0 ||
            req.out_syncobjs != 0) {
            ret = -EOPNOTSUPP;
            goto execbuffer_done;
        }
        if ((req.flags & VIRTGPU_EXECBUF_FENCE_FD_IN) != 0) {
            phase_start = r_time();
            ret = gpu_drm_execbuffer_wait_fence_fd(owner, req.fence_fd);
            in_fence_ticks = r_time() - phase_start;
            if (ret != 0)
                goto execbuffer_done;
        }
        if (req.command == 0 || req.size == 0 || req.size > PGSIZE * 64 ||
            (req.size & 3) != 0) {
            ret = -EINVAL;
            goto execbuffer_done;
        }
        if ((req.num_bo_handles != 0 && req.bo_handles == 0) ||
            req.num_bo_handles > 4096) {
            ret = -EINVAL;
            goto execbuffer_done;
        }

        while (alloc_len < req.size) {
            if (order >= PAGE_BUDDY_MAX_ORDER) {
                ret = -ENOMEM;
                goto execbuffer_done;
            }
            order++;
            alloc_len <<= 1;
        }

        phase_start = r_time();
        cmds = page_alloc(order, PAGE_TYPE_ANON);
        if (cmds == NULL) {
            cmd_copy_ticks = r_time() - phase_start;
            ret = -ENOMEM;
            goto execbuffer_done;
        }
        if (either_copyin(cmds, 1, req.command, req.size) < 0) {
            cmd_copy_ticks = r_time() - phase_start;
            ret = -EFAULT;
            goto execbuffer_done;
        }
        first_word = cmds[0];
        cmd_copy_ticks = r_time() - phase_start;
        if (req.num_bo_handles != 0) {
            uint64 bytes = (uint64)req.num_bo_handles * sizeof(uint32);

            phase_start = r_time();
            handles = kvmalloc(bytes);
            resources = kvmalloc(bytes);
            if (handles == NULL || resources == NULL) {
                bo_resolve_ticks = r_time() - phase_start;
                ret = -ENOMEM;
                goto execbuffer_done;
            }
            if (either_copyin(handles, 1, req.bo_handles, bytes) < 0) {
                bo_resolve_ticks = r_time() - phase_start;
                ret = -EFAULT;
                goto execbuffer_done;
            }
            for (uint32 i = 0; i < req.num_bo_handles; i++) {
                int imported = 0;

                ret = gpu_drm_virtgpu_resource_for_bo_handle(
                    owner, handles[i], &resources[i], NULL, NULL, &imported);
                if (ret != 0) {
                    bo_resolve_ticks = r_time() - phase_start;
                    goto execbuffer_done;
                }
                if (imported) {
                    ret = virtio_gpu_user_resource_attach(owner->id,
                                                          owner->tgid,
                                                          owner->default_ctx_id,
                                                          resources[i], 1);
                    if (ret != 0) {
                        bo_resolve_ticks = r_time() - phase_start;
                        goto execbuffer_done;
                    }
                }
            }
            bo_resolve_ticks = r_time() - phase_start;
        }
        if (gpu_drm_execbuffer_async_allowed())
            submit_flags |= FB_GPU_VIRGL_SUBMIT_ASYNC;
        submit_flags |= FB_GPU_VIRGL_SUBMIT_ALLOW_IMPORTED_RESOURCES;
        trace_shape.valid = 1;
        trace_shape.drm_flags = req.flags;
        trace_shape.nr_dwords = req.size / sizeof(uint32);
        trace_shape.resource_count = req.num_bo_handles;
        trace_shape.first_word = first_word;
        trace_shape.fence_fd =
            (req.flags & VIRTGPU_EXECBUF_FENCE_FD_IN) != 0 ?
            req.fence_fd : -1;
        phase_start = r_time();
        ret = virtio_gpu_user_submit(owner->id, owner->tgid,
                                     owner->default_ctx_id, submit_flags, cmds,
                                     req.size / sizeof(uint32), resources,
                                     req.num_bo_handles, &fence, &signaled,
                                     &first_submit, &trace_shape);
        submit_ticks = r_time() - phase_start;
        submit_ret = ret;
        submit_attempted = 1;
        if (ret == 0 &&
            (req.flags & VIRTGPU_EXECBUF_FENCE_FD_OUT) != 0) {
            phase_start = r_time();
            ret = gpu_drm_execbuffer_export_fence_fd(fence,
                                                     signaled >= fence,
                                                     &out_fence_fd);
            if (ret == 0) {
                req.fence_fd = out_fence_fd;
                if (either_copyout(1, arg, &req, sizeof(req)) < 0) {
                    fb_gpu_close_exported_fd(out_fence_fd);
                    ret = -EFAULT;
                }
            }
            out_fence_ticks = r_time() - phase_start;
        }
execbuffer_done:
        total_work_ticks = r_time() - total_start;
        if (submit_attempted && submit_ret == 0 && first_submit) {
            trace_start = r_time();
            gpu_drm_trace_context_attrib(
                owner, "first-submit-execbuffer", owner->default_ctx_id,
                submit_flags, req.flags, req.size / sizeof(uint32),
                req.num_bo_handles, first_word, fence, signaled, submit_ret);
            trace_log_ticks += r_time() - trace_start;
        }
        if (submit_attempted && chrome_drm_trace_owner(owner)) {
            trace_start = r_time();
            printf("chrome-drm-detail: execbuffer-submit owner=%lu:%d "
                   "ctx=%u flags=0x%x size=%u words=%u bo_count=%u "
                   "first=0x%x ret=%d fence=%lu signaled=%lu\n",
                   owner->id, owner->tgid, owner->default_ctx_id,
                   req.flags, req.size, req.size / (uint32)sizeof(uint32),
                   req.num_bo_handles, first_word, submit_ret, fence,
                   signaled);
            trace_log_ticks += r_time() - trace_start;
        }
        if (traced) {
            printf("chrome-drm-detail: execbuffer-time owner=%lu:%d "
                   "pid=%d proc=%s ctx=%u flags=0x%x submit_flags=0x%x "
                   "size=%u words=%u bo_count=%u first=0x%x fence_fd=%d "
                   "out_fence_fd=%d fence=%lu signaled=%lu ret=%d "
                   "total_us=%lu ensure_us=%lu in_fence_us=%lu "
                   "cmd_copy_us=%lu bo_resolve_us=%lu submit_us=%lu "
                   "out_fence_us=%lu trace_log_us=%lu total_ticks=%lu "
                   "ensure_ticks=%lu "
                   "in_fence_ticks=%lu cmd_copy_ticks=%lu "
                   "bo_resolve_ticks=%lu submit_ticks=%lu "
                   "out_fence_ticks=%lu trace_log_ticks=%lu\n",
                   owner ? owner->id : 0, owner ? owner->tgid : -1,
                   current ? current->pid : -1,
                   current ? current->name : "?",
                   owner ? owner->default_ctx_id : 0, req.flags, submit_flags,
                   req.size, req.size / (uint32)sizeof(uint32),
                   req.num_bo_handles, first_word, req.fence_fd,
                   out_fence_fd, fence, signaled, ret,
                   gpu_drm_ticks_to_us(total_work_ticks),
                   gpu_drm_ticks_to_us(ensure_ticks),
                   gpu_drm_ticks_to_us(in_fence_ticks),
                   gpu_drm_ticks_to_us(cmd_copy_ticks),
                   gpu_drm_ticks_to_us(bo_resolve_ticks),
                   gpu_drm_ticks_to_us(submit_ticks),
                   gpu_drm_ticks_to_us(out_fence_ticks),
                   gpu_drm_ticks_to_us(trace_log_ticks),
                   total_work_ticks, ensure_ticks, in_fence_ticks,
                   cmd_copy_ticks, bo_resolve_ticks, submit_ticks,
                   out_fence_ticks, trace_log_ticks);
        }
        kvfree(handles);
        kvfree(resources);
        if (cmds != NULL)
            page_free(cmds, order);
        return ret;
    }
    case DRM_IOCTL_NOUVEAU_GETPARAM:
        return gpu_nouveau_getparam(arg);
    case DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC:
        return gpu_nouveau_channel_alloc(owner, arg);
    case DRM_IOCTL_NOUVEAU_CHANNEL_FREE:
        return gpu_nouveau_channel_free(owner, arg);
    case DRM_IOCTL_NOUVEAU_GROBJ_ALLOC:
        return gpu_nouveau_grobj_alloc(owner, arg);
    case DRM_IOCTL_NOUVEAU_GPUOBJ_FREE:
        return gpu_nouveau_gpuobj_free(owner, arg);
    case DRM_IOCTL_NOUVEAU_NVIF:
        return gpu_nouveau_nvif(owner, arg);
    case DRM_IOCTL_NOUVEAU_GEM_NEW:
        return gpu_nouveau_gem_new(owner, arg);
    case DRM_IOCTL_NOUVEAU_GEM_INFO:
        return gpu_nouveau_gem_info(owner, arg);
    case DRM_IOCTL_NOUVEAU_GEM_CPU_PREP:
        return gpu_nouveau_cpu_prep(owner, arg);
    case DRM_IOCTL_NOUVEAU_GEM_CPU_FINI:
        return gpu_nouveau_cpu_fini(owner, arg);
    case DRM_IOCTL_NOUVEAU_GEM_PUSHBUF:
        return gpu_nouveau_pushbuf(owner, arg);
    case DRM_IOCTL_NOUVEAU_VM_INIT:
        return gpu_nouveau_vm_init(owner, arg);
    case DRM_IOCTL_NOUVEAU_VM_BIND:
        return gpu_nouveau_bind_or_exec(owner, arg, 0);
    case DRM_IOCTL_NOUVEAU_EXEC:
        return gpu_nouveau_bind_or_exec(owner, arg, 1);
    default:
        return -EINVAL;
    }
}

#define GPU_DRM_IOCTL_DESC(_cmd, _flags) \
    { (_cmd), #_cmd, (_flags), gpu_drm_ioctl_handle }

static const struct drm_core_ioctl_desc gpu_drm_ioctls[] = {
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VERSION, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GET_UNIQUE, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GET_MAGIC,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GET_MAP,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GET_CLIENT, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GET_STATS, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SET_VERSION, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GET_CAP, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SET_CLIENT_CAP, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SET_CLIENT_NAME, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_AUTH_MAGIC,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_ADD_MAP,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_ADD_BUFS,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MARK_BUFS,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_INFO_BUFS,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MAP_BUFS,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_FREE_BUFS,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_RM_MAP,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SET_SAREA_CTX,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GET_SAREA_CTX,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SET_MASTER,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_DROP_MASTER,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_ADD_CTX,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_RM_CTX,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MOD_CTX,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GET_CTX,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SWITCH_CTX,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NEW_CTX,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_RES_CTX,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_DMA,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_LOCK,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_UNLOCK,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_FINISH,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GEM_CLOSE, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GEM_FLINK, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GEM_OPEN, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_PRIME_HANDLE_TO_FD, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_PRIME_FD_TO_HANDLE, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_AGP_ACQUIRE,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_AGP_RELEASE,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_AGP_ENABLE,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_AGP_INFO,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_AGP_ALLOC,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_AGP_FREE,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_AGP_BIND,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_AGP_UNBIND,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SG_ALLOC,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SG_FREE,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_WAIT_VBLANK,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_CRTC_GET_SEQUENCE,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_CRTC_QUEUE_SEQUENCE,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETRESOURCES,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETCRTC,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_SETCRTC,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_CURSOR,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETGAMMA,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_SETGAMMA,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETENCODER,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETCONNECTOR,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETPROPERTY,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETPROPBLOB,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETFB,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_ADDFB,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_RMFB,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_PAGE_FLIP,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_DIRTYFB,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_CREATE_DUMB, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_MAP_DUMB, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_DESTROY_DUMB, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETPLANERESOURCES,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETPLANE,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_SETPLANE,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_ADDFB2,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_OBJ_GETPROPERTIES,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_OBJ_SETPROPERTY,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_CURSOR2,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_ATOMIC,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_CREATEPROPBLOB,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_DESTROYPROPBLOB,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_CREATE_LEASE,
                       DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_LIST_LESSEES,
                       DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GET_LEASE,
                       DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_REVOKE_LEASE,
                       DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_CREATE, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_DESTROY, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_XV6_SYNCOBJ_HANDLE_TO_FD_LEGACY,
                       DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_XV6_SYNCOBJ_FD_TO_HANDLE_LEGACY,
                       DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_WAIT, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_RESET, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_SIGNAL, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_QUERY, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_TRANSFER, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETFB2,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_EVENTFD, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_CLOSEFB,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),

    /* Driver-private virtio-gpu commands. */
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_MAP, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_EXECBUFFER, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_GETPARAM, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_RESOURCE_CREATE,
                       DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_RESOURCE_INFO, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST,
                       DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST,
                       DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_WAIT, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_GET_CAPS, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB,
                       DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_CONTEXT_INIT, DRM_CORE_IOCTL_ANY),

    /* Driver-private Nouveau commands. */
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_GETPARAM, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_CHANNEL_FREE, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_GROBJ_ALLOC, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_GPUOBJ_FREE, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_NVIF, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_VM_INIT, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_VM_BIND, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_EXEC, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_GEM_NEW, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_GEM_PUSHBUF, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_GEM_CPU_PREP, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_GEM_CPU_FINI, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_GEM_INFO, DRM_CORE_IOCTL_ANY),
};

static int gpu_drm_ioctl(struct fb_gpu_render_owner *owner, uint64 cmd,
                         uint64 arg)
{
    const char *name = NULL;
    int known = 0;
    int ret;

    if (owner == NULL)
        return -EBADF;
    if (owner->drm.node_type == DRM_CORE_NODE_PRIMARY ||
        owner->drm.node_type == DRM_CORE_NODE_RENDER) {
        spin_lock(&fb_state.lock);
        fb_state.stats.drm_ioctls++;
        spin_unlock(&fb_state.lock);
    }
    ret = drm_core_dispatch_ioctl(&owner->drm, owner, cmd, arg,
                                  gpu_drm_ioctls,
                                  sizeof(gpu_drm_ioctls) /
                                      sizeof(gpu_drm_ioctls[0]),
                                  &name, &known);
    if (!known) {
        spin_lock(&fb_state.lock);
        fb_state.stats.drm_unknown_ioctls++;
        spin_unlock(&fb_state.lock);
        printf("DRM: unknown ioctl node=%s owner=%lu tgid=%d cmd=0x%lx\n",
               drm_core_node_name(owner->drm.node_type),
               owner->id, owner->tgid, cmd);
    } else if (known == 2) {
        printf("DRM: denied ioctl node=%s owner=%lu tgid=%d cmd=0x%lx(%s)\n",
               drm_core_node_name(owner->drm.node_type),
               owner->id, owner->tgid, cmd, name ? name : "?");
    }
    return ret;
}

static int chrome_drm_ioctl_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("chrome_drm_ioctl_trace", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static _Atomic int chrome_drm_trace_tgid;

static int chrome_drm_current_is_chrome(void)
{
    return current != NULL &&
        (strncmp(current->name, "chrome", 6) == 0 ||
         strncmp(current->name, "Chrome", 6) == 0 ||
         strncmp(current->name, "egl-wayland", 11) == 0 ||
         strncmp(current->name, "Xwayland", 8) == 0 ||
         strncmp(current->name, "xwayland", 8) == 0);
}

static int chrome_drm_trace_owner(const struct fb_gpu_render_owner *owner)
{
    int trace_tgid;

    if (!chrome_drm_ioctl_trace_enabled() || owner == NULL)
        return 0;
    if (chrome_drm_current_is_chrome())
        return 1;
    trace_tgid = __atomic_load_n(&chrome_drm_trace_tgid, __ATOMIC_ACQUIRE);
    return owner->tgid > 0 && owner->tgid == trace_tgid;
}

static void chrome_drm_trace(const char *phase,
                             const struct fb_gpu_render_owner *owner,
                             uint64 cmd, const char *name, int ret)
{
    if (!chrome_drm_trace_owner(owner))
        return;

    printf("chrome-drm-ioctl: %s pid=%d name=%s node=%s owner=%lu:%d "
           "cmd=0x%lx(%s) ret=%d\n",
           phase, current ? current->pid : -1,
           current ? current->name : "?",
           owner ? drm_core_node_name(owner->drm.node_type) : "?",
           owner ? owner->id : 0, owner ? owner->tgid : -1, cmd,
           name ? name : fb_gpu_ioctl_name(cmd), ret);
}

static int gpu_fops_release(struct vfs_inode *inode, struct vfs_file *file)
{
    struct fb_gpu_render_owner *owner =
        file ? (struct fb_gpu_render_owner *)file->private_data : NULL;

    (void)inode;
    if (file != NULL)
        file->private_data = NULL;
    if (owner != NULL) {
        uint64 stale_events;
        uint64 stale_kms_fbs;
        uint64 stale_syncobjs;
        uint64 stale_gem_handles;
        enum drm_core_node_type node_type = owner->drm.node_type;

        spin_lock(&fb_state.lock);
        owner->drm_event_file = NULL;
        stale_events = gpu_drm_event_release_stale_locked(owner);
        spin_unlock(&fb_state.lock);
        drm_core_release_file(&owner->drm);
        fb_dxg_present_release_owner_sources(owner->id, owner->tgid);
        (void)gpu_nouveau_destroy_owner(owner);
        stale_kms_fbs = gpu_kms_destroy_owner_fbs(owner);
        stale_syncobjs = gpu_syncobj_destroy_owner(owner);
        stale_gem_handles = fb_gpu_count_render_owner_bos(owner->id);
        fb_gpu_destroy_render_owner(owner->id);
        fb_gpu_render_owner_unregister(owner);
        virtio_gpu_user_destroy_render_owner(owner->id);
        gpu_drm_lifecycle_live_close(owner);
        gpu_drm_lifecycle_close(node_type, stale_gem_handles, stale_kms_fbs,
                                stale_syncobjs, stale_events);
        (void)gpu_release_node(node_type);
        kvfree(owner);
        return 0;
    }
    return gpu_release(&gpu_cdev);
}

static void gpu_fops_last_fd_close(struct vfs_file *file)
{
    struct fb_gpu_render_owner *owner =
        file ? (struct fb_gpu_render_owner *)file->private_data : NULL;

    gpu_drm_lifecycle_live_close(owner);
}

static void gpu_fops_first_fd_open(struct vfs_file *file)
{
    struct fb_gpu_render_owner *owner =
        file ? (struct fb_gpu_render_owner *)file->private_data : NULL;

    gpu_drm_lifecycle_live_open(owner);
}

static int gpu_fops_ioctl(struct vfs_file *file, uint64 cmd, void *arg)
{
    struct fb_gpu_render_owner *owner =
        file ? (struct fb_gpu_render_owner *)file->private_data : NULL;
    int trace = fb_gpu_trace_enabled() && fb_gpu_trace_process();
    int ret;

    if (owner == NULL)
        return -EBADF;
    if (trace)
        printf("fb-gpu-trace: enter pid=%d name=%s owner=%lu:%d cmd=0x%lx(%s)\n",
               current ? current->pid : -1,
               current ? current->name : "?", owner->id, owner->tgid, cmd,
               fb_gpu_ioctl_name(cmd));
    switch (cmd) {
    case FB_GPU_GET_STATS:
    case FB_GPU_BO_CREATE:
    case FB_GPU_BO_PRESENT:
    case FB_GPU_PAGE_FLIP:
    case FB_GPU_BO_COPY:
    case FB_GPU_SET_CURSOR:
    case FB_GPU_MOVE_CURSOR:
    case FB_GPU_BO_DESTROY:
    case FB_GPU_BO_IMPORT:
    case FB_GPU_BO_EXPORT_FD:
    case FB_GPU_TEST_DMABUF_EXPORT_FD:
    case FB_GPU_BO_IMPORT_FD:
    case FB_GPU_BO_INFO:
    case FB_GPU_TTM_VALIDATE:
    case FB_GPU_DXG_PRESENT_SOURCE_REGISTER:
    case FB_GPU_DXG_PRESENT_SOURCE_COMMIT:
    case FB_GPU_DXG_PRESENT_SOURCE_QUERY:
    case FB_GPU_DXG_PRESENT_BIND_CONTRACT_QUERY:
    case FB_GPU_BO_FENCE:
    case FB_GPU_FENCE_EXPORT_FD:
    case FB_GPU_FENCE_QUERY:
    case FB_GPU_VIRGL_CTX_CREATE:
    case FB_GPU_VIRGL_CTX_DESTROY:
    case FB_GPU_VIRGL_SUBMIT:
    case FB_GPU_VIRGL_FENCE:
    case FB_GPU_VIRGL_FENCE_EXPORT_FD:
    case FB_GPU_VIRGL_FENCE_QUERY_FD:
    case FB_GPU_VIRGL_GET_CAPS:
    case FB_GPU_VIRGL_RESOURCE_CREATE:
    case FB_GPU_VIRGL_RESOURCE_DESTROY:
    case FB_GPU_VIRGL_RESOURCE_ATTACH:
    case FB_GPU_VIRGL_RESOURCE_EXPORT_FD:
    case FB_GPU_VIRGL_TRANSFER_TO_HOST:
    case FB_GPU_VIRGL_TRANSFER_FROM_HOST:
    case FB_GPU_DISPLAY_PROBE:
    case FB_GPU_BACKEND_QUERY:
        break;
    default:
        ret = gpu_drm_ioctl(owner, cmd, (uint64)arg);
        if (trace)
            printf("fb-gpu-trace: exit pid=%d name=%s owner=%lu:%d cmd=0x%lx(%s) ret=%d\n",
                   current ? current->pid : -1,
                   current ? current->name : "?", owner->id, owner->tgid, cmd,
                   fb_gpu_ioctl_name(cmd), ret);
        return ret;
    }

    spin_lock(&fb_state.lock);
    fb_state.stats.gpu_ioctls++;
    spin_unlock(&fb_state.lock);
    ret = fb_ioctl_for_owner(&gpu_cdev, cmd, arg, owner->id, owner->tgid);
    if (trace)
        printf("fb-gpu-trace: exit pid=%d name=%s owner=%lu:%d cmd=0x%lx(%s) ret=%d\n",
               current ? current->pid : -1,
               current ? current->name : "?", owner->id, owner->tgid, cmd,
               fb_gpu_ioctl_name(cmd), ret);
    return ret;
}

static ssize_t gpu_fops_read(struct vfs_file *file, char *buf, size_t count,
                             bool user)
{
    struct fb_gpu_render_owner *owner =
        file ? (struct fb_gpu_render_owner *)file->private_data : NULL;
    struct drm_event_vblank_compat ev;
    uint32 old_head;

    if (owner == NULL || buf == NULL)
        return -EBADF;
    spin_lock(&fb_state.lock);
    if (owner->drm_event_count == 0) {
        spin_unlock(&fb_state.lock);
        return -EAGAIN;
    }
    old_head = owner->drm_event_head;
    ev = owner->drm_events[old_head];
    spin_unlock(&fb_state.lock);
    if (count < ev.base.length)
        return -EINVAL;
    if (either_copyout(user ? 1 : 0, (uint64)buf, &ev, sizeof(ev)) < 0)
        return -EFAULT;

    spin_lock(&fb_state.lock);
    if (owner->drm_event_count != 0 && owner->drm_event_head == old_head) {
        owner->drm_event_head =
            (owner->drm_event_head + 1) % FB_GPU_DRM_EVENT_QUEUE_CAPACITY;
        owner->drm_event_count--;
        if (fb_state.stats.drm_event_queue_depth != 0)
            fb_state.stats.drm_event_queue_depth--;
        fb_state.stats.drm_events_read++;
    }
    spin_unlock(&fb_state.lock);
    return sizeof(ev);
}

static int gpu_fops_poll(struct vfs_file *file, short events)
{
    struct fb_gpu_render_owner *owner =
        file ? (struct fb_gpu_render_owner *)file->private_data : NULL;

    if (owner == NULL)
        return POLLERR | POLLHUP;
    spin_lock(&fb_state.lock);
    if (owner->drm_event_count != 0) {
        spin_unlock(&fb_state.lock);
        return events & (POLLIN | POLLRDNORM | POLLRDBAND);
    }
    spin_unlock(&fb_state.lock);
    return 0;
}

static void *gpu_fops_fault(struct vfs_file *file, struct vma *vma, uint64 va)
{
    struct fb_gpu_render_owner *owner =
        file ? (struct fb_gpu_render_owner *)file->private_data : NULL;
    uint64 off;
    uint32 handle;
    uint64 page_index;
    void *pa;

    if (owner == NULL || vma == NULL || va < vma->start)
        return NULL;

    off = vma->pgoff + (va - vma->start);
    handle = GPU_DRM_MMAP_HANDLE(off);
    page_index = GPU_DRM_MMAP_PAGE(off);
    if (handle == 0)
        return virtio_gpu_user_host_visible_page(owner->id, owner->tgid,
                                                 off);

    pa = fb_bo_page_for_owner(handle, owner->id, owner->tgid, page_index);
    if (pa != NULL)
        return pa;
    return virtio_gpu_user_resource_page(owner->id, owner->tgid, handle,
                                         page_index);
}

static int gpu_fops_mmap(struct vfs_file *file, struct vma *vma)
{
    struct fb_gpu_render_owner *owner =
        file ? (struct fb_gpu_render_owner *)file->private_data : NULL;
    uint64 size;

    if (owner == NULL || vma == NULL)
        return -EBADF;
    size = vma->end - vma->start;
    if (GPU_DRM_MMAP_HANDLE(vma->pgoff) != 0)
        return 0;
    if (virtio_gpu_user_host_visible_mmap(owner->id, owner->tgid,
                                          vma->pgoff, size) != 0)
        return 0;
    vma->flags |= VMA_FLAG_PFNMAP;
    return 0;
}

static struct vfs_file_ops gpu_file_ops = {
    .read    = gpu_fops_read,
    .release = gpu_fops_release,
    .ioctl   = gpu_fops_ioctl,
    .poll    = gpu_fops_poll,
    .mmap    = gpu_fops_mmap,
    .fault   = gpu_fops_fault,
    .first_fd_open = gpu_fops_first_fd_open,
    .last_fd_close = gpu_fops_last_fd_close,
};

static int gpu_drm_private_render_ioctl_allowed(uint64 cmd)
{
    switch (cmd) {
    case FB_GPU_BACKEND_QUERY:
    case FB_GPU_DISPLAY_PROBE:
    case FB_GPU_BO_CREATE:
    case FB_GPU_BO_DESTROY:
    case FB_GPU_BO_COPY:
    case FB_GPU_BO_IMPORT:
    case FB_GPU_BO_EXPORT_FD:
    case FB_GPU_TEST_DMABUF_EXPORT_FD:
    case FB_GPU_BO_IMPORT_FD:
    case FB_GPU_BO_INFO:
    case FB_GPU_BO_FENCE:
    case FB_GPU_FENCE_EXPORT_FD:
    case FB_GPU_FENCE_QUERY:
    case FB_GPU_VIRGL_CTX_CREATE:
    case FB_GPU_VIRGL_CTX_DESTROY:
    case FB_GPU_VIRGL_SUBMIT:
    case FB_GPU_VIRGL_FENCE:
    case FB_GPU_VIRGL_FENCE_EXPORT_FD:
    case FB_GPU_VIRGL_FENCE_QUERY_FD:
    case FB_GPU_VIRGL_GET_CAPS:
    case FB_GPU_VIRGL_RESOURCE_CREATE:
    case FB_GPU_VIRGL_RESOURCE_DESTROY:
    case FB_GPU_VIRGL_RESOURCE_ATTACH:
    case FB_GPU_VIRGL_RESOURCE_EXPORT_FD:
    case FB_GPU_VIRGL_TRANSFER_TO_HOST:
    case FB_GPU_VIRGL_TRANSFER_FROM_HOST:
        return 1;
    default:
        return 0;
    }
}

static int gpu_drm_fops_ioctl(struct vfs_file *file, uint64 cmd, void *arg)
{
    struct fb_gpu_render_owner *owner =
        file ? (struct fb_gpu_render_owner *)file->private_data : NULL;
    int trace = fb_gpu_trace_enabled() && fb_gpu_trace_process();
    const char *chrome_name = fb_gpu_ioctl_name(cmd);
    int ret;

    if (owner == NULL)
        return -EBADF;
    if (trace)
        printf("fb-gpu-trace: enter pid=%d name=%s drm-owner=%lu:%d cmd=0x%lx(%s)\n",
               current ? current->pid : -1,
               current ? current->name : "?", owner->id, owner->tgid, cmd,
               fb_gpu_ioctl_name(cmd));

    if (gpu_drm_private_render_ioctl_allowed(cmd)) {
        /*
         * Mesa's xv6 virgl winsys opens the Linux render node, then issues the
         * xv6 driver-private virgl/BO ioctls that back that winsys.  Keep that
         * render-private ABI available here, while display-present/blit,
         * scanout, and Hyper-V present-source commands stay on the fb/gpu
         * compatibility devices.
         */
        spin_lock(&fb_state.lock);
        fb_state.stats.gpu_ioctls++;
        spin_unlock(&fb_state.lock);
        chrome_drm_trace("enter-private", owner, cmd, chrome_name, 0);
        ret = fb_ioctl_for_owner(&gpu_cdev, cmd, arg, owner->id,
                                 owner->tgid);
        chrome_drm_trace("exit-private", owner, cmd, chrome_name, ret);
        if (trace)
            printf("fb-gpu-trace: exit pid=%d name=%s drm-owner=%lu:%d cmd=0x%lx(%s) ret=%d\n",
                   current ? current->pid : -1,
                   current ? current->name : "?", owner->id, owner->tgid, cmd,
                   fb_gpu_ioctl_name(cmd), ret);
        return ret;
    }

    chrome_drm_trace("enter", owner, cmd, chrome_name, 0);
    ret = gpu_drm_ioctl(owner, cmd, (uint64)arg);
    chrome_drm_trace("exit", owner, cmd, chrome_name, ret);
    if (trace)
        printf("fb-gpu-trace: exit pid=%d name=%s drm-owner=%lu:%d cmd=0x%lx(%s) ret=%d\n",
               current ? current->pid : -1,
               current ? current->name : "?", owner->id, owner->tgid, cmd,
               fb_gpu_ioctl_name(cmd), ret);
    return ret;
}

static struct vfs_file_ops gpu_drm_file_ops = {
    .read    = gpu_fops_read,
    .release = gpu_fops_release,
    .ioctl   = gpu_drm_fops_ioctl,
    .poll    = gpu_fops_poll,
    .mmap    = gpu_fops_mmap,
    .fault   = gpu_fops_fault,
    .first_fd_open = gpu_fops_first_fd_open,
    .last_fd_close = gpu_fops_last_fd_close,
};

struct chrome_drm_dump_ctx {
    int tgid;
    int sample;
    int count;
};

static int chrome_drm_thread_dump_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("chrome_drm_thread_dump", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static void chrome_drm_dump_thread_cb(struct thread *p, void *arg)
{
    struct chrome_drm_dump_ctx *ctx = arg;

    if (p == NULL || ctx == NULL || p->tgid != ctx->tgid)
        return;

    enum thread_state state = __thread_state_get(p);
    int pid = p->pid;
    int tgid = p->tgid;
    int cpu = p->sched_entity ? p->sched_entity->cpu_id : -1;
    int on_cpu = p->sched_entity ?
        smp_load_acquire(&p->sched_entity->on_cpu) : 0;
    void *chan = p->chan;
    char name[sizeof(p->name)];
    safestrcpy(name, p->name, sizeof(name));
    ctx->count++;

    printf("chrome-drm-dump: sample=%d tgid=%d tid=%d name=%s state=%s "
           "cpu=%d on_cpu=%d chan=%p\n",
           ctx->sample, tgid, pid, name, thread_state_short(state),
           cpu, on_cpu, chan);
}

static void chrome_drm_dump_worker(uint64 arg1, uint64 arg2)
{
    (void)arg2;
    int tgid = (int)arg1;

    for (int sample = 0; sample < 3; sample++) {
        sleep_ms(sample == 0 ? 1000 : 1500);
        struct chrome_drm_dump_ctx ctx = {
            .tgid = tgid,
            .sample = sample,
        };
        printf("chrome-drm-dump: begin sample=%d tgid=%d\n", sample, tgid);
        proctab_for_each_rcu(chrome_drm_dump_thread_cb, &ctx);
        printf("chrome-drm-dump: end sample=%d tgid=%d count=%d\n",
               sample, tgid, ctx.count);
    }
}

static void maybe_start_chrome_drm_thread_dump(const struct fb_gpu_render_owner *owner)
{
    static _Atomic int started;

    if (owner == NULL || owner->drm.node_type != DRM_CORE_NODE_RENDER ||
        owner->tgid <= 0)
        return;
    if (current == NULL)
        return;
    if (chrome_drm_ioctl_trace_enabled() && chrome_drm_current_is_chrome()) {
        int old = 0;
        (void)__atomic_compare_exchange_n(&chrome_drm_trace_tgid, &old,
                                          owner->tgid, 0, __ATOMIC_ACQ_REL,
                                          __ATOMIC_ACQUIRE);
        printf("chrome-drm-ioctl: trace-target candidate tgid=%d name=%s target=%d\n",
               owner->tgid, current->name,
               __atomic_load_n(&chrome_drm_trace_tgid, __ATOMIC_ACQUIRE));
    }
    if (!chrome_drm_thread_dump_enabled())
        return;
    printf("chrome-drm-dump: render-open candidate tgid=%d name=%s\n",
           owner->tgid, current->name);
    if (strcmp(current->name, "sh") == 0 || strcmp(current->name, "weston") == 0)
        return;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL) != 0)
        return;

    struct thread *t = kthread_create("chrome_drm_dump",
                                      chrome_drm_dump_worker,
                                      (uint64)owner->tgid, 0,
                                      KERNEL_STACK_ORDER);
    if (IS_ERR_OR_NULL(t)) {
        printf("chrome-drm-dump: failed to start tgid=%d\n", owner->tgid);
        return;
    }
    wakeup(t);
}

static int gpu_open_file_common(cdev_t *cdev, struct vfs_file *file,
                                struct vfs_file_ops *ops)
{
    struct fb_gpu_render_owner *owner;
    int ret = gpu_open(cdev);

    if (ret != 0)
        return ret;
    owner = kvmalloc(sizeof(*owner));
    if (owner == NULL) {
        printf("DRM: open failed stage=owner-alloc node=%s tgid=%d ret=%d\n",
               drm_core_node_name(gpu_drm_node_from_cdev(cdev)),
               current ? current->tgid : -1, -ENOMEM);
        (void)gpu_release(cdev);
        return -ENOMEM;
    }
    memset(owner, 0, sizeof(*owner));
    owner->id = gpu_alloc_render_owner_id();
    owner->tgid = current ? current->tgid : 0;
    owner->next_bo_handle = 1;
    drm_core_file_init(&fb_drm_device, &owner->drm,
                       gpu_drm_node_from_cdev(cdev),
                       owner->id, owner->tgid);
    ret = fb_gpu_render_owner_register(owner);
    if (ret != 0) {
        printf("DRM: open failed stage=owner-register node=%s tgid=%d "
               "owner=%lu ret=%d capacity=%d\n",
               drm_core_node_name(owner->drm.node_type), owner->tgid,
               owner->id, ret, FB_GPU_MAX_RENDER_OWNERS);
        kvfree(owner);
        (void)gpu_release(cdev);
        return ret;
    }
    /* Linux parity: first primary opener becomes master when none exists
     * (see drm_core_master_open); no-op for render nodes or when a
     * compositor already holds mastership.  Placed after successful
     * registration so an open that fails cannot leak the device master
     * cookie (release_file would never run for it). */
    drm_core_master_open(&owner->drm);
    file->ops = ops;
    file->private_data = owner;
    owner->drm_event_file = file;
    gpu_drm_lifecycle_open(owner->drm.node_type);
    owner->lifecycle_live_accounted = 1;
    if (chrome_drm_ioctl_trace_enabled())
        printf("DRM: open node=%s owner=%lu tgid=%d magic=%u\n",
               drm_core_node_name(owner->drm.node_type), owner->id,
               owner->tgid, owner->drm.magic);
    maybe_start_chrome_drm_thread_dump(owner);
    return 0;
}

static int gpu_open_file(cdev_t *cdev, struct vfs_file *file)
{
    return gpu_open_file_common(cdev, file, &gpu_file_ops);
}

static int gpu_drm_open_file(cdev_t *cdev, struct vfs_file *file)
{
    return gpu_open_file_common(cdev, file, &gpu_drm_file_ops);
}

static cdev_t fb_cdev = {
    .dev = {
        .major = FB_MAJOR,
        .minor = FB_MINOR,
        .devname = "fb0",
        .devmode = S_IFCHR | 0666,
    },
    .readable = 1,
    .writable = 1,
    .ops = {
        .read    = fb_read,
        .write   = fb_write,
        .open    = fb_open,
        .release = fb_release,
