static int fb_cmdline_enabled(const char *key)
{
    char buf[16];

    if (cmdline_get_param(key, buf, sizeof(buf)) != 0)
        return 0;
    return strcmp(buf, "1") == 0 ||
           strcmp(buf, "yes") == 0 ||
           strcmp(buf, "true") == 0 ||
           strcmp(buf, "on") == 0;
}

static int fb_gpu_trace_enabled(void)
{
    return fb_cmdline_enabled("webkit_gpu_trace");
}

static int fb_gpu_trace_process(void)
{
    return current != NULL &&
        (strncmp(current->name, "MiniBrowser", 11) == 0 ||
         strncmp(current->name, "WebKit", 6) == 0 ||
         strncmp(current->name, "wlcomp", 6) == 0);
}

static const char *fb_gpu_ioctl_name(uint64 cmd)
{
    switch (cmd) {
    case FBIOGET_VSCREENINFO: return "FBIOGET_VSCREENINFO";
    case FBIOGET_FSCREENINFO: return "FBIOGET_FSCREENINFO";
    case FBIOPUT_VSCREENINFO: return "FBIOPUT_VSCREENINFO";
    case FB_GPU_GET_STATS: return "FB_GPU_GET_STATS";
    case FB_GPU_BO_CREATE: return "FB_GPU_BO_CREATE";
    case FB_GPU_BO_DESTROY: return "FB_GPU_BO_DESTROY";
    case FB_GPU_BO_IMPORT: return "FB_GPU_BO_IMPORT";
    case FB_GPU_BO_EXPORT_FD: return "FB_GPU_BO_EXPORT_FD";
    case FB_GPU_BO_IMPORT_FD: return "FB_GPU_BO_IMPORT_FD";
    case FB_GPU_BO_INFO: return "FB_GPU_BO_INFO";
    case FB_GPU_TTM_VALIDATE: return "FB_GPU_TTM_VALIDATE";
    case FB_GPU_DXG_PRESENT_SOURCE_REGISTER: return "FB_GPU_DXG_PRESENT_SOURCE_REGISTER";
    case FB_GPU_DXG_PRESENT_SOURCE_COMMIT: return "FB_GPU_DXG_PRESENT_SOURCE_COMMIT";
    case FB_GPU_DXG_PRESENT_SOURCE_QUERY: return "FB_GPU_DXG_PRESENT_SOURCE_QUERY";
    case FB_GPU_DXG_PRESENT_BIND_CONTRACT_QUERY: return "FB_GPU_DXG_PRESENT_BIND_CONTRACT_QUERY";
    case FB_GPU_BO_FENCE: return "FB_GPU_BO_FENCE";
    case FB_GPU_FENCE_EXPORT_FD: return "FB_GPU_FENCE_EXPORT_FD";
    case FB_GPU_FENCE_QUERY: return "FB_GPU_FENCE_QUERY";
    case FB_GPU_VIRGL_CTX_CREATE: return "FB_GPU_VIRGL_CTX_CREATE";
    case FB_GPU_VIRGL_CTX_DESTROY: return "FB_GPU_VIRGL_CTX_DESTROY";
    case FB_GPU_VIRGL_SUBMIT: return "FB_GPU_VIRGL_SUBMIT";
    case FB_GPU_VIRGL_FENCE: return "FB_GPU_VIRGL_FENCE";
    case FB_GPU_VIRGL_FENCE_EXPORT_FD: return "FB_GPU_VIRGL_FENCE_EXPORT_FD";
    case FB_GPU_VIRGL_FENCE_QUERY_FD: return "FB_GPU_VIRGL_FENCE_QUERY_FD";
    case FB_GPU_VIRGL_GET_CAPS: return "FB_GPU_VIRGL_GET_CAPS";
    case FB_GPU_VIRGL_RESOURCE_CREATE: return "FB_GPU_VIRGL_RESOURCE_CREATE";
    case FB_GPU_VIRGL_RESOURCE_DESTROY: return "FB_GPU_VIRGL_RESOURCE_DESTROY";
    case FB_GPU_VIRGL_RESOURCE_EXPORT_FD: return "FB_GPU_VIRGL_RESOURCE_EXPORT_FD";
    case FB_GPU_VIRGL_TRANSFER_TO_HOST: return "FB_GPU_VIRGL_TRANSFER_TO_HOST";
    case FB_GPU_VIRGL_TRANSFER_FROM_HOST: return "FB_GPU_VIRGL_TRANSFER_FROM_HOST";
    case FB_GPU_SCANOUT_MAP: return "FB_GPU_SCANOUT_MAP";
    case FB_GPU_SCANOUT_FLUSH: return "FB_GPU_SCANOUT_FLUSH";
    case FB_GPU_DISPLAY_PROBE: return "FB_GPU_DISPLAY_PROBE";
    case FB_GPU_BACKEND_QUERY: return "FB_GPU_BACKEND_QUERY";
    case FB_GPU_DISPLAY_WAIT: return "FB_GPU_DISPLAY_WAIT";
    case DRM_IOCTL_VERSION: return "DRM_IOCTL_VERSION";
    case DRM_IOCTL_GET_UNIQUE: return "DRM_IOCTL_GET_UNIQUE";
    case DRM_IOCTL_GET_MAGIC: return "DRM_IOCTL_GET_MAGIC";
    case DRM_IOCTL_GET_MAP: return "DRM_IOCTL_GET_MAP";
    case DRM_IOCTL_GET_CLIENT: return "DRM_IOCTL_GET_CLIENT";
    case DRM_IOCTL_GET_STATS: return "DRM_IOCTL_GET_STATS";
    case DRM_IOCTL_SET_VERSION: return "DRM_IOCTL_SET_VERSION";
    case DRM_IOCTL_GET_CAP: return "DRM_IOCTL_GET_CAP";
    case DRM_IOCTL_SET_CLIENT_CAP: return "DRM_IOCTL_SET_CLIENT_CAP";
    case DRM_IOCTL_SET_CLIENT_NAME: return "DRM_IOCTL_SET_CLIENT_NAME";
    case DRM_IOCTL_AUTH_MAGIC: return "DRM_IOCTL_AUTH_MAGIC";
    case DRM_IOCTL_ADD_MAP: return "DRM_IOCTL_ADD_MAP";
    case DRM_IOCTL_ADD_BUFS: return "DRM_IOCTL_ADD_BUFS";
    case DRM_IOCTL_MARK_BUFS: return "DRM_IOCTL_MARK_BUFS";
    case DRM_IOCTL_INFO_BUFS: return "DRM_IOCTL_INFO_BUFS";
    case DRM_IOCTL_MAP_BUFS: return "DRM_IOCTL_MAP_BUFS";
    case DRM_IOCTL_FREE_BUFS: return "DRM_IOCTL_FREE_BUFS";
    case DRM_IOCTL_RM_MAP: return "DRM_IOCTL_RM_MAP";
    case DRM_IOCTL_SET_SAREA_CTX: return "DRM_IOCTL_SET_SAREA_CTX";
    case DRM_IOCTL_GET_SAREA_CTX: return "DRM_IOCTL_GET_SAREA_CTX";
    case DRM_IOCTL_SET_MASTER: return "DRM_IOCTL_SET_MASTER";
    case DRM_IOCTL_DROP_MASTER: return "DRM_IOCTL_DROP_MASTER";
    case DRM_IOCTL_ADD_CTX: return "DRM_IOCTL_ADD_CTX";
    case DRM_IOCTL_RM_CTX: return "DRM_IOCTL_RM_CTX";
    case DRM_IOCTL_MOD_CTX: return "DRM_IOCTL_MOD_CTX";
    case DRM_IOCTL_GET_CTX: return "DRM_IOCTL_GET_CTX";
    case DRM_IOCTL_SWITCH_CTX: return "DRM_IOCTL_SWITCH_CTX";
    case DRM_IOCTL_NEW_CTX: return "DRM_IOCTL_NEW_CTX";
    case DRM_IOCTL_RES_CTX: return "DRM_IOCTL_RES_CTX";
    case DRM_IOCTL_DMA: return "DRM_IOCTL_DMA";
    case DRM_IOCTL_LOCK: return "DRM_IOCTL_LOCK";
    case DRM_IOCTL_UNLOCK: return "DRM_IOCTL_UNLOCK";
    case DRM_IOCTL_FINISH: return "DRM_IOCTL_FINISH";
    case DRM_IOCTL_GEM_CLOSE: return "DRM_IOCTL_GEM_CLOSE";
    case DRM_IOCTL_GEM_FLINK: return "DRM_IOCTL_GEM_FLINK";
    case DRM_IOCTL_GEM_OPEN: return "DRM_IOCTL_GEM_OPEN";
    case DRM_IOCTL_PRIME_HANDLE_TO_FD: return "DRM_IOCTL_PRIME_HANDLE_TO_FD";
    case DRM_IOCTL_PRIME_FD_TO_HANDLE: return "DRM_IOCTL_PRIME_FD_TO_HANDLE";
    case DRM_IOCTL_AGP_ACQUIRE: return "DRM_IOCTL_AGP_ACQUIRE";
    case DRM_IOCTL_AGP_RELEASE: return "DRM_IOCTL_AGP_RELEASE";
    case DRM_IOCTL_AGP_ENABLE: return "DRM_IOCTL_AGP_ENABLE";
    case DRM_IOCTL_AGP_INFO: return "DRM_IOCTL_AGP_INFO";
    case DRM_IOCTL_AGP_ALLOC: return "DRM_IOCTL_AGP_ALLOC";
    case DRM_IOCTL_AGP_FREE: return "DRM_IOCTL_AGP_FREE";
    case DRM_IOCTL_AGP_BIND: return "DRM_IOCTL_AGP_BIND";
    case DRM_IOCTL_AGP_UNBIND: return "DRM_IOCTL_AGP_UNBIND";
    case DRM_IOCTL_SG_ALLOC: return "DRM_IOCTL_SG_ALLOC";
    case DRM_IOCTL_SG_FREE: return "DRM_IOCTL_SG_FREE";
    case DRM_IOCTL_WAIT_VBLANK: return "DRM_IOCTL_WAIT_VBLANK";
    case DRM_IOCTL_CRTC_GET_SEQUENCE: return "DRM_IOCTL_CRTC_GET_SEQUENCE";
    case DRM_IOCTL_CRTC_QUEUE_SEQUENCE: return "DRM_IOCTL_CRTC_QUEUE_SEQUENCE";
    case DRM_IOCTL_MODE_GETRESOURCES: return "DRM_IOCTL_MODE_GETRESOURCES";
    case DRM_IOCTL_MODE_GETCRTC: return "DRM_IOCTL_MODE_GETCRTC";
    case DRM_IOCTL_MODE_SETCRTC: return "DRM_IOCTL_MODE_SETCRTC";
    case DRM_IOCTL_MODE_CURSOR: return "DRM_IOCTL_MODE_CURSOR";
    case DRM_IOCTL_MODE_GETGAMMA: return "DRM_IOCTL_MODE_GETGAMMA";
    case DRM_IOCTL_MODE_SETGAMMA: return "DRM_IOCTL_MODE_SETGAMMA";
    case DRM_IOCTL_MODE_GETENCODER: return "DRM_IOCTL_MODE_GETENCODER";
    case DRM_IOCTL_MODE_GETCONNECTOR: return "DRM_IOCTL_MODE_GETCONNECTOR";
    case DRM_IOCTL_MODE_GETPROPERTY: return "DRM_IOCTL_MODE_GETPROPERTY";
    case DRM_IOCTL_MODE_GETPROPBLOB: return "DRM_IOCTL_MODE_GETPROPBLOB";
    case DRM_IOCTL_MODE_GETFB: return "DRM_IOCTL_MODE_GETFB";
    case DRM_IOCTL_MODE_ADDFB: return "DRM_IOCTL_MODE_ADDFB";
    case DRM_IOCTL_MODE_RMFB: return "DRM_IOCTL_MODE_RMFB";
    case DRM_IOCTL_MODE_PAGE_FLIP: return "DRM_IOCTL_MODE_PAGE_FLIP";
    case DRM_IOCTL_MODE_DIRTYFB: return "DRM_IOCTL_MODE_DIRTYFB";
    case DRM_IOCTL_MODE_CREATE_DUMB: return "DRM_IOCTL_MODE_CREATE_DUMB";
    case DRM_IOCTL_MODE_MAP_DUMB: return "DRM_IOCTL_MODE_MAP_DUMB";
    case DRM_IOCTL_MODE_DESTROY_DUMB: return "DRM_IOCTL_MODE_DESTROY_DUMB";
    case DRM_IOCTL_MODE_GETPLANERESOURCES: return "DRM_IOCTL_MODE_GETPLANERESOURCES";
    case DRM_IOCTL_MODE_GETPLANE: return "DRM_IOCTL_MODE_GETPLANE";
    case DRM_IOCTL_MODE_SETPLANE: return "DRM_IOCTL_MODE_SETPLANE";
    case DRM_IOCTL_MODE_ADDFB2: return "DRM_IOCTL_MODE_ADDFB2";
    case DRM_IOCTL_MODE_OBJ_GETPROPERTIES: return "DRM_IOCTL_MODE_OBJ_GETPROPERTIES";
    case DRM_IOCTL_MODE_OBJ_SETPROPERTY: return "DRM_IOCTL_MODE_OBJ_SETPROPERTY";
    case DRM_IOCTL_MODE_CURSOR2: return "DRM_IOCTL_MODE_CURSOR2";
    case DRM_IOCTL_MODE_ATOMIC: return "DRM_IOCTL_MODE_ATOMIC";
    case DRM_IOCTL_MODE_CREATEPROPBLOB: return "DRM_IOCTL_MODE_CREATEPROPBLOB";
    case DRM_IOCTL_MODE_DESTROYPROPBLOB: return "DRM_IOCTL_MODE_DESTROYPROPBLOB";
    case DRM_IOCTL_MODE_CREATE_LEASE: return "DRM_IOCTL_MODE_CREATE_LEASE";
    case DRM_IOCTL_MODE_LIST_LESSEES: return "DRM_IOCTL_MODE_LIST_LESSEES";
    case DRM_IOCTL_MODE_GET_LEASE: return "DRM_IOCTL_MODE_GET_LEASE";
    case DRM_IOCTL_MODE_REVOKE_LEASE: return "DRM_IOCTL_MODE_REVOKE_LEASE";
    case DRM_IOCTL_SYNCOBJ_CREATE: return "DRM_IOCTL_SYNCOBJ_CREATE";
    case DRM_IOCTL_SYNCOBJ_DESTROY: return "DRM_IOCTL_SYNCOBJ_DESTROY";
    case DRM_IOCTL_XV6_SYNCOBJ_HANDLE_TO_FD_LEGACY:
        return "DRM_IOCTL_XV6_SYNCOBJ_HANDLE_TO_FD_LEGACY";
    case DRM_IOCTL_XV6_SYNCOBJ_FD_TO_HANDLE_LEGACY:
        return "DRM_IOCTL_XV6_SYNCOBJ_FD_TO_HANDLE_LEGACY";
    case DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD: return "DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD";
    case DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE: return "DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE";
    case DRM_IOCTL_SYNCOBJ_WAIT: return "DRM_IOCTL_SYNCOBJ_WAIT";
    case DRM_IOCTL_SYNCOBJ_RESET: return "DRM_IOCTL_SYNCOBJ_RESET";
    case DRM_IOCTL_SYNCOBJ_SIGNAL: return "DRM_IOCTL_SYNCOBJ_SIGNAL";
    case DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT: return "DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT";
    case DRM_IOCTL_SYNCOBJ_QUERY: return "DRM_IOCTL_SYNCOBJ_QUERY";
    case DRM_IOCTL_SYNCOBJ_TRANSFER: return "DRM_IOCTL_SYNCOBJ_TRANSFER";
    case DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL: return "DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL";
    case DRM_IOCTL_MODE_GETFB2: return "DRM_IOCTL_MODE_GETFB2";
    case DRM_IOCTL_SYNCOBJ_EVENTFD: return "DRM_IOCTL_SYNCOBJ_EVENTFD";
    case DRM_IOCTL_MODE_CLOSEFB: return "DRM_IOCTL_MODE_CLOSEFB";
    case DRM_IOCTL_VIRTGPU_MAP: return "DRM_IOCTL_VIRTGPU_MAP";
    case DRM_IOCTL_VIRTGPU_EXECBUFFER: return "DRM_IOCTL_VIRTGPU_EXECBUFFER";
    case DRM_IOCTL_VIRTGPU_GETPARAM: return "DRM_IOCTL_VIRTGPU_GETPARAM";
    case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE: return "DRM_IOCTL_VIRTGPU_RESOURCE_CREATE";
    case DRM_IOCTL_VIRTGPU_RESOURCE_INFO: return "DRM_IOCTL_VIRTGPU_RESOURCE_INFO";
    case DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST: return "DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST";
    case DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST: return "DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST";
    case DRM_IOCTL_VIRTGPU_WAIT: return "DRM_IOCTL_VIRTGPU_WAIT";
    case DRM_IOCTL_VIRTGPU_GET_CAPS: return "DRM_IOCTL_VIRTGPU_GET_CAPS";
    case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB: return "DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB";
    case DRM_IOCTL_VIRTGPU_CONTEXT_INIT: return "DRM_IOCTL_VIRTGPU_CONTEXT_INIT";
    case DRM_IOCTL_NOUVEAU_GETPARAM: return "DRM_IOCTL_NOUVEAU_GETPARAM";
    case DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC: return "DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC";
    case DRM_IOCTL_NOUVEAU_CHANNEL_FREE: return "DRM_IOCTL_NOUVEAU_CHANNEL_FREE";
    case DRM_IOCTL_NOUVEAU_VM_INIT: return "DRM_IOCTL_NOUVEAU_VM_INIT";
    case DRM_IOCTL_NOUVEAU_VM_BIND: return "DRM_IOCTL_NOUVEAU_VM_BIND";
    case DRM_IOCTL_NOUVEAU_EXEC: return "DRM_IOCTL_NOUVEAU_EXEC";
    case DRM_IOCTL_NOUVEAU_GEM_NEW: return "DRM_IOCTL_NOUVEAU_GEM_NEW";
    case DRM_IOCTL_NOUVEAU_GEM_PUSHBUF: return "DRM_IOCTL_NOUVEAU_GEM_PUSHBUF";
    case DRM_IOCTL_NOUVEAU_GEM_CPU_PREP: return "DRM_IOCTL_NOUVEAU_GEM_CPU_PREP";
    case DRM_IOCTL_NOUVEAU_GEM_CPU_FINI: return "DRM_IOCTL_NOUVEAU_GEM_CPU_FINI";
    case DRM_IOCTL_NOUVEAU_GEM_INFO: return "DRM_IOCTL_NOUVEAU_GEM_INFO";
    default: return "?";
    }
}

static int fb_blit_from_user(struct fb_gpu_blit cmd, int count_present)
{
    uint32 xres, yres;
    uint32 pitch;
    bool virtio_backed;
    int clipped = 0;

    if (cmd.w == 0 || cmd.h == 0)
        return 0;
    if (cmd.w > 0xffffffffU / 4) {
        spin_lock(&fb_state.lock);
        fb_state.stats.rejected_blits++;
        spin_unlock(&fb_state.lock);
        return -EINVAL;
    }
    if (cmd.pixels == 0 || cmd.src_pitch == 0 ||
        (cmd.src_pitch & 3) != 0 ||
        cmd.src_pitch < cmd.w * 4) {
        spin_lock(&fb_state.lock);
        fb_state.stats.rejected_blits++;
        spin_unlock(&fb_state.lock);
        return -EINVAL;
    }

    spin_lock(&fb_state.lock);
    xres = fb_state.xres;
    yres = fb_state.yres;
    pitch = fb_state.pitch;
    virtio_backed = fb_state.virtio_backed;
    spin_unlock(&fb_state.lock);

    if (cmd.x >= xres || cmd.y >= yres)
        return 0;

    uint32 cw = cmd.w, ch = cmd.h;
    if ((uint64)cmd.x + cw > xres) {
        cw = xres - cmd.x;
        clipped = 1;
    }
    if ((uint64)cmd.y + ch > yres) {
        ch = yres - cmd.y;
        clipped = 1;
    }
    if (cw == 0 || ch == 0)
        return 0;

    volatile uint8 *fb = fb_state.fb_virt;
    uint64 row_bytes = (uint64)cw * 4;

    spin_lock(&fb_state.lock);
    if (cmd.x == 0 && cmd.y == 0 && cw == xres && ch == yres)
        fb_state.stats.full_blits++;
    else
        fb_state.stats.partial_blits++;
    if (clipped)
        fb_state.stats.clipped_blits++;
    fb_state.stats.blit_bytes += (uint64)cw * ch * 4;
    if (count_present)
        fb_state.stats.bo_presents++;
    spin_unlock(&fb_state.lock);

    /*
     * fb_virt is PA2VA-mapped (cached RAM, both Hyper-V synthvid and BGA),
     * so no volatile/lock/bounce-buffer is required to write pixels.  Copy
     * each row in a single either_copyin from user shadow buffer directly
     * into the framebuffer row.  This eliminates the per-pixel volatile
     * loop and per-chunk spin_lock that previously made non-virtio
     * (Hyper-V) blits cost hundreds of ms per frame.
     */
    (void)virtio_backed;
    for (uint32 row = 0; row < ch; row++) {
        uint64 src_addr = cmd.pixels + (uint64)row * cmd.src_pitch;
        void *dst = (void *)(fb + (uint64)(cmd.y + row) * pitch +
                             (uint64)cmd.x * 4);

        if (either_copyin(dst, 1, src_addr, row_bytes) < 0)
            return -EFAULT;
    }
    if (virtio_backed)
        virtio_gpu_present_fb_rect(fb_state.fb_virt, fb_state.pitch,
                                   cmd.x, cmd.y, cw, ch);
    hyperv_video_dirty(cmd.x, cmd.y, cw, ch);
    spin_lock(&fb_state.lock);
    fb_note_display_complete_locked();
    spin_unlock(&fb_state.lock);
    return 0;
}

static uint64 fb_bo_signal_present_locked(struct fb_gpu_bo_entry *bo)
{
    uint64 fence = fb_state.next_bo_fence++;

    if (fb_state.next_bo_fence == 0)
        fb_state.next_bo_fence = 1;
    bo->last_fence = fence;
    bo->signaled_fence = fence;
    if (bo->gem != NULL) {
        bo->gem->last_fence = fence;
        bo->gem->signaled_fence = fence;
        bo->gem->metadata.implicit_fence = fence;
        bo->gem->metadata.explicit_fence = fence;
        fb_ttm_resv_wakeup_locked(bo->gem,
                                  FB_GPU_DMABUF_POLL_WAKE_PRESENT_SIGNAL);
    }
    fb_state.stats.bo_fences++;
    return fence;
}

static uint64 fb_note_display_complete_locked(void)
{
    uint64 seq = ++fb_state.stats.display_last_present;

    if (seq == 0)
        seq = fb_state.stats.display_last_present = 1;
    fb_state.stats.display_presents++;
    fb_state.stats.display_last_complete = seq;
    fb_state.stats.display_completions++;
    return seq;
}

static int fb_scanout_format_needs_rb_swap(uint32 pixel_format)
{
    return pixel_format == DRM_FORMAT_XBGR8888 ||
           pixel_format == DRM_FORMAT_ABGR8888;
}

static int fb_scanout_format_supported(uint32 pixel_format, uint64 modifier)
{
    if (modifier != DRM_FORMAT_MOD_LINEAR)
        return 0;
    return pixel_format == DRM_FORMAT_XRGB8888 ||
           pixel_format == DRM_FORMAT_ARGB8888 ||
           pixel_format == DRM_FORMAT_XBGR8888 ||
           pixel_format == DRM_FORMAT_ABGR8888;
}

static void fb_copy_scanout_chunk(volatile uint8 *dst, uint8 *src,
                                  uint32 chunk, int swap_rb)
{
    if (!swap_rb) {
        memcpy((void *)dst, src, chunk);
        return;
    }
    for (uint32 i = 0; i < chunk; i += 4) {
        dst[i + 0] = src[i + 2];
        dst[i + 1] = src[i + 1];
        dst[i + 2] = src[i + 0];
        dst[i + 3] = src[i + 3];
    }
}

static int fb_blit_from_bo_format(struct fb_gpu_bo_entry *bo,
                                  struct fb_gpu_bo_present cmd,
                                  uint32 pixel_format, uint64 modifier,
                                  uint64 *fence_out)
{
    uint32 xres, yres;
    uint32 pitch;
    bool virtio_backed;
    uint32 cw, ch;
    uint64 offset;
    uint64 last;
    int clipped = 0;
    int swap_rb;

    if (bo == NULL)
        return -EINVAL;
    if (!fb_scanout_format_supported(pixel_format, modifier))
        return -EOPNOTSUPP;
    if (cmd.w == 0)
        cmd.w = bo->width;
    if (cmd.h == 0)
        cmd.h = bo->height;
    if (cmd.w == 0 || cmd.h == 0)
        return 0;
    if (cmd.w > 0xffffffffU / 4)
        goto reject;

    offset = cmd.pixels;
    last = offset + (uint64)(cmd.h - 1) * bo->pitch + (uint64)cmd.w * 4;
    if (offset >= bo->size || last > bo->size || last < offset)
        goto reject;

    spin_lock(&fb_state.lock);
    xres = fb_state.xres;
    yres = fb_state.yres;
    pitch = fb_state.pitch;
    virtio_backed = fb_state.virtio_backed;
    spin_unlock(&fb_state.lock);

    if (cmd.x >= xres || cmd.y >= yres)
        return 0;

    cw = cmd.w;
    ch = cmd.h;
    if ((uint64)cmd.x + cw > xres) {
        cw = xres - cmd.x;
        clipped = 1;
    }
    if ((uint64)cmd.y + ch > yres) {
        ch = yres - cmd.y;
        clipped = 1;
    }
    if (cw == 0 || ch == 0)
        return 0;
    swap_rb = fb_scanout_format_needs_rb_swap(pixel_format);
    spin_lock(&fb_state.lock);
    if (cmd.x == 0 && cmd.y == 0 && cw == xres && ch == yres)
        fb_state.stats.full_blits++;
    else
        fb_state.stats.partial_blits++;
    if (clipped)
        fb_state.stats.clipped_blits++;
    fb_state.stats.blit_bytes += (uint64)cw * ch * 4;
    fb_state.stats.bo_presents++;
    spin_unlock(&fb_state.lock);

    for (uint32 row = 0; row < ch; row++) {
        uint64 src_off = offset + (uint64)row * bo->pitch;
        volatile uint8 *dst = fb_state.fb_virt +
                              ((uint64)(cmd.y + row) * pitch) +
                              (uint64)cmd.x * 4;
        uint32 remaining = cw * 4;
        uint32 copied = 0;

        while (remaining > 0) {
            uint32 page_idx = src_off / PGSIZE;
            uint32 page_off = src_off & (PGSIZE - 1);
            uint32 chunk = PGSIZE - page_off;
            uint64 pa;
            uint8 *src;

            if (page_idx >= bo->npages)
                goto reject;
            if (chunk > remaining)
                chunk = remaining;
            pa = __page_to_pa(bo->pages[page_idx]);
            src = (uint8 *)PA2VA(pa) + page_off;

            /*
             * fb_virt is PA2VA-mapped cached RAM for Hyper-V synthvid and
             * BGA.  Plain memcpy is the fast path; XBGR/ABGR use an explicit
             * R/B swap so the advertised primary formats are real scanout
             * formats, not just accepted metadata.
             */
            fb_copy_scanout_chunk(dst + copied, src, chunk, swap_rb);

            src_off += chunk;
            copied += chunk;
            remaining -= chunk;
        }
    }
    if (virtio_backed)
        virtio_gpu_present_fb_rect(fb_state.fb_virt, pitch,
                                   cmd.x, cmd.y, cw, ch);
    hyperv_video_dirty(cmd.x, cmd.y, cw, ch);
    spin_lock(&fb_state.lock);
    fb_note_display_complete_locked();
    if (fence_out)
        *fence_out = fb_bo_signal_present_locked(bo);
    spin_unlock(&fb_state.lock);
    return 0;

reject:
    spin_lock(&fb_state.lock);
    fb_state.stats.rejected_blits++;
    spin_unlock(&fb_state.lock);
    return -EINVAL;
}

static int fb_blit_from_bo(struct fb_gpu_bo_entry *bo,
                           struct fb_gpu_bo_present cmd,
                           uint64 *fence_out)
{
    return fb_blit_from_bo_format(bo, cmd, DRM_FORMAT_XRGB8888,
                                  DRM_FORMAT_MOD_LINEAR, fence_out);
}

static int fb_map_scanout_current(struct fb_gpu_scanout_map *req)
{
    vm_t *vm;
    vma_t *vma;
    uint64 addr;
    uint64 flags;
    uint64 pte_flags;
    uint64 fb_phys;
    uint64 fb_virt;
    uint64 map_size;
    uint32 xres, yres, pitch, fb_size;
    int mappable;
    int pfnmap;

    if (req == NULL || current == NULL || current->vm == NULL)
        return -EINVAL;

    spin_lock(&fb_state.lock);
    mappable = fb_state.detected && fb_state.scanout_mappable &&
               fb_state.fb_virt != NULL;
    fb_phys = fb_state.fb_phys;
    fb_virt = (uint64)(uintptr_t)fb_state.fb_virt;
    xres = fb_state.xres;
    yres = fb_state.yres;
    pitch = fb_state.pitch;
    fb_size = fb_state.fb_size;
    spin_unlock(&fb_state.lock);

    if (!mappable || fb_size == 0)
        return -ENODEV;

    map_size = PGROUNDUP((uint64)fb_size);
    if (map_size == 0)
        return -EINVAL;

    vm = current->vm;
    pfnmap = fb_phys != 0;
    flags = PROT_READ | PROT_WRITE | VMA_FLAG_USER |
            VMA_FLAG_DONTFORK | VMA_FLAG_DONTDUMP |
            (pfnmap ? VMA_FLAG_PFNMAP : 0);

    vm_wlock(vm);
    addr = vm_find_free_range(vm,
                              (size_t)(map_size + FB_GPU_D3D12_HEAP_ALIGN),
                              0);
    if (addr == 0) {
        printf("FB: scanout map failed no free user range size=0x%lx\n",
               map_size);
        vm_wunlock(vm);
        return -ENOMEM;
    }
    addr = FB_GPU_ALIGN_UP(addr, FB_GPU_D3D12_HEAP_ALIGN);

    vma = vma_alloc(vm, addr, map_size, flags);
    if (vma == NULL) {
        printf("FB: scanout map vma_alloc failed addr=0x%lx size=0x%lx flags=0x%lx\n",
               addr, map_size, flags);
        vm_wunlock(vm);
        return -ENOMEM;
    }
    if (!pfnmap && anon_vma_prepare(vma) != 0) {
        printf("FB: scanout map anon_vma_prepare failed addr=0x%lx size=0x%lx\n",
               addr, map_size);
        vma_free(vm, vma);
        vm_wunlock(vm);
        return -ENOMEM;
    }

    pte_flags = vma2pte_flags(flags);
    /*
     * Firmware framebuffers used by Hyper-V are MMIO-like PFN ranges.  The
     * kernel's direct map marks that range uncached, but this user mapping is
     * built by hand, so carry the same cache policy here.  Without this,
     * wlcomp can write through a cached alias while Hyper-V keeps scanning out
     * the old boot-logo contents.
     */
#ifdef PTE_PWT
    if (pfnmap)
        pte_flags |= PTE_PWT;
#endif
    for (uint64 off = 0; off < map_size; off += PGSIZE) {
        uint64 va = addr + off;
        uint64 pa;
        page_t *page;

        if (fb_phys != 0) {
            pa = fb_phys + off;
        } else {
            uint64 kva = fb_virt + off;
            pte_t *kpte = NULL;

            if (kva >= (uint64)PA2VA(KERNBASE) &&
                kva < (uint64)PA2VA(PHYSTOP)) {
                pa = VA2PA(kva);
            } else if (kernel_vm != NULL && kernel_vm->pagetable != NULL) {
                kpte = walk(kernel_vm->pagetable, kva, 0, NULL, NULL);
                if (kpte == NULL || !pte_present(kpte)) {
                    printf("FB: scanout map kernel walk failed kva=0x%lx off=0x%lx\n",
                           kva, off);
                    vma_free(vm, vma);
                    vm_wunlock(vm);
                    return -ENODEV;
                }
                pa = pte_pa(kpte) + (kva & (PGSIZE - 1));
            } else {
                vma_free(vm, vma);
                vm_wunlock(vm);
                return -ENODEV;
            }
        }

        if (!pfnmap) {
            page = __pa_to_page(pa);
            if (page == NULL) {
                printf("FB: scanout map unmanaged pa=0x%lx off=0x%lx\n",
                       pa, off);
                vma_free(vm, vma);
                vm_wunlock(vm);
                return -ENODEV;
            }
            if (page_ref_inc((void *)pa) <= 0) {
                printf("FB: scanout map page_ref_inc failed pa=0x%lx off=0x%lx\n",
                       pa, off);
                vma_free(vm, vma);
                vm_wunlock(vm);
                return -ENOMEM;
            }
        } else {
            page = NULL;
        }
        if (mappages(vm->pagetable, va, PGSIZE, pa, pte_flags) != 0) {
            printf("FB: scanout map mappages failed va=0x%lx pa=0x%lx off=0x%lx pte_flags=0x%lx pfnmap=%d\n",
                   va, pa, off, pte_flags, pfnmap);
            if (!pfnmap)
                page_ref_dec((void *)pa);
            vma_free(vm, vma);
            vm_wunlock(vm);
            return -ENOMEM;
        }
        if (!pfnmap)
            page_add_anon_rmap(page, vma, va);
    }
    vm_wunlock(vm);

    memset(req, 0, sizeof(*req));
    req->width = xres;
    req->height = yres;
    req->pitch = pitch;
    req->size = map_size;
    req->addr = addr;
    return 0;
}

static int fb_flush_scanout_rect(struct fb_gpu_scanout_flush req)
{
    uint32 xres, yres;
    uint32 w, h;
    int virtio_backed;

    spin_lock(&fb_state.lock);
    if (!fb_state.detected || !fb_state.scanout_mappable ||
        fb_state.fb_virt == NULL) {
        spin_unlock(&fb_state.lock);
        return -ENODEV;
    }
    virtio_backed = fb_state.virtio_backed;
    xres = fb_state.xres;
    yres = fb_state.yres;
    spin_unlock(&fb_state.lock);

    if (req.w == 0 || req.h == 0)
        return 0;
    if (req.x >= xres || req.y >= yres)
        return 0;

    w = req.w;
    h = req.h;
    if ((uint64)req.x + w > xres)
        w = xres - req.x;
    if ((uint64)req.y + h > yres)
        h = yres - req.y;
    if (w == 0 || h == 0)
        return 0;

    spin_lock(&fb_state.lock);
    if (req.x == 0 && req.y == 0 && w == xres && h == yres)
        fb_state.stats.full_blits++;
    else
        fb_state.stats.partial_blits++;
    fb_state.stats.blit_bytes += (uint64)w * h * 4;
    spin_unlock(&fb_state.lock);

    if (virtio_backed)
        virtio_gpu_present_fb_rect(fb_state.fb_virt, fb_state.pitch,
                                   req.x, req.y, w, h);
    hyperv_video_dirty(req.x, req.y, w, h);
    spin_lock(&fb_state.lock);
    fb_note_display_complete_locked();
    spin_unlock(&fb_state.lock);
    return 0;
}
