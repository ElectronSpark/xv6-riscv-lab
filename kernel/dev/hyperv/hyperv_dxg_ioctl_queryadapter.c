/*
 * Hyper-V DXG: QueryAdapterInfo ioctl and its self-contained helpers.
 *
 * Part of the dev/hyperv unity translation unit (included by module.c).
 * Split out of hyperv_dxg_ioctls.c for readability; include order in
 * module.c preserves the original definition order.
 */

static void hvdxg_note_unsupported_ioctl(uint32 cmd, uint32 device,
                                         uint32 handle, uint32 count)
{
    hvdxg.unsupported_last_cmd = cmd;
    hvdxg.unsupported_last_ret = -ENOTSUP;
    hvdxg.unsupported_last_device = device;
    hvdxg.unsupported_last_handle = handle;
    hvdxg.unsupported_last_count = count;
    hvdxg.unsupported_last_nr = cmd & 0xffU;
    hvdxg.unsupported_last_size = (cmd >> _IOC_SIZESHIFT) &
                                  ((1U << _IOC_SIZEBITS) - 1U);
    hvdxg.unsupported_last_name =
        cmd == LX_DXMARKDEVICEASERROR ? 1U : 0U;
}

static uint32 hvdxg_queryadapter_wsl_command_len(uint32 private_data_size)
{
    return __builtin_offsetof(struct hvdxg_command_queryadapterinfo_wsl,
                              private_data) + private_data_size;
}

static uint32 hvdxg_queryadapter_private_max(void)
{
    uint32 command_max = HV_DXG_VM_BUS_PACKET_MAX -
                         (HV_DXG_QUERYADAPTERINFO_WSL_NATURAL_BASE - 1U);
    uint32 result_max = HV_DXG_VM_BUS_PACKET_MAX -
                        sizeof(struct hvdxg_ntstatus);

    return command_max < result_max ? command_max : result_max;
}

static void hvdxg_fill_render_adapter_type(struct d3dkmt_adaptertype *type)
{
    /*
     * WSL patches adapter type replies after the host QAI completion:
     * force GPU-PV/paravirtualized shape and hide display/post/indirect/ACG
     * timing capabilities from Linux userspace.
     */
    uint32 raw;

    if (type == NULL)
        return;
    raw = type->value;
    type->render_supported = 1;
    type->paravirtualized = 1;
    type->display_supported = 0;
    type->post_device = 0;
    type->indirect_display_device = 0;
    type->acg_supported = 0;
    type->support_set_timings_from_vidpn = 0;
    hvdxg.queryadapter_adaptertype_raw_value = raw;
    hvdxg.queryadapter_adaptertype_wsl_value = type->value;
    hvdxg.queryadapter_adaptertype_cleared_bits = raw & ~type->value;
    hvdxg.queryadapter_adaptertype_forced_bits = type->value & ~raw;
    hvdxg.queryadapter_adaptertype_compute_only = type->compute_only;
    hvdxg.queryadapter_adaptertype_last_raw_value = raw;
    hvdxg.queryadapter_adaptertype_last_wsl_value = type->value;
}

static int hvdxg_ioctl_queryadapterinfo(uint64 arg,
                                        struct hvdxg_open_state *owner)
{
    struct d3dkmt_queryadapterinfo req;
    uint8 *command_buf = NULL;
    uint8 *result_buf = NULL;
    struct hvdxg_command_queryadapterinfo_wsl *query;
    uint32 command_len;
    uint32 result_len;
    uint32 actual_len = 0;
    uint32 private_max = hvdxg_queryadapter_private_max();
    uint8 *private_data;
    uint32 primary_len = 0;
    uint32 host_adapter = 0;
    uint32 resolve_source = 0;
    uint32 query_source = 0;
    int alias_adapter = 0;
    int primary_ret = 0;
    int32 primary_status = 0;
    int process_locked = 0;
    struct hvdxg_d3dkmthandle query_process;
    struct hvdxg_queryadapter_context query_context;
    const struct hvdxg_winluid *query_ext_luid = NULL;
    int ret;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    ret = hvdxg_d3dkmt_ensure_adapter();
    if (ret != 0)
        return ret;

    hvdxg.queryadapter_last_type = (uint32)req.type;
	    hvdxg.queryadapter_last_size = req.private_data_size;
	    hvdxg.queryadapter_last_len = 0;
	    hvdxg.queryadapter_last_user_len = 0;
	    hvdxg.queryadapter_last_ret = 0;
    hvdxg.queryadapter_last_status = 0;
    hvdxg.queryadapter_last_layout = 0;
    hvdxg.queryadapter_last_cmd_len = 0;
    hvdxg.queryadapter_last_type_offset = 0;
    hvdxg.queryadapter_last_size_offset = 0;
    hvdxg.queryadapter_last_data_offset = 0;
    hvdxg.queryadapter_last_adapter = req.adapter.v;
    hvdxg.queryadapter_last_host_adapter = 0;
    hvdxg.queryadapter_last_resolve_source = 0;
    hvdxg.queryadapter_last_owner_process =
        owner != NULL ? owner->dxg_process.v : 0;
    hvdxg.queryadapter_last_owner_generation =
        hvdxg_open_process_generation(owner);
    hvdxg.queryadapter_last_owner_refs = hvdxg_open_process_refs(owner);
    hvdxg.queryadapter_last_result_len = 0;
    hvdxg.queryadapter_last_expected_wsl_len =
        req.private_data_size + sizeof(struct hvdxg_ntstatus);
    hvdxg.queryadapter_last_process_source = 0;
    hvdxg.queryadapter_last_adapter_object = 0;
    hvdxg.queryadapter_last_adapter_object_host = 0;
    hvdxg.queryadapter_last_adapter_object_owner = 0;
    hvdxg.queryadapter_last_adapter_object_owner_generation = 0;
    hvdxg.queryadapter_last_adapter_object_generation = 0;
    hvdxg.queryadapter_local_namespace = 0;
    hvdxg.queryadapter_process_adapter_refs = 0;
    hvdxg.queryadapter_process_adapter_locals = 0;
    hvdxg.queryadapter_process_adapter_generation = 0;
	    hvdxg.queryadapter_type15_fail_ret = 0;
	    hvdxg.queryadapter_type15_fail_status = 0;
	    hvdxg.queryadapter_type15_fail_process = 0;
	    hvdxg.queryadapter_type15_fail_route = 0;
	    hvdxg.queryadapter_type15_fail_ext_luid_low = 0;
	    hvdxg.queryadapter_type15_fail_ext_luid_high = 0;
    hvdxg.queryadapter_adaptertype_rewrite_type = 0;
    hvdxg.queryadapter_adaptertype_rewrite_source = 0;
    hvdxg.queryadapter_adaptertype_raw_value = 0;
    hvdxg.queryadapter_adaptertype_wsl_value = 0;
    hvdxg.queryadapter_adaptertype_cleared_bits = 0;
    hvdxg.queryadapter_adaptertype_forced_bits = 0;
    hvdxg.queryadapter_adaptertype_compute_only = 0;
	    hvdxg.queryadapter_zero_success_host_type = 0;
	    hvdxg.queryadapter_zero_success_host_len = 0;
	    hvdxg.queryadapter_zero_success_host_ret = 0;
	    hvdxg.queryadapter_zero_success_host_status = 0;
	    hvdxg.queryadapter_zero_success_user_ret = 0;
	    hvdxg.queryadapter_source_last = 0;
    hvdxg.queryadapter_alias_cache_last_type = 0;
    hvdxg.queryadapter_alias_cache_last_size = 0;
    hvdxg.queryadapter_alias_cache_last_len = 0;
    hvdxg.queryadapter_alias_cache_last_alias = 0;
    hvdxg.queryadapter_alias_cache_last_host = 0;
    hvdxg.queryadapter_alias_cache_last_hash = 0;
    hvdxg.queryadapter_alias_cache_last_result = 0;
    hvdxg.queryadapter_alias_staged_type = 0;
    hvdxg.queryadapter_alias_staged_size = 0;
    hvdxg.queryadapter_alias_staged_alias = 0;
    hvdxg.queryadapter_alias_staged_host = 0;
    hvdxg.queryadapter_alias_staged_path = 0;
    hvdxg.queryadapter_alias_staged_ret = 0;
    hvdxg.queryadapter_send_route = 0;
    hvdxg.queryadapter_send_ext_luid_low = 0;
    hvdxg.queryadapter_send_ext_luid_high = 0;
    hvdxg.queryadapter_completion_desc_type = 0;
    hvdxg.queryadapter_completion_desc_flags = 0;
    hvdxg.queryadapter_completion_desc_len8 = 0;
    hvdxg.queryadapter_completion_desc_offset8 = 0;
    hvdxg.queryadapter_completion_packet_len = 0;
    hvdxg.queryadapter_completion_packet_offset = 0;
    hvdxg.queryadapter_completion_payload_len = 0;
    hvdxg.queryadapter_completion_trans_id = 0;
    hvdxg.queryadapter_completion_waiting_trans_id = 0;
    hvdxg.queryadapter_completion_source_channel = HV_DXG_CHANNEL_NONE;
    hvdxg.queryadapter_completion_source_relid = 0;
    hvdxg.queryadapter_completion_waiting_channel = HV_DXG_CHANNEL_NONE;
    hvdxg.queryadapter_completion_waiting_relid = 0;
    hvdxg.queryadapter_completion_waiting_match = 0;
    hvdxg.queryadapter_completion_waiting_channel_match = 0;
    hvdxg.queryadapter_completion_type = 0;
    hvdxg.queryadapter_completion_len = 0;
    memset(hvdxg.queryadapter_completion_prefix, 0,
           sizeof(hvdxg.queryadapter_completion_prefix));
    hvdxg_reset_queryadapter_return_head();
    if (req.private_data == 0 || req.private_data_size == 0 ||
        req.private_data_size > private_max) {
        ret = -EINVAL;
        hvdxg.queryadapter_last_ret = ret;
        hvdxg_note_queryadapter_history((uint32)req.type,
            req.private_data_size, ret, 0);
        return ret;
    }

    if (owner != NULL) {
        mutex_lock(&hvdxg.process_lock);
        process_locked = 1;
        ret = hvdxg_bind_open_process(owner);
        hvdxg.queryadapter_last_owner_process = owner->dxg_process.v;
        hvdxg.queryadapter_last_owner_generation =
            hvdxg_open_process_generation(owner);
        hvdxg.queryadapter_last_owner_refs = hvdxg_open_process_refs(owner);
        if (ret != 0) {
            hvdxg.queryadapter_last_ret = ret;
            hvdxg_note_queryadapter_history((uint32)req.type,
                req.private_data_size, ret, 0);
            goto cleanup;
        }
    }
    memset(&query_context, 0, sizeof(query_context));
    ret = hvdxg_resolve_queryadapter_context(owner, req.adapter.v,
                                             &host_adapter,
                                             &resolve_source,
                                             &query_context);
    hvdxg.queryadapter_last_host_adapter = host_adapter;
    hvdxg.queryadapter_last_resolve_source = resolve_source;
    hvdxg.queryadapter_local_namespace = query_context.local_namespace;
    alias_adapter = query_context.process_adapter != NULL &&
                    host_adapter == hvdxg.host_adapter_handle &&
                    req.adapter.v != host_adapter;
    if (query_context.process_adapter != NULL) {
        hvdxg.queryadapter_process_adapter_refs =
            query_context.process_adapter->refs;
        hvdxg.queryadapter_process_adapter_locals =
            query_context.process_adapter->local_handle_count;
        hvdxg.queryadapter_process_adapter_generation =
            query_context.process_adapter->generation;
        query_ext_luid = &query_context.process_adapter->host_vgpu_luid;
    }
    hvdxg_note_queryadapter_adapter_object(owner, req.adapter.v,
                                           &query_context);
    if (ret != 0) {
        hvdxg.queryadapter_last_ret = ret;
        hvdxg_note_queryadapter_history((uint32)req.type,
            req.private_data_size, ret, 0);
        goto cleanup;
    }

    if (owner == NULL) {
        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0) {
            hvdxg.queryadapter_last_ret = ret;
            hvdxg_note_queryadapter_history((uint32)req.type,
                req.private_data_size, ret, 0);
            goto cleanup;
        }
    }

    command_len = hvdxg_queryadapter_wsl_command_len(req.private_data_size);
    result_len = req.private_data_size + sizeof(struct hvdxg_ntstatus);
    hvdxg.queryadapter_last_result_len = result_len;
    hvdxg.queryadapter_last_expected_wsl_len = result_len;
    command_buf = kvmalloc(command_len);
    result_buf = kvmalloc(result_len);
    if (command_buf == NULL || result_buf == NULL) {
        ret = -ENOMEM;
        hvdxg.queryadapter_last_ret = ret;
        hvdxg_note_queryadapter_history((uint32)req.type,
            req.private_data_size, ret, 0);
        goto cleanup;
    }
    memset(command_buf, 0, command_len);
    query = (struct hvdxg_command_queryadapterinfo_wsl *)command_buf;
    if (query_context.process != NULL)
        query_process = query_context.process->host_process;
    else if (owner != NULL && owner->dxg_process.v != 0)
        query_process = owner->dxg_process;
    else
        query_process = hvdxg.dxg_process;
    if (owner != NULL && owner->process_state != NULL &&
        query_process.v == 0) {
        ret = -EINVAL;
        hvdxg.queryadapter_last_ret = ret;
        hvdxg_note_queryadapter_history((uint32)req.type,
            req.private_data_size, ret, 0);
        goto cleanup;
    }
    query->hdr.process = query_process;
    hvdxg.queryadapter_last_owner_process = query_process.v;
    hvdxg.queryadapter_last_process_source =
        query_context.process_adapter != NULL ? 4 :
        (owner != NULL && owner->process_state != NULL &&
         owner->process_state->host_process.v != 0) ? 1 :
        (owner != NULL && owner->dxg_process.v != 0) ? 2 : 3;
    query->hdr.channel_type = HV_DXGKVMB_VGPU_TO_HOST;
    query->hdr.command_type = HV_DXGK_VMBCOMMAND_QUERYADAPTERINFO;
    query->query_type = (uint32)req.type;
    query->private_data_size = req.private_data_size;
    hvdxg.queryadapter_last_cmd_len = command_len;
    hvdxg.queryadapter_last_type_offset =
        __builtin_offsetof(struct hvdxg_command_queryadapterinfo_wsl,
                           query_type);
    hvdxg.queryadapter_last_size_offset =
        __builtin_offsetof(struct hvdxg_command_queryadapterinfo_wsl,
                           private_data_size);
    hvdxg.queryadapter_last_data_offset =
        __builtin_offsetof(struct hvdxg_command_queryadapterinfo_wsl,
                           private_data);
    if (either_copyin(query->private_data, 1, req.private_data,
                      req.private_data_size) < 0) {
        ret = -EFAULT;
        hvdxg.queryadapter_last_ret = ret;
        hvdxg_note_queryadapter_history((uint32)req.type,
            req.private_data_size, ret, 0);
        goto cleanup;
    }
    if ((uint32)req.type == HV_DXG_QAITYPE_SELECTED_ADAPTER)
        hvdxg_note_queryadapter_type0_input(query->private_data,
                                            req.private_data_size);
    if (command_len > hvdxg.queryadapter_last_data_offset +
                      req.private_data_size) {
        memset(command_buf + hvdxg.queryadapter_last_data_offset +
               req.private_data_size, 0,
               command_len - hvdxg.queryadapter_last_data_offset -
               req.private_data_size);
    }
    memset(result_buf, 0, result_len);
    if ((uint32)req.type == HV_DXG_QAITYPE_ADAPTER_HARDWARE_ID) {
        if (req.private_data_size !=
                HV_DXG_PHYSICAL_ADAPTER_DEVICE_IDS_SIZE) {
            ret = -EINVAL;
            hvdxg.queryadapter_last_ret = ret;
            hvdxg_note_queryadapter_history((uint32)req.type,
                req.private_data_size, ret, 0);
            goto cleanup;
        }
        if (!hvdxg_type31_active_v40_ext() &&
            hvdxg_try_fallback_adapter_hardware_id(
                result_buf, req.private_data_size)) {
            private_data = result_buf;
            actual_len = req.private_data_size;
            query_source = 3;
            hvdxg.queryadapter_last_len = actual_len;
            hvdxg.queryadapter_last_status = 0;
            hvdxg.queryadapter_last_ret = 0;
            hvdxg.queryadapter_last_layout = 3;
            goto queryadapter_copyout;
        }
    }
	    if (alias_adapter &&
	        ((uint32)req.type == HV_DXG_QAITYPE_SELECTED_ADAPTER ||
	         hvdxg_queryadapter_alias_cache_type((uint32)req.type)) &&
	        hvdxg_queryadapter_alias_cache_load((uint32)req.type,
	            req.private_data_size, req.adapter.v, host_adapter,
	            result_buf) == 0) {
        private_data = result_buf;
        actual_len = req.private_data_size;
        query_source = 2;
        hvdxg.queryadapter_last_len = actual_len;
        hvdxg.queryadapter_last_status = 0;
        hvdxg.queryadapter_last_ret = 0;
        hvdxg.queryadapter_last_layout = 5;
        goto queryadapter_copyout;
    }
    if (alias_adapter && (uint32)req.type == _KMTQAITYPE_UMDRIVERNAME &&
        hvdxg_queryadapter_stage_umd_payload((uint32)req.type,
            req.private_data_size, req.adapter.v, host_adapter,
            result_buf) == 0) {
        private_data = result_buf;
        actual_len = req.private_data_size;
        query_source = 4;
        hvdxg.queryadapter_last_len = actual_len;
        hvdxg.queryadapter_last_status = 0;
        hvdxg.queryadapter_last_ret = 0;
        hvdxg.queryadapter_last_layout = 6;
        goto queryadapter_copyout;
    }
    ret = hvdxg_send_sync_vgpu_flags_luid(query, command_len, result_buf,
                                          result_len, &actual_len, 0,
                                          query_ext_luid);
    hvdxg_capture_queryadapter_completion();
    hvdxg_note_queryadapter_return_head(result_buf, actual_len);
    primary_len = actual_len;
    primary_ret = ret;
    if (actual_len >= sizeof(struct hvdxg_ntstatus))
        primary_status = ((struct hvdxg_ntstatus *)result_buf)->v;
    else
        primary_status = 0;
    hvdxg.queryadapter_last_len = actual_len;
    hvdxg.queryadapter_last_ret = ret;
    hvdxg.queryadapter_last_status = primary_status;
    if ((uint32)req.type == HV_DXG_QAITYPE_SELECTED_ADAPTER) {
        hvdxg.queryadapter_type0_primary_len = primary_len;
        hvdxg.queryadapter_type0_primary_ret = primary_ret;
        hvdxg.queryadapter_type0_primary_status = primary_status;
        hvdxg.queryadapter_type0_result_route = 1;
        if (ret == 0 && actual_len < result_len) {
            ret = -EOVERFLOW;
            primary_ret = ret;
            hvdxg.queryadapter_type0_primary_ret = primary_ret;
            hvdxg.queryadapter_last_ret = ret;
        }
    }
	    if ((uint32)req.type == HV_DXG_QAITYPE_SELECTED_ADAPTER &&
	        ret != 0 && actual_len == 0) {
	        hvdxg.queryadapter_type0_fallback_attempted = 1;
	        hvdxg.queryadapter_type0_fallback_reason = 1;
        hvdxg.queryadapter_type0_fallback_route = 1;
        memset(result_buf, 0, result_len);
        actual_len = 0;
        ret = hvdxg_send_sync_vgpu_flags_luid(query, command_len,
            result_buf, result_len, &actual_len,
            HV_DXG_SEND_SYNC_VGPU_F_NO_EXT_HEADER |
            HV_DXG_SEND_SYNC_VGPU_F_FORCE_VGPU, query_ext_luid);
        hvdxg_capture_queryadapter_completion();
        hvdxg_note_queryadapter_return_head(result_buf, actual_len);
        hvdxg.queryadapter_type0_fallback_len = actual_len;
	        hvdxg.queryadapter_type0_fallback_ret = ret;
	        if (actual_len >= sizeof(struct hvdxg_ntstatus))
	            hvdxg.queryadapter_type0_fallback_status =
	                ((struct hvdxg_ntstatus *)result_buf)->v;
	        else
	            hvdxg.queryadapter_type0_fallback_status = 0;
	        hvdxg.queryadapter_last_len = actual_len;
	        hvdxg.queryadapter_last_ret = ret;
	        hvdxg.queryadapter_last_status =
	            hvdxg.queryadapter_type0_fallback_status;
	        if (ret == 0 && actual_len < result_len) {
	            ret = -EOVERFLOW;
	            hvdxg.queryadapter_type0_fallback_ret = ret;
	            hvdxg.queryadapter_last_ret = ret;
	        }
	        if (ret == 0 && actual_len >= result_len) {
	            hvdxg.queryadapter_type0_fallback_used = 1;
	            hvdxg.queryadapter_type0_result_route = 2;
	        }
	    }
	    if (ret != 0) {
	        hvdxg_note_queryadapter_type15_failure((uint32)req.type, ret,
	            hvdxg.queryadapter_last_status, query_process.v);
	        if (hvdxg_queryadapter_type55_zero_completion((uint32)req.type,
	                req.private_data_size, actual_len, ret)) {
	            memset(result_buf, 0, req.private_data_size);
	            private_data = result_buf;
	            ret = 0;
	            hvdxg.queryadapter_last_ret = 0;
	            hvdxg.queryadapter_last_status = 0;
	            hvdxg.queryadapter_last_layout = 4;
	            hvdxg_note_queryadapter_zero_success((uint32)req.type,
	                req.private_data_size, actual_len, ret,
	                hvdxg.queryadapter_last_status, 0);
	            goto queryadapter_copyout;
	        }
	        if (hvdxg_queryadapter_physicalcount_fallback((uint32)req.type,
	                req.private_data_size, actual_len, ret)) {
	            memset(result_buf, 0, req.private_data_size);
	            *(uint32 *)result_buf = 1;
	            private_data = result_buf;
	            ret = 0;
	            hvdxg.queryadapter_last_ret = 0;
	            hvdxg.queryadapter_last_status = 0;
	            hvdxg.queryadapter_last_layout = 4;
	            hvdxg_note_queryadapter_zero_success((uint32)req.type,
	                req.private_data_size, actual_len, ret,
	                hvdxg.queryadapter_last_status, 0);
	            goto queryadapter_copyout;
	        }
        if ((uint32)req.type == HV_DXG_QAITYPE_ADAPTER_HARDWARE_ID &&
            hvdxg_type31_active_v40_ext() &&
            hvdxg_adapter_hardware_primary_too_short(result_buf,
                actual_len, req.private_data_size))
            hvdxg_note_adapter_hardware_v40_short(actual_len, ret,
                hvdxg.queryadapter_last_status);
        if ((uint32)req.type == HV_DXG_QAITYPE_ADAPTER_HARDWARE_ID &&
            hvdxg_try_adapter_hardware_failure_fallbacks(
                owner, query, command_len, result_buf, result_len,
                req.private_data_size, &actual_len,
                !hvdxg_type31_active_v40_ext() ||
                    hvdxg_adapter_hardware_primary_too_short(result_buf,
                        actual_len, req.private_data_size),
                !hvdxg_type31_active_v40_ext())) {
            private_data = result_buf;
            hvdxg.queryadapter_last_len = actual_len;
            hvdxg.queryadapter_last_status = 0;
            hvdxg.queryadapter_last_ret = 0;
            hvdxg.queryadapter_last_layout = 3;
            goto queryadapter_copyout;
        }
        hvdxg_note_queryadapter_history((uint32)req.type,
            req.private_data_size, ret,
            (uint32)hvdxg.queryadapter_last_status);
        goto cleanup;
    }
	    if (actual_len >= sizeof(struct hvdxg_ntstatus))
	        hvdxg.queryadapter_last_status =
	            ((struct hvdxg_ntstatus *)result_buf)->v;
	    else
	        hvdxg.queryadapter_last_status = 0;
	    if (hvdxg_queryadapter_type55_zero_completion((uint32)req.type,
	            req.private_data_size, actual_len, ret)) {
	        memset(result_buf, 0, req.private_data_size);
	        private_data = result_buf;
	        hvdxg.queryadapter_last_ret = 0;
	        hvdxg.queryadapter_last_layout = 4;
	        hvdxg_note_queryadapter_zero_success((uint32)req.type,
	            req.private_data_size, actual_len, ret,
	            hvdxg.queryadapter_last_status, 0);
	        goto queryadapter_copyout;
	    }
	    if (hvdxg_queryadapter_adaptertype_zero_completion((uint32)req.type,
	            req.private_data_size, actual_len, ret)) {
	        memset(result_buf, 0, req.private_data_size);
	        private_data = result_buf;
	        hvdxg.queryadapter_last_ret = 0;
	        hvdxg.queryadapter_last_status = 0;
	        hvdxg.queryadapter_last_layout = 4;
	        hvdxg_note_queryadapter_zero_success((uint32)req.type,
	            req.private_data_size, actual_len, ret,
	            hvdxg.queryadapter_last_status, 0);
	        goto queryadapter_copyout;
	    }
    if ((uint32)req.type == HV_DXG_QAITYPE_ADAPTER_HARDWARE_ID &&
        hvdxg_adapter_hardware_primary_too_short(result_buf, actual_len,
                                                 req.private_data_size)) {
        ret = -EOVERFLOW;
        hvdxg.queryadapter_last_ret = ret;
        hvdxg_note_queryadapter_history((uint32)req.type,
            req.private_data_size, ret,
            (uint32)hvdxg.queryadapter_last_status);
        if (hvdxg_type31_active_v40_ext()) {
            hvdxg_note_adapter_hardware_v40_short(actual_len, ret,
                hvdxg.queryadapter_last_status);
            if (hvdxg_try_adapter_hardware_failure_fallbacks(
                    owner, query, command_len, result_buf, result_len,
                    req.private_data_size, &actual_len, 1, 0)) {
                private_data = result_buf;
                hvdxg.queryadapter_last_len = actual_len;
                hvdxg.queryadapter_last_status = 0;
                hvdxg.queryadapter_last_ret = 0;
                hvdxg.queryadapter_last_layout = 3;
                goto queryadapter_copyout;
            }
            goto cleanup;
        }

        memset(result_buf, 0, result_len);
        actual_len = 0;
        ret = hvdxg_send_sync_vgpu_flags_luid(query, command_len,
            result_buf, result_len, &actual_len,
            HV_DXG_SEND_SYNC_VGPU_F_NO_EXT_HEADER |
            HV_DXG_SEND_SYNC_VGPU_F_FORCE_VGPU, query_ext_luid);
        hvdxg_capture_queryadapter_completion();
        hvdxg_note_queryadapter_return_head(result_buf, actual_len);
        hvdxg.queryadapter_last_len = actual_len;
        hvdxg.queryadapter_last_ret = ret;
        if (actual_len >= sizeof(struct hvdxg_ntstatus))
            hvdxg.queryadapter_last_status =
                ((struct hvdxg_ntstatus *)result_buf)->v;
        else
            hvdxg.queryadapter_last_status = 0;
        if (ret != 0) {
            hvdxg_note_queryadapter_history((uint32)req.type,
                req.private_data_size, ret,
                (uint32)hvdxg.queryadapter_last_status);
            if (hvdxg_try_adapter_hardware_failure_fallbacks(
                    owner, query, command_len, result_buf, result_len,
                    req.private_data_size, &actual_len,
                    1,
                    !hvdxg_type31_active_v40_ext())) {
                private_data = result_buf;
                hvdxg.queryadapter_last_len = actual_len;
                hvdxg.queryadapter_last_status = 0;
                hvdxg.queryadapter_last_ret = 0;
                hvdxg.queryadapter_last_layout = 3;
                goto queryadapter_copyout;
            }
            goto cleanup;
        }
        if (hvdxg_accept_legacy_adapter_hardware_id(result_buf, actual_len,
                                                    req.private_data_size)) {
            private_data = result_buf;
            hvdxg.queryadapter_last_layout = 3;
            goto queryadapter_copyout;
        }
        hvdxg_note_queryadapter_history((uint32)req.type,
            req.private_data_size,
            actual_len < req.private_data_size ? -EOVERFLOW : -EINVAL,
            (uint32)hvdxg.queryadapter_last_status);
        if (hvdxg_try_adapter_hardware_failure_fallbacks(
                owner, query, command_len, result_buf, result_len,
                req.private_data_size, &actual_len,
                1,
                !hvdxg_type31_active_v40_ext())) {
            private_data = result_buf;
            hvdxg.queryadapter_last_len = actual_len;
            hvdxg.queryadapter_last_status = 0;
            hvdxg.queryadapter_last_ret = 0;
            hvdxg.queryadapter_last_layout = 3;
            goto queryadapter_copyout;
        }
        if (actual_len >= sizeof(struct hvdxg_ntstatus) &&
            hvdxg_ntstatus_plausible(
                *(struct hvdxg_ntstatus *)result_buf)) {
            struct hvdxg_ntstatus *status =
                (struct hvdxg_ntstatus *)result_buf;

            ret = hvdxg_ntstatus_to_errno(*status);
            if (ret >= 0)
                ret = -EOVERFLOW;
        } else {
            ret = actual_len < req.private_data_size ?
                  -EOVERFLOW : -EINVAL;
        }
        hvdxg.queryadapter_last_ret = ret;
        hvdxg_note_queryadapter_history((uint32)req.type,
            req.private_data_size, ret,
            (uint32)hvdxg.queryadapter_last_status);
        goto cleanup;
    }
    if (actual_len >= req.private_data_size +
                      sizeof(struct hvdxg_ntstatus) &&
        hvdxg_ntstatus_plausible(
            *(struct hvdxg_ntstatus *)result_buf)) {
        struct hvdxg_ntstatus *status =
            (struct hvdxg_ntstatus *)result_buf;
        hvdxg.queryadapter_last_layout = 2;
        ret = hvdxg_ntstatus_to_errno(*status);
        hvdxg.queryadapter_last_ret = ret;
        if (ret < 0) {
            hvdxg_note_queryadapter_type15_failure((uint32)req.type, ret,
                status->v, query_process.v);
            if ((uint32)req.type == HV_DXG_QAITYPE_ADAPTER_HARDWARE_ID &&
                hvdxg_try_adapter_hardware_failure_fallbacks(
                    owner, query, command_len, result_buf, result_len,
                    req.private_data_size, &actual_len,
                    !hvdxg_type31_active_v40_ext() ||
                        hvdxg_adapter_hardware_primary_too_short(
                            result_buf, actual_len,
                            req.private_data_size),
                    !hvdxg_type31_active_v40_ext())) {
                private_data = result_buf;
                hvdxg.queryadapter_last_len = actual_len;
                hvdxg.queryadapter_last_status = 0;
                hvdxg.queryadapter_last_ret = 0;
                hvdxg.queryadapter_last_layout = 3;
                goto queryadapter_copyout;
            }
            hvdxg_note_queryadapter_history((uint32)req.type,
                req.private_data_size, ret, (uint32)status->v);
            goto cleanup;
        }
        private_data = result_buf + sizeof(struct hvdxg_ntstatus);
    } else if (actual_len >= req.private_data_size) {
        private_data = result_buf;
        hvdxg.queryadapter_last_layout = 1;
    } else if (actual_len >= sizeof(struct hvdxg_ntstatus) &&
               hvdxg_ntstatus_plausible(
                   *(struct hvdxg_ntstatus *)result_buf)) {
        struct hvdxg_ntstatus *status =
            (struct hvdxg_ntstatus *)result_buf;
        hvdxg.queryadapter_last_layout = 2;
        ret = hvdxg_ntstatus_to_errno(*status);
        if (ret >= 0)
            ret = -EOVERFLOW;
        if ((uint32)req.type == HV_DXG_QAITYPE_ADAPTER_HARDWARE_ID &&
            hvdxg_try_adapter_hardware_failure_fallbacks(
                owner, query, command_len, result_buf, result_len,
                req.private_data_size, &actual_len,
                !hvdxg_type31_active_v40_ext() ||
                    hvdxg_adapter_hardware_primary_too_short(result_buf,
                        actual_len, req.private_data_size),
                !hvdxg_type31_active_v40_ext())) {
            private_data = result_buf;
            hvdxg.queryadapter_last_len = actual_len;
            hvdxg.queryadapter_last_status = 0;
            hvdxg.queryadapter_last_ret = 0;
            hvdxg.queryadapter_last_layout = 3;
            goto queryadapter_copyout;
        }
        hvdxg.queryadapter_last_ret = ret;
        hvdxg_note_queryadapter_history((uint32)req.type,
            req.private_data_size, ret, (uint32)status->v);
        goto cleanup;
    } else {
        ret = -EOVERFLOW;
        if (hvdxg_queryadapter_physicalcount_fallback((uint32)req.type,
                req.private_data_size, actual_len, ret)) {
            memset(result_buf, 0, req.private_data_size);
            *(uint32 *)result_buf = 1;
            private_data = result_buf;
            ret = 0;
	            hvdxg.queryadapter_last_ret = 0;
	            hvdxg.queryadapter_last_status = 0;
	            hvdxg.queryadapter_last_layout = 4;
	            hvdxg_note_queryadapter_zero_success((uint32)req.type,
	                req.private_data_size, actual_len, ret,
	                hvdxg.queryadapter_last_status, 0);
	            goto queryadapter_copyout;
	        }
        if ((uint32)req.type == HV_DXG_QAITYPE_ADAPTER_HARDWARE_ID &&
            hvdxg_try_adapter_hardware_failure_fallbacks(
                owner, query, command_len, result_buf, result_len,
                req.private_data_size, &actual_len,
                !hvdxg_type31_active_v40_ext() ||
                    hvdxg_adapter_hardware_primary_too_short(result_buf,
                        actual_len, req.private_data_size),
                !hvdxg_type31_active_v40_ext())) {
            private_data = result_buf;
            hvdxg.queryadapter_last_len = actual_len;
            hvdxg.queryadapter_last_status = 0;
            hvdxg.queryadapter_last_ret = 0;
            hvdxg.queryadapter_last_layout = 3;
            goto queryadapter_copyout;
        }
        hvdxg.queryadapter_last_ret = ret;
        hvdxg_note_queryadapter_history((uint32)req.type,
            req.private_data_size, ret,
            (uint32)hvdxg.queryadapter_last_status);
        goto cleanup;
    }
queryadapter_copyout:
    if (query_source == 0)
        query_source = (hvdxg.queryadapter_last_layout == 1 ||
                        hvdxg.queryadapter_last_layout == 2) ? 1 : 3;
    hvdxg.queryadapter_source_last = query_source;
    if (query_source == 1) {
        hvdxg.queryadapter_source_real_host++;
        hvdxg_queryadapter_alias_cache_store((uint32)req.type,
            req.private_data_size, host_adapter, private_data);
    } else if (query_source == 2) {
        hvdxg.queryadapter_source_alias_cache++;
    } else if (query_source == 4) {
        hvdxg.queryadapter_source_staged++;
        hvdxg_queryadapter_alias_cache_store((uint32)req.type,
            req.private_data_size, host_adapter, private_data);
    } else {
        hvdxg.queryadapter_source_fallback++;
        hvdxg.queryadapter_alias_cache_last_type = (uint32)req.type;
        hvdxg.queryadapter_alias_cache_last_size = req.private_data_size;
        hvdxg.queryadapter_alias_cache_last_len = actual_len;
        hvdxg.queryadapter_alias_cache_last_alias = req.adapter.v;
        hvdxg.queryadapter_alias_cache_last_host = host_adapter;
        hvdxg.queryadapter_alias_cache_last_hash =
            hvdxg_hash_bytes(private_data, req.private_data_size);
        hvdxg.queryadapter_alias_cache_last_result = 4;
    }
    if (req.type == _KMTQAITYPE_UMDRIVERNAME) {
        ret = hvdxg_rewrite_umd_driver_path(
            private_data, req.private_data_size,
            (uint32)hvdxg.queryadapter_last_status);
        if (ret != 0) {
            hvdxg.queryadapter_last_ret = ret;
            hvdxg_note_queryadapter_history((uint32)req.type,
                req.private_data_size, ret,
                (uint32)hvdxg.queryadapter_last_status);
            goto cleanup;
        }
    }
    if ((req.type == _KMTQAITYPE_ADAPTERTYPE ||
         req.type == _KMTQAITYPE_ADAPTERTYPE_RENDER) &&
        req.private_data_size >= sizeof(struct d3dkmt_adaptertype)) {
        struct d3dkmt_adaptertype *adapter_type =
            (struct d3dkmt_adaptertype *)private_data;
        /*
         * Hyper-V GPU-PV hosts in this VM report a transport-ready adapter
         * with render_supported clear when the partition's current compute
         * budget is zero. WSL/DXCore consumers treat adapter type as part of
         * adapter discovery, so expose the vGPU as a render-capable
         * paravirtualized adapter here while fb.c continues to gate real
         * OpenGL submit on validated context/submit success.
         */
        hvdxg_fill_render_adapter_type(adapter_type);
        hvdxg.queryadapter_adaptertype_rewrite_count++;
        hvdxg.queryadapter_adaptertype_rewrite_type = (uint32)req.type;
        hvdxg.queryadapter_adaptertype_rewrite_source = query_source;
        hvdxg.queryadapter_adaptertype_last_type = (uint32)req.type;
        hvdxg.queryadapter_adaptertype_last_source = query_source;
    }
	    if ((uint32)req.type == HV_DXG_QAITYPE_ADAPTER_HARDWARE_ID &&
	        hvdxg.queryadapter_last_layout != 3 &&
	        !hvdxg_normalize_adapter_hardware_id(private_data,
	                                             req.private_data_size)) {
        if (hvdxg_try_adapter_hardware_failure_fallbacks(
                owner, query, command_len, result_buf, result_len,
                req.private_data_size, &actual_len,
                !hvdxg_type31_active_v40_ext(),
                !hvdxg_type31_active_v40_ext())) {
            private_data = result_buf;
            hvdxg.queryadapter_last_len = actual_len;
            hvdxg.queryadapter_last_status = 0;
            hvdxg.queryadapter_last_ret = 0;
            hvdxg.queryadapter_last_layout = 3;
        } else {
            ret = -EINVAL;
            hvdxg.queryadapter_last_ret = ret;
            hvdxg_note_queryadapter_history((uint32)req.type,
                req.private_data_size, ret,
                (uint32)hvdxg.queryadapter_last_status);
            goto cleanup;
	        }
	    }
	    hvdxg.queryadapter_last_user_len = req.private_data_size;
	    ret = either_copyout(1, req.private_data, private_data,
	                         req.private_data_size) < 0 ? -EFAULT : 0;
    hvdxg.queryadapter_last_ret = ret;
    hvdxg_note_queryadapter_history((uint32)req.type,
        req.private_data_size, ret,
        (uint32)hvdxg.queryadapter_last_status);

cleanup:
    if (process_locked)
        mutex_unlock(&hvdxg.process_lock);
    if (command_buf)
        kvfree(command_buf);
    if (result_buf)
        kvfree(result_buf);
    return ret;
}

