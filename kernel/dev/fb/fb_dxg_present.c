
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

struct fb_dxg_display_bind_request {
    uint32 present_source;
    uint64 source_generation;
    uint64 resource_generation;
    uint32 flags;
    uint32 sync_object;
    uint64 fence_value;
    int32 dxg_fd;
    int32 resource_fd;
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
    uint32 adapter_identity;
    uint32 provenance_flags;
    uint64 required_metadata;
    uint64 lifetime;
    uint64 block_reason;
    uint32 pin_valid;
    struct hyperv_dxg_display_bind_pin_snapshot pin;
};

struct fb_dxg_display_bind_result {
    int status;
    uint32 transport;
    uint32 operation;
    uint32 completion_source;
    uint64 present_id;
    uint64 completed_id;
    uint64 source_generation;
    uint64 resource_generation;
    uint64 block_reason;
    uint32 host_abi_present;
    uint32 sender_present;
    uint32 completion_present;
    uint32 pin_revalidated;
    uint32 no_host_abi;
    uint32 no_sender;
    uint32 no_completion;
    uint32 publication_attempted;
    uint32 publish_before_send;
    uint64 transport_pending_id;
    uint32 command_id;
    uint64 transaction_id;
    uint32 channel;
    uint32 completion_demux_registered;
    uint32 transport_source;
    uint32 host_saw_packet;
    uint32 wsl_presenthistory_completion_credit;
    uint32 resolved_or_cancelled;
    uint32 refs_released;
    uint32 no_host_abi_cancelled;
    uint32 no_host_abi_refs_released;
    uint64 pending_owner_generation;
    uint64 pending_source_generation;
    uint64 pending_resource_generation;
    uint64 pending_dxgprocess_generation;
    uint64 pending_process_adapter_generation;
    uint32 pending_hmgr_index_unique_valid;
    uint32 pending_parent_resource_ref_held;
    uint32 pending_opened_child_ref_held;
    uint32 pending_syncobject_ref_held;
    uint32 pending_owner_close_cancelled;
    uint32 request_metadata_complete;
    uint32 request_sync_metadata_complete;
    uint64 request_missing_metadata;
};

static uint64 fb_dxg_present_resource_generation(uint32 device,
                                                 uint32 resource,
                                                 uint32 allocation,
                                                 uint32 allocation_count);

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

static void fb_dxg_present_note_wsl_candidate_namespace_locked(void)
{
    fb_state.stats.dxg_scanout_bind_candidate_presenthistory_cmd =
        FB_GPU_DXG_WSL_PRESENTHISTORYTOKEN_CMD;
    fb_state.stats.dxg_scanout_bind_candidate_redirected_flip_fence_cmd =
        FB_GPU_DXG_WSL_SETREDIRECTEDFLIPFENCEVALUE_CMD;
    fb_state.stats.dxg_scanout_bind_candidate_blt_cmd =
        FB_GPU_DXG_WSL_BLT_CMD;
    fb_state.stats.dxg_scanout_bind_candidate_propagate_presenthistory_cmd =
        FB_GPU_DXG_WSL_PROPAGATE_PRESENTHISTORYTOKEN_CMD;
    fb_state.stats.dxg_scanout_bind_candidate_cmds_known = 4;
    fb_state.stats.dxg_scanout_bind_candidate_vmbus_enum_known = 1;
    fb_state.stats.dxg_scanout_bind_wsl_ioctl_namespace_checked = 1;
    fb_state.stats.dxg_scanout_bind_wsl_display_bind_ioctl_absent = 1;
    fb_state.stats.dxg_scanout_bind_candidate_linux_ioctl_contracts = 0;
    fb_state.stats.dxg_scanout_bind_candidate_resource_bind_contracts = 0;
    fb_state.stats.dxg_scanout_bind_candidate_display_completion_contracts = 0;
    fb_state.stats.dxg_scanout_bind_standard_alloc_private_data = 1;
    fb_state.stats.dxg_scanout_bind_standard_alloc_display_bind_absent = 1;
    fb_state.stats.dxg_scanout_bind_candidate_reject_reasons |=
        FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_ALL;
}

static int
fb_dxg_present_provider_submit_display_bind(
    const struct fb_dxg_display_bind_request *bind,
    struct fb_dxg_display_bind_result *result)
{
    struct hyperv_dxg_display_bind_request hv_bind;
    struct hyperv_dxg_display_bind_result hv_result;
    int ret;

    if (bind == NULL || result == NULL)
        return -EINVAL;
    memset(result, 0, sizeof(*result));
    memset(&hv_bind, 0, sizeof(hv_bind));
    memset(&hv_result, 0, sizeof(hv_result));

    hv_bind.present_source = bind->present_source;
    hv_bind.source_generation = bind->source_generation;
    hv_bind.resource_generation = bind->resource_generation;
    hv_bind.flags = bind->flags;
    hv_bind.sync_object = bind->sync_object;
    hv_bind.fence_value = bind->fence_value;
    hv_bind.dxg_fd = bind->dxg_fd;
    hv_bind.resource_fd = bind->resource_fd;
    hv_bind.device = bind->device;
    hv_bind.resource = bind->resource;
    hv_bind.allocation = bind->allocation;
    hv_bind.allocation_count = bind->allocation_count;
    hv_bind.width = bind->width;
    hv_bind.height = bind->height;
    hv_bind.pitch = bind->pitch;
    hv_bind.format = bind->format;
    hv_bind.modifier = bind->modifier;
    hv_bind.adapter_luid_low = bind->adapter_luid_low;
    hv_bind.adapter_luid_high = bind->adapter_luid_high;
    hv_bind.adapter_identity = bind->adapter_identity;
    hv_bind.provenance_flags = bind->provenance_flags;
    hv_bind.required_metadata = bind->required_metadata;
    hv_bind.lifetime = bind->lifetime;
    hv_bind.block_reason = bind->block_reason;
    hv_bind.pin_valid = bind->pin_valid;
    hv_bind.pin = bind->pin;

    ret = hyperv_dxg_display_bind_submit(&hv_bind, &hv_result);
    result->status = hv_result.status;
    result->transport = hv_result.transport;
    result->operation = hv_result.operation;
    result->completion_source = hv_result.completion_source;
    result->present_id = hv_result.present_id;
    result->completed_id = hv_result.completed_id;
    result->source_generation = hv_result.source_generation;
    result->resource_generation = hv_result.resource_generation;
    result->block_reason = hv_result.block_reason;
    result->host_abi_present = hv_result.host_abi_present;
    result->sender_present = hv_result.sender_present;
    result->completion_present = hv_result.completion_present;
    result->pin_revalidated = hv_result.pin_revalidated;
    result->no_host_abi = hv_result.no_host_abi;
    result->no_sender = hv_result.no_sender;
    result->no_completion = hv_result.no_completion;
    result->publication_attempted = hv_result.publication_attempted;
    result->publish_before_send = hv_result.publish_before_send;
    result->transport_pending_id = hv_result.transport_pending_id;
    result->command_id = hv_result.command_id;
    result->transaction_id = hv_result.transaction_id;
    result->channel = hv_result.channel;
    result->completion_demux_registered =
        hv_result.completion_demux_registered;
    result->transport_source = hv_result.transport_source;
    result->host_saw_packet = hv_result.host_saw_packet;
    result->wsl_presenthistory_completion_credit =
        hv_result.wsl_presenthistory_completion_credit;
    result->resolved_or_cancelled = hv_result.resolved_or_cancelled;
    result->refs_released = hv_result.refs_released;
    result->no_host_abi_cancelled = hv_result.no_host_abi_cancelled;
    result->no_host_abi_refs_released =
        hv_result.no_host_abi_refs_released;
    result->pending_owner_generation = hv_result.pending_owner_generation;
    result->pending_source_generation = hv_result.pending_source_generation;
    result->pending_resource_generation =
        hv_result.pending_resource_generation;
    result->pending_dxgprocess_generation =
        hv_result.pending_dxgprocess_generation;
    result->pending_process_adapter_generation =
        hv_result.pending_process_adapter_generation;
    result->pending_hmgr_index_unique_valid =
        hv_result.pending_hmgr_index_unique_valid;
    result->pending_parent_resource_ref_held =
        hv_result.pending_parent_resource_ref_held;
    result->pending_opened_child_ref_held =
        hv_result.pending_opened_child_ref_held;
    result->pending_syncobject_ref_held =
        hv_result.pending_syncobject_ref_held;
    result->pending_owner_close_cancelled =
        hv_result.pending_owner_close_cancelled;
    result->request_metadata_complete =
        hv_result.request_metadata_complete;
    result->request_sync_metadata_complete =
        hv_result.request_sync_metadata_complete;
    result->request_missing_metadata =
        hv_result.request_missing_metadata;
    return ret;
}

static int
fb_dxg_present_display_bind_result_accepts(
    const struct fb_dxg_display_bind_request *bind,
    const struct fb_dxg_display_bind_result *result,
    int submit_ret)
{
    if (bind == NULL || result == NULL)
        return 0;
    return submit_ret == 0 &&
           result->status == 0 &&
           result->transport != FB_GPU_DXG_PRESENT_GPUP_DDA_TRANSPORT_NONE &&
           result->transport_source ==
               FB_GPU_DXG_DISPLAY_BIND_SOURCE_NON_WSL_DXGKRNL_EXTENSION &&
           result->operation == FB_GPU_DXG_PRESENT_GPUP_DDA_OP_SCANOUT_BIND &&
           result->host_abi_present != 0 &&
           result->sender_present != 0 &&
           result->completion_present != 0 &&
           result->pin_revalidated != 0 &&
           result->publication_attempted != 0 &&
           result->publish_before_send != 0 &&
           result->transport_pending_id != 0 &&
           result->command_id != 0 &&
           result->transaction_id != 0 &&
           result->channel != 0 &&
           result->completion_demux_registered != 0 &&
           result->host_saw_packet != 0 &&
           result->wsl_presenthistory_completion_credit == 0 &&
           result->no_host_abi == 0 &&
           result->no_sender == 0 &&
           result->no_completion == 0 &&
           result->block_reason == 0 &&
           result->source_generation == bind->source_generation &&
           result->resource_generation == bind->resource_generation &&
           result->present_id != 0 &&
           result->completed_id >= result->present_id &&
           result->completion_source == FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY;
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
    if ((fb_state.stats.dxg_present_host_rejects &
         FB_GPU_DXG_PRESENT_REJECT_WSL_ENUM_ONLY) != 0 ||
        (fb_state.stats.dxg_scanout_bind_candidate_reject_reasons &
         FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_ENUM_ONLY) != 0)
        reason |= FB_GPU_DXG_PRESENT_BLOCK_WSL_ENUM_ONLY;
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
    fb_dxg_present_note_wsl_candidate_namespace_locked();
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

    fb_state.stats.dxg_scanout_bind_synthvid_resource_bind_absent = 1;

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
        fb_state.stats.dxg_scanout_bind_synthvid_gpa_dirty_present = 1;
        fb_state.stats.dxg_scanout_bind_synthvid_resource_bind_absent = 1;
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
        rejects |= FB_GPU_DXG_PRESENT_REJECT_WSL_ENUM_ONLY;
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
    fb_state.stats.dxg_scanout_bind_dda_pci_display_present =
        fb_state.stats.dxg_present_dda_nouveau_present;
    fb_state.stats.dxg_scanout_bind_dda_resource_import_absent =
        fb_state.stats.dxg_present_dda_nouveau_import_path_present == 0;
    fb_state.stats.dxg_scanout_bind_dda_scanout_bind_absent =
        fb_state.stats.dxg_present_dda_nouveau_scanout_bind_present == 0;
    fb_state.stats.dxg_scanout_bind_dda_hw_flip_completion_absent = 1;
    fb_state.stats.dxg_present_host_candidates = candidates;
    fb_state.stats.dxg_present_host_rejects = rejects;
    fb_state.stats.dxg_present_synthvid_state = synthvid_state;
    fb_state.stats.dxg_present_dxg_state = dxg_state;
    fb_dxg_present_note_wsl_candidate_namespace_locked();
    fb_state.stats.dxg_scanout_bind_candidate_sender_contracts = 0;
    fb_state.stats.dxg_scanout_bind_candidate_completion_contracts = 0;
    fb_state.stats.dxg_present_helper_block_reason =
        fb_dxg_present_transport_block_reason_locked();
}

static uint64
fb_dxg_present_source_resource_generation_locked(
    const struct fb_gpu_dxg_present_source_entry *source)
{
    if (source == NULL)
        return 0;
    if (source->resource_fd_generation != 0)
        return source->resource_fd_generation;
    return fb_dxg_present_resource_generation(source->device,
                                             source->resource,
                                             source->allocation,
                                             source->allocation_count);
}

static void
fb_dxg_present_sync_display_bind_state_locked(
    const struct fb_gpu_dxg_present_source_entry *source)
{
    fb_state.stats.dxg_display_bind_contract_version =
        fb_state.stats.dxg_present_helper_contract_version;
    fb_state.stats.dxg_display_bind_backend =
        fb_state.stats.dxg_present_selected_lane;
    fb_state.stats.dxg_display_bind_transport =
        fb_state.stats.dxg_present_helper_transport;
    fb_state.stats.dxg_display_bind_transport_present =
        fb_state.stats.dxg_present_helper_transport_present;
    fb_state.stats.dxg_display_bind_operation =
        fb_state.stats.dxg_present_helper_operation;
    fb_state.stats.dxg_display_bind_required_metadata =
        fb_state.stats.dxg_present_helper_required_metadata;
    fb_state.stats.dxg_display_bind_lifetime =
        fb_state.stats.dxg_present_helper_lifetime;
    fb_state.stats.dxg_display_bind_block_reason =
        fb_state.stats.dxg_present_helper_block_reason;
    fb_state.stats.dxg_display_bind_completion_source =
        source != NULL && source->display_bind_completion_source != 0 ?
        source->display_bind_completion_source :
        FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY;
    fb_state.stats.dxg_display_bind_present_id =
        source != NULL && source->display_bind_attempts != 0 ?
        source->display_bind_present_id :
        fb_state.stats.dxg_scanout_bind_last_present_id;
    fb_state.stats.dxg_display_bind_completed_id =
        source != NULL && source->display_bind_attempts != 0 ?
        source->display_bind_completed_id :
        fb_state.stats.dxg_scanout_bind_last_completed;
    fb_state.stats.dxg_display_bind_source_generation =
        source != NULL && source->display_bind_attempts != 0 ?
        source->display_bind_source_generation :
        source != NULL ? source->generation :
        fb_state.stats.dxg_scanout_bind_last_source_generation;
    fb_state.stats.dxg_display_bind_resource_generation =
        source != NULL && source->display_bind_attempts != 0 ?
        source->display_bind_resource_generation :
        source != NULL ? fb_dxg_present_source_resource_generation_locked(source) :
        fb_state.stats.dxg_scanout_bind_last_resource_generation;
    fb_state.stats.dxg_display_bind_status =
        source != NULL && source->display_bind_attempts != 0 ?
        source->display_bind_status :
        fb_state.stats.dxg_scanout_bind_last_status;
}

static void
fb_dxg_present_clear_display_bind_release_locked(
    const struct fb_gpu_dxg_present_source_entry *source)
{
    int had_source_bind =
        source != NULL &&
        (source->display_bind_attempts != 0 ||
         source->display_bind_present_id != 0 ||
         source->display_bind_completed_id != 0 ||
         source->display_bind_source_generation != 0 ||
         source->display_bind_resource_generation != 0);
    int had_global_bind =
        fb_state.stats.dxg_display_bind_present_id != 0 ||
        fb_state.stats.dxg_display_bind_completed_id != 0 ||
        fb_state.stats.dxg_scanout_bind_last_present_id != 0 ||
        fb_state.stats.dxg_scanout_bind_last_completed != 0;

    if (had_source_bind || had_global_bind)
        fb_state.stats.dxg_display_bind_release_clears++;
    if (had_global_bind)
        fb_state.stats.dxg_display_bind_stale_completion_rejects++;

    fb_state.stats.dxg_display_bind_present_id = 0;
    fb_state.stats.dxg_display_bind_completed_id = 0;
    fb_state.stats.dxg_display_bind_status = EOPNOTSUPP;
    fb_state.stats.dxg_scanout_bind_last_present_id = 0;
    fb_state.stats.dxg_scanout_bind_last_completed = 0;
}

static uint64
fb_dxg_present_pending_create_locked(
    struct fb_gpu_dxg_present_source_entry *source,
    const struct fb_dxg_display_bind_request *bind)
{
    uint64 pending_id;

    if (source == NULL || bind == NULL)
        return 0;
    if (source->display_bind_pending_active &&
        fb_state.stats.dxg_display_bind_pending_active != 0) {
        fb_state.stats.dxg_display_bind_pending_cancelled++;
        fb_state.stats.dxg_display_bind_pending_active--;
    }
    pending_id = ++fb_state.stats.dxg_display_bind_pending_sequence;
    if (pending_id == 0)
        pending_id = ++fb_state.stats.dxg_display_bind_pending_sequence;
    source->display_bind_pending_id = pending_id;
    source->display_bind_pending_source_generation = bind->source_generation;
    source->display_bind_pending_resource_generation =
        bind->resource_generation;
    source->display_bind_pending_status = EINPROGRESS;
    source->display_bind_pending_block_reason = bind->block_reason;
    source->display_bind_pending_active = 1;
    fb_state.stats.dxg_display_bind_pending_created++;
    fb_state.stats.dxg_display_bind_pending_active++;
    if (fb_state.stats.dxg_display_bind_pending_active >
        fb_state.stats.dxg_display_bind_pending_peak)
        fb_state.stats.dxg_display_bind_pending_peak =
            fb_state.stats.dxg_display_bind_pending_active;
    fb_state.stats.dxg_display_bind_pending_last_source_generation =
        bind->source_generation;
    fb_state.stats.dxg_display_bind_pending_last_resource_generation =
        bind->resource_generation;
    fb_state.stats.dxg_display_bind_pending_last_status = EINPROGRESS;
    fb_state.stats.dxg_display_bind_pending_last_block_reason =
        bind->block_reason;
    fb_state.stats.dxg_display_bind_pending_last_owner_generation =
        bind->pin_valid ? bind->pin.process_generation : 0;
    return pending_id;
}

static void
fb_dxg_present_pending_resolve_locked(
    struct fb_gpu_dxg_present_source_entry *source,
    uint64 pending_id, int completed, uint64 status, uint64 block_reason)
{
    if (source == NULL || pending_id == 0 ||
        !source->display_bind_pending_active ||
        source->display_bind_pending_id != pending_id)
        return;
    source->display_bind_pending_active = 0;
    source->display_bind_pending_status = status;
    source->display_bind_pending_block_reason = block_reason;
    if (fb_state.stats.dxg_display_bind_pending_active != 0)
        fb_state.stats.dxg_display_bind_pending_active--;
    if (completed)
        fb_state.stats.dxg_display_bind_pending_completed++;
    else if (status == EOPNOTSUPP)
        fb_state.stats.dxg_display_bind_pending_failclosed++;
    else
        fb_state.stats.dxg_display_bind_pending_cancelled++;
    fb_state.stats.dxg_display_bind_pending_last_status = status;
    fb_state.stats.dxg_display_bind_pending_last_block_reason =
        block_reason;
    fb_state.stats.dxg_display_bind_pending_last_source_generation =
        source->display_bind_pending_source_generation;
    fb_state.stats.dxg_display_bind_pending_last_resource_generation =
        source->display_bind_pending_resource_generation;
}

static void
fb_dxg_present_pending_cancel_release_locked(
    struct fb_gpu_dxg_present_source_entry *source, uint64 reason)
{
    if (source == NULL || !source->display_bind_pending_active)
        return;
    fb_dxg_present_pending_resolve_locked(source,
                                         source->display_bind_pending_id,
                                         0, ESTALE, reason);
}

static int
fb_dxg_present_scanout_bind_locked(
    struct fb_gpu_dxg_present_source_entry **sourcep,
    struct fb_gpu_dxg_present_source_commit *req)
{
    struct fb_gpu_dxg_present_source_entry *source =
        sourcep != NULL ? *sourcep : NULL;
    struct fb_dxg_display_bind_request bind;
    struct fb_dxg_display_bind_result result;
    uint64 weak_evidence = 0;
    uint64 pending_id = 0;
    uint32 source_provenance = source != NULL ? source->provenance_flags : 0;
    uint32 source_adapter_identity =
        source != NULL ? source->adapter_identity :
        FB_GPU_DXG_PRESENT_ADAPTER_UNKNOWN;
    int source_index = -1;
    int submit_ret;
    int accept;

    memset(&bind, 0, sizeof(bind));
    memset(&result, 0, sizeof(result));
    if (source != NULL) {
        source_index = (int)(source - fb_state.dxg_present_sources);
        bind.present_source = source->handle;
        bind.source_generation = source->generation;
        bind.resource_generation =
            fb_dxg_present_source_resource_generation_locked(source);
        bind.dxg_fd = source->dxg_fd;
        bind.resource_fd = source->resource_fd;
        bind.device = source->device;
        bind.resource = source->resource;
        bind.allocation = source->allocation;
        bind.allocation_count = source->allocation_count;
        bind.width = source->width;
        bind.height = source->height;
        bind.pitch = source->pitch;
        bind.format = source->format;
        bind.modifier = source->modifier;
        bind.adapter_luid_low = source->adapter_luid_low;
        bind.adapter_luid_high = source->adapter_luid_high;
        bind.adapter_identity = source->adapter_identity;
        bind.provenance_flags = source->provenance_flags;
    }
    if (req != NULL) {
        bind.flags = req->flags;
        bind.sync_object = req->sync_object;
        bind.fence_value = req->fence_value;
    }
    bind.required_metadata =
        fb_state.stats.dxg_present_helper_required_metadata;
    bind.lifetime = fb_state.stats.dxg_present_helper_lifetime;
    bind.block_reason = fb_state.stats.dxg_present_helper_block_reason;

    fb_state.stats.dxg_scanout_bind_attempts++;
    fb_dxg_present_note_wsl_candidate_namespace_locked();
    fb_state.stats.dxg_scanout_bind_candidate_sender_contracts = 0;
    fb_state.stats.dxg_scanout_bind_candidate_completion_contracts = 0;
    fb_state.stats.dxg_scanout_bind_last_transport =
        fb_state.stats.dxg_present_helper_transport;
    fb_state.stats.dxg_scanout_bind_last_status = EOPNOTSUPP;
    fb_state.stats.dxg_scanout_bind_last_present_id = 0;
    fb_state.stats.dxg_scanout_bind_last_completed = 0;
    fb_state.stats.dxg_scanout_bind_last_source_generation =
        bind.source_generation;
    fb_state.stats.dxg_scanout_bind_last_resource_generation =
        bind.resource_generation;
    fb_state.stats.dxg_scanout_bind_last_dirty_sequence = 0;
    fb_state.stats.dxg_scanout_bind_last_dirty_rects = 0;

    if (source != NULL && source->display_bind_pin_active) {
        bind.pin_valid = 1;
        bind.pin = source->display_bind_pin;
        fb_state.stats.dxg_display_bind_pinned_dxg_file =
            source->display_bind_pin.dxg_file_pinned;
        fb_state.stats.dxg_display_bind_pinned_resource_file =
            source->display_bind_pin.resource_file_pinned;
        fb_state.stats.dxg_display_bind_pinned_resource_generation =
            source->display_bind_pin.generation;
        fb_state.stats.dxg_display_bind_pinned_process_generation =
            source->display_bind_pin.process_generation;
        fb_state.stats.dxg_display_bind_pinned_process_refs =
            source->display_bind_pin.process_refs;
        fb_state.stats.dxg_display_bind_pinned_shared_parent =
            source->display_bind_pin.shared_parent_id;
        fb_state.stats.dxg_display_bind_pinned_parent_refs =
            source->display_bind_pin.shared_parent_refs;
        fb_state.stats.dxg_display_bind_pinned_parent_children =
            source->display_bind_pin.shared_parent_children;
    }
    fb_dxg_present_sync_display_bind_state_locked(source);
    pending_id = fb_dxg_present_pending_create_locked(source, &bind);
    fb_state.stats.dxg_display_bind_provider_submits++;
    fb_state.stats.dxg_display_bind_lock_dropped_submits++;
    spin_unlock(&fb_state.lock);
    submit_ret = fb_dxg_present_provider_submit_display_bind(&bind, &result);
    spin_lock(&fb_state.lock);

    fb_state.stats.dxg_display_bind_revalidate_attempts++;
    source = NULL;
    if (source_index >= 0 &&
        source_index < FB_GPU_MAX_DXG_PRESENT_SOURCES) {
        struct fb_gpu_dxg_present_source_entry *candidate =
            &fb_state.dxg_present_sources[source_index];

        if (candidate->in_use &&
            candidate->handle == bind.present_source &&
            candidate->generation == bind.source_generation &&
            fb_dxg_present_source_resource_generation_locked(candidate) ==
                bind.resource_generation)
            source = candidate;
    }
    if (sourcep != NULL)
        *sourcep = source;
    if (source == NULL) {
        fb_state.stats.dxg_display_bind_revalidate_failures++;
        fb_state.stats.dxg_display_bind_stale_after_release_rejects++;
        fb_state.stats.dxg_display_bind_stale_generation_rejects++;
        if (result.present_id != 0 || result.completed_id != 0 ||
            result.resolved_or_cancelled != 0) {
            fb_state.stats.dxg_display_bind_stale_completion_rejects++;
            fb_state.stats.dxg_display_bind_late_completion_after_release++;
        }
        result.status = ESTALE;
        result.transport = FB_GPU_DXG_PRESENT_GPUP_DDA_TRANSPORT_NONE;
        result.operation = FB_GPU_DXG_PRESENT_GPUP_DDA_OP_SCANOUT_BIND;
        result.completion_source = FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY;
        result.present_id = 0;
        result.completed_id = 0;
        result.block_reason |= FB_GPU_DXG_PRESENT_BLOCK_NO_REGISTERED_SOURCE |
                               FB_GPU_DXG_PRESENT_BLOCK_NO_COMPLETION;
        fb_state.stats.dxg_display_bind_pending_last_status = ESTALE;
        fb_state.stats.dxg_display_bind_pending_last_block_reason =
            result.block_reason;
        fb_state.stats.dxg_display_bind_pending_last_owner_generation =
            result.pending_owner_generation;
        fb_state.stats.dxg_display_bind_pending_last_source_generation =
            bind.source_generation;
        fb_state.stats.dxg_display_bind_pending_last_resource_generation =
            bind.resource_generation;
    } else {
        fb_state.stats.dxg_display_bind_revalidate_successes++;
        source_adapter_identity = source->adapter_identity;
    }
    fb_state.stats.dxg_scanout_bind_last_transport = result.transport;
    fb_state.stats.dxg_scanout_bind_last_status = (uint64)result.status;
    fb_state.stats.dxg_scanout_bind_last_present_id = result.present_id;
    fb_state.stats.dxg_scanout_bind_last_completed = result.completed_id;
    fb_state.stats.dxg_present_helper_block_reason = result.block_reason;
    fb_state.stats.dxg_display_bind_provider_pin_revalidated =
        result.pin_revalidated;
    fb_state.stats.dxg_display_bind_provider_no_host_abi =
        result.no_host_abi;
    fb_state.stats.dxg_display_bind_provider_no_sender =
        result.no_sender;
    fb_state.stats.dxg_display_bind_provider_no_completion =
        result.no_completion;
    fb_state.stats.dxg_display_bind_provider_publication_attempts +=
        result.publication_attempted != 0;
    fb_state.stats.dxg_display_bind_provider_publish_before_send =
        result.publish_before_send;
    fb_state.stats.dxg_display_bind_provider_transport_pending_id =
        result.transport_pending_id;
    fb_state.stats.dxg_display_bind_provider_command_id =
        result.command_id;
    fb_state.stats.dxg_display_bind_provider_transaction_id =
        result.transaction_id;
    fb_state.stats.dxg_display_bind_provider_channel =
        result.channel;
    fb_state.stats.dxg_display_bind_provider_completion_demux_registered =
        result.completion_demux_registered;
    fb_state.stats.dxg_display_bind_transport_source =
        result.transport_source;
    fb_state.stats.dxg_display_bind_host_saw_packet =
        result.host_saw_packet;
    fb_state.stats.dxg_display_bind_wsl_presenthistory_completion_credit =
        result.wsl_presenthistory_completion_credit;
    fb_state.stats.dxg_display_bind_provider_resolved_or_cancelled =
        result.resolved_or_cancelled;
    fb_state.stats.dxg_display_bind_provider_refs_released =
        result.refs_released;
    fb_state.stats.dxg_display_bind_provider_no_host_abi_cancelled =
        result.no_host_abi_cancelled;
    fb_state.stats.dxg_display_bind_provider_no_host_abi_refs_released =
        result.no_host_abi_refs_released;
    fb_state.stats.dxg_display_bind_provider_pending_owner_generation =
        result.pending_owner_generation;
    fb_state.stats.dxg_display_bind_provider_pending_source_generation =
        result.pending_source_generation;
    fb_state.stats.dxg_display_bind_provider_pending_resource_generation =
        result.pending_resource_generation;
    fb_state.stats.dxg_display_bind_provider_pending_dxgprocess_generation =
        result.pending_dxgprocess_generation;
    fb_state.stats.dxg_display_bind_provider_pending_process_adapter_generation =
        result.pending_process_adapter_generation;
    fb_state.stats.dxg_display_bind_provider_pending_hmgr_index_unique_valid =
        result.pending_hmgr_index_unique_valid;
    fb_state.stats.dxg_display_bind_provider_pending_parent_resource_ref_held =
        result.pending_parent_resource_ref_held;
    fb_state.stats.dxg_display_bind_provider_pending_opened_child_ref_held =
        result.pending_opened_child_ref_held;
    fb_state.stats.dxg_display_bind_provider_pending_syncobject_ref_held =
        result.pending_syncobject_ref_held;
    fb_state.stats.dxg_display_bind_provider_pending_owner_close_cancelled =
        result.pending_owner_close_cancelled;
    if (result.request_metadata_complete)
        fb_state.stats.dxg_display_bind_request_metadata_complete = 1;
    if (result.request_sync_metadata_complete)
        fb_state.stats.dxg_display_bind_request_sync_metadata_complete = 1;
    if (result.request_missing_metadata == 0 ||
        fb_state.stats.dxg_display_bind_request_metadata_complete == 0 ||
        fb_state.stats.dxg_display_bind_request_sync_metadata_complete == 0)
        fb_state.stats.dxg_display_bind_request_missing_metadata =
            result.request_missing_metadata;
    fb_state.stats.dxg_scanout_bind_candidate_sender_contracts =
        result.sender_present != 0;
    fb_state.stats.dxg_scanout_bind_candidate_completion_contracts =
        result.completion_present != 0;

    accept = source != NULL && req != NULL &&
             fb_dxg_present_display_bind_result_accepts(
                 &bind, &result, submit_ret);

    if (req == NULL || source == NULL || !accept)
        weak_evidence = 1;
    if (!accept &&
        (fb_state.stats.dxg_present_helper_transport_present == 0 ||
        fb_state.stats.dxg_present_display_target_kind ==
            FB_GPU_DXG_DISPLAY_TARGET_NONE ||
        fb_state.stats.dxg_present_missing_host_abi ==
            FB_GPU_DXG_PRESENT_MISSING_SCANOUT_BIND ||
        result.present_id == 0 ||
        result.completed_id == 0))
        weak_evidence = 1;
    if (weak_evidence) {
        fb_state.stats.dxg_scanout_bind_weak_evidence_rejects++;
        fb_state.stats.dxg_scanout_bind_candidate_rejects++;
    }
    if ((fb_state.stats.dxg_present_host_candidates &
         FB_GPU_DXG_PRESENT_HOST_DXG) != 0 &&
        fb_state.stats.dxg_present_helper_transport_present == 0)
        fb_state.stats.dxg_scanout_bind_weak_dxg_ready_only++;
    if ((source_provenance &
         FB_GPU_DXG_PRESENT_PROV_D3DKMT_HANDLES) != 0)
        fb_state.stats.dxg_scanout_bind_weak_d3dkmt_handles_only++;
    if ((source_provenance &
         FB_GPU_DXG_PRESENT_PROV_RESOURCE_FD) != 0 &&
        source_adapter_identity == FB_GPU_DXG_PRESENT_ADAPTER_MATCH)
        fb_state.stats.dxg_scanout_bind_weak_same_adapter_resource_only++;
    if (req != NULL &&
        (req->flags & FB_GPU_DXG_PRESENT_F_WAIT_SYNC) != 0 &&
        req->sync_object != 0)
        fb_state.stats.dxg_scanout_bind_weak_syncfile_only++;
    if ((fb_state.stats.dxg_present_host_rejects &
         FB_GPU_DXG_PRESENT_REJECT_SYNTHVID_GPA_ONLY) != 0)
        fb_state.stats.dxg_scanout_bind_weak_synthvid_gpa_dirty_only++;
    if (fb_state.stats.dxg_present_display_target_kind ==
        FB_GPU_DXG_DISPLAY_TARGET_NONE)
        fb_state.stats.dxg_scanout_bind_weak_software_or_readback_path++;
    if (source != NULL) {
        source->display_bind_attempts++;
        source->display_bind_present_id = result.present_id;
        source->display_bind_completed_id = result.completed_id;
        source->display_bind_source_generation =
            bind.source_generation;
        source->display_bind_resource_generation =
            bind.resource_generation;
        source->display_bind_status = (uint64)result.status;
        source->display_bind_block_reason = result.block_reason;
        source->display_bind_completion_source =
            result.completion_source;
    }
    if (accept) {
        fb_dxg_present_pending_resolve_locked(source, pending_id, 1, 0, 0);
        req->present_id = result.present_id;
        req->completed = result.completed_id;
        fb_state.stats.dxg_present_display_target_kind =
            FB_GPU_DXG_DISPLAY_TARGET_RUNTIME_D3D12_RESOURCE;
        fb_state.stats.dxg_present_requires_host_protocol = 0;
        fb_state.stats.dxg_present_missing_host_abi =
            FB_GPU_DXG_PRESENT_MISSING_NONE;
        fb_state.stats.dxg_present_helper_transport = result.transport;
        fb_state.stats.dxg_present_helper_transport_present = 1;
        fb_state.stats.dxg_present_helper_operation = result.operation;
        fb_state.stats.dxg_present_helper_block_reason = 0;
        fb_state.stats.dxg_scanout_bind_successes++;
        fb_state.stats.dxg_scanout_bind_completion_successes++;
        fb_dxg_present_sync_display_bind_state_locked(source);
        return 0;
    }
    fb_dxg_present_pending_resolve_locked(
        source, pending_id, 0,
        result.status < 0 ? (uint64)(-result.status) :
            (uint64)result.status,
        result.block_reason);
    fb_state.stats.dxg_scanout_bind_rejects++;
    fb_dxg_present_sync_display_bind_state_locked(source);
    return -EOPNOTSUPP;
}

static int fb_dxg_present_register(uint64 owner_id, pid_t owner_tgid,
                                   struct fb_gpu_dxg_present_source_register *req)
{
    struct fb_gpu_dxg_present_source_entry *source = NULL;
    struct hyperv_dxg_shared_resource_snapshot resource_snapshot;
    struct hyperv_dxg_display_bind_pin_snapshot pin_snapshot;
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
    int resource_snapshot_valid = 0;
    int register_shape_valid = 0;
    int display_bind_pin_required = 0;
    int pin_ret = -EINVAL;
    int pin_owned = 0;
    int ret = 0;

    if (req == NULL)
        return -EINVAL;
    fb_dxg_present_sanitize_register_tail(req);
    memset(&resource_snapshot, 0, sizeof(resource_snapshot));
    memset(&pin_snapshot, 0, sizeof(pin_snapshot));
    if (req->resource_fd >= 0) {
        ret = hyperv_dxg_shared_resource_snapshot_from_opened_resource(
            req->dxg_fd, req->resource_fd, req->device, req->resource,
            req->allocation, req->allocation_count, &resource_snapshot);
        if (ret != 0)
            ret = hyperv_dxg_shared_resource_snapshot_from_fd(
                req->resource_fd, &resource_snapshot);
        if (ret == 0 &&
            resource_snapshot.device == req->device &&
            resource_snapshot.resource == req->resource &&
            resource_snapshot.allocation_count == req->allocation_count &&
            resource_snapshot.first_allocation == req->allocation)
            resource_snapshot_valid = 1;
        else
            ret = -EINVAL;
    }
    if (ret == 0 && req->flags == 0 && req->dxg_fd >= 0 &&
        req->resource_fd >= -1 && req->device != 0 && req->resource != 0 &&
        req->allocation != 0 && req->allocation_count != 0 &&
        req->allocation_count <= 1024 && req->width != 0 &&
        req->height != 0 && req->width <= 16384 && req->height <= 16384 &&
        req->width <= 0xffffffffU / 4U && req->format != 0 &&
        req->pitch != 0 && req->pitch >= req->width * 4U)
        register_shape_valid = 1;
    display_bind_pin_required = register_shape_valid && req->resource_fd >= 0;
    if (display_bind_pin_required) {
        pin_ret = hyperv_dxg_display_bind_pin_from_fds(
            req->dxg_fd, req->resource_fd, req->device, req->resource,
            req->allocation, req->allocation_count, &pin_snapshot);
        if (pin_ret == 0)
            pin_owned = 1;
    }
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
    fb_dxg_present_sync_display_bind_state_locked(NULL);

    if (!register_shape_valid || (display_bind_pin_required && pin_ret != 0) ||
        req->flags != 0 || req->dxg_fd < 0 || req->resource_fd < -1 ||
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
    if (display_bind_pin_required) {
        fb_state.stats.dxg_display_bind_pin_attempts++;
        fb_state.stats.dxg_display_bind_pin_successes++;
        fb_state.stats.dxg_display_bind_pinned_dxg_file =
            pin_snapshot.dxg_file_pinned;
        fb_state.stats.dxg_display_bind_pinned_resource_file =
            pin_snapshot.resource_file_pinned;
        fb_state.stats.dxg_display_bind_pinned_resource_generation =
            pin_snapshot.generation;
        fb_state.stats.dxg_display_bind_pinned_process_generation =
            pin_snapshot.process_generation;
        fb_state.stats.dxg_display_bind_pinned_process_refs =
            pin_snapshot.process_refs;
        fb_state.stats.dxg_display_bind_pinned_shared_parent =
            pin_snapshot.shared_parent_id;
        fb_state.stats.dxg_display_bind_pinned_parent_refs =
            pin_snapshot.shared_parent_refs;
        fb_state.stats.dxg_display_bind_pinned_parent_children =
            pin_snapshot.shared_parent_children;
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
    if (display_bind_pin_required) {
        source->display_bind_pin = pin_snapshot;
        source->display_bind_pin_active = 1;
        pin_owned = 0;
    }
    if (resource_snapshot_valid) {
        source->resource_fd_kind = resource_snapshot.kind;
        source->resource_fd_sealed = resource_snapshot.sealed;
        source->resource_fd_shared_records_valid =
            resource_snapshot.shared_records_valid;
        source->resource_fd_matches_handles = 1;
        source->resource_fd_generation = resource_snapshot.generation;
    }
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
    fb_dxg_present_sync_display_bind_state_locked(source);

out:
    if (ret != 0) {
        if (display_bind_pin_required) {
            fb_state.stats.dxg_display_bind_pin_attempts++;
            fb_state.stats.dxg_display_bind_pin_failures++;
        }
        fb_state.stats.dxg_present_register_rejects++;
        fb_state.stats.dxg_present_last_ret = (uint64)(-ret);
        print_rejects = fb_state.stats.dxg_present_register_rejects;
    }
    spin_unlock(&fb_state.lock);
    if (pin_owned)
        hyperv_dxg_display_bind_unpin(&pin_snapshot);
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
    struct hyperv_dxg_display_bind_pin_snapshot pin_snapshot;
    int pin_active = 0;

    memset(&pin_snapshot, 0, sizeof(pin_snapshot));
    spin_lock(&fb_state.lock);
    struct fb_gpu_dxg_present_source_entry *source =
        fb_dxg_present_source_lookup_locked(handle);
    if (source != NULL) {
        fb_dxg_present_pending_cancel_release_locked(
            source, FB_GPU_DXG_PRESENT_BLOCK_NO_REGISTERED_SOURCE);
        fb_dxg_present_clear_display_bind_release_locked(source);
        if (source->display_bind_pin_active) {
            pin_snapshot = source->display_bind_pin;
            source->display_bind_pin_active = 0;
            fb_state.stats.dxg_display_bind_unpins++;
            pin_active = 1;
        }
        memset(source, 0, sizeof(*source));
    }
    spin_unlock(&fb_state.lock);
    if (pin_active)
        hyperv_dxg_display_bind_unpin(&pin_snapshot);
}

static void fb_dxg_present_release_owner_sources(uint64 owner_id,
                                                 pid_t owner_tgid)
{
    struct hyperv_dxg_display_bind_pin_snapshot pins[FB_GPU_MAX_DXG_PRESENT_SOURCES];
    int pin_count = 0;

    memset(pins, 0, sizeof(pins));
    spin_lock(&fb_state.lock);
    for (int i = 0; i < FB_GPU_MAX_DXG_PRESENT_SOURCES; i++) {
        struct fb_gpu_dxg_present_source_entry *source =
            &fb_state.dxg_present_sources[i];

        if (!source->in_use)
            continue;
        if (!fb_dxg_present_source_owner_matches(source, owner_id,
                                                owner_tgid))
            continue;
        if (source->display_bind_pin_active &&
            pin_count < FB_GPU_MAX_DXG_PRESENT_SOURCES) {
            pins[pin_count++] = source->display_bind_pin;
            source->display_bind_pin_active = 0;
            fb_state.stats.dxg_display_bind_unpins++;
        }
        fb_dxg_present_pending_cancel_release_locked(
            source, FB_GPU_DXG_PRESENT_BLOCK_NO_REGISTERED_SOURCE);
        fb_dxg_present_clear_display_bind_release_locked(source);
        memset(source, 0, sizeof(*source));
        fb_state.stats.dxg_present_release_sources++;
    }
    spin_unlock(&fb_state.lock);
    for (int i = 0; i < pin_count; i++)
        hyperv_dxg_display_bind_unpin(&pins[i]);
}

static int fb_dxg_present_commit(uint64 owner_id, pid_t owner_tgid,
                                 struct fb_gpu_dxg_present_source_commit *req)
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
    uint64 print_candidate_reject_reasons = 0;
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
    fb_dxg_present_sync_display_bind_state_locked(NULL);

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
    fb_dxg_present_sync_display_bind_state_locked(source);

    if ((req->flags & ~FB_GPU_DXG_PRESENT_F_WAIT_SYNC) != 0 ||
        ((req->flags & FB_GPU_DXG_PRESENT_F_WAIT_SYNC) != 0 &&
         req->sync_object == 0) ||
        ((req->flags & FB_GPU_DXG_PRESENT_F_WAIT_SYNC) == 0 &&
         req->sync_object != 0)) {
        fb_state.stats.dxg_present_commit_bad_flags++;
        ret = -EINVAL;
        goto out;
    }

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
    ret = fb_dxg_present_scanout_bind_locked(&source, req);
    if (ret != 0) {
        fb_state.stats.dxg_present_host_handoff_missing++;
        if (fb_state.stats.dxg_present_helper_transport_present == 0)
            fb_state.stats.dxg_present_commit_no_transport++;
        if (fb_state.stats.dxg_present_helper_requires_completion != 0)
            fb_state.stats.dxg_present_commit_no_completion++;
    }
    if (ret != 0 && source != NULL &&
        fb_state.stats.dxg_present_host_handoff_missing <= 8) {
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
        print_candidate_reject_reasons =
            fb_state.stats.dxg_scanout_bind_candidate_reject_reasons;
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
    if (ret != 0)
        fb_state.stats.dxg_present_commit_rejects++;
    fb_state.stats.dxg_present_last_ret = ret < 0 ? (uint64)(-ret) : 0;
    fb_state.stats.dxg_scanout_bind_last_status =
        ret < 0 ? (uint64)(-ret) : 0;
    fb_dxg_present_sync_display_bind_state_locked(source);
    print_commit_rejects = fb_state.stats.dxg_present_commit_rejects;
    print_helper_block = fb_state.stats.dxg_present_helper_block_reason;
    print_no_source = fb_state.stats.dxg_present_commit_no_source;
    print_bad_flags = fb_state.stats.dxg_present_commit_bad_flags;
    spin_unlock(&fb_state.lock);
    fb_dxg_present_hex32(print_luid_high_hex,
                         (uint32)(print_luid_high & 0xffffffffULL));
    fb_dxg_present_hex32(print_luid_low_hex,
                         (uint32)(print_luid_low & 0xffffffffULL));
    if (ret != 0 && !print_missing && print_commit_rejects <= 8) {
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
               "adapter_mismatch=%u,no_completion=%u,dda_no_import_path=%u,"
               "wsl_enum_only=%u "
               "counters:no_source=%lu,bad_flags=%lu,no_transport=%lu,"
               "no_completion=%lu,resource_fd_unverified=%lu,"
               "adapter_mismatch=%lu "
               "dxg_fd:0x%lx resource_fd:0x%lx luid:%s:%s "
               "adapter_identity:%u "
               "synthvid:gpa-dirty-only dxg_display_bind:0 dda_nouveau:%u "
               "candidate_cmds:presenthistory=%u,redirected_flip_fence=%u,"
               "blt=%u,propagate_presenthistory=%u "
               "candidate_contracts:linux_ioctl=0,resource_bind=0,"
               "display_completion=0,reject_reasons=0x%lx "
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
               (print_helper_block &
                FB_GPU_DXG_PRESENT_BLOCK_WSL_ENUM_ONLY) != 0,
               print_no_source, print_bad_flags, print_no_transport,
               print_no_completion, print_resource_fd_unverified,
               print_adapter_mismatch,
               print_dxg_fd, print_resource_fd, print_luid_high_hex,
               print_luid_low_hex, print_adapter_identity,
               (print_candidates & FB_GPU_DXG_PRESENT_HOST_DDA_NOUVEAU) != 0,
               FB_GPU_DXG_WSL_PRESENTHISTORYTOKEN_CMD,
               FB_GPU_DXG_WSL_SETREDIRECTEDFLIPFENCEVALUE_CMD,
               FB_GPU_DXG_WSL_BLT_CMD,
               FB_GPU_DXG_WSL_PROPAGATE_PRESENTHISTORYTOKEN_CMD,
               print_candidate_reject_reasons);
    }
    return ret;
}

static int fb_dxg_present_query(uint64 owner_id, pid_t owner_tgid,
                                struct fb_gpu_dxg_present_source_query *req)
{
    struct fb_gpu_dxg_present_source_entry *source;
    uint32 source_handle;
    uint32 provider_submitted;
    uint32 provider_no_host_abi;
    uint32 provider_no_sender;
    uint32 provider_no_completion;
    uint64 helper_meta;
    uint64 helper_lifetime;

    if (req == NULL)
        return -EINVAL;

    spin_lock(&fb_state.lock);
    fb_state.stats.dxg_present_query_attempts++;
    fb_state.stats.dxg_present_query_rejects++;
    fb_state.stats.dxg_present_last_ret = EOPNOTSUPP;
    fb_state.stats.dxg_scanout_bind_completion_queries++;
    fb_state.stats.dxg_scanout_bind_completion_pending++;
    fb_state.stats.dxg_scanout_bind_last_status = EOPNOTSUPP;
    fb_state.stats.dxg_scanout_bind_last_present_id = 0;
    fb_state.stats.dxg_scanout_bind_last_completed = 0;

    source_handle = req->present_source != 0 ?
        req->present_source : (uint32)fb_state.stats.dxg_present_last_source;
    source = fb_dxg_present_source_lookup_locked(source_handle);
    if (source != NULL &&
        !fb_dxg_present_source_owner_matches(source, owner_id, owner_tgid))
        source = NULL;
    if (source_handle != 0 && source == NULL) {
        fb_state.stats.dxg_display_bind_after_close_queries++;
        fb_state.stats.dxg_display_bind_stale_source_rejects++;
        fb_state.stats.dxg_display_bind_stale_generation_rejects++;
        fb_state.stats.dxg_display_bind_stale_completion_rejects++;
        fb_state.stats.dxg_display_bind_stale_after_release_rejects++;
        if (fb_state.stats.dxg_display_bind_present_id != 0 ||
            fb_state.stats.dxg_display_bind_completed_id != 0 ||
            fb_state.stats.dxg_scanout_bind_last_present_id != 0 ||
            fb_state.stats.dxg_scanout_bind_last_completed != 0) {
            fb_state.stats.dxg_display_bind_after_close_nonzero_id_rejects++;
            fb_state.stats.dxg_display_bind_late_completion_after_release++;
        }
    }

    if (source != NULL) {
        fb_state.stats.dxg_present_last_source = source->handle;
        fb_state.stats.dxg_present_last_device = source->device;
        fb_state.stats.dxg_present_last_resource = source->resource;
        fb_state.stats.dxg_present_last_allocation = source->allocation;
        fb_dxg_present_note_source_shape_locked(source);
        fb_state.stats.dxg_scanout_bind_last_source_generation =
            source->generation;
        fb_state.stats.dxg_scanout_bind_last_resource_generation =
            fb_dxg_present_source_resource_generation_locked(source);
    }
    fb_dxg_present_note_transport_contract_locked(
        source, (uint32)fb_state.stats.dxg_present_last_flags);
    fb_dxg_present_sync_display_bind_state_locked(source);
    provider_submitted =
        fb_state.stats.dxg_display_bind_provider_submits != 0;
    provider_no_host_abi = provider_submitted ?
        (uint32)fb_state.stats.dxg_display_bind_provider_no_host_abi : 1;
    provider_no_sender = provider_submitted ?
        (uint32)fb_state.stats.dxg_display_bind_provider_no_sender : 1;
    provider_no_completion = provider_submitted ?
        (uint32)fb_state.stats.dxg_display_bind_provider_no_completion : 1;
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
        req->resource_fd_kind = source->resource_fd_kind;
        req->resource_fd_sealed = source->resource_fd_sealed;
        req->resource_fd_matches_handles =
            source->resource_fd_matches_handles;
        req->resource_fd_shared_records_valid =
            source->resource_fd_shared_records_valid;
        req->resource_fd_generation = source->resource_fd_generation;
        req->source_generation = source->generation;
        req->resource_generation =
            fb_dxg_present_source_resource_generation_locked(source);
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
        req->source_generation =
            fb_state.stats.dxg_scanout_bind_last_source_generation;
        req->resource_generation =
            fb_state.stats.dxg_scanout_bind_last_resource_generation;
    }
    req->selected_lane = (uint32)fb_state.stats.dxg_present_selected_lane;
    req->helper_block_reason =
        (uint32)fb_state.stats.dxg_present_helper_block_reason;
    req->host_candidates = fb_state.stats.dxg_present_host_candidates;
    req->host_rejects = fb_state.stats.dxg_present_host_rejects;
    req->display_bind_status = fb_state.stats.dxg_display_bind_status;
    req->display_bind_block_reason =
        fb_state.stats.dxg_display_bind_block_reason;
    req->display_bind_completion_source =
        fb_state.stats.dxg_display_bind_completion_source;
    req->display_bind_dirty_sequence =
        fb_state.stats.dxg_scanout_bind_last_dirty_sequence;
    req->display_bind_dirty_rects =
        fb_state.stats.dxg_scanout_bind_last_dirty_rects;
    req->display_bind_host_abi_present =
        provider_submitted && provider_no_host_abi == 0;
    req->display_bind_sender_present =
        provider_submitted && provider_no_sender == 0;
    req->display_bind_completion_present =
        provider_submitted && provider_no_completion == 0;
    req->display_bind_provider_no_host_abi = provider_no_host_abi;
    req->display_bind_provider_no_sender = provider_no_sender;
    req->display_bind_provider_no_completion = provider_no_completion;
    req->display_bind_provider_pin_revalidated =
        (uint32)fb_state.stats.dxg_display_bind_provider_pin_revalidated;
    req->display_bind_transport_source =
        (uint32)fb_state.stats.dxg_display_bind_transport_source;
    req->display_bind_host_saw_packet =
        (uint32)fb_state.stats.dxg_display_bind_host_saw_packet;
    req->display_bind_wsl_presenthistory_completion_credit =
        (uint32)fb_state.stats.dxg_display_bind_wsl_presenthistory_completion_credit;
    req->reserved2 = 0;
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
    uint32 provider_submitted;
    uint32 provider_no_host_abi;
    uint32 provider_no_sender;
    uint32 provider_no_completion;
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
            req->resource_fd_kind = source->resource_fd_kind;
            req->resource_fd_sealed = source->resource_fd_sealed;
            req->resource_fd_matches_handles =
                source->resource_fd_matches_handles;
            req->resource_fd_shared_records_valid =
                source->resource_fd_shared_records_valid;
            req->resource_fd_generation = source->resource_fd_generation;
        }
    }
    if (req->present_source != 0 && source == NULL) {
        fb_state.stats.dxg_display_bind_after_close_queries++;
        fb_state.stats.dxg_display_bind_stale_source_rejects++;
        fb_state.stats.dxg_display_bind_stale_generation_rejects++;
        fb_state.stats.dxg_display_bind_stale_completion_rejects++;
        fb_state.stats.dxg_display_bind_stale_after_release_rejects++;
        if (fb_state.stats.dxg_display_bind_present_id != 0 ||
            fb_state.stats.dxg_display_bind_completed_id != 0 ||
            fb_state.stats.dxg_scanout_bind_last_present_id != 0 ||
            fb_state.stats.dxg_scanout_bind_last_completed != 0) {
            fb_state.stats.dxg_display_bind_after_close_nonzero_id_rejects++;
            fb_state.stats.dxg_display_bind_late_completion_after_release++;
        }
    }
    flags = req->flags;
    adapter_identity = fb_dxg_present_adapter_identity(
        req->adapter_luid_low, req->adapter_luid_high);
    provenance = fb_dxg_present_bind_contract_provenance(req);
    resource_generation = req->resource_fd_generation != 0 ?
        req->resource_fd_generation :
        fb_dxg_present_resource_generation(
            req->device, req->resource, req->allocation,
            req->allocation_count);

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
    fb_state.stats.dxg_scanout_bind_last_source_generation =
        source_generation;
    fb_state.stats.dxg_scanout_bind_last_resource_generation =
        resource_generation;
    fb_state.stats.dxg_scanout_bind_last_status = EOPNOTSUPP;
    fb_state.stats.dxg_scanout_bind_last_present_id = 0;
    fb_state.stats.dxg_scanout_bind_last_completed = 0;
    fb_dxg_present_note_transport_contract_locked(NULL, flags);
    fb_dxg_present_note_host_lanes_locked();
    if (req->present_source != 0 && !source_live)
        fb_state.stats.dxg_present_helper_block_reason |=
            FB_GPU_DXG_PRESENT_BLOCK_NO_REGISTERED_SOURCE;
    if (req->resource_fd < 0)
        fb_state.stats.dxg_present_helper_block_reason |=
            FB_GPU_DXG_PRESENT_BLOCK_RESOURCE_FD_UNVERIFIED;
    if (req->resource_fd >= 0 &&
        (req->resource_fd_kind != 2 || req->resource_fd_sealed == 0 ||
         req->resource_fd_matches_handles == 0 ||
         req->resource_fd_shared_records_valid == 0))
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

    provider_submitted =
        fb_state.stats.dxg_display_bind_provider_submits != 0;
    provider_no_host_abi = provider_submitted ?
        (uint32)fb_state.stats.dxg_display_bind_provider_no_host_abi : 1;
    provider_no_sender = provider_submitted ?
        (uint32)fb_state.stats.dxg_display_bind_provider_no_sender : 1;
    provider_no_completion = provider_submitted ?
        (uint32)fb_state.stats.dxg_display_bind_provider_no_completion : 1;

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
    req->provider_status = ret < 0 ? (uint64)(-ret) : 0;
    req->provider_block_reason =
        fb_state.stats.dxg_present_helper_block_reason;
    req->dirty_sequence =
        fb_state.stats.dxg_scanout_bind_last_dirty_sequence;
    req->dirty_rects =
        fb_state.stats.dxg_scanout_bind_last_dirty_rects;
    req->host_abi_present =
        provider_submitted && provider_no_host_abi == 0;
    req->sender_present =
        provider_submitted && provider_no_sender == 0;
    req->completion_present =
        provider_submitted && provider_no_completion == 0;
    req->provider_no_host_abi = provider_no_host_abi;
    req->provider_no_sender = provider_no_sender;
    req->provider_no_completion = provider_no_completion;
    req->provider_pin_revalidated =
        (uint32)fb_state.stats.dxg_display_bind_provider_pin_revalidated;
    req->transport_source =
        (uint32)fb_state.stats.dxg_display_bind_transport_source;
    req->host_saw_packet =
        (uint32)fb_state.stats.dxg_display_bind_host_saw_packet;
    req->wsl_presenthistory_completion_credit =
        (uint32)fb_state.stats.dxg_display_bind_wsl_presenthistory_completion_credit;
    req->reserved3 = 0;
    spin_unlock(&fb_state.lock);

    return ret;
}
