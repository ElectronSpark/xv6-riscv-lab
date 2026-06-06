static int fb_bo_destroy(uint32 handle);

static int fb_dmabuf_fops_release(struct vfs_inode *inode,
                                  struct vfs_file *file)
{
    (void)inode;
    struct dma_buf *dbuf =
        file ? (struct dma_buf *)file->private_data : NULL;

    if (file != NULL)
        file->private_data = NULL;
    fb_dmabuf_put(dbuf);
    return 0;
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
    if (virtio_gpu_user_fence(0, 0, &signaled) != 0)
        return POLLERR | POLLHUP;

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
        int ready = sync_file->snapshot_signaled ||
            dma_fence_is_signaled(sync_file->fence);

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

static struct vfs_file_ops fb_syncobj_file_ops = {
    .poll = fb_syncobj_fops_poll,
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
