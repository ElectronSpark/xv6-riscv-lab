        top_nr[1], count[1], avg_us[1], max_us[1], last_us[1],
        top_nr[2], count[2], avg_us[2], max_us[2], last_us[2],
        top_nr[3], count[3], avg_us[3], max_us[3], last_us[3]);
}

#define HV_DXG_IOCTL_H_ARGS(i) \
    hvdxg.ioctl_history_cmd[(i)], \
    hvdxg.ioctl_history_nr[(i)], \
    hvdxg.ioctl_history_ret[(i)]

static int hvdxg_open(cdev_t *cdev)
{
    (void)cdev;
    hvdxg.read_emitted = 0;
    hvdxg.read_offset = 0;
    hvdxg.open_count++;
    hvdxg.live_open_count++;
    return 0;
}

static int hvdxg_release(cdev_t *cdev)
{
    (void)cdev;
    if (hvdxg.live_open_count > 0)
        hvdxg.live_open_count--;
    return 0;
}

static const char *hvdxg_early_bind_source_name(uint32 source);

static const char *hvdxg_channel_name(uint32 channel)
{
    switch (channel) {
    case HV_DXG_CHANNEL_GLOBAL:
        return "global";
    case HV_DXG_CHANNEL_VGPU:
        return "vgpu";
    default:
        return "none";
    }
}

static int hvdxg_read_status(cdev_t *cdev, bool user, void *buf,
                             size_t count, size_t *read_offset,
                             int *read_emitted, char **read_status,
                             size_t *read_status_len)
{
    (void)cdev;
    char *status;
    size_t status_size = HV_DXG_STATUS_BUF_SIZE;
    int cached = 0;
    int len = 0;
    uint32 host_luid_equiv_basis;
    struct hvdxg_winluid user_luid;
    uint32 ioctl_top_nr[HV_DXG_IOCTL_TIME_TOP];
    uint32 existing_sysmem_diag_attempts;
    uint32 existing_sysmem_diag_path;
    uint32 existing_sysmem_diag_standard;
    uint32 existing_sysmem_diag_writable;
    uint32 existing_sysmem_diag_device;
    uint32 existing_sysmem_diag_allocation;
    uint32 existing_sysmem_diag_pages;
    int32 existing_sysmem_diag_pin_ret;
    int32 existing_sysmem_diag_set_ret;
    uint32 existing_sysmem_diag_pin_ok;
    uint32 existing_sysmem_diag_set_ok;
    uint64 existing_sysmem_diag_va;
    uint64 existing_sysmem_diag_size;
    uint64 existing_sysmem_diag_first_pfn;
    uint64 existing_sysmem_diag_last_pfn;
    uint64 existing_sysmem_diag_total_pages;

    if (hvdxg.vgpu_open_ok && hvdxg.probe_attempts == 0)
        (void)hvdxg_probe_transport();

    if (read_offset == NULL || read_emitted == NULL)
        return -EINVAL;
    if (read_status != NULL && read_status_len != NULL &&
        *read_status != NULL) {
        status = *read_status;
        cached = 1;
        goto copy_cached;
    }

    status = kvmalloc(status_size);
    if (status == NULL)
        return -ENOMEM;
    hvdxg_ioctl_timing_top(ioctl_top_nr);
    user_luid = hvdxg_user_adapter_luid(&host_luid_equiv_basis);
    existing_sysmem_diag_attempts = hvdxg.existing_sysmem_attempts;
    existing_sysmem_diag_path = hvdxg.existing_sysmem_last_path;
    existing_sysmem_diag_standard = hvdxg.existing_sysmem_last_standard;
    existing_sysmem_diag_writable = hvdxg.existing_sysmem_last_writable;
    existing_sysmem_diag_device = hvdxg.existing_sysmem_last_device;
    existing_sysmem_diag_allocation =
        hvdxg.existing_sysmem_last_allocation;
    existing_sysmem_diag_va = hvdxg.existing_sysmem_last_va;
    existing_sysmem_diag_size = hvdxg.existing_sysmem_last_size;
    existing_sysmem_diag_first_pfn =
        hvdxg.existing_sysmem_last_first_pfn;
    existing_sysmem_diag_last_pfn = hvdxg.existing_sysmem_last_last_pfn;
    existing_sysmem_diag_pages = hvdxg.existing_sysmem_last_pages;
    existing_sysmem_diag_pin_ret = hvdxg.existing_sysmem_last_pin_ret;
    existing_sysmem_diag_set_ret = hvdxg.existing_sysmem_last_set_ret;
    existing_sysmem_diag_pin_ok = hvdxg.existing_sysmem_pin_successes;
    existing_sysmem_diag_set_ok = hvdxg.existing_sysmem_set_successes;
    existing_sysmem_diag_total_pages = hvdxg.existing_sysmem_total_pages;
    if (existing_sysmem_diag_attempts == 0 &&
        (existing_sysmem_diag_pages != 0 ||
         existing_sysmem_diag_pin_ok != 0 ||
         existing_sysmem_diag_set_ok != 0)) {
        existing_sysmem_diag_attempts =
            existing_sysmem_diag_pin_ok != 0 ?
            existing_sysmem_diag_pin_ok : 1;
    }
    if (existing_sysmem_diag_path == 0 &&
        (hvdxg.allocation_last_sysmem != 0 ||
         existing_sysmem_diag_pages != 0))
        existing_sysmem_diag_path = 1;
    if (existing_sysmem_diag_device == 0)
        existing_sysmem_diag_device = hvdxg.allocation_last_device;
    if (existing_sysmem_diag_allocation == 0)
        existing_sysmem_diag_allocation = hvdxg.last_allocation_handle;
    if (existing_sysmem_diag_va == 0)
        existing_sysmem_diag_va = hvdxg.allocation_last_sysmem;
    if (existing_sysmem_diag_size == 0)
        existing_sysmem_diag_size = hvdxg.last_allocation_size;

    len = snprintf(status, status_size,
        "hyperv_dxg_global=%d relid=%u conn=%u monitor=%u\n"
        "hyperv_dxg_global_transport=gpadl:%d status:%u open:%d status:%u rx:%u\n"
        "dxg_iospace=offer_mb:%u set:%d ret:%d len:%u base:0x%lx size:0x%lx\n"
        "dxg_fence_map=source:%u mode:%u raw:0x%lx canonical:0x%lx off:0x%lx off_candidate:0x%lx off_cur:%lu size:%lu user:0x%lx kva:0x%lx failures:%u max_seen:%u max_source:%u max_mode:%u max_raw:0x%lx max_canonical:0x%lx max_off_candidate:0x%lx max_off_cur:%lu max_kva:0x%lx max_cur:%lu max_target:%lu\n"
        "hyperv_dxg_vgpu=%d count=%d relid=%u conn=%u monitor=%u\n"
        "hyperv_dxg_vgpu_transport=gpadl:%d status:%u open:%d status:%u rx:%u\n"
        "dxg_status=size:%u truncated:%d snapshot:%d\n"
        "dxg_host_events=next:%lu last:%lu signals:%u wait_ok:%u wait_timeout:%u wait_fail:%u\n"
        "dxg_channel_pump=active:%d skips:%u sync_active:%d sync_waits:%u sync_timeouts:%u\n"
        "dxg_completion_last=type:%u flags:0x%x desc_len8:%u desc_off8:%u pkt_len:%u pkt_off:%u payload:%u trans:%lu waiting:%lu source:%s/%u wait_source:%s/%u match:%u channel_match:%u captured_type:%u captured_len:%u prefix:%02x%02x%02x%02x%02x%02x%02x%02x\n"
        "dxg_object_table=max:%u drops:%u denied:%u generation:%u reuse_delayed:%u reuse_allowed:%u min_free:%u free_count:%u free_head:%u free_tail:%u\n"
        "dxg_track_limits=limit:%u alloc_max:%u alloc_drop:%u gpuva_max:%u gpuva_drop:%u hwqueue_max:%u hwqueue_drop:%u pagingqueue_max:%u pagingqueue_drop:%u\n"
        "dxg_pagingqueue_last=len:%u ret:%d queue:0x%x sync:0x%x fence_pa:0x%lx fence_off:0x%lx\n"
        "dxg_gpuva_last=reserve_len:%u reserve_ret:%d va:0x%lx fence:%lu free_len:%u free_ret:%d free_adapter:0x%x free_base:0x%lx free_size:%lu free_wire_size:%lu\n"
        "dxg_syncobject_last=len:%u ret:%d handle:0x%x type:%u flags:0x%x global:0x%x fence_cpu:0x%lx fence_gpu:0x%lx fence_pa:0x%lx fence_off:0x%lx signal_len:%u signal_ret:%d signal_status:0x%x wait_len:%u wait_ret:%d wait_status:0x%x gpu_signal_len:%u gpu_signal_ret:%d gpu_signal_status:0x%x gpu_wait_len:%u gpu_wait_ret:%d gpu_wait_status:0x%x\n"
        "dxg_syncobject_wire=cmd_len:%u result_len:%u res_sync_off:%u res_global_off:%u res_fgpu_off:%u res_fpa_off:%u res_foff_off:%u head:%u args_off:%u hint_off:%u hint:%u in_shared:0x%x proc:0x%x owner:0x%x/%u\n"
        "dxg_syncwait_detail=event:%lu async:%u object:0x%x fence:%lu current:%lu result:%u\n"
        "dxg_syncgpu_wait_detail=context:0x%x object:0x%x count:%u type:%u legacy:%u fence:%lu cmd_len:%u\n"
        "dxg_allocation_last=len:%u ret:%d count:%u resource:0x%x allocation:0x%x size:%lu destroy_len:%u destroy_ret:%d unwind_attempts:%u unwind_successes:%u unwind_ret:%d\n"
        "dxg_destroyallocation_last=dev:0x%x res:0x%x alloc:0x%x proc:0x%x ctx:%u count:%u len:%u ret:%d status:0x%x d3d12_match:%u pending:%u last:%u seq:%lu create_seq:%lu first_nt_seq:%lu first_match_seq:%lu first_ctx:%u first_match:%u first_pending:%u first_before_nt:%u last_before_nt:%u ctx_mask:0x%x\n"
        "dxg_createallocation_wire=cmd_len:%u hdr:%u prr_off:%u make_off:%u allocinfo_off:%u private_off:%u result_min:%u result_len:%u wire:%u ext:%u eoff:%u route_global:%u send_ret:%d proc:0x%x\n"
        "dxg_createallocation_result=flags_off:%u res_off:%u global_off:%u vgpu_off:%u allocinfo_off:%u allocinfo_size:%u head:%u\n"
        "dxg_allocation_priv=runtime:%u resource_priv:%u size:%u flags:0x%x sysmem:0x%lx pri:0x%lx existing_pages:%u pin_ret:%d set_ret:%d pin_ok:%u set_ok:%u total_pages:%lu in_len:%u in:%02x%02x%02x%02x%02x%02x%02x%02x out_len:%u out:%02x%02x%02x%02x%02x%02x%02x%02x\n"
        "dxg_existing_sysmem=attempts:%u path:%u standard:%u writable:%u dev:0x%x alloc:0x%x va:0x%lx size:%lu pages:%u first_pfn:0x%lx last_pfn:0x%lx pin_ret:%d set_ret:%d pin_ok:%u set_ok:%u total_pages:%lu active_pages:%lu pin_events:%lu unpin_events:%lu\n"
        "dxg_residency_last=make_len:%u make_ret:%d make_host_ret:%d make_user_ret:%d make_status:0x%x pending_ok:%u fence:%lu cur:%lu sync:0x%x trim:%lu device:0x%x pq:0x%x flags:0x%x count:%u sorted:%u in:%x,%x,%x,%x wire:%x,%x,%x,%x evict_len:%u evict_ret:%d evict_trim:%lu\n"
        "dxg_makeresident_shape=cmd:%u wsl_cmd:%u result:%u actual:%u owner_ok:%u tracked:%u order:%u a0:alloc:0x%x/dev:0x%x/res:0x%x/owner:0x%x/%u/%u a1:alloc:0x%x/dev:0x%x/res:0x%x/owner:0x%x/%u/%u\n"
        "dxg_mapgpuva_last=len:%u ret:%d status:0x%x pq:0x%x alloc:0x%x base:0x%lx min:0x%lx max:0x%lx pages:%lu prot:0x%lx dprot:0x%lx va:0x%lx fence:%lu cur:%lu sync:0x%x\n"
        "dxg_submit_last=submit_len:%u submit_ret:%d submit_status:0x%x cmd:0x%lx cmd_len:%u flags:0x%x priv:%u contexts:%u ctx0:0x%x\n"
        "dxg_lock2_last=len:%u ret:%d status:0x%x allocation:0x%x offset:0x%lx user_va:0x%lx unlock_len:%u unlock_ret:%d unlock_status:0x%x unlock_allocation:0x%x\n"
        "dxg_allocation_history=index:%u h0:dev:0x%x/res:0x%x/alloc:0x%x/size:%lu/len:%u/ret:%d/count:%u/priv:%u h1:dev:0x%x/res:0x%x/alloc:0x%x/size:%lu/len:%u/ret:%d/count:%u/priv:%u h2:dev:0x%x/res:0x%x/alloc:0x%x/size:%lu/len:%u/ret:%d/count:%u/priv:%u h3:dev:0x%x/res:0x%x/alloc:0x%x/size:%lu/len:%u/ret:%d/count:%u/priv:%u\n"
        "dxg_allocation_history2=h4:dev:0x%x/res:0x%x/alloc:0x%x/size:%lu/len:%u/ret:%d/count:%u/priv:%u h5:dev:0x%x/res:0x%x/alloc:0x%x/size:%lu/len:%u/ret:%d/count:%u/priv:%u h6:dev:0x%x/res:0x%x/alloc:0x%x/size:%lu/len:%u/ret:%d/count:%u/priv:%u h7:dev:0x%x/res:0x%x/alloc:0x%x/size:%lu/len:%u/ret:%d/count:%u/priv:%u\n"
        "dxg_allocation_history_meta=h0:global:0x%x/flags:0x%x h1:global:0x%x/flags:0x%x h2:global:0x%x/flags:0x%x h3:global:0x%x/flags:0x%x h4:global:0x%x/flags:0x%x h5:global:0x%x/flags:0x%x h6:global:0x%x/flags:0x%x h7:global:0x%x/flags:0x%x\n"
        "dxg_mapgpuva_history=index:%u h0:alloc:0x%x/va:0x%lx/pages:%lu/fence:%lu/len:%u/ret:%d/status:0x%x/pq:0x%x h1:alloc:0x%x/va:0x%lx/pages:%lu/fence:%lu/len:%u/ret:%d/status:0x%x/pq:0x%x h2:alloc:0x%x/va:0x%lx/pages:%lu/fence:%lu/len:%u/ret:%d/status:0x%x/pq:0x%x h3:alloc:0x%x/va:0x%lx/pages:%lu/fence:%lu/len:%u/ret:%d/status:0x%x/pq:0x%x\n"
        "dxg_mapgpuva_history2=h4:alloc:0x%x/va:0x%lx/pages:%lu/fence:%lu/len:%u/ret:%d/status:0x%x/pq:0x%x h5:alloc:0x%x/va:0x%lx/pages:%lu/fence:%lu/len:%u/ret:%d/status:0x%x/pq:0x%x h6:alloc:0x%x/va:0x%lx/pages:%lu/fence:%lu/len:%u/ret:%d/status:0x%x/pq:0x%x h7:alloc:0x%x/va:0x%lx/pages:%lu/fence:%lu/len:%u/ret:%d/status:0x%x/pq:0x%x\n"
        "dxg_lock2_history=index:%u h0:dev:0x%x/alloc:0x%x/off:0x%lx/user:0x%lx/size:%lu/len:%u/ret:%d/status:0x%x h1:dev:0x%x/alloc:0x%x/off:0x%lx/user:0x%lx/size:%lu/len:%u/ret:%d/status:0x%x h2:dev:0x%x/alloc:0x%x/off:0x%lx/user:0x%lx/size:%lu/len:%u/ret:%d/status:0x%x h3:dev:0x%x/alloc:0x%x/off:0x%lx/user:0x%lx/size:%lu/len:%u/ret:%d/status:0x%x\n"
        "dxg_lock2_history2=h4:dev:0x%x/alloc:0x%x/off:0x%lx/user:0x%lx/size:%lu/len:%u/ret:%d/status:0x%x h5:dev:0x%x/alloc:0x%x/off:0x%lx/user:0x%lx/size:%lu/len:%u/ret:%d/status:0x%x h6:dev:0x%x/alloc:0x%x/off:0x%lx/user:0x%lx/size:%lu/len:%u/ret:%d/status:0x%x h7:dev:0x%x/alloc:0x%x/off:0x%lx/user:0x%lx/size:%lu/len:%u/ret:%d/status:0x%x\n"
	        "dxg_queryadapter_last=type:%u size:%u len:%u user_len:%u ret:%d status:0x%x layout:%u\n"
        "dxg_queryadapter_wire=cmd_len:%u type_off:%u size_off:%u data_off:%u adapter:0x%x host:0x%x source:%u owner:0x%x/%u refs:%u result_req:%u expected_wsl:%u proc_src:%u\n"
        "dxg_queryadapter_adapter_object=handle:0x%x host:0x%x owner:0x%x/%u generation:%u\n"
        "dxg_queryadapter_process_adapter=local_ns:%u refs:%u locals:%u generation:%u type15_fail:ret:%d status:0x%x proc:0x%x route:%u ext_luid:%x:%x\n"
        "dxg_queryadapter_send=route:%u ext_luid:%x:%x\n"
        "dxg_queryadapter_completion=desc_type:%u flags:0x%x desc_len8:%u desc_off8:%u pkt_len:%u pkt_off:%u payload:%u trans:%lu waiting:%lu source:%s/%u wait_source:%s/%u match:%u channel_match:%u captured:%u/%u prefix:%02x%02x%02x%02x%02x%02x%02x%02x\n"
	        "dxg_queryadapter_zero_success=type:%u size:%u count:%u "
	        "host:type:%u/len:%u/ret:%d/status:0x%x user_ret:%d\n"
        "dxg_queryadapter_type0=priv_size:%u hash:%08x primary_len:%u primary_ret:%d primary_status:0x%x fallback_attempted:%u fallback_used:%u fallback_len:%u fallback_ret:%d fallback_status:0x%x result_route:%u fallback_reason:%u fallback_route:%u\n"
        "dxg_queryadapter_umd_rewrite=attempted:%u rewritten:%u path:%u vendor:0x%x orig_hash:%08x orig:%x,%x host_status:0x%x ret:%d\n"
        "dxg_queryadapter_type0_input=head_len:%u head:%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x tail_len:%u tail:%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n"
        "dxg_adapter_hardware=vendor:0x%x device:0x%x raw:%x,%x,%x,%x,%x,%x,%x size:%u norm:%u fallback:%u count:%u source:%u source_name:%s rejected_synthetic:%u cache_available:%u payload_len:%u\n"
        "dxg_adapter_hardware_v40=short_zero:%u len:%u ret:%d status:0x%x cached_real:%u source_name:%s rejected_synthetic_pci:%u\n"
        "dxg_adapter_hardware_temp_v27=attempts:%u successes:%u failures:%u ret:%d status:0x%x open_len:%u query_len:%u close_len:%u close_ret:%d close_status:0x%x handle:0x%x restored:%u/%u\n"
        "dxg_enumadapters_last=cmd:0x%x in_count:%u out_count:%u num_sources:%u buffer:0x%lx handle:0x%x luid:%x:%x source:%u ret:%d\n"
        "dxg_enumadapters_diag=stage:%u ensure:%d bind:%d local:%d copy:%d global:%u/%u/%u vgpu:%u/%u/%u host:0x%x probe:%u/%d/0x%x/0x%x ready:%u process:0x%x/%u gen:%u refs:%u adapters:%u locals:%u objects:%u\n"
        "dxg_local_adapter_namespace=hits:%u misses:%u last:result:%u handle:0x%x host:0x%x refs:%u locals:%u generation:%u reuse_delayed:%u reuse_allowed:%u min_free:%u\n"
        "dxg_queryadapter_history=index:%u h0:%u/%u/%u/%d/0x%x h1:%u/%u/%u/%d/0x%x h2:%u/%u/%u/%d/0x%x h3:%u/%u/%u/%d/0x%x h4:%u/%u/%u/%d/0x%x h5:%u/%u/%u/%d/0x%x h6:%u/%u/%u/%d/0x%x h7:%u/%u/%u/%d/0x%x\n"
        "dxg_queryadapter_history2=h8:%u/%u/%u/%d/0x%x h9:%u/%u/%u/%d/0x%x h10:%u/%u/%u/%d/0x%x h11:%u/%u/%u/%d/0x%x h12:%u/%u/%u/%d/0x%x h13:%u/%u/%u/%d/0x%x h14:%u/%u/%u/%d/0x%x h15:%u/%u/%u/%d/0x%x\n"
        "dxg_qai_admission=index:%u a0:k%u/t%u/sz%u/len%u/ret%d/st0x%x/r%u/src%u/a0x%x/h0x%x/head0x%x a1:k%u/t%u/sz%u/len%u/ret%d/st0x%x/r%u/src%u/a0x%x/h0x%x/head0x%x a2:k%u/t%u/sz%u/len%u/ret%d/st0x%x/r%u/src%u/a0x%x/h0x%x/head0x%x a3:k%u/t%u/sz%u/len%u/ret%d/st0x%x/r%u/src%u/a0x%x/h0x%x/head0x%x\n"
        "dxg_qai_admission2=a4:k%u/t%u/sz%u/len%u/ret%d/st0x%x/r%u/src%u/a0x%x/h0x%x/head0x%x a5:k%u/t%u/sz%u/len%u/ret%d/st0x%x/r%u/src%u/a0x%x/h0x%x/head0x%x a6:k%u/t%u/sz%u/len%u/ret%d/st0x%x/r%u/src%u/a0x%x/h0x%x/head0x%x a7:k%u/t%u/sz%u/len%u/ret%d/st0x%x/r%u/src%u/a0x%x/h0x%x/head0x%x\n"
        "dxg_queryregistry_last=query:%u flags:0x%x value_type:%u phys:%u output_size:%u status:%u name:%s name0:0x%x name1:0x%x\n"
        "dxg_feature_last=id:%u len:%u ret:%d status:0x%x result:0x%x\n"
        "dxg_priority_last=sched_len:%u sched_ret:%d sched_status:0x%x context:0x%x priority:%d alloc_len:%u alloc_ret:%d alloc_status:0x%x count:%u residency_len:%u residency_ret:%d residency_status:0x%x residency_count:%u residency_value:%u\n"
        "dxg_statistics_last=len:%u ret:%d status:0x%x type:%u\n"
        "dxg_clockcalibration_last=len:%u ret:%d status:0x%x node:%u gpu_freq:%lu gpu_counter:%lu cpu_counter:%lu\n"
        "dxg_stdalloc_last=len:%u ret:%d status:0x%x alloc_priv:%u res_priv:%u\n"
        "dxg_escape_last=len:%u ret:%d type:%u flags:0x%x size:%u\n"
        "dxg_shareobject_last=len:%u cmd_len:%u dev_off:%u obj_off:%u wire:%u ext:%u eoff:%u result_len:%u head:%u comp:%u/%u/%02x%02x%02x%02x%02x%02x%02x%02x ret:%d status:0x%x proc:0x%x device:0x%x object:0x%x reserved:0x%lx nt:0x%lx\n"
        "dxg_shareobject_diag=attempted:%u valid_nt:%u kind:%u reason:%u\n"
        "dxg_vgpu_send_last=cmd:%u cmd_len:%u wire_len:%u ext:%u off:%u proc:0x%x channel:%u luid:%x:%x route_global:%u retries:%u ret:%d\n"
        "dxg_global_send_last=cmd:%u cmd_len:%u wire_len:%u ext:%u off:%u proc:0x%x channel:%u luid:%x:%x retries:%u ret:%d\n"
        "dxg_global_send_share=nt:%u/%u/%u/0x%x share:%u/%u/%u/0x%x destroynt:%u/%u/%u/0x%x destroysync:%u/%u/%u/0x%x\n"
        "dxg_global_send_ntwire=cmdid:%lu cmd:%u channel:%u proc:0x%x cmd_len:%u wire_len:%u result_len:%u ext:%u off:%u relid:%u conn:%u monitor:%u/%u dedicated:%u luid:%x:%x\n"
        "dxg_global_send_ntext=cmdid:%lu cmd:%u channel:%u proc:0x%x cmd_len:%u wire_len:%u result_len:%u ext:%u off:%u relid:%u conn:%u monitor:%u/%u dedicated:%u luid:%x:%x\n"
        "dxg_global_send_sharewire=cmdid:%lu cmd:%u channel:%u proc:0x%x cmd_len:%u wire_len:%u result_len:%u ext:%u off:%u relid:%u conn:%u monitor:%u/%u dedicated:%u\n"
        "dxg_ntshared_last=create_len:%u create_cmd_len:%u obj_off:%u result_len:%u comp:%u/%u/%02x%02x%02x%02x%02x%02x%02x%02x create_ret:%d proc:0x%x type:%u channel:%u object:0x%x handle:0x%x raw0:0x%x zero_len:%u create_nt_zero_len:%u hard_fail:%u side_effect:%u share_fallback:%u share_valid:%u destroy_len:%u destroy_ret:%d destroy_handle:0x%x\n"
        "dxg_process_match=alloc_proc:0x%x alloc_owner:0x%x/%u sync_proc:0x%x sync_owner:0x%x/%u nt_proc:0x%x nt_obj:0x%x alloc_dev:0x%x known:%u from_create:%u adapter:0x%x\n"
        "dxg_ntshared_cache=hits:%u misses:%u inserts:%u releases:%u destroys:%u full:%u last:kind:%u/proc:0x%x/obj:0x%x/nt:0x%x/refs:%u\n"
        "dxg_sharedhandle_last=cmd:0x%x ret:%d device:0x%x object:0x%x nt:0x%lx count:%u global:0x%x d3d_flags:0x%x fops:%u\n"
        "dxg_sharedhandle_invariant=mode:wsl_raw current:0x%x/%u creator:0x%x/%u owner:0x%x/%u used:%u raw:0x%x/0x%x host_diag:0x%x/0x%x dev_found:%u obj_found:%u kind:%u type:%u flags:0x%x allocs:%u\n"
        "dxg_sharedhandle_lookup=raw_obj:0x%x resource:%u allocation:%u sync:%u chosen_kind:%u chosen_type:%u chosen_obj:0x%x\n"
        "dxg_sharedhandle_detail=kind:%u host_object:0x%x found:%u parent:0x%lx current:0x%x/%u owner:0x%x/%u/%u used:%u type:%u obj_dev:0x%x alloc0:0x%x alloc_found:%u alloc_owner:0x%x/%u/%u flags:0x%x allocs:%u sealed:%u\n"
        "dxg_shared_sync_detail=sync_type:%u sync_flags:0x%x sync_global:0x%x monitor:%u fence_cpu:0x%lx fence_kva:0x%lx\n"
        "dxg_sharedsync_export=fd:%u ret:%d cloexec:%u fops:%u kind:%u type:%u flags:0x%x monitor:%u global:0x%x zero:%u host_shared_handle:0x%x host_nt:0x%x refs:%u owner_obj:0x%x device:0x%x host_device:0x%x object:0x%x host_object:0x%x cache_proc:0x%x owner:0x%x/%u/%u\n"
        "dxg_sharedfd_shape=sync_fops:%u resource_fops:%u last_fops:%u last_kind:%u generic_custom_fd:%u vfs_name:%u anon_inode:%u wider_vfs:%u stat_mode:0x%x sync_ino:0x%x resource_ino:0x%x\n"
        "dxg_shareobjects_input=desired:0x%x attr:0x%lx attr_len:%u attr_ret:%d attr_head:%02x%02x%02x%02x%02x%02x%02x%02x\n"
        "dxg_sharedhandle_prereq=map:va:0x%lx/pages:%lu/fence:%lu/ret:%d/status:0x%x resident:pq:0x%x/sync:0x%x/fence:%lu/current:%lu wait:%u/%d missing:%u enforced:%u\n"
        "dxg_sharedresource_lifetime=created:%u seals:%u reuses:%u denied:%u seal_allocs:%u seal_priv:%u open_tracked:%u\n"
        "dxg_sharedresource_owner=exists:%u cached:%u reused:%u nt:0x%x refs:%u sealed:%u object:0x%x proc:0x%x open_global:0x%x pre_sealable:%u pre_ret:%d\n"
        "dxg_sharedresource_metadata=track_host:%u runtime:%u/%08x resource:%u/%08x total:%u/%08x alloc0:%u match_in:%u match_out:%u w4:%08x w8:%08x logical_flags:0x%x host_result_flags:0x%x host_flags_ignored:%u\n"
        "dxg_sharedresource_preseal=mode:deferred applied:%u before:%u after:%u ret:%d\n"
        "dxg_sharedresource_record=valid:%u stage:%u key:k%u/p0x%x/o0x%x/g0x%x/nt0x%x source:proc0x%x/tgid%lu/gen%u dev:0x%x res:0x%x alloc0:0x%x adapter:%x:%x host_adapter:%x:%x sealed:%u gen:%u before_fd:%u allocs:%u sizes:%u/%u/%u/%u hashes:%08x/%08x/%08x/%08x refs:%u query:%u open:%u fd:%u admit:%d exact:%u mutated:%u\n"
        "dxg_sharedresource_model=valid:%u flat_match:%u allocs:%u alloc0:0x%x priv0:%u size0:%lu flags0:0x%x sizes:%u/%u/%u sealed:%u gen:%u\n"
        "dxg_queryresource_nt=seen:%u ret:%d device:0x%x nt:0x%lx kind:%u fops:%u sync_probe:%u global:0x%x host_nt:0x%x refs:%u object:0x%x cache_obj:0x%x allocs:%u runtime:%u resource:%u total:%u\n"
        "dxg_openresource_envelope=route:vgpu global_route:%u cmd:%u wire:%u ext:%u eoff:%u result:%u actual:%u ret:%d status:0x%x proc:0x%x device:0x%x global:0x%x allocs:%u total_priv:%u out_res:0x%x out_alloc0:0x%x seal:%u->%u fd_kind:%u fd_fops:%u fd_refs:%u\n"
        "dxg_opensync_envelope=route:global cmd:%u wire:%u ext:%u eoff:%u result:%u actual:%u ret:%d status:0x%x proc:0x%x device:0x%x global:0x%x flags:0x%x out_sync:0x%x gpu_va:0x%lx cpu_pa:0x%lx fd_kind:%u fd_refs:%u\n"
        "dxg_opensync_shape=fops:%u expected_fops:%u type:%u user_flags:0x%x wire_flags:0x%x forced:%u sync_fd_fops:%u resource_fd_fops:%u generic_custom_fd:%u vfs_name:%u off_dev:%u off_global:%u off_flags:%u\n"
        "dxg_opensync_target=target:0x%x/host:0x%x owner:0x%x/%u gen:%u source_dev:0x%x/host:0x%x source_owner:0x%x/%u same:%u adapter_match:%u adapter:%x:%x host_adapter:%x:%x source_flags:0x%x source_type:%u monitor:%u global:0x%x nt:0x%x status:0x%x\n"
        "dxg_opensync_handle_source=input_nt:0x%lx host_nt:0x%x host_shared:0x%x object:0x%x cache_obj:0x%x fd_kind:%u fops:%u refs:%u uses_global:%u nt_diff:%u\n"
        "dxg_opensync_gate=count:%u gate:%u last_cmd:0x%x last_nr:%u input_nt:0x%lx host_nt:0x%x kind:%u global:0x%x object:0x%x cache_obj:0x%x ret:%d\n"
        "dxg_sharedfd_close=kind:%u fops:%u proc:0x%x obj:0x%x cache_obj:0x%x nt:0x%x host_shared:0x%x global:0x%x refs:%u->%u destroyed:0x%x destroy_ret:%d status:0x%x actual:%u cmd:%u wire:%u ext:%u eoff:%u result_ntstatus:%u handle_off:%u\n"
        "dxg_sharedfd_close_by_kind=sync:seen:%u/fops:%u/refs:%u->%u/destroyed:0x%x/ret:%d/status:0x%x/actual:%u/cmd:%u/wire:%u/ext:%u/result:%u resource:seen:%u/fops:%u/refs:%u->%u/destroyed:0x%x/ret:%d/status:0x%x/actual:%u/cmd:%u/wire:%u/ext:%u/result:%u\n"
        "dxg_unsupported_last=cmd:0x%x ret:%d device:0x%x handle:0x%x count:%u nr:%u size:%u name:%u\n"
        "dxg_markdeviceaserror_last=len:%u ret:%d status:0x%x device:0x%x reason:0x%x process:0x%x cmd_len:%u\n"
        "dxg_syncfile_last=cmd:0x%x ret:%d device:0x%x object:0x%x context:0x%x handle:0x%lx fence:%lu global:0x%x host_nt:0x%x source_flags:0x%x open_flags:0x%x len:%u status:0x%x out_sync:0x%x cpu:0x%lx gpu:0x%lx\n"
        "dxg_syncfile_lifetime=live:%u creates:%u releases:%u event_removed:%u nt_released:%u create_faults:%u fd_reclaimed:%u open_faults:%u open_destroy:%u/%u/%d host_events:%u/%u/%u last_event:%lu removed:%lu\n"
        "dxg_updateallocproperty_last=len:%u ret:%d status:0x%x allocation:0x%x fence:%lu\n"
        "dxg_vidmem_reservation_last=len:%u ret:%d status:0x%x group:%u reservation:%lu\n"
        "dxg_offer_reclaim_last=offer_len:%u offer_ret:%d offer_status:0x%x offer_count:%u reclaim_len:%u reclaim_ret:%d reclaim_status:0x%x reclaim_count:%u reclaim_result0:%u reclaim_fence:%lu\n"
        "dxg_updategpuva_last=len:%u ret:%d status:0x%x ops:%u fence:%lu device:0x%x context:0x%x fence_obj:0x%x flags:0x%x cmd_len:%u op_off:%u op_size:%u op0:type:%u/base:0x%lx/size:%lu/alloc:0x%x/aoff:%lu/asize:%lu/src:0x%lx/dst:0x%lx/prot:0x%lx/dprot:0x%lx\n"
        "dxg_cacheops_last=len:%u ret:%d status:0x%x allocation:0x%x\n"
        "dxg_context_last=len:%u ret:%d handle:0x%x device:0x%x node:%u engine:%u flags:0x%x hint:%u priv:%u fail_len:%u fail_ret:%d fail_status:0x%x\n"
        "dxg_context_priv_head=len:%u bytes:%02x%02x%02x%02x%02x%02x%02x%02x\n"
        "dxg_destroy_last=device_len:%u device_ret:%d device_status:0x%x context_len:%u context_ret:%d context_status:0x%x paging_len:%u paging_ret:%d paging_status:0x%x sync_len:%u sync_ret:%d sync_status:0x%x sync_handle:0x%x sync_device:0x%x sync_type:%u sync_flags:0x%x sync_global:0x%x sync_monitor_fence:%u sync_cmd_len:%u sync_wire:%u sync_ext:%u sync_eoff:%u\n"
        "dxg_flushdevice_last=len:%u ret:%d status:0x%x device:0x%x reason:%u\n"
        "dxg_hwqueue_last=create_len:%u create_ret:%d create_status:0x%x context:0x%x flags:0x%x priv:%u queue:0x%x fence:0x%x fence_cpu:0x%lx fence_gpu:0x%lx submit_len:%u submit_ret:%d destroy_len:%u destroy_ret:%d\n"
        "dxg_hwqueue_priv_head=create_len:%u create:%02x%02x%02x%02x%02x%02x%02x%02x submit_queue:0x%x submit_fence:%lu submit_cmd_len:%u submit_priv:%u submit_len:%u submit:%02x%02x%02x%02x%02x%02x%02x%02x\n"
        "dxg_probe_attempts=%u successes=%u last_ret=%d\n"
        "dxg_probe_open_status=%d handle=0x%x host_version=%u host_compat=%u\n"
        "dxg_probe_info_len=%u flags=0x%x async_msg=%u ext_header=%u host_vgpu_luid=%x:%x\n"
        "dxg_probe_v40=open_ret:%d open_len:%u open_status:0x%x open:0x%x/%u/%u guest_luid:%x:%x getinternal_ret:%d getinternal_len:%u getinternal_flags:0x%x reject:%u\n"
        "dxg_identity=pci_source:%s guestcaps:%d/%u/%u bdf:%x global:%u/%u/%u/%u/%u vgpu:%u/%u/%u/%u/%u gdef:%08x:%08x:%08x:%08x vdef:%08x:%08x:%08x:%08x openver:%u/%u/%u flags:0x%x async:%u ext:%u luid_src:%u host_luid:%x:%x pci_luid:%x:%x mmio_mb:%u\n"
        "dxg_transport_policy=normal-v40-ext/wsl-open-zero-luid active:v%u host_v40:%s pci_host:%u pci_neg:%u ext:%u type31:v40-first/cached-real-or-fail\n"
        "dxg_negotiation=active:%u compat:%u source:%u fallbacks:%u pci_host:%u pci_neg:%u pci_write:%u/%u/0x%x/0x%x/%d open:%u/%u/%u ext:%u info_len:%u\n"
        "dxg_pci_vmbus_write=attempted:%u writes:%u supported:%u cfg_ret:%d verify_ret:%d value:0x%x readback:0x%x ret:%d source:%u token:0x%x\n"
        "hyperv_pci_offer=present:%u count:%u relid:%u conn:%u monitor:%u alloc:%u dedicated:%u flags:0x%x mmio_mb:%u inst:%08x:%04x:%04x:%02x%02x user:%08x:%08x:%08x:%08x\n"
        "hyperv_pci_channel=present:%u relid:%u conn:%u gpadl:%d/%u open:%d/%u protocol:%d version:0x%x attempts:%u last:0x%x status:%d ret:%d pkt:%u len:%u trans:%lu prefix:%02x%02x%02x%02x%02x%02x%02x%02x\n"
        "hyperv_pci_bus=cfg:%d src:%u ret:%d pa:0x%lx va:0x%lx size:%u cand:0x%lx lim:0x%lx reject:%u backend:%d/%d d0:%u/%u/%d/%d query:%u/%u/%d rel:%u/%u/%u/%u/%u child:%u reg:%u/%u first:%04x:%04x class:0x%x wslot:0x%x bdf:%u:%u:%u cfgio:%u/%u rej:%u last:%d token:0x%x off:%u size:%u val:0x%x pkt:%u/%u prefix:%02x%02x%02x%02x%02x%02x%02x%02x\n"
        "dxg_pci=domain:%u bdf:%x bus:%u dev:%u func:%u vendor:0x%x device:0x%x class:0x%x version:%u guid:%x-%x-%x-%x host_luid:%x:%x\n"
        "dxg_pci_guestcaps=attempts:%u writes:%u found:%u scan:%u attempted:%u verified:%u source:%u token:0x%x off:%u value:0x%x readback:0x%x ret:%d before_probe:%u bdf:%x\n"
        "dxg_probe_last_packet=type:%u len:%u prefix:%02x%02x%02x%02x%02x%02x%02x%02x\n"
        "dxg_createprocess_last=len:%u cmd_len:%u ret:%d guest:0x%lx pid:%lu tgid:%lu handle:0x%x layout:%u gen:%u destroy_len:%u destroy_ret:%d destroy_handle:0x%x\n"
        "d3dkmt_ioctls=%u successes=%u ready=%d last_ret=%d process=0x%x\n"
        "d3dkmt_ioctl_history=index:%u h0:0x%x/%u/%d h1:0x%x/%u/%d h2:0x%x/%u/%d h3:0x%x/%u/%d h4:0x%x/%u/%d h5:0x%x/%u/%d h6:0x%x/%u/%d h7:0x%x/%u/%d\n"
        "d3dkmt_ioctl_history2=h8:0x%x/%u/%d h9:0x%x/%u/%d h10:0x%x/%u/%d h11:0x%x/%u/%d h12:0x%x/%u/%d h13:0x%x/%u/%d h14:0x%x/%u/%d h15:0x%x/%u/%d\n"
        "d3dkmt_ioctl_counts=destroyalloc:%u destroycontext:%u destroyhwqueue:%u destroypaging:%u destroydevice:%u destroysync:%u freegpuva:%u closeadapter:%u\n"
        "d3dkmt_open_files=opens:%u live:%u cleanup_attempts:%u cleanup_successes:%u cleanup_last_ret:%d cleanup_last_op:%u cleanup_last_handle:0x%x cleanup_failed_op:%u cleanup_failed_handle:0x%x cleanup_had_tracked:%u\n"
        "d3dkmt_cleanup_detail=resource_host:%u resource_child_local:%u standalone_alloc_host:%u resource_alloc_skips:%u\n"
        "d3dkmt_cleanup_wsl_order=seq:%u sync:%u allocation:%u resource:%u context:%u hwqueue:%u paging:%u gpuva:%u device:%u process:%u valid:%u\n"
        "d3dkmt_processes=live:%u max:%u creates:%u reuses:%u releases:%u destroy_attempts:%u destroy_successes:%u destroy_failures:%u destroy_suppressed:%u destroy_deferred:%u shared_reuses:%u isolated_reuses:%u full:%u generation:%u retained:0x%x retained_gen:%u retained_refs:%u\n"
        "d3dkmt_process_destroy_active=total:%u device:%u context:%u hwqueue:%u paging:%u sync:%u allocation:%u resource:%u gpuva:%u\n"
        "d3dkmt_process_binding=new_host_created:%u same_tgid_reused:%u retained_reuse_avoided:%u avoided_tgid:%lu avoided_src_tgid:%lu avoided_handle:0x%x avoided_gen:%u avoided_src_gen:%u\n"
        "d3dkmt_process_reuse=last_tgid:%lu handle:0x%x local_gen:%u source_gen:%u copied_objects:%u source_objects:%u\n"
        "d3dkmt_process_lifetime=object_refs_last:%u mem_refs_last:%u object_releases:%u mem_releases:%u mem_frees:%u\n"
        "note=Hyper-V GPU-PV D3DKMT adapter ioctls are available; "
        "device/context/sync/paging/allocation residency/map/submit ioctls are wired; higher-level OpenGL/D3D runtime validation is still pending.\n",
        hvdxg.global_present, hvdxg.global_relid, hvdxg.global_conn_id,
        hvdxg.global_monitorid, hvdxg.global_gpadl_ok,
        hvdxg.global_gpadl_status, hvdxg.global_open_ok,
        hvdxg.global_open_status, hvdxg.global_rx_packets,
        hvdxg.global_mmio_megabytes, hvdxg.iospace_set,
        hvdxg.iospace_last_ret, hvdxg.iospace_last_len,
        hvdxg.iospace_base, hvdxg.iospace_size,
        hvdxg.fence_map_last_source, hvdxg.fence_map_last_mode,
        hvdxg.fence_map_last_raw_pa, hvdxg.fence_map_last_canonical_pa,
        hvdxg.fence_map_last_offset,
        hvdxg.fence_map_last_offset_candidate_pa,
        hvdxg.fence_map_last_offset_candidate_current,
        hvdxg.fence_map_last_size, hvdxg.fence_map_last_user_va,
        hvdxg.fence_map_last_kva, hvdxg.fence_map_failures,
        hvdxg.fence_value_max_seen, hvdxg.fence_map_max_source,
        hvdxg.fence_map_max_mode, hvdxg.fence_map_max_raw_pa,
        hvdxg.fence_map_max_canonical_pa,
        hvdxg.fence_map_max_offset_candidate_pa,
        hvdxg.fence_map_max_offset_candidate_current,
        hvdxg.fence_value_last_kva,
        hvdxg.fence_value_last_current, hvdxg.fence_value_last_target,
        hvdxg.vgpu_present, hvdxg.vgpu_count, hvdxg.vgpu_relid,
        hvdxg.vgpu_conn_id, hvdxg.vgpu_monitorid, hvdxg.vgpu_gpadl_ok,
        hvdxg.vgpu_gpadl_status, hvdxg.vgpu_open_ok,
        hvdxg.vgpu_open_status, hvdxg.vgpu_rx_packets,
        (uint32)status_size, 0, read_status != NULL ? 1 : 0,
        hvdxg.host_event_next_id, hvdxg.host_event_last_id,
        hvdxg.host_event_signal_count, hvdxg.host_event_wait_successes,
        hvdxg.host_event_wait_timeouts, hvdxg.host_event_wait_failures,
        hvdxg.pump_active, hvdxg.pump_skips, hvdxg.sync_active,
        hvdxg.sync_waits, hvdxg.sync_timeouts,
        hvdxg.completion_desc_type, hvdxg.completion_desc_flags,
        hvdxg.completion_desc_len8, hvdxg.completion_desc_offset8,
        hvdxg.completion_packet_len, hvdxg.completion_packet_offset,
        hvdxg.completion_payload_len,
        hvdxg.completion_desc_trans_id,
        hvdxg.completion_waiting_trans_id,
        hvdxg_channel_name(hvdxg.completion_source_channel),
        hvdxg.completion_source_relid,
        hvdxg_channel_name(hvdxg.completion_waiting_channel),
        hvdxg.completion_waiting_relid,
        hvdxg.completion_waiting_match,
        hvdxg.completion_waiting_channel_match,
        hvdxg.completion_type, hvdxg.completion_len,
        hvdxg.completion_buf[0], hvdxg.completion_buf[1],
        hvdxg.completion_buf[2], hvdxg.completion_buf[3],
        hvdxg.completion_buf[4], hvdxg.completion_buf[5],
        hvdxg.completion_buf[6], hvdxg.completion_buf[7],
        hvdxg.object_table_max, hvdxg.object_table_drops,
        hvdxg.object_table_denied, hvdxg.object_table_generation,
        hvdxg.object_table_reuse_delayed,
        hvdxg.object_table_reuse_allowed,
        hvdxg.object_table_min_free_entries,
        hvdxg.object_table_free_count,
        hvdxg.object_table_free_head,
        hvdxg.object_table_free_tail,
        (uint32)HV_DXG_OPEN_TRACKED_MAX, hvdxg.track_allocation_max,
        hvdxg.track_allocation_drops, hvdxg.track_gpuva_max,
        hvdxg.track_gpuva_drops, hvdxg.track_hwqueue_max,
        hvdxg.track_hwqueue_drops, hvdxg.track_pagingqueue_max,
        hvdxg.track_pagingqueue_drops,
        hvdxg.pagingqueue_last_len, hvdxg.pagingqueue_last_ret,
        hvdxg.pagingqueue_last_queue, hvdxg.pagingqueue_last_sync,
        hvdxg.pagingqueue_last_fence_pa, hvdxg.pagingqueue_last_fence_off,
        hvdxg.gpuva_reserve_last_len, hvdxg.gpuva_reserve_last_ret,
        hvdxg.gpuva_reserve_last_va, hvdxg.gpuva_reserve_last_fence,
        hvdxg.gpuva_free_last_len, hvdxg.gpuva_free_last_ret,
        hvdxg.gpuva_free_last_adapter, hvdxg.gpuva_free_last_base,
        hvdxg.gpuva_free_last_size, hvdxg.gpuva_free_last_wire_size,
        hvdxg.syncobject_last_len, hvdxg.syncobject_last_ret,
        hvdxg.syncobject_last_handle, hvdxg.syncobject_last_type,
        hvdxg.syncobject_last_flags, hvdxg.syncobject_last_global,
        hvdxg.syncobject_last_fence_cpu,
        hvdxg.syncobject_last_fence_gpu, hvdxg.syncobject_last_fence_pa,
        hvdxg.syncobject_last_fence_off, hvdxg.syncsignal_last_len,
        hvdxg.syncsignal_last_ret, (uint32)hvdxg.syncsignal_last_status,
        hvdxg.syncwait_last_len, hvdxg.syncwait_last_ret,
        (uint32)hvdxg.syncwait_last_status,
        hvdxg.syncwait_last_event, hvdxg.syncwait_last_async,
        hvdxg.syncwait_last_object, hvdxg.syncwait_last_fence,
        hvdxg.syncwait_last_current, hvdxg.syncwait_last_result,
        hvdxg.syncgpu_signal_last_len, hvdxg.syncgpu_signal_last_ret,
        (uint32)hvdxg.syncgpu_signal_last_status,
        hvdxg.syncgpu_wait_last_len, hvdxg.syncgpu_wait_last_ret,
        (uint32)hvdxg.syncgpu_wait_last_status,
        hvdxg.syncgpu_wait_last_context,
        hvdxg.syncgpu_wait_last_object,
        hvdxg.syncgpu_wait_last_object_count,
        hvdxg.syncgpu_wait_last_object_type,
        hvdxg.syncgpu_wait_last_legacy,
        hvdxg.syncgpu_wait_last_fence,
        hvdxg.syncgpu_wait_last_cmd_len,
        hvdxg.syncobject_last_cmd_len,
        hvdxg.syncobject_last_result_len,
        hvdxg.syncobject_last_result_sync_offset,
        hvdxg.syncobject_last_result_global_offset,
        hvdxg.syncobject_last_result_fence_gpu_offset,
        hvdxg.syncobject_last_result_fence_pa_offset,
        hvdxg.syncobject_last_result_fence_off_offset,
        hvdxg.syncobject_last_result_head_len,
        hvdxg.syncobject_last_args_offset,
        hvdxg.syncobject_last_client_hint_offset,
        hvdxg.syncobject_last_client_hint,
        hvdxg.syncobject_last_input_shared,
        hvdxg.syncobject_last_process,
        hvdxg.syncobject_last_owner_process,
        hvdxg.syncobject_last_owner_generation,
        hvdxg.allocation_last_len, hvdxg.allocation_last_ret,
        hvdxg.allocation_last_count, hvdxg.last_resource_handle,
        hvdxg.last_allocation_handle, hvdxg.last_allocation_size,
        hvdxg.destroyalloc_last_len, hvdxg.destroyalloc_last_ret,
        hvdxg.createalloc_unwind_attempts,
        hvdxg.createalloc_unwind_successes,
        hvdxg.createalloc_unwind_last_ret,
        hvdxg.destroyalloc_last_device,
        hvdxg.destroyalloc_last_resource,
        hvdxg.destroyalloc_last_allocation,
        hvdxg.destroyalloc_last_process,
        hvdxg.destroyalloc_last_context,
        hvdxg.destroyalloc_last_count,
        hvdxg.destroyalloc_last_len,
        hvdxg.destroyalloc_last_ret,
        (uint32)hvdxg.destroyalloc_last_status,
        hvdxg.destroyalloc_d3d12_match_count,
        hvdxg.destroyalloc_d3d12_pending_match_count,
        hvdxg.destroyalloc_d3d12_last_match,
        hvdxg.destroyalloc_d3d12_last_seq,
        hvdxg.d3d12_shared_create_seq,
        hvdxg.d3d12_shared_first_nt_seq,
        hvdxg.destroyalloc_d3d12_first_seq,
        hvdxg.destroyalloc_d3d12_first_context,
        hvdxg.destroyalloc_d3d12_first_match,
        hvdxg.destroyalloc_d3d12_first_pending,
        hvdxg.destroyalloc_d3d12_first_before_nt,
        hvdxg.destroyalloc_d3d12_last_before_nt,
        hvdxg.destroyalloc_d3d12_context_mask,
        hvdxg.allocation_last_cmd_len,
        hvdxg.allocation_last_hdr_size,
        hvdxg.allocation_last_prr_offset,
        hvdxg.allocation_last_make_resident_offset,
        hvdxg.allocation_last_allocinfo_offset,
        hvdxg.allocation_last_private_offset,
        hvdxg.allocation_last_result_min_len,
        hvdxg.allocation_last_result_len,
        hvdxg.allocation_last_wire_len,
        hvdxg.allocation_last_ext,
        hvdxg.allocation_last_ext_offset,
        hvdxg.allocation_last_route_global,
        hvdxg.allocation_last_send_ret,
        hvdxg.allocation_last_process,
        hvdxg.allocation_last_result_flags_offset,
        hvdxg.allocation_last_result_resource_offset,
        hvdxg.allocation_last_result_global_offset,
        hvdxg.allocation_last_result_vgpu_offset,
        hvdxg.allocation_last_result_allocinfo_offset,
        hvdxg.allocation_last_result_allocinfo_size,
        hvdxg.allocation_last_result_head_len,
        hvdxg.allocation_last_runtime_size,
        hvdxg.allocation_last_resource_priv_size,
        hvdxg.allocation_last_priv_size, hvdxg.allocation_last_flags,
        hvdxg.allocation_last_sysmem, hvdxg.allocation_last_priority,
        hvdxg.existing_sysmem_last_pages,
        hvdxg.existing_sysmem_last_pin_ret,
        hvdxg.existing_sysmem_last_set_ret,
        hvdxg.existing_sysmem_pin_successes,
        hvdxg.existing_sysmem_set_successes,
        hvdxg.existing_sysmem_total_pages,
        hvdxg.existing_sysmem_active_pages,
        hvdxg.existing_sysmem_pin_events,
        hvdxg.existing_sysmem_unpin_events,
        hvdxg.allocation_last_in_priv_head_len,
        hvdxg.allocation_last_in_priv_head[0],
        hvdxg.allocation_last_in_priv_head[1],
        hvdxg.allocation_last_in_priv_head[2],
        hvdxg.allocation_last_in_priv_head[3],
        hvdxg.allocation_last_in_priv_head[4],
        hvdxg.allocation_last_in_priv_head[5],
        hvdxg.allocation_last_in_priv_head[6],
        hvdxg.allocation_last_in_priv_head[7],
        hvdxg.allocation_last_out_priv_head_len,
        hvdxg.allocation_last_out_priv_head[0],
        hvdxg.allocation_last_out_priv_head[1],
        hvdxg.allocation_last_out_priv_head[2],
        hvdxg.allocation_last_out_priv_head[3],
        hvdxg.allocation_last_out_priv_head[4],
        hvdxg.allocation_last_out_priv_head[5],
        hvdxg.allocation_last_out_priv_head[6],
        hvdxg.allocation_last_out_priv_head[7],
        existing_sysmem_diag_attempts,
        existing_sysmem_diag_path,
        existing_sysmem_diag_standard,
        existing_sysmem_diag_writable,
        existing_sysmem_diag_device,
        existing_sysmem_diag_allocation,
        existing_sysmem_diag_va,
        existing_sysmem_diag_size,
        existing_sysmem_diag_pages,
        existing_sysmem_diag_first_pfn,
        existing_sysmem_diag_last_pfn,
        existing_sysmem_diag_pin_ret,
        existing_sysmem_diag_set_ret,
        existing_sysmem_diag_pin_ok,
        existing_sysmem_diag_set_ok,
        existing_sysmem_diag_total_pages,
        hvdxg.makeresident_last_len, hvdxg.makeresident_last_ret,
        hvdxg.makeresident_last_host_ret,
        hvdxg.makeresident_last_user_ret,
        (uint32)hvdxg.makeresident_last_status,
        hvdxg.makeresident_last_pending_ok,
        hvdxg.makeresident_last_fence,
        hvdxg.makeresident_last_fence_current,
        hvdxg.makeresident_last_sync, hvdxg.makeresident_last_trim,
        hvdxg.makeresident_last_device,
        hvdxg.makeresident_last_paging_queue,
        hvdxg.makeresident_last_flags,
        hvdxg.makeresident_last_count,
        hvdxg.makeresident_last_sorted,
        hvdxg.makeresident_last_in_alloc[0],
        hvdxg.makeresident_last_in_alloc[1],
        hvdxg.makeresident_last_in_alloc[2],
        hvdxg.makeresident_last_in_alloc[3],
        hvdxg.makeresident_last_wire_alloc[0],
        hvdxg.makeresident_last_wire_alloc[1],
        hvdxg.makeresident_last_wire_alloc[2],
        hvdxg.makeresident_last_wire_alloc[3],
        hvdxg.evict_last_len, hvdxg.evict_last_ret,
        hvdxg.evict_last_trim,
        hvdxg.makeresident_last_cmd_len,
        hvdxg.makeresident_last_wsl_cmd_len,
        hvdxg.makeresident_last_result_len,
        hvdxg.makeresident_last_actual_len,
        hvdxg.makeresident_last_owner_ok_count,
        hvdxg.makeresident_last_tracked_count,
        hvdxg.makeresident_last_order_matches,
        hvdxg.makeresident_last_in_alloc[0],
        hvdxg.makeresident_last_owner_dev[0],
        hvdxg.makeresident_last_owner_res[0],
        hvdxg.makeresident_last_owner_proc[0],
        hvdxg.makeresident_last_owner_gen[0],
        hvdxg.makeresident_last_owner_refs[0],
        hvdxg.makeresident_last_in_alloc[1],
        hvdxg.makeresident_last_owner_dev[1],
        hvdxg.makeresident_last_owner_res[1],
        hvdxg.makeresident_last_owner_proc[1],
        hvdxg.makeresident_last_owner_gen[1],
        hvdxg.makeresident_last_owner_refs[1],
        hvdxg.mapgpuva_last_len,
        hvdxg.mapgpuva_last_ret, (uint32)hvdxg.mapgpuva_last_status,
        hvdxg.mapgpuva_last_paging_queue, hvdxg.mapgpuva_last_allocation,
        hvdxg.mapgpuva_last_base, hvdxg.mapgpuva_last_min,
        hvdxg.mapgpuva_last_max, hvdxg.mapgpuva_last_size_pages,
        hvdxg.mapgpuva_last_protection,
        hvdxg.mapgpuva_last_driver_protection, hvdxg.mapgpuva_last_va,
        hvdxg.mapgpuva_last_fence, hvdxg.mapgpuva_last_fence_current,
        hvdxg.mapgpuva_last_sync, hvdxg.submit_last_len,
        hvdxg.submit_last_ret, (uint32)hvdxg.submit_last_status,
        hvdxg.submit_last_command_buffer,
        hvdxg.submit_last_command_length, hvdxg.submit_last_flags,
        hvdxg.submit_last_priv_size, hvdxg.submit_last_context_count,
        hvdxg.submit_last_context0,
        hvdxg.lock2_last_len, hvdxg.lock2_last_ret,
        (uint32)hvdxg.lock2_last_status, hvdxg.lock2_last_allocation,
        hvdxg.lock2_last_offset, hvdxg.lock2_last_user_va,
        hvdxg.unlock2_last_len, hvdxg.unlock2_last_ret,
        (uint32)hvdxg.unlock2_last_status,
        hvdxg.unlock2_last_allocation,
        hvdxg.allocation_history_index,
        HV_DXG_AH_ARGS(0), HV_DXG_AH_ARGS(1),
        HV_DXG_AH_ARGS(2), HV_DXG_AH_ARGS(3),
        HV_DXG_AH_ARGS(4), HV_DXG_AH_ARGS(5),
        HV_DXG_AH_ARGS(6), HV_DXG_AH_ARGS(7),
        hvdxg.allocation_history_global_share[0],
        hvdxg.allocation_history_create_flags[0],
        hvdxg.allocation_history_global_share[1],
        hvdxg.allocation_history_create_flags[1],
        hvdxg.allocation_history_global_share[2],
        hvdxg.allocation_history_create_flags[2],
        hvdxg.allocation_history_global_share[3],
        hvdxg.allocation_history_create_flags[3],
        hvdxg.allocation_history_global_share[4],
        hvdxg.allocation_history_create_flags[4],
        hvdxg.allocation_history_global_share[5],
        hvdxg.allocation_history_create_flags[5],
        hvdxg.allocation_history_global_share[6],
        hvdxg.allocation_history_create_flags[6],
        hvdxg.allocation_history_global_share[7],
        hvdxg.allocation_history_create_flags[7],
        hvdxg.mapgpuva_history_index,
        HV_DXG_MH_ARGS(0), HV_DXG_MH_ARGS(1),
        HV_DXG_MH_ARGS(2), HV_DXG_MH_ARGS(3),
        HV_DXG_MH_ARGS(4), HV_DXG_MH_ARGS(5),
        HV_DXG_MH_ARGS(6), HV_DXG_MH_ARGS(7),
        hvdxg.lock2_history_index,
        HV_DXG_LH_ARGS(0), HV_DXG_LH_ARGS(1),
        HV_DXG_LH_ARGS(2), HV_DXG_LH_ARGS(3),
        HV_DXG_LH_ARGS(4), HV_DXG_LH_ARGS(5),
        HV_DXG_LH_ARGS(6), HV_DXG_LH_ARGS(7),
	        hvdxg.queryadapter_last_type, hvdxg.queryadapter_last_size,
	        hvdxg.queryadapter_last_len, hvdxg.queryadapter_last_user_len,
	        hvdxg.queryadapter_last_ret,
        (uint32)hvdxg.queryadapter_last_status,
        hvdxg.queryadapter_last_layout,
        hvdxg.queryadapter_last_cmd_len,
        hvdxg.queryadapter_last_type_offset,
        hvdxg.queryadapter_last_size_offset,
        hvdxg.queryadapter_last_data_offset,
        hvdxg.queryadapter_last_adapter,
        hvdxg.queryadapter_last_host_adapter,
        hvdxg.queryadapter_last_resolve_source,
        hvdxg.queryadapter_last_owner_process,
        hvdxg.queryadapter_last_owner_generation,
        hvdxg.queryadapter_last_owner_refs,
        hvdxg.queryadapter_last_result_len,
        hvdxg.queryadapter_last_expected_wsl_len,
        hvdxg.queryadapter_last_process_source,
        hvdxg.queryadapter_last_adapter_object,
        hvdxg.queryadapter_last_adapter_object_host,
        hvdxg.queryadapter_last_adapter_object_owner,
        hvdxg.queryadapter_last_adapter_object_owner_generation,
        hvdxg.queryadapter_last_adapter_object_generation,
        hvdxg.queryadapter_local_namespace,
        hvdxg.queryadapter_process_adapter_refs,
        hvdxg.queryadapter_process_adapter_locals,
        hvdxg.queryadapter_process_adapter_generation,
        hvdxg.queryadapter_type15_fail_ret,
        (uint32)hvdxg.queryadapter_type15_fail_status,
        hvdxg.queryadapter_type15_fail_process,
        hvdxg.queryadapter_type15_fail_route,
        hvdxg.queryadapter_type15_fail_ext_luid_low,
        hvdxg.queryadapter_type15_fail_ext_luid_high,
        hvdxg.queryadapter_send_route,
        hvdxg.queryadapter_send_ext_luid_low,
        hvdxg.queryadapter_send_ext_luid_high,
        hvdxg.queryadapter_completion_desc_type,
        hvdxg.queryadapter_completion_desc_flags,
        hvdxg.queryadapter_completion_desc_len8,
        hvdxg.queryadapter_completion_desc_offset8,
        hvdxg.queryadapter_completion_packet_len,
        hvdxg.queryadapter_completion_packet_offset,
        hvdxg.queryadapter_completion_payload_len,
        hvdxg.queryadapter_completion_trans_id,
        hvdxg.queryadapter_completion_waiting_trans_id,
        hvdxg_channel_name(hvdxg.queryadapter_completion_source_channel),
        hvdxg.queryadapter_completion_source_relid,
        hvdxg_channel_name(hvdxg.queryadapter_completion_waiting_channel),
        hvdxg.queryadapter_completion_waiting_relid,
        hvdxg.queryadapter_completion_waiting_match,
        hvdxg.queryadapter_completion_waiting_channel_match,
        hvdxg.queryadapter_completion_type,
        hvdxg.queryadapter_completion_len,
        hvdxg.queryadapter_completion_prefix[0],
        hvdxg.queryadapter_completion_prefix[1],
        hvdxg.queryadapter_completion_prefix[2],
        hvdxg.queryadapter_completion_prefix[3],
        hvdxg.queryadapter_completion_prefix[4],
        hvdxg.queryadapter_completion_prefix[5],
        hvdxg.queryadapter_completion_prefix[6],
        hvdxg.queryadapter_completion_prefix[7],
	        hvdxg.queryadapter_zero_success_type,
	        hvdxg.queryadapter_zero_success_size,
	        hvdxg.queryadapter_zero_success_count,
	        hvdxg.queryadapter_zero_success_host_type,
	        hvdxg.queryadapter_zero_success_host_len,
	        hvdxg.queryadapter_zero_success_host_ret,
	        (uint32)hvdxg.queryadapter_zero_success_host_status,
	        hvdxg.queryadapter_zero_success_user_ret,
	        hvdxg.queryadapter_type0_private_size,
        hvdxg.queryadapter_type0_private_hash,
        hvdxg.queryadapter_type0_primary_len,
        hvdxg.queryadapter_type0_primary_ret,
        (uint32)hvdxg.queryadapter_type0_primary_status,
        hvdxg.queryadapter_type0_fallback_attempted,
        hvdxg.queryadapter_type0_fallback_used,
        hvdxg.queryadapter_type0_fallback_len,
        hvdxg.queryadapter_type0_fallback_ret,
        (uint32)hvdxg.queryadapter_type0_fallback_status,
        hvdxg.queryadapter_type0_result_route,
        hvdxg.queryadapter_type0_fallback_reason,
        hvdxg.queryadapter_type0_fallback_route,
        hvdxg.queryadapter_umd_rewrite_attempted,
        hvdxg.queryadapter_umd_rewrite_rewritten,
        hvdxg.queryadapter_umd_rewrite_path,
        hvdxg.queryadapter_umd_rewrite_vendor,
        hvdxg.queryadapter_umd_rewrite_original_hash,
        hvdxg.queryadapter_umd_rewrite_original0,
        hvdxg.queryadapter_umd_rewrite_original1,
        hvdxg.queryadapter_umd_rewrite_host_status,
        hvdxg.queryadapter_umd_rewrite_ret,
        hvdxg.queryadapter_type0_private_head_len,
        hvdxg.queryadapter_type0_private_head[0],
        hvdxg.queryadapter_type0_private_head[1],
        hvdxg.queryadapter_type0_private_head[2],
        hvdxg.queryadapter_type0_private_head[3],
        hvdxg.queryadapter_type0_private_head[4],
        hvdxg.queryadapter_type0_private_head[5],
        hvdxg.queryadapter_type0_private_head[6],
        hvdxg.queryadapter_type0_private_head[7],
        hvdxg.queryadapter_type0_private_head[8],
        hvdxg.queryadapter_type0_private_head[9],
        hvdxg.queryadapter_type0_private_head[10],
        hvdxg.queryadapter_type0_private_head[11],
        hvdxg.queryadapter_type0_private_head[12],
        hvdxg.queryadapter_type0_private_head[13],
        hvdxg.queryadapter_type0_private_head[14],
        hvdxg.queryadapter_type0_private_head[15],
        hvdxg.queryadapter_type0_private_head[16],
        hvdxg.queryadapter_type0_private_head[17],
        hvdxg.queryadapter_type0_private_head[18],
        hvdxg.queryadapter_type0_private_head[19],
        hvdxg.queryadapter_type0_private_head[20],
        hvdxg.queryadapter_type0_private_head[21],
        hvdxg.queryadapter_type0_private_head[22],
        hvdxg.queryadapter_type0_private_head[23],
        hvdxg.queryadapter_type0_private_head[24],
        hvdxg.queryadapter_type0_private_head[25],
        hvdxg.queryadapter_type0_private_head[26],
        hvdxg.queryadapter_type0_private_head[27],
        hvdxg.queryadapter_type0_private_head[28],
        hvdxg.queryadapter_type0_private_head[29],
        hvdxg.queryadapter_type0_private_head[30],
        hvdxg.queryadapter_type0_private_head[31],
        hvdxg.queryadapter_type0_private_tail_len,
        hvdxg.queryadapter_type0_private_tail[0],
        hvdxg.queryadapter_type0_private_tail[1],
        hvdxg.queryadapter_type0_private_tail[2],
        hvdxg.queryadapter_type0_private_tail[3],
        hvdxg.queryadapter_type0_private_tail[4],
        hvdxg.queryadapter_type0_private_tail[5],
        hvdxg.queryadapter_type0_private_tail[6],
        hvdxg.queryadapter_type0_private_tail[7],
        hvdxg.queryadapter_type0_private_tail[8],
        hvdxg.queryadapter_type0_private_tail[9],
        hvdxg.queryadapter_type0_private_tail[10],
        hvdxg.queryadapter_type0_private_tail[11],
        hvdxg.queryadapter_type0_private_tail[12],
        hvdxg.queryadapter_type0_private_tail[13],
        hvdxg.queryadapter_type0_private_tail[14],
        hvdxg.queryadapter_type0_private_tail[15],
        hvdxg.queryadapter_type0_private_tail[16],
        hvdxg.queryadapter_type0_private_tail[17],
        hvdxg.queryadapter_type0_private_tail[18],
        hvdxg.queryadapter_type0_private_tail[19],
        hvdxg.queryadapter_type0_private_tail[20],
        hvdxg.queryadapter_type0_private_tail[21],
        hvdxg.queryadapter_type0_private_tail[22],
        hvdxg.queryadapter_type0_private_tail[23],
        hvdxg.queryadapter_type0_private_tail[24],
        hvdxg.queryadapter_type0_private_tail[25],
        hvdxg.queryadapter_type0_private_tail[26],
        hvdxg.queryadapter_type0_private_tail[27],
        hvdxg.queryadapter_type0_private_tail[28],
        hvdxg.queryadapter_type0_private_tail[29],
        hvdxg.queryadapter_type0_private_tail[30],
        hvdxg.queryadapter_type0_private_tail[31],
        hvdxg.adapter_vendor_id, hvdxg.adapter_device_id,
        hvdxg.adapter_hardware_raw[0], hvdxg.adapter_hardware_raw[1],
        hvdxg.adapter_hardware_raw[2], hvdxg.adapter_hardware_raw[3],
        hvdxg.adapter_hardware_raw[4], hvdxg.adapter_hardware_raw[5],
        hvdxg.adapter_hardware_raw[6],
        hvdxg.adapter_hardware_raw_size, hvdxg.adapter_hardware_normalized,
        hvdxg.adapter_hardware_fallback,
        hvdxg.adapter_hardware_fallback_count,
        hvdxg.adapter_hardware_fallback_source,
        hvdxg_adapter_hardware_source_name(),
        hvdxg.adapter_hardware_synthetic_rejected,
        hvdxg.adapter_hardware_cache_available,
        hvdxg.adapter_hardware_payload_len,
        hvdxg.adapter_hardware_v40_short_zero,
        hvdxg.adapter_hardware_v40_short_len,
        hvdxg.adapter_hardware_v40_short_ret,
        (uint32)hvdxg.adapter_hardware_v40_short_status,
        hvdxg.adapter_hardware_cache_available,
        hvdxg_adapter_hardware_source_name(),
        hvdxg.adapter_hardware_synthetic_rejected,
        hvdxg.adapter_hardware_temp_v27_attempts,
        hvdxg.adapter_hardware_temp_v27_successes,
        hvdxg.adapter_hardware_temp_v27_failures,
        hvdxg.adapter_hardware_temp_v27_last_ret,
        (uint32)hvdxg.adapter_hardware_temp_v27_last_status,
        hvdxg.adapter_hardware_temp_v27_open_len,
        hvdxg.adapter_hardware_temp_v27_query_len,
        hvdxg.adapter_hardware_temp_v27_close_len,
        hvdxg.adapter_hardware_temp_v27_close_ret,
        (uint32)hvdxg.adapter_hardware_temp_v27_close_status,
        hvdxg.adapter_hardware_temp_v27_handle,
        hvdxg.adapter_hardware_temp_v27_restored_version,
        hvdxg.adapter_hardware_temp_v27_restored_ext,
        hvdxg.enumadapters_last_cmd, hvdxg.enumadapters_last_in_count,
        hvdxg.enumadapters_last_out_count,
        hvdxg.enumadapters_last_num_sources,
        hvdxg.enumadapters_last_buffer,
        hvdxg.enumadapters_last_handle,
        hvdxg.enumadapters_last_luid_high,
        hvdxg.enumadapters_last_luid_low,
        hvdxg.enumadapters_last_luid_source,
        hvdxg.enumadapters_last_ret,
        hvdxg.enumadapters_last_stage,
        hvdxg.enumadapters_last_ensure_ret,
        hvdxg.enumadapters_last_bind_ret,
        hvdxg.enumadapters_last_local_ret,
        hvdxg.enumadapters_last_copyout_ret,
        hvdxg.enumadapters_last_global_open,
        hvdxg.enumadapters_last_global_relid,
        hvdxg.enumadapters_last_global_conn,
        hvdxg.enumadapters_last_vgpu_open,
        hvdxg.enumadapters_last_vgpu_relid,
        hvdxg.enumadapters_last_vgpu_conn,
        hvdxg.enumadapters_last_host_adapter,
        hvdxg.enumadapters_last_probe_successes,
        hvdxg.enumadapters_last_probe_ret,
        (uint32)hvdxg.enumadapters_last_probe_status,
        hvdxg.enumadapters_last_probe_handle,
        hvdxg.enumadapters_last_ready,
        hvdxg.enumadapters_last_process,
        hvdxg.enumadapters_last_process_created,
        hvdxg.enumadapters_last_process_generation,
        hvdxg.enumadapters_last_process_refs,
        hvdxg.enumadapters_last_process_adapters,
        hvdxg.enumadapters_last_process_locals,
        hvdxg.enumadapters_last_process_objects,
        hvdxg.local_adapter_namespace_hits,
        hvdxg.local_adapter_namespace_misses,
        hvdxg.local_adapter_last_result,
        hvdxg.local_adapter_last_handle,
        hvdxg.local_adapter_last_host,
        hvdxg.local_adapter_last_refs,
        hvdxg.local_adapter_last_locals,
        hvdxg.local_adapter_last_generation,
        hvdxg.local_adapter_reuse_delayed,
        hvdxg.local_adapter_reuse_allowed,
        hvdxg.local_adapter_min_free_entries,
        hvdxg.queryadapter_history_index,
        HV_DXG_QH_ARGS(0), HV_DXG_QH_ARGS(1),
        HV_DXG_QH_ARGS(2), HV_DXG_QH_ARGS(3),
        HV_DXG_QH_ARGS(4), HV_DXG_QH_ARGS(5),
        HV_DXG_QH_ARGS(6), HV_DXG_QH_ARGS(7),
        HV_DXG_QH_ARGS(8), HV_DXG_QH_ARGS(9),
        HV_DXG_QH_ARGS(10), HV_DXG_QH_ARGS(11),
        HV_DXG_QH_ARGS(12), HV_DXG_QH_ARGS(13),
        HV_DXG_QH_ARGS(14), HV_DXG_QH_ARGS(15),
        hvdxg.queryadapter_admission_history_index,
        HV_DXG_QAI_ADMISSION_ARGS(0),
        HV_DXG_QAI_ADMISSION_ARGS(1),
        HV_DXG_QAI_ADMISSION_ARGS(2),
        HV_DXG_QAI_ADMISSION_ARGS(3),
        HV_DXG_QAI_ADMISSION_ARGS(4),
        HV_DXG_QAI_ADMISSION_ARGS(5),
        HV_DXG_QAI_ADMISSION_ARGS(6),
        HV_DXG_QAI_ADMISSION_ARGS(7),
        hvdxg.queryregistry_last_query_type,
        hvdxg.queryregistry_last_flags,
        hvdxg.queryregistry_last_value_type,
        hvdxg.queryregistry_last_phys,
        hvdxg.queryregistry_last_output_size,
        hvdxg.queryregistry_last_status,
        hvdxg.queryregistry_last_name,
        hvdxg.queryregistry_last_name0,
        hvdxg.queryregistry_last_name1,
        hvdxg.feature_last_id, hvdxg.feature_last_len,
        hvdxg.feature_last_ret, (uint32)hvdxg.feature_last_status,
        hvdxg.feature_last_result,
        hvdxg.schedprio_last_len, hvdxg.schedprio_last_ret,
        (uint32)hvdxg.schedprio_last_status,
        hvdxg.schedprio_last_context, hvdxg.schedprio_last_value,
        hvdxg.allocprio_last_len, hvdxg.allocprio_last_ret,
        (uint32)hvdxg.allocprio_last_status,
        hvdxg.allocprio_last_count,
        hvdxg.allocres_last_len, hvdxg.allocres_last_ret,
        (uint32)hvdxg.allocres_last_status,
        hvdxg.allocres_last_count, hvdxg.allocres_last_value,
        hvdxg.statistics_last_len, hvdxg.statistics_last_ret,
        (uint32)hvdxg.statistics_last_status,
        hvdxg.statistics_last_type,
        hvdxg.clockcalibration_last_len, hvdxg.clockcalibration_last_ret,
        (uint32)hvdxg.clockcalibration_last_status,
        hvdxg.clockcalibration_last_node,
        hvdxg.clockcalibration_last_gpu_frequency,
        hvdxg.clockcalibration_last_gpu_counter,
        hvdxg.clockcalibration_last_cpu_counter,
        hvdxg.stdalloc_last_len, hvdxg.stdalloc_last_ret,
        (uint32)hvdxg.stdalloc_last_status,
        hvdxg.stdalloc_last_alloc_size, hvdxg.stdalloc_last_res_size,
        hvdxg.escape_last_len, hvdxg.escape_last_ret,
        hvdxg.escape_last_type, hvdxg.escape_last_flags,
        hvdxg.escape_last_size,
        hvdxg.shareobject_last_len, hvdxg.shareobject_last_cmd_len,
        hvdxg.shareobject_last_device_offset,
        hvdxg.shareobject_last_object_offset,
        hvdxg.shareobject_last_wire_len,
        hvdxg.shareobject_last_ext,
        hvdxg.shareobject_last_ext_offset,
        hvdxg.shareobject_last_result_len,
        hvdxg.shareobject_last_head_len,
        hvdxg.shareobject_last_completion_type,
        hvdxg.shareobject_last_completion_len,
        hvdxg.shareobject_last_completion_prefix[0],
        hvdxg.shareobject_last_completion_prefix[1],
        hvdxg.shareobject_last_completion_prefix[2],
        hvdxg.shareobject_last_completion_prefix[3],
        hvdxg.shareobject_last_completion_prefix[4],
        hvdxg.shareobject_last_completion_prefix[5],
        hvdxg.shareobject_last_completion_prefix[6],
        hvdxg.shareobject_last_completion_prefix[7],
        hvdxg.shareobject_last_ret,
        (uint32)hvdxg.shareobject_last_status,
        hvdxg.shareobject_last_process,
        hvdxg.shareobject_last_device, hvdxg.shareobject_last_object,
        hvdxg.shareobject_last_reserved,
        hvdxg.shareobject_last_nt_handle,
        hvdxg.shareobject_diag_attempted,
        hvdxg.shareobject_diag_valid_nt,
        hvdxg.shareobject_diag_kind,
        hvdxg.shareobject_diag_reason,
        hvdxg.vgpu_send_last_command,
        hvdxg.vgpu_send_last_cmd_len,
        hvdxg.vgpu_send_last_wire_len,
        hvdxg.vgpu_send_last_ext,
        hvdxg.vgpu_send_last_ext_offset,
        hvdxg.vgpu_send_last_process,
        hvdxg.vgpu_send_last_channel,
        hvdxg.vgpu_send_last_luid.b,
        hvdxg.vgpu_send_last_luid.a,
        hvdxg.vgpu_send_last_route_global,
        hvdxg.vgpu_send_last_retries,
        hvdxg.vgpu_send_last_ret,
        hvdxg.global_send_last_command,
        hvdxg.global_send_last_cmd_len,
        hvdxg.global_send_last_wire_len,
        hvdxg.global_send_last_ext,
        hvdxg.global_send_last_ext_offset,
        hvdxg.global_send_last_process,
        hvdxg.global_send_last_channel,
        hvdxg.global_send_last_luid.b,
        hvdxg.global_send_last_luid.a,
        hvdxg.global_send_last_retries,
        hvdxg.global_send_last_ret,
        hvdxg.global_send_ntshared.cmd_len,
        hvdxg.global_send_ntshared.wire_len,
        hvdxg.global_send_ntshared.ext_offset,
        hvdxg.global_send_ntshared.process,
        hvdxg.global_send_shareobject.cmd_len,
        hvdxg.global_send_shareobject.wire_len,
        hvdxg.global_send_shareobject.ext_offset,
        hvdxg.global_send_shareobject.process,
        hvdxg.global_send_destroynt.cmd_len,
        hvdxg.global_send_destroynt.wire_len,
        hvdxg.global_send_destroynt.ext_offset,
        hvdxg.global_send_destroynt.process,
        hvdxg.global_send_destroysync.cmd_len,
        hvdxg.global_send_destroysync.wire_len,
        hvdxg.global_send_destroysync.ext_offset,
        hvdxg.global_send_destroysync.process,
        hvdxg.global_send_ntshared.command_id,
        hvdxg.global_send_ntshared.command,
        hvdxg.global_send_ntshared.channel,
        hvdxg.global_send_ntshared.process,
        hvdxg.global_send_ntshared.cmd_len,
        hvdxg.global_send_ntshared.wire_len,
        hvdxg.global_send_ntshared.result_len,
        hvdxg.global_send_ntshared.ext,
        hvdxg.global_send_ntshared.ext_offset,
        hvdxg.global_send_ntshared.relid,
        hvdxg.global_send_ntshared.conn_id,
        hvdxg.global_send_ntshared.monitor_allocated,
        hvdxg.global_send_ntshared.monitorid,
        hvdxg.global_send_ntshared.dedicated,
        hvdxg.global_send_ntshared.luid.b,
        hvdxg.global_send_ntshared.luid.a,
        hvdxg.global_send_ntshared_ext.command_id,
        hvdxg.global_send_ntshared_ext.command,
        hvdxg.global_send_ntshared_ext.channel,
        hvdxg.global_send_ntshared_ext.process,
        hvdxg.global_send_ntshared_ext.cmd_len,
        hvdxg.global_send_ntshared_ext.wire_len,
        hvdxg.global_send_ntshared_ext.result_len,
        hvdxg.global_send_ntshared_ext.ext,
        hvdxg.global_send_ntshared_ext.ext_offset,
        hvdxg.global_send_ntshared_ext.relid,
        hvdxg.global_send_ntshared_ext.conn_id,
        hvdxg.global_send_ntshared_ext.monitor_allocated,
        hvdxg.global_send_ntshared_ext.monitorid,
        hvdxg.global_send_ntshared_ext.dedicated,
        hvdxg.global_send_ntshared_ext.luid.b,
        hvdxg.global_send_ntshared_ext.luid.a,
        hvdxg.global_send_shareobject.command_id,
        hvdxg.global_send_shareobject.command,
        hvdxg.global_send_shareobject.channel,
        hvdxg.global_send_shareobject.process,
        hvdxg.global_send_shareobject.cmd_len,
        hvdxg.global_send_shareobject.wire_len,
        hvdxg.global_send_shareobject.result_len,
        hvdxg.global_send_shareobject.ext,
        hvdxg.global_send_shareobject.ext_offset,
        hvdxg.global_send_shareobject.relid,
        hvdxg.global_send_shareobject.conn_id,
        hvdxg.global_send_shareobject.monitor_allocated,
        hvdxg.global_send_shareobject.monitorid,
        hvdxg.global_send_shareobject.dedicated,
        hvdxg.ntshared_last_create_len, hvdxg.ntshared_last_create_cmd_len,
        hvdxg.ntshared_last_create_object_offset,
        hvdxg.ntshared_last_create_result_len,
        hvdxg.ntshared_last_create_completion_type,
        hvdxg.ntshared_last_create_completion_len,
        hvdxg.ntshared_last_create_completion_prefix[0],
        hvdxg.ntshared_last_create_completion_prefix[1],
        hvdxg.ntshared_last_create_completion_prefix[2],
        hvdxg.ntshared_last_create_completion_prefix[3],
        hvdxg.ntshared_last_create_completion_prefix[4],
        hvdxg.ntshared_last_create_completion_prefix[5],
        hvdxg.ntshared_last_create_completion_prefix[6],
        hvdxg.ntshared_last_create_completion_prefix[7],
        hvdxg.ntshared_last_create_ret,
        hvdxg.ntshared_last_create_process,
        hvdxg.ntshared_last_create_type,
        hvdxg.ntshared_last_create_channel,
        hvdxg.ntshared_last_create_object,
        hvdxg.ntshared_last_create_handle,
        hvdxg.ntshared_last_create_raw0,
        hvdxg.ntshared_last_create_zero_len,
        hvdxg.ntshared_last_create_zero_len,
        (uint32)(hvdxg.ntshared_last_create_zero_len &&
                 hvdxg.ntshared_last_create_ret != 0 &&
                 hvdxg.ntshared_last_create_share_fallback == 0 ? 1 : 0),
        hvdxg.ntshared_last_create_side_effect,
        hvdxg.ntshared_last_create_share_fallback,
        hvdxg.ntshared_last_create_share_valid,
        hvdxg.ntshared_last_destroy_len, hvdxg.ntshared_last_destroy_ret,
        hvdxg.ntshared_last_destroy_handle,
        hvdxg.allocation_last_process,
        hvdxg.sharedhandle_last_allocation_owner_process != 0 ?
            hvdxg.sharedhandle_last_allocation_owner_process :
            hvdxg.allocation_last_owner_process,
        hvdxg.sharedhandle_last_allocation_owner_generation != 0 ?
            hvdxg.sharedhandle_last_allocation_owner_generation :
            hvdxg.allocation_last_owner_generation,
        hvdxg.syncobject_last_process,
        hvdxg.syncobject_last_owner_process,
        hvdxg.syncobject_last_owner_generation,
        hvdxg.ntshared_last_create_process,
        hvdxg.ntshared_last_create_object,
        hvdxg.allocation_last_device,
        hvdxg.allocation_last_device_known,
        hvdxg.allocation_last_device_from_create,
        hvdxg.host_adapter_handle,
        hvdxg.ntshared_cache_hits, hvdxg.ntshared_cache_misses,
        hvdxg.ntshared_cache_inserts, hvdxg.ntshared_cache_releases,
        hvdxg.ntshared_cache_destroys, hvdxg.ntshared_cache_full,
        hvdxg.ntshared_cache_last_kind,
        hvdxg.ntshared_cache_last_process,
        hvdxg.ntshared_cache_last_object,
        hvdxg.ntshared_cache_last_handle,
        hvdxg.ntshared_cache_last_refs,
        hvdxg.sharedhandle_last_cmd, hvdxg.sharedhandle_last_ret,
        hvdxg.sharedhandle_last_device, hvdxg.sharedhandle_last_object,
        hvdxg.sharedhandle_last_nt_handle, hvdxg.sharedhandle_last_count,
        hvdxg.sharedhandle_last_global_share,
        hvdxg.sharedhandle_last_runtime_d3d12_flags,
        hvdxg.sharedhandle_last_fops_kind,
        hvdxg.sharedhandle_last_current_process,
        hvdxg.sharedhandle_last_current_generation,
        hvdxg.sharedhandle_last_creator_process,
        hvdxg.sharedhandle_last_creator_generation,
        hvdxg.sharedhandle_last_owner_process,
        hvdxg.sharedhandle_last_owner_generation,
        hvdxg.sharedhandle_last_owner_used,
        hvdxg.sharedhandle_last_raw_device,
        hvdxg.sharedhandle_last_raw_object,
        hvdxg.sharedhandle_last_host_device,
        hvdxg.sharedhandle_last_host_object,
        hvdxg.sharedhandle_last_host_device_found,
        hvdxg.sharedhandle_last_object_found,
        hvdxg.sharedhandle_last_kind,
        hvdxg.sharedhandle_last_object_type,
        hvdxg.sharedhandle_last_create_flags,
        hvdxg.sharedhandle_last_alloc_count,
        hvdxg.sharedhandle_last_raw_object,
        hvdxg.sharedhandle_last_raw_resource_found,
        hvdxg.sharedhandle_last_raw_allocation_found,
        hvdxg.sharedhandle_last_raw_sync_found,
        hvdxg.sharedhandle_last_kind,
        hvdxg.sharedhandle_last_object_type,
        hvdxg.sharedhandle_last_object,
        hvdxg.sharedhandle_last_kind, hvdxg.sharedhandle_last_host_object,
        hvdxg.sharedhandle_last_object_found, hvdxg.sharedhandle_last_parent,
        hvdxg.sharedhandle_last_current_process,
        hvdxg.sharedhandle_last_current_generation,
        hvdxg.sharedhandle_last_owner_process,
        hvdxg.sharedhandle_last_owner_generation,
        hvdxg.sharedhandle_last_owner_refs,
        hvdxg.sharedhandle_last_owner_used,
        hvdxg.sharedhandle_last_object_type,
        hvdxg.sharedhandle_last_object_device,
        hvdxg.sharedhandle_last_allocation,
        hvdxg.sharedhandle_last_allocation_found,
        hvdxg.sharedhandle_last_allocation_owner_process,
        hvdxg.sharedhandle_last_allocation_owner_generation,
        hvdxg.sharedhandle_last_allocation_owner_refs,
        hvdxg.sharedhandle_last_create_flags,
        hvdxg.sharedhandle_last_alloc_count,
        hvdxg.sharedhandle_last_sealed,
        hvdxg.sharedhandle_last_sync_type,
        hvdxg.sharedhandle_last_sync_flags,
        hvdxg.sharedhandle_last_sync_global,
        hvdxg.sharedhandle_last_sync_monitor_fence,
        hvdxg.sharedhandle_last_sync_fence_cpu,
        hvdxg.sharedhandle_last_sync_fence_kva,
        hvdxg.sharedsync_export_fd,
        hvdxg.sharedsync_export_ret,
        hvdxg.sharedsync_export_cloexec,
        hvdxg.sharedsync_export_fops_kind,
        hvdxg.sharedsync_export_fd_kind,
        hvdxg.sharedsync_export_sync_type,
        hvdxg.sharedsync_export_sync_flags,
        hvdxg.sharedsync_export_monitor_fence,
        hvdxg.sharedsync_export_global_share,
        hvdxg.sharedsync_export_global_zero,
        hvdxg.sharedsync_export_host_shared_handle,
        hvdxg.sharedsync_export_host_nt_handle,
        hvdxg.sharedsync_export_nt_refs,
        hvdxg.sharedsync_export_shared_owner_object,
        hvdxg.sharedsync_export_device,
        hvdxg.sharedsync_export_host_device,
        hvdxg.sharedsync_export_object,
        hvdxg.sharedsync_export_host_object,
        hvdxg.sharedsync_export_cache_process,
        hvdxg.sharedsync_export_owner_process,
        hvdxg.sharedsync_export_owner_generation,
        hvdxg.sharedsync_export_owner_refs,
        HV_DXG_SHARED_FOPS_SYNC,
        HV_DXG_SHARED_FOPS_RESOURCE,
        hvdxg.sharedsync_export_fops_kind,
        hvdxg.sharedsync_export_fd_kind,
        0U,
        1U,
        1U,
        0U,
        S_IFREG | 0600,
        0x64786701U,
        0x64786702U,
        hvdxg.shareobjects_last_desired_access,
        hvdxg.shareobjects_last_object_attr,
        hvdxg.shareobjects_last_attr_len,
        hvdxg.shareobjects_last_attr_ret,
        hvdxg.shareobjects_last_attr_head[0],
        hvdxg.shareobjects_last_attr_head[1],
        hvdxg.shareobjects_last_attr_head[2],
        hvdxg.shareobjects_last_attr_head[3],
        hvdxg.shareobjects_last_attr_head[4],
        hvdxg.shareobjects_last_attr_head[5],
        hvdxg.shareobjects_last_attr_head[6],
        hvdxg.shareobjects_last_attr_head[7],
        hvdxg.sharedhandle_last_map_va,
        hvdxg.sharedhandle_last_map_pages,
        hvdxg.sharedhandle_last_map_fence,
        hvdxg.sharedhandle_last_map_ret,
        hvdxg.sharedhandle_last_map_status,
        hvdxg.sharedhandle_last_resident_paging_queue,
        hvdxg.sharedhandle_last_resident_sync,
        hvdxg.sharedhandle_last_resident_fence,
        hvdxg.sharedhandle_last_resident_current,
        hvdxg.sharedhandle_last_resident_wait_result,
        hvdxg.sharedhandle_last_resident_wait_ret,
        hvdxg.sharedhandle_last_resident_missing,
        hvdxg.sharedhandle_last_resident_enforced,
        hvdxg.sharedresource_created,
        hvdxg.sharedresource_seals, hvdxg.sharedresource_seal_reuses,
        hvdxg.sharedresource_seal_denied,
        hvdxg.sharedresource_seal_allocs,
        hvdxg.sharedresource_seal_private,
        hvdxg.sharedresource_open_tracked,
        hvdxg.sharedresource_owner_exists,
        hvdxg.sharedresource_owner_cached,
        hvdxg.sharedresource_owner_reused,
        hvdxg.sharedresource_owner_nt,
        hvdxg.sharedresource_owner_refs,
        hvdxg.sharedresource_owner_sealed,
        hvdxg.sharedresource_owner_object,
        hvdxg.sharedresource_owner_process,
        hvdxg.sharedresource_open_global,
        hvdxg.sharedresource_pre_nt_sealable,
        hvdxg.sharedresource_pre_nt_seal_ret,
        hvdxg.sharedresource_meta_track_host,
        hvdxg.sharedresource_meta_runtime_len,
        hvdxg.sharedresource_meta_runtime_hash,
        hvdxg.sharedresource_meta_resource_len,
        hvdxg.sharedresource_meta_resource_hash,
        hvdxg.sharedresource_meta_total_len,
        hvdxg.sharedresource_meta_total_hash,
        hvdxg.sharedresource_meta_alloc0_priv,
        hvdxg.sharedresource_meta_match_in,
        hvdxg.sharedresource_meta_match_out,
        hvdxg.sharedresource_meta_total_w4,
        hvdxg.sharedresource_meta_total_w8,
        hvdxg.sharedresource_meta_logical_flags,
        hvdxg.sharedresource_meta_host_result_flags,
        hvdxg.sharedresource_meta_host_flags_ignored,
        hvdxg.sharedresource_pre_nt_seal_applied,
        hvdxg.sharedresource_pre_nt_seal_before,
        hvdxg.sharedresource_pre_nt_seal_after,
        hvdxg.sharedresource_pre_nt_seal_actual_ret,
        hvdxg.sharedresource_record_valid,
        hvdxg.sharedresource_record_stage,
        hvdxg.sharedresource_record_key_kind,
        hvdxg.sharedresource_record_key_process,
        hvdxg.sharedresource_record_key_object,
        hvdxg.sharedresource_record_key_global,
        hvdxg.sharedresource_record_key_nt,
        hvdxg.sharedresource_record_source_process,
        hvdxg.sharedresource_record_source_tgid,
        hvdxg.sharedresource_record_source_generation,
        hvdxg.sharedresource_record_device,
        hvdxg.sharedresource_record_resource,
        hvdxg.sharedresource_record_allocation,
        hvdxg.sharedresource_record_adapter_high,
        hvdxg.sharedresource_record_adapter_low,
        hvdxg.sharedresource_record_host_adapter_high,
        hvdxg.sharedresource_record_host_adapter_low,
        hvdxg.sharedresource_record_sealed,
        hvdxg.sharedresource_record_sealed_generation,
        hvdxg.sharedresource_record_seal_before_fd,
        hvdxg.sharedresource_record_alloc_count,
        hvdxg.sharedresource_record_runtime_size,
        hvdxg.sharedresource_record_resource_size,
        hvdxg.sharedresource_record_total_size,
        hvdxg.sharedresource_record_alloc0_priv,
        hvdxg.sharedresource_record_runtime_hash,
        hvdxg.sharedresource_record_resource_hash,
        hvdxg.sharedresource_record_total_hash,
        hvdxg.sharedresource_record_alloc0_hash,
        hvdxg.sharedresource_record_nt_refs,
        hvdxg.sharedresource_record_query_count,
        hvdxg.sharedresource_record_open_count,
        hvdxg.sharedresource_record_fd_publish_count,
        hvdxg.sharedresource_record_local_admit_ret,
        hvdxg.sharedresource_record_local_exact,
        hvdxg.sharedresource_record_mutation_after_seal,
        hvdxg.sharedresource_model_valid,
        hvdxg.sharedresource_model_flat_match,
        hvdxg.sharedresource_model_alloc_count,
        hvdxg.sharedresource_model_alloc0,
        hvdxg.sharedresource_model_alloc0_priv,
        hvdxg.sharedresource_model_alloc0_size,
        hvdxg.sharedresource_model_alloc0_flags,
        hvdxg.sharedresource_model_runtime_size,
        hvdxg.sharedresource_model_resource_size,
        hvdxg.sharedresource_model_total_size,
        hvdxg.sharedresource_model_sealed,
        hvdxg.sharedresource_model_generation,
        hvdxg.queryresource_last_seen,
        hvdxg.queryresource_last_ret,
        hvdxg.queryresource_last_device,
        hvdxg.queryresource_last_nt,
        hvdxg.queryresource_last_fd_kind,
        hvdxg.queryresource_last_fops_kind,
        hvdxg.queryresource_last_sync_probe,
        hvdxg.queryresource_last_global,
        hvdxg.queryresource_last_host_nt,
        hvdxg.queryresource_last_refs,
        hvdxg.queryresource_last_object,
        hvdxg.queryresource_last_cache_object,
        hvdxg.queryresource_last_alloc_count,
        hvdxg.queryresource_last_runtime_size,
        hvdxg.queryresource_last_resource_size,
        hvdxg.queryresource_last_total_size,
        hvdxg.openresource_last_route_global,
        hvdxg.openresource_last_cmd_len,
        hvdxg.openresource_last_wire_len,
        hvdxg.openresource_last_ext,
        hvdxg.openresource_last_ext_offset,
        hvdxg.openresource_last_result_len,
        hvdxg.openresource_last_actual_len,
        hvdxg.openresource_last_ret,
        (uint32)hvdxg.openresource_last_status,
        hvdxg.openresource_last_process,
        hvdxg.openresource_last_device,
        hvdxg.openresource_last_global,
        hvdxg.openresource_last_alloc_count,
        hvdxg.openresource_last_total_priv,
        hvdxg.openresource_last_result_resource,
        hvdxg.openresource_last_result_alloc0,
        hvdxg.openresource_last_seal_before,
        hvdxg.openresource_last_seal_after,
        hvdxg.openresource_last_fd_kind,
        hvdxg.openresource_last_fops_kind,
        hvdxg.openresource_last_fd_refs,
        hvdxg.opensync_last_cmd_len,
        hvdxg.opensync_last_wire_len,
        hvdxg.opensync_last_ext,
        hvdxg.opensync_last_ext_offset,
        hvdxg.opensync_last_result_len,
        hvdxg.opensync_last_actual_len,
        hvdxg.opensync_last_ret,
        (uint32)hvdxg.opensync_last_status,
        hvdxg.opensync_last_process,
        hvdxg.opensync_last_device,
        hvdxg.opensync_last_global,
        hvdxg.opensync_last_flags,
        hvdxg.opensync_last_result_sync,
        hvdxg.opensync_last_gpu_va,
        hvdxg.opensync_last_cpu_pa,
        hvdxg.opensync_last_fd_kind,
        hvdxg.opensync_last_fd_refs,
        hvdxg.opensync_last_fops_kind,
        HV_DXG_SHARED_FOPS_SYNC,
        hvdxg.opensync_last_sync_type,
        hvdxg.opensync_last_flags,
        hvdxg.opensync_last_wire_flags,
        hvdxg.opensync_last_forced_flags,
        HV_DXG_SHARED_FOPS_SYNC,
        HV_DXG_SHARED_FOPS_RESOURCE,
        0U,
        1U,
        (uint32)offsetof(struct hvdxg_command_opensyncobject, device),
        (uint32)offsetof(struct hvdxg_command_opensyncobject,
                         global_sync_object),
        (uint32)offsetof(struct hvdxg_command_opensyncobject, flags),
        hvdxg.opensync_last_device,
        hvdxg.opensync_last_device_host,
        hvdxg.opensync_last_device_owner,
        hvdxg.opensync_last_device_owner_generation,
        hvdxg.opensync_last_device_generation,
        hvdxg.opensync_last_source_device,
        hvdxg.opensync_last_source_device_host,
        hvdxg.opensync_last_source_owner,
        hvdxg.opensync_last_source_owner_generation,
        hvdxg.opensync_last_same_device,
        hvdxg.opensync_last_adapter_match,
        hvdxg.opensync_last_adapter_high,
        hvdxg.opensync_last_adapter_low,
        hvdxg.opensync_last_host_adapter_high,
        hvdxg.opensync_last_host_adapter_low,
        hvdxg.opensync_last_source_flags,
        hvdxg.opensync_last_sync_type,
        hvdxg_sync_type_is_monitored(hvdxg.opensync_last_sync_type),
        hvdxg.opensync_last_global,
        hvdxg.opensync_last_host_nt,
        (uint32)hvdxg.opensync_last_status,
        hvdxg.opensync_last_input_nt,
        hvdxg.opensync_last_host_nt,
        hvdxg.opensync_last_global,
        hvdxg.opensync_last_object,
        hvdxg.opensync_last_cache_object,
        hvdxg.opensync_last_fd_kind,
        hvdxg.opensync_last_fops_kind,
        hvdxg.opensync_last_fd_refs,
        hvdxg.opensync_last_global != 0 ? 1U : 0U,
        hvdxg.opensync_last_host_nt != 0 &&
            hvdxg.opensync_last_global != hvdxg.opensync_last_host_nt,
        hvdxg.opensync_ioctl_count,
        hvdxg.opensync_last_gate,
        hvdxg.sharedhandle_last_cmd,
        hvdxg.sharedhandle_last_cmd & 0xffU,
        hvdxg.opensync_last_input_nt,
        hvdxg.opensync_last_host_nt,
        hvdxg.opensync_last_fd_kind,
        hvdxg.opensync_last_global,
        hvdxg.opensync_last_object,
        hvdxg.opensync_last_cache_object,
        hvdxg.opensync_last_ret,
        hvdxg.sharedclose_last_kind,
        hvdxg.sharedclose_last_fops_kind,
        hvdxg.sharedclose_last_process,
        hvdxg.sharedclose_last_object,
        hvdxg.sharedclose_last_cache_object,
        hvdxg.sharedclose_last_nt_handle,
        hvdxg.sharedclose_last_host_shared_handle,
        hvdxg.sharedclose_last_global,
        hvdxg.sharedclose_last_refs_before,
        hvdxg.sharedclose_last_refs_after,
        hvdxg.sharedclose_last_destroy_handle,
        hvdxg.sharedclose_last_destroy_ret,
        hvdxg.sharedclose_last_destroy_status,
        hvdxg.sharedclose_last_destroy_actual_len,
        hvdxg.sharedclose_last_destroy_cmd_len,
        hvdxg.sharedclose_last_destroy_wire_len,
        hvdxg.sharedclose_last_destroy_ext,
        hvdxg.sharedclose_last_destroy_ext_offset,
        hvdxg.sharedclose_last_destroy_result_len,
        hvdxg.sharedclose_last_destroy_handle_offset,
        hvdxg.sharedclose_kind_seen[HV_DXG_SHARED_OBJECT_SYNC],
        hvdxg.sharedclose_kind_fops[HV_DXG_SHARED_OBJECT_SYNC],
        hvdxg.sharedclose_kind_refs_before[HV_DXG_SHARED_OBJECT_SYNC],
        hvdxg.sharedclose_kind_refs_after[HV_DXG_SHARED_OBJECT_SYNC],
        hvdxg.sharedclose_kind_destroy_handle[HV_DXG_SHARED_OBJECT_SYNC],
        hvdxg.sharedclose_kind_destroy_ret[HV_DXG_SHARED_OBJECT_SYNC],
        hvdxg.sharedclose_kind_destroy_status[HV_DXG_SHARED_OBJECT_SYNC],
        hvdxg.sharedclose_kind_destroy_actual_len[HV_DXG_SHARED_OBJECT_SYNC],
        hvdxg.sharedclose_kind_destroy_cmd_len[HV_DXG_SHARED_OBJECT_SYNC],
        hvdxg.sharedclose_kind_destroy_wire_len[HV_DXG_SHARED_OBJECT_SYNC],
        hvdxg.sharedclose_kind_destroy_ext[HV_DXG_SHARED_OBJECT_SYNC],
        hvdxg.sharedclose_kind_destroy_result_len[HV_DXG_SHARED_OBJECT_SYNC],
        hvdxg.sharedclose_kind_seen[HV_DXG_SHARED_OBJECT_RESOURCE],
        hvdxg.sharedclose_kind_fops[HV_DXG_SHARED_OBJECT_RESOURCE],
        hvdxg.sharedclose_kind_refs_before[HV_DXG_SHARED_OBJECT_RESOURCE],
        hvdxg.sharedclose_kind_refs_after[HV_DXG_SHARED_OBJECT_RESOURCE],
        hvdxg.sharedclose_kind_destroy_handle[HV_DXG_SHARED_OBJECT_RESOURCE],
        hvdxg.sharedclose_kind_destroy_ret[HV_DXG_SHARED_OBJECT_RESOURCE],
        hvdxg.sharedclose_kind_destroy_status[HV_DXG_SHARED_OBJECT_RESOURCE],
        hvdxg.sharedclose_kind_destroy_actual_len[HV_DXG_SHARED_OBJECT_RESOURCE],
        hvdxg.sharedclose_kind_destroy_cmd_len[HV_DXG_SHARED_OBJECT_RESOURCE],
        hvdxg.sharedclose_kind_destroy_wire_len[HV_DXG_SHARED_OBJECT_RESOURCE],
        hvdxg.sharedclose_kind_destroy_ext[HV_DXG_SHARED_OBJECT_RESOURCE],
        hvdxg.sharedclose_kind_destroy_result_len[HV_DXG_SHARED_OBJECT_RESOURCE],
        hvdxg.unsupported_last_cmd, hvdxg.unsupported_last_ret,
        hvdxg.unsupported_last_device, hvdxg.unsupported_last_handle,
        hvdxg.unsupported_last_count,
        hvdxg.unsupported_last_nr,
        hvdxg.unsupported_last_size,
        hvdxg.unsupported_last_name,
        hvdxg.markdevice_last_len,
        hvdxg.markdevice_last_ret,
        (uint32)hvdxg.markdevice_last_status,
        hvdxg.markdevice_last_device,
        hvdxg.markdevice_last_reason,
        hvdxg.markdevice_last_process,
        hvdxg.markdevice_last_cmd_len,
        hvdxg.syncfile_last_cmd, hvdxg.syncfile_last_ret,
        hvdxg.syncfile_last_device, hvdxg.syncfile_last_object,
        hvdxg.syncfile_last_context, hvdxg.syncfile_last_handle,
        hvdxg.syncfile_last_fence,
        hvdxg.syncfile_last_global,
        hvdxg.syncfile_last_host_nt,
        hvdxg.syncfile_last_source_flags,
        hvdxg.syncfile_last_open_flags,
        hvdxg.syncfile_last_len,
        (uint32)hvdxg.syncfile_last_status,
        hvdxg.syncfile_last_out_sync,
        hvdxg.syncfile_last_cpu_va,
        hvdxg.syncfile_last_gpu_va,
        hvdxg.syncfile_live_count,
        hvdxg.syncfile_create_count,
        hvdxg.syncfile_release_count,
        hvdxg.syncfile_release_event_removed,
        hvdxg.syncfile_release_nt_released,
        hvdxg.syncfile_create_copyout_failures,
        hvdxg.syncfile_create_copyout_fd_reclaimed,
        hvdxg.syncfile_open_copyout_failures,
        hvdxg.syncfile_open_unwind_destroy_attempts,
        hvdxg.syncfile_open_unwind_destroy_successes,
        hvdxg.syncfile_open_unwind_destroy_ret,
        hvdxg.host_event_active_count,
        hvdxg.host_event_alloc_count,
        hvdxg.host_event_remove_count,
        hvdxg.host_event_last_id,
        hvdxg.host_event_last_removed_id,
        hvdxg.updateallocproperty_last_len,
        hvdxg.updateallocproperty_last_ret,
        (uint32)hvdxg.updateallocproperty_last_status,
        hvdxg.updateallocproperty_last_allocation,
        hvdxg.updateallocproperty_last_fence,
        hvdxg.vidmem_reservation_last_len,
        hvdxg.vidmem_reservation_last_ret,
        (uint32)hvdxg.vidmem_reservation_last_status,
        hvdxg.vidmem_reservation_last_group,
        hvdxg.vidmem_reservation_last_value,
        hvdxg.offer_last_len, hvdxg.offer_last_ret,
        (uint32)hvdxg.offer_last_status, hvdxg.offer_last_count,
        hvdxg.reclaim_last_len, hvdxg.reclaim_last_ret,
        (uint32)hvdxg.reclaim_last_status, hvdxg.reclaim_last_count,
        hvdxg.reclaim_last_result0, hvdxg.reclaim_last_fence,
        hvdxg.updategpuva_last_len, hvdxg.updategpuva_last_ret,
        (uint32)hvdxg.updategpuva_last_status,
        hvdxg.updategpuva_last_ops, hvdxg.updategpuva_last_fence,
        hvdxg.updategpuva_last_device,
        hvdxg.updategpuva_last_context,
        hvdxg.updategpuva_last_fence_object,
        hvdxg.updategpuva_last_flags,
        hvdxg.updategpuva_last_cmd_len,
        hvdxg.updategpuva_last_op_offset,
        hvdxg.updategpuva_last_op_size,
        hvdxg.updategpuva_last_op0_type,
        hvdxg.updategpuva_last_op0_base,
        hvdxg.updategpuva_last_op0_size,
        hvdxg.updategpuva_last_op0_allocation,
        hvdxg.updategpuva_last_op0_alloc_offset,
        hvdxg.updategpuva_last_op0_alloc_size,
        hvdxg.updategpuva_last_op0_source,
        hvdxg.updategpuva_last_op0_dest,
        hvdxg.updategpuva_last_op0_protection,
        hvdxg.updategpuva_last_op0_driver_protection,
        hvdxg.cacheops_last_len, hvdxg.cacheops_last_ret,
        (uint32)hvdxg.cacheops_last_status,
        hvdxg.cacheops_last_allocation,
        hvdxg.createcontext_last_len,
        hvdxg.createcontext_last_ret, hvdxg.createcontext_last_handle,
        hvdxg.createcontext_last_device, hvdxg.createcontext_last_node,
        hvdxg.createcontext_last_engine, hvdxg.createcontext_last_flags,
        hvdxg.createcontext_last_hint, hvdxg.createcontext_last_priv_size,
        hvdxg.createcontext_fail_len, hvdxg.createcontext_fail_ret,
        (uint32)hvdxg.createcontext_fail_status,
        hvdxg.createcontext_last_priv_head_len,
        hvdxg.createcontext_last_priv_head[0],
        hvdxg.createcontext_last_priv_head[1],
        hvdxg.createcontext_last_priv_head[2],
        hvdxg.createcontext_last_priv_head[3],
        hvdxg.createcontext_last_priv_head[4],
        hvdxg.createcontext_last_priv_head[5],
        hvdxg.createcontext_last_priv_head[6],
        hvdxg.createcontext_last_priv_head[7],
        hvdxg.destroydevice_last_len, hvdxg.destroydevice_last_ret,
        (uint32)hvdxg.destroydevice_last_status,
        hvdxg.destroycontext_last_len, hvdxg.destroycontext_last_ret,
        (uint32)hvdxg.destroycontext_last_status,
        hvdxg.destroypaging_last_len, hvdxg.destroypaging_last_ret,
        (uint32)hvdxg.destroypaging_last_status,
        hvdxg.destroysync_last_len, hvdxg.destroysync_last_ret,
        (uint32)hvdxg.destroysync_last_status,
        hvdxg.destroysync_last_handle, hvdxg.destroysync_last_device,
        hvdxg.destroysync_last_type, hvdxg.destroysync_last_flags,
        hvdxg.destroysync_last_global,
        hvdxg.destroysync_last_monitor_fence,
        hvdxg.destroysync_last_cmd_len,
        hvdxg.destroysync_last_wire_len,
        hvdxg.destroysync_last_ext,
        hvdxg.destroysync_last_ext_offset,
        hvdxg.flushdevice_last_len, hvdxg.flushdevice_last_ret,
        (uint32)hvdxg.flushdevice_last_status,
        hvdxg.flushdevice_last_device, hvdxg.flushdevice_last_reason,
        hvdxg.createhwqueue_last_len, hvdxg.createhwqueue_last_ret,
        (uint32)hvdxg.createhwqueue_last_status,
        hvdxg.createhwqueue_last_context, hvdxg.createhwqueue_last_flags,
        hvdxg.createhwqueue_last_priv_size,
        hvdxg.createhwqueue_last_queue, hvdxg.createhwqueue_last_fence,
        hvdxg.createhwqueue_last_fence_cpu,
        hvdxg.createhwqueue_last_fence_gpu, hvdxg.submithwqueue_last_len,
        hvdxg.submithwqueue_last_ret, hvdxg.destroyhwqueue_last_len,
        hvdxg.destroyhwqueue_last_ret,
        hvdxg.createhwqueue_last_priv_head_len,
        hvdxg.createhwqueue_last_priv_head[0],
        hvdxg.createhwqueue_last_priv_head[1],
        hvdxg.createhwqueue_last_priv_head[2],
        hvdxg.createhwqueue_last_priv_head[3],
        hvdxg.createhwqueue_last_priv_head[4],
        hvdxg.createhwqueue_last_priv_head[5],
        hvdxg.createhwqueue_last_priv_head[6],
        hvdxg.createhwqueue_last_priv_head[7],
        hvdxg.submithwqueue_last_queue,
        hvdxg.submithwqueue_last_fence_id,
        hvdxg.submithwqueue_last_command_length,
        hvdxg.submithwqueue_last_priv_size,
        hvdxg.submithwqueue_last_priv_head_len,
        hvdxg.submithwqueue_last_priv_head[0],
        hvdxg.submithwqueue_last_priv_head[1],
        hvdxg.submithwqueue_last_priv_head[2],
        hvdxg.submithwqueue_last_priv_head[3],
        hvdxg.submithwqueue_last_priv_head[4],
        hvdxg.submithwqueue_last_priv_head[5],
        hvdxg.submithwqueue_last_priv_head[6],
        hvdxg.submithwqueue_last_priv_head[7],
        hvdxg.probe_attempts, hvdxg.probe_successes,
        hvdxg.probe_last_ret, hvdxg.probe_open_status,
        hvdxg.probe_open_handle, hvdxg.probe_open_host_version,
        hvdxg.probe_open_host_compat, hvdxg.probe_info_len,
        hvdxg.probe_info_flags, hvdxg.probe_async_msg_enabled,
        hvdxg.use_ext_header, hvdxg.host_vgpu_luid.b,
        hvdxg.host_vgpu_luid.a,
        hvdxg.probe_v40_open_send_ret,
        hvdxg.probe_v40_open_actual_len,
        (uint32)hvdxg.probe_v40_open_status,
        hvdxg.probe_v40_open_handle,
        hvdxg.probe_v40_open_host_version,
        hvdxg.probe_v40_open_host_compat,
        hvdxg.probe_v40_open_guest_luid_high,
        hvdxg.probe_v40_open_guest_luid_low,
        hvdxg.probe_v40_getinternal_send_ret,
        hvdxg.probe_v40_getinternal_actual_len,
        hvdxg.probe_v40_getinternal_flags,
        hvdxg.probe_v40_reject_reason,
        hvdxg_pci_guestcaps_source_name(),
        hvdxg.pci_guestcaps_ret,
        hvdxg.pci_guestcaps_found,
        hvdxg.pci_guestcaps_write_verified,
        hvdxg.pci_guestcaps_busdevfn,
        hvdxg.global_relid, hvdxg.global_conn_id,
        hvdxg.global_monitorid, hvdxg.global_monitor_allocated,
        hvdxg.global_dedicated,
        hvdxg.vgpu_relid, hvdxg.vgpu_conn_id,
        hvdxg.vgpu_monitorid, hvdxg.vgpu_monitor_allocated,
        hvdxg.vgpu_dedicated,
        hvdxg.global_offer_user_def[0],
        hvdxg.global_offer_user_def[1],
        hvdxg.global_offer_user_def[2],
        hvdxg.global_offer_user_def[3],
        hvdxg.vgpu_offer_user_def[0],
        hvdxg.vgpu_offer_user_def[1],
        hvdxg.vgpu_offer_user_def[2],
        hvdxg.vgpu_offer_user_def[3],
        hvdxg.probe_open_requested_version,
        hvdxg.probe_open_host_version,
        hvdxg.probe_open_host_compat,
        hvdxg.probe_info_flags, hvdxg.probe_async_msg_enabled,
        hvdxg.use_ext_header, host_luid_equiv_basis,
        hvdxg.host_vgpu_luid.b, hvdxg.host_vgpu_luid.a,
        hvdxg.pci_host_vgpu_luid.b, hvdxg.pci_host_vgpu_luid.a,
        hvdxg.global_mmio_megabytes,
        hvdxg.active_vmbus_version,
        hvdxg.pci_dxg_vmbus_version >= HV_DXG_VMBUS_INTERFACE_VERSION ?
            "true" : "false",
        hvdxg.pci_dxg_vmbus_version,
        hvdxg.pci_dxg_vmbus_negotiated_version,
        hvdxg.use_ext_header,
        hvdxg.active_vmbus_version,
        hvdxg.active_vmbus_last_compat,
        hvdxg.active_vmbus_source,
        hvdxg.active_vmbus_fallbacks,
        hvdxg.pci_dxg_vmbus_version,
        hvdxg.pci_dxg_vmbus_negotiated_version,
        hvdxg.pci_dxg_vmbus_write_attempted,
        hvdxg.pci_dxg_vmbus_writes,
        hvdxg.pci_dxg_vmbus_write_value,
        hvdxg.pci_dxg_vmbus_write_readback,
        hvdxg.pci_dxg_vmbus_write_ret,
        hvdxg.probe_open_requested_version,
        hvdxg.probe_open_host_version,
        hvdxg.probe_open_host_compat,
        hvdxg.use_ext_header,
        hvdxg.probe_info_len,
        hvdxg.pci_dxg_vmbus_write_attempted,
        hvdxg.pci_dxg_vmbus_writes,
        hvdxg.pci_dxg_vmbus_write_config_supported,
        hvdxg.pci_dxg_vmbus_write_config_ret,
        hvdxg.pci_dxg_vmbus_write_verify_ret,
        hvdxg.pci_dxg_vmbus_write_value,
        hvdxg.pci_dxg_vmbus_write_readback,
        hvdxg.pci_dxg_vmbus_write_ret,
        hvdxg.pci_guestcaps_source,
        hvdxg.pci_guestcaps_token,
        hvdxg.hyperv_pci_offer_present, hvdxg.hyperv_pci_offer_count,
        hvdxg.hyperv_pci_offer_relid, hvdxg.hyperv_pci_offer_conn_id,
        hvdxg.hyperv_pci_offer_monitorid,
        hvdxg.hyperv_pci_offer_monitor_allocated,
        hvdxg.hyperv_pci_offer_dedicated, hvdxg.hyperv_pci_offer_flags,
        hvdxg.hyperv_pci_offer_mmio_megabytes,
        hvdxg.hyperv_pci_offer_instance.a,
        hvdxg.hyperv_pci_offer_instance.b,
        hvdxg.hyperv_pci_offer_instance.c,
        hvdxg.hyperv_pci_offer_instance.d[0],
        hvdxg.hyperv_pci_offer_instance.d[1],
        hvdxg.hyperv_pci_offer_user_def[0],
        hvdxg.hyperv_pci_offer_user_def[1],
        hvdxg.hyperv_pci_offer_user_def[2],
        hvdxg.hyperv_pci_offer_user_def[3],
        hvpci.present, hvpci.child_relid, hvpci.signal_conn_id,
        hvpci.gpadl_ok, hvpci.gpadl_status,
        hvpci.open_ok, hvpci.open_status, hvpci.protocol_ok,
        hvpci.protocol_selected_version, hvpci.protocol_attempts,
        hvpci.protocol_last_version, hvpci.protocol_last_status,
        hvpci.protocol_last_ret, hvpci.protocol_last_packet_type,
        hvpci.protocol_last_len, hvpci.protocol_last_trans_id,
        hvpci.protocol_last_prefix[0], hvpci.protocol_last_prefix[1],
        hvpci.protocol_last_prefix[2], hvpci.protocol_last_prefix[3],
        hvpci.protocol_last_prefix[4], hvpci.protocol_last_prefix[5],
        hvpci.protocol_last_prefix[6], hvpci.protocol_last_prefix[7],
        hvpci.config_window_ok, hvpci.config_window_source,
        hvpci.config_window_ret,
        hvpci.config_window_pa, hvpci.config_window_va,
        hvpci.config_window_size, hvpci.config_window_candidate,
        hvpci.config_window_limit, hvpci.config_window_rejects,
        hvpci.backend_registered,
        hvpci.backend_index, hvpci.d0_attempts, hvpci.d0_sent,
        hvpci.d0_status, hvpci.d0_ret, hvpci.query_attempts,
        hvpci.query_sent, hvpci.query_ret, hvpci.relations_seen,
        hvpci.relations_len, hvpci.relations_count,
        hvpci.relations_desc_size, hvpci.relations_parse_ok,
        hvpci.child_count, hvpci.registered_count,
        hvpci.register_last_ret, hvpci.child[0].vendor_id,
        hvpci.child[0].device_id, hvpci.child[0].class_code,
        hvpci.child[0].win_slot, hvpci.child[0].bus,
        hvpci.child[0].dev, hvpci.child[0].func,
        hvpci.config_read_count, hvpci.config_write_count,
        hvpci.config_reject_count, hvpci.config_last_ret,
        hvpci.config_last_token, hvpci.config_last_offset,
        hvpci.config_last_size, hvpci.config_last_value,
        hvpci.query_packet_type, hvpci.query_len,
        hvpci.query_prefix[0], hvpci.query_prefix[1],
        hvpci.query_prefix[2], hvpci.query_prefix[3],
        hvpci.query_prefix[4], hvpci.query_prefix[5],
        hvpci.query_prefix[6], hvpci.query_prefix[7],
        hvdxg.pci_domain, hvdxg.pci_guestcaps_busdevfn,
        hvdxg.pci_bus, hvdxg.pci_dev, hvdxg.pci_func,
        hvdxg.pci_vendor, hvdxg.pci_dxg_device, hvdxg.pci_class,
        hvdxg.pci_dxg_vmbus_version,
        hvdxg.pci_dxg_guid[0], hvdxg.pci_dxg_guid[1],
        hvdxg.pci_dxg_guid[2], hvdxg.pci_dxg_guid[3],
        hvdxg.pci_host_vgpu_luid.b, hvdxg.pci_host_vgpu_luid.a,
        hvdxg.pci_guestcaps_attempts, hvdxg.pci_guestcaps_writes,
        hvdxg.pci_guestcaps_found, hvdxg.pci_guestcaps_scan_done,
        hvdxg.pci_guestcaps_write_attempted,
        hvdxg.pci_guestcaps_write_verified,
        hvdxg.pci_guestcaps_source,
        hvdxg.pci_guestcaps_token,
        hvdxg.pci_guestcaps_offset, hvdxg.pci_guestcaps_value,
        hvdxg.pci_guestcaps_readback, hvdxg.pci_guestcaps_ret,
        hvdxg.pci_guestcaps_before_probe, hvdxg.pci_guestcaps_busdevfn,
        hvdxg.probe_last_type, hvdxg.probe_last_len,
        hvdxg.probe_last_prefix[0], hvdxg.probe_last_prefix[1],
        hvdxg.probe_last_prefix[2], hvdxg.probe_last_prefix[3],
        hvdxg.probe_last_prefix[4], hvdxg.probe_last_prefix[5],
        hvdxg.probe_last_prefix[6], hvdxg.probe_last_prefix[7],
        hvdxg.createprocess_last_len, hvdxg.createprocess_last_cmd_len,
        hvdxg.createprocess_last_ret, hvdxg.createprocess_last_guest,
        hvdxg.createprocess_last_pid, hvdxg.createprocess_last_tgid,
        hvdxg.createprocess_last_handle,
        hvdxg.createprocess_last_layout, hvdxg.createprocess_last_generation,
        hvdxg.destroyprocess_last_len,
        hvdxg.destroyprocess_last_ret, hvdxg.destroyprocess_last_handle,
        hvdxg.ioctl_count, hvdxg.ioctl_successes, hvdxg.d3dkmt_ready,
        hvdxg.ioctl_last_ret, hvdxg.dxg_process.v,
        hvdxg.ioctl_history_index,
        HV_DXG_IOCTL_H_ARGS(0), HV_DXG_IOCTL_H_ARGS(1),
        HV_DXG_IOCTL_H_ARGS(2), HV_DXG_IOCTL_H_ARGS(3),
        HV_DXG_IOCTL_H_ARGS(4), HV_DXG_IOCTL_H_ARGS(5),
        HV_DXG_IOCTL_H_ARGS(6), HV_DXG_IOCTL_H_ARGS(7),
        HV_DXG_IOCTL_H_ARGS(8), HV_DXG_IOCTL_H_ARGS(9),
        HV_DXG_IOCTL_H_ARGS(10), HV_DXG_IOCTL_H_ARGS(11),
        HV_DXG_IOCTL_H_ARGS(12), HV_DXG_IOCTL_H_ARGS(13),
        HV_DXG_IOCTL_H_ARGS(14), HV_DXG_IOCTL_H_ARGS(15),
        hvdxg.ioctl_nr_count[0x13], hvdxg.ioctl_nr_count[0x05],
        hvdxg.ioctl_nr_count[0x1b], hvdxg.ioctl_nr_count[0x1c],
        hvdxg.ioctl_nr_count[0x19], hvdxg.ioctl_nr_count[0x1d],
        hvdxg.ioctl_nr_count[0x20], hvdxg.ioctl_nr_count[0x15],
        hvdxg.open_count, hvdxg.live_open_count, hvdxg.cleanup_attempts,
        hvdxg.cleanup_successes, hvdxg.cleanup_last_ret,
        hvdxg.cleanup_last_op, hvdxg.cleanup_last_handle,
        hvdxg.cleanup_failed_op, hvdxg.cleanup_failed_handle,
        hvdxg.cleanup_had_tracked,
        hvdxg.cleanup_resource_host_destroys,
        hvdxg.cleanup_resource_child_locals,
        hvdxg.cleanup_standalone_alloc_destroys,
        hvdxg.cleanup_resource_alloc_skips,
        hvdxg.cleanup_wsl_order_seq,
        hvdxg.cleanup_wsl_order_sync,
        hvdxg.cleanup_wsl_order_allocation,
        hvdxg.cleanup_wsl_order_resource,
        hvdxg.cleanup_wsl_order_context,
        hvdxg.cleanup_wsl_order_hwqueue,
        hvdxg.cleanup_wsl_order_pagingqueue,
        hvdxg.cleanup_wsl_order_gpuva,
        hvdxg.cleanup_wsl_order_device,
        hvdxg.cleanup_wsl_order_process,
        hvdxg.cleanup_wsl_order_valid,
        hvdxg.process_live, hvdxg.process_live_max, hvdxg.process_creates,
        hvdxg.process_reuses, hvdxg.process_releases,
        hvdxg.process_destroy_attempts, hvdxg.process_destroy_successes,
        hvdxg.process_destroy_failures,
        hvdxg.process_destroy_suppressed,
        hvdxg.process_destroy_deferred, hvdxg.process_shared_reuses,
        hvdxg.process_isolated_reuses, hvdxg.process_table_full,
        hvdxg.process_generation,
        hvdxg.process_retained_handle, hvdxg.process_retained_generation,
        hvdxg.process_retained_refs,
        hvdxg.process_destroy_active_total,
        hvdxg.process_destroy_active_device,
        hvdxg.process_destroy_active_context,
        hvdxg.process_destroy_active_hwqueue,
        hvdxg.process_destroy_active_pagingqueue,
        hvdxg.process_destroy_active_sync,
        hvdxg.process_destroy_active_allocation,
        hvdxg.process_destroy_active_resource,
        hvdxg.process_destroy_active_gpuva,
        hvdxg.open_createprocess_successes,
        hvdxg.process_shared_reuses,
        hvdxg.process_retained_reuse_avoided,
        hvdxg.process_retained_avoided_tgid,
        hvdxg.process_retained_avoided_source_tgid,
        hvdxg.process_retained_avoided_handle,
        hvdxg.process_retained_avoided_generation,
        hvdxg.process_retained_avoided_source_generation,
        hvdxg.process_isolated_last_tgid,
        hvdxg.process_isolated_last_handle,
        hvdxg.process_isolated_last_generation,
        hvdxg.process_isolated_source_generation,
        hvdxg.process_isolated_copied_objects,
        hvdxg.process_isolated_source_objects,
        hvdxg.process_object_refs_last,
        hvdxg.process_mem_refs_last,
        hvdxg.process_releases,
        hvdxg.process_mem_releases,
        hvdxg.process_mem_frees);
    if (len < 0) {
        kvfree(status);
        return -EIO;
    }
    if ((size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_adapter_luids=adapter:%x:%x host_adapter:%x:%x "
            "host_vgpu:%x:%x pci:%x:%x\n"
            "dxg_luid_equivalence=user_luid:%x:%x "
            "host_adapter:%x:%x host_vgpu:%x:%x "
            "user_luid_known:%u user_luid_source:%u "
            "scope:per_vm_vmbus_synthetic\n"
            "dxg_openadapterfromluid=input:%x:%x user_luid:%x:%x "
            "ext_luid:%x:%x user_source:%u match:%u reject:%u ret:%d "
            "status:0x%x handle:0x%x host_handle:0x%x\n"
            "dxg_createdevice_last=adapter:0x%x host_adapter:0x%x "
            "device:0x%x eq_local:%u eq_host:%u ret:%d\n"
            "dxg_createdevice_object=proc:0x%x owner:0x%x/%u/%u "
            "found:%u type:%u local:0x%x host:0x%x parent:0x%x "
            "entry_dev:0x%x gen:%u destroyed:%u\n"
            "dxg_createprocess_success=len:%u cmd_len:%u ret:%d "
            "guest:0x%lx pid:%lu tgid:%lu handle:0x%x layout:%u gen:%u\n"
            "dxg_open_createprocess=attempts:%u successes:%u failures:%u "
            "ignored:%u last_ret:%d guest:0x%lx pid:%lu tgid:%lu "
            "handle:0x%x created:%u gen:%u refs:%u\n"
            "dxg_early_bind=attempts:%u successes:%u failures:%u "
            "source:%u source_name:%s cmd:0x%x ret:%d handle:0x%x "
            "created:%u gen:%u refs:%u\n"
            "dxg_host_cmd_counts=destroyalloc:%u destroycontext:%u "
            "destroyhwqueue:%u destroypaging:%u destroydevice:%u "
            "destroysync:%u destroyprocess:%u freegpuva:%u lock:%u "
            "unlock:%u closeadapter_ioctl:%u closeadapter_local:%u "
            "closeadapter_host:%u closeadapter_invalid:%u "
            "closeadapter_len:%u closeadapter_ret:%d closeadapter_status:0x%x\n"
            "dxg_closeadapter_order=seq:%u close:%u last_destroy:%u "
            "destroyprocess:%u after_destroy:%u\n"
            "dxg_syncobject_mapped=count:%u len:%u type:%u flags:0x%x "
            "fence_cpu:0x%lx fence_gpu:0x%lx\n",
            hvdxg.adapter_luid.b, hvdxg.adapter_luid.a,
            hvdxg.host_adapter_luid.b, hvdxg.host_adapter_luid.a,
            hvdxg.host_vgpu_luid.b, hvdxg.host_vgpu_luid.a,
            hvdxg.pci_host_vgpu_luid.b, hvdxg.pci_host_vgpu_luid.a,
            user_luid.b, user_luid.a,
            hvdxg.host_adapter_luid.b, hvdxg.host_adapter_luid.a,
            hvdxg.host_vgpu_luid.b, hvdxg.host_vgpu_luid.a,
            host_luid_equiv_basis != 0 ? 1U : 0U,
            host_luid_equiv_basis,
            hvdxg.openadapter_luid_last_input_high,
            hvdxg.openadapter_luid_last_input_low,
            hvdxg.openadapter_luid_last_um_high,
            hvdxg.openadapter_luid_last_um_low,
            hvdxg.openadapter_luid_last_host_high,
            hvdxg.openadapter_luid_last_host_low,
            hvdxg.openadapter_luid_last_host_basis,
            hvdxg.openadapter_luid_last_match,
            hvdxg.openadapter_luid_last_reject,
            hvdxg.openadapter_luid_last_ret,
            (uint32)hvdxg.openadapter_luid_last_status,
            hvdxg.openadapter_luid_last_handle,
            hvdxg.host_adapter_handle,
            hvdxg.createdevice_last_adapter,
            hvdxg.createdevice_last_host_adapter,
            hvdxg.createdevice_last_device,
            hvdxg.createdevice_adapter_equals_device,
            hvdxg.createdevice_host_adapter_equals_device,
            hvdxg.createdevice_last_ret,
            hvdxg.createdevice_last_process,
            hvdxg.createdevice_last_owner_process,
            hvdxg.createdevice_last_owner_generation,
            hvdxg.createdevice_last_owner_refs,
            hvdxg.createdevice_object_found,
            hvdxg.createdevice_object_type,
            hvdxg.createdevice_object_local,
            hvdxg.createdevice_object_host,
            hvdxg.createdevice_object_parent,
            hvdxg.createdevice_object_device,
            hvdxg.createdevice_object_generation,
            hvdxg.createdevice_object_destroyed,
            hvdxg.createprocess_success_len,
            hvdxg.createprocess_success_cmd_len,
            hvdxg.createprocess_success_ret,
            hvdxg.createprocess_success_guest,
            hvdxg.createprocess_success_pid,
            hvdxg.createprocess_success_tgid,
            hvdxg.createprocess_success_handle,
            hvdxg.createprocess_success_layout,
            hvdxg.createprocess_success_generation,
            hvdxg.open_createprocess_attempts,
            hvdxg.open_createprocess_successes,
            hvdxg.open_createprocess_failures,
            hvdxg.open_createprocess_ignored_failures,
            hvdxg.open_createprocess_last_ret,
            hvdxg.open_createprocess_last_guest,
            hvdxg.open_createprocess_last_pid,
            hvdxg.open_createprocess_last_tgid,
            hvdxg.open_createprocess_last_handle,
            hvdxg.open_createprocess_last_created,
            hvdxg.open_createprocess_last_generation,
            hvdxg.open_createprocess_last_refs,
            hvdxg.early_bind_attempts,
            hvdxg.early_bind_successes,
            hvdxg.early_bind_failures,
            hvdxg.early_bind_last_source,
            hvdxg_early_bind_source_name(hvdxg.early_bind_last_source),
            hvdxg.early_bind_last_cmd,
            hvdxg.early_bind_last_ret,
            hvdxg.early_bind_last_handle,
            hvdxg.early_bind_last_created,
            hvdxg.early_bind_last_generation,
            hvdxg.early_bind_last_refs,
            hvdxg.host_cmd_destroyallocation,
            hvdxg.host_cmd_destroycontext,
            hvdxg.host_cmd_destroyhwqueue,
            hvdxg.host_cmd_destroypagingqueue,
            hvdxg.host_cmd_destroydevice,
            hvdxg.host_cmd_destroysync,
            hvdxg.host_cmd_destroyprocess,
            hvdxg.host_cmd_freegpuva,
            hvdxg.host_cmd_lock2,
            hvdxg.host_cmd_unlock2,
            hvdxg.closeadapter_ioctl_count,
            hvdxg.closeadapter_local_count,
            hvdxg.closeadapter_host_count,
            hvdxg.closeadapter_invalid_count,
            hvdxg.closeadapter_last_len,
            hvdxg.closeadapter_last_ret,
            (uint32)hvdxg.closeadapter_last_status,
            hvdxg.cleanup_order_seq,
            hvdxg.closeadapter_last_order,
            hvdxg.cleanup_last_destroy_order,
            hvdxg.cleanup_destroyprocess_order,
            hvdxg.closeadapter_after_destroy_count,
            hvdxg.syncobject_mapped_count,
            hvdxg.syncobject_mapped_len,
            hvdxg.syncobject_mapped_type,
            hvdxg.syncobject_mapped_flags,
            hvdxg.syncobject_mapped_fence_cpu,
            hvdxg.syncobject_mapped_fence_gpu);
    }
    if ((size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_mapgpuva_layout_history=index:%u "
            "h0:pages:%lu/prot:0x%lx/dprot:0x%lx "
            "h1:pages:%lu/prot:0x%lx/dprot:0x%lx "
            "h2:pages:%lu/prot:0x%lx/dprot:0x%lx "
            "h3:pages:%lu/prot:0x%lx/dprot:0x%lx "
            "h4:pages:%lu/prot:0x%lx/dprot:0x%lx "
            "h5:pages:%lu/prot:0x%lx/dprot:0x%lx "
            "h6:pages:%lu/prot:0x%lx/dprot:0x%lx "
            "h7:pages:%lu/prot:0x%lx/dprot:0x%lx\n"
            "dxg_hwqueue_submit_history=index:%u "
            "h0:len:%u/ret:%d/queue:0x%x/cmd_len:%u/priv:%u/head_len:%u/head:%02x%02x%02x%02x%02x%02x%02x%02x "
            "h1:len:%u/ret:%d/queue:0x%x/cmd_len:%u/priv:%u/head_len:%u/head:%02x%02x%02x%02x%02x%02x%02x%02x "
            "h2:len:%u/ret:%d/queue:0x%x/cmd_len:%u/priv:%u/head_len:%u/head:%02x%02x%02x%02x%02x%02x%02x%02x "
            "h3:len:%u/ret:%d/queue:0x%x/cmd_len:%u/priv:%u/head_len:%u/head:%02x%02x%02x%02x%02x%02x%02x%02x "
            "h4:len:%u/ret:%d/queue:0x%x/cmd_len:%u/priv:%u/head_len:%u/head:%02x%02x%02x%02x%02x%02x%02x%02x "
            "h5:len:%u/ret:%d/queue:0x%x/cmd_len:%u/priv:%u/head_len:%u/head:%02x%02x%02x%02x%02x%02x%02x%02x "
            "h6:len:%u/ret:%d/queue:0x%x/cmd_len:%u/priv:%u/head_len:%u/head:%02x%02x%02x%02x%02x%02x%02x%02x "
            "h7:len:%u/ret:%d/queue:0x%x/cmd_len:%u/priv:%u/head_len:%u/head:%02x%02x%02x%02x%02x%02x%02x%02x\n"
            "dxg_lock2_detail=ioctls:%u forwarded:%u cached_refs:%u "
            "sysmem:%u map_fail:%u diag:%u last_len:%u last_ret:%d "
            "last_status:0x%x last_user:0x%lx\n"
            "dxg_unlock2_detail=ioctls:%u forwarded:%u missing_tracking:%u "
            "cached_refs:%u diag:%u last_len:%u last_ret:%d "
            "last_status:0x%x\n",
            hvdxg.mapgpuva_history_index,
            hvdxg.mapgpuva_history_pages[0],
            hvdxg.mapgpuva_history_protection[0],
            hvdxg.mapgpuva_history_driver_protection[0],
            hvdxg.mapgpuva_history_pages[1],
            hvdxg.mapgpuva_history_protection[1],
            hvdxg.mapgpuva_history_driver_protection[1],
            hvdxg.mapgpuva_history_pages[2],
            hvdxg.mapgpuva_history_protection[2],
            hvdxg.mapgpuva_history_driver_protection[2],
            hvdxg.mapgpuva_history_pages[3],
            hvdxg.mapgpuva_history_protection[3],
            hvdxg.mapgpuva_history_driver_protection[3],
            hvdxg.mapgpuva_history_pages[4],
            hvdxg.mapgpuva_history_protection[4],
            hvdxg.mapgpuva_history_driver_protection[4],
            hvdxg.mapgpuva_history_pages[5],
            hvdxg.mapgpuva_history_protection[5],
            hvdxg.mapgpuva_history_driver_protection[5],
            hvdxg.mapgpuva_history_pages[6],
            hvdxg.mapgpuva_history_protection[6],
            hvdxg.mapgpuva_history_driver_protection[6],
            hvdxg.mapgpuva_history_pages[7],
            hvdxg.mapgpuva_history_protection[7],
            hvdxg.mapgpuva_history_driver_protection[7],
            hvdxg.submithwqueue_history_index,
            HV_DXG_SH_ARGS(0), HV_DXG_SH_ARGS(1),
            HV_DXG_SH_ARGS(2), HV_DXG_SH_ARGS(3),
            HV_DXG_SH_ARGS(4), HV_DXG_SH_ARGS(5),
            HV_DXG_SH_ARGS(6), HV_DXG_SH_ARGS(7),
            hvdxg.lock2_ioctl_count,
            hvdxg.lock2_host_forward_count,
            hvdxg.lock2_cached_ref_count,
            hvdxg.lock2_sysmem_count,
            hvdxg.lock2_map_fail_count,
            hvdxg.lock2_diag_prints,
            hvdxg.lock2_last_len,
            hvdxg.lock2_last_ret,
            (uint32)hvdxg.lock2_last_status,
            hvdxg.lock2_last_user_va,
            hvdxg.unlock2_ioctl_count,
            hvdxg.unlock2_host_forward_count,
            hvdxg.unlock2_missing_tracking_count,
            hvdxg.unlock2_cached_ref_count,
            hvdxg.unlock2_diag_prints,
            hvdxg.unlock2_last_len,
            hvdxg.unlock2_last_ret,
            (uint32)hvdxg.unlock2_last_status);
    }
    if ((size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_ntshared_create_attempts=count:%u "
            "a0:cmd:%u/off:%u/wire:%u/ext:%u/eoff:%u/len:%u/ret:%d/raw:0x%x/status:0x%x/proc:0x%x/obj:0x%x "
            "a1:cmd:%u/off:%u/wire:%u/ext:%u/eoff:%u/len:%u/ret:%d/raw:0x%x/status:0x%x/proc:0x%x/obj:0x%x "
            "a2:cmd:%u/off:%u/wire:%u/ext:%u/eoff:%u/len:%u/ret:%d/raw:0x%x/status:0x%x/proc:0x%x/obj:0x%x "
            "a3:cmd:%u/off:%u/wire:%u/ext:%u/eoff:%u/len:%u/ret:%d/raw:0x%x/status:0x%x/proc:0x%x/obj:0x%x "
            "a4:cmd:%u/off:%u/wire:%u/ext:%u/eoff:%u/len:%u/ret:%d/raw:0x%x/status:0x%x/proc:0x%x/obj:0x%x\n",
            hvdxg.ntshared_create_attempts,
            hvdxg.ntshared_create_attempt_cmd_len[0],
            hvdxg.ntshared_create_attempt_object_offset[0],
            hvdxg.ntshared_create_attempt_wire_len[0],
            hvdxg.ntshared_create_attempt_ext[0],
            hvdxg.ntshared_create_attempt_ext_offset[0],
            hvdxg.ntshared_create_attempt_len[0],
            hvdxg.ntshared_create_attempt_ret[0],
            hvdxg.ntshared_create_attempt_raw0[0],
            hvdxg.ntshared_create_attempt_status[0],
            hvdxg.ntshared_create_attempt_process[0],
            hvdxg.ntshared_create_attempt_object[0],
            hvdxg.ntshared_create_attempt_cmd_len[1],
            hvdxg.ntshared_create_attempt_object_offset[1],
            hvdxg.ntshared_create_attempt_wire_len[1],
            hvdxg.ntshared_create_attempt_ext[1],
            hvdxg.ntshared_create_attempt_ext_offset[1],
            hvdxg.ntshared_create_attempt_len[1],
            hvdxg.ntshared_create_attempt_ret[1],
            hvdxg.ntshared_create_attempt_raw0[1],
            hvdxg.ntshared_create_attempt_status[1],
            hvdxg.ntshared_create_attempt_process[1],
            hvdxg.ntshared_create_attempt_object[1],
            hvdxg.ntshared_create_attempt_cmd_len[2],
            hvdxg.ntshared_create_attempt_object_offset[2],
            hvdxg.ntshared_create_attempt_wire_len[2],
            hvdxg.ntshared_create_attempt_ext[2],
            hvdxg.ntshared_create_attempt_ext_offset[2],
            hvdxg.ntshared_create_attempt_len[2],
            hvdxg.ntshared_create_attempt_ret[2],
            hvdxg.ntshared_create_attempt_raw0[2],
            hvdxg.ntshared_create_attempt_status[2],
            hvdxg.ntshared_create_attempt_process[2],
            hvdxg.ntshared_create_attempt_object[2],
            hvdxg.ntshared_create_attempt_cmd_len[3],
            hvdxg.ntshared_create_attempt_object_offset[3],
            hvdxg.ntshared_create_attempt_wire_len[3],
            hvdxg.ntshared_create_attempt_ext[3],
            hvdxg.ntshared_create_attempt_ext_offset[3],
            hvdxg.ntshared_create_attempt_len[3],
            hvdxg.ntshared_create_attempt_ret[3],
            hvdxg.ntshared_create_attempt_raw0[3],
            hvdxg.ntshared_create_attempt_status[3],
            hvdxg.ntshared_create_attempt_process[3],
            hvdxg.ntshared_create_attempt_object[3],
            hvdxg.ntshared_create_attempt_cmd_len[4],
            hvdxg.ntshared_create_attempt_object_offset[4],
            hvdxg.ntshared_create_attempt_wire_len[4],
            hvdxg.ntshared_create_attempt_ext[4],
            hvdxg.ntshared_create_attempt_ext_offset[4],
            hvdxg.ntshared_create_attempt_len[4],
            hvdxg.ntshared_create_attempt_ret[4],
            hvdxg.ntshared_create_attempt_raw0[4],
            hvdxg.ntshared_create_attempt_status[4],
            hvdxg.ntshared_create_attempt_process[4],
            hvdxg.ntshared_create_attempt_object[4]);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_queryadapter_packet=type:%u size:%u cmd_len:%u "
            "wire_len:%u ext:%u ext_off:%u type_off:%u size_off:%u "
            "data_off:%u desc_size:%u pkt_len:%u pkt_aligned:%u "
            "pkt_pad:%u desc_off8:%u desc_len8:%u ring_total:%u "
            "actual:%u ret:%d status:0x%x\n",
            hvdxg.queryadapter_packet_type,
            hvdxg.queryadapter_packet_size,
            hvdxg.queryadapter_packet_cmd_len,
            hvdxg.queryadapter_packet_wire_len,
            hvdxg.queryadapter_packet_ext,
            hvdxg.queryadapter_packet_ext_offset,
            hvdxg.queryadapter_packet_type_offset,
            hvdxg.queryadapter_packet_size_offset,
            hvdxg.queryadapter_packet_data_offset,
            hvdxg.queryadapter_packet_desc_size,
            hvdxg.queryadapter_packet_len,
            hvdxg.queryadapter_packet_aligned,
            hvdxg.queryadapter_packet_pad,
            hvdxg.queryadapter_packet_desc_off8,
            hvdxg.queryadapter_packet_desc_len8,
            hvdxg.queryadapter_packet_ring_total,
            hvdxg.queryadapter_last_len,
            hvdxg.queryadapter_last_ret,
            (uint32)hvdxg.queryadapter_last_status);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
                        "dxg_queryadapter_bytes=ext_len:%u ext:",
                        hvdxg.queryadapter_packet_ext_len);
        hvdxg_status_append_hex_inline(status, status_size, &len,
            hvdxg.queryadapter_packet_ext_bytes,
            hvdxg.queryadapter_packet_ext_len);
        if (len >= 0 && (size_t)len < status_size)
            len += snprintf(status + len, status_size - (size_t)len,
                            " cmdhdr_len:%u cmdhdr:",
                            hvdxg.queryadapter_packet_cmdhdr_len);
        hvdxg_status_append_hex_inline(status, status_size, &len,
            hvdxg.queryadapter_packet_cmdhdr,
            hvdxg.queryadapter_packet_cmdhdr_len);
        if (len >= 0 && (size_t)len < status_size)
            len += snprintf(status + len, status_size - (size_t)len,
                            " priv_head_len:%u priv_head:",
                            hvdxg.queryadapter_packet_priv_head_len);
        hvdxg_status_append_hex_inline(status, status_size, &len,
            hvdxg.queryadapter_packet_priv_head,
            hvdxg.queryadapter_packet_priv_head_len);
        if (len >= 0 && (size_t)len < status_size)
            len += snprintf(status + len, status_size - (size_t)len,
                            "\n");
    }
    hvdxg_status_append_queryadapter_payloads(status, status_size, &len);
    if ((size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_ntshared_attempt_meta="
            "a0:label:%u/id:%lu/type:%u/rlen:%u/head:%u "
            "a1:label:%u/id:%lu/type:%u/rlen:%u/head:%u "
            "a2:label:%u/id:%lu/type:%u/rlen:%u/head:%u "
            "a3:label:%u/id:%lu/type:%u/rlen:%u/head:%u "
            "a4:label:%u/id:%lu/type:%u/rlen:%u/head:%u "
            "labels:1=wsl_ext24_zero_luid,2=wsl_noext24,3=ext24_zero_luid,4=ext24_host_luid,5=noext28,6=noext24,7=noext32_fallback,8=global_noext32,9=global_ext32_zero_luid,10=legacy_ext32_zero_luid,11=wsl_noext32_natural,12=wsl_ext32_zero_luid_natural\n",
            hvdxg.ntshared_create_attempt_label[0],
            hvdxg.ntshared_create_attempt_cmdid[0],
            hvdxg.ntshared_create_attempt_command[0],
            hvdxg.ntshared_create_attempt_result_len[0],
            hvdxg.ntshared_create_attempt_head_len[0],
            hvdxg.ntshared_create_attempt_label[1],
            hvdxg.ntshared_create_attempt_cmdid[1],
            hvdxg.ntshared_create_attempt_command[1],
            hvdxg.ntshared_create_attempt_result_len[1],
            hvdxg.ntshared_create_attempt_head_len[1],
            hvdxg.ntshared_create_attempt_label[2],
            hvdxg.ntshared_create_attempt_cmdid[2],
            hvdxg.ntshared_create_attempt_command[2],
            hvdxg.ntshared_create_attempt_result_len[2],
            hvdxg.ntshared_create_attempt_head_len[2],
            hvdxg.ntshared_create_attempt_label[3],
            hvdxg.ntshared_create_attempt_cmdid[3],
            hvdxg.ntshared_create_attempt_command[3],
            hvdxg.ntshared_create_attempt_result_len[3],
            hvdxg.ntshared_create_attempt_head_len[3],
            hvdxg.ntshared_create_attempt_label[4],
            hvdxg.ntshared_create_attempt_cmdid[4],
            hvdxg.ntshared_create_attempt_command[4],
            hvdxg.ntshared_create_attempt_result_len[4],
            hvdxg.ntshared_create_attempt_head_len[4]);
    }
    if ((size_t)len < status_size) {
        uint32 ext24_zero_luid = 0;
        uint32 ext32_zero_luid_natural = 0;
        uint32 noext24 = 0;
        uint32 noext32_natural = 0;
        uint32 ext24_attempt = 0xffffffffU;
        uint32 ext32_attempt = 0xffffffffU;
        uint32 noext24_attempt = 0xffffffffU;
        uint32 noext32_natural_attempt = 0xffffffffU;

        for (uint32 i = 0; i < HV_DXG_NTSHARED_ATTEMPT_MAX; i++) {
            if ((hvdxg.ntshared_create_attempt_label[i] ==
                    HV_DXG_NTSHARED_LABEL_WSL_EXT24_ZERO_LUID ||
                 hvdxg.ntshared_create_attempt_label[i] ==
                    HV_DXG_NTSHARED_LABEL_EXT24_ZERO_LUID) &&
                hvdxg.ntshared_create_attempt_cmdid[i] == 0 &&
                hvdxg.ntshared_create_attempt_command[i] ==
                    HV_DXGK_VMBCOMMAND_CREATENTSHAREDOBJECT &&
                hvdxg.ntshared_create_attempt_cmd_len[i] == 24 &&
                hvdxg.ntshared_create_attempt_wire_len[i] == 40 &&
                hvdxg.ntshared_create_attempt_ext[i] == 1 &&
                hvdxg.ntshared_create_attempt_ext_offset[i] == 16 &&
                hvdxg.ntshared_create_attempt_object_offset[i] == 20 &&
                hvdxg.ntshared_create_attempt_result_len[i] == 4 &&
                hvdxg.ntshared_create_attempt_head_len[i] >= 40 &&
                hvdxg_read_u32_at(hvdxg.ntshared_create_attempt_head[i],
                                  hvdxg.ntshared_create_attempt_head_len[i],
                                  0) == 16 &&
                hvdxg_read_u32_at(hvdxg.ntshared_create_attempt_head[i],
                                  hvdxg.ntshared_create_attempt_head_len[i],
                                  8) == 0 &&
                hvdxg_read_u32_at(hvdxg.ntshared_create_attempt_head[i],
                                  hvdxg.ntshared_create_attempt_head_len[i],
                                  12) == 0 &&
                hvdxg_read_u32_at(hvdxg.ntshared_create_attempt_head[i],
                                  hvdxg.ntshared_create_attempt_head_len[i],
                                  36) ==
                    hvdxg.ntshared_create_attempt_object[i]) {
                ext24_zero_luid = 1;
                ext24_attempt = i;
            }
            if (hvdxg.ntshared_create_attempt_label[i] ==
                    HV_DXG_NTSHARED_LABEL_WSL_EXT32_ZERO_LUID_NATURAL &&
                hvdxg.ntshared_create_attempt_cmdid[i] == 0 &&
                hvdxg.ntshared_create_attempt_command[i] ==
                    HV_DXGK_VMBCOMMAND_CREATENTSHAREDOBJECT &&
                hvdxg.ntshared_create_attempt_cmd_len[i] == 32 &&
                hvdxg.ntshared_create_attempt_wire_len[i] == 48 &&
                hvdxg.ntshared_create_attempt_ext[i] == 1 &&
                hvdxg.ntshared_create_attempt_ext_offset[i] == 16 &&
                hvdxg.ntshared_create_attempt_object_offset[i] == 24 &&
                hvdxg.ntshared_create_attempt_result_len[i] == 4 &&
                hvdxg.ntshared_create_attempt_head_len[i] >= 48 &&
                hvdxg_read_u32_at(hvdxg.ntshared_create_attempt_head[i],
                                  hvdxg.ntshared_create_attempt_head_len[i],
                                  0) == 16 &&
                hvdxg_read_u32_at(hvdxg.ntshared_create_attempt_head[i],
                                  hvdxg.ntshared_create_attempt_head_len[i],
                                  8) == 0 &&
                hvdxg_read_u32_at(hvdxg.ntshared_create_attempt_head[i],
                                  hvdxg.ntshared_create_attempt_head_len[i],
                                  12) == 0 &&
                hvdxg_read_u32_at(hvdxg.ntshared_create_attempt_head[i],
                                  hvdxg.ntshared_create_attempt_head_len[i],
                                  40) ==
                    hvdxg.ntshared_create_attempt_object[i]) {
                ext32_zero_luid_natural = 1;
                ext32_attempt = i;
            }
            if (hvdxg.ntshared_create_attempt_label[i] ==
                    HV_DXG_NTSHARED_LABEL_WSL_NOEXT32_NATURAL &&
                hvdxg.ntshared_create_attempt_cmdid[i] == 0 &&
                hvdxg.ntshared_create_attempt_command[i] ==
                    HV_DXGK_VMBCOMMAND_CREATENTSHAREDOBJECT &&
                hvdxg.ntshared_create_attempt_cmd_len[i] == 32 &&
                hvdxg.ntshared_create_attempt_wire_len[i] == 32 &&
                hvdxg.ntshared_create_attempt_ext[i] == 0 &&
                hvdxg.ntshared_create_attempt_ext_offset[i] == 0 &&
                hvdxg.ntshared_create_attempt_object_offset[i] == 24 &&
                hvdxg.ntshared_create_attempt_result_len[i] == 4 &&
                hvdxg.ntshared_create_attempt_head_len[i] >= 32 &&
                hvdxg_read_u32_at(hvdxg.ntshared_create_attempt_head[i],
                                  hvdxg.ntshared_create_attempt_head_len[i],
                                  24) ==
                    hvdxg.ntshared_create_attempt_object[i]) {
                noext32_natural = 1;
                noext32_natural_attempt = i;
            }
            if (hvdxg.ntshared_create_attempt_label[i] ==
                    HV_DXG_NTSHARED_LABEL_WSL_NOEXT24 &&
                hvdxg.ntshared_create_attempt_cmdid[i] == 0 &&
                hvdxg.ntshared_create_attempt_command[i] ==
                    HV_DXGK_VMBCOMMAND_CREATENTSHAREDOBJECT &&
                hvdxg.ntshared_create_attempt_cmd_len[i] == 24 &&
                hvdxg.ntshared_create_attempt_wire_len[i] == 24 &&
                hvdxg.ntshared_create_attempt_ext[i] == 0 &&
                hvdxg.ntshared_create_attempt_ext_offset[i] == 0 &&
                hvdxg.ntshared_create_attempt_object_offset[i] == 20 &&
                hvdxg.ntshared_create_attempt_result_len[i] == 4 &&
                hvdxg.ntshared_create_attempt_head_len[i] >= 24 &&
                hvdxg_read_u32_at(hvdxg.ntshared_create_attempt_head[i],
                                  hvdxg.ntshared_create_attempt_head_len[i],
                                  20) ==
                    hvdxg.ntshared_create_attempt_object[i]) {
                noext24 = 1;
                noext24_attempt = i;
            }
        }
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_ntshared_wsl_exact=ext32_zero_luid_natural:%u/a%u ext24_zero_luid:%u/a%u noext32_natural:%u/a%u noext24:%u/a%u first_label:%u verdict:%s ext_verdict:%s\n",
            ext32_zero_luid_natural,
            ext32_attempt == 0xffffffffU ? 99U : ext32_attempt,
            ext24_zero_luid,
            ext24_attempt == 0xffffffffU ? 99U : ext24_attempt,
            noext32_natural,
            noext32_natural_attempt == 0xffffffffU ? 99U :
                noext32_natural_attempt,
            noext24,
            noext24_attempt == 0xffffffffU ? 99U : noext24_attempt,
            hvdxg.ntshared_create_attempt_label[0],
            (ext32_zero_luid_natural || noext32_natural) ?
                "sent" : "missing",
            ext32_zero_luid_natural ? "sent" : "missing");
    }
    if (len >= 0 && (size_t)len < status_size) {
        uint32 retry_attempted = hvdxg.ntshared_create_attempts > 1 ? 1 : 0;
        uint32 first_is_wsl_natural =
            hvdxg.ntshared_create_attempt_label[0] ==
                HV_DXG_NTSHARED_LABEL_WSL_NOEXT32_NATURAL ||
            hvdxg.ntshared_create_attempt_label[0] ==
                HV_DXG_NTSHARED_LABEL_WSL_EXT32_ZERO_LUID_NATURAL;
        uint32 retry_is_old_ext =
            hvdxg.ntshared_create_attempt_label[1] ==
            HV_DXG_NTSHARED_LABEL_WSL_EXT24_ZERO_LUID;

        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_ntshared_envelope="
            "wsl_src:global_vm_to_host/natural32/process_host/object/result4 "
            "expected:payload:32/obj_off:24/result:4/wire:32_or_ext48 "
            "first:path:%u/wsl:%u/route:%s/ch:%u/type:%u/payload:%u/"
            "wire:%u/ext:%u/eoff:%u/obj_off:%u/result:%u/proc:0x%x/"
            "obj:0x%x/actual:%u/ret:%d/status:0x%x/raw:0x%x "
            "retry:attempted:%u/path:%u/old_ext24:%u/route:%s/ch:%u/"
            "type:%u/payload:%u/wire:%u/ext:%u/eoff:%u/obj_off:%u/"
            "result:%u/proc:0x%x/obj:0x%x/actual:%u/ret:%d/"
            "status:0x%x/raw:0x%x alt_policy:no_layout_retry\n",
            hvdxg.ntshared_create_attempt_label[0],
            first_is_wsl_natural,
            hvdxg_channel_name(HV_DXG_CHANNEL_GLOBAL),
            hvdxg.ntshared_create_attempt_channel[0],
            hvdxg.ntshared_create_attempt_command[0],
            hvdxg.ntshared_create_attempt_cmd_len[0],
            hvdxg.ntshared_create_attempt_wire_len[0],
            hvdxg.ntshared_create_attempt_ext[0],
            hvdxg.ntshared_create_attempt_ext_offset[0],
            hvdxg.ntshared_create_attempt_object_offset[0],
            hvdxg.ntshared_create_attempt_result_len[0],
            hvdxg.ntshared_create_attempt_process[0],
            hvdxg.ntshared_create_attempt_object[0],
            hvdxg.ntshared_create_attempt_len[0],
            hvdxg.ntshared_create_attempt_ret[0],
            hvdxg.ntshared_create_attempt_status[0],
            hvdxg.ntshared_create_attempt_raw0[0],
            retry_attempted,
            hvdxg.ntshared_create_attempt_label[1],
            retry_is_old_ext,
            hvdxg_channel_name(HV_DXG_CHANNEL_GLOBAL),
            hvdxg.ntshared_create_attempt_channel[1],
            hvdxg.ntshared_create_attempt_command[1],
            hvdxg.ntshared_create_attempt_cmd_len[1],
            hvdxg.ntshared_create_attempt_wire_len[1],
            hvdxg.ntshared_create_attempt_ext[1],
            hvdxg.ntshared_create_attempt_ext_offset[1],
            hvdxg.ntshared_create_attempt_object_offset[1],
            hvdxg.ntshared_create_attempt_result_len[1],
            hvdxg.ntshared_create_attempt_process[1],
            hvdxg.ntshared_create_attempt_object[1],
            hvdxg.ntshared_create_attempt_len[1],
            hvdxg.ntshared_create_attempt_ret[1],
            hvdxg.ntshared_create_attempt_status[1],
            hvdxg.ntshared_create_attempt_raw0[1]);
    }
    hvdxg_status_append_hex(status, status_size, &len,
                            "dxg_ntshared_wire_a0",
                            hvdxg.ntshared_create_attempt_head[0],
                            hvdxg.ntshared_create_attempt_head_len[0]);
    hvdxg_status_append_hex(status, status_size, &len,
                            "dxg_ntshared_wire_a1",
                            hvdxg.ntshared_create_attempt_head[1],
                            hvdxg.ntshared_create_attempt_head_len[1]);
    hvdxg_status_append_hex(status, status_size, &len,
                            "dxg_ntshared_wire_a2",
                            hvdxg.ntshared_create_attempt_head[2],
                            hvdxg.ntshared_create_attempt_head_len[2]);
    hvdxg_status_append_hex(status, status_size, &len,
                            "dxg_ntshared_wire_a3",
                            hvdxg.ntshared_create_attempt_head[3],
                            hvdxg.ntshared_create_attempt_head_len[3]);
    hvdxg_status_append_hex(status, status_size, &len,
                            "dxg_ntshared_wire_a4",
                            hvdxg.ntshared_create_attempt_head[4],
                            hvdxg.ntshared_create_attempt_head_len[4]);
	    hvdxg_status_append_hex(status, status_size, &len,
	                            "dxg_shareobject_wire",
	                            hvdxg.shareobject_last_head,
	                            hvdxg.shareobject_last_head_len);
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_queryadapter_adaptertype_rewrite=count:%u type:%u "
            "source:%u raw:0x%x wsl:0x%x cleared:0x%x forced:0x%x "
            "compute_only:%u\n",
            hvdxg.queryadapter_adaptertype_rewrite_count,
            hvdxg.queryadapter_adaptertype_rewrite_type,
            hvdxg.queryadapter_adaptertype_rewrite_source,
            hvdxg.queryadapter_adaptertype_raw_value,
            hvdxg.queryadapter_adaptertype_wsl_value,
            hvdxg.queryadapter_adaptertype_cleared_bits,
            hvdxg.queryadapter_adaptertype_forced_bits,
            hvdxg.queryadapter_adaptertype_compute_only);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_queryadapter_type27_route=last_type:%u route:%u "
            "packet_type:%u cmd_type:%u channel_type:%u proc:0x%x "
            "cmd_len:%u wire_len:%u ext:%u eoff:%u data_off:%u "
            "priv:%u result_req:%u expected:%u actual:%u ret:%d status:0x%x "
            "completion_payload:%u source:%s/%u wait:%s/%u match:%u/%u\n",
            hvdxg.queryadapter_last_type,
            hvdxg.queryadapter_send_route,
            hvdxg.queryadapter_packet_type,
            hvdxg_read_u32_at(hvdxg.queryadapter_packet_cmdhdr,
                              hvdxg.queryadapter_packet_cmdhdr_len, 16),
            hvdxg_read_u32_at(hvdxg.queryadapter_packet_cmdhdr,
                              hvdxg.queryadapter_packet_cmdhdr_len, 12),
            hvdxg_read_u32_at(hvdxg.queryadapter_packet_cmdhdr,
                              hvdxg.queryadapter_packet_cmdhdr_len, 8),
            hvdxg.queryadapter_packet_cmd_len,
            hvdxg.queryadapter_packet_wire_len,
            hvdxg.queryadapter_packet_ext,
            hvdxg.queryadapter_packet_ext_offset,
            hvdxg.queryadapter_packet_data_offset,
            hvdxg.queryadapter_packet_size,
            hvdxg.queryadapter_last_result_len,
            hvdxg.queryadapter_last_expected_wsl_len,
            hvdxg.queryadapter_last_len,
            hvdxg.queryadapter_last_ret,
            (uint32)hvdxg.queryadapter_last_status,
            hvdxg.queryadapter_completion_payload_len,
            hvdxg_channel_name(hvdxg.queryadapter_completion_source_channel),
            hvdxg.queryadapter_completion_source_relid,
            hvdxg_channel_name(hvdxg.queryadapter_completion_waiting_channel),
            hvdxg.queryadapter_completion_waiting_relid,
            hvdxg.queryadapter_completion_waiting_match,
            hvdxg.queryadapter_completion_waiting_channel_match);
    }
	    if (len >= 0 && (size_t)len < status_size) {
	        len += snprintf(status + len, status_size - (size_t)len,
	            "dxg_ntshared_entry=found:%u exact:%u local:0x%x host:0x%x "
            "type:%u parent:0x%x device:0x%x owner:0x%x/%u gen:%u "
            "stale:%u destroyed:%u\n",
            hvdxg.ntshared_obj_found,
            hvdxg.ntshared_obj_exact,
            hvdxg.ntshared_obj_local,
            hvdxg.ntshared_obj_host,
            hvdxg.ntshared_obj_type,
            hvdxg.ntshared_obj_parent,
            hvdxg.ntshared_obj_device,
            hvdxg.ntshared_obj_owner_process,
            hvdxg.ntshared_obj_owner_generation,
            hvdxg.ntshared_obj_generation,
            hvdxg.ntshared_obj_stale,
            hvdxg.ntshared_obj_destroyed);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_ntshared_runtime_object=seen:%u user_obj:0x%x "
            "user_dev:0x%x kind:%u host_obj:0x%x host_dev:0x%x "
            "entry:%u/%u type:%u local:0x%x host:0x%x parent:0x%x "
            "entry_dev:0x%x entry_gen:%u destroyed:%u owner:0x%x/%u/%u\n",
            hvdxg.ntshared_runtime_seen,
            hvdxg.ntshared_runtime_user_object,
            hvdxg.ntshared_runtime_user_device,
            hvdxg.ntshared_runtime_kind,
            hvdxg.ntshared_runtime_host_object,
            hvdxg.ntshared_runtime_host_device,
            hvdxg.ntshared_runtime_entry_found,
            hvdxg.ntshared_runtime_entry_exact,
            hvdxg.ntshared_runtime_entry_type,
            hvdxg.ntshared_runtime_entry_local,
            hvdxg.ntshared_runtime_entry_host,
            hvdxg.ntshared_runtime_entry_parent,
            hvdxg.ntshared_runtime_entry_device,
            hvdxg.ntshared_runtime_entry_generation,
            hvdxg.ntshared_runtime_entry_destroyed,
            hvdxg.ntshared_runtime_owner_process,
            hvdxg.ntshared_runtime_owner_generation,
            hvdxg.ntshared_runtime_owner_refs);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_ntshared_runtime_resource=res:0x%x res_host:0x%x "
            "res_flags:0x%x alloc:0x%x alloc_host:0x%x "
            "alloc_flags:0x%x alloc_size:%lu alloc_owner:0x%x/%u/%u "
            "meta:%u seal:%u->%u host_seal:%u->%u\n",
            hvdxg.ntshared_runtime_resource,
            hvdxg.ntshared_runtime_resource_host,
            hvdxg.ntshared_runtime_resource_flags,
            hvdxg.ntshared_runtime_alloc,
            hvdxg.ntshared_runtime_alloc_host,
            hvdxg.ntshared_runtime_alloc_flags,
            hvdxg.ntshared_runtime_alloc_size,
            hvdxg.ntshared_runtime_alloc_owner_process,
            hvdxg.ntshared_runtime_alloc_owner_generation,
            hvdxg.ntshared_runtime_alloc_owner_refs,
            hvdxg.ntshared_runtime_meta_created,
            hvdxg.ntshared_runtime_sealed_before,
            hvdxg.ntshared_runtime_sealed_after,
            hvdxg.ntshared_runtime_host_sealed_before,
            hvdxg.ntshared_runtime_host_sealed_after);
    }
	    if (len >= 0 && (size_t)len < status_size) {
	        len += snprintf(status + len, status_size - (size_t)len,
	            "dxg_ntshared_runtime_layout=cmd_len:%u wire:%u ext:%u "
	            "eoff:%u obj_off:%u result_req:%u return_len:%u ret:%d "
	            "raw:0x%x handle:0x%x\n",
            hvdxg.ntshared_runtime_cmd_len,
            hvdxg.ntshared_runtime_wire_len,
            hvdxg.ntshared_runtime_ext,
            hvdxg.ntshared_runtime_ext_offset,
            hvdxg.ntshared_runtime_object_offset,
            hvdxg.ntshared_runtime_result_len,
            hvdxg.ntshared_runtime_return_len,
            hvdxg.ntshared_runtime_return_ret,
	            hvdxg.ntshared_runtime_return_raw,
	            hvdxg.ntshared_runtime_return_handle);
	    }
	    if (len >= 0 && (size_t)len < status_size) {
	        len += snprintf(status + len, status_size - (size_t)len,
	            "dxg_ntshared_wsl_object=passed:0x%x resource:0x%x "
	            "res_host:0x%x shared_owner_obj:0x%x cached_nt:0x%x "
	            "pass_is_resource:%u pass_is_shared_owner:%u\n",
	            hvdxg.ntshared_runtime_host_object,
	            hvdxg.ntshared_runtime_resource,
	            hvdxg.ntshared_runtime_resource_host,
	            hvdxg.sharedresource_owner_object,
	            hvdxg.sharedresource_owner_nt,
	            hvdxg.ntshared_runtime_host_object != 0 &&
	            hvdxg.ntshared_runtime_host_object ==
	                hvdxg.ntshared_runtime_resource_host ? 1U : 0U,
	            hvdxg.ntshared_runtime_host_object != 0 &&
	            hvdxg.ntshared_runtime_host_object ==
	                hvdxg.sharedresource_owner_object ? 1U : 0U);
	    }
	    if (len >= 0 && (size_t)len < status_size) {
	        len += snprintf(status + len, status_size - (size_t)len,
	            "dxg_ntshared_runtime_process=current:0x%x/%u "
            "resource_owner:0x%x/%u/%u alloc_owner:0x%x/%u/%u "
            "attempt_proc:0x%x global_proc:0x%x "
            "match_cur_res:%u match_cur_global:%u match_res_global:%u\n",
            hvdxg.sharedhandle_last_current_process,
            hvdxg.sharedhandle_last_current_generation,
            hvdxg.ntshared_runtime_owner_process,
            hvdxg.ntshared_runtime_owner_generation,
            hvdxg.ntshared_runtime_owner_refs,
            hvdxg.ntshared_runtime_alloc_owner_process,
            hvdxg.ntshared_runtime_alloc_owner_generation,
            hvdxg.ntshared_runtime_alloc_owner_refs,
            hvdxg.ntshared_create_attempt_process[0],
            hvdxg.global_send_ntshared.process,
            hvdxg.sharedhandle_last_current_process != 0 &&
            hvdxg.sharedhandle_last_current_process ==
                hvdxg.ntshared_runtime_owner_process ? 1U : 0U,
            hvdxg.sharedhandle_last_current_process != 0 &&
            hvdxg.sharedhandle_last_current_process ==
                hvdxg.global_send_ntshared.process ? 1U : 0U,
	            hvdxg.ntshared_runtime_owner_process != 0 &&
	            hvdxg.ntshared_runtime_owner_process ==
	                hvdxg.global_send_ntshared.process ? 1U : 0U);
	    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_ntshared_pre_classes="
            "res:type:%u/local:0x%x/host:0x%x/gen:%u/destroyed:%u/refs:%u/sealed:%u/open:%u "
            "alloc:type:%u/local:0x%x/host:0x%x/gen:%u/destroyed:%u/refs:%u "
            "dev:type:%u/local:0x%x/host:0x%x/gen:%u/destroyed:%u/refs:%u "
            "shared_owner:found:%u/type:%u/local:0x%x/host:0x%x/gen:%u/destroyed:%u/obj:0x%x/proc:0x%x/refs:%u/nt:0x%x/sealed:%u\n",
            hvdxg.ntshared_pre_resource_type,
            hvdxg.ntshared_pre_resource_local,
            hvdxg.ntshared_pre_resource_host,
            hvdxg.ntshared_pre_resource_generation,
            hvdxg.ntshared_pre_resource_destroyed,
            hvdxg.ntshared_pre_resource_refs,
            hvdxg.ntshared_pre_resource_sealed,
            hvdxg.ntshared_pre_resource_open_count,
            hvdxg.ntshared_pre_alloc_type,
            hvdxg.ntshared_pre_alloc_local,
            hvdxg.ntshared_pre_alloc_host,
            hvdxg.ntshared_pre_alloc_generation,
            hvdxg.ntshared_pre_alloc_destroyed,
            hvdxg.ntshared_pre_alloc_refs,
            hvdxg.ntshared_pre_device_type,
            hvdxg.ntshared_pre_device_local,
            hvdxg.ntshared_pre_device_host,
            hvdxg.ntshared_pre_device_generation,
            hvdxg.ntshared_pre_device_destroyed,
            hvdxg.ntshared_pre_device_refs,
            hvdxg.ntshared_pre_shared_owner_found,
            hvdxg.ntshared_pre_shared_owner_type,
            hvdxg.ntshared_pre_shared_owner_local,
            hvdxg.ntshared_pre_shared_owner_host,
            hvdxg.ntshared_pre_shared_owner_generation,
            hvdxg.ntshared_pre_shared_owner_destroyed,
            hvdxg.ntshared_pre_shared_owner_object,
            hvdxg.ntshared_pre_shared_owner_process,
            hvdxg.ntshared_pre_shared_owner_refs,
            hvdxg.ntshared_pre_shared_owner_nt,
            hvdxg.ntshared_pre_shared_owner_sealed);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_ntshared_handle_fields="
            "res:index:%u/unique:%u/instance:%u "
            "alloc:index:%u/unique:%u/instance:%u "
            "dev:index:%u/unique:%u/instance:%u "
            "shared_owner:index:%u/unique:%u/instance:%u\n",
            hvdxg.ntshared_pre_resource_index,
            hvdxg.ntshared_pre_resource_unique,
            hvdxg.ntshared_pre_resource_instance,
            hvdxg.ntshared_pre_alloc_index,
            hvdxg.ntshared_pre_alloc_unique,
            hvdxg.ntshared_pre_alloc_instance,
            hvdxg.ntshared_pre_device_index,
            hvdxg.ntshared_pre_device_unique,
            hvdxg.ntshared_pre_device_instance,
            hvdxg.ntshared_pre_shared_owner_index,
            hvdxg.ntshared_pre_shared_owner_unique,
            hvdxg.ntshared_pre_shared_owner_instance);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_ntshared_pre_private="
            "runtime:%u/%08x/%08x,%08x,%08x,%08x "
            "resource:%u/%08x/%08x,%08x,%08x,%08x "
            "total:%u/%08x/%08x,%08x,%08x,%08x "
            "alloc_out:%u/%08x/%08x,%08x,%08x,%08x\n",
            hvdxg.ntshared_pre_runtime_size,
            hvdxg.ntshared_pre_runtime_hash,
            hvdxg.ntshared_pre_runtime_w[0],
            hvdxg.ntshared_pre_runtime_w[1],
            hvdxg.ntshared_pre_runtime_w[2],
            hvdxg.ntshared_pre_runtime_w[3],
            hvdxg.ntshared_pre_resource_priv_size,
            hvdxg.ntshared_pre_resource_priv_hash,
            hvdxg.ntshared_pre_resource_priv_w[0],
            hvdxg.ntshared_pre_resource_priv_w[1],
            hvdxg.ntshared_pre_resource_priv_w[2],
            hvdxg.ntshared_pre_resource_priv_w[3],
            hvdxg.ntshared_pre_total_priv_size,
            hvdxg.ntshared_pre_total_priv_hash,
            hvdxg.ntshared_pre_total_priv_w[0],
            hvdxg.ntshared_pre_total_priv_w[1],
            hvdxg.ntshared_pre_total_priv_w[2],
            hvdxg.ntshared_pre_total_priv_w[3],
            hvdxg.ntshared_pre_alloc_out_size,
            hvdxg.ntshared_pre_alloc_out_hash,
            hvdxg.ntshared_pre_alloc_out_w[0],
            hvdxg.ntshared_pre_alloc_out_w[1],
            hvdxg.ntshared_pre_alloc_out_w[2],
            hvdxg.ntshared_pre_alloc_out_w[3]);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_ntshared_pre_order=create_seq:%lu first_nt_before:%lu "
            "event_before:%lu expected_nt_seq:%lu attempts:%u first_label:%u "
            "make:len:%u/ret:%d/host:%d/user:%d/status:0x%x/count:%u/sync:0x%x/fence:%lu/current:%lu "
            "resident:enforced:%u/missing:%u/wait:%u/%d\n",
            hvdxg.ntshared_pre_create_seq,
            hvdxg.ntshared_pre_first_nt_seq,
            hvdxg.ntshared_pre_event_seq,
            hvdxg.ntshared_pre_event_seq + 1,
            hvdxg.ntshared_create_attempts,
            hvdxg.ntshared_create_attempt_label[0],
            hvdxg.makeresident_last_len,
            hvdxg.makeresident_last_ret,
            hvdxg.makeresident_last_host_ret,
            hvdxg.makeresident_last_user_ret,
            (uint32)hvdxg.makeresident_last_status,
            hvdxg.makeresident_last_count,
            hvdxg.makeresident_last_sync,
            hvdxg.makeresident_last_fence,
            hvdxg.makeresident_last_fence_current,
            hvdxg.sharedhandle_last_resident_enforced,
            hvdxg.sharedhandle_last_resident_missing,
            hvdxg.sharedhandle_last_resident_wait_result,
            hvdxg.sharedhandle_last_resident_wait_ret);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_ntshared_wsl_model=proc_state:%u open_proc:0x%x/%u/%u "
            "cmd_proc:0x%x global_proc:0x%x res_owner:0x%x/%u "
            "alloc_owner:0x%x/%u match:open:%u/global:%u/res:%u/alloc:%u "
            "counts:open_obj:%u/proc_obj:%u/dev:%u/ctx:%u/res:%u/alloc:%u "
            "proc_live:%u proc_gen:%u\n",
            hvdxg.ntshared_model_process_state,
            hvdxg.ntshared_model_open_process,
            hvdxg.ntshared_model_open_generation,
            hvdxg.ntshared_model_open_refs,
            hvdxg.ntshared_model_command_process,
            hvdxg.ntshared_model_global_process,
            hvdxg.ntshared_model_resource_owner,
            hvdxg.ntshared_model_resource_generation,
            hvdxg.ntshared_model_alloc_owner,
            hvdxg.ntshared_model_alloc_generation,
            hvdxg.ntshared_model_cmd_eq_open,
            hvdxg.ntshared_model_cmd_eq_global,
            hvdxg.ntshared_model_cmd_eq_resource,
            hvdxg.ntshared_model_cmd_eq_alloc,
            hvdxg.ntshared_model_open_objects,
            hvdxg.ntshared_model_process_objects,
            hvdxg.ntshared_model_devices,
            hvdxg.ntshared_model_contexts,
            hvdxg.ntshared_model_resources,
            hvdxg.ntshared_model_allocations,
            hvdxg.ntshared_model_process_live,
            hvdxg.ntshared_model_process_generation);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_ntshared_failed_cleanup=ret:%d cached:%u->%u "
            "refs:%u->%u object:0x%x->0x%x nt:0x%x->0x%x "
            "sealed:%u->%u cache_inserts:%u->%u "
            "wsl_fd_before_host:0 xv6_fd_before_host:0 "
            "wsl_seal_after_host:1 xv6_seal_after_host:1\n",
            (int32)hvdxg.ntshared_cleanup_ret,
            hvdxg.ntshared_cleanup_cached_before,
            hvdxg.ntshared_cleanup_cached_after,
            hvdxg.ntshared_cleanup_refs_before,
            hvdxg.ntshared_cleanup_refs_after,
            hvdxg.ntshared_cleanup_object_before,
            hvdxg.ntshared_cleanup_object_after,
            hvdxg.ntshared_cleanup_nt_before,
            hvdxg.ntshared_cleanup_nt_after,
            hvdxg.ntshared_cleanup_sealed_before,
            hvdxg.ntshared_cleanup_sealed_after,
            hvdxg.ntshared_cleanup_cache_inserts_before,
            hvdxg.ntshared_cleanup_cache_inserts_after);
    }
	    if (len >= 0 && (size_t)len < status_size) {
	        len += snprintf(status + len, status_size - (size_t)len,
	            "dxg_ntshared_env=a0:l%u/id%lu/t%u/ch%u/p0x%x/o0x%x/off%u/cl%u/wl%u/rl%u/m%u.%u/d%u "
            "a1:l%u/id%lu/t%u/ch%u/p0x%x/o0x%x/off%u/cl%u/wl%u/rl%u/m%u.%u/d%u "
            "a2:l%u/id%lu/t%u/ch%u/p0x%x/o0x%x/off%u/cl%u/wl%u/rl%u/m%u.%u/d%u\n",
            hvdxg.ntshared_create_attempt_label[0],
            hvdxg.ntshared_create_attempt_cmdid[0],
            hvdxg.ntshared_create_attempt_command[0],
            hvdxg.ntshared_create_attempt_channel[0],
            hvdxg.ntshared_create_attempt_process[0],
            hvdxg.ntshared_create_attempt_object[0],
            hvdxg.ntshared_create_attempt_object_offset[0],
            hvdxg.ntshared_create_attempt_cmd_len[0],
            hvdxg.ntshared_create_attempt_wire_len[0],
            hvdxg.ntshared_create_attempt_result_len[0],
            hvdxg.ntshared_create_attempt_monitor[0],
            hvdxg.ntshared_create_attempt_monitorid[0],
            hvdxg.ntshared_create_attempt_dedicated[0],
            hvdxg.ntshared_create_attempt_label[1],
            hvdxg.ntshared_create_attempt_cmdid[1],
            hvdxg.ntshared_create_attempt_command[1],
            hvdxg.ntshared_create_attempt_channel[1],
            hvdxg.ntshared_create_attempt_process[1],
            hvdxg.ntshared_create_attempt_object[1],
            hvdxg.ntshared_create_attempt_object_offset[1],
            hvdxg.ntshared_create_attempt_cmd_len[1],
            hvdxg.ntshared_create_attempt_wire_len[1],
            hvdxg.ntshared_create_attempt_result_len[1],
            hvdxg.ntshared_create_attempt_monitor[1],
            hvdxg.ntshared_create_attempt_monitorid[1],
            hvdxg.ntshared_create_attempt_dedicated[1],
            hvdxg.ntshared_create_attempt_label[2],
            hvdxg.ntshared_create_attempt_cmdid[2],
            hvdxg.ntshared_create_attempt_command[2],
            hvdxg.ntshared_create_attempt_channel[2],
            hvdxg.ntshared_create_attempt_process[2],
            hvdxg.ntshared_create_attempt_object[2],
            hvdxg.ntshared_create_attempt_object_offset[2],
            hvdxg.ntshared_create_attempt_cmd_len[2],
            hvdxg.ntshared_create_attempt_wire_len[2],
            hvdxg.ntshared_create_attempt_result_len[2],
            hvdxg.ntshared_create_attempt_monitor[2],
            hvdxg.ntshared_create_attempt_monitorid[2],
            hvdxg.ntshared_create_attempt_dedicated[2]);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_ntshared_env2=a3:l%u/id%lu/t%u/ch%u/p0x%x/o0x%x/off%u/cl%u/wl%u/rl%u/m%u.%u/d%u "
            "a4:l%u/id%lu/t%u/ch%u/p0x%x/o0x%x/off%u/cl%u/wl%u/rl%u/m%u.%u/d%u\n",
            hvdxg.ntshared_create_attempt_label[3],
            hvdxg.ntshared_create_attempt_cmdid[3],
            hvdxg.ntshared_create_attempt_command[3],
            hvdxg.ntshared_create_attempt_channel[3],
            hvdxg.ntshared_create_attempt_process[3],
            hvdxg.ntshared_create_attempt_object[3],
            hvdxg.ntshared_create_attempt_object_offset[3],
            hvdxg.ntshared_create_attempt_cmd_len[3],
            hvdxg.ntshared_create_attempt_wire_len[3],
            hvdxg.ntshared_create_attempt_result_len[3],
            hvdxg.ntshared_create_attempt_monitor[3],
            hvdxg.ntshared_create_attempt_monitorid[3],
            hvdxg.ntshared_create_attempt_dedicated[3],
            hvdxg.ntshared_create_attempt_label[4],
            hvdxg.ntshared_create_attempt_cmdid[4],
            hvdxg.ntshared_create_attempt_command[4],
            hvdxg.ntshared_create_attempt_channel[4],
            hvdxg.ntshared_create_attempt_process[4],
            hvdxg.ntshared_create_attempt_object[4],
            hvdxg.ntshared_create_attempt_object_offset[4],
            hvdxg.ntshared_create_attempt_cmd_len[4],
            hvdxg.ntshared_create_attempt_wire_len[4],
            hvdxg.ntshared_create_attempt_result_len[4],
            hvdxg.ntshared_create_attempt_monitor[4],
            hvdxg.ntshared_create_attempt_monitorid[4],
            hvdxg.ntshared_create_attempt_dedicated[4]);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_sharedresource_ntdiag=res_host:0x%x alloc_host:0x%x "
            "res_flags:0x%x alloc_flags:0x%x alloc_hash:%08x/%08x "
            "alloc_size:%lu meta:%u->%u seal:%u->%u host_seal:%u->%u\n",
            hvdxg.sharedresource_nt_resource_host,
            hvdxg.sharedresource_nt_alloc_host,
            hvdxg.sharedresource_nt_resource_flags,
            hvdxg.sharedresource_nt_alloc_flags,
            hvdxg.sharedresource_nt_alloc_in_hash,
            hvdxg.sharedresource_nt_alloc_out_hash,
            hvdxg.sharedresource_nt_alloc_size,
            hvdxg.sharedresource_nt_meta_before,
            hvdxg.sharedresource_nt_meta_after,
            hvdxg.sharedresource_nt_seal_before,
            hvdxg.sharedresource_nt_seal_after,
            hvdxg.sharedresource_nt_host_seal_before,
            hvdxg.sharedresource_nt_host_seal_after);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_existing_sysmem_target=pfnmap_pages:%u pfnmap_ok:%u "
            "vram:%u vram_gpa:0x%lx vram_size:%lu fb:0x%lx/%lu "
            "meaning:pfnmap_allows_synthvid_vram_existing_sysmem_test\n",
            hvdxg.existing_sysmem_last_pfnmap_pages,
            hvdxg.existing_sysmem_pfnmap_successes,
            hvdxg.existing_sysmem_last_vram,
            hvdxg.existing_sysmem_last_vram_gpa,
            hvdxg.existing_sysmem_last_vram_size,
            platform.has_framebuffer ? platform.framebuffer_base : 0,
            platform.has_framebuffer ? platform.framebuffer_size : 0);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_existing_sysmem_ntbridge=stage:%u resource:0x%x "
            "global:0x%x flags:0x%x host_flags:0x%x metadata:%u "
            "sealed:%u nt:0x%x shareable:%u reason:%u allocs:%u "
            "runtime:%u resource_priv:%u alloc_priv:%u total:%u "
            "pfnmap:%u vram:%u va:0x%lx size:%lu\n",
            hvdxg.existing_sysmem_share_stage,
            hvdxg.existing_sysmem_share_resource,
            hvdxg.existing_sysmem_share_global,
            hvdxg.existing_sysmem_share_flags,
            hvdxg.existing_sysmem_share_host_flags,
            hvdxg.existing_sysmem_share_metadata,
            hvdxg.existing_sysmem_share_sealed,
            hvdxg.existing_sysmem_share_nt,
            hvdxg.existing_sysmem_share_shareable,
            hvdxg.existing_sysmem_share_reason,
            hvdxg.existing_sysmem_share_alloc_count,
            hvdxg.existing_sysmem_share_runtime_priv,
            hvdxg.existing_sysmem_share_resource_priv,
            hvdxg.existing_sysmem_share_alloc_priv,
            hvdxg.existing_sysmem_share_total_priv,
            hvdxg.existing_sysmem_share_pfnmap_pages,
            hvdxg.existing_sysmem_share_vram,
            hvdxg.existing_sysmem_share_va,
            hvdxg.existing_sysmem_share_size);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_sharedresource_seal_verify=tracked:%u allocs:%u "
            "expected_priv:%u actual_priv:%u ret:%d missing:0x%x "
            "extra:0x%x append_rejects:%u resource:0x%x generation:%u\n",
            hvdxg.sharedresource_seal_tracked_allocs,
            hvdxg.sharedresource_seal_allocs,
            hvdxg.sharedresource_seal_expected_private,
            hvdxg.sharedresource_seal_actual_private,
            hvdxg.sharedresource_seal_verify_ret,
            hvdxg.sharedresource_seal_missing_alloc,
            hvdxg.sharedresource_seal_extra_alloc,
            hvdxg.sharedresource_seal_append_rejects,
            hvdxg.sharedresource_seal_last_resource,
            hvdxg.sharedresource_seal_last_generation);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "d3dkmt_process_namespace=tgid:%lu handle:0x%x "
            "source_gen:%u new_gen:%u objects:%u->%u "
            "adapters:%u->%u locals:%u->%u fresh:%u\n",
            hvdxg.process_namespace_last_tgid,
            hvdxg.process_namespace_last_handle,
            hvdxg.process_namespace_source_generation,
            hvdxg.process_namespace_new_generation,
            hvdxg.process_namespace_objects_before,
            hvdxg.process_namespace_objects_after,
            hvdxg.process_namespace_adapters_before,
            hvdxg.process_namespace_adapters_after,
            hvdxg.process_namespace_locals_before,
            hvdxg.process_namespace_locals_after,
            hvdxg.process_namespace_fresh);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_ioctl_tgid_gate=checks:%u passes:%u denied:%u "
            "cmd:0x%x current_tgid:%lu owner_tgid:%lu owner_gen:%u "
            "ret:%d process_required:%u\n",
            hvdxg.ioctl_tgid_gate_checks,
            hvdxg.ioctl_tgid_gate_passes,
            hvdxg.ioctl_tgid_gate_denied,
            hvdxg.ioctl_tgid_gate_last_cmd,
            hvdxg.ioctl_tgid_gate_current_tgid,
            hvdxg.ioctl_tgid_gate_owner_tgid,
            hvdxg.ioctl_tgid_gate_owner_generation,
            hvdxg.ioctl_tgid_gate_last_ret,
            hvdxg.ioctl_tgid_gate_process_required);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_sharedhandle_copyout=failures:%u kind:%u proc:0x%x "
            "object:0x%x nt:0x%x fd:%u reclaimed:%u refs_after:%u "
            "ret:%d\n",
            hvdxg.sharedhandle_copyout_failures,
            hvdxg.sharedhandle_copyout_last_kind,
            hvdxg.sharedhandle_copyout_last_process,
            hvdxg.sharedhandle_copyout_last_object,
            hvdxg.sharedhandle_copyout_last_nt,
            hvdxg.sharedhandle_copyout_last_fd,
            hvdxg.sharedhandle_copyout_last_reclaimed,
            hvdxg.sharedhandle_copyout_last_refs_after,
            hvdxg.sharedhandle_copyout_last_ret);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_opensync_namespace=current_tgid:%lu owner_tgid:%lu "
            "owner_gen:%u mismatch:%u rejects:%u gate:%u ret:%d "
            "device:0x%x source_dev:0x%x global:0x%x\n",
            hvdxg.opensync_last_current_tgid,
            hvdxg.opensync_last_owner_tgid,
            hvdxg.opensync_last_owner_generation,
            hvdxg.opensync_last_namespace_mismatch,
            hvdxg.opensync_last_namespace_rejects,
            hvdxg.opensync_last_gate,
            hvdxg.opensync_last_ret,
            hvdxg.opensync_last_device,
            hvdxg.opensync_last_source_device,
            hvdxg.opensync_last_global);
    }
    hvdxg_status_append_hex(status, status_size, &len,
                            "dxg_createallocation_result_raw",
                            hvdxg.allocation_last_result_head,
                            hvdxg.allocation_last_result_head_len);
    hvdxg_status_append_hex(status, status_size, &len,
                            "dxg_d3d12_shared_result_raw",
                            hvdxg.d3d12_shared_result_head,
                            hvdxg.d3d12_shared_result_head_len);
    hvdxg_status_append_hex(status, status_size, &len,
                            "dxg_syncobject_result_raw",
                            hvdxg.syncobject_last_result_head,
                            hvdxg.syncobject_last_result_head_len);
    hvdxg_status_append_ioctl_timing(status, status_size, &len,
                                     ioctl_top_nr);
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_d3d12_shared_alloc=seen:%u len:%u ret:%d "
            "dev:0x%x res_in:0x%x res_out:0x%x alloc:0x%x "
            "count:%u flags:0x%x d3d_flags:0x%x global:0x%x process:0x%lx "
            "size:%lu rt_resource:0x%lx runtime:%u res_priv:%u "
            "alloc_priv:%u out_priv:%u\n",
            hvdxg.d3d12_shared_alloc_seen,
            hvdxg.d3d12_shared_alloc_len,
            hvdxg.d3d12_shared_alloc_ret,
            hvdxg.d3d12_shared_alloc_device,
            hvdxg.d3d12_shared_alloc_resource_in,
            hvdxg.d3d12_shared_alloc_resource_out,
            hvdxg.d3d12_shared_alloc_allocation,
            hvdxg.d3d12_shared_alloc_count,
            hvdxg.d3d12_shared_alloc_flags,
            hvdxg.d3d12_shared_runtime_d3d12_flags,
            hvdxg.d3d12_shared_alloc_global_share,
            hvdxg.d3d12_shared_alloc_process,
            hvdxg.d3d12_shared_alloc_size,
            hvdxg.d3d12_shared_alloc_rt_resource,
            hvdxg.d3d12_shared_runtime_size,
            hvdxg.d3d12_shared_resource_priv_size,
            hvdxg.d3d12_shared_alloc_priv_size,
            hvdxg.d3d12_shared_alloc_out_priv_size);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_d3d12_shared_create=wire_flags:0x%x "
            "create:%u shared:%u nt:%u nonsec:%u std:%u "
            "wire_make:%u wire_rt:0x%lx host_result_flags:0x%x "
            "result_global:0x%x result_vgpu:0x%x result_alloc_flags:0x%x "
            "result_min:%u result_len:%u result_flags_off:%u "
            "result_res_off:%u result_global_off:%u result_vgpu_off:%u "
            "result_allocinfo_off:%u result_allocinfo_size:%u "
            "result_head:%u "
            "result_norm:%u norm_reason:%u candidate:0x%x delta:0x%x "
            "track_shared:%u track_nt:%u track_meta:%u track_sent:%u\n",
            hvdxg.d3d12_shared_wire_flags,
            (hvdxg.d3d12_shared_wire_flags & 0x1U) != 0 ? 1 : 0,
            (hvdxg.d3d12_shared_wire_flags & 0x2U) != 0 ? 1 : 0,
            (hvdxg.d3d12_shared_wire_flags & 0x40U) != 0 ? 1 : 0,
            (hvdxg.d3d12_shared_wire_flags & 0x4U) != 0 ? 1 : 0,
            (hvdxg.d3d12_shared_wire_flags & 0x10000U) != 0 ? 1 : 0,
            hvdxg.d3d12_shared_wire_make_resident,
            hvdxg.d3d12_shared_wire_rt_resource,
            hvdxg.d3d12_shared_result_flags,
            hvdxg.d3d12_shared_result_global_share,
            hvdxg.d3d12_shared_result_vgpu_flags,
            hvdxg.d3d12_shared_result_alloc_flags,
            hvdxg.d3d12_shared_result_min_len,
            hvdxg.d3d12_shared_result_len,
            hvdxg.d3d12_shared_result_flags_offset,
            hvdxg.d3d12_shared_result_resource_offset,
            hvdxg.d3d12_shared_result_global_offset,
            hvdxg.d3d12_shared_result_vgpu_offset,
            hvdxg.d3d12_shared_result_allocinfo_offset,
            hvdxg.d3d12_shared_result_allocinfo_size,
            hvdxg.d3d12_shared_result_head_len,
            hvdxg.d3d12_shared_result_flag_norm,
            hvdxg.d3d12_shared_result_flag_norm_reason,
            hvdxg.d3d12_shared_result_flag_candidate,
            hvdxg.d3d12_shared_result_flag_delta,
            hvdxg.d3d12_shared_track_shared,
            hvdxg.d3d12_shared_track_nt,
            hvdxg.d3d12_shared_track_metadata,
            hvdxg.d3d12_shared_track_sent_bytes);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_d3d12_private_layout=allocinfo_off:%u private_off:%u "
            "runtime_off:%u resource_off:%u alloc0_off:%u "
            "runtime:%u resource:%u alloc0:%u hashes:%08x/%08x/%08x\n",
            hvdxg.d3d12_shared_allocinfo_offset,
            hvdxg.allocation_last_private_offset,
            hvdxg.d3d12_shared_runtime_offset,
            hvdxg.d3d12_shared_resource_priv_offset,
            hvdxg.d3d12_shared_alloc_priv_offset,
            hvdxg.d3d12_shared_runtime_size,
            hvdxg.d3d12_shared_resource_priv_size,
            hvdxg.d3d12_shared_alloc_priv_size,
            hvdxg_hash_bytes(hvdxg.d3d12_shared_runtime_head,
                             hvdxg.d3d12_shared_runtime_head_len),
            hvdxg_hash_bytes(hvdxg.d3d12_shared_resource_priv_head,
                             hvdxg.d3d12_shared_resource_priv_head_len),
            hvdxg_hash_bytes(hvdxg.d3d12_shared_alloc_priv_head,
                             hvdxg.d3d12_shared_alloc_priv_head_len));
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_d3d12_runtime_copy=user_ret:%d mismatch:%u "
            "user_len:%u sent_len:%u user_hash:%08x sent_hash:%08x "
            "user_q08:%lx user_q10:%lx user_q38:%lx user_q50:%lx user_q58:%lx user_d3c:%08x "
            "sent_q08:%lx sent_q10:%lx sent_q38:%lx sent_q50:%lx sent_q58:%lx sent_d3c:%08x\n",
            hvdxg.d3d12_shared_runtime_user_copy_ret,
            hvdxg.d3d12_shared_runtime_user_mismatch,
            hvdxg.d3d12_shared_runtime_user_head_len,
            hvdxg.d3d12_shared_runtime_head_len,
            hvdxg_hash_bytes(hvdxg.d3d12_shared_runtime_user_head,
                             hvdxg.d3d12_shared_runtime_user_head_len),
            hvdxg_hash_bytes(hvdxg.d3d12_shared_runtime_head,
                             hvdxg.d3d12_shared_runtime_head_len),
            hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_user_head,
                              hvdxg.d3d12_shared_runtime_user_head_len, 0x8),
            hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_user_head,
                              hvdxg.d3d12_shared_runtime_user_head_len, 0x10),
            hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_user_head,
                              hvdxg.d3d12_shared_runtime_user_head_len, 0x38),
            hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_user_head,
                              hvdxg.d3d12_shared_runtime_user_head_len, 0x50),
            hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_user_head,
                              hvdxg.d3d12_shared_runtime_user_head_len, 0x58),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_user_head,
                              hvdxg.d3d12_shared_runtime_user_head_len, 0x3c),
            hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x8),
            hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x10),
            hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x38),
            hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x50),
            hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x58),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x3c));
    }
    if (len >= 0 && (size_t)len < status_size) {
        uint64 q38 = hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_head,
                                       hvdxg.d3d12_shared_runtime_head_len,
                                       0x38);
        uint32 d3c = hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                                       hvdxg.d3d12_shared_runtime_head_len,
                                       0x3c);
        uint32 rt_hi = (uint32)(hvdxg.d3d12_shared_wire_rt_resource >> 32);
        uint32 q38_hi = (uint32)(q38 >> 32);

        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_d3d12_runtime_pointer=rt_handle:0x%lx rt_hi:0x%x "
            "q38:0x%lx q38_hi:0x%x d3c:0x%x hi_match:%u q38_zero:%u\n",
            hvdxg.d3d12_shared_wire_rt_resource,
            rt_hi,
            q38,
            q38_hi,
            d3c,
            rt_hi != 0 && rt_hi == q38_hi ? 1 : 0,
            q38 == 0 ? 1 : 0);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_d3d12_runtime_desc=base:0xbc dim:%u align:%lu "
            "width:%lu height:%u depth:%u mips:%u fmt:%u sample:%u/%u "
            "layout:%u flags:0x%x raw_b8:%08x raw_bc:%08x raw_ec:%08x "
            "trusted:%u\n",
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0xbc),
            hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0xc4),
            hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0xcc),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0xd4),
            hvdxg_read_u16_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0xd8),
            hvdxg_read_u16_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0xda),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0xdc),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0xe0),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0xe4),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0xe8),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0xec),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0xb8),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0xbc),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0xec),
            0);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_d3d12_runtime_candidates=q08:%lx q10:%lx "
            "q20:%lx q28:%lx q38:%lx q40:%lx q48:%lx q50:%lx q58:%lx "
            "d20:%08x d28:%08x d38:%08x d3c:%08x d40:%08x d48:%08x d50:%08x d58:%08x "
            "legacy_b8:%08x legacy_bc:%08x legacy_ec:%08x\n",
            hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x8),
            hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x10),
            hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x20),
            hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x28),
            hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x38),
            hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x40),
            hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x48),
            hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x50),
            hvdxg_read_u64_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x58),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x20),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x28),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x38),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x3c),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x40),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x48),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x50),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0x58),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0xb8),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0xbc),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_runtime_head,
                              hvdxg.d3d12_shared_runtime_head_len, 0xec));
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_d3d12_alloc_priv_words=w0:%08x w1:%08x w2:%08x "
            "w3:%08x w4:%08x w5:%08x w6:%08x w7:%08x "
            "w16:%08x w32:%08x w64:%08x w128:%08x\n",
            hvdxg_read_u32_at(hvdxg.d3d12_shared_alloc_priv_head,
                              hvdxg.d3d12_shared_alloc_priv_head_len, 0),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_alloc_priv_head,
                              hvdxg.d3d12_shared_alloc_priv_head_len, 4),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_alloc_priv_head,
                              hvdxg.d3d12_shared_alloc_priv_head_len, 8),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_alloc_priv_head,
                              hvdxg.d3d12_shared_alloc_priv_head_len, 12),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_alloc_priv_head,
                              hvdxg.d3d12_shared_alloc_priv_head_len, 16),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_alloc_priv_head,
                              hvdxg.d3d12_shared_alloc_priv_head_len, 20),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_alloc_priv_head,
                              hvdxg.d3d12_shared_alloc_priv_head_len, 24),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_alloc_priv_head,
                              hvdxg.d3d12_shared_alloc_priv_head_len, 28),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_alloc_priv_head,
                              hvdxg.d3d12_shared_alloc_priv_head_len, 64),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_alloc_priv_head,
                              hvdxg.d3d12_shared_alloc_priv_head_len, 128),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_alloc_priv_head,
                              hvdxg.d3d12_shared_alloc_priv_head_len, 256),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_alloc_priv_head,
                              hvdxg.d3d12_shared_alloc_priv_head_len, 512));
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_d3d12_private_normalize=seen:%u applied:%u reason:%u "
            "magic:%08x/%08x w4:%08x->%08x w8:%08x->%08x "
            "runtime_patch:%u wh:%u/%u rt8:%lx->%lx rt10:%lx->%lx "
            "rt38:%lx->%lx rt50:%lx->%lx rt58:%lx->%lx "
            "runtime_reason:bgra8_guarded optin:dxg_d3d12_private_normalize\n",
            hvdxg.d3d12_shared_norm_seen,
            hvdxg.d3d12_shared_norm_applied,
            hvdxg.d3d12_shared_norm_reason,
            hvdxg.d3d12_shared_norm_magic0,
            hvdxg.d3d12_shared_norm_magic3,
            hvdxg.d3d12_shared_norm_pre_w4,
            hvdxg.d3d12_shared_norm_post_w4,
            hvdxg.d3d12_shared_norm_pre_w8,
            hvdxg.d3d12_shared_norm_post_w8,
            hvdxg.d3d12_shared_norm_runtime_applied,
            hvdxg.d3d12_shared_norm_width,
            hvdxg.d3d12_shared_norm_height,
            hvdxg.d3d12_shared_norm_pre_rt8,
            hvdxg.d3d12_shared_norm_post_rt8,
            hvdxg.d3d12_shared_norm_pre_rt10,
            hvdxg.d3d12_shared_norm_post_rt10,
            hvdxg.d3d12_shared_norm_pre_rt38,
            hvdxg.d3d12_shared_norm_post_rt38,
            hvdxg.d3d12_shared_norm_pre_rt50,
            hvdxg.d3d12_shared_norm_post_rt50,
            hvdxg.d3d12_shared_norm_pre_rt58,
            hvdxg.d3d12_shared_norm_post_rt58);
    }
    if (len >= 0 && (size_t)len < status_size) {
        len += snprintf(status + len, status_size - (size_t)len,
            "dxg_d3d12_alloc_priv_io=in_hash:%08x out_hash:%08x "
            "in_w4:%08x in_w8:%08x out_w4:%08x out_w8:%08x "
            "out_len:%u track_host:%u\n",
            hvdxg_hash_bytes(hvdxg.d3d12_shared_alloc_priv_head,
                             hvdxg.d3d12_shared_alloc_priv_head_len),
            hvdxg_hash_bytes(hvdxg.d3d12_shared_alloc_out_priv_head,
                             hvdxg.d3d12_shared_alloc_out_priv_head_len),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_alloc_priv_head,
                              hvdxg.d3d12_shared_alloc_priv_head_len, 0x10),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_alloc_priv_head,
                              hvdxg.d3d12_shared_alloc_priv_head_len, 0x20),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_alloc_out_priv_head,
                              hvdxg.d3d12_shared_alloc_out_priv_head_len,
                              0x10),
            hvdxg_read_u32_at(hvdxg.d3d12_shared_alloc_out_priv_head,
                              hvdxg.d3d12_shared_alloc_out_priv_head_len,
                              0x20),
            hvdxg.d3d12_shared_alloc_out_priv_head_len,
            hvdxg.d3d12_shared_track_alloc_from_host);
    }
    hvdxg_status_append_hex(status, status_size, &len,
                            "dxg_d3d12_shared_runtime",
                            hvdxg.d3d12_shared_runtime_head,
                            hvdxg.d3d12_shared_runtime_head_len);
    hvdxg_status_append_hex(status, status_size, &len,
                            "dxg_d3d12_shared_resource_priv",
                            hvdxg.d3d12_shared_resource_priv_head,
                            hvdxg.d3d12_shared_resource_priv_head_len);
    hvdxg_status_append_hex(status, status_size, &len,
                            "dxg_d3d12_shared_alloc_priv",
                            hvdxg.d3d12_shared_alloc_priv_head,
                            hvdxg.d3d12_shared_alloc_priv_head_len);
    hvdxg_status_append_hex(status, status_size, &len,
                            "dxg_d3d12_shared_alloc_out_priv",
                            hvdxg.d3d12_shared_alloc_out_priv_head,
                            hvdxg.d3d12_shared_alloc_out_priv_head_len);
    hvdxg_status_append_hex(status, status_size, &len, "dxg_alloc_priv_in",
                            hvdxg.allocation_last_in_priv_head,
                            hvdxg.allocation_last_in_priv_head_len);
    hvdxg_status_append_hex(status, status_size, &len, "dxg_alloc_priv_out",
                            hvdxg.allocation_last_out_priv_head,
                            hvdxg.allocation_last_out_priv_head_len);
    hvdxg_status_append_hex(status, status_size, &len, "dxg_context_priv",
                            hvdxg.createcontext_last_priv_head,
                            hvdxg.createcontext_last_priv_head_len);
    hvdxg_status_append_hex(status, status_size, &len, "dxg_hwqueue_priv",
                            hvdxg.createhwqueue_last_priv_head,
                            hvdxg.createhwqueue_last_priv_head_len);
    if ((size_t)len >= status_size) {
        size_t mark = status_size > 96 ? status_size - 96 : 0;

        snprintf(status + mark, status_size - mark,
                 "\ndxg_status_truncated=1 wanted:%d size:%u\n",
                 len, HV_DXG_STATUS_BUF_SIZE);
        len = status_size - 1;
    }
    if (read_status != NULL && read_status_len != NULL) {
        *read_status = status;
        *read_status_len = (size_t)len;
        cached = 1;
    }

copy_cached:
    size_t total = cached && read_status_len != NULL ?
                   *read_status_len : (size_t)len;
    if (*read_offset >= total) {
        *read_emitted = 1;
        if (!cached)
            kvfree(status);
        return 0;
    }
    size_t out = min(count, total - *read_offset);
    if (either_copyout(user, (uint64)buf, status + *read_offset, out) < 0) {
        if (!cached)
            kvfree(status);
        return -EFAULT;
    }
    *read_offset += out;
    if (*read_offset >= total)
        *read_emitted = 1;
    if (!cached)
        kvfree(status);
    return (int)out;
}
