static int fb_bo_destroy(uint32 handle);
static struct vfs_file_ops fb_fence_file_ops;
static struct vfs_file_ops fb_virgl_fence_file_ops;
static struct vfs_file_ops fb_syncobj_file_ops;
static void fb_gpu_close_exported_fd(int fd);
static struct fb_gpu_syncobj_state_entry *
gpu_syncobj_state_locked(uint32 state_index);
static int gpu_syncobj_state_ready_locked(
    struct fb_gpu_syncobj_state_entry *state, uint64 point);

#ifndef _IOC_NRBITS
#define _IOC_NRBITS 8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14
#define _IOC_DIRBITS 2
#define _IOC_NRSHIFT 0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT (_IOC_SIZESHIFT + _IOC_SIZEBITS)
#define _IOC_NONE 0U
#define _IOC_WRITE 1U
#define _IOC_READ 2U
#define _IOC(dir, type, nr, size) \
    (((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | \
     ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))
#define _IOW(type, nr, size) _IOC(_IOC_WRITE, (type), (nr), sizeof(size))
#define _IOWR(type, nr, size) \
    _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), sizeof(size))
#endif

#define DMA_BUF_SYNC_READ  (1U << 0)
#define DMA_BUF_SYNC_WRITE (2U << 0)
#define DMA_BUF_SYNC_RW    (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_END   (1U << 2)
#define DMA_BUF_SYNC_VALID_FLAGS_MASK (DMA_BUF_SYNC_RW | DMA_BUF_SYNC_END)

struct dma_buf_sync_compat {
    uint64 flags;
};

struct dma_buf_export_sync_file_compat {
    uint32 flags;
    int32 fd;
};

struct dma_buf_import_sync_file_compat {
    uint32 flags;
    int32 fd;
};

#define DMA_BUF_BASE 'b'
#define DMA_BUF_IOCTL_SYNC \
    _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync_compat)
#define DMA_BUF_SET_NAME_A _IOW(DMA_BUF_BASE, 1, uint32)
#define DMA_BUF_SET_NAME_B _IOW(DMA_BUF_BASE, 1, uint64)
#define DMA_BUF_IOCTL_EXPORT_SYNC_FILE \
    _IOWR(DMA_BUF_BASE, 2, struct dma_buf_export_sync_file_compat)
#define DMA_BUF_IOCTL_IMPORT_SYNC_FILE \
    _IOW(DMA_BUF_BASE, 3, struct dma_buf_import_sync_file_compat)

struct sync_fence_info_compat {
    char obj_name[32];
    char driver_name[32];
    int32 status;
    uint32 flags;
    uint64 timestamp_ns;
};

struct sync_file_info_compat {
    char name[32];
    int32 status;
    uint32 flags;
    uint32 num_fences;
    uint32 pad;
    uint64 sync_fence_info;
};

struct sync_set_deadline_compat {
    uint64 deadline_ns;
    uint64 pad;
};

#define SYNC_IOC_MAGIC '>'
#define SYNC_IOC_FILE_INFO \
    _IOWR(SYNC_IOC_MAGIC, 4, struct sync_file_info_compat)
#define SYNC_IOC_SET_DEADLINE \
    _IOW(SYNC_IOC_MAGIC, 5, struct sync_set_deadline_compat)

static int fb_dmabuf_sync_flags_valid(uint32 flags)
{
    return (flags & DMA_BUF_SYNC_RW) != 0 &&
        (flags & ~DMA_BUF_SYNC_RW) == 0;
}

static int fb_dma_fence_status(struct dma_fence *fence)
{
    int status = 1;

    if (fence == NULL)
        return -EINVAL;
    spin_lock(&fence->lock);
    if (fence->error != 0)
        status = fence->error < 0 ? fence->error : -fence->error;
    else if (!fence->signaled)
        status = 0;
    spin_unlock(&fence->lock);
    return status;
}

static int fb_syncobj_file_status(struct fb_gpu_syncobj_file *sync_file)
{
    uint64 virtio_fence;
    int status;

    if (sync_file == NULL || sync_file->kind != FB_GPU_SYNCOBJ_FD_SYNC_FILE)
        return -EINVAL;
    if (sync_file->virtio_fence == 0)
        sync_file->virtio_fence =
            dma_fence_get_virtio_fence(sync_file->fence);
    status = fb_dma_fence_status(sync_file->fence);
    if (status != 0)
        return status;
    if (sync_file->snapshot_signaled)
        return 1;
    virtio_fence = sync_file->virtio_fence;
    if (virtio_fence != 0 && virtio_gpu_user_last_fence() >= virtio_fence) {
        (void)dma_fence_signal(sync_file->fence, 0);
        sync_file->snapshot_signaled = 1;
        return 1;
    }
    if (sync_file->reservation_gem != NULL &&
        sync_file->reservation_fence != 0) {
        int ready = 0;

        spin_lock(&fb_state.lock);
        if (sync_file->reservation_gem->in_use &&
            !sync_file->reservation_gem->dead &&
            sync_file->reservation_gem->signaled_fence >=
                sync_file->reservation_fence)
            ready = 1;
        spin_unlock(&fb_state.lock);
        if (ready) {
            (void)dma_fence_signal(sync_file->fence, 0);
            sync_file->snapshot_signaled = 1;
            return 1;
        }
    }
    if (sync_file->state_index != 0) {
        struct fb_gpu_syncobj_state_entry *state;
        uint64 reservation_fence = 0;
        int ready;

        spin_lock(&fb_state.lock);
        state = gpu_syncobj_state_locked(sync_file->state_index);
        ready = gpu_syncobj_state_ready_locked(
            state, sync_file->snapshot_timeline_value);
        if (ready && state != NULL)
            reservation_fence = state->reservation_fence;
        spin_unlock(&fb_state.lock);
        if (ready) {
            if (sync_file->snapshot_reservation_fence == 0)
                sync_file->snapshot_reservation_fence = reservation_fence;
            (void)dma_fence_signal(sync_file->fence, 0);
            sync_file->snapshot_signaled = 1;
            return 1;
        }
    }
    return 0;
}

static int fb_syncobj_file_info_ioctl(struct fb_gpu_syncobj_file *sync_file,
                                      uint64 arg)
{
    struct sync_file_info_compat info;
    struct sync_fence_info_compat fence_info;
    uint32 user_num_fences;
    int status;

    if (arg == 0)
        return -EFAULT;
    if (either_copyin(&info, 1, arg, sizeof(info)) < 0)
        return -EFAULT;

    user_num_fences = info.num_fences;
    status = fb_syncobj_file_status(sync_file);
    memset(info.name, 0, sizeof(info.name));
    memcpy(info.name, "xv6-sync-file", sizeof("xv6-sync-file"));
    info.status = status;
    info.flags = 0;
    info.num_fences = 1;
    info.pad = 0;

    if (user_num_fences != 0) {
        if (info.sync_fence_info == 0)
            return -EFAULT;
        memset(&fence_info, 0, sizeof(fence_info));
        memcpy(fence_info.obj_name, "xv6-fence", sizeof("xv6-fence"));
        memcpy(fence_info.driver_name, "xv6-gpu", sizeof("xv6-gpu"));
        fence_info.status = status;
        fence_info.timestamp_ns = (sched_timer_now_ms() + 1) * 1000000ULL;
        if (either_copyout(1, info.sync_fence_info, &fence_info,
                           sizeof(fence_info)) < 0)
            return -EFAULT;
    }

    if (either_copyout(1, arg, &info, sizeof(info)) < 0)
        return -EFAULT;
    return 0;
}

static int fb_syncobj_create_sync_file_fd(
    uint64 fence_id, int signaled, struct dma_fence *source_fence,
    struct fb_gpu_gem_object *reservation_gem, uint64 reservation_fence,
    int *fd_out)
{
    struct fb_gpu_syncobj_file *sync_file;
    struct dma_fence *fence;
    int fd;

    if (fd_out == NULL) {
        fb_gem_put(reservation_gem);
        return -EINVAL;
    }
    *fd_out = -1;
    if (fence_id == 0)
        fence_id = 1;

    if (source_fence != NULL) {
        fence = dma_fence_get(source_fence);
        if (fence == NULL) {
            fb_gem_put(reservation_gem);
            return -ENOMEM;
        }
    } else {
        fence = kvmalloc(sizeof(*fence));
        if (fence == NULL) {
            fb_gem_put(reservation_gem);
            return -ENOMEM;
        }
        dma_fence_init(fence, 0, fence_id);
        if (signaled)
            (void)dma_fence_signal(fence, 0);
    }

    sync_file = kvmalloc(sizeof(*sync_file));
    if (sync_file == NULL) {
        dma_fence_put(fence);
        return -ENOMEM;
    }
    memset(sync_file, 0, sizeof(*sync_file));
    sync_file->kind = FB_GPU_SYNCOBJ_FD_SYNC_FILE;
    sync_file->snapshot_signaled = signaled != 0 || dma_fence_is_signaled(fence);
    sync_file->snapshot_timeline_value = fence_id;
    sync_file->snapshot_reservation_fence = fence_id;
    sync_file->virtio_fence = dma_fence_get_virtio_fence(fence);
    sync_file->reservation_gem = reservation_gem;
    sync_file->reservation_fence = reservation_fence;
    sync_file->fence = dma_fence_get(fence);
    if (sync_file->fence == NULL) {
        kvfree(sync_file);
        dma_fence_put(fence);
        fb_gem_put(reservation_gem);
        return -ENOMEM;
    }

    fd = vfs_custom_fd_alloc(&fb_syncobj_file_ops, sync_file, 0);
    if (fd < 0) {
        dma_fence_put(sync_file->fence);
        kvfree(sync_file);
        dma_fence_put(fence);
        fb_gem_put(reservation_gem);
        return fd;
    }

    spin_lock(&fb_state.lock);
    fb_state.stats.syncobj_sync_file_exports++;
    fb_state.stats.ttm_resv_attach_sync_file_export++;
    fb_state.stats.syncobj_resv_attach++;
    spin_unlock(&fb_state.lock);
    dma_fence_put(fence);
    *fd_out = fd;
    return 0;
}

static int fb_dmabuf_fops_release(struct vfs_inode *inode,
                                  struct vfs_file *file)
{
    (void)inode;
    struct dma_buf *dbuf =
        file ? (struct dma_buf *)file->private_data : NULL;

    if (file != NULL)
        file->private_data = NULL;
    if (dbuf != NULL) {
        dma_fence_put(dbuf->resv_excl);
        dbuf->resv_excl = NULL;
    }
    fb_dmabuf_put(dbuf);
    return 0;
}

static int fb_dmabuf_export_sync_file_ioctl(
    struct dma_buf *dbuf, struct dma_buf_export_sync_file_compat *req)
{
    struct fb_gpu_dmabuf_object *dmabuf = fb_dmabuf_from_dma_buf(dbuf);
    struct fb_gpu_gem_object *gem;
    struct dma_fence *source_fence = NULL;
    struct fb_gpu_gem_object *reservation_gem = NULL;
    uint64 target = 0;
    uint64 reservation_fence = 0;
    uint64 signaled = 0;
    int ready = 1;
    int ret;

    if (dbuf == NULL || req == NULL || !fb_dmabuf_sync_flags_valid(req->flags))
        return -EINVAL;
    if (dmabuf == NULL || dmabuf->gem == NULL)
        return -EINVAL;

    spin_lock(&fb_state.lock);
    gem = dmabuf->gem;
    if (gem == NULL || !gem->in_use || gem->dead) {
        spin_unlock(&fb_state.lock);
        return -ENOENT;
    }
    target = gem->ttm_resv_exclusive_fence;
    if ((req->flags & DMA_BUF_SYNC_WRITE) != 0) {
        uint64 shared = fb_ttm_resv_latest_shared_fence_locked(gem);

        if (shared > target)
            target = shared;
    }
    signaled = gem->signaled_fence;
    if (target == 0)
        target = 1;
    ready = signaled >= target;
    if (dbuf->resv_excl != NULL)
        source_fence = dma_fence_get(dbuf->resv_excl);
    if (!ready) {
        fb_gem_get_locked(gem);
        reservation_gem = gem;
        reservation_fence = target;
    }
    fb_ttm_resv_note_attach_locked(FB_GPU_RESV_ATTACH_SYNC_FILE_EXPORT);
    spin_unlock(&fb_state.lock);

    ret = fb_syncobj_create_sync_file_fd(target, ready, source_fence,
                                         reservation_gem, reservation_fence,
                                         &req->fd);
    dma_fence_put(source_fence);
    return ret;
}

static int fb_dmabuf_import_sync_file_ioctl(
    struct dma_buf *dbuf, const struct dma_buf_import_sync_file_compat *req)
{
    struct fb_gpu_dmabuf_object *dmabuf = fb_dmabuf_from_dma_buf(dbuf);
    struct vfs_file *file;
    struct dma_fence *fence = NULL;
    uint64 fence_id = 0;
    int ready = 0;

    if (dbuf == NULL || req == NULL || !fb_dmabuf_sync_flags_valid(req->flags))
        return -EINVAL;
    if (dmabuf == NULL || dmabuf->gem == NULL)
        return -EINVAL;
    if (req->fd < 0 || current == NULL || current->fdtable == NULL)
        return -EBADF;

    file = vfs_fdtable_get_file(current->fdtable, req->fd);
    if (file == NULL)
        return -EBADF;

    if (file->ops == &fb_syncobj_file_ops && file->private_data != NULL) {
        struct fb_gpu_syncobj_file *sync_file =
            (struct fb_gpu_syncobj_file *)file->private_data;

        if (sync_file->kind != FB_GPU_SYNCOBJ_FD_SYNC_FILE ||
            sync_file->fence == NULL) {
            vfs_fput(file);
            return -EINVAL;
        }
        fence = dma_fence_get(sync_file->fence);
        if (sync_file->virtio_fence != 0)
            dma_fence_set_virtio_fence(fence, sync_file->virtio_fence);
        fence_id = sync_file->snapshot_reservation_fence != 0 ?
            sync_file->snapshot_reservation_fence :
            (sync_file->snapshot_timeline_value != 0 ?
                sync_file->snapshot_timeline_value : sync_file->fence->seqno);
        ready = fb_syncobj_file_status(sync_file) > 0;
    } else if (file->ops == &fb_fence_file_ops && file->private_data != NULL) {
        struct fb_gpu_fence_file *fence_file =
            (struct fb_gpu_fence_file *)file->private_data;

        fence_id = fence_file->fence;
        spin_lock(&fb_state.lock);
        (void)fb_gpu_fence_file_refresh_locked(fence_file);
        ready = fb_gpu_fence_file_signaled_locked(fence_file) >=
            fence_file->fence;
        spin_unlock(&fb_state.lock);
    } else if (file->ops == &fb_virgl_fence_file_ops &&
               file->private_data != NULL) {
        struct fb_gpu_virgl_fence_file *fence_file =
            (struct fb_gpu_virgl_fence_file *)file->private_data;
        uint64 signaled = 0;

        fence_id = fence_file->fence;
        ready = virtio_gpu_user_fence(fence_file->fence, 1, &signaled) == 0 &&
            signaled >= fence_file->fence;
    } else {
        vfs_fput(file);
        return -EINVAL;
    }
    vfs_fput(file);

    if (fence_id == 0)
        fence_id = 1;
    if (fence == NULL) {
        fence = kvmalloc(sizeof(*fence));
        if (fence == NULL)
            return -ENOMEM;
        dma_fence_init(fence, 0, fence_id);
        if (ready)
            (void)dma_fence_signal(fence, 0);
    }

    spin_lock(&fb_state.lock);
    if (dmabuf->gem == NULL || !dmabuf->gem->in_use || dmabuf->gem->dead) {
        spin_unlock(&fb_state.lock);
        dma_fence_put(fence);
        return -ENOENT;
    }
    if ((req->flags & DMA_BUF_SYNC_WRITE) != 0) {
        struct fb_gpu_bo_entry *bo = NULL;

        for (uint32 i = 0; i < FB_GPU_MAX_BOS; i++) {
            if (fb_state.bos[i].in_use &&
                fb_state.bos[i].gem == dmabuf->gem) {
                bo = &fb_state.bos[i];
                break;
            }
        }
        dmabuf->gem->ttm_resv_seq++;
        dmabuf->gem->ttm_resv_exclusive_fence = fence_id;
        fb_ttm_resv_note_issued_fence_locked(dmabuf->gem, fence_id);
        if (ready && dmabuf->gem->signaled_fence < fence_id)
            dmabuf->gem->signaled_fence = fence_id;
        fb_state.stats.ttm_resv_exclusive_fences++;
        dma_fence_put(dbuf->resv_excl);
        dbuf->resv_excl = dma_fence_get(fence);
        fb_ttm_resv_wakeup_locked(dmabuf->gem,
                                  FB_GPU_DMABUF_POLL_WAKE_EXCLUSIVE_ACQUIRE);
        if (bo != NULL)
            fb_ttm_propagate_gem_locked(bo);
        fb_ttm_resv_note_attach_locked(FB_GPU_RESV_ATTACH_SYNC_FILE_IMPORT);
    } else {
        struct fb_gpu_bo_entry *bo = NULL;

        for (uint32 i = 0; i < FB_GPU_MAX_BOS; i++) {
            if (fb_state.bos[i].in_use &&
                fb_state.bos[i].gem == dmabuf->gem) {
                bo = &fb_state.bos[i];
                break;
            }
        }
        if (bo != NULL) {
            struct fb_gpu_resv_shared_fence *slot;
            uint32 index;

            index = dmabuf->gem->ttm_resv_shared_next %
                FB_GPU_RESV_SHARED_SLOTS;
            if (dmabuf->gem->ttm_resv_shared_count ==
                FB_GPU_RESV_SHARED_SLOTS)
                fb_state.stats.ttm_resv_shared_replaced++;
            else
                dmabuf->gem->ttm_resv_shared_count++;
            dmabuf->gem->ttm_resv_shared_next =
                (index + 1) % FB_GPU_RESV_SHARED_SLOTS;

            slot = &dmabuf->gem->ttm_resv_shared[index];
            memset(slot, 0, sizeof(*slot));
            slot->seq = ++dmabuf->gem->ttm_resv_seq;
            slot->fence = fence_id;
            slot->owner_id = bo->owner_id;
            slot->owner_tgid = bo->owner_tgid;
            slot->attach_point = FB_GPU_RESV_ATTACH_SYNC_FILE_IMPORT;
            slot->exporter_tag = FB_GPU_DMABUF_TAG_DRM_PRIME;

            fb_state.stats.ttm_resv_shared_fences++;
            fb_state.stats.ttm_resv_shared_used =
                dmabuf->gem->ttm_resv_shared_count;
            fb_state.stats.ttm_resv_last_shared_fence = fence_id;
            fb_state.stats.dmabuf_last_ttm_resv_shared_fence = fence_id;
            fb_state.stats.dmabuf_last_ttm_resv_shared_count =
                dmabuf->gem->ttm_resv_shared_count;
            fb_ttm_resv_note_attach_locked(
                FB_GPU_RESV_ATTACH_SYNC_FILE_IMPORT);
            fb_ttm_resv_wakeup_locked(
                dmabuf->gem, FB_GPU_DMABUF_POLL_WAKE_SHARED_ATTACH);
            fb_ttm_resv_sync_bo_from_gem_locked(bo, dmabuf->gem);
            fb_ttm_propagate_gem_locked(bo);
        } else {
            fb_ttm_resv_note_attach_locked(
                FB_GPU_RESV_ATTACH_SYNC_FILE_IMPORT);
            fb_ttm_resv_wakeup_locked(dmabuf->gem,
                                      FB_GPU_DMABUF_POLL_WAKE_SHARED_ATTACH);
        }
    }
    fb_dmabuf_snapshot_locked(dmabuf);
    spin_unlock(&fb_state.lock);

    dma_fence_put(fence);
    return 0;
}

static int fb_dmabuf_fops_ioctl(struct vfs_file *file, uint64 cmd, void *arg)
{
    struct dma_buf *dbuf = file ? (struct dma_buf *)file->private_data : NULL;

    switch (cmd) {
    case DMA_BUF_IOCTL_SYNC: {
        struct dma_buf_sync_compat req;

        if (arg == NULL ||
            either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if ((req.flags & ~DMA_BUF_SYNC_VALID_FLAGS_MASK) != 0 ||
            (req.flags & DMA_BUF_SYNC_RW) == 0)
            return -EINVAL;
        return dbuf != NULL ? 0 : -EINVAL;
    }
    case DMA_BUF_SET_NAME_A:
    case DMA_BUF_SET_NAME_B:
        return dbuf != NULL ? 0 : -EINVAL;
    case DMA_BUF_IOCTL_EXPORT_SYNC_FILE: {
        struct dma_buf_export_sync_file_compat req;
        int ret;

        if (arg == NULL ||
            either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        ret = fb_dmabuf_export_sync_file_ioctl(dbuf, &req);
        if (ret == 0 &&
            either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0) {
            fb_gpu_close_exported_fd(req.fd);
            return -EFAULT;
        }
        return ret;
    }
    case DMA_BUF_IOCTL_IMPORT_SYNC_FILE: {
        struct dma_buf_import_sync_file_compat req;

        if (arg == NULL ||
            either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        return fb_dmabuf_import_sync_file_ioctl(dbuf, &req);
    }
    default:
        return -ENOTTY;
    }
}

static int fb_dmabuf_fops_poll(struct vfs_file *file, short events)
{
    struct dma_buf *dbuf = file ? (struct dma_buf *)file->private_data : NULL;
    struct fb_gpu_dmabuf_object *dmabuf = fb_dmabuf_from_dma_buf(dbuf);
    struct fb_gpu_gem_object *gem;
    uint64 read_fence;
    uint64 write_fence;
    uint64 signaled;
    short read_events = events & (POLLIN | POLLRDNORM | POLLRDBAND);
    short write_events = events & (POLLOUT | POLLWRNORM | POLLWRBAND);
    short revents = 0;

    if (dmabuf == NULL || dmabuf->gem == NULL) {
        spin_lock(&fb_state.lock);
        fb_state.stats.dmabuf_poll_semantics = 1;
        fb_state.stats.dmabuf_poll_attempts++;
        fb_state.stats.dmabuf_poll_errors++;
        spin_unlock(&fb_state.lock);
        return POLLERR | POLLHUP;
    }

    spin_lock(&fb_state.lock);
    gem = dmabuf->gem;
    read_fence = gem->ttm_resv_exclusive_fence;
    write_fence = read_fence;
    if (dmabuf->ttm_resv_shared_fence_snapshot > write_fence)
        write_fence = dmabuf->ttm_resv_shared_fence_snapshot;
    if (fb_ttm_resv_latest_shared_fence_locked(gem) > write_fence)
        write_fence = fb_ttm_resv_latest_shared_fence_locked(gem);
    signaled = gem->signaled_fence;
    fb_state.stats.dmabuf_poll_semantics = 1;
    fb_state.stats.dmabuf_poll_attempts++;
    fb_state.stats.dmabuf_poll_last_target_fence = write_fence;
    fb_state.stats.dmabuf_poll_last_signaled_fence = signaled;
    fb_state.stats.dmabuf_poll_last_read_fence = read_fence;
    fb_state.stats.dmabuf_poll_last_write_fence = write_fence;
    if (read_events != 0 &&
        (read_fence == 0 || signaled >= read_fence)) {
        fb_state.stats.dmabuf_poll_read_ready++;
        revents |= read_events;
        if (read_fence != 0)
            fb_state.stats.dmabuf_poll_real_fence_ready++;
    }
    if (write_events != 0 &&
        (write_fence == 0 || signaled >= write_fence)) {
        fb_state.stats.dmabuf_poll_write_ready++;
        revents |= write_events;
        if (write_fence != 0)
            fb_state.stats.dmabuf_poll_real_fence_ready++;
    }
    if (revents != 0) {
        fb_state.stats.dmabuf_poll_ready++;
        if (gem->dmabuf_poll_fired != 0 &&
            gem->dmabuf_poll_pending_to_ready_reported == 0 &&
            gem->dmabuf_poll_target_fence != 0 &&
            signaled >= gem->dmabuf_poll_target_fence) {
            gem->dmabuf_poll_pending_to_ready_reported = 1;
            fb_state.stats.dmabuf_poll_pending_to_ready++;
        }
    } else if (read_events != 0 || write_events != 0) {
        fb_state.stats.dmabuf_poll_not_ready++;
        fb_state.stats.dmabuf_poll_pending++;
        gem->dmabuf_poll_armed = 1;
        gem->dmabuf_poll_fired = 0;
        gem->dmabuf_poll_pending_to_ready_reported = 0;
        gem->dmabuf_poll_events = (uint32)(read_events | write_events);
        gem->dmabuf_poll_last_source = FB_GPU_DMABUF_POLL_WAKE_UNKNOWN;
        gem->dmabuf_poll_target_fence = write_fence;
        gem->dmabuf_poll_armed_wakeup_seq = gem->ttm_resv_wakeup_seq;
        fb_state.stats.dmabuf_poll_callbacks_armed++;
    }
    spin_unlock(&fb_state.lock);
    return revents;
}

static int fb_dmabuf_fops_mmap(struct vfs_file *file, struct vma *vma)
{
    struct dma_buf *dbuf = file ? (struct dma_buf *)file->private_data : NULL;
    uint64 length;

    if (dbuf == NULL || vma == NULL)
        return -EINVAL;
    if (vma->end <= vma->start)
        return -EINVAL;
    length = vma->end - vma->start;
    if ((vma->pgoff & (PGSIZE - 1)) != 0 ||
        vma->pgoff >= dbuf->size ||
        length > dbuf->size - vma->pgoff)
        return -EINVAL;

    vma->flags |= VMA_FLAG_DONTFORK | VMA_FLAG_DONTDUMP;
    return 0;
}

static void *fb_dmabuf_fops_fault(struct vfs_file *file, struct vma *vma,
                                  uint64 va)
{
    struct dma_buf *dbuf = file ? (struct dma_buf *)file->private_data : NULL;
    uint64 offset;

    if (dbuf == NULL || vma == NULL || va < vma->start || va >= vma->end)
        return NULL;
    offset = vma->pgoff + (va - vma->start);
    return fb_dma_buf_fault(dbuf, offset);
}

static void fb_dmabuf_fops_last_fd_close(struct vfs_file *file)
{
    struct dma_buf *dbuf = file ? (struct dma_buf *)file->private_data : NULL;
    struct fb_gpu_dmabuf_object *dmabuf = fb_dmabuf_from_dma_buf(dbuf);

    spin_lock(&fb_state.lock);
    fb_dmabuf_live_close_locked(dmabuf);
    spin_unlock(&fb_state.lock);
}

static void fb_dmabuf_fops_first_fd_open(struct vfs_file *file)
{
    struct dma_buf *dbuf = file ? (struct dma_buf *)file->private_data : NULL;
    struct fb_gpu_dmabuf_object *dmabuf = fb_dmabuf_from_dma_buf(dbuf);

    spin_lock(&fb_state.lock);
    fb_dmabuf_live_open_locked(dmabuf);
    spin_unlock(&fb_state.lock);
}

static struct vfs_file_ops fb_dmabuf_file_ops = {
    .poll = fb_dmabuf_fops_poll,
    .ioctl = fb_dmabuf_fops_ioctl,
    .mmap = fb_dmabuf_fops_mmap,
    .fault = fb_dmabuf_fops_fault,
    .release = fb_dmabuf_fops_release,
    .first_fd_open = fb_dmabuf_fops_first_fd_open,
    .last_fd_close = fb_dmabuf_fops_last_fd_close,
};

static void fb_gpu_close_exported_fd(int fd)
{
    struct vfs_file *file;

    if (fd < 0 || current == NULL || current->fdtable == NULL)
        return;
    spin_lock(&current->fdtable->lock);
    file = vfs_fdtable_dealloc_fd(current->fdtable, fd);
    spin_unlock(&current->fdtable->lock);
    if (file != NULL) {
        vfs_file_maybe_last_fd_close(file);
        vfs_fput(file);
    }
}

static void fb_fence_cancel_pending_callback_locked(
    struct fb_gpu_fence_file *fence_file);

static int fb_fence_fops_release(struct vfs_inode *inode,
                                 struct vfs_file *file)
{
    (void)inode;
    struct fb_gpu_fence_file *fence =
        file ? (struct fb_gpu_fence_file *)file->private_data : NULL;

    if (file != NULL)
        file->private_data = NULL;
    if (fence != NULL) {
        struct fb_gpu_fence *fence_obj = fence->fence_obj;

        if (fence->live_accounted) {
            spin_lock(&fb_state.lock);
            fb_fence_cancel_pending_callback_locked(fence);
            if (fb_state.stats.fence_fd_live > 0)
                fb_state.stats.fence_fd_live--;
            spin_unlock(&fb_state.lock);
            fence->live_accounted = 0;
        } else {
            spin_lock(&fb_state.lock);
            fb_fence_cancel_pending_callback_locked(fence);
            spin_unlock(&fb_state.lock);
        }
        fb_bo_put(fence->bo);
        kvfree(fence);
        fb_gpu_fence_put(fence_obj);
    }
    return 0;
}

static void fb_fence_cancel_pending_callback_locked(
    struct fb_gpu_fence_file *fence_file)
{
    struct fb_gpu_fence *fence;

    if (fence_file == NULL || !fence_file->pending_callback_armed)
        return;
    fence = fence_file->fence_obj;
    if (fence == NULL) {
        fence_file->pending_callback_armed = 0;
        fb_state.stats.fence_objects_callback_errors++;
        return;
    }
    if (fence->callback_fired_generation >=
        fence_file->pending_callback_generation) {
        fence_file->pending_callback_armed = 0;
        return;
    }
    if (!fence->signaled) {
        if (fence->pending_callbacks > 0) {
            fence->pending_callbacks--;
            fb_state.stats.fence_objects_callbacks_removed++;
        } else {
            fb_state.stats.fence_objects_callback_errors++;
        }
    }
    fence_file->pending_callback_armed = 0;
}

static void fb_fence_fops_last_fd_close(struct vfs_file *file)
{
    struct fb_gpu_fence_file *fence =
        file ? (struct fb_gpu_fence_file *)file->private_data : NULL;

    if (fence != NULL) {
        spin_lock(&fb_state.lock);
        fb_fence_cancel_pending_callback_locked(fence);
        if (fence->live_accounted && fb_state.stats.fence_fd_live > 0)
            fb_state.stats.fence_fd_live--;
        spin_unlock(&fb_state.lock);
        fence->live_accounted = 0;
    }
}

static void fb_fence_fops_first_fd_open(struct vfs_file *file)
{
    struct fb_gpu_fence_file *fence =
        file ? (struct fb_gpu_fence_file *)file->private_data : NULL;

    if (fence == NULL || fence->live_accounted)
        return;
    spin_lock(&fb_state.lock);
    if (!fence->live_accounted) {
        fb_state.stats.fence_fd_live++;
        if (fb_state.stats.fence_fd_live > fb_state.stats.fence_fd_peak)
            fb_state.stats.fence_fd_peak = fb_state.stats.fence_fd_live;
        fence->live_accounted = 1;
    }
    spin_unlock(&fb_state.lock);
}

static int fb_fence_fops_poll(struct vfs_file *file, short events)
{
    struct fb_gpu_fence_file *fence =
        file ? (struct fb_gpu_fence_file *)file->private_data : NULL;
    short revents = 0;

    if (fence == NULL || fence->fence_obj == NULL)
        return POLLERR | POLLHUP;

    spin_lock(&fb_state.lock);
    (void)fb_gpu_fence_file_refresh_locked(fence);
    if (fence->fence_obj != NULL && fence->fence_obj->error != 0)
        revents |= POLLERR;
    if (fence->fence_obj != NULL && fence->fence_obj->signaled) {
        revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
        if (!fence->pending_callback_armed)
            fb_state.stats.fence_objects_callbacks_late++;
    } else if (fence->fence_obj != NULL &&
               !fence->pending_callback_armed) {
        fence->fence_obj->pending_callbacks++;
        fence->fence_obj->callback_generation++;
        fence->pending_callback_armed = 1;
        fence->pending_callback_generation =
            fence->fence_obj->callback_generation;
        fb_state.stats.fence_objects_callbacks_added++;
    }
    fb_state.stats.fence_fd_polls++;
    if (revents & (POLLIN | POLLRDNORM | POLLRDBAND))
        fb_state.stats.fence_fd_poll_ready++;
    spin_unlock(&fb_state.lock);

    return revents;
}

static struct vfs_file_ops fb_fence_file_ops = {
    .poll = fb_fence_fops_poll,
    .release = fb_fence_fops_release,
    .first_fd_open = fb_fence_fops_first_fd_open,
    .last_fd_close = fb_fence_fops_last_fd_close,
    .early_release_on_close = 1,
};

static int fb_virgl_fence_fops_release(struct vfs_inode *inode,
                                       struct vfs_file *file)
{
    struct fb_gpu_virgl_fence_file *fence =
        file ? (struct fb_gpu_virgl_fence_file *)file->private_data : NULL;

    (void)inode;
    if (file != NULL)
        file->private_data = NULL;
    if (fence != NULL) {
        if (fence->live_accounted) {
            spin_lock(&fb_state.lock);
            if (fb_state.stats.fence_fd_live > 0)
                fb_state.stats.fence_fd_live--;
            spin_unlock(&fb_state.lock);
            fence->live_accounted = 0;
        }
        kvfree(fence);
    }
    return 0;
}

static void fb_virgl_fence_fops_last_fd_close(struct vfs_file *file)
{
    struct fb_gpu_virgl_fence_file *fence =
        file ? (struct fb_gpu_virgl_fence_file *)file->private_data : NULL;

    if (fence != NULL && fence->live_accounted) {
        spin_lock(&fb_state.lock);
        if (fb_state.stats.fence_fd_live > 0)
            fb_state.stats.fence_fd_live--;
        spin_unlock(&fb_state.lock);
        fence->live_accounted = 0;
    }
}

static void fb_virgl_fence_fops_first_fd_open(struct vfs_file *file)
{
    struct fb_gpu_virgl_fence_file *fence =
        file ? (struct fb_gpu_virgl_fence_file *)file->private_data : NULL;

    if (fence == NULL || fence->live_accounted)
        return;
    spin_lock(&fb_state.lock);
    if (!fence->live_accounted) {
        fb_state.stats.fence_fd_live++;
        if (fb_state.stats.fence_fd_live > fb_state.stats.fence_fd_peak)
            fb_state.stats.fence_fd_peak = fb_state.stats.fence_fd_live;
        fence->live_accounted = 1;
    }
    spin_unlock(&fb_state.lock);
}

static int fb_virgl_fence_fops_poll(struct vfs_file *file, short events)
{
    struct fb_gpu_virgl_fence_file *fence =
        file ? (struct fb_gpu_virgl_fence_file *)file->private_data : NULL;
    uint64 signaled = 0;
    short revents = 0;

    if (fence == NULL)
        return POLLERR | POLLHUP;
    signaled = virtio_gpu_user_last_fence();

    spin_lock(&fb_state.lock);
    fb_state.stats.fence_fd_polls++;
    if (signaled >= fence->fence) {
        revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
        if (revents & (POLLIN | POLLRDNORM | POLLRDBAND))
            fb_state.stats.fence_fd_poll_ready++;
    }
    spin_unlock(&fb_state.lock);
    return revents;
}

static struct vfs_file_ops fb_virgl_fence_file_ops = {
    .poll = fb_virgl_fence_fops_poll,
    .release = fb_virgl_fence_fops_release,
    .first_fd_open = fb_virgl_fence_fops_first_fd_open,
    .last_fd_close = fb_virgl_fence_fops_last_fd_close,
};

static struct fb_gpu_syncobj_state_entry *
gpu_syncobj_state_locked(uint32 state_index);
static void gpu_syncobj_state_put_locked(uint32 state_index);
static int gpu_syncobj_state_get_locked(uint32 state_index);
static int gpu_syncobj_alloc_state_locked(int signaled,
                                          uint32 *state_index);
static int gpu_syncobj_state_ready_locked(
    struct fb_gpu_syncobj_state_entry *state, uint64 point);

static void fb_syncobj_fence_callback(struct dma_fence *fence, void *arg)
{
    struct fb_gpu_syncobj_file *sync_file =
        (struct fb_gpu_syncobj_file *)arg;
    struct vfs_file *file;

    (void)fence;
    if (sync_file == NULL)
        return;
    file = sync_file->callback_file;
    sync_file->callback_file = NULL;
    sync_file->fence_callback_armed = 0;
    if (file != NULL) {
        vfs_file_knote_notify(file, EVFILT_READ, 0);
        vfs_file_knote_notify(file, EVFILT_WRITE, 0);
        vfs_fput(file);
    }
    wakeup_on_chan(&fb_state.syncobj_wakeup_seq);
}

static void fb_syncobj_cancel_fence_callback(struct fb_gpu_syncobj_file *sync_file)
{
    struct vfs_file *file;

    if (sync_file == NULL || !sync_file->fence_callback_armed ||
        sync_file->fence == NULL)
        return;
    if (dma_fence_remove_callback(sync_file->fence,
                                  &sync_file->fence_cb) != 0)
        return;
    file = sync_file->callback_file;
    sync_file->callback_file = NULL;
    sync_file->fence_callback_armed = 0;
    if (file != NULL)
        vfs_fput(file);
}

static void fb_syncobj_cancel_pending_callback_locked(
    struct fb_gpu_syncobj_file *sync_file)
{
    struct fb_gpu_syncobj_state_entry *state;
    uint64 point;

    if (sync_file == NULL ||
        sync_file->kind != FB_GPU_SYNCOBJ_FD_SYNC_FILE ||
        !sync_file->pending_callback_armed ||
        sync_file->state_index == 0)
        return;

    state = gpu_syncobj_state_locked(sync_file->state_index);
    point = sync_file->snapshot_timeline_value != 0 ?
        sync_file->snapshot_timeline_value : 1;
    if (state != NULL &&
        state->sync_file_callback_fired_generation >=
            sync_file->pending_callback_generation) {
        sync_file->pending_callback_armed = 0;
        return;
    }
    if (state != NULL && state->signaled_point < point) {
        if (state->pending_sync_file_callbacks > 0) {
            state->pending_sync_file_callbacks--;
            fb_state.stats.sync_file_pending_callbacks_cancelled++;
        } else {
            fb_state.stats.sync_file_pending_callback_late_fires++;
        }
    }
    sync_file->pending_callback_armed = 0;
}

static void fb_syncobj_fops_last_fd_close(struct vfs_file *file)
{
    struct fb_gpu_syncobj_file *sync_file =
        file ? (struct fb_gpu_syncobj_file *)file->private_data : NULL;

    if (sync_file == NULL)
        return;
    fb_syncobj_cancel_fence_callback(sync_file);
    spin_lock(&fb_state.lock);
    fb_syncobj_cancel_pending_callback_locked(sync_file);
    spin_unlock(&fb_state.lock);
}

static int fb_syncobj_fops_release(struct vfs_inode *inode,
                                   struct vfs_file *file)
{
    struct fb_gpu_syncobj_file *sync_file =
        file ? (struct fb_gpu_syncobj_file *)file->private_data : NULL;

    (void)inode;
    if (file != NULL)
        file->private_data = NULL;
    if (sync_file != NULL) {
        fb_syncobj_cancel_fence_callback(sync_file);
        if (sync_file->state_index != 0) {
            spin_lock(&fb_state.lock);
            fb_syncobj_cancel_pending_callback_locked(sync_file);
            gpu_syncobj_state_put_locked(sync_file->state_index);
            spin_unlock(&fb_state.lock);
        }
        dma_fence_put(sync_file->fence);
        fb_gem_put(sync_file->reservation_gem);
        kvfree(sync_file);
    }
    return 0;
}

static int fb_syncobj_fops_poll(struct vfs_file *file, short events)
{
    struct fb_gpu_syncobj_file *sync_file =
        file ? (struct fb_gpu_syncobj_file *)file->private_data : NULL;
    struct fb_gpu_syncobj_state_entry *state = NULL;
    short revents = 0;

    if (sync_file == NULL)
        return POLLERR | POLLHUP;
    if (sync_file->kind == FB_GPU_SYNCOBJ_FD_SYNC_FILE) {
        int ready = fb_syncobj_file_status(sync_file) > 0;

        if (!ready && sync_file->state_index != 0) {
            spin_lock(&fb_state.lock);
            state = gpu_syncobj_state_locked(sync_file->state_index);
            ready = gpu_syncobj_state_ready_locked(
                state, sync_file->snapshot_timeline_value);
            spin_unlock(&fb_state.lock);
        }
        if (ready) {
            revents |= events & (POLLIN | POLLRDNORM | POLLRDBAND |
                                 POLLOUT | POLLWRNORM | POLLWRBAND);
            if (!sync_file->snapshot_signaled) {
                spin_lock(&fb_state.lock);
                fb_state.stats.sync_file_pending_poll_ready++;
                spin_unlock(&fb_state.lock);
            }
        } else {
            struct vfs_file *callback_file = NULL;
            int armed = 0;

            if (!sync_file->fence_callback_armed &&
                sync_file->fence != NULL) {
                callback_file = vfs_fdup(file);
                if (callback_file != NULL) {
                    int cb_ret;

                    sync_file->callback_file = callback_file;
                    sync_file->fence_callback_armed = 1;
                    cb_ret = dma_fence_add_callback(
                        sync_file->fence, &sync_file->fence_cb,
                        fb_syncobj_fence_callback, sync_file);
                    if (cb_ret == 0) {
                        armed = 1;
                    } else {
                        sync_file->callback_file = NULL;
                        sync_file->fence_callback_armed = 0;
                        vfs_fput(callback_file);
                        if (cb_ret == -ENOENT)
                            revents |= events &
                                (POLLIN | POLLRDNORM | POLLRDBAND |
                                 POLLOUT | POLLWRNORM | POLLWRBAND);
                    }
                }
            }
            if (revents != 0)
                return revents;
            spin_lock(&fb_state.lock);
            fb_state.stats.sync_file_pending_poll_not_ready++;
            if (armed)
                fb_state.stats.sync_file_pending_callbacks_armed++;
            if (!sync_file->pending_callback_armed && state != NULL) {
                state->pending_sync_file_callbacks++;
                state->sync_file_callback_generation++;
                sync_file->pending_callback_armed = 1;
                sync_file->pending_callback_generation =
                    state->sync_file_callback_generation;
            }
            spin_unlock(&fb_state.lock);
        }
        return revents;
    }
    spin_lock(&fb_state.lock);
    state = gpu_syncobj_state_locked(sync_file->state_index);
    if (state == NULL) {
        revents = POLLERR | POLLHUP;
    } else if (state->signaled) {
        revents |= events & (POLLIN | POLLRDNORM | POLLRDBAND |
                             POLLOUT | POLLWRNORM | POLLWRBAND);
    }
    spin_unlock(&fb_state.lock);
    return revents;
}

static int fb_syncobj_fops_ioctl(struct vfs_file *file, uint64 cmd, void *arg)
{
    struct fb_gpu_syncobj_file *sync_file =
        file ? (struct fb_gpu_syncobj_file *)file->private_data : NULL;

    if (sync_file == NULL)
        return -EINVAL;
    switch (cmd) {
    case SYNC_IOC_FILE_INFO:
        return fb_syncobj_file_info_ioctl(sync_file, (uint64)arg);
    case SYNC_IOC_SET_DEADLINE: {
        struct sync_set_deadline_compat req;

        if (arg == NULL ||
            either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        return req.pad == 0 ? 0 : -EINVAL;
    }
    default:
        return -ENOTTY;
    }
}

static struct vfs_file_ops fb_syncobj_file_ops = {
    .poll = fb_syncobj_fops_poll,
    .ioctl = fb_syncobj_fops_ioctl,
    .release = fb_syncobj_fops_release,
    .last_fd_close = fb_syncobj_fops_last_fd_close,
};

static int fb_bo_map_current(struct fb_gpu_bo_entry *bo, uint64 *addr_out)
{
    vm_t *vm;
    vma_t *vma;
    uint64 addr;
    uint64 flags;
    uint64 pte_flags;

    if (bo == NULL || addr_out == NULL || current == NULL ||
        current->vm == NULL)
        return -EINVAL;

    vm = current->vm;
    /*
     * The BO mapping aliases the kernel's bo->pages, which are read directly
     * (via PA2VA) by the FB_GPU_BO_PRESENT path.  It must stay coherent with
     * those pages across fork(): without VMA_FLAG_DONTFORK a fork (e.g. when
     * the compositor spawns a browser/helper process) write-protects the
     * parent's PTEs for COW, so the parent's next render to a BO page is
     * copied to a private page while the kernel keeps presenting the stale
     * bo->page -- leaving wallpaper over freshly drawn window chrome.  The
     * scanout mapping uses the same flags for the same reason.
     */
    flags = PROT_READ | PROT_WRITE | VMA_FLAG_USER |
            VMA_FLAG_DONTFORK | VMA_FLAG_DONTDUMP;

    vm_wlock(vm);
    addr = vm_find_free_range(vm, (size_t)(bo->size + FB_GPU_D3D12_HEAP_ALIGN), 0);
    if (addr == 0) {
        vm_wunlock(vm);
        return -ENOMEM;
    }
    addr = FB_GPU_ALIGN_UP(addr, FB_GPU_D3D12_HEAP_ALIGN);

    vma = vma_alloc(vm, addr, bo->size, flags);
    if (vma == NULL) {
        vm_wunlock(vm);
        return -ENOMEM;
    }
    if (anon_vma_prepare(vma) != 0) {
        vma_free(vm, vma);
        vm_wunlock(vm);
        return -ENOMEM;
    }

    pte_flags = vma2pte_flags(flags);
    for (uint32 i = 0; i < bo->npages; i++) {
        uint64 va = addr + (uint64)i * PGSIZE;
        uint64 pa = __page_to_pa(bo->pages[i]);

        if (page_ref_inc((void *)pa) <= 0) {
            vma_free(vm, vma);
            vm_wunlock(vm);
            return -ENOMEM;
        }
        if (mappages(vm->pagetable, va, PGSIZE, pa, pte_flags) != 0) {
            page_ref_dec((void *)pa);
            vma_free(vm, vma);
            vm_wunlock(vm);
            return -ENOMEM;
        }
        page_add_anon_rmap(bo->pages[i], vma, va);
    }

    vm_wunlock(vm);
    *addr_out = addr;
    return 0;
}
