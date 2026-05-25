
#define FB_GPU_MAX_BOS 128
#define FB_GPU_MAX_KMS_FBS 32
#define FB_GPU_MAX_SYNCOBJS 128
#define FB_GPU_MAX_SYNCOBJ_STATES 128
#define FB_GPU_MAX_DXG_PRESENT_SOURCES 32
#define FB_GPU_DRM_EVENT_QUEUE_CAPACITY DRM_XV6_EVENT_QUEUE_CAPACITY
#define FB_GPU_SYNTHETIC_VBLANK_PERIOD_TICKS (10000000ULL / 60ULL)
#define FB_GPU_DXG_MISSING_PRESENT_BIND "dxg-resource-scanout-bind"
#define FB_GPU_DXG_MISSING_PRESENT_HELPER \
    "gpu-p-dxg-resource-scanout-bind"
#define FB_GPU_DXG_WSL_PRESENTHISTORYTOKEN_CMD 34U
#define FB_GPU_DXG_WSL_SETREDIRECTEDFLIPFENCEVALUE_CMD 35U
#define FB_GPU_DXG_WSL_BLT_CMD 38U
#define FB_GPU_DXG_WSL_PROPAGATE_PRESENTHISTORYTOKEN_CMD 1U
#define FB_GPU_D3D12_HEAP_ALIGN (64ULL * 1024ULL)
#define FB_GPU_ALIGN_UP(x, a) ((((uint64)(x)) + ((uint64)(a) - 1)) & ~((uint64)(a) - 1))

#define GPU_DRM_CRTC_ID                       1
#define GPU_DRM_ENCODER_ID                    2
#define GPU_DRM_CONNECTOR_ID                  3
#define GPU_DRM_PRIMARY_PLANE_ID              4
#define GPU_DRM_MODE_BLOB_ID                  5
#define GPU_DRM_IN_FORMATS_BLOB_ID            6
#define GPU_DRM_PROP_CRTC_ID                  10
#define GPU_DRM_PROP_MODE_ID                  11
#define GPU_DRM_PROP_ACTIVE                   12
#define GPU_DRM_PROP_PLANE_TYPE               13
#define GPU_DRM_PROP_FB_ID                    14
#define GPU_DRM_PROP_SRC_X                    15
#define GPU_DRM_PROP_SRC_Y                    16
#define GPU_DRM_PROP_SRC_W                    17
#define GPU_DRM_PROP_SRC_H                    18
#define GPU_DRM_PROP_CRTC_X                   19
#define GPU_DRM_PROP_CRTC_Y                   20
#define GPU_DRM_PROP_CRTC_W                   21
#define GPU_DRM_PROP_CRTC_H                   22
#define GPU_DRM_PROP_IN_FENCE_FD              23
#define GPU_DRM_PROP_OUT_FENCE_PTR            24
#define GPU_DRM_PROP_IN_FORMATS               25

#define FB_TTM_PL_SYSTEM                      0x0001
#define FB_TTM_PL_TT                          0x0002
#define FB_TTM_PL_VRAM                        0x0004
#define FB_TTM_PL_STOLEN                      0x0008
#define FB_TTM_MEM_SYSTEM                     0
#define FB_TTM_MEM_TT                         1
#define FB_TTM_MEM_VRAM                       2
#define FB_TTM_MEM_STOLEN                     3

#define GPU_DRM_MMAP_HANDLE_SHIFT 32
#define GPU_DRM_MMAP_OFFSET(handle) ((uint64)(handle) << GPU_DRM_MMAP_HANDLE_SHIFT)
#define GPU_DRM_MMAP_HANDLE(offset) ((uint32)((offset) >> GPU_DRM_MMAP_HANDLE_SHIFT))
#define GPU_DRM_MMAP_PAGE(offset) \
    (((offset) & ((1ULL << GPU_DRM_MMAP_HANDLE_SHIFT) - 1)) >> PGSHIFT)

struct fb_gpu_resv_shared_fence {
    uint64 seq;
    uint64 fence;
    uint64 owner_id;
    pid_t owner_tgid;
    uint32 attach_point;
    uint32 exporter_tag;
};

struct fb_gpu_ww_acquire_ctx {
    uint64 stamp;
    uint64 owner_id;
    pid_t owner_tgid;
    uint32 acquired;
    uint32 contended;
    uint32 retries;
};

struct fb_gpu_bo_entry {
    int in_use;
    int dead;
    uint32 handle;
    uint32 refs;
    uint32 gem_id;
    int stats_accounted;
    uint64 owner_id;
    pid_t owner_tgid;
    uint32 width;
    uint32 height;
    uint32 pitch;
    uint32 npages;
    uint64 size;
    uint64 last_fence;
    uint64 signaled_fence;
    uint32 ttm_placement;
    uint32 ttm_mem_type;
    uint32 ttm_pin_count;
    uint32 ttm_reservation_seq;
    uint32 ttm_tt_populated;
    uint32 ttm_sg_nents;
    uint64 ttm_dma_addr_base;
    uint64 ttm_lru_seq;
    uint64 ttm_move_count;
    uint64 ttm_resv_count;
    uint64 ttm_resv_seq;
    uint64 ttm_resv_exclusive_fence;
    uint64 ttm_resv_owner_id;
    pid_t ttm_resv_owner_tgid;
    uint32 ttm_resv_shared_count;
    uint32 ttm_resv_shared_next;
    uint32 ttm_resv_waiters;
    uint64 ttm_resv_wakeup_seq;
    struct fb_gpu_resv_shared_fence
        ttm_resv_shared[FB_GPU_RESV_SHARED_SLOTS];
    uint32 nouveau_domain;
    uint32 nouveau_tile_mode;
    uint32 nouveau_tile_flags;
    uint32 dmabuf_attachment_accounted;
    uint32 dmabuf_importer_tag;
    page_t **pages;
    struct fb_gpu_gem_object *gem;
};

struct fb_gpu_dmabuf_metadata {
    uint32 format;
    uint64 modifier;
    uint32 plane_count;
    uint32 offsets[4];
    uint32 strides[4];
    uint64 implicit_fence;
    uint64 explicit_fence;
};

struct fb_gpu_gem_object {
    int in_use;
    int dead;
    uint32 id;
    uint32 refs;
    uint32 width;
    uint32 height;
    uint32 pitch;
    uint32 npages;
    uint64 size;
    uint64 last_fence;
    uint64 signaled_fence;
    uint32 ttm_placement;
    uint32 ttm_mem_type;
    uint32 ttm_pin_count;
    uint32 ttm_reservation_seq;
    uint32 ttm_tt_populated;
    uint32 ttm_sg_nents;
    uint64 ttm_dma_addr_base;
    uint64 ttm_lru_seq;
    uint64 ttm_move_count;
    uint64 ttm_resv_count;
    uint64 ttm_resv_seq;
    uint64 ttm_resv_exclusive_fence;
    uint64 ttm_resv_owner_id;
    pid_t ttm_resv_owner_tgid;
    uint32 ttm_resv_shared_count;
    uint32 ttm_resv_shared_next;
    uint32 ttm_resv_waiters;
    uint64 ttm_resv_wakeup_seq;
    uint32 dmabuf_poll_armed;
    uint32 dmabuf_poll_fired;
    uint32 dmabuf_poll_pending_to_ready_reported;
    uint32 dmabuf_poll_events;
    uint32 dmabuf_poll_last_source;
    uint64 dmabuf_poll_target_fence;
    uint64 dmabuf_poll_armed_wakeup_seq;
    struct fb_gpu_resv_shared_fence
        ttm_resv_shared[FB_GPU_RESV_SHARED_SLOTS];
    uint32 nouveau_domain;
    uint32 nouveau_tile_mode;
    uint32 nouveau_tile_flags;
    struct fb_gpu_dmabuf_metadata metadata;
    page_t **pages;
};

struct fb_gpu_dmabuf_object {
    struct fb_gpu_gem_object *gem;
    uint32 exporter_tag;
    uint32 importer_tag;
    uint32 attachment_count;
    uint32 import_count;
    int live_accounted;
    uint64 ttm_resv_seq_snapshot;
    uint64 ttm_resv_exclusive_fence_snapshot;
    uint32 ttm_resv_shared_count_snapshot;
    uint64 ttm_resv_shared_fence_snapshot;
};

struct fb_ttm_resource_manager {
    uint32 mem_type;
    uint64 limit;
    uint64 used;
    uint64 evictions;
    uint64 moves;
    uint64 cpu_copy_fallback_moves;
    uint64 metadata_noop_moves;
    uint64 unsupported_hw_copy_moves;
    uint64 real_copy_moves;
    uint64 lru_seq;
};

struct fb_gpu_kms_fb_entry {
    int in_use;
    uint32 fb_id;
    uint32 bo_handle;
    uint32 bo_handles[4];
    uint64 owner_id;
    pid_t owner_tgid;
    uint32 width;
    uint32 height;
    uint32 pitch;
    uint32 pitches[4];
    uint32 offsets[4];
    uint32 plane_count;
    uint32 pixel_format;
    uint64 modifier;
};

struct fb_gpu_syncobj_entry {
    int in_use;
    uint32 handle;
    uint64 owner_id;
    pid_t owner_tgid;
    uint32 state_index;
};

struct fb_gpu_syncobj_state_entry {
    int in_use;
    uint32 refs;
    int signaled;
    uint64 timeline_value;
    uint32 waiters;
    uint64 wakeup_seq;
    uint64 reservation_seq;
    uint64 reservation_fence;
    uint32 proxy_source_index;
    uint64 proxy_point;
    uint32 proxy_kind;
    uint32 pending_sync_file_callbacks;
    uint64 sync_file_callback_generation;
    uint64 sync_file_callback_fired_generation;
    uint32 pending_wait_callbacks;
    uint64 wait_callback_generation;
    uint64 wait_callback_fired_generation;
};

struct fb_gpu_dxg_present_source_entry {
    int in_use;
    uint32 handle;
    uint64 generation;
    uint64 owner_id;
    pid_t owner_tgid;
    int32 dxg_fd;
    int32 resource_fd;
    uint32 resource_fd_kind;
    uint32 resource_fd_sealed;
    uint32 resource_fd_shared_records_valid;
    uint32 resource_fd_matches_handles;
    uint64 resource_fd_generation;
    uint32 device;
    uint32 resource;
    uint32 allocation;
    uint32 allocation_count;
    uint32 width;
    uint32 height;
    uint32 pitch;
    uint32 format;
    uint64 modifier;
    uint32 adapter_luid_low;
    uint32 adapter_luid_high;
    uint32 provenance_flags;
    uint32 adapter_identity;
    uint64 display_bind_attempts;
    uint64 display_bind_present_id;
    uint64 display_bind_completed_id;
    uint64 display_bind_source_generation;
    uint64 display_bind_resource_generation;
    uint64 display_bind_status;
    uint64 display_bind_block_reason;
    uint32 display_bind_completion_source;
    uint32 display_bind_pin_active;
    struct hyperv_dxg_display_bind_pin_snapshot display_bind_pin;
};

struct fb_gpu_fence {
    uint32 refs;
    uint64 context;
    uint64 seqno;
    int signaled;
    int error;
    uint32 waiters;
    uint64 wakeup_seq;
    uint32 pending_callbacks;
    uint64 callback_generation;
    uint64 callback_fired_generation;
};

struct fb_gpu_fence_file {
    struct fb_gpu_bo_entry *bo;
    uint64 fence;
    struct fb_gpu_fence *fence_obj;
    int live_accounted;
    int pending_callback_armed;
    uint64 pending_callback_generation;
};

struct fb_gpu_virgl_fence_file {
    uint64 fence;
    int live_accounted;
};

struct fb_gpu_syncobj_file {
    uint32 kind;
    uint32 state_index;
    int snapshot_signaled;
    uint64 snapshot_timeline_value;
    uint64 snapshot_reservation_fence;
    int pending_callback_armed;
    uint64 pending_callback_generation;
};

#define FB_GPU_SYNCOBJ_FD_OPAQUE    1
#define FB_GPU_SYNCOBJ_FD_SYNC_FILE 2

/* ── I/O port helpers ────────────────────────────────────────────── */

static inline void fb_outb(uint16 port, uint8 val)
{
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8 fb_inb(uint16 port)
{
    uint8 val;
    asm volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void fb_outw(uint16 port, uint16 val)
{
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16 fb_inw(uint16 port)
{
    uint16 val;
    asm volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* ── BGA register access ─────────────────────────────────────────── */

static void bga_write_reg(uint16 index, uint16 value)
{
    fb_outw(VBE_DISPI_IOPORT_INDEX, index);
    fb_outw(VBE_DISPI_IOPORT_DATA, value);
}

static uint16 bga_read_reg(uint16 index)
{
    fb_outw(VBE_DISPI_IOPORT_INDEX, index);
    return fb_inw(VBE_DISPI_IOPORT_DATA);
}

/* ── Module state ─────────────────────────────────────────────────── */

static struct {
    int         detected;       /* non-zero if a fbdev-compatible scanout exists */
    int         virtio_backed;  /* shadow framebuffer presented via virtio-gpu */
    int         firmware_backed; /* bootloader/firmware linear framebuffer */
    uint64      fb_phys;        /* physical address of LFB (BAR0), if any */
    volatile uint8 *fb_virt;    /* kernel virtual address of framebuffer */
    int         scanout_mappable; /* scanout can be mapped into userspace */
    uint32      xres;
    uint32      yres;
    uint32      bpp;
    uint32      pitch;          /* bytes per scanline */
    uint32      fb_size;        /* total framebuffer size in bytes */
    struct fb_gpu_stats stats;
    uint32      next_bo_handle;
    uint32      next_gem_id;
    uint32      next_kms_fb_id;
    uint32      next_syncobj_handle;
    uint32      next_dxg_present_source;
    uint64      next_bo_fence;
    uint64      next_fence_context;
    uint64      next_render_owner_id;
    uint64      next_ww_acquire_stamp;
    uint64      ttm_lru_clock;
    uint64      ttm_evictions;
    struct fb_gpu_bo_entry bos[FB_GPU_MAX_BOS];
    struct fb_gpu_gem_object gems[FB_GPU_MAX_BOS];
    struct fb_ttm_resource_manager ttm_mgr[4];
    struct fb_gpu_kms_fb_entry kms_fbs[FB_GPU_MAX_KMS_FBS];
    struct fb_gpu_syncobj_entry syncobjs[FB_GPU_MAX_SYNCOBJS];
    struct fb_gpu_syncobj_state_entry syncobj_states[FB_GPU_MAX_SYNCOBJ_STATES];
    uint32      syncobj_waiters;
    uint64      syncobj_wakeup_seq;
    uint32      current_kms_fb_id;
    struct fb_gpu_dxg_present_source_entry dxg_present_sources[FB_GPU_MAX_DXG_PRESENT_SOURCES];
    spinlock_t  lock;           /* serializes concurrent access */
} fb_state = {
    .next_bo_handle = 1,
    .next_gem_id = 1,
    .next_kms_fb_id = 100,
    .next_syncobj_handle = 1,
    .next_dxg_present_source = 1,
    .next_bo_fence = 1,
    .next_fence_context = 1,
    .next_render_owner_id = 1,
    .next_ww_acquire_stamp = 1,
    .stats.dxg_scanout_bind_candidate_cmds_known = 4,
    .stats.dxg_scanout_bind_candidate_presenthistory_cmd =
        FB_GPU_DXG_WSL_PRESENTHISTORYTOKEN_CMD,
    .stats.dxg_scanout_bind_candidate_redirected_flip_fence_cmd =
        FB_GPU_DXG_WSL_SETREDIRECTEDFLIPFENCEVALUE_CMD,
    .stats.dxg_scanout_bind_candidate_blt_cmd = FB_GPU_DXG_WSL_BLT_CMD,
    .stats.dxg_scanout_bind_candidate_propagate_presenthistory_cmd =
        FB_GPU_DXG_WSL_PROPAGATE_PRESENTHISTORYTOKEN_CMD,
    .stats.dxg_scanout_bind_candidate_vmbus_enum_known = 1,
    .stats.dxg_scanout_bind_candidate_reject_reasons =
        FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_ALL,
    .stats.nouveau_pci_native_present_credit = 0,
    .stats.nouveau_native_display_ready = 0,
    .stats.nouveau_dda_native_display_present = 0,
    .stats.nouveau_display_create_attempts = 0,
    .stats.nouveau_display_create_successes = 0,
    .stats.nouveau_display_create_fail_closed = 0,
    .stats.nouveau_display_heads = 0,
    .stats.nouveau_display_connectors = 0,
    .stats.nouveau_display_vblank_supported = 0,
    .stats.nouveau_display_vblank_irqs = 0,
    .stats.nouveau_display_page_flip_completions = 0,
    .stats.nouveau_native_display_reject_reasons =
        FB_GPU_KMS_PRESENT_REJECT_ALL,
    .stats.kms_present_last_lane = FB_GPU_KMS_PRESENT_LANE_NONE,
    .stats.kms_present_dumb = 0,
    .stats.kms_present_synthvid = 0,
    .stats.kms_present_nouveau_hw = 0,
    .stats.kms_present_rejects = 0,
    .stats.kms_present_reject_reasons =
        FB_GPU_KMS_PRESENT_REJECT_ALL,
    .stats.kms_present_reject_no_native_display = 0,
    .stats.kms_present_reject_no_nouveau_display = 0,
    .stats.kms_present_reject_no_display_create = 0,
    .stats.kms_present_reject_no_heads = 0,
    .stats.kms_present_reject_no_connectors = 0,
    .stats.kms_present_reject_no_vblank = 0,
    .stats.kms_present_reject_no_hw_completion = 0,
    .lock = SPINLOCK_INITIALIZED("fb"),
};

static int fb_kernel_range_has_pages(uint64 base, uint64 size);
static void gpu_backend_fill(struct fb_gpu_backend_info *info);
static int gpu_backend_query(uint64 arg);
static uint64 fb_note_display_complete_locked(void);
static void fb_ttm_resv_wakeup_locked(struct fb_gpu_gem_object *gem,
                                      uint32 source);
static void fb_ttm_propagate_gem_locked(struct fb_gpu_bo_entry *bo);
static void fb_ttm_resv_mark_stale_locked(void);
static int fb_ttm_resv_wait_fence_locked(struct fb_gpu_gem_object *gem,
                                         uint64 target);
static void gpu_kms_sample_vblank_locked(uint64 min_sequence,
                                         uint64 *sequence_out,
                                         uint64 *timestamp_ns_out);

static uint64 fb_gpu_fence_next_context_locked(void)
{
    uint64 context = fb_state.next_fence_context++;

    if (fb_state.next_fence_context == 0)
        fb_state.next_fence_context = 1;
    if (context == 0)
        context = fb_state.next_fence_context++;
    return context;
}

static struct fb_gpu_fence *fb_gpu_fence_create(uint64 context,
                                                uint64 seqno,
                                                int signaled,
                                                int error)
{
    struct fb_gpu_fence *fence = kvmalloc(sizeof(*fence));

    if (fence == NULL)
        return NULL;
    memset(fence, 0, sizeof(*fence));

    spin_lock(&fb_state.lock);
    fence->refs = 1;
    fence->context = context != 0 ? context :
        fb_gpu_fence_next_context_locked();
    fence->seqno = seqno;
    fence->signaled = signaled != 0;
    fence->error = error;
    fb_state.stats.fence_objects_created++;
    fb_state.stats.fence_objects_live++;
    if (fb_state.stats.fence_objects_live >
        fb_state.stats.fence_objects_peak)
        fb_state.stats.fence_objects_peak =
            fb_state.stats.fence_objects_live;
    if (fence->signaled)
        fb_state.stats.fence_objects_signaled++;
    if (fence->error != 0)
        fb_state.stats.fence_objects_errors++;
    spin_unlock(&fb_state.lock);
    return fence;
}

static int fb_gpu_fence_signal_locked(struct fb_gpu_fence *fence, int error)
{
    uint32 waiters;

    if (fence == NULL)
        return -EINVAL;
    if (fence->signaled) {
        if (fence->error == 0 && error != 0) {
            fence->error = error;
            fb_state.stats.fence_objects_errors++;
        }
        return 0;
    }

    fence->signaled = 1;
    fence->error = error;
    fence->wakeup_seq++;
    waiters = fence->waiters;
    fb_state.stats.fence_objects_signaled++;
    if (error != 0)
        fb_state.stats.fence_objects_errors++;
    if (waiters != 0) {
        fb_state.stats.fence_objects_wakeups += waiters;
        wakeup_on_chan(&fence->wakeup_seq);
    }
    if (fence->pending_callbacks != 0) {
        fb_state.stats.fence_objects_callbacks_fired +=
            fence->pending_callbacks;
        fence->pending_callbacks = 0;
        fence->callback_fired_generation = fence->callback_generation;
    }
    return 0;
}

static void fb_gpu_fence_get_locked(struct fb_gpu_fence *fence)
{
    if (fence == NULL)
        return;
    fence->refs++;
}

static void fb_gpu_fence_put(struct fb_gpu_fence *fence)
{
    int do_free = 0;

    if (fence == NULL)
        return;

    spin_lock(&fb_state.lock);
    if (fence->refs != 0) {
        fence->refs--;
        fb_state.stats.fence_objects_ref_puts++;
        if (fence->refs == 0) {
            if (fb_state.stats.fence_objects_live > 0)
                fb_state.stats.fence_objects_live--;
            do_free = 1;
        }
    }
    spin_unlock(&fb_state.lock);

    if (do_free)
        kvfree(fence);
}

static int fb_gpu_fence_wait_locked(struct fb_gpu_fence *fence)
{
    int ret = 0;

    if (fence == NULL)
        return -EINVAL;
    fb_state.stats.fence_objects_waits++;
    while (!fence->signaled && fence->error == 0) {
        fence->waiters++;
        fb_state.stats.fence_objects_wait_queued++;
        ret = sleep_on_chan_interruptible(&fence->wakeup_seq,
                                          &fb_state.lock);
        if (fence->waiters > 0)
            fence->waiters--;
        if (ret != 0)
            return ret;
    }
    if (fence->error != 0)
        return fence->error < 0 ? fence->error : -fence->error;
    return 0;
}

static uint64 fb_gpu_fence_query_locked(const struct fb_gpu_fence *fence,
                                        int *signaled, int *error)
{
    if (signaled != NULL)
        *signaled = fence != NULL && fence->signaled;
    if (error != NULL)
        *error = fence != NULL ? fence->error : -EINVAL;
    return fence != NULL ? fence->seqno : 0;
}

static uint64 fb_gpu_fence_file_context_locked(struct fb_gpu_bo_entry *bo)
{
    if (bo == NULL)
        return 0;
    if (bo->gem != NULL)
        return ((uint64)bo->gem->id << 32) | 1U;
    return ((uint64)bo->handle << 32) | 2U;
}

static uint64 fb_gpu_fence_file_signaled_locked(
    const struct fb_gpu_fence_file *fence_file)
{
    struct fb_gpu_bo_entry *bo;

    if (fence_file == NULL)
        return 0;
    bo = fence_file->bo;
    if (bo == NULL)
        return fence_file->fence_obj != NULL &&
            fence_file->fence_obj->signaled ? fence_file->fence : 0;
    return bo->gem != NULL ? bo->gem->signaled_fence :
        bo->signaled_fence;
}

static int fb_gpu_fence_file_refresh_locked(
    struct fb_gpu_fence_file *fence_file)
{
    struct fb_gpu_bo_entry *bo;
    uint64 signaled;

    if (fence_file == NULL || fence_file->fence_obj == NULL)
        return -EINVAL;
    bo = fence_file->bo;
    if (bo == NULL)
        return fence_file->fence_obj->signaled ? 0 : -EAGAIN;

    signaled = fb_gpu_fence_file_signaled_locked(fence_file);
    if (signaled >= fence_file->fence)
        return fb_gpu_fence_signal_locked(fence_file->fence_obj, 0);
    return 0;
}

static int fb_gpu_fence_file_wait_locked(
    struct fb_gpu_fence_file *fence_file)
{
    struct fb_gpu_bo_entry *bo;
    int ret;

    if (fence_file == NULL || fence_file->fence_obj == NULL)
        return -EINVAL;
    bo = fence_file->bo;
    if (bo == NULL)
        return fb_gpu_fence_wait_locked(fence_file->fence_obj);

    if (bo->gem != NULL) {
        ret = fb_ttm_resv_wait_fence_locked(bo->gem, fence_file->fence);
        if (ret != 0)
            return ret;
        (void)fb_gpu_fence_file_refresh_locked(fence_file);
        if (bo->gem->signaled_fence < fence_file->fence)
            return -EAGAIN;
        return 0;
    }
    if (fence_file->fence > bo->last_fence) {
        fb_ttm_resv_mark_stale_locked();
        return -EAGAIN;
    }
    (void)fb_gpu_fence_file_refresh_locked(fence_file);
    if (bo->signaled_fence < fence_file->fence)
        return -EAGAIN;
    return 0;
}

static uint32 fb_pack_rgb(uint8 r, uint8 g, uint8 b)
{
    if (fb_state.firmware_backed && platform.has_framebuffer) {
        return ((uint32)r << platform.framebuffer_red_pos) |
               ((uint32)g << platform.framebuffer_green_pos) |
               ((uint32)b << platform.framebuffer_blue_pos);
    }

    return ((uint32)r << 16) | ((uint32)g << 8) | b;
}

static void fb_fill_rect(uint32 x, uint32 y, uint32 w, uint32 h, uint32 color)
{
    if (fb_state.fb_virt == NULL || fb_state.bpp != 32 ||
        x >= fb_state.xres || y >= fb_state.yres)
        return;

    if (x + w > fb_state.xres)
        w = fb_state.xres - x;
    if (y + h > fb_state.yres)
        h = fb_state.yres - y;

    for (uint32 row = 0; row < h; row++) {
        volatile uint32 *dst = (volatile uint32 *)
            (fb_state.fb_virt + (uint64)(y + row) * fb_state.pitch +
             (uint64)x * sizeof(uint32));
        for (uint32 col = 0; col < w; col++)
            dst[col] = color;
    }
}

static void fb_draw_boot_glyph(uint32 x, uint32 y, uint32 scale,
                               const uint8 glyph[7], uint32 color)
{
    for (uint32 row = 0; row < 7; row++) {
        for (uint32 col = 0; col < 5; col++) {
            if (glyph[row] & (1U << (4 - col)))
                fb_fill_rect(x + col * scale, y + row * scale,
                             scale, scale, color);
        }
    }
}

static const uint8 *fb_boot_glyph_for(char ch)
{
    static const uint8 glyph_space[7] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8 glyph_6[7] = {
        0x0f, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e,
    };
    static const uint8 glyph_d[7] = {
        0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e,
    };
    static const uint8 glyph_e[7] = {
        0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f,
    };
    static const uint8 glyph_k[7] = {
        0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11,
    };
    static const uint8 glyph_o[7] = {
        0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e,
    };
    static const uint8 glyph_p[7] = {
        0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10,
    };
    static const uint8 glyph_s[7] = {
        0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e,
    };
    static const uint8 glyph_t[7] = {
        0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    };
    static const uint8 glyph_v[7] = {
        0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04,
    };
    static const uint8 glyph_x[7] = {
        0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11,
    };

    switch (ch) {
    case '6': return glyph_6;
    case 'D':
    case 'd': return glyph_d;
    case 'E':
    case 'e': return glyph_e;
    case 'K':
    case 'k': return glyph_k;
    case 'O':
    case 'o': return glyph_o;
    case 'P':
    case 'p': return glyph_p;
    case 'S':
    case 's': return glyph_s;
    case 'T':
    case 't': return glyph_t;
    case 'V':
    case 'v': return glyph_v;
    case 'X':
    case 'x': return glyph_x;
    default: return glyph_space;
    }
}

static uint32 fb_boot_text_width(const char *text, uint32 scale, uint32 gap)
{
    uint32 width = 0;

    for (const char *p = text; *p; p++) {
        if (p != text)
            width += gap;
        width += (*p == ' ') ? (3 * scale) : (5 * scale);
    }

    return width;
}

static void fb_draw_boot_text(uint32 x, uint32 y, uint32 scale,
                              const char *text, uint32 color)
{
    uint32 gap = scale;

    for (const char *p = text; *p; p++) {
        if (*p == ' ') {
            x += 3 * scale + gap;
            continue;
        }
        fb_draw_boot_glyph(x, y, scale, fb_boot_glyph_for(*p), color);
        x += 5 * scale + gap;
    }
}

static void fb_paint_boot_logo(void)
{
    const char *title = "XV6 Desktop";

    if (fb_state.fb_virt == NULL || fb_state.bpp != 32 ||
        fb_state.xres == 0 || fb_state.yres == 0)
        return;

    uint32 bg = fb_pack_rgb(10, 22, 38);
    for (uint32 y = 0; y < fb_state.yres; y++) {
        volatile uint32 *row = (volatile uint32 *)
            (fb_state.fb_virt + (uint64)y * fb_state.pitch);
        for (uint32 x = 0; x < fb_state.xres; x++)
            row[x] = bg;
    }

    uint32 scale = fb_state.xres / 80;
    if (scale < 8)
        scale = 8;
    if (scale > 18)
        scale = 18;

    uint32 gap = scale;
    uint32 logo_w = fb_boot_text_width(title, scale, gap);
    uint32 logo_h = 7 * scale;
    uint32 x = fb_state.xres > logo_w ? (fb_state.xres - logo_w) / 2 : 16;
    uint32 y = fb_state.yres > logo_h ? (fb_state.yres - logo_h) / 2 : 16;

    fb_fill_rect(x - scale, y - scale, logo_w + 2 * scale,
                 logo_h + 2 * scale, fb_pack_rgb(15, 35, 58));
    fb_draw_boot_text(x, y, scale, title, fb_pack_rgb(116, 198, 255));
    fb_fill_rect(x - scale, y + logo_h + scale / 2,
                 logo_w + 2 * scale, scale / 2 + 1,
                 fb_pack_rgb(86, 230, 170));
    __sync_synchronize();
}

static cdev_t gpu_cdev;

static const struct drm_core_driver fb_drm_driver = {
    .driver_name = "xv6_gpu",
    .desc = "xv6 DRM compatibility GPU facade",
    .driver_major = 0,
    .driver_minor = 1,
    .driver_patchlevel = 0,
};
static const struct drm_core_driver nouveau_drm_driver = {
    .driver_name = "nouveau",
    .desc = "xv6 native Nouveau DRM compatibility facade",
    .driver_major = 1,
    .driver_minor = 3,
    .driver_patchlevel = 0,
};
static struct drm_core_device fb_drm_device;

struct fb_gpu_render_owner;
static enum drm_core_node_type gpu_drm_node_from_cdev(cdev_t *cdev);
static int gpu_drm_is_primary_like(struct fb_gpu_render_owner *owner);
static struct pci_device_info *gpu_nouveau_device(void);
static int gpu_nouveau_pci_probe(struct pci_device_info *pdev,
                                 const struct pci_device_id *id);
static void gpu_nouveau_pci_remove(struct pci_device_info *pdev);
static int gpu_nouveau_pci_suspend(struct pci_device_info *pdev);
static int gpu_nouveau_pci_resume(struct pci_device_info *pdev);

struct gpu_nouveau_pci_state {
    int registered;
    int probed;
    int irq_vector;
    int irq_registered;
    int irq_number;
    struct pci_device_info *pdev;
    void *bar0;
    void *bar1;
    uint64 bar0_len;
    uint64 bar1_len;
    int bar0_claimed;
    int bar1_claimed;
    uint8 irq_pin;
    uint8 msi_cap;
    uint8 msix_cap;
};

static struct gpu_nouveau_pci_state gpu_nouveau_pci;

static const struct pci_device_id gpu_nouveau_pci_ids[] = {
    {
        .vendor = PCI_VENDOR_NVIDIA,
        .device = PCI_ANY_ID,
        .subvendor = PCI_ANY_ID,
        .subdevice = PCI_ANY_ID,
        .class = PCI_CLASS_DISPLAY << 16,
        .class_mask = 0xff0000,
    },
    {0},
};

static struct pci_driver gpu_nouveau_pci_driver = {
    .name = "nouveau",
    .id_table = gpu_nouveau_pci_ids,
    .probe = gpu_nouveau_pci_probe,
    .remove = gpu_nouveau_pci_remove,
    .suspend = gpu_nouveau_pci_suspend,
    .resume = gpu_nouveau_pci_resume,
};

struct fb_gpu_render_owner {
    uint64 id;
    pid_t tgid;
    struct drm_core_file drm;
    uint32 default_ctx_id;
    uint32 capset_id;
    int nouveau_channel;
    uint32 nouveau_channel_handle;
    uint32 nouveau_pushbuf_domains;
    uint32 nouveau_next_notifier_offset;
    int nouveau_vm_initialized;
    struct {
        uint32 handle;
        uint32 class_id;
        uint32 kind;
        uint32 size;
        uint32 offset;
    } nouveau_objects[16];
    struct drm_event_vblank_compat drm_events[FB_GPU_DRM_EVENT_QUEUE_CAPACITY];
    uint32 drm_event_head;
    uint32 drm_event_tail;
    uint32 drm_event_count;
    uint32 drm_event_high_water;
    int lifecycle_live_accounted;
};
