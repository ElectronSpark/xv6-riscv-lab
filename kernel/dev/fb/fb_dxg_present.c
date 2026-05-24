
static int fb_dxg_present_source_owner_matches(
    const struct fb_gpu_dxg_present_source_entry *source, uint64 owner_id,
    pid_t owner_tgid)
{
    if (source == NULL)
        return 0;
    if (owner_id != 0)
        return source->owner_id == owner_id;
    if (owner_tgid > 0 && source->owner_id != 0)
        return source->owner_tgid == owner_tgid;
    return 1;
}

static struct fb_gpu_dxg_present_source_entry *
fb_dxg_present_source_lookup_locked(uint32 handle)
{
    if (handle == 0)
        return NULL;
    for (int i = 0; i < FB_GPU_MAX_DXG_PRESENT_SOURCES; i++) {
        if (fb_state.dxg_present_sources[i].in_use &&
            fb_state.dxg_present_sources[i].handle == handle)
            return &fb_state.dxg_present_sources[i];
    }
    return NULL;
}

static void
fb_dxg_present_note_source_shape_locked(
    const struct fb_gpu_dxg_present_source_entry *source)
{
    if (source == NULL)
        return;
    fb_state.stats.dxg_present_last_width = source->width;
    fb_state.stats.dxg_present_last_height = source->height;
    fb_state.stats.dxg_present_last_pitch = source->pitch;
    fb_state.stats.dxg_present_last_format = source->format;
    fb_state.stats.dxg_present_last_allocation_count =
        source->allocation_count;
    fb_state.stats.dxg_present_last_dxg_fd =
        source->dxg_fd < 0 ? 0xffffffffULL : (uint64)(uint32)source->dxg_fd;
    fb_state.stats.dxg_present_last_resource_fd =
        source->resource_fd < 0 ?
        0xffffffffULL : (uint64)(uint32)source->resource_fd;
    fb_state.stats.dxg_present_last_provenance = source->provenance_flags;
    fb_state.stats.dxg_present_last_adapter_luid_low =
        source->adapter_luid_low;
    fb_state.stats.dxg_present_last_adapter_luid_high =
        source->adapter_luid_high;
    fb_state.stats.dxg_present_last_adapter_identity =
        source->adapter_identity;
}

static uint64 fb_dxg_present_required_metadata(uint32 flags)
{
    uint64 metadata = FB_GPU_DXG_PRESENT_META_DEVICE |
                      FB_GPU_DXG_PRESENT_META_RESOURCE |
                      FB_GPU_DXG_PRESENT_META_ALLOCATION |
                      FB_GPU_DXG_PRESENT_META_DIMENSIONS |
                      FB_GPU_DXG_PRESENT_META_FORMAT |
                      FB_GPU_DXG_PRESENT_META_MODIFIER |
                      FB_GPU_DXG_PRESENT_META_ADAPTER_LUID;

    if ((flags & FB_GPU_DXG_PRESENT_F_WAIT_SYNC) != 0)
        metadata |= FB_GPU_DXG_PRESENT_META_SYNC_OBJECT |
                    FB_GPU_DXG_PRESENT_META_FENCE_VALUE;
    return metadata;
}

static uint64 fb_dxg_present_fd_diag(int32 fd)
{
    return fd < 0 ? 0xffffffffULL : (uint64)(uint32)fd;
}

static void fb_dxg_present_hex32(char out[9], uint32 value)
{
    static const char hex[] = "0123456789abcdef";

    for (int i = 7; i >= 0; i--) {
        out[i] = hex[value & 0xfU];
        value >>= 4;
    }
    out[8] = '\0';
}

static uint32 fb_dxg_present_adapter_identity(uint32 luid_low,
                                              uint32 luid_high)
{
    struct hyperv_dxg_status dxg;

    if (luid_low == 0 && luid_high == 0)
        return FB_GPU_DXG_PRESENT_ADAPTER_UNKNOWN;
    if (hyperv_dxg_get_status(&dxg) != 0 ||
        (dxg.user_adapter_luid_low == 0 &&
         dxg.user_adapter_luid_high == 0))
        return FB_GPU_DXG_PRESENT_ADAPTER_UNVERIFIED;
    if (luid_low == dxg.user_adapter_luid_low &&
        luid_high == dxg.user_adapter_luid_high)
        return FB_GPU_DXG_PRESENT_ADAPTER_MATCH;
    return FB_GPU_DXG_PRESENT_ADAPTER_MISMATCH;
}

static uint32 fb_dxg_present_provenance_mask(void)
{
    return FB_GPU_DXG_PRESENT_PROV_DXG_FD |
           FB_GPU_DXG_PRESENT_PROV_RESOURCE_FD |
           FB_GPU_DXG_PRESENT_PROV_D3DKMT_HANDLES |
           FB_GPU_DXG_PRESENT_PROV_DIMENSIONS |
           FB_GPU_DXG_PRESENT_PROV_ADAPTER_LUID;
}

static void fb_dxg_present_sanitize_register_tail(
    struct fb_gpu_dxg_present_source_register *req)
{
    uint32 mask = fb_dxg_present_provenance_mask();

    if (req == NULL)
        return;
    /*
     * The fields after present_source are passive diagnostics.  If an older
     * caller uses the stable register prefix, the extra copyin bytes can be
     * unrelated stack data.  Treat unknown provenance bits or reserved tail
     * bytes as absent optional metadata, while keeping flags strictly checked
     * at their stable prefix offset.
     */
    if (req->reserved1 != 0 || (req->provenance_flags & ~mask) != 0) {
        req->adapter_luid_low = 0;
        req->adapter_luid_high = 0;
        req->provenance_flags = 0;
        req->reserved1 = 0;
    } else {
        req->provenance_flags &= mask;
    }
}

static uint32 fb_dxg_present_source_provenance(
    const struct fb_gpu_dxg_present_source_register *req)
{
    uint32 provenance = 0;

    if (req == NULL)
        return 0;
    if (req->dxg_fd >= 0)
        provenance |= FB_GPU_DXG_PRESENT_PROV_DXG_FD;
    if (req->resource_fd >= 0)
        provenance |= FB_GPU_DXG_PRESENT_PROV_RESOURCE_FD;
    if (req->device != 0 && req->resource != 0 && req->allocation != 0)
        provenance |= FB_GPU_DXG_PRESENT_PROV_D3DKMT_HANDLES;
    if (req->width != 0 && req->height != 0 && req->pitch != 0 &&
        req->format != 0)
        provenance |= FB_GPU_DXG_PRESENT_PROV_DIMENSIONS;
    if (req->adapter_luid_low != 0 || req->adapter_luid_high != 0)
        provenance |= FB_GPU_DXG_PRESENT_PROV_ADAPTER_LUID;
    provenance |= req->provenance_flags & fb_dxg_present_provenance_mask();
    return provenance;
}

static uint64 fb_dxg_present_transport_block_reason_locked(void)
{
    uint64 reason = FB_GPU_DXG_PRESENT_BLOCK_NO_TRANSPORT;

    if ((fb_state.stats.dxg_present_host_rejects &
         FB_GPU_DXG_PRESENT_REJECT_SYNTHVID_GPA_ONLY) != 0)
        reason |= FB_GPU_DXG_PRESENT_BLOCK_SYNTHVID_GPA_ONLY;
    if ((fb_state.stats.dxg_present_host_rejects &
         FB_GPU_DXG_PRESENT_REJECT_DXG_NO_DISPLAY_BIND) != 0)
        reason |= FB_GPU_DXG_PRESENT_BLOCK_DXG_NO_DISPLAY_BIND;
    if ((fb_state.stats.dxg_present_host_rejects &
         FB_GPU_DXG_PRESENT_REJECT_DDA_NO_IMPORT_PATH) != 0)
        reason |= FB_GPU_DXG_PRESENT_BLOCK_DDA_NO_IMPORT_PATH;
    if (fb_state.stats.dxg_present_last_adapter_identity !=
        FB_GPU_DXG_PRESENT_ADAPTER_MATCH)
        reason |= FB_GPU_DXG_PRESENT_BLOCK_LUID_UNVERIFIED;
    if (fb_state.stats.dxg_present_last_adapter_identity ==
        FB_GPU_DXG_PRESENT_ADAPTER_MISMATCH)
        reason |= FB_GPU_DXG_PRESENT_BLOCK_ADAPTER_MISMATCH;
    if (fb_state.stats.dxg_present_helper_transport_present == 0)
        reason |= FB_GPU_DXG_PRESENT_BLOCK_NO_TRANSPORT;
    if (fb_state.stats.dxg_present_helper_requires_completion != 0)
        reason |= FB_GPU_DXG_PRESENT_BLOCK_NO_COMPLETION;
    return reason;
}

static void fb_dxg_present_note_transport_contract_locked(
    const struct fb_gpu_dxg_present_source_entry *source, uint32 flags)
{
    uint64 lifetime = FB_GPU_DXG_PRESENT_LIFE_HANDLES_VALID |
                      FB_GPU_DXG_PRESENT_LIFE_HOST_COMPLETION |
                      FB_GPU_DXG_PRESENT_LIFE_NO_CPU_READBACK;

    if (source != NULL && source->in_use)
        lifetime |= FB_GPU_DXG_PRESENT_LIFE_SOURCE_REGISTERED;
    if ((flags & FB_GPU_DXG_PRESENT_F_WAIT_SYNC) != 0)
        lifetime |= FB_GPU_DXG_PRESENT_LIFE_SYNC_VALID;

    fb_state.stats.dxg_present_helper_contract_version = 1;
    fb_state.stats.dxg_present_helper_required_metadata =
        fb_dxg_present_required_metadata(flags);
    fb_state.stats.dxg_present_helper_transport =
        FB_GPU_DXG_PRESENT_HELPER_TRANSPORT_NONE;
    fb_state.stats.dxg_present_helper_transport_present = 0;
    fb_state.stats.dxg_present_helper_operation =
        FB_GPU_DXG_PRESENT_HELPER_OP_SCANOUT_BIND;
    fb_state.stats.dxg_present_helper_lifetime = lifetime;
    fb_state.stats.dxg_present_helper_source_live =
        source != NULL && source->in_use ? 1 : 0;
    fb_state.stats.dxg_present_helper_requires_completion = 1;
    fb_state.stats.dxg_present_selected_lane =
        FB_GPU_DXG_PRESENT_LANE_HELPER_SCANOUT_BIND;
    fb_state.stats.dxg_present_helper_block_reason =
        fb_dxg_present_transport_block_reason_locked();
}

static void
fb_dxg_present_note_host_lanes_locked(void)
{
    struct hyperv_video_status video;
    struct hyperv_dxg_status dxg;
    uint64 candidates = 0;
    uint64 rejects = 0;
    uint64 synthvid_state = 0;
    uint64 dxg_state = 0;

    /*
     * Linux hyperv_drm/hyperv_fb present through SYNTHVID_VRAM_LOCATION
     * followed by SYNTHVID_DIRT: the host consumes a guest physical VRAM
     * address, not a D3DKMT resource or allocation handle.  WSL dxgkrnl also
     * clears display_supported/indirect_display_device for QAI adapter type,
     * and exposes no D3DKMT display-bind ioctl.  A real native handoff needs a
     * new host ABI that binds {device, resource, allocation, sync/fence} to
     * scanout, or a documented GPU-P/DXG command with equivalent semantics.
     * DDA is represented separately by a real NVIDIA PCI function bound to
     * Nouveau.  The WSL VMBus command enum names present-history,
     * redirected-flip-fence, and BLT commands, but this kernel has no
     * source-backed packet/result contract that converts a runtime D3D12
     * resource into synthvid scanout.
     */
    if (hyperv_video_get_status(&video) == 0 && video.present) {
        candidates |= FB_GPU_DXG_PRESENT_HOST_SYNTHVID;
        if (video.present)
            synthvid_state |= 0x1;
        if (video.gpadl_ok)
            synthvid_state |= 0x2;
        if (video.open_ok)
            synthvid_state |= 0x4;
        if (video.initialized)
            synthvid_state |= 0x8;
        if (video.dirt_needed)
            synthvid_state |= 0x10;
        fb_state.stats.dxg_present_synthvid_vram_gpa = video.vram_gpa;
        rejects |= FB_GPU_DXG_PRESENT_REJECT_SYNTHVID_GPA_ONLY;
    }
    if (hyperv_dxg_get_status(&dxg) == 0 &&
        (dxg.global_present || dxg.vgpu_present)) {
        candidates |= FB_GPU_DXG_PRESENT_HOST_DXG;
        if (dxg.global_present)
            dxg_state |= FB_GPU_DXG_STATE_GLOBAL_PRESENT;
        if (dxg.global_open_ok)
            dxg_state |= FB_GPU_DXG_STATE_GLOBAL_OPEN;
        if (dxg.vgpu_present)
            dxg_state |= FB_GPU_DXG_STATE_VGPU_PRESENT;
        if (dxg.vgpu_open_ok)
            dxg_state |= FB_GPU_DXG_STATE_VGPU_OPEN;
        if (hyperv_dxg_d3dkmt_ready())
            dxg_state |= FB_GPU_DXG_STATE_D3DKMT_READY;
        if (dxg.adapter_paravirtualized)
            dxg_state |= FB_GPU_DXG_STATE_PARAVIRTUALIZED;
        if (!dxg.adapter_display_supported)
            dxg_state |= FB_GPU_DXG_STATE_NO_DISPLAY;
        if (dxg.adapter_source_count == 0)
            dxg_state |= FB_GPU_DXG_STATE_NO_SOURCES;
        fb_state.stats.dxg_present_dxg_adapter_type_raw =
            dxg.adapter_type_raw_value;
        fb_state.stats.dxg_present_dxg_adapter_type_wsl =
            dxg.adapter_type_wsl_value;
        fb_state.stats.dxg_present_dxg_adapter_type_rewrites =
            dxg.adapter_type_rewrites;
        fb_state.stats.dxg_present_dxg_adapter_sources =
            dxg.adapter_source_count;
        fb_state.stats.dxg_present_dxg_adapter_render_supported =
            dxg.adapter_render_supported;
        fb_state.stats.dxg_present_dxg_adapter_display_supported =
            dxg.adapter_display_supported;
        fb_state.stats.dxg_present_dxg_adapter_paravirtualized =
            dxg.adapter_paravirtualized;
        fb_state.stats.dxg_present_dxg_adapter_compute_only =
            dxg.adapter_compute_only;
        fb_state.stats.dxg_present_dxg_adapter_sources_known =
            dxg.adapter_sources_known;
        fb_state.stats.dxg_present_dxg_enum_adapter_count =
            dxg.enum_adapter_count;
        fb_state.stats.dxg_present_dxg_enum_adapter_handle =
            dxg.enum_adapter_handle;
        fb_state.stats.dxg_present_dxg_enum_adapter_luid_low =
            dxg.enum_adapter_luid_low;
        fb_state.stats.dxg_present_dxg_enum_adapter_luid_high =
            dxg.enum_adapter_luid_high;
        fb_state.stats.dxg_present_dxg_user_luid_low =
            dxg.user_adapter_luid_low;
        fb_state.stats.dxg_present_dxg_user_luid_high =
            dxg.user_adapter_luid_high;
        rejects |= FB_GPU_DXG_PRESENT_REJECT_DXG_NO_DISPLAY_BIND;
    }
    if (gpu_nouveau_pci.probed) {
        candidates |= FB_GPU_DXG_PRESENT_HOST_DDA_NOUVEAU;
        fb_state.stats.dxg_present_dda_nouveau_present = 1;
        fb_state.stats.dxg_present_dda_nouveau_import_path_present = 0;
        fb_state.stats.dxg_present_dda_nouveau_scanout_bind_present = 0;
        rejects |= FB_GPU_DXG_PRESENT_REJECT_DDA_NO_IMPORT_PATH;
    } else {
        fb_state.stats.dxg_present_dda_nouveau_present = 0;
        fb_state.stats.dxg_present_dda_nouveau_import_path_present = 0;
        fb_state.stats.dxg_present_dda_nouveau_scanout_bind_present = 0;
        rejects |= FB_GPU_DXG_PRESENT_REJECT_DDA_ABSENT;
    }
    fb_state.stats.dxg_present_host_candidates = candidates;
    fb_state.stats.dxg_present_host_rejects = rejects;
    fb_state.stats.dxg_present_synthvid_state = synthvid_state;
    fb_state.stats.dxg_present_dxg_state = dxg_state;
    fb_state.stats.dxg_present_helper_block_reason =
        fb_dxg_present_transport_block_reason_locked();
}

static int fb_dxg_present_register(uint64 owner_id, pid_t owner_tgid,
                                   struct fb_gpu_dxg_present_source_register *req)
{
    struct fb_gpu_dxg_present_source_entry *source = NULL;
    int print_flags_reject = 0;
    uint32 print_flags = 0;
    uint32 print_device = 0;
    uint32 print_resource = 0;
    uint32 print_allocation = 0;
    uint32 print_width = 0;
    uint32 print_height = 0;
    uint32 print_pitch = 0;
    uint32 print_format = 0;
    uint64 print_dxg_fd = 0xffffffffULL;
    uint64 print_resource_fd = 0xffffffffULL;
    uint64 print_rejects = 0;
    int ret = 0;

    if (req == NULL)
        return -EINVAL;
    fb_dxg_present_sanitize_register_tail(req);
    spin_lock(&fb_state.lock);
    fb_state.stats.dxg_present_register_attempts++;
    fb_state.stats.dxg_present_last_ret = 0;
    fb_state.stats.dxg_present_last_device = req->device;
    fb_state.stats.dxg_present_last_resource = req->resource;
    fb_state.stats.dxg_present_last_allocation = req->allocation;
    fb_state.stats.dxg_present_last_sync = 0;
    fb_state.stats.dxg_present_last_flags = req->flags;
    fb_state.stats.dxg_present_last_fence_value = 0;
    fb_state.stats.dxg_present_last_width = req->width;
    fb_state.stats.dxg_present_last_height = req->height;
    fb_state.stats.dxg_present_last_pitch = req->pitch;
    fb_state.stats.dxg_present_last_format = req->format;
    fb_state.stats.dxg_present_last_allocation_count = req->allocation_count;
    fb_state.stats.dxg_present_last_dxg_fd =
        fb_dxg_present_fd_diag(req->dxg_fd);
    fb_state.stats.dxg_present_last_resource_fd =
        fb_dxg_present_fd_diag(req->resource_fd);
    fb_state.stats.dxg_present_last_provenance =
        fb_dxg_present_source_provenance(req);
    fb_state.stats.dxg_present_last_adapter_luid_low =
        req->adapter_luid_low;
    fb_state.stats.dxg_present_last_adapter_luid_high =
        req->adapter_luid_high;
    fb_state.stats.dxg_present_last_adapter_identity =
        fb_dxg_present_adapter_identity(req->adapter_luid_low,
                                        req->adapter_luid_high);
    fb_state.stats.dxg_present_selected_lane =
        FB_GPU_DXG_PRESENT_LANE_HELPER_SCANOUT_BIND;
    fb_state.stats.dxg_present_display_target_kind =
        FB_GPU_DXG_DISPLAY_TARGET_NONE;
    fb_state.stats.dxg_present_requires_host_protocol = 0;
    fb_state.stats.dxg_present_missing_host_abi =
        FB_GPU_DXG_PRESENT_MISSING_NONE;
    fb_state.stats.dxg_present_host_candidates = 0;
    fb_state.stats.dxg_present_host_rejects = 0;
    fb_state.stats.dxg_present_synthvid_state = 0;
    fb_state.stats.dxg_present_synthvid_vram_gpa = 0;
    fb_state.stats.dxg_present_dxg_state = 0;
    fb_state.stats.dxg_present_helper_block_reason =
        FB_GPU_DXG_PRESENT_BLOCK_NO_TRANSPORT |
        FB_GPU_DXG_PRESENT_BLOCK_LUID_UNVERIFIED;
    fb_dxg_present_note_transport_contract_locked(NULL, 0);

    if (req->flags != 0 || req->dxg_fd < 0 || req->resource_fd < -1 ||
        req->device == 0 || req->resource == 0 || req->allocation == 0 ||
        req->allocation_count == 0 || req->allocation_count > 1024 ||
        req->width == 0 || req->height == 0 ||
        req->width > 16384 || req->height > 16384 ||
        req->width > 0xffffffffU / 4U ||
        req->format == 0 || req->pitch == 0 ||
        req->pitch < req->width * 4U) {
        if (req->flags != 0) {
            print_flags_reject = 1;
            print_flags = req->flags;
            print_device = req->device;
            print_resource = req->resource;
            print_allocation = req->allocation;
            print_width = req->width;
            print_height = req->height;
            print_pitch = req->pitch;
            print_format = req->format;
            print_dxg_fd = fb_dxg_present_fd_diag(req->dxg_fd);
            print_resource_fd = fb_dxg_present_fd_diag(req->resource_fd);
        }
        ret = -EINVAL;
        goto out;
    }

    for (int i = 0; i < FB_GPU_MAX_DXG_PRESENT_SOURCES; i++) {
        if (!fb_state.dxg_present_sources[i].in_use) {
            source = &fb_state.dxg_present_sources[i];
            break;
        }
    }
    if (source == NULL) {
        ret = -ENOSPC;
        goto out;
    }

    req->present_source = fb_state.next_dxg_present_source++;
    if (fb_state.next_dxg_present_source == 0)
        fb_state.next_dxg_present_source = 1;
    memset(source, 0, sizeof(*source));
    source->in_use = 1;
    source->handle = req->present_source;
    source->generation = req->present_source;
    source->owner_id = owner_id;
    source->owner_tgid = owner_tgid;
    source->dxg_fd = req->dxg_fd;
    source->resource_fd = req->resource_fd;
    source->device = req->device;
    source->resource = req->resource;
    source->allocation = req->allocation;
    source->allocation_count = req->allocation_count;
    source->width = req->width;
    source->height = req->height;
    source->pitch = req->pitch;
    source->format = req->format;
    source->modifier = req->modifier;
    source->adapter_luid_low = req->adapter_luid_low;
    source->adapter_luid_high = req->adapter_luid_high;
    source->provenance_flags = fb_dxg_present_source_provenance(req);
    source->adapter_identity =
        fb_dxg_present_adapter_identity(req->adapter_luid_low,
                                        req->adapter_luid_high);
    fb_state.stats.dxg_present_register_successes++;
    fb_state.stats.dxg_present_last_source = req->present_source;
    fb_dxg_present_note_source_shape_locked(source);
    fb_dxg_present_note_transport_contract_locked(source, 0);

out:
    if (ret != 0) {
        fb_state.stats.dxg_present_register_rejects++;
        fb_state.stats.dxg_present_last_ret = (uint64)(-ret);
        print_rejects = fb_state.stats.dxg_present_register_rejects;
    }
    spin_unlock(&fb_state.lock);
    if (print_flags_reject && print_rejects <= 8) {
        printf("fb: dxg present-source register rejected: "
               "reserved flags nonzero flags:0x%x errno:%d "
               "dxg_fd:0x%lx resource_fd:0x%lx dev:0x%x res:0x%x "
               "alloc:0x%x %ux%u pitch:%u fmt:0x%x "
               "abi_hint:register-prefix-append-only\n",
               print_flags, EINVAL, print_dxg_fd, print_resource_fd,
               print_device, print_resource, print_allocation, print_width,
               print_height, print_pitch, print_format);
    }
    return ret;
}

static void fb_dxg_present_source_unregister(uint32 handle)
{
    spin_lock(&fb_state.lock);
    struct fb_gpu_dxg_present_source_entry *source =
        fb_dxg_present_source_lookup_locked(handle);
    if (source != NULL)
        memset(source, 0, sizeof(*source));
    spin_unlock(&fb_state.lock);
}

static void fb_dxg_present_release_owner_sources(uint64 owner_id,
                                                 pid_t owner_tgid)
{
    spin_lock(&fb_state.lock);
    for (int i = 0; i < FB_GPU_MAX_DXG_PRESENT_SOURCES; i++) {
        struct fb_gpu_dxg_present_source_entry *source =
            &fb_state.dxg_present_sources[i];

        if (!source->in_use)
            continue;
        if (!fb_dxg_present_source_owner_matches(source, owner_id,
                                                owner_tgid))
            continue;
        memset(source, 0, sizeof(*source));
        fb_state.stats.dxg_present_release_sources++;
    }
    spin_unlock(&fb_state.lock);
}

static int fb_dxg_present_commit(uint64 owner_id, pid_t owner_tgid,
                                 const struct fb_gpu_dxg_present_source_commit *req)
{
    struct fb_gpu_dxg_present_source_entry *source;
    int print_missing = 0;
    uint32 print_source = 0;
    uint32 print_device = 0;
    uint32 print_resource = 0;
    uint32 print_allocation = 0;
    uint32 print_allocation_count = 0;
    uint32 print_sync = 0;
    uint64 print_fence = 0;
    uint32 print_width = 0;
    uint32 print_height = 0;
    uint32 print_pitch = 0;
    uint32 print_format = 0;
    uint64 print_candidates = 0;
    uint64 print_rejects = 0;
    uint64 print_synthvid_state = 0;
    uint64 print_synthvid_vram = 0;
    uint64 print_dxg_state = 0;
    uint64 print_helper_meta = 0;
    uint64 print_helper_lifetime = 0;
    uint64 print_helper_block = 0;
    uint64 print_provenance = 0;
    uint64 print_no_source = 0;
    uint64 print_bad_flags = 0;
    uint64 print_no_transport = 0;
    uint64 print_no_completion = 0;
    uint64 print_resource_fd_unverified = 0;
    uint64 print_adapter_mismatch = 0;
    uint64 print_commit_rejects = 0;
    uint32 print_lane = 0;
    uint32 print_adapter_identity = 0;
    uint64 print_luid_low = 0;
    uint64 print_luid_high = 0;
    char print_luid_low_hex[9];
    char print_luid_high_hex[9];
    uint64 print_dxg_fd = 0xffffffffULL;
    uint64 print_resource_fd = 0xffffffffULL;
    int ret = -EOPNOTSUPP;

    if (req == NULL)
        return -EINVAL;
    fb_dxg_present_hex32(print_luid_low_hex, 0);
    fb_dxg_present_hex32(print_luid_high_hex, 0);
    spin_lock(&fb_state.lock);
    fb_state.stats.dxg_present_commit_attempts++;
    fb_state.stats.dxg_present_last_source = req->present_source;
    fb_state.stats.dxg_present_last_sync = req->sync_object;
    fb_state.stats.dxg_present_last_flags = req->flags;
    fb_state.stats.dxg_present_last_fence_value = req->fence_value;
    fb_dxg_present_note_transport_contract_locked(NULL, req->flags);

    source = fb_dxg_present_source_lookup_locked(req->present_source);
    if (source == NULL ||
        !fb_dxg_present_source_owner_matches(source, owner_id, owner_tgid)) {
        fb_state.stats.dxg_present_commit_no_source++;
        fb_state.stats.dxg_present_helper_block_reason |=
            FB_GPU_DXG_PRESENT_BLOCK_NO_REGISTERED_SOURCE;
        ret = -ENOENT;
        goto out;
    }
    fb_state.stats.dxg_present_last_device = source->device;
    fb_state.stats.dxg_present_last_resource = source->resource;
    fb_state.stats.dxg_present_last_allocation = source->allocation;
    fb_dxg_present_note_source_shape_locked(source);
    fb_dxg_present_note_transport_contract_locked(source, req->flags);

    if ((req->flags & ~FB_GPU_DXG_PRESENT_F_WAIT_SYNC) != 0 ||
        ((req->flags & FB_GPU_DXG_PRESENT_F_WAIT_SYNC) != 0 &&
         req->sync_object == 0) ||
        ((req->flags & FB_GPU_DXG_PRESENT_F_WAIT_SYNC) == 0 &&
         req->sync_object != 0)) {
        fb_state.stats.dxg_present_commit_bad_flags++;
        ret = -EINVAL;
        goto out;
    }

    fb_state.stats.dxg_present_host_handoff_missing++;
    fb_state.stats.dxg_present_display_target_kind =
        FB_GPU_DXG_DISPLAY_TARGET_NONE;
    fb_state.stats.dxg_present_requires_host_protocol = 1;
    fb_state.stats.dxg_present_missing_host_abi =
        FB_GPU_DXG_PRESENT_MISSING_SCANOUT_BIND;
    fb_dxg_present_note_host_lanes_locked();
    if (source->resource_fd < 0) {
        fb_state.stats.dxg_present_commit_resource_fd_unverified++;
        fb_state.stats.dxg_present_helper_block_reason |=
            FB_GPU_DXG_PRESENT_BLOCK_RESOURCE_FD_UNVERIFIED;
    }
    if (source->adapter_identity == FB_GPU_DXG_PRESENT_ADAPTER_MISMATCH) {
        fb_state.stats.dxg_present_commit_adapter_mismatch++;
        fb_state.stats.dxg_present_helper_block_reason |=
            FB_GPU_DXG_PRESENT_BLOCK_ADAPTER_MISMATCH;
    }
    if (fb_state.stats.dxg_present_helper_transport_present == 0)
        fb_state.stats.dxg_present_commit_no_transport++;
    if (fb_state.stats.dxg_present_helper_requires_completion != 0)
        fb_state.stats.dxg_present_commit_no_completion++;
    if (fb_state.stats.dxg_present_host_handoff_missing <= 8) {
        print_missing = 1;
        print_source = source->handle;
        print_device = source->device;
        print_resource = source->resource;
        print_allocation = source->allocation;
        print_allocation_count = source->allocation_count;
        print_sync = req->sync_object;
        print_fence = req->fence_value;
        print_width = source->width;
        print_height = source->height;
        print_pitch = source->pitch;
        print_format = source->format;
        print_candidates = fb_state.stats.dxg_present_host_candidates;
        print_rejects = fb_state.stats.dxg_present_host_rejects;
        print_synthvid_state = fb_state.stats.dxg_present_synthvid_state;
        print_synthvid_vram = fb_state.stats.dxg_present_synthvid_vram_gpa;
        print_dxg_state = fb_state.stats.dxg_present_dxg_state;
        print_helper_meta =
            fb_state.stats.dxg_present_helper_required_metadata;
        print_helper_lifetime =
            fb_state.stats.dxg_present_helper_lifetime;
        print_helper_block =
            fb_state.stats.dxg_present_helper_block_reason;
        print_provenance = source->provenance_flags;
        print_no_source = fb_state.stats.dxg_present_commit_no_source;
        print_bad_flags = fb_state.stats.dxg_present_commit_bad_flags;
        print_no_transport =
            fb_state.stats.dxg_present_commit_no_transport;
        print_no_completion =
            fb_state.stats.dxg_present_commit_no_completion;
        print_resource_fd_unverified =
            fb_state.stats.dxg_present_commit_resource_fd_unverified;
        print_adapter_mismatch =
            fb_state.stats.dxg_present_commit_adapter_mismatch;
        print_lane = (uint32)fb_state.stats.dxg_present_selected_lane;
        print_adapter_identity = source->adapter_identity;
        print_luid_low = (uint64)source->adapter_luid_low;
        print_luid_high = (uint64)source->adapter_luid_high;
        print_dxg_fd = fb_dxg_present_fd_diag(source->dxg_fd);
        print_resource_fd = fb_dxg_present_fd_diag(source->resource_fd);
    }

out:
    fb_state.stats.dxg_present_commit_rejects++;
    fb_state.stats.dxg_present_last_ret = (uint64)(-ret);
    print_commit_rejects = fb_state.stats.dxg_present_commit_rejects;
    print_helper_block = fb_state.stats.dxg_present_helper_block_reason;
    print_no_source = fb_state.stats.dxg_present_commit_no_source;
    print_bad_flags = fb_state.stats.dxg_present_commit_bad_flags;
    spin_unlock(&fb_state.lock);
    fb_dxg_present_hex32(print_luid_high_hex,
                         (uint32)(print_luid_high & 0xffffffffULL));
    fb_dxg_present_hex32(print_luid_low_hex,
                         (uint32)(print_luid_low & 0xffffffffULL));
    if (!print_missing && print_commit_rejects <= 8) {
        printf("fb: dxg present-source commit rejected: "
               "source:%u flags:0x%x sync:0x%x fence:%lu errno:%d "
               "block_reason:0x%lx no_source:%lu bad_flags:%lu "
               "gpu_p_or_dda_transport_present:0 no_completion:1 "
               "present_id:0 completed:0\n",
               req->present_source, req->flags, req->sync_object,
               req->fence_value, -ret, print_helper_block,
               print_no_source, print_bad_flags);
    }
    if (print_missing) {
        printf("fb: dxg present-source commit blocked: missing "
               "host ABI=%s gpu_p_or_dda_bind=%s source:%u "
               "dev:0x%x res:0x%x alloc:0x%x allocs:%u "
               "sync:0x%x fence:%lu %ux%u pitch:%u fmt:0x%x "
               "candidates:0x%lx rejects:0x%lx synthvid:0x%lx "
               "vram:0x%lx dxg:0x%lx "
               "dependency:bind-runtime-d3d12-resource-through-gpu-p-or-dda "
               "display_contract:v1 transport:%u "
               "operation:%u vmbus_offer:0 hvsock_service:0 custom_host_tool:0 "
               "required_meta:0x%lx lifetime:0x%lx "
               "block_reason:0x%lx lane:%u provenance:0x%lx "
               "block_bits:no_transport=%u,synthvid_gpa=%u,dxg_no_bind=%u,"
               "luid_unverified=%u,no_source=%u,resource_fd_unverified=%u,"
               "adapter_mismatch=%u,no_completion=%u,dda_no_import_path=%u "
               "counters:no_source=%lu,bad_flags=%lu,no_transport=%lu,"
               "no_completion=%lu,resource_fd_unverified=%lu,"
               "adapter_mismatch=%lu "
               "dxg_fd:0x%lx resource_fd:0x%lx luid:%s:%s "
               "adapter_identity:%u "
               "synthvid:gpa-dirty-only dxg_display_bind:0 dda_nouveau:%u "
               "candidate_cmds:presenthistory=%u,redirected_flip_fence=%u,blt=%u "
               "present_id:0 completed:0\n",
               FB_GPU_DXG_MISSING_PRESENT_BIND,
               FB_GPU_DXG_MISSING_PRESENT_HELPER,
               print_source, print_device, print_resource,
               print_allocation, print_allocation_count, print_sync,
               print_fence, print_width, print_height, print_pitch,
               print_format, print_candidates, print_rejects,
               print_synthvid_state, print_synthvid_vram, print_dxg_state,
               FB_GPU_DXG_PRESENT_HELPER_TRANSPORT_NONE,
               FB_GPU_DXG_PRESENT_HELPER_OP_SCANOUT_BIND,
               print_helper_meta, print_helper_lifetime,
               print_helper_block, print_lane, print_provenance,
               (print_helper_block &
                FB_GPU_DXG_PRESENT_BLOCK_NO_TRANSPORT) != 0,
               (print_helper_block &
                FB_GPU_DXG_PRESENT_BLOCK_SYNTHVID_GPA_ONLY) != 0,
               (print_helper_block &
                FB_GPU_DXG_PRESENT_BLOCK_DXG_NO_DISPLAY_BIND) != 0,
               (print_helper_block &
                FB_GPU_DXG_PRESENT_BLOCK_LUID_UNVERIFIED) != 0,
               (print_helper_block &
                FB_GPU_DXG_PRESENT_BLOCK_NO_REGISTERED_SOURCE) != 0,
               (print_helper_block &
                FB_GPU_DXG_PRESENT_BLOCK_RESOURCE_FD_UNVERIFIED) != 0,
               (print_helper_block &
                FB_GPU_DXG_PRESENT_BLOCK_ADAPTER_MISMATCH) != 0,
               (print_helper_block &
                FB_GPU_DXG_PRESENT_BLOCK_NO_COMPLETION) != 0,
               (print_helper_block &
                FB_GPU_DXG_PRESENT_BLOCK_DDA_NO_IMPORT_PATH) != 0,
               print_no_source, print_bad_flags, print_no_transport,
               print_no_completion, print_resource_fd_unverified,
               print_adapter_mismatch,
               print_dxg_fd, print_resource_fd, print_luid_high_hex,
               print_luid_low_hex, print_adapter_identity,
               (print_candidates & FB_GPU_DXG_PRESENT_HOST_DDA_NOUVEAU) != 0,
               FB_GPU_DXG_WSL_PRESENTHISTORYTOKEN_CMD,
               FB_GPU_DXG_WSL_SETREDIRECTEDFLIPFENCEVALUE_CMD,
               FB_GPU_DXG_WSL_BLT_CMD);
    }
    return ret;
}

static int fb_dxg_present_query(uint64 owner_id, pid_t owner_tgid,
                                struct fb_gpu_dxg_present_source_query *req)
{
    struct fb_gpu_dxg_present_source_entry *source;
    uint32 source_handle;
    uint64 helper_meta;
    uint64 helper_lifetime;

    if (req == NULL)
        return -EINVAL;

    spin_lock(&fb_state.lock);
    fb_state.stats.dxg_present_query_attempts++;
    fb_state.stats.dxg_present_query_rejects++;
    fb_state.stats.dxg_present_last_ret = EOPNOTSUPP;

    source_handle = req->present_source != 0 ?
        req->present_source : (uint32)fb_state.stats.dxg_present_last_source;
    source = fb_dxg_present_source_lookup_locked(source_handle);
    if (source != NULL &&
        !fb_dxg_present_source_owner_matches(source, owner_id, owner_tgid))
        source = NULL;

    if (source != NULL) {
        fb_state.stats.dxg_present_last_source = source->handle;
        fb_state.stats.dxg_present_last_device = source->device;
        fb_state.stats.dxg_present_last_resource = source->resource;
        fb_state.stats.dxg_present_last_allocation = source->allocation;
        fb_dxg_present_note_source_shape_locked(source);
    }
    fb_dxg_present_note_transport_contract_locked(
        source, (uint32)fb_state.stats.dxg_present_last_flags);
    helper_meta = fb_state.stats.dxg_present_helper_required_metadata;
    helper_lifetime = fb_state.stats.dxg_present_helper_lifetime;

    memset(req, 0, sizeof(*req));
    req->present_source = source_handle;
    req->display_target_kind =
        (uint32)fb_state.stats.dxg_present_display_target_kind;
    req->source_live = source != NULL ? 1 : 0;
    req->present_id = 0;
    req->completed = 0;
    req->host_handoff_missing =
        fb_state.stats.dxg_present_host_handoff_missing;
    req->requires_host_protocol = 1;
    req->missing_host_abi = FB_GPU_DXG_PRESENT_MISSING_SCANOUT_BIND;
    req->helper_contract_version =
        fb_state.stats.dxg_present_helper_contract_version;
    req->helper_required_metadata = helper_meta;
    req->helper_transport = fb_state.stats.dxg_present_helper_transport;
    req->helper_transport_present =
        fb_state.stats.dxg_present_helper_transport_present;
    req->helper_operation = fb_state.stats.dxg_present_helper_operation;
    req->helper_lifetime = helper_lifetime;
    req->helper_requires_completion =
        fb_state.stats.dxg_present_helper_requires_completion;
    if (source != NULL) {
        req->device = source->device;
        req->resource = source->resource;
        req->allocation = source->allocation;
        req->allocation_count = source->allocation_count;
    } else {
        req->device = (uint32)fb_state.stats.dxg_present_last_device;
        req->resource = (uint32)fb_state.stats.dxg_present_last_resource;
        req->allocation = (uint32)fb_state.stats.dxg_present_last_allocation;
        req->allocation_count =
            (uint32)fb_state.stats.dxg_present_last_allocation_count;
    }
    req->sync_object = (uint32)fb_state.stats.dxg_present_last_sync;
    req->last_flags = (uint32)fb_state.stats.dxg_present_last_flags;
    req->fence_value = fb_state.stats.dxg_present_last_fence_value;
    req->last_ret = EOPNOTSUPP;
    if (source != NULL) {
        req->dxg_fd = source->dxg_fd;
        req->resource_fd = source->resource_fd;
        req->provenance_flags = source->provenance_flags;
        req->adapter_luid_low = source->adapter_luid_low;
        req->adapter_luid_high = source->adapter_luid_high;
        req->adapter_identity = source->adapter_identity;
    } else {
        req->dxg_fd = (int32)fb_state.stats.dxg_present_last_dxg_fd;
        req->resource_fd =
            (int32)fb_state.stats.dxg_present_last_resource_fd;
        req->provenance_flags =
            (uint32)fb_state.stats.dxg_present_last_provenance;
        req->adapter_luid_low =
            (uint32)fb_state.stats.dxg_present_last_adapter_luid_low;
        req->adapter_luid_high =
            (uint32)fb_state.stats.dxg_present_last_adapter_luid_high;
        req->adapter_identity =
            (uint32)fb_state.stats.dxg_present_last_adapter_identity;
    }
    req->selected_lane = (uint32)fb_state.stats.dxg_present_selected_lane;
    req->helper_block_reason =
        (uint32)fb_state.stats.dxg_present_helper_block_reason;
    req->host_candidates = fb_state.stats.dxg_present_host_candidates;
    req->host_rejects = fb_state.stats.dxg_present_host_rejects;
    spin_unlock(&fb_state.lock);

    return -EOPNOTSUPP;
}

static uint32 fb_dxg_present_bind_contract_provenance(
    const struct fb_gpu_dxg_present_host_bind_contract *req)
{
    uint32 provenance = 0;

    if (req == NULL)
        return 0;
    if (req->dxg_fd >= 0)
        provenance |= FB_GPU_DXG_PRESENT_PROV_DXG_FD;
    if (req->resource_fd >= 0)
        provenance |= FB_GPU_DXG_PRESENT_PROV_RESOURCE_FD;
    if (req->device != 0 && req->resource != 0 && req->allocation != 0)
        provenance |= FB_GPU_DXG_PRESENT_PROV_D3DKMT_HANDLES;
    if (req->width != 0 && req->height != 0 && req->pitch != 0 &&
        req->format != 0)
        provenance |= FB_GPU_DXG_PRESENT_PROV_DIMENSIONS;
    if (req->adapter_luid_low != 0 || req->adapter_luid_high != 0)
        provenance |= FB_GPU_DXG_PRESENT_PROV_ADAPTER_LUID;
    provenance |= (uint32)req->provenance_flags &
                  fb_dxg_present_provenance_mask();
    return provenance;
}

static uint64 fb_dxg_present_resource_generation(uint32 device,
                                                 uint32 resource,
                                                 uint32 allocation,
                                                 uint32 allocation_count)
{
    uint64 gen = 1469598103934665603ULL;

    gen ^= device;
    gen *= 1099511628211ULL;
    gen ^= resource;
    gen *= 1099511628211ULL;
    gen ^= allocation;
    gen *= 1099511628211ULL;
    gen ^= allocation_count;
    gen *= 1099511628211ULL;
    return gen == 0 ? 1 : gen;
}

static int fb_dxg_present_bind_contract_query(
    uint64 owner_id, pid_t owner_tgid,
    struct fb_gpu_dxg_present_host_bind_contract *req)
{
    struct fb_gpu_dxg_present_source_entry *source = NULL;
    uint32 flags;
    uint32 adapter_identity;
    uint32 provenance;
    uint32 source_live = 0;
    uint64 source_generation = 0;
    uint64 resource_generation = 0;
    int ret = -EOPNOTSUPP;

    if (req == NULL)
        return -EINVAL;

    spin_lock(&fb_state.lock);
    if (req->present_source != 0) {
        source = fb_dxg_present_source_lookup_locked(req->present_source);
        if (source != NULL &&
            !fb_dxg_present_source_owner_matches(source, owner_id,
                                                 owner_tgid))
            source = NULL;
        if (source != NULL) {
            source_live = 1;
            source_generation = source->generation;
            req->dxg_fd = source->dxg_fd;
            req->resource_fd = source->resource_fd;
            req->device = source->device;
            req->resource = source->resource;
            req->allocation = source->allocation;
            req->allocation_count = source->allocation_count;
            req->width = source->width;
            req->height = source->height;
            req->pitch = source->pitch;
            req->format = source->format;
            req->modifier = source->modifier;
            req->adapter_luid_low = source->adapter_luid_low;
            req->adapter_luid_high = source->adapter_luid_high;
            req->provenance_flags = source->provenance_flags;
        }
    }
    flags = req->flags;
    adapter_identity = fb_dxg_present_adapter_identity(
        req->adapter_luid_low, req->adapter_luid_high);
    provenance = fb_dxg_present_bind_contract_provenance(req);
    resource_generation = fb_dxg_present_resource_generation(
        req->device, req->resource, req->allocation, req->allocation_count);

    if ((req->version != 0 && req->version != 1) ||
        (flags & ~FB_GPU_DXG_PRESENT_F_WAIT_SYNC) != 0 ||
        ((flags & FB_GPU_DXG_PRESENT_F_WAIT_SYNC) != 0 &&
         req->sync_object == 0) ||
        ((flags & FB_GPU_DXG_PRESENT_F_WAIT_SYNC) == 0 &&
         req->sync_object != 0) ||
        req->dxg_fd < 0 || req->resource_fd < -1 ||
        req->device == 0 || req->resource == 0 || req->allocation == 0 ||
        req->allocation_count == 0 || req->width == 0 ||
        req->height == 0 || req->pitch == 0 || req->format == 0 ||
        req->width > 16384 || req->height > 16384 ||
        req->width > 0xffffffffU / 4U ||
        req->pitch < req->width * 4U)
        ret = -EINVAL;

    fb_state.stats.dxg_present_bind_contract_queries++;
    fb_state.stats.dxg_present_last_ret = (uint64)(-ret);
    fb_state.stats.dxg_present_last_source = req->present_source;
    fb_state.stats.dxg_present_last_device = req->device;
    fb_state.stats.dxg_present_last_resource = req->resource;
    fb_state.stats.dxg_present_last_allocation = req->allocation;
    fb_state.stats.dxg_present_last_sync = req->sync_object;
    fb_state.stats.dxg_present_last_flags = flags;
    fb_state.stats.dxg_present_last_fence_value = req->fence_value;
    fb_state.stats.dxg_present_last_width = req->width;
    fb_state.stats.dxg_present_last_height = req->height;
    fb_state.stats.dxg_present_last_pitch = req->pitch;
    fb_state.stats.dxg_present_last_format = req->format;
    fb_state.stats.dxg_present_last_allocation_count =
        req->allocation_count;
    fb_state.stats.dxg_present_last_dxg_fd =
        fb_dxg_present_fd_diag(req->dxg_fd);
    fb_state.stats.dxg_present_last_resource_fd =
        fb_dxg_present_fd_diag(req->resource_fd);
    fb_state.stats.dxg_present_last_provenance = provenance;
    fb_state.stats.dxg_present_last_adapter_luid_low =
        req->adapter_luid_low;
    fb_state.stats.dxg_present_last_adapter_luid_high =
        req->adapter_luid_high;
    fb_state.stats.dxg_present_last_adapter_identity = adapter_identity;
    fb_state.stats.dxg_present_selected_lane =
        FB_GPU_DXG_PRESENT_LANE_GPUP_DXG_SCANOUT_BIND;
    fb_state.stats.dxg_present_display_target_kind =
        FB_GPU_DXG_DISPLAY_TARGET_NONE;
    fb_state.stats.dxg_present_requires_host_protocol = 1;
    fb_state.stats.dxg_present_missing_host_abi =
        FB_GPU_DXG_PRESENT_MISSING_SCANOUT_BIND;
    fb_dxg_present_note_transport_contract_locked(NULL, flags);
    fb_dxg_present_note_host_lanes_locked();
    if (req->present_source != 0 && !source_live)
        fb_state.stats.dxg_present_helper_block_reason |=
            FB_GPU_DXG_PRESENT_BLOCK_NO_REGISTERED_SOURCE;
    if (req->resource_fd < 0)
        fb_state.stats.dxg_present_helper_block_reason |=
            FB_GPU_DXG_PRESENT_BLOCK_RESOURCE_FD_UNVERIFIED;
    if (adapter_identity == FB_GPU_DXG_PRESENT_ADAPTER_MISMATCH)
        fb_state.stats.dxg_present_helper_block_reason |=
            FB_GPU_DXG_PRESENT_BLOCK_ADAPTER_MISMATCH;
    if (ret == -EOPNOTSUPP)
        fb_state.stats.dxg_present_bind_contract_rejects++;
    else if (ret == 0)
        fb_state.stats.dxg_present_bind_contract_successes++;
    else
        fb_state.stats.dxg_present_bind_contract_rejects++;

    req->version = 1;
    req->transport = (uint32)fb_state.stats.dxg_present_helper_transport;
    req->operation = (uint32)fb_state.stats.dxg_present_helper_operation;
    req->flags = flags;
    req->source_live = source_live;
    req->present_id = 0;
    req->completed = 0;
    req->provenance_flags = provenance;
    req->selected_lane = fb_state.stats.dxg_present_selected_lane;
    req->helper_block_reason =
        fb_state.stats.dxg_present_helper_block_reason;
    req->required_metadata =
        fb_state.stats.dxg_present_helper_required_metadata;
    req->lifetime = fb_state.stats.dxg_present_helper_lifetime;
    req->host_candidates = fb_state.stats.dxg_present_host_candidates;
    req->host_rejects = fb_state.stats.dxg_present_host_rejects;
    req->source_generation = source_generation;
    req->resource_generation = resource_generation;
    req->completion_source = FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY;
    req->adapter_identity = adapter_identity;
    req->reserved = 0;
    req->reserved2 = 0;
    spin_unlock(&fb_state.lock);

    return ret;
}
