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

static int hvdxg_ioctl_requires_process(uint32 cmd)
{
    switch (cmd) {
    case LX_DXENUMADAPTERS2:
    case LX_DXENUMADAPTERS3:
        /*
         * WSL creates the host dxgprocess before returning local adapter
         * aliases.  These discovery ioctls bind explicitly after user args
         * are copied, immediately before the local handle can be returned.
         */
        return 0;
    case LX_DXOPENADAPTERFROMLUID:
        return 0;
    case LX_DXQUERYADAPTERINFO:
        /*
         * QueryAdapterInfo does its own WSL-order bind and adapter handle
         * resolution after copying the user args, while still allowing
         * ownerless internal probes to use the raw host adapter handle.
         */
        return 0;
    default:
        return 1;
    }
}

static int hvdxg_ioctl_tgid_gate(struct hvdxg_open_state *owner,
                                 uint32 cmd, int process_required)
{
    uint64 current_tgid = current != NULL ?
                          (uint64)thread_tgid(current) : 0;
    uint64 owner_tgid = owner != NULL && owner->process_state != NULL ?
                        owner->process_state->tgid : 0;
    uint32 owner_generation =
        owner != NULL && owner->process_state != NULL ?
        owner->process_state->generation : 0;

    hvdxg.ioctl_tgid_gate_checks++;
    hvdxg.ioctl_tgid_gate_last_cmd = cmd;
    hvdxg.ioctl_tgid_gate_current_tgid = current_tgid;
    hvdxg.ioctl_tgid_gate_owner_tgid = owner_tgid;
    hvdxg.ioctl_tgid_gate_owner_generation = owner_generation;
    hvdxg.ioctl_tgid_gate_process_required =
        process_required ? 1U : 0U;

    if (owner != NULL && owner->process_state != NULL &&
        current_tgid != 0 && owner_tgid != 0 &&
        current_tgid != owner_tgid) {
        hvdxg.ioctl_tgid_gate_denied++;
        hvdxg.ioctl_tgid_gate_last_ret = -ENOTTY;
        return -ENOTTY;
    }

    hvdxg.ioctl_tgid_gate_passes++;
    hvdxg.ioctl_tgid_gate_last_ret = 0;
    return 0;
}

#define HV_DXG_ENUM_STAGE_START      1U
#define HV_DXG_ENUM_STAGE_ENSURE     2U
#define HV_DXG_ENUM_STAGE_BIND       3U
#define HV_DXG_ENUM_STAGE_COUNT      4U
#define HV_DXG_ENUM_STAGE_LOCAL      5U
#define HV_DXG_ENUM_STAGE_COPYOUT    6U
#define HV_DXG_ENUM_STAGE_DONE       7U

static void hvdxg_note_enumadapters_state(struct hvdxg_open_state *owner,
                                          uint32 stage)
{
    struct hvdxg_process_state *process =
        owner != NULL ? owner->process_state : NULL;

    hvdxg.enumadapters_last_stage = stage;
    hvdxg.enumadapters_last_global_open = hvdxg.global_open_ok ? 1 : 0;
    hvdxg.enumadapters_last_vgpu_open = hvdxg.vgpu_open_ok ? 1 : 0;
    hvdxg.enumadapters_last_global_relid = hvdxg.global_relid;
    hvdxg.enumadapters_last_vgpu_relid = hvdxg.vgpu_relid;
    hvdxg.enumadapters_last_global_conn = hvdxg.global_conn_id;
    hvdxg.enumadapters_last_vgpu_conn = hvdxg.vgpu_conn_id;
    hvdxg.enumadapters_last_host_adapter = hvdxg.host_adapter_handle;
    hvdxg.enumadapters_last_probe_successes = hvdxg.probe_successes;
    hvdxg.enumadapters_last_probe_ret = hvdxg.probe_last_ret;
    hvdxg.enumadapters_last_probe_status = hvdxg.probe_open_status;
    hvdxg.enumadapters_last_probe_handle = hvdxg.probe_open_handle;
    hvdxg.enumadapters_last_ready = hvdxg.d3dkmt_ready ? 1 : 0;
    hvdxg.enumadapters_last_process =
        hvdxg_owner_bound_process_handle(owner).v;
    hvdxg.enumadapters_last_process_created =
        owner != NULL && owner->dxg_process_created ? 1 : 0;
    hvdxg.enumadapters_last_process_generation =
        process != NULL ? process->generation : 0;
    hvdxg.enumadapters_last_process_refs =
        process != NULL ? process->process_refs : 0;
    hvdxg.enumadapters_last_process_adapters =
        process != NULL ? process->adapter_count : 0;
    hvdxg.enumadapters_last_process_locals =
        process != NULL ? process->local_adapter_count : 0;
    hvdxg.enumadapters_last_process_objects =
        process != NULL ? process->object_count : 0;
}

static int hvdxg_ioctl_common(cdev_t *cdev, uint64 cmd, void *arg,
                              struct hvdxg_open_state *owner)
{
    (void)cdev;
    int ret = -EINVAL;
    int process_locked = 0;
    int process_required = hvdxg_ioctl_requires_process((uint32)cmd);
    uint64 start_ticks = r_time();

    hvdxg.ioctl_count++;
    ret = hvdxg_ioctl_tgid_gate(owner, (uint32)cmd, process_required);
    if (ret != 0) {
        hvdxg.ioctl_last_ret = ret;
        hvdxg_note_ioctl_timing(cmd, r_time() - start_ticks);
        return ret;
    }
    if (owner != NULL && process_required) {
        mutex_lock(&hvdxg.process_lock);
        process_locked = 1;
    }
    if (process_required) {
        ret = hvdxg_bind_open_process(owner);
        if (ret != 0) {
            hvdxg.ioctl_last_ret = ret;
            hvdxg_note_ioctl_timing(cmd, r_time() - start_ticks);
            if (process_locked)
                mutex_unlock(&hvdxg.process_lock);
            return ret;
        }
    }

    switch ((uint32)cmd) {
    case LX_DXENUMADAPTERS2: {
        struct d3dkmt_enumadapters2 req;
        struct d3dkmt_adapterinfo info;
        struct hvdxg_winluid user_luid;
        uint32 luid_source;
        uint32 local_adapter;

        hvdxg.enumadapters_last_cmd = (uint32)cmd;
        hvdxg.enumadapters_last_in_count = 0;
        hvdxg.enumadapters_last_out_count = 0;
        hvdxg.enumadapters_last_num_sources = 0;
        hvdxg.enumadapters_last_buffer = 0;
        hvdxg.enumadapters_last_handle = 0;
        hvdxg.enumadapters_last_luid_low = 0;
        hvdxg.enumadapters_last_luid_high = 0;
        hvdxg.enumadapters_last_luid_source = 0;
        hvdxg.enumadapters_last_ret = 0;
        hvdxg.enumadapters_last_ensure_ret = 0;
        hvdxg.enumadapters_last_bind_ret = 0;
        hvdxg.enumadapters_last_local_ret = 0;
        hvdxg.enumadapters_last_copyout_ret = 0;
        hvdxg_note_enumadapters_state(owner, HV_DXG_ENUM_STAGE_START);
        ret = hvdxg_d3dkmt_ensure_adapter();
        hvdxg.enumadapters_last_ensure_ret = ret;
        hvdxg_note_enumadapters_state(owner, HV_DXG_ENUM_STAGE_ENSURE);
        if (ret != 0) {
            hvdxg.enumadapters_last_ret = ret;
            break;
        }
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            hvdxg.enumadapters_last_ret = ret;
            break;
        }
        hvdxg.enumadapters_last_cmd = (uint32)cmd;
        hvdxg.enumadapters_last_in_count = req.num_adapters;
        hvdxg.enumadapters_last_out_count = 0;
        hvdxg.enumadapters_last_num_sources = 0;
        hvdxg.enumadapters_last_buffer = req.adapters;
        hvdxg.enumadapters_last_handle = 0;
        hvdxg.enumadapters_last_luid_low = 0;
        hvdxg.enumadapters_last_luid_high = 0;
        hvdxg.enumadapters_last_luid_source = 0;
        hvdxg.enumadapters_last_ret = 0;
        user_luid = hvdxg_user_adapter_luid(&luid_source);
        hvdxg.enumadapters_last_luid_low = user_luid.a;
        hvdxg.enumadapters_last_luid_high = user_luid.b;
        hvdxg.enumadapters_last_luid_source = luid_source;
        if (owner == NULL) {
            ret = -EINVAL;
            hvdxg.enumadapters_last_ret = ret;
            break;
        }
        ret = hvdxg_bind_open_process_early(owner,
            HV_DXG_BIND_SOURCE_ENUMADAPTERS2, (uint32)cmd);
        hvdxg.enumadapters_last_bind_ret = ret;
        hvdxg_note_enumadapters_state(owner, HV_DXG_ENUM_STAGE_BIND);
        if (ret != 0) {
            hvdxg.enumadapters_last_ret = ret;
            break;
        }
        if (req.adapters == 0) {
            req.num_adapters = 1;
            hvdxg.enumadapters_last_out_count = req.num_adapters;
            ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
                  -EFAULT : 0;
            hvdxg.enumadapters_last_copyout_ret = ret;
            hvdxg_note_enumadapters_state(owner, HV_DXG_ENUM_STAGE_COUNT);
            hvdxg.enumadapters_last_ret = ret;
            break;
        }
        if (req.num_adapters == 0) {
            req.num_adapters = 1;
            hvdxg.enumadapters_last_out_count = req.num_adapters;
            (void)either_copyout(1, (uint64)arg, &req, sizeof(req));
            ret = -EOVERFLOW;
            hvdxg.enumadapters_last_copyout_ret = ret;
            hvdxg_note_enumadapters_state(owner, HV_DXG_ENUM_STAGE_COUNT);
            hvdxg.enumadapters_last_ret = ret;
            break;
        }
        if (req.num_adapters > D3DKMT_ADAPTERS_MAX) {
            ret = -EINVAL;
            hvdxg_note_enumadapters_state(owner, HV_DXG_ENUM_STAGE_COUNT);
            hvdxg.enumadapters_last_ret = ret;
            break;
        }
        if (owner != NULL)
            mutex_lock(&hvdxg.process_lock);
        ret = hvdxg_get_local_adapter_handle(owner, &local_adapter);
        if (owner != NULL)
            mutex_unlock(&hvdxg.process_lock);
        hvdxg.enumadapters_last_local_ret = ret;
        hvdxg_note_enumadapters_state(owner, HV_DXG_ENUM_STAGE_LOCAL);
        if (ret != 0) {
            hvdxg.enumadapters_last_ret = ret;
            break;
        }
        memset(&info, 0, sizeof(info));
        info.adapter_handle.v = local_adapter;
        info.adapter_luid.a = user_luid.a;
        info.adapter_luid.b = user_luid.b;
        hvdxg.enumadapters_last_num_sources = info.num_sources;
        req.num_adapters = 1;
        if (either_copyout(1, req.adapters, &info, sizeof(info)) < 0 ||
            either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0)
            ret = -EFAULT;
        else
            ret = 0;
        hvdxg.enumadapters_last_copyout_ret = ret;
        hvdxg.enumadapters_last_out_count = req.num_adapters;
        hvdxg.enumadapters_last_handle = info.adapter_handle.v;
        hvdxg_note_enumadapters_state(owner, HV_DXG_ENUM_STAGE_COPYOUT);
        hvdxg.enumadapters_last_ret = ret;
        break;
    }

    case LX_DXENUMADAPTERS3: {
        struct d3dkmt_enumadapters3 req;
        struct d3dkmt_adapterinfo info;
        struct hvdxg_winluid user_luid;
        uint32 luid_source;
        uint32 local_adapter;

        hvdxg.enumadapters_last_cmd = (uint32)cmd;
        hvdxg.enumadapters_last_in_count = 0;
        hvdxg.enumadapters_last_out_count = 0;
        hvdxg.enumadapters_last_num_sources = 0;
        hvdxg.enumadapters_last_buffer = 0;
        hvdxg.enumadapters_last_handle = 0;
        hvdxg.enumadapters_last_luid_low = 0;
        hvdxg.enumadapters_last_luid_high = 0;
        hvdxg.enumadapters_last_luid_source = 0;
        hvdxg.enumadapters_last_ret = 0;
        hvdxg.enumadapters_last_ensure_ret = 0;
        hvdxg.enumadapters_last_bind_ret = 0;
        hvdxg.enumadapters_last_local_ret = 0;
        hvdxg.enumadapters_last_copyout_ret = 0;
        hvdxg_note_enumadapters_state(owner, HV_DXG_ENUM_STAGE_START);
        ret = hvdxg_d3dkmt_ensure_adapter();
        hvdxg.enumadapters_last_ensure_ret = ret;
        hvdxg_note_enumadapters_state(owner, HV_DXG_ENUM_STAGE_ENSURE);
        if (ret != 0) {
            hvdxg.enumadapters_last_ret = ret;
            break;
        }
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            hvdxg.enumadapters_last_ret = ret;
            break;
        }
        hvdxg.enumadapters_last_cmd = (uint32)cmd;
        hvdxg.enumadapters_last_in_count = req.adapter_count;
        hvdxg.enumadapters_last_out_count = 0;
        hvdxg.enumadapters_last_num_sources = 0;
        hvdxg.enumadapters_last_buffer = req.adapters;
        hvdxg.enumadapters_last_handle = 0;
        hvdxg.enumadapters_last_luid_low = 0;
        hvdxg.enumadapters_last_luid_high = 0;
        hvdxg.enumadapters_last_luid_source = 0;
        hvdxg.enumadapters_last_ret = 0;
        user_luid = hvdxg_user_adapter_luid(&luid_source);
        hvdxg.enumadapters_last_luid_low = user_luid.a;
        hvdxg.enumadapters_last_luid_high = user_luid.b;
        hvdxg.enumadapters_last_luid_source = luid_source;
        if (owner == NULL) {
            ret = -EINVAL;
            hvdxg.enumadapters_last_ret = ret;
            break;
        }
        ret = hvdxg_bind_open_process_early(owner,
            HV_DXG_BIND_SOURCE_ENUMADAPTERS3, (uint32)cmd);
        hvdxg.enumadapters_last_bind_ret = ret;
        hvdxg_note_enumadapters_state(owner, HV_DXG_ENUM_STAGE_BIND);
        if (ret != 0) {
            hvdxg.enumadapters_last_ret = ret;
            break;
        }
        if (req.adapters == 0 || req.adapter_count == 0) {
            req.adapter_count = 1;
            hvdxg.enumadapters_last_out_count = req.adapter_count;
            ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
                  -EFAULT : 0;
            hvdxg.enumadapters_last_copyout_ret = ret;
            hvdxg_note_enumadapters_state(owner, HV_DXG_ENUM_STAGE_COUNT);
            hvdxg.enumadapters_last_ret = ret;
            break;
        }
        if (req.adapter_count > D3DKMT_ADAPTERS_MAX) {
            ret = -EINVAL;
            hvdxg_note_enumadapters_state(owner, HV_DXG_ENUM_STAGE_COUNT);
            hvdxg.enumadapters_last_ret = ret;
            break;
        }
        if (owner != NULL)
            mutex_lock(&hvdxg.process_lock);
        ret = hvdxg_get_local_adapter_handle(owner, &local_adapter);
        if (owner != NULL)
            mutex_unlock(&hvdxg.process_lock);
        hvdxg.enumadapters_last_local_ret = ret;
        hvdxg_note_enumadapters_state(owner, HV_DXG_ENUM_STAGE_LOCAL);
        if (ret != 0) {
            hvdxg.enumadapters_last_ret = ret;
            break;
        }
        memset(&info, 0, sizeof(info));
        info.adapter_handle.v = local_adapter;
        info.adapter_luid.a = user_luid.a;
        info.adapter_luid.b = user_luid.b;
        hvdxg.enumadapters_last_num_sources = info.num_sources;
        req.adapter_count = 1;
        if (either_copyout(1, req.adapters, &info, sizeof(info)) < 0 ||
            either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0)
            ret = -EFAULT;
        else
            ret = 0;
        hvdxg.enumadapters_last_copyout_ret = ret;
        hvdxg.enumadapters_last_out_count = req.adapter_count;
        hvdxg.enumadapters_last_handle = info.adapter_handle.v;
        hvdxg_note_enumadapters_state(owner, HV_DXG_ENUM_STAGE_COPYOUT);
        hvdxg.enumadapters_last_ret = ret;
        break;
    }

    case LX_DXOPENADAPTERFROMLUID: {
        struct d3dkmt_openadapterfromluid req;
        struct hvdxg_winluid luid;
        struct hvdxg_winluid user_luid;
        struct hvdxg_winluid ext_luid;
        uint32 luid_source;
        uint32 local_adapter;

        ret = hvdxg_d3dkmt_ensure_adapter();
        if (ret != 0) {
            hvdxg.openadapter_luid_last_ret = ret;
            hvdxg.openadapter_luid_last_status = hvdxg.probe_open_status;
            break;
        }
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            hvdxg.openadapter_luid_last_ret = ret;
            hvdxg.openadapter_luid_last_status = hvdxg.probe_open_status;
            break;
        }
        luid.a = req.adapter_luid.a;
        luid.b = req.adapter_luid.b;
        user_luid = hvdxg_user_adapter_luid(&luid_source);
        ext_luid = hvdxg_ext_adapter_luid(NULL);
        hvdxg.openadapter_luid_last_input_low = luid.a;
        hvdxg.openadapter_luid_last_input_high = luid.b;
        hvdxg.openadapter_luid_last_um_low = user_luid.a;
        hvdxg.openadapter_luid_last_um_high = user_luid.b;
        hvdxg.openadapter_luid_last_host_low = ext_luid.a;
        hvdxg.openadapter_luid_last_host_high = ext_luid.b;
        hvdxg.openadapter_luid_last_host_basis = luid_source;
        hvdxg.openadapter_luid_last_match = 0;
        hvdxg.openadapter_luid_last_reject = HV_DXG_OAFLUID_REJECT_NONE;
        hvdxg.openadapter_luid_last_handle = 0;
        hvdxg.openadapter_luid_last_status = hvdxg.probe_open_status;
        if (!hvdxg_luid_nonzero(luid)) {
            ret = -EINVAL;
            hvdxg.openadapter_luid_last_reject =
                HV_DXG_OAFLUID_REJECT_ZERO;
            hvdxg.openadapter_luid_last_ret = ret;
            break;
        }
        if (!hvdxg_luid_equal(luid, user_luid)) {
            ret = -EINVAL;
            hvdxg.openadapter_luid_last_reject =
                HV_DXG_OAFLUID_REJECT_MISMATCH;
            hvdxg.openadapter_luid_last_ret = ret;
            break;
        }
        hvdxg.openadapter_luid_last_match = 1;
        if (owner == NULL) {
            ret = -EINVAL;
            hvdxg.openadapter_luid_last_ret = ret;
            break;
        }
        ret = hvdxg_bind_open_process_early(owner,
            HV_DXG_BIND_SOURCE_OPENADAPTERFROMLUID, (uint32)cmd);
        if (ret != 0) {
            hvdxg.openadapter_luid_last_ret = ret;
            break;
        }
        if (owner != NULL)
            mutex_lock(&hvdxg.process_lock);
        ret = hvdxg_get_local_adapter_handle(owner, &local_adapter);
        if (owner != NULL)
            mutex_unlock(&hvdxg.process_lock);
        if (ret != 0) {
            hvdxg.openadapter_luid_last_ret = ret;
            break;
        }
        req.adapter_handle.v = local_adapter;
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        hvdxg.openadapter_luid_last_handle = req.adapter_handle.v;
        hvdxg.openadapter_luid_last_ret = ret;
        break;
    }

    case LX_DXCREATEDEVICE: {
        struct d3dkmt_createdevice req;
        struct hvdxg_command_createdevice create;
        struct hvdxg_command_createdevice_return result;
        uint32 actual_len = 0;
        uint32 local_adapter;
        uint32 host_adapter;
        struct hvdxg_d3dkmthandle create_process = { 0 };

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        local_adapter = req.adapter.v;
        hvdxg.createdevice_last_adapter = local_adapter;
        hvdxg.createdevice_last_host_adapter = 0;
        hvdxg.createdevice_last_device = 0;
        hvdxg.createdevice_adapter_equals_device = 0;
        hvdxg.createdevice_host_adapter_equals_device = 0;
        hvdxg.createdevice_last_ret = 0;
        hvdxg.createdevice_last_process = 0;
        hvdxg.createdevice_last_owner_process =
            hvdxg_open_host_process(owner);
        hvdxg.createdevice_last_owner_generation =
            hvdxg_open_process_generation(owner);
        hvdxg.createdevice_last_owner_refs =
            hvdxg_open_process_refs(owner);
        hvdxg_note_createdevice_object(owner, 0);
        if (hvdxg_resolve_adapter_handle(owner, local_adapter,
                                         &host_adapter) != 0) {
            ret = -EINVAL;
            hvdxg.createdevice_last_ret = ret;
            break;
        }
        hvdxg.createdevice_last_host_adapter = host_adapter;
        memset(&create, 0, sizeof(create));
        memset(&result, 0, sizeof(result));
        create_process = hvdxg_owner_bound_process_handle(owner);
        hvdxg.createdevice_last_process = create_process.v;
        hvdxg_command_vgpu_init_process(&create.hdr,
                                        HV_DXGK_VMBCOMMAND_CREATEDEVICE,
                                        create_process);
        create.flags = req.flags;
        create.cdd_device = req.flags.gdi_device ? 1 : 0;
        create.error_code = (uint64)&hvdxg.device_state_counter;
        ret = hvdxg_send_sync_vgpu(&create, sizeof(create), &result,
                                   sizeof(result), &actual_len);
        if (ret != 0) {
            hvdxg.createdevice_last_ret = ret;
            break;
        }
        if (actual_len < sizeof(result) || result.device.v == 0) {
            ret = -EIO;
            hvdxg.createdevice_last_ret = ret;
            break;
        }
        req.device.v = result.device.v;
        hvdxg.createdevice_last_device = req.device.v;
        hvdxg.createdevice_adapter_equals_device =
            local_adapter == req.device.v ? 1 : 0;
        hvdxg.createdevice_host_adapter_equals_device =
            host_adapter == req.device.v ? 1 : 0;
        hvdxg.last_device_handle = req.device.v;
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        if (ret == 0 && owner != NULL) {
            if (hvdxg_track_object(owner, HV_DXG_OBJECT_DEVICE,
                                   req.device.v, local_adapter,
                                   req.device.v) != 0 ||
                hvdxg_track_u32_grow(&owner->devices,
                                      &owner->device_count,
                                      &owner->device_capacity,
                                      req.device.v) != 0) {
                hvdxg_untrack_object(owner, HV_DXG_OBJECT_DEVICE,
                                     req.device.v);
                (void)hvdxg_destroy_device_host(req.device.v);
                ret = -ENOMEM;
            }
        }
        hvdxg_note_createdevice_object(owner, req.device.v);
        hvdxg.createdevice_last_ret = ret;
        hvdxg_note_queryadapter_admission(
            HV_DXG_QAI_ADMISSION_KIND_CREATEDEVICE,
            HV_DXG_QAITYPE_CREATEDEVICE_MARKER, 0, req.device.v, ret, 0,
            hvdxg.vgpu_send_last_route_global ? HV_DXG_CHANNEL_GLOBAL :
            HV_DXG_CHANNEL_VGPU, 0, local_adapter, host_adapter, 0);
        break;
    }

    case LX_DXDESTROYDEVICE: {
        struct d3dkmt_destroydevice req;
        int child_ret;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EPERM;
            break;
        }
        (void)hvdxg_flush_device_host(req.device.v);
        child_ret = hvdxg_destroy_device_owned_objects(owner, req.device.v);
        ret = hvdxg_destroy_device_host(req.device.v);
        if (ret == 0)
            ret = child_ret;
        if (ret == 0 && hvdxg.last_device_handle == req.device.v)
            hvdxg.last_device_handle = 0;
        if (ret == 0 && owner != NULL) {
            hvdxg_untrack_object(owner, HV_DXG_OBJECT_DEVICE,
                                 req.device.v);
            hvdxg_untrack_u32(owner->devices, &owner->device_count,
                              req.device.v);
        }
        break;
    }

    case LX_DXCREATEALLOCATION: {
        struct d3dkmt_createallocation req;
        struct d3dkmt_createallocationflags requested_flags;
        struct d3dkmt_createstandardallocation standard_alloc;
        struct d3dddi_allocationinfo2 alloc_info[HV_DXG_ALLOCATION_MAX];
        uint32 alloc_priv_capacity[HV_DXG_ALLOCATION_MAX];
        uint64 *sysmem_pages[HV_DXG_ALLOCATION_MAX];
        uint32 sysmem_page_count[HV_DXG_ALLOCATION_MAX];
        uint8 *command_buf = NULL;
        uint8 *result_buf = NULL;
        struct hvdxg_command_createallocation *create =
            NULL;
        struct hvdxg_d3dkmthandle create_process = { 0 };
        struct hvdxg_command_createallocation_allocinfo *wire_alloc;
        struct hvdxg_command_createallocation_return *result =
            NULL;
        uint8 *private_data;
        uint8 *command_private_base;
        uint8 *alloc_private_data;
        uint8 *alloc_private_data_base = NULL;
        uint8 *input_alloc_private_data_base = NULL;
        uint8 *standard_alloc_priv_data = NULL;
        uint8 *standard_res_priv_data = NULL;
        uint8 *resource_priv_data = NULL;
        uint32 standard_alloc_priv_data_size = 0;
        uint32 standard_res_priv_data_size = 0;
        uint32 total_alloc_private = 0;
        uint32 tracked_alloc_private = 0;
        uint32 tracked_resource_private_size = 0;
        uint32 host_total_alloc_private = 0;
        uint32 total_private;
        uint32 command_len;
        uint32 result_min_len;
        uint32 result_len;
        uint32 actual_len = 0;
        uint32 requested_resource = 0;
        uint32 device_known = 0;
        int track_alloc_input_private = 0;
        int host_allocation_created = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        requested_flags = req.flags;
        requested_resource = req.resource.v;
        if (req.device.v == 0 || req.alloc_count == 0 ||
            req.alloc_count > HV_DXG_ALLOCATION_MAX ||
            req.allocation_info == 0) {
            ret = -EINVAL;
            break;
        }
        device_known = hvdxg_owner_has_device(owner, req.device.v);
        hvdxg.allocation_last_device = req.device.v;
        hvdxg.allocation_last_device_known = device_known;
        hvdxg.allocation_last_device_from_create = device_known ? 1 : 0;
        if (!device_known) {
            ret = -EPERM;
            break;
        }
        if (requested_flags.create_shared &&
            (!requested_flags.create_resource ||
             !requested_flags.nt_security_sharing)) {
            hvdxg.sharedresource_seal_denied++;
            ret = -EINVAL;
            break;
        }
        if (req.resource.v != 0) {
            struct hvdxg_tracked_resource *existing =
                hvdxg_owner_find_resource(owner, req.device.v,
                                          req.resource.v);

            if (existing != NULL && existing->sealed) {
                hvdxg.sharedresource_seal_denied++;
                hvdxg.sharedresource_seal_append_rejects++;
                hvdxg.sharedresource_seal_last_resource =
                    existing->resource;
                hvdxg.sharedresource_seal_last_generation =
                    existing->sealed_generation;
                ret = -EINVAL;
                break;
            }
        }
        if (req.flags.standard_allocation) {
            if (req.standard_allocation == 0 || req.priv_drv_data_size != 0 ||
                req.alloc_count != 1) {
                ret = -EINVAL;
                break;
            }
            if (either_copyin(&standard_alloc, 1, req.standard_allocation,
                              sizeof(standard_alloc)) < 0) {
                ret = -EFAULT;
                break;
            }
            if (standard_alloc.existing_heap_data.size == 0 ||
                (standard_alloc.existing_heap_data.size & (PGSIZE - 1)) != 0) {
                ret = -EINVAL;
                break;
            }
            if (standard_alloc.type == _D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP) {
                if (!req.flags.existing_sysmem) {
                    ret = -EINVAL;
                    break;
                }
            } else if (standard_alloc.type == _D3DKMT_STANDARDALLOCATIONTYPE_CROSSADAPTER) {
                if (req.flags.existing_sysmem) {
                    ret = -EINVAL;
                    break;
                }
            } else {
                ret = -EINVAL;
                break;
            }
            req.priv_drv_data_size = sizeof(standard_alloc);
        }
        if (req.private_runtime_data_size > HV_DXG_VM_BUS_PACKET_MAX ||
            req.priv_drv_data_size > HV_DXG_VM_BUS_PACKET_MAX ||
            (req.private_runtime_data_size != 0 &&
             req.private_runtime_data == 0) ||
            (req.priv_drv_data_size != 0 && req.priv_drv_data == 0 &&
             !req.flags.standard_allocation)) {
            ret = -EINVAL;
            break;
        }
        if (either_copyin(alloc_info, 1, req.allocation_info,
                          req.alloc_count * sizeof(alloc_info[0])) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.allocation_last_priv_size = alloc_info[0].priv_drv_data_size;
        hvdxg.allocation_last_runtime_size = req.private_runtime_data_size;
        hvdxg.allocation_last_resource_priv_size = req.priv_drv_data_size;
        hvdxg.allocation_last_flags = alloc_info[0].flags.value;
        hvdxg.allocation_last_sysmem = alloc_info[0].sysmem;
        hvdxg.allocation_last_priority = alloc_info[0].unused;
        hvdxg.existing_sysmem_last_pages = 0;
        hvdxg.existing_sysmem_last_pin_ret = 0;
        hvdxg.existing_sysmem_last_set_ret = 0;
        hvdxg.existing_sysmem_last_path = req.flags.existing_sysmem ? 1 : 0;
        hvdxg.existing_sysmem_last_standard =
            req.flags.standard_allocation ? 1 : 0;
        hvdxg.existing_sysmem_last_writable = req.flags.read_only ? 0 : 1;
        hvdxg.existing_sysmem_last_device = req.device.v;
        hvdxg.existing_sysmem_last_allocation = 0;
        hvdxg.existing_sysmem_last_va = alloc_info[0].sysmem;
        hvdxg.existing_sysmem_last_size = 0;
        hvdxg.existing_sysmem_last_first_pfn = 0;
        hvdxg.existing_sysmem_last_last_pfn = 0;
        hvdxg.existing_sysmem_last_pfnmap_pages = 0;
        hvdxg.existing_sysmem_last_vram = 0;
        hvdxg.existing_sysmem_last_vram_gpa = 0;
        hvdxg.existing_sysmem_last_vram_size = 0;
        hvdxg.allocation_last_in_priv_head_len = 0;
        hvdxg.allocation_last_out_priv_head_len = 0;
        hvdxg.allocation_last_process = 0;
        hvdxg.allocation_last_owner_process = 0;
        hvdxg.allocation_last_owner_generation = 0;
        hvdxg.allocation_last_device = req.device.v;
        hvdxg.allocation_last_device_known = device_known;
        hvdxg.allocation_last_device_from_create = device_known ? 1 : 0;
        memset(hvdxg.allocation_last_in_priv_head, 0,
               sizeof(hvdxg.allocation_last_in_priv_head));
        memset(hvdxg.allocation_last_out_priv_head, 0,
               sizeof(hvdxg.allocation_last_out_priv_head));
        memset(alloc_priv_capacity, 0, sizeof(alloc_priv_capacity));
        memset(sysmem_pages, 0, sizeof(sysmem_pages));
        memset(sysmem_page_count, 0, sizeof(sysmem_page_count));
        for (uint32 i = 0; i < req.alloc_count; i++) {
            if (alloc_info[i].sysmem != 0 &&
                (alloc_info[i].sysmem & (PGSIZE - 1)) != 0) {
                ret = -EINVAL;
                break;
            }
            if ((alloc_info[0].sysmem == 0) !=
                (alloc_info[i].sysmem == 0)) {
                ret = -EINVAL;
                break;
            }
            if (req.flags.standard_allocation &&
                alloc_info[i].priv_drv_data_size != 0) {
                ret = -EINVAL;
                break;
            }
            alloc_priv_capacity[i] = alloc_info[i].priv_drv_data_size;
            if (alloc_info[i].priv_drv_data_size > HV_DXG_VM_BUS_PACKET_MAX ||
                (alloc_info[i].priv_drv_data_size != 0 &&
                 alloc_info[i].priv_drv_data == 0)) {
                ret = -EINVAL;
                break;
            }
            total_alloc_private += alloc_info[i].priv_drv_data_size;
            if (total_alloc_private > HV_DXG_VM_BUS_PACKET_MAX) {
                ret = -EOVERFLOW;
                break;
            }
        }
        if (ret != 0)
            break;
        if (req.flags.standard_allocation) {
            ret = hvdxg_get_standard_alloc_priv_data(
                req.device.v, &standard_alloc,
                &standard_alloc_priv_data_size, &standard_alloc_priv_data,
                &standard_res_priv_data_size, &standard_res_priv_data);
            if (ret != 0)
                goto createallocation_done;
            resource_priv_data = standard_res_priv_data;
            tracked_resource_private_size = standard_res_priv_data_size;
        } else {
            ret = hvdxg_copy_user_bytes(&resource_priv_data,
                                        req.priv_drv_data,
                                        req.priv_drv_data_size);
            if (ret != 0)
                goto createallocation_done;
            tracked_resource_private_size = req.priv_drv_data_size;
        }
        total_private = req.private_runtime_data_size +
                        req.priv_drv_data_size + total_alloc_private;
        if (total_private > HV_DXG_VM_BUS_PACKET_MAX) {
            ret = -EOVERFLOW;
            goto createallocation_done;
        }

        command_len = sizeof(*create) +
                      req.alloc_count * sizeof(*wire_alloc) +
                      total_private;
        result_min_len = sizeof(*result) +
                         (req.alloc_count - 1) *
                             sizeof(struct hvdxg_command_allocinfo_return);
        result_len = result_min_len + total_alloc_private;
        hvdxg.allocation_last_cmd_len = command_len;
        hvdxg.allocation_last_hdr_size =
            sizeof(struct hvdxg_command_vgpu_to_host);
        hvdxg.allocation_last_prr_offset =
            offsetof(struct hvdxg_command_createallocation,
                     private_runtime_resource_handle);
        hvdxg.allocation_last_make_resident_offset =
            offsetof(struct hvdxg_command_createallocation,
                     make_resident);
        hvdxg.allocation_last_allocinfo_offset = sizeof(*create);
        hvdxg.allocation_last_private_offset =
            sizeof(*create) + req.alloc_count * sizeof(*wire_alloc);
        hvdxg.allocation_last_result_min_len = result_min_len;
        hvdxg.allocation_last_result_len = result_len;
        hvdxg.allocation_last_result_flags_offset =
            offsetof(struct hvdxg_command_createallocation_return, flags);
        hvdxg.allocation_last_result_resource_offset =
            offsetof(struct hvdxg_command_createallocation_return, resource);
        hvdxg.allocation_last_result_global_offset =
            offsetof(struct hvdxg_command_createallocation_return,
                     global_share);
        hvdxg.allocation_last_result_vgpu_offset =
            offsetof(struct hvdxg_command_createallocation_return,
                     vgpu_flags);
        hvdxg.allocation_last_result_allocinfo_offset =
            offsetof(struct hvdxg_command_createallocation_return,
                     allocation_info);
        hvdxg.allocation_last_result_allocinfo_size =
            sizeof(struct hvdxg_command_allocinfo_return);
        hvdxg.allocation_last_result_head_len = 0;
        hvdxg.allocation_last_wire_len = 0;
        hvdxg.allocation_last_ext = 0;
        hvdxg.allocation_last_ext_offset = 0;
        hvdxg.allocation_last_route_global = 0;
        hvdxg.allocation_last_send_ret = 0;
        memset(hvdxg.allocation_last_result_head, 0,
               sizeof(hvdxg.allocation_last_result_head));
        if (command_len > HV_DXG_VM_BUS_PACKET_MAX ||
            result_len > HV_DXG_VM_BUS_PACKET_MAX) {
            ret = -EOVERFLOW;
            goto createallocation_done;
        }
        command_buf = kvmalloc(command_len);
        result_buf = kvmalloc(result_len);
        if (command_buf == NULL || result_buf == NULL) {
            ret = -ENOMEM;
            goto createallocation_done;
        }
        memset(command_buf, 0, command_len);
        memset(result_buf, 0, result_len);
        create = (struct hvdxg_command_createallocation *)command_buf;
        result = (struct hvdxg_command_createallocation_return *)result_buf;
        create_process = hvdxg_owner_bound_process_handle(owner);
        hvdxg.allocation_last_process = create_process.v;
        hvdxg_command_vgpu_init_process(&create->hdr,
                                        HV_DXGK_VMBCOMMAND_CREATEALLOCATION,
                                        create_process);
        create->device.v = req.device.v;
        create->resource.v = req.resource.v;
        create->private_runtime_data_size = req.private_runtime_data_size;
        create->priv_drv_data_size = req.priv_drv_data_size;
        create->alloc_count = req.alloc_count;
        create->flags = req.flags;
        create->private_runtime_resource_handle =
            req.private_runtime_resource_handle;
        create->make_resident = 0;
        if (alloc_info[0].sysmem != 0)
            create->flags.existing_sysmem = 1;

        wire_alloc = (struct hvdxg_command_createallocation_allocinfo *)&create[1];
        command_private_base = (uint8 *)wire_alloc +
                               req.alloc_count * sizeof(wire_alloc[0]);
        private_data = command_private_base;
        if (req.flags.value == 0x47)
            hvdxg_capture_d3d12_runtime_user(&req);
        if (req.private_runtime_data_size != 0) {
            if (either_copyin(private_data, 1, req.private_runtime_data,
                              req.private_runtime_data_size) < 0) {
                ret = -EFAULT;
                goto createallocation_done;
            }
            private_data += req.private_runtime_data_size;
        }
        if (req.flags.standard_allocation) {
            memmove(private_data, &standard_alloc, sizeof(standard_alloc));
            private_data += sizeof(standard_alloc);
        } else if (req.priv_drv_data_size != 0) {
            if (either_copyin(private_data, 1, req.priv_drv_data,
                              req.priv_drv_data_size) < 0) {
                ret = -EFAULT;
                goto createallocation_done;
            }
            private_data += req.priv_drv_data_size;
        }
        input_alloc_private_data_base = private_data;
        for (uint32 i = 0; i < req.alloc_count; i++) {
            wire_alloc[i].flags = alloc_info[i].flags.value;
            wire_alloc[i].priv_drv_data_size = alloc_info[i].priv_drv_data_size;
            wire_alloc[i].vidpn_source_id = alloc_info[i].vidpn_source_id;
            if (alloc_info[i].priv_drv_data_size != 0) {
                if (either_copyin(private_data, 1, alloc_info[i].priv_drv_data,
                                  alloc_info[i].priv_drv_data_size) < 0) {
                    ret = -EFAULT;
                    break;
                }
                if (i == 0)
                    hvdxg_save_priv_head(hvdxg.allocation_last_in_priv_head,
                                         sizeof(hvdxg.allocation_last_in_priv_head),
                                         &hvdxg.allocation_last_in_priv_head_len,
                                         private_data,
                                         alloc_info[i].priv_drv_data_size);
                private_data += alloc_info[i].priv_drv_data_size;
            }
        }
        if (ret != 0)
            goto createallocation_done;

        hvdxg_normalize_d3d12_shared_alloc_priv(
            &req, alloc_info, command_private_base);
        hvdxg_note_d3d12_shared_createallocation_request(
            &req, alloc_info, command_private_base, create_process.v);
        ret = hvdxg_send_sync_vgpu(create, command_len, result,
                                   result_len, &actual_len);
        hvdxg.allocation_last_wire_len = hvdxg.vgpu_send_last_wire_len;
        hvdxg.allocation_last_ext = hvdxg.vgpu_send_last_ext;
        hvdxg.allocation_last_ext_offset = hvdxg.vgpu_send_last_ext_offset;
        hvdxg.allocation_last_route_global =
            hvdxg.vgpu_send_last_route_global;
        hvdxg.allocation_last_send_ret = hvdxg.vgpu_send_last_ret;
        hvdxg_save_priv_head(hvdxg.allocation_last_result_head,
                             sizeof(hvdxg.allocation_last_result_head),
                             &hvdxg.allocation_last_result_head_len,
                             result_buf,
                             actual_len < result_len ?
                                 actual_len : result_len);
        hvdxg.allocation_last_len = actual_len;
        hvdxg.allocation_last_ret = ret;
        hvdxg.allocation_last_count = req.alloc_count;
        hvdxg_note_d3d12_shared_createallocation_result(
            actual_len, ret, &req, requested_flags.value, alloc_info,
            NULL, NULL);
        if (ret != 0) {
            hvdxg_note_allocation_history(actual_len, ret, req.device.v,
                                          req.resource.v, 0, 0,
                                          req.alloc_count, total_private, req.global_share.v, requested_flags.value);
            goto createallocation_done;
        }
        if (actual_len < result_min_len ||
            result->allocation_info[0].allocation.v == 0) {
            ret = -EIO;
            hvdxg.allocation_last_ret = ret;
            hvdxg_note_allocation_history(actual_len, ret, req.device.v,
                                          req.resource.v, 0, 0,
                                          req.alloc_count, total_private, req.global_share.v, requested_flags.value);
            goto createallocation_done;
        }
        host_allocation_created = 1;
        req.resource.v = result->resource.v;
        req.global_share.v = result->global_share.v;
        alloc_private_data = result_buf + result_min_len;
        alloc_private_data_base = alloc_private_data;
        for (uint32 i = 0; i < req.alloc_count; i++) {
            uint32 host_private_size =
                result->allocation_info[i].priv_drv_data_size;
            alloc_info[i].allocation.v =
                result->allocation_info[i].allocation.v;
            alloc_info[i].priv_drv_data_size = host_private_size;
            if (host_private_size != 0) {
                if (host_private_size > alloc_priv_capacity[i] ||
                    alloc_info[i].priv_drv_data == 0 ||
                    (uint64)(alloc_private_data - result_buf) +
                        host_private_size > actual_len) {
                    ret = -EOVERFLOW;
                    break;
                }
                if (either_copyout(1, alloc_info[i].priv_drv_data,
                                   alloc_private_data,
                                   host_private_size) < 0) {
                    ret = -EFAULT;
                    break;
                }
                if (i == 0)
                    hvdxg_save_priv_head(hvdxg.allocation_last_out_priv_head,
                                         sizeof(hvdxg.allocation_last_out_priv_head),
                                         &hvdxg.allocation_last_out_priv_head_len,
                                         alloc_private_data,
                                         host_private_size);
                if (i == 0)
                    hvdxg_note_d3d12_shared_createallocation_result(
                        actual_len, ret, &req, requested_flags.value,
                        alloc_info, result, alloc_private_data);
                host_total_alloc_private += host_private_size;
                alloc_private_data += host_private_size;
            }
        }
        hvdxg_note_d3d12_shared_createallocation_result(
            actual_len, ret, &req, requested_flags.value, alloc_info, result,
            alloc_private_data_base);
        if (ret != 0) {
            hvdxg.allocation_last_ret = ret;
            hvdxg_note_allocation_history(actual_len, ret, req.device.v,
                                          req.resource.v,
                                          alloc_info[0].allocation.v,
                                          result->allocation_info[0].allocation_size,
                                          req.alloc_count, total_private, req.global_share.v, requested_flags.value);
            goto createallocation_done;
        }
        for (uint32 i = 0; i < req.alloc_count; i++) {
            if (alloc_info[i].sysmem != 0) {
                hvdxg.existing_sysmem_attempts++;
                hvdxg.existing_sysmem_last_path =
                    req.flags.existing_sysmem ? 1 : 0;
                hvdxg.existing_sysmem_last_standard =
                    req.flags.standard_allocation ? 1 : 0;
                hvdxg.existing_sysmem_last_writable =
                    req.flags.read_only ? 0 : 1;
                hvdxg.existing_sysmem_last_device = req.device.v;
                hvdxg.existing_sysmem_last_allocation =
                    alloc_info[i].allocation.v;
                hvdxg.existing_sysmem_last_va = alloc_info[i].sysmem;
                hvdxg.existing_sysmem_last_size =
                    result->allocation_info[i].allocation_size;
                hvdxg.existing_sysmem_last_first_pfn = 0;
                hvdxg.existing_sysmem_last_last_pfn = 0;
                hvdxg.existing_sysmem_last_pfnmap_pages = 0;
                hvdxg.existing_sysmem_last_vram = 0;
                hvdxg.existing_sysmem_last_vram_gpa = 0;
                hvdxg.existing_sysmem_last_vram_size = 0;
                ret = hvdxg_pin_existing_sysmem(
                    alloc_info[i].sysmem,
                    result->allocation_info[i].allocation_size,
                    !req.flags.read_only,
                    &sysmem_pages[i],
                    &sysmem_page_count[i],
                    &hvdxg.existing_sysmem_last_pfnmap_pages);
                hvdxg.existing_sysmem_last_pages = sysmem_page_count[i];
                hvdxg.existing_sysmem_last_pin_ret = ret;
                if (ret == 0 && sysmem_page_count[i] != 0) {
                    hvdxg.existing_sysmem_last_first_pfn =
                        sysmem_pages[i][0] >> PGSHIFT;
                    hvdxg.existing_sysmem_last_last_pfn =
                        sysmem_pages[i][sysmem_page_count[i] - 1] >>
                        PGSHIFT;
                    if (platform.has_framebuffer) {
                        uint64 first_pa = sysmem_pages[i][0];
                        uint64 last_pa =
                            sysmem_pages[i][sysmem_page_count[i] - 1];
                        uint64 fb_base = platform.framebuffer_base;
                        uint64 fb_size = platform.framebuffer_size;
                        uint64 fb_end = fb_base + fb_size;
                        uint64 alloc_end = last_pa + PGSIZE;

                        if (fb_size != 0 && fb_end > fb_base &&
                            first_pa < fb_end && fb_base < alloc_end) {
                            hvdxg.existing_sysmem_last_vram = 1;
                            hvdxg.existing_sysmem_last_vram_gpa = fb_base;
                            hvdxg.existing_sysmem_last_vram_size = fb_size;
                        }
                    }
                }
                if (ret != 0) {
                    hvdxg.allocation_last_ret = ret;
                    hvdxg_note_allocation_history(
                        actual_len, ret, req.device.v, req.resource.v,
                        alloc_info[i].allocation.v,
                        result->allocation_info[i].allocation_size,
                        req.alloc_count, total_private, req.global_share.v, requested_flags.value);
                    goto createallocation_done;
                }
                hvdxg.existing_sysmem_pin_successes++;
                if (hvdxg.existing_sysmem_last_pfnmap_pages != 0)
                    hvdxg.existing_sysmem_pfnmap_successes++;
                hvdxg.existing_sysmem_total_pages += sysmem_page_count[i];
                ret = hvdxg_set_existing_sysmem_pages(
                    req.device.v, alloc_info[i].allocation.v,
                    sysmem_pages[i], sysmem_page_count[i]);
                hvdxg.existing_sysmem_last_set_ret = ret;
                if (ret != 0) {
                    hvdxg.allocation_last_ret = ret;
                    hvdxg_note_allocation_history(
                        actual_len, ret, req.device.v, req.resource.v,
                        alloc_info[i].allocation.v,
                        result->allocation_info[i].allocation_size,
                        req.alloc_count, total_private, req.global_share.v, requested_flags.value);
                    goto createallocation_done;
                }
                hvdxg.existing_sysmem_set_successes++;
            }
        }
        for (uint32 i = 0; i < req.alloc_count; i++) {
            uint64 user_alloc_handle =
                req.allocation_info +
                (uint64)i * sizeof(struct d3dddi_allocationinfo2) +
                offsetof(struct d3dddi_allocationinfo2, allocation);
            if (either_copyout(1, user_alloc_handle,
                               &alloc_info[i].allocation,
                               sizeof(alloc_info[i].allocation)) < 0) {
                ret = -EFAULT;
                break;
            }
        }
        if (ret == 0 && req.flags.create_resource &&
            either_copyout(1,
                           (uint64)arg +
                               offsetof(struct d3dkmt_createallocation,
                                        resource),
                           &req.resource, sizeof(req.resource)) < 0) {
            ret = -EFAULT;
        }
        if (ret == 0 &&
            either_copyout(1,
                           (uint64)arg +
                               offsetof(struct d3dkmt_createallocation,
                                        global_share),
                           &req.global_share, sizeof(req.global_share)) < 0) {
            ret = -EFAULT;
        }
        if (ret != 0) {
            hvdxg.allocation_last_ret = ret;
            hvdxg_note_allocation_history(actual_len, ret, req.device.v,
                                          req.resource.v,
                                          alloc_info[0].allocation.v,
                                          result->allocation_info[0].allocation_size,
                                          req.alloc_count, total_private, req.global_share.v, requested_flags.value);
            goto createallocation_done;
        }
        hvdxg.last_device_handle = req.device.v;
        hvdxg.last_resource_handle = req.resource.v;
        hvdxg.last_allocation_handle = alloc_info[0].allocation.v;
        hvdxg.last_allocation_device = req.device.v;
        hvdxg.last_allocation_size = result->allocation_info[0].allocation_size;
        if (owner != NULL) {
            track_alloc_input_private =
                req.flags.create_resource &&
                requested_flags.create_shared &&
                requested_flags.nt_security_sharing;
            tracked_alloc_private = req.flags.standard_allocation ?
                                    standard_alloc_priv_data_size :
                                    (track_alloc_input_private ?
                                         total_alloc_private :
                                         host_total_alloc_private);
            ret = hvdxg_track_resource(owner, &req, requested_flags,
                                        alloc_info, result,
                                        req.flags.standard_allocation ?
                                            standard_alloc_priv_data :
                                            (track_alloc_input_private ?
                                                 input_alloc_private_data_base :
                                                 alloc_private_data_base),
                                        track_alloc_input_private ?
                                            alloc_priv_capacity : NULL,
                                        tracked_alloc_private,
                                        track_alloc_input_private ? 0 :
                                            (req.flags.standard_allocation ? 0 : 1),
                                        req.private_runtime_data_size != 0 ?
                                            command_private_base : NULL,
                                        req.flags.standard_allocation ?
                                            resource_priv_data :
                                            command_private_base +
                                                req.private_runtime_data_size,
                                        tracked_resource_private_size);
            if (ret != 0)
                goto createallocation_done;
            for (uint32 i = 0; i < req.alloc_count; i++) {
                ret = hvdxg_track_allocation(
                    owner, req.device.v, req.resource.v,
                    alloc_info[i].allocation.v,
                    result->allocation_info[i].allocation_size,
                    result->allocation_info[i].allocation_flags,
                    alloc_info[i].sysmem, sysmem_pages[i],
                    sysmem_page_count[i]);
                if (ret != 0)
                    goto createallocation_done;
                hvdxg_link_resource_allocation(
                    owner, req.device.v, req.resource.v,
                    alloc_info[i].allocation.v,
                    result->allocation_info[i].allocation_size,
                    result->allocation_info[i].allocation_flags);
                sysmem_pages[i] = NULL;
                sysmem_page_count[i] = 0;
                hvdxg_note_allocation_history(
                    actual_len, ret, req.device.v, req.resource.v,
                    alloc_info[i].allocation.v,
                    result->allocation_info[i].allocation_size,
                    req.alloc_count, total_private, req.global_share.v, requested_flags.value);
            }
        }
createallocation_done:
        if (ret != 0 && owner != NULL && req.device.v != 0) {
            if (req.resource.v != 0) {
                hvdxg_untrack_allocation(owner, req.device.v,
                                         req.resource.v, 0);
                hvdxg_untrack_resource(owner, req.device.v,
                                       req.resource.v);
            }
            for (uint32 i = 0; i < req.alloc_count &&
                                i < HV_DXG_ALLOCATION_MAX; i++) {
                if (alloc_info[i].allocation.v != 0)
                    hvdxg_untrack_allocation(
                        owner, req.device.v, req.resource.v,
                        alloc_info[i].allocation.v);
            }
        }
        if (ret != 0 && host_allocation_created) {
            int unwind_ret =
                hvdxg_destroy_createallocation_result(
                    create_process.v, req.device.v, requested_resource, result,
                    req.alloc_count);

            if (unwind_ret != 0 && hvdxg.allocation_last_ret == ret)
                hvdxg.allocation_last_ret = unwind_ret;
        }
        if (command_buf != NULL)
            kvfree(command_buf);
        if (result_buf != NULL)
            kvfree(result_buf);
        if (standard_alloc_priv_data != NULL)
            kvfree(standard_alloc_priv_data);
        if (standard_res_priv_data != NULL)
            kvfree(standard_res_priv_data);
        if (!req.flags.standard_allocation && resource_priv_data != NULL)
            kvfree(resource_priv_data);
        for (uint32 i = 0; i < HV_DXG_ALLOCATION_MAX; i++)
            hvdxg_unpin_existing_sysmem_pages(sysmem_pages[i],
                                              sysmem_page_count[i]);
        break;
    }

    case LX_DXDESTROYALLOCATION2: {
        struct d3dkmt_destroyallocation2 req;
        uint8 command_buf[sizeof(struct hvdxg_command_destroyallocation) +
                          HV_DXG_ALLOCATION_MAX *
                              sizeof(struct hvdxg_d3dkmthandle)];
        struct hvdxg_command_destroyallocation *destroy =
            (struct hvdxg_command_destroyallocation *)command_buf;
        struct hvdxg_ntstatus status;
        struct hvdxg_tracked_resource *resource = NULL;
        uint32 actual_len = 0;
        uint32 command_len;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0 || req.alloc_count > HV_DXG_ALLOCATION_MAX ||
            (req.alloc_count != 0 && req.allocations == 0)) {
            ret = -EINVAL;
            break;
        }
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(&destroy->hdr,
                                        HV_DXGK_VMBCOMMAND_DESTROYALLOCATION,
                                        hvdxg.dxg_process);
        destroy->device.v = req.device.v;
        destroy->resource.v = req.resource.v;
        destroy->alloc_count = req.alloc_count;
        destroy->flags = req.flags;
        if (req.alloc_count != 0 &&
            either_copyin(destroy->allocations, 1, req.allocations,
                          req.alloc_count * sizeof(destroy->allocations[0])) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.alloc_count == 0) {
            resource = hvdxg_owner_find_resource(owner, req.device.v,
                                                 req.resource.v);
            if (!hvdxg_owner_has_allocation(owner, req.device.v,
                                            req.resource.v, 0)) {
                ret = -EPERM;
                break;
            }
        } else {
            for (uint32 i = 0; i < req.alloc_count; i++) {
                if (!hvdxg_owner_has_allocation(owner, req.device.v,
                                                req.resource.v,
                                                destroy->allocations[i].v)) {
                    ret = -EPERM;
                    break;
                }
            }
            if (ret != 0)
                break;
        }
        command_len = sizeof(*destroy) +
                      req.alloc_count * sizeof(destroy->allocations[0]);
        ret = hvdxg_send_sync_vgpu(destroy, command_len, &status,
                                   sizeof(status), &actual_len);
        if (ret == 0 && actual_len < sizeof(status))
            ret = -EOVERFLOW;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg_note_destroyallocation(
            req.device.v, req.resource.v,
            req.alloc_count != 0 ? destroy->allocations[0].v : 0,
            destroy->hdr.process.v, HV_DXG_DESTROY_ALLOC_CTX_IOCTL,
            req.alloc_count, actual_len, ret, status);
        if (ret == -EINVAL && req.alloc_count == 0 && resource != NULL &&
            resource->host_shared_handle_nt != 0 &&
            resource->host_shared_process != 0 &&
            resource->host_shared_object != 0) {
            int defer_ret = hvdxg_defer_shared_resource_destroy(
                resource->host_shared_process,
                resource->host_shared_object,
                resource->host_shared_handle_nt,
                req.device.v, req.resource.v);
            if (defer_ret == 0)
                ret = 0;
        }
        if (ret == 0 && owner != NULL) {
            if (req.alloc_count == 0) {
                hvdxg_untrack_resource(owner, req.device.v, req.resource.v);
                hvdxg_untrack_allocation(owner, req.device.v, req.resource.v,
                                         0);
            } else {
                for (uint32 i = 0; i < req.alloc_count; i++)
                    hvdxg_untrack_allocation(owner, req.device.v,
                                             req.resource.v,
                                             destroy->allocations[i].v);
                if (!hvdxg_owner_has_allocation(owner, req.device.v,
                                                req.resource.v, 0))
                    hvdxg_untrack_resource(owner, req.device.v,
                                           req.resource.v);
            }
        }
        break;
    }

    case LX_DXGETDEVICESTATE: {
        struct d3dkmt_getdevicestate req;
        struct hvdxg_command_getdevicestate getstate;
        struct hvdxg_command_getdevicestate_return result;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EPERM;
            break;
        }
        memset(&getstate, 0, sizeof(getstate));
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(&getstate.hdr,
                                        HV_DXGK_VMBCOMMAND_GETDEVICESTATE,
                                        hvdxg.dxg_process);
        getstate.args = req;
        ret = hvdxg_send_sync_vgpu(&getstate, sizeof(getstate), &result,
                                   sizeof(result), &actual_len);
        if (ret != 0)
            break;
        if (actual_len < sizeof(result)) {
            ret = -EOVERFLOW;
            break;
        }
        ret = hvdxg_ntstatus_to_errno(result.status);
        if (ret != 0)
            break;
        req = result.args;
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        break;
    }

    case LX_DXCREATEPAGINGQUEUE: {
        struct d3dkmt_createpagingqueue req;
        struct hvdxg_command_createpagingqueue create;
        struct hvdxg_command_createpagingqueue_return result;
        uint32 actual_len = 0;
        uint64 fence_kva = 0;
        uint64 fence_pa = 0;
        uint64 raw_fence_pa = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EPERM;
            break;
        }
        memset(&create, 0, sizeof(create));
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(&create.hdr,
                                        HV_DXGK_VMBCOMMAND_CREATEPAGINGQUEUE,
                                        hvdxg.dxg_process);
        create.args = req;
        create.args.paging_queue.v = 0;
        create.args.sync_object.v = 0;
        create.args.fence_cpu_virtual_address = 0;
        ret = hvdxg_send_sync_vgpu(&create, sizeof(create), &result,
                                   sizeof(result), &actual_len);
        hvdxg.pagingqueue_last_len = actual_len;
        hvdxg.pagingqueue_last_ret = ret;
        hvdxg.pagingqueue_last_queue = result.paging_queue.v;
        hvdxg.pagingqueue_last_sync = result.sync_object.v;
        hvdxg.pagingqueue_last_fence_pa = result.fence_storage_physical_address;
        hvdxg.pagingqueue_last_fence_off = result.fence_storage_offset;
        if (ret != 0)
            break;
        if (actual_len < sizeof(result) || result.paging_queue.v == 0) {
            ret = -EIO;
            break;
        }
        req.paging_queue.v = result.paging_queue.v;
        req.sync_object.v = result.sync_object.v;
        raw_fence_pa = result.fence_storage_physical_address;
        req.fence_cpu_virtual_address = hvdxg_map_iospace_user_canonical(
            HV_DXG_FENCE_SOURCE_PAGINGQUEUE, raw_fence_pa, sizeof(uint64),
            0, &fence_pa, NULL, 0);
        fence_kva = hvdxg_map_iospace_kernel_canonical(
            HV_DXG_FENCE_SOURCE_PAGINGQUEUE, raw_fence_pa, sizeof(uint64),
            fence_pa, 0);
        hvdxg_note_fence_offset_candidate(HV_DXG_FENCE_SOURCE_PAGINGQUEUE,
                                          raw_fence_pa,
                                          result.fence_storage_offset,
                                          sizeof(uint64));
        if (req.fence_cpu_virtual_address == 0) {
            ret = -ENOMEM;
            break;
        }
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        if (ret == 0 && owner != NULL) {
            hvdxg_track_pagingqueue(owner, req.device.v, req.paging_queue.v,
                                    req.sync_object.v, fence_pa);
            hvdxg_track_sync(owner, req.device.v, req.sync_object.v,
                             _D3DDDI_MONITORED_FENCE,
                             0, result.sync_object.v,
                             req.fence_cpu_virtual_address, fence_kva, 1);
        }
        break;
    }

    case LX_DXDESTROYPAGINGQUEUE: {
        struct d3dddi_destroypagingqueue req;
        struct hvdxg_command_destroypagingqueue destroy;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.paging_queue.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_pagingqueue(owner, req.paging_queue.v)) {
            ret = -EPERM;
            break;
        }
        memset(&destroy, 0, sizeof(destroy));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(&destroy.hdr,
                                        HV_DXGK_VMBCOMMAND_DESTROYPAGINGQUEUE,
                                        hvdxg.dxg_process);
        destroy.paging_queue.v = req.paging_queue.v;
        ret = hvdxg_send_sync_vgpu(&destroy, sizeof(destroy), &status,
                                   sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.destroypaging_last_status = status.v;
        if (ret == 0 && actual_len < sizeof(status))
            ret = -EOVERFLOW;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.destroypaging_last_len = actual_len;
        hvdxg.destroypaging_last_ret = ret;
        if (ret == 0 && owner != NULL) {
            uint32 sync = hvdxg_untrack_pagingqueue(owner,
                                                    req.paging_queue.v);
            if (sync != 0)
                hvdxg_untrack_sync(owner, sync);
        }
        break;
    }

    case LX_DXMAKERESIDENT: {
        struct d3dddi_makeresident req;
        struct hvdxg_command_makeresident *make = NULL;
        struct hvdxg_command_makeresident_return result;
        uint32 actual_len = 0;
        uint32 command_len;
        uint32 paging_sync = 0;
        int host_ret = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.paging_queue.v == 0 || req.alloc_count == 0 ||
            req.alloc_count > HV_DXG_RESIDENCY_ALLOCATION_MAX ||
            req.allocation_list == 0) {
            ret = -EINVAL;
            break;
        }
        /*
         * WSL's dxgkvmb_command_makeresident is naturally 8-byte aligned.
         * The allocation array starts at the same offset in our packed wire
         * struct, but the host also expects the trailing alignment dword.
         */
        command_len = sizeof(*make) +
                      (req.alloc_count - 1) * sizeof(make->allocations[0]) +
                      sizeof(uint32);
        if (command_len > HV_DXG_VM_BUS_PACKET_MAX) {
            ret = -EOVERFLOW;
            break;
        }
        make = kvmalloc(command_len);
        if (make == NULL) {
            ret = -ENOMEM;
            break;
        }
        memset(make, 0, command_len);
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(&make->hdr,
                                        HV_DXGK_VMBCOMMAND_MAKERESIDENT,
                                        hvdxg.dxg_process);
        make->paging_queue.v = req.paging_queue.v;
        make->flags = req.flags;
        make->alloc_count = req.alloc_count;
        if (either_copyin(make->allocations, 1, req.allocation_list,
                          req.alloc_count * sizeof(make->allocations[0])) < 0) {
            ret = -EFAULT;
            goto makeresident_done;
        }
        hvdxg.makeresident_last_device = 0;
        hvdxg.makeresident_last_paging_queue = req.paging_queue.v;
        hvdxg.makeresident_last_flags = req.flags.value;
        hvdxg.makeresident_last_count = req.alloc_count;
        hvdxg.makeresident_last_sorted = 0;
        hvdxg.makeresident_last_cmd_len = command_len;
        hvdxg.makeresident_last_wsl_cmd_len =
            sizeof(*make) +
            (req.alloc_count - 1) * sizeof(make->allocations[0]) +
            sizeof(uint32);
        hvdxg.makeresident_last_result_len = sizeof(result);
        hvdxg.makeresident_last_actual_len = 0;
        hvdxg.makeresident_last_host_ret = 0;
        hvdxg.makeresident_last_user_ret = 0;
        hvdxg.makeresident_last_pending_ok = 0;
        hvdxg.makeresident_last_status = 0;
        hvdxg.makeresident_last_sync = 0;
        hvdxg.makeresident_last_fence_current = 0;
        hvdxg.makeresident_last_owner_ok_count = 0;
        hvdxg.makeresident_last_tracked_count = 0;
        hvdxg.makeresident_last_order_matches = 1;
        memset(hvdxg.makeresident_last_in_alloc, 0,
               sizeof(hvdxg.makeresident_last_in_alloc));
        memset(hvdxg.makeresident_last_wire_alloc, 0,
               sizeof(hvdxg.makeresident_last_wire_alloc));
        memset(hvdxg.makeresident_last_owner_dev, 0,
               sizeof(hvdxg.makeresident_last_owner_dev));
        memset(hvdxg.makeresident_last_owner_res, 0,
               sizeof(hvdxg.makeresident_last_owner_res));
        memset(hvdxg.makeresident_last_owner_proc, 0,
               sizeof(hvdxg.makeresident_last_owner_proc));
        memset(hvdxg.makeresident_last_owner_gen, 0,
               sizeof(hvdxg.makeresident_last_owner_gen));
        memset(hvdxg.makeresident_last_owner_refs, 0,
               sizeof(hvdxg.makeresident_last_owner_refs));
        for (uint32 i = 0; i < req.alloc_count && i < 4; i++)
            hvdxg.makeresident_last_in_alloc[i] = make->allocations[i].v;
        if (!hvdxg_owner_has_pagingqueue(owner, req.paging_queue.v)) {
            ret = -EPERM;
            goto makeresident_done;
        }
        paging_sync = hvdxg_owner_pagingqueue_sync(owner,
                                                   req.paging_queue.v);
        hvdxg.makeresident_last_sync = paging_sync;
        for (uint32 i = 0; i < req.alloc_count; i++) {
            struct hvdxg_tracked_allocation *tracked;
            struct hvdxg_object_entry *object;
            int owner_ok;

            tracked = hvdxg_owner_find_allocation(
                owner, 0, 0, make->allocations[i].v);
            object = hvdxg_owner_find_object(
                owner, HV_DXG_OBJECT_ALLOCATION,
                make->allocations[i].v);
            owner_ok = hvdxg_owner_has_allocation(
                owner, 0, 0, make->allocations[i].v);
            if (i < 2 && tracked != NULL) {
                hvdxg.makeresident_last_owner_dev[i] =
                    tracked->device;
                hvdxg.makeresident_last_owner_res[i] =
                    tracked->resource;
                hvdxg.makeresident_last_owner_proc[i] =
                    tracked->owner_process;
                hvdxg.makeresident_last_owner_gen[i] =
                    tracked->owner_generation;
                hvdxg.makeresident_last_owner_refs[i] =
                    tracked->owner_refs;
            } else if (i < 2 && object != NULL) {
                hvdxg.makeresident_last_owner_dev[i] =
                    object->device;
                hvdxg.makeresident_last_owner_res[i] =
                    (uint32)object->parent;
                hvdxg.makeresident_last_owner_proc[i] =
                    hvdxg_open_host_process(owner);
                hvdxg.makeresident_last_owner_gen[i] =
                    hvdxg_open_process_generation(owner);
                hvdxg.makeresident_last_owner_refs[i] =
                    hvdxg_open_process_refs(owner);
            }
            if (tracked != NULL)
                hvdxg.makeresident_last_tracked_count++;
            if (owner_ok)
                hvdxg.makeresident_last_owner_ok_count++;
            if (!owner_ok) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            goto makeresident_done;
        /*
         * WSL leaves the VM-bus device field zero for MAKERESIDENT even
         * though the wire struct carries it.
         */
        make->device.v = 0;
        hvdxg.makeresident_last_device = make->device.v;
        for (uint32 i = 0; i < req.alloc_count && i < 4; i++) {
            hvdxg.makeresident_last_wire_alloc[i] = make->allocations[i].v;
            if (hvdxg.makeresident_last_wire_alloc[i] !=
                hvdxg.makeresident_last_in_alloc[i])
                hvdxg.makeresident_last_order_matches = 0;
        }
        host_ret = hvdxg_send_sync_vgpu(make, command_len, &result,
                                        sizeof(result), &actual_len);
        hvdxg.makeresident_last_actual_len = actual_len;
        hvdxg.makeresident_last_host_ret = host_ret;
        ret = host_ret;
        if (ret == 0 && actual_len < sizeof(result)) {
            ret = -EOVERFLOW;
        } else if (ret == 0) {
            hvdxg.makeresident_last_status = result.status.v;
            ret = hvdxg_ntstatus_to_errno(result.status);
        }
        /*
         * Match WSL's make-resident contract: STATUS_PENDING is a
         * positive success code returned to user mode, and the paging
         * fence/trim outputs must still be copied back.  Mesa waits on
         * that fence before locking/touching the allocation.
         */
        if (ret == 0 || ret == HV_DXG_STATUS_PENDING) {
            int copy_ret = ret;

            req.paging_fence_value = result.paging_fence_value;
            req.num_bytes_to_trim = result.num_bytes_to_trim;
            hvdxg.makeresident_last_pending_ok =
                copy_ret == HV_DXG_STATUS_PENDING &&
                result.status.v == HV_DXG_STATUS_PENDING;
            if (owner != NULL && result.paging_fence_value != 0) {
                for (uint32 i = 0; i < req.alloc_count; i++) {
                    hvdxg_note_allocation_resident(
                        owner, make->allocations[i].v, req.paging_queue.v,
                        result.paging_fence_value);
                }
            }
            hvdxg.makeresident_last_fence_current =
                hvdxg_owner_sync_fence_value(owner, paging_sync);
            ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
                  -EFAULT : copy_ret;
        }
        if ((req.alloc_count > 1 ||
             (ret != 0 && ret != HV_DXG_STATUS_PENDING)) &&
            hvdxg.makeresident_diag_prints < 32) {
            printf("hyperv-dxg: makeresident wire device=0x%x pq=0x%x "
                   "flags=0x%x count=%u sorted=%u status=0x%x ret=%d "
                   "host_ret=%d pending_ok=%u "
                   "fence=%lu cur=%lu sync=0x%x trim=%lu "
                   "in=%x,%x,%x,%x wire=%x,%x,%x,%x\n",
                   hvdxg.makeresident_last_device,
                   hvdxg.makeresident_last_paging_queue,
                   hvdxg.makeresident_last_flags,
                   hvdxg.makeresident_last_count,
                   hvdxg.makeresident_last_sorted,
                   (uint32)hvdxg.makeresident_last_status,
                   ret, hvdxg.makeresident_last_host_ret,
                   hvdxg.makeresident_last_pending_ok,
                   result.paging_fence_value,
                   hvdxg.makeresident_last_fence_current,
                   hvdxg.makeresident_last_sync,
                   result.num_bytes_to_trim,
                   hvdxg.makeresident_last_in_alloc[0],
                   hvdxg.makeresident_last_in_alloc[1],
                   hvdxg.makeresident_last_in_alloc[2],
                   hvdxg.makeresident_last_in_alloc[3],
                   hvdxg.makeresident_last_wire_alloc[0],
                   hvdxg.makeresident_last_wire_alloc[1],
                   hvdxg.makeresident_last_wire_alloc[2],
                   hvdxg.makeresident_last_wire_alloc[3]);
            hvdxg.makeresident_diag_prints++;
        }
makeresident_done:
        hvdxg.makeresident_last_len = actual_len;
        hvdxg.makeresident_last_actual_len = actual_len;
        hvdxg.makeresident_last_ret = ret;
        hvdxg.makeresident_last_user_ret = ret;
        hvdxg.makeresident_last_fence = req.paging_fence_value;
        hvdxg.makeresident_last_trim = req.num_bytes_to_trim;
        if (make != NULL)
            kvfree(make);
        break;
    }

    case LX_DXEVICT: {
        struct d3dkmt_evict req;
        struct hvdxg_command_evict *evict = NULL;
        struct hvdxg_command_evict_return result;
        uint32 actual_len = 0;
        uint32 command_len;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0 || req.alloc_count == 0 ||
            req.alloc_count > HV_DXG_RESIDENCY_ALLOCATION_MAX ||
            req.allocations == 0) {
            ret = -EINVAL;
            break;
        }
        command_len = sizeof(*evict) +
                      (req.alloc_count - 1) * sizeof(evict->allocations[0]);
        if (command_len > HV_DXG_VM_BUS_PACKET_MAX) {
            ret = -EOVERFLOW;
            break;
        }
        evict = kvmalloc(command_len);
        if (evict == NULL) {
            ret = -ENOMEM;
            break;
        }
        memset(evict, 0, command_len);
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(&evict->hdr,
                                        HV_DXGK_VMBCOMMAND_EVICT,
                                        hvdxg.dxg_process);
        evict->device.v = req.device.v;
        evict->flags = req.flags;
        evict->alloc_count = req.alloc_count;
        if (either_copyin(evict->allocations, 1, req.allocations,
                          req.alloc_count * sizeof(evict->allocations[0])) < 0) {
            ret = -EFAULT;
            goto evict_done;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EPERM;
            goto evict_done;
        }
        for (uint32 i = 0; i < req.alloc_count; i++) {
            if (!hvdxg_owner_has_allocation(owner, req.device.v, 0,
                                            evict->allocations[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            goto evict_done;
        ret = hvdxg_send_sync_vgpu(evict, command_len, &result,
                                   sizeof(result), &actual_len);
        if (ret == 0 && actual_len >= sizeof(result)) {
            req.num_bytes_to_trim = result.num_bytes_to_trim;
            ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
                  -EFAULT : 0;
        }
evict_done:
        hvdxg.evict_last_len = actual_len;
        hvdxg.evict_last_ret = ret;
        hvdxg.evict_last_trim = req.num_bytes_to_trim;
        if (evict != NULL)
            kvfree(evict);
        break;
    }

    case LX_DXMAPGPUVIRTUALADDRESS: {
        struct d3dddi_mapgpuvirtualaddress req;
        struct hvdxg_command_mapgpuvirtualaddress map;
        struct hvdxg_command_mapgpuvirtualaddress_return result;
        uint32 actual_len = 0;
        uint64 allocation_pages = 0;
        uint64 mapped_pages = 0;
        int map_ok = 0;
        uint32 paging_sync = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.paging_queue.v == 0 || req.size_in_pages == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_pagingqueue(owner, req.paging_queue.v)) {
            ret = -EPERM;
            break;
        }
        paging_sync = hvdxg_owner_pagingqueue_sync(owner,
                                                   req.paging_queue.v);
        if (req.allocation.v != 0) {
            if (!hvdxg_owner_has_allocation(owner, 0, 0,
                                            req.allocation.v)) {
                ret = -EPERM;
                break;
            }
        } else {
            uint64 map_size = req.size_in_pages << 12;

            if ((map_size >> 12) != req.size_in_pages ||
                req.base_address == 0 ||
                (!hvdxg_owner_gpuva_contains(owner, req.paging_queue.v,
                                             req.base_address, map_size) &&
                 !hvdxg_owner_gpuva_contains(owner, 0, req.base_address,
                                             map_size))) {
                ret = -EPERM;
                break;
            }
        }
        memset(&map, 0, sizeof(map));
        memset(&result, 0, sizeof(result));
        hvdxg.mapgpuva_last_status = 0;
        hvdxg.mapgpuva_last_paging_queue = req.paging_queue.v;
        hvdxg.mapgpuva_last_allocation = req.allocation.v;
        hvdxg.mapgpuva_last_base = req.base_address;
        hvdxg.mapgpuva_last_min = req.minimum_address;
        hvdxg.mapgpuva_last_max = req.maximum_address;
        hvdxg.mapgpuva_last_size_pages = req.size_in_pages;
        hvdxg.mapgpuva_last_protection = req.protection.value;
        hvdxg.mapgpuva_last_driver_protection = req.driver_protection;
        hvdxg.mapgpuva_last_sync = paging_sync;
        hvdxg.mapgpuva_last_fence_current = 0;
        hvdxg_command_vgpu_init_process(&map.hdr,
                                        HV_DXGK_VMBCOMMAND_MAPGPUVIRTUALADDRESS,
                                        hvdxg.dxg_process);
        map.args = req;
        map.device.v = 0;
        mapped_pages = req.size_in_pages;
        ret = hvdxg_send_sync_vgpu(&map, sizeof(map), &result,
                                   sizeof(result), &actual_len);
        if (actual_len >= sizeof(result))
            hvdxg.mapgpuva_last_status = result.status.v;
        if (ret == 0 && actual_len < sizeof(result))
            ret = -EOVERFLOW;
        else if (ret == 0)
            ret = hvdxg_ntstatus_to_errno(result.status);
        if (ret != 0 &&
            (uint32)hvdxg.mapgpuva_last_status == 0xC0000001U &&
            req.allocation.v != 0 &&
            hvdxg.last_allocation_handle == req.allocation.v &&
            hvdxg.last_allocation_size != 0) {
            allocation_pages =
                (hvdxg.last_allocation_size + PGSIZE - 1) >> 12;
            if (allocation_pages != 0 &&
                req.size_in_pages > allocation_pages) {
                struct d3dddi_mapgpuvirtualaddress retry_args = req;

                retry_args.size_in_pages = allocation_pages;
                retry_args.virtual_address = 0;
                retry_args.paging_fence_value = 0;
                memset(&map, 0, sizeof(map));
                memset(&result, 0, sizeof(result));
                hvdxg_command_vgpu_init_process(
                    &map.hdr, HV_DXGK_VMBCOMMAND_MAPGPUVIRTUALADDRESS,
                    hvdxg.dxg_process);
                map.args = retry_args;
                map.device.v = 0;
                actual_len = 0;
                mapped_pages = allocation_pages;
                ret = hvdxg_send_sync_vgpu(&map, sizeof(map), &result,
                                           sizeof(result), &actual_len);
                if (actual_len >= sizeof(result))
                    hvdxg.mapgpuva_last_status = result.status.v;
                if (ret == 0 && actual_len < sizeof(result))
                    ret = -EOVERFLOW;
                else if (ret == 0)
                    ret = hvdxg_ntstatus_to_errno(result.status);
            }
        }
        if (ret == 0 || ret == HV_DXG_STATUS_PENDING) {
            int copy_ret = ret;

            req.virtual_address = result.virtual_address;
            req.paging_fence_value = result.paging_fence_value;
            if (mapped_pages != req.size_in_pages)
                req.size_in_pages = mapped_pages;
            if (either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0)
                ret = -EFAULT;
            else {
                map_ok = req.virtual_address != 0;
                hvdxg.mapgpuva_last_fence_current =
                    hvdxg_owner_sync_fence_value(owner, paging_sync);
                ret = copy_ret;
            }
        }
        hvdxg.mapgpuva_last_len = actual_len;
        hvdxg.mapgpuva_last_ret = ret;
        hvdxg.mapgpuva_last_va = req.virtual_address;
        hvdxg.mapgpuva_last_fence = req.paging_fence_value;
        hvdxg_note_mapgpuva_history(actual_len, ret,
                                    (uint32)hvdxg.mapgpuva_last_status,
                                    req.paging_queue.v, req.allocation.v,
                                    mapped_pages, req.protection.value,
                                    req.driver_protection,
                                    req.virtual_address,
                                    req.paging_fence_value);
        if (owner != NULL && req.allocation.v != 0)
            hvdxg_note_allocation_mapgpuva(
                owner, req.allocation.v, req.paging_queue.v,
                req.virtual_address, mapped_pages, req.paging_fence_value,
                ret, (uint32)hvdxg.mapgpuva_last_status);
        if (map_ok && owner != NULL)
            hvdxg_track_gpuva(owner, hvdxg.host_adapter_handle,
                              req.virtual_address, mapped_pages << 12,
                              req.paging_fence_value,
                              hvdxg_owner_pagingqueue_fence_pa(
                                  owner, req.paging_queue.v));
        break;
    }

    case LX_DXSUBMITCOMMAND: {
        struct d3dkmt_submitcommand req;
        struct hvdxg_command_submitcommand *submit = NULL;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 history_size;
        uint32 command_len;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.submit_last_command_buffer = req.command_buffer;
        hvdxg.submit_last_command_length = req.command_length;
        hvdxg.submit_last_flags = req.flags.value;
        hvdxg.submit_last_priv_size = req.priv_drv_data_size;
        hvdxg.submit_last_context_count = req.broadcast_context_count;
        hvdxg.submit_last_context0 =
            req.broadcast_context_count != 0 ? req.broadcast_context[0].v : 0;
        hvdxg.submit_last_status = 0;
        if (req.broadcast_context_count == 0 ||
            req.broadcast_context_count > D3DDDI_MAX_BROADCAST_CONTEXT ||
            req.num_primaries > D3DDDI_MAX_WRITTEN_PRIMARIES ||
            req.num_history_buffers > HV_DXG_HISTORY_BUFFER_MAX ||
            req.priv_drv_data_size > HV_DXG_VM_BUS_PACKET_MAX ||
            (req.num_history_buffers != 0 && req.history_buffer_array == 0) ||
            (req.priv_drv_data_size != 0 && req.priv_drv_data == 0)) {
            ret = -EINVAL;
            break;
        }
        for (uint32 i = 0; i < req.broadcast_context_count; i++) {
            if (!hvdxg_owner_has_context(owner, req.broadcast_context[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            break;
        history_size = req.num_history_buffers *
                       sizeof(struct hvdxg_d3dkmthandle);
        command_len = sizeof(*submit) + history_size +
                      req.priv_drv_data_size;
        if (command_len > HV_DXG_VM_BUS_PACKET_MAX) {
            ret = -EOVERFLOW;
            break;
        }
        submit = kvmalloc(command_len);
        if (submit == NULL) {
            ret = -ENOMEM;
            break;
        }
        memset(submit, 0, command_len);
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(&submit->hdr,
                                        HV_DXGK_VMBCOMMAND_SUBMITCOMMAND,
                                        hvdxg.dxg_process);
        submit->args = req;
        if (history_size != 0 &&
            either_copyin(&submit[1], 1, req.history_buffer_array,
                          history_size) < 0) {
            ret = -EFAULT;
            goto submitcommand_done;
        }
        if (req.priv_drv_data_size != 0 &&
            either_copyin((uint8 *)&submit[1] + history_size, 1,
                          req.priv_drv_data,
                          req.priv_drv_data_size) < 0) {
            ret = -EFAULT;
            goto submitcommand_done;
        }
        hvdxg_wc_store_fence();
        ret = hvdxg_send_sync_vgpu(submit, command_len, &status,
                                   sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.submit_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.submit_last_len = actual_len;
        hvdxg.submit_last_ret = ret;
submitcommand_done:
        if (submit != NULL)
            kvfree(submit);
        break;
    }

    case LX_DXSUBMITCOMMANDTOHWQUEUE: {
        struct d3dkmt_submitcommandtohwqueue req;
        struct hvdxg_command_submitcommandtohwqueue *submit = NULL;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 primaries_size;
        uint32 command_len;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.hwqueue.v == 0 ||
            req.num_primaries > D3DDDI_MAX_WRITTEN_PRIMARIES ||
            req.priv_drv_data_size > HV_DXG_VM_BUS_PACKET_MAX ||
            (req.num_primaries != 0 && req.written_primaries == 0) ||
            (req.priv_drv_data_size != 0 && req.priv_drv_data == 0)) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_hwqueue(owner, req.hwqueue.v)) {
            ret = -EPERM;
            break;
        }
        hvdxg.submithwqueue_last_queue = req.hwqueue.v;
        hvdxg.submithwqueue_last_fence_id =
            req.hwqueue_progress_fence_id;
        hvdxg.submithwqueue_last_command_length = req.command_length;
        hvdxg.submithwqueue_last_priv_size = req.priv_drv_data_size;
        hvdxg.submithwqueue_last_priv_head_len = 0;
        memset(hvdxg.submithwqueue_last_priv_head, 0,
               sizeof(hvdxg.submithwqueue_last_priv_head));
        primaries_size =
            req.num_primaries * sizeof(struct hvdxg_d3dkmthandle);
        command_len =
            sizeof(*submit) + primaries_size + req.priv_drv_data_size;
        if (command_len > HV_DXG_VM_BUS_PACKET_MAX) {
            ret = -EOVERFLOW;
            break;
        }
        submit = kvmalloc(command_len);
        if (submit == NULL) {
            ret = -ENOMEM;
            break;
        }
        memset(submit, 0, command_len);
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(
            &submit->hdr, HV_DXGK_VMBCOMMAND_SUBMITCOMMANDTOHWQUEUE,
            hvdxg.dxg_process);
        submit->args = req;
        if (primaries_size != 0 &&
            either_copyin(&submit[1], 1, req.written_primaries,
                          primaries_size) < 0) {
            ret = -EFAULT;
            goto submithwqueue_done;
        }
        if (req.priv_drv_data_size != 0 &&
            either_copyin((uint8 *)&submit[1] + primaries_size, 1,
                          req.priv_drv_data,
                          req.priv_drv_data_size) < 0) {
            ret = -EFAULT;
            goto submithwqueue_done;
        }
        if (req.priv_drv_data_size != 0)
            hvdxg_save_priv_head(hvdxg.submithwqueue_last_priv_head,
                                 sizeof(hvdxg.submithwqueue_last_priv_head),
                                 &hvdxg.submithwqueue_last_priv_head_len,
                                 (uint8 *)&submit[1] + primaries_size,
                                 req.priv_drv_data_size);
        hvdxg_wc_store_fence();
        ret = hvdxg_send_sync_vgpu(submit, command_len, &status,
                                   sizeof(status), &actual_len);
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.submithwqueue_last_len = actual_len;
        hvdxg.submithwqueue_last_ret = ret;
submithwqueue_done:
        hvdxg_note_submithwqueue_history(
            hvdxg.submithwqueue_last_len,
            hvdxg.submithwqueue_last_ret,
            hvdxg.submithwqueue_last_queue,
            hvdxg.submithwqueue_last_command_length,
            hvdxg.submithwqueue_last_priv_size,
            hvdxg.submithwqueue_last_priv_head,
            hvdxg.submithwqueue_last_priv_head_len);
        if (submit != NULL)
            kvfree(submit);
        break;
    }

    case LX_DXRESERVEGPUVIRTUALADDRESS: {
        struct d3dddi_reservegpuvirtualaddress req;
        struct d3dddi_reservegpuvirtualaddress wire_req;
        struct hvdxg_command_reservegpuvirtualaddress reserve;
        struct hvdxg_command_reservegpuvirtualaddress_return result;
        uint32 actual_len = 0;
        uint32 host_adapter = 0;
        int adapter_handle;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        adapter_handle =
            hvdxg_resolve_adapter_handle(owner, req.adapter.v,
                                         &host_adapter) == 0;
        if ((!adapter_handle &&
             !hvdxg_owner_has_pagingqueue(owner, req.adapter.v)) ||
            req.size == 0) {
            ret = -EINVAL;
            break;
        }
        wire_req = req;
        if (adapter_handle)
            wire_req.adapter.v = host_adapter;
        memset(&reserve, 0, sizeof(reserve));
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(
            &reserve.hdr, HV_DXGK_VMBCOMMAND_RESERVEGPUVIRTUALADDRESS,
            hvdxg.dxg_process);
        reserve.args = wire_req;
        ret = hvdxg_send_sync_vgpu(&reserve, sizeof(reserve), &result,
                                   sizeof(result), &actual_len);
        hvdxg.gpuva_reserve_last_len = actual_len;
        hvdxg.gpuva_reserve_last_ret = ret;
        hvdxg.gpuva_reserve_last_va = result.virtual_address;
        hvdxg.gpuva_reserve_last_fence = result.paging_fence_value;
        if (ret != 0)
            break;
        if (actual_len < sizeof(result) || result.virtual_address == 0) {
            ret = -EIO;
            break;
        }
        req.virtual_address = result.virtual_address;
        req.paging_fence_value = result.paging_fence_value;
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        if (ret == 0 && owner != NULL)
            hvdxg_track_gpuva(owner, req.adapter.v, req.virtual_address,
                              req.size, req.paging_fence_value, 0);
        break;
    }

    case LX_DXFREEGPUVIRTUALADDRESS: {
        struct d3dkmt_freegpuvirtualaddress req;
        struct d3dkmt_freegpuvirtualaddress wire_req;
        struct hvdxg_command_freegpuvirtualaddress free_gpuva;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 host_adapter = 0;
        int adapter_handle;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        adapter_handle =
            hvdxg_resolve_adapter_handle(owner, req.adapter.v,
                                         &host_adapter) == 0;
        if ((!adapter_handle &&
             !hvdxg_owner_has_gpuva(owner, req.adapter.v,
                                     req.base_address)) ||
            req.base_address == 0 || req.size == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_gpuva(owner, req.adapter.v, req.base_address) &&
            !hvdxg_owner_has_gpuva(owner, 0, req.base_address)) {
            ret = -EPERM;
            break;
        }
        memset(&free_gpuva, 0, sizeof(free_gpuva));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(
            &free_gpuva.hdr, HV_DXGK_VMBCOMMAND_FREEGPUVIRTUALADDRESS,
            hvdxg.dxg_process);
        wire_req = req;
        if (adapter_handle)
            wire_req.adapter.v = host_adapter;
        free_gpuva.args = wire_req;
        hvdxg.gpuva_free_last_adapter = req.adapter.v;
        hvdxg.gpuva_free_last_base = req.base_address;
        hvdxg.gpuva_free_last_size = req.size;
        hvdxg.gpuva_free_last_wire_size = free_gpuva.args.size;
        ret = hvdxg_send_sync_vgpu(&free_gpuva, sizeof(free_gpuva), &status,
                                   sizeof(status), &actual_len);
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.gpuva_free_last_len = actual_len;
        hvdxg.gpuva_free_last_ret = ret;
        if (ret == 0 && owner != NULL)
            hvdxg_untrack_gpuva(owner, req.base_address);
        break;
    }

    case LX_DXFLUSHHEAPTRANSITIONS: {
        struct d3dkmt_flushheaptransitions req;
        uint32 host_adapter;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (hvdxg_resolve_adapter_handle(owner, req.adapter.v,
                                         &host_adapter) != 0) {
            ret = -EINVAL;
            break;
        }
        ret = hvdxg_flush_heap_transitions_process(
            hvdxg_owner_bound_process_handle(owner).v);
        break;
    }

    case LX_DXINVALIDATECACHE: {
        struct d3dkmt_invalidatecache req;
        struct hvdxg_command_invalidatecache invalidate;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0 || req.allocation.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v) ||
            !hvdxg_owner_has_allocation(owner, req.device.v, 0,
                                        req.allocation.v)) {
            ret = -EPERM;
            break;
        }
        memset(&invalidate, 0, sizeof(invalidate));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(
            &invalidate.hdr, HV_DXGK_VMBCOMMAND_INVALIDATECACHE,
            hvdxg.dxg_process);
        invalidate.device.v = req.device.v;
        invalidate.allocation.v = req.allocation.v;
        invalidate.offset = req.offset;
        invalidate.length = req.length;
        ret = hvdxg_send_sync_vgpu(&invalidate, sizeof(invalidate), &status,
                                   sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.cacheops_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.cacheops_last_len = actual_len;
        hvdxg.cacheops_last_ret = ret;
        hvdxg.cacheops_last_allocation = req.allocation.v;
        break;
    }

    case LX_DXSETCONTEXTSCHEDULINGPRIORITY:
    case LX_DXSETCONTEXTINPROCESSSCHEDULINGPRIORITY: {
        struct d3dkmt_setcontextschedulingpriority req;
        struct hvdxg_command_setcontextschedulingpriority set;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.context.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_context(owner, req.context.v)) {
            ret = -EPERM;
            break;
        }
        memset(&set, 0, sizeof(set));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(
            &set.hdr, HV_DXGK_VMBCOMMAND_SETCONTEXTSCHEDULINGPRIORITY,
            hvdxg.dxg_process);
        set.context.v = req.context.v;
        set.priority = req.priority;
        set.in_process =
            ((uint32)cmd == LX_DXSETCONTEXTINPROCESSSCHEDULINGPRIORITY);
        ret = hvdxg_send_sync_vgpu(&set, sizeof(set), &status,
                                   sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.schedprio_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.schedprio_last_len = actual_len;
        hvdxg.schedprio_last_ret = ret;
        hvdxg.schedprio_last_context = req.context.v;
        hvdxg.schedprio_last_value = req.priority;
        break;
    }

    case LX_DXGETCONTEXTSCHEDULINGPRIORITY:
    case LX_DXGETCONTEXTINPROCESSSCHEDULINGPRIORITY: {
        struct d3dkmt_getcontextschedulingpriority req;
        struct hvdxg_command_getcontextschedulingpriority get;
        struct hvdxg_command_getcontextschedulingpriority_return result;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.context.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_context(owner, req.context.v)) {
            ret = -EPERM;
            break;
        }
        memset(&get, 0, sizeof(get));
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(
            &get.hdr, HV_DXGK_VMBCOMMAND_GETCONTEXTSCHEDULINGPRIORITY,
            hvdxg.dxg_process);
        get.context.v = req.context.v;
        get.in_process =
            ((uint32)cmd == LX_DXGETCONTEXTINPROCESSSCHEDULINGPRIORITY);
        ret = hvdxg_send_sync_vgpu(&get, sizeof(get), &result,
                                   sizeof(result), &actual_len);
        if (actual_len >= sizeof(result.status))
            hvdxg.schedprio_last_status = result.status.v;
        if (ret == 0 && actual_len >= sizeof(result.status))
            ret = hvdxg_ntstatus_to_errno(result.status);
        if (ret == 0 && actual_len >= sizeof(result)) {
            req.priority = result.priority;
            ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
                  -EFAULT : 0;
        }
        hvdxg.schedprio_last_len = actual_len;
        hvdxg.schedprio_last_ret = ret;
        hvdxg.schedprio_last_context = req.context.v;
        hvdxg.schedprio_last_value = req.priority;
        break;
    }

    case LX_DXSETALLOCATIONPRIORITY: {
        struct d3dkmt_setallocationpriority req;
        uint8 command_buf[sizeof(struct hvdxg_command_setallocationpriority) -
                          1 + HV_DXG_ALLOCATION_MAX *
                                  sizeof(struct hvdxg_d3dkmthandle) +
                          HV_DXG_ALLOCATION_MAX * sizeof(uint32)];
        struct hvdxg_command_setallocationpriority *set =
            (struct hvdxg_command_setallocationpriority *)command_buf;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 alloc_size = 0;
        uint32 priority_count;
        uint32 priority_size;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0 || req.priorities == 0 ||
            req.allocation_count > HV_DXG_ALLOCATION_MAX) {
            ret = -EINVAL;
            break;
        }
        if (req.resource.v != 0) {
            if (req.allocation_count != 0) {
                ret = -EINVAL;
                break;
            }
            priority_count = 1;
            if (!hvdxg_owner_has_allocation(owner, req.device.v,
                                            req.resource.v, 0)) {
                ret = -EPERM;
                break;
            }
        } else {
            if (req.allocation_count == 0 || req.allocation_list == 0) {
                ret = -EINVAL;
                break;
            }
            priority_count = req.allocation_count;
            alloc_size = req.allocation_count *
                         sizeof(struct hvdxg_d3dkmthandle);
        }
        priority_size = priority_count * sizeof(uint32);
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(
            &set->hdr, HV_DXGK_VMBCOMMAND_SETALLOCATIONPRIORITY,
            hvdxg.dxg_process);
        set->device.v = req.device.v;
        set->resource.v = req.resource.v;
        set->allocation_count = req.allocation_count;
        if (alloc_size != 0 &&
            either_copyin(set->data, 1, req.allocation_list,
                          alloc_size) < 0) {
            ret = -EFAULT;
            break;
        }
        if (alloc_size != 0) {
            struct hvdxg_d3dkmthandle *allocations =
                (struct hvdxg_d3dkmthandle *)set->data;
            if (!hvdxg_owner_has_device(owner, req.device.v)) {
                ret = -EPERM;
                break;
            }
            for (uint32 i = 0; i < req.allocation_count; i++) {
                if (!hvdxg_owner_has_allocation(owner, req.device.v, 0,
                                                allocations[i].v)) {
                    ret = -EPERM;
                    break;
                }
            }
            if (ret != 0)
                break;
        }
        if (either_copyin(set->data + alloc_size, 1, req.priorities,
                          priority_size) < 0) {
            ret = -EFAULT;
            break;
        }
        ret = hvdxg_send_sync_vgpu(set,
                                   sizeof(*set) - 1 + alloc_size +
                                       priority_size,
                                   &status, sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.allocprio_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.allocprio_last_len = actual_len;
        hvdxg.allocprio_last_ret = ret;
        hvdxg.allocprio_last_count = req.allocation_count;
        break;
    }

    case LX_DXGETALLOCATIONPRIORITY: {
        struct d3dkmt_getallocationpriority req;
        uint8 command_buf[sizeof(struct hvdxg_command_getallocationpriority) +
                          (HV_DXG_ALLOCATION_MAX - 1) *
                              sizeof(struct hvdxg_d3dkmthandle)];
        uint8 result_buf[sizeof(struct hvdxg_command_getallocationpriority_return) +
                         (HV_DXG_ALLOCATION_MAX - 1) * sizeof(uint32)];
        struct hvdxg_command_getallocationpriority *get =
            (struct hvdxg_command_getallocationpriority *)command_buf;
        struct hvdxg_command_getallocationpriority_return *result =
            (struct hvdxg_command_getallocationpriority_return *)result_buf;
        uint32 actual_len = 0;
        uint32 alloc_size = 0;
        uint32 priority_count;
        uint32 priority_size;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0 || req.priorities == 0 ||
            req.allocation_count > HV_DXG_ALLOCATION_MAX) {
            ret = -EINVAL;
            break;
        }
        if (req.resource.v != 0) {
            if (req.allocation_count != 0) {
                ret = -EINVAL;
                break;
            }
            priority_count = 1;
            if (!hvdxg_owner_has_allocation(owner, req.device.v,
                                            req.resource.v, 0)) {
                ret = -EPERM;
                break;
            }
        } else {
            if (req.allocation_count == 0 || req.allocation_list == 0) {
                ret = -EINVAL;
                break;
            }
            priority_count = req.allocation_count;
            alloc_size = req.allocation_count *
                         sizeof(struct hvdxg_d3dkmthandle);
        }
        priority_size = priority_count * sizeof(uint32);
        memset(command_buf, 0, sizeof(command_buf));
        memset(result_buf, 0, sizeof(result_buf));
        hvdxg_command_vgpu_init_process(
            &get->hdr, HV_DXGK_VMBCOMMAND_GETALLOCATIONPRIORITY,
            hvdxg.dxg_process);
        get->device.v = req.device.v;
        get->resource.v = req.resource.v;
        get->allocation_count = req.allocation_count;
        if (alloc_size != 0 &&
            either_copyin(get->allocations, 1, req.allocation_list,
                          alloc_size) < 0) {
            ret = -EFAULT;
            break;
        }
        if (alloc_size != 0) {
            if (!hvdxg_owner_has_device(owner, req.device.v)) {
                ret = -EPERM;
                break;
            }
            for (uint32 i = 0; i < req.allocation_count; i++) {
                if (!hvdxg_owner_has_allocation(owner, req.device.v, 0,
                                                get->allocations[i].v)) {
                    ret = -EPERM;
                    break;
                }
            }
            if (ret != 0)
                break;
        }
        ret = hvdxg_send_sync_vgpu(get, sizeof(*get) - sizeof(get->allocations) +
                                        alloc_size,
                                   result, sizeof(result_buf), &actual_len);
        if (actual_len >= sizeof(result->status))
            hvdxg.allocprio_last_status = result->status.v;
        if (ret == 0 && actual_len >= sizeof(result->status))
            ret = hvdxg_ntstatus_to_errno(result->status);
        if (ret == 0 && actual_len >= sizeof(result->status) + priority_size) {
            ret = either_copyout(1, req.priorities, result->priorities,
                                 priority_size) < 0 ? -EFAULT : 0;
        }
        hvdxg.allocprio_last_len = actual_len;
        hvdxg.allocprio_last_ret = ret;
        hvdxg.allocprio_last_count = req.allocation_count;
        break;
    }

    case LX_DXQUERYALLOCATIONRESIDENCY: {
        struct d3dkmt_queryallocationresidency req;
        uint8 command_buf[sizeof(struct hvdxg_command_queryallocationresidency) +
                          (HV_DXG_ALLOCATION_MAX - 1) *
                              sizeof(struct hvdxg_d3dkmthandle)];
        uint8 result_buf[sizeof(struct hvdxg_command_queryallocationresidency_return) +
                         (HV_DXG_ALLOCATION_MAX - 1) *
                             sizeof(enum d3dkmt_allocationresidencystatus)];
        struct hvdxg_command_queryallocationresidency *query =
            (struct hvdxg_command_queryallocationresidency *)command_buf;
        struct hvdxg_command_queryallocationresidency_return *result =
            (struct hvdxg_command_queryallocationresidency_return *)result_buf;
        uint32 actual_len = 0;
        uint32 alloc_count;
        uint32 alloc_size = 0;
        uint32 result_size;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0 || req.residency_status == 0 ||
            req.allocation_count > HV_DXG_ALLOCATION_MAX) {
            ret = -EINVAL;
            break;
        }
        if (req.allocation_count != 0) {
            if (req.allocations == 0) {
                ret = -EINVAL;
                break;
            }
            alloc_count = req.allocation_count;
            alloc_size = alloc_count * sizeof(struct hvdxg_d3dkmthandle);
        } else {
            if (req.resource.v == 0) {
                ret = -EINVAL;
                break;
            }
            alloc_count = 1;
            if (!hvdxg_owner_has_allocation(owner, req.device.v,
                                            req.resource.v, 0)) {
                ret = -EPERM;
                break;
            }
        }
        result_size = sizeof(result->status) +
                      alloc_count *
                          sizeof(enum d3dkmt_allocationresidencystatus);
        memset(command_buf, 0, sizeof(command_buf));
        memset(result_buf, 0, sizeof(result_buf));
        hvdxg_command_vgpu_init_process(
            &query->hdr, HV_DXGK_VMBCOMMAND_QUERYALLOCATIONRESIDENCY,
            hvdxg.dxg_process);
        query->args = req;
        if (alloc_size != 0 &&
            either_copyin(query->allocations, 1, req.allocations,
                          alloc_size) < 0) {
            ret = -EFAULT;
            break;
        }
        if (alloc_size != 0) {
            if (!hvdxg_owner_has_device(owner, req.device.v)) {
                ret = -EPERM;
                break;
            }
            for (uint32 i = 0; i < req.allocation_count; i++) {
                if (!hvdxg_owner_has_allocation(owner, req.device.v, 0,
                                                query->allocations[i].v)) {
                    ret = -EPERM;
                    break;
                }
            }
            if (ret != 0)
                break;
        }
        ret = hvdxg_send_sync_vgpu(query,
                                   sizeof(*query) -
                                       sizeof(query->allocations) +
                                       alloc_size,
                                   result, result_size, &actual_len);
        if (actual_len >= sizeof(result->status))
            hvdxg.allocres_last_status = result->status.v;
        if (ret == 0 && actual_len >= sizeof(result->status))
            ret = hvdxg_ntstatus_to_errno(result->status);
        if (ret == 0 && actual_len >= result_size) {
            ret = either_copyout(1, req.residency_status,
                                 result->residency_status,
                                 result_size - sizeof(result->status)) < 0 ?
                  -EFAULT : 0;
            hvdxg.allocres_last_value = result->residency_status[0];
        }
        hvdxg.allocres_last_len = actual_len;
        hvdxg.allocres_last_ret = ret;
        hvdxg.allocres_last_count = alloc_count;
        break;
    }

    case LX_DXOFFERALLOCATIONS: {
        struct d3dkmt_offerallocations req;
        uint8 command_buf[sizeof(struct hvdxg_command_offerallocations) +
                          (HV_DXG_ALLOCATION_MAX - 1) *
                              sizeof(struct hvdxg_d3dkmthandle)];
        struct hvdxg_command_offerallocations *offer =
            (struct hvdxg_command_offerallocations *)command_buf;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 alloc_size;
        uint64 list_ptr;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.offer_last_status = 0;
        hvdxg.offer_last_count = req.allocation_count;
        if (req.device.v == 0 || req.allocation_count == 0 ||
            req.allocation_count > HV_DXG_ALLOCATION_MAX ||
            ((req.resources == 0) == (req.allocations == 0))) {
            ret = -EINVAL;
            hvdxg.offer_last_ret = ret;
            break;
        }
        alloc_size = req.allocation_count *
                     sizeof(struct hvdxg_d3dkmthandle);
        list_ptr = req.resources != 0 ? req.resources : req.allocations;
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(
            &offer->hdr, HV_DXGK_VMBCOMMAND_OFFERALLOCATIONS,
            hvdxg.dxg_process);
        offer->device.v = req.device.v;
        offer->allocation_count = req.allocation_count;
        offer->priority = req.priority;
        offer->flags = req.flags;
        offer->resources = req.resources != 0 ? 1 : 0;
        if (either_copyin(offer->allocations, 1, list_ptr, alloc_size) < 0) {
            ret = -EFAULT;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EPERM;
            break;
        }
        for (uint32 i = 0; i < req.allocation_count; i++) {
            if (!hvdxg_owner_has_allocation(owner, req.device.v,
                                            offer->resources ?
                                            offer->allocations[i].v : 0,
                                            offer->resources ? 0 :
                                            offer->allocations[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            break;
        ret = hvdxg_send_sync_vgpu(offer,
                                   sizeof(*offer) -
                                       sizeof(offer->allocations) +
                                       alloc_size,
                                   &status, sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.offer_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.offer_last_len = actual_len;
        hvdxg.offer_last_ret = ret;
        break;
    }

    case LX_DXRECLAIMALLOCATIONS2: {
        struct d3dkmt_reclaimallocations2 req;
        uint8 command_buf[sizeof(struct hvdxg_command_reclaimallocations) +
                          (HV_DXG_ALLOCATION_MAX - 1) *
                              sizeof(struct hvdxg_d3dkmthandle)];
        uint8 result_buf[sizeof(struct hvdxg_command_reclaimallocations_return) +
                         (HV_DXG_ALLOCATION_MAX - 1) *
                             sizeof(enum d3dddi_reclaim_result)];
        struct hvdxg_command_reclaimallocations *reclaim =
            (struct hvdxg_command_reclaimallocations *)command_buf;
        struct hvdxg_command_reclaimallocations_return *result =
            (struct hvdxg_command_reclaimallocations_return *)result_buf;
        uint32 actual_len = 0;
        uint32 alloc_size;
        uint32 result_size;
        uint64 list_ptr;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.reclaim_last_status = 0;
        hvdxg.reclaim_last_count = req.allocation_count;
        hvdxg.reclaim_last_result0 = 0;
        hvdxg.reclaim_last_fence = 0;
        if (req.paging_queue.v == 0 || req.allocation_count == 0 ||
            req.allocation_count > HV_DXG_ALLOCATION_MAX ||
            ((req.resources == 0) == (req.allocations == 0))) {
            ret = -EINVAL;
            hvdxg.reclaim_last_ret = ret;
            break;
        }
        alloc_size = req.allocation_count *
                     sizeof(struct hvdxg_d3dkmthandle);
        result_size = sizeof(*result);
        if (req.results != 0)
            result_size += (req.allocation_count - 1) *
                           sizeof(enum d3dddi_reclaim_result);
        list_ptr = req.resources != 0 ? req.resources : req.allocations;
        memset(command_buf, 0, sizeof(command_buf));
        memset(result_buf, 0, sizeof(result_buf));
        hvdxg_command_vgpu_init_process(
            &reclaim->hdr, HV_DXGK_VMBCOMMAND_RECLAIMALLOCATIONS,
            hvdxg.dxg_process);
        reclaim->paging_queue.v = req.paging_queue.v;
        reclaim->allocation_count = req.allocation_count;
        reclaim->resources = req.resources != 0 ? 1 : 0;
        reclaim->write_results = req.results != 0 ? 1 : 0;
        if (either_copyin(reclaim->allocations, 1, list_ptr, alloc_size) < 0) {
            ret = -EFAULT;
            break;
        }
        if (!hvdxg_owner_has_pagingqueue(owner, req.paging_queue.v)) {
            ret = -EPERM;
            break;
        }
        for (uint32 i = 0; i < req.allocation_count; i++) {
            if (!hvdxg_owner_has_allocation(owner, 0,
                                            reclaim->resources ?
                                            reclaim->allocations[i].v : 0,
                                            reclaim->resources ? 0 :
                                            reclaim->allocations[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            break;
        reclaim->device.v = reclaim->resources ?
                            hvdxg.last_device_handle :
                            hvdxg_device_for_allocation(
                                reclaim->allocations[0].v);
        if (reclaim->device.v == 0) {
            ret = -EINVAL;
            hvdxg.reclaim_last_ret = ret;
            break;
        }
        ret = hvdxg_send_sync_vgpu(reclaim,
                                   sizeof(*reclaim) -
                                       sizeof(reclaim->allocations) +
                                       alloc_size,
                                   result, result_size, &actual_len);
        if (actual_len >= sizeof(result->paging_fence_value) +
                          sizeof(result->status)) {
            hvdxg.reclaim_last_fence = result->paging_fence_value;
            hvdxg.reclaim_last_status = result->status.v;
        }
        if (ret == 0 &&
            actual_len >= sizeof(result->paging_fence_value) +
                          sizeof(result->status))
            ret = hvdxg_ntstatus_to_errno(result->status);
        if (ret == 0) {
            req.paging_fence_value = result->paging_fence_value;
            if (either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0) {
                ret = -EFAULT;
                break;
            }
            if (req.results != 0 && actual_len >= result_size) {
                hvdxg.reclaim_last_result0 = result->results[0];
                ret = either_copyout(1, req.results, result->results,
                                     req.allocation_count *
                                         sizeof(enum d3dddi_reclaim_result)) < 0 ?
                      -EFAULT : 0;
            }
        }
        hvdxg.reclaim_last_len = actual_len;
        hvdxg.reclaim_last_ret = ret;
        break;
    }

    case LX_DXUPDATEGPUVIRTUALADDRESS: {
        struct d3dkmt_updategpuvirtualaddress req;
        struct hvdxg_command_updategpuvirtualaddress *update = NULL;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 op_size;
        uint32 command_len;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0 || req.num_operations == 0 ||
            req.operations == 0 ||
            req.num_operations >
                HV_DXG_VM_BUS_PACKET_MAX /
                    sizeof(struct d3dddi_updategpuvirtualaddress_operation)) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v) ||
            (req.context.v != 0 &&
             !hvdxg_owner_has_context(owner, req.context.v)) ||
            (req.fence_object.v != 0 &&
             !hvdxg_owner_has_sync(owner, req.fence_object.v))) {
            ret = -EPERM;
            break;
        }
        op_size = req.num_operations *
                  sizeof(struct d3dddi_updategpuvirtualaddress_operation);
        command_len = sizeof(*update) - sizeof(update->operations) + op_size;
        if (command_len > HV_DXG_VM_BUS_PACKET_MAX) {
            ret = -EINVAL;
            break;
        }
        update = kvmalloc(command_len);
        if (update == NULL) {
            ret = -ENOMEM;
            break;
        }
        memset(update, 0, command_len);
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(
            &update->hdr, HV_DXGK_VMBCOMMAND_UPDATEGPUVIRTUALADDRESS,
            hvdxg.dxg_process);
        update->fence_value = req.fence_value;
        update->device.v = req.device.v;
        update->context.v = req.context.v;
        update->fence_object.v = req.fence_object.v;
        update->num_operations = req.num_operations;
        update->flags = req.flags.value;
        hvdxg.updategpuva_last_len = 0;
        hvdxg.updategpuva_last_ret = 0;
        hvdxg.updategpuva_last_status = 0;
        hvdxg.updategpuva_last_ops = req.num_operations;
        hvdxg.updategpuva_last_fence = req.fence_value;
        hvdxg.updategpuva_last_device = req.device.v;
        hvdxg.updategpuva_last_context = req.context.v;
        hvdxg.updategpuva_last_fence_object = req.fence_object.v;
        hvdxg.updategpuva_last_flags = req.flags.value;
        hvdxg.updategpuva_last_cmd_len = command_len;
        hvdxg.updategpuva_last_op_offset =
            offsetof(struct hvdxg_command_updategpuvirtualaddress,
                     operations);
        hvdxg.updategpuva_last_op_size =
            sizeof(struct d3dddi_updategpuvirtualaddress_operation);
        hvdxg.updategpuva_last_op0_type = 0;
        hvdxg.updategpuva_last_op0_base = 0;
        hvdxg.updategpuva_last_op0_size = 0;
        hvdxg.updategpuva_last_op0_allocation = 0;
        hvdxg.updategpuva_last_op0_alloc_offset = 0;
        hvdxg.updategpuva_last_op0_alloc_size = 0;
        hvdxg.updategpuva_last_op0_source = 0;
        hvdxg.updategpuva_last_op0_dest = 0;
        hvdxg.updategpuva_last_op0_protection = 0;
        hvdxg.updategpuva_last_op0_driver_protection = 0;
        if (either_copyin(update->operations, 1, req.operations,
                          op_size) < 0) {
            ret = -EFAULT;
            kvfree(update);
            update = NULL;
            break;
        }
        hvdxg.updategpuva_last_op0_type = update->operations[0].operation;
        switch (update->operations[0].operation) {
        case _D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP:
            hvdxg.updategpuva_last_op0_base =
                update->operations[0].map.base_address;
            hvdxg.updategpuva_last_op0_size =
                update->operations[0].map.size;
            hvdxg.updategpuva_last_op0_allocation =
                update->operations[0].map.allocation.v;
            hvdxg.updategpuva_last_op0_alloc_offset =
                update->operations[0].map.allocation_offset;
            hvdxg.updategpuva_last_op0_alloc_size =
                update->operations[0].map.allocation_size;
            break;
        case _D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP_PROTECT:
            hvdxg.updategpuva_last_op0_base =
                update->operations[0].map_protect.base_address;
            hvdxg.updategpuva_last_op0_size =
                update->operations[0].map_protect.size;
            hvdxg.updategpuva_last_op0_allocation =
                update->operations[0].map_protect.allocation.v;
            hvdxg.updategpuva_last_op0_alloc_offset =
                update->operations[0].map_protect.allocation_offset;
            hvdxg.updategpuva_last_op0_alloc_size =
                update->operations[0].map_protect.allocation_size;
            hvdxg.updategpuva_last_op0_protection =
                update->operations[0].map_protect.protection.value;
            hvdxg.updategpuva_last_op0_driver_protection =
                update->operations[0].map_protect.driver_protection;
            break;
        case _D3DDDI_UPDATEGPUVIRTUALADDRESS_UNMAP:
            hvdxg.updategpuva_last_op0_base =
                update->operations[0].unmap.base_address;
            hvdxg.updategpuva_last_op0_size =
                update->operations[0].unmap.size;
            hvdxg.updategpuva_last_op0_protection =
                update->operations[0].unmap.protection.value;
            break;
        case _D3DDDI_UPDATEGPUVIRTUALADDRESS_COPY:
            hvdxg.updategpuva_last_op0_source =
                update->operations[0].copy.source_address;
            hvdxg.updategpuva_last_op0_size =
                update->operations[0].copy.size;
            hvdxg.updategpuva_last_op0_dest =
                update->operations[0].copy.dest_address;
            break;
        default:
            break;
        }
        ret = hvdxg_send_sync_vgpu(update, command_len, &status,
                                   sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.updategpuva_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.updategpuva_last_len = actual_len;
        hvdxg.updategpuva_last_ret = ret;
        hvdxg.updategpuva_last_ops = req.num_operations;
        hvdxg.updategpuva_last_fence = req.fence_value;
        if (update != NULL)
            kvfree(update);
        break;
    }

    case LX_DXLOCK2: {
        struct d3dkmt_lock2 req;
        struct hvdxg_command_lock2 lock;
        struct hvdxg_command_lock2_return result;
        struct hvdxg_tracked_allocation *tracked;
        uint32 actual_len = 0;
        uint64 map_size = 0;
        uint64 mapped_size;
        uint64 map_pa;
        uint64 canonical_pa = 0;
        uint64 map_extra_flags;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.lock2_ioctl_count++;
        hvdxg.lock2_last_allocation = req.allocation.v;
        hvdxg.lock2_last_status = 0;
        hvdxg.lock2_last_offset = 0;
        hvdxg.lock2_last_user_va = 0;
        if (req.device.v == 0 || req.allocation.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v) ||
            !hvdxg_owner_has_allocation(owner, req.device.v, 0,
                                        req.allocation.v)) {
            ret = -EPERM;
            break;
        }
        tracked = hvdxg_owner_find_allocation(owner, req.device.v, 0,
                                              req.allocation.v);
        if (tracked != NULL && tracked->cpu_va != 0 &&
            tracked->cpu_vm != NULL &&
            (current == NULL || current->vm == NULL ||
             tracked->cpu_vm != current->vm)) {
            ret = -EBUSY;
            break;
        }
        if (tracked != NULL && tracked->cpu_va != 0) {
            req.data = tracked->cpu_va;
            tracked->lock_refcount++;
            ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
                  -EFAULT : 0;
            if (ret != 0 && tracked->lock_refcount != 0)
                tracked->lock_refcount--;
            hvdxg.lock2_last_len = 0;
            hvdxg.lock2_last_ret = ret;
            hvdxg.lock2_last_user_va = req.data;
            if (tracked->map_size == 0)
                hvdxg.lock2_sysmem_count++;
            else
                hvdxg.lock2_cached_ref_count++;
            hvdxg_note_lock2_history(0, ret, 0, req.device.v,
                                     req.allocation.v, 0, req.data,
                                     tracked->map_size);
            if (hvdxg.lock2_diag_prints < 32) {
                printf("hyperv-dxg: lock2 cached device=0x%x "
                       "allocation=0x%x sysmem=%u user=0x%lx "
                       "map_size=%lu ref=%u ret=%d\n",
                       req.device.v, req.allocation.v,
                       tracked->map_size == 0 ? 1 : 0, req.data,
                       tracked->map_size, tracked->lock_refcount, ret);
                hvdxg.lock2_diag_prints++;
            }
            break;
        }
        memset(&lock, 0, sizeof(lock));
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(&lock.hdr,
                                        HV_DXGK_VMBCOMMAND_LOCK2,
                                        hvdxg.dxg_process);
        lock.args = req;
        lock.args.data = 0;
        ret = hvdxg_send_sync_vgpu(&lock, sizeof(lock), &result,
                                   sizeof(result), &actual_len);
        hvdxg.lock2_host_forward_count++;
        hvdxg.lock2_last_len = actual_len;
        hvdxg.lock2_last_ret = ret;
        if (actual_len >= sizeof(result.status))
            hvdxg.lock2_last_status = result.status.v;
        if (ret == 0 && actual_len >= sizeof(result.status))
            ret = hvdxg_ntstatus_to_errno(result.status);
        if (ret != 0) {
            hvdxg_note_lock2_history(actual_len, ret,
                                     (uint32)hvdxg.lock2_last_status,
                                     req.device.v, req.allocation.v, 0, 0, 0);
            if (hvdxg.lock2_diag_prints < 32) {
                printf("hyperv-dxg: lock2 host_fail device=0x%x "
                       "allocation=0x%x flags=0x%x status=0x%x "
                       "ret=%d len=%u\n",
                       req.device.v, req.allocation.v, req.flags.value,
                       (uint32)hvdxg.lock2_last_status, ret, actual_len);
                hvdxg.lock2_diag_prints++;
            }
            break;
        }
        if (actual_len < sizeof(result)) {
            ret = -EOVERFLOW;
            hvdxg.lock2_last_ret = ret;
            hvdxg_note_lock2_history(actual_len, ret,
                                     (uint32)hvdxg.lock2_last_status,
                                     req.device.v, req.allocation.v, 0, 0, 0);
            if (hvdxg.lock2_diag_prints < 32) {
                printf("hyperv-dxg: lock2 short_return device=0x%x "
                       "allocation=0x%x status=0x%x len=%u need=%lu\n",
                       req.device.v, req.allocation.v,
                       (uint32)hvdxg.lock2_last_status, actual_len,
                       (uint64)sizeof(result));
                hvdxg.lock2_diag_prints++;
            }
            break;
        }
        hvdxg.lock2_last_offset = result.cpu_visible_buffer_offset;
        map_size = hvdxg_owner_allocation_size(owner, req.device.v, 0,
                                               req.allocation.v);
        if (map_size == 0 && hvdxg.last_allocation_handle == req.allocation.v)
            map_size = hvdxg.last_allocation_size;
        if (map_size == 0)
            map_size = PGSIZE;
        map_pa = result.cpu_visible_buffer_offset;
        if (tracked != NULL && tracked->cpu_va != 0) {
            req.data = tracked->cpu_va;
            tracked->lock_refcount++;
        } else {
            map_extra_flags =
                hv_cmdline_enabled("dxg_lock_cached") ||
                (tracked != NULL &&
                 (tracked->flags & HV_DXG_ALLOCATION_FLAG_CACHED)) ?
                0 : HV_DXG_PTE_WRITE_THROUGH;
            req.data = hvdxg_map_iospace_user_canonical(
                HV_DXG_FENCE_SOURCE_LOCK2, map_pa, map_size,
                map_extra_flags, &canonical_pa, NULL, 1);
            map_pa = canonical_pa;
            if (tracked != NULL && req.data != 0) {
                mapped_size = PGROUNDUP((map_pa & (PGSIZE - 1)) +
                                        map_size);
                tracked->cpu_va = req.data;
                tracked->map_size = mapped_size;
                tracked->cpu_vm = current ? current->vm : NULL;
                tracked->lock_refcount = 1;
            }
        }
        hvdxg.lock2_last_user_va = req.data;
        if (req.data == 0) {
            ret = -ENOMEM;
            hvdxg.lock2_last_ret = ret;
            hvdxg.lock2_map_fail_count++;
            hvdxg_note_lock2_history(actual_len, ret,
                                     (uint32)hvdxg.lock2_last_status,
                                     req.device.v, req.allocation.v,
                                     hvdxg.lock2_last_offset, 0, map_size);
            if (hvdxg.lock2_diag_prints < 32) {
                printf("hyperv-dxg: lock2 map_fail device=0x%x "
                       "allocation=0x%x status=0x%x offset=0x%lx "
                       "map_pa=0x%lx map_size=%lu ret=%d\n",
                       req.device.v, req.allocation.v,
                       (uint32)hvdxg.lock2_last_status,
                       hvdxg.lock2_last_offset, map_pa, map_size, ret);
                hvdxg.lock2_diag_prints++;
            }
            break;
        }
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        hvdxg.lock2_last_ret = ret;
        hvdxg_note_lock2_history(actual_len, ret,
                                 (uint32)hvdxg.lock2_last_status,
                                 req.device.v, req.allocation.v,
                                 hvdxg.lock2_last_offset,
                                 hvdxg.lock2_last_user_va, map_size);
        if (hvdxg.lock2_diag_prints < 32) {
            printf("hyperv-dxg: lock2 host device=0x%x allocation=0x%x "
                   "flags=0x%x status=0x%x ret=%d len=%u offset=0x%lx "
                   "map_pa=0x%lx map_size=%lu user=0x%lx ref=%u\n",
                   req.device.v, req.allocation.v, req.flags.value,
                   (uint32)hvdxg.lock2_last_status, ret, actual_len,
                   hvdxg.lock2_last_offset, map_pa, map_size,
                   hvdxg.lock2_last_user_va,
                   tracked != NULL ? tracked->lock_refcount : 0);
            hvdxg.lock2_diag_prints++;
        }
        break;
    }

    case LX_DXUNLOCK2: {
        struct d3dkmt_unlock2 req;
        struct hvdxg_command_unlock2 unlock;
        struct hvdxg_ntstatus status;
        struct hvdxg_tracked_allocation *tracked;
        uint32 actual_len = 0;
        int skip_host_unlock = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.unlock2_ioctl_count++;
        hvdxg.unlock2_last_allocation = req.allocation.v;
        hvdxg.unlock2_last_status = 0;
        if (req.device.v == 0 || req.allocation.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v) ||
            !hvdxg_owner_has_allocation(owner, req.device.v, 0,
                                        req.allocation.v)) {
            ret = -EPERM;
            break;
        }
        tracked = hvdxg_owner_find_allocation(owner, req.device.v, 0,
                                              req.allocation.v);
        if (tracked == NULL || tracked->cpu_va == 0)
            hvdxg.unlock2_missing_tracking_count++;
        if (tracked != NULL && tracked->lock_refcount > 0) {
            tracked->lock_refcount--;
            if (tracked->lock_refcount != 0)
                skip_host_unlock = 1;
            else
                (void)hvdxg_unmap_tracked_allocation(tracked);
        } else {
            /*
             * WSL forwards LX_DXUNLOCK2 to the host even when userspace has
             * already dropped the CPU mapping bookkeeping.  Keep local mmap
             * cleanup best-effort, but do not hide the host-visible packet.
             */
            skip_host_unlock = 0;
        }
        if (skip_host_unlock) {
            hvdxg.unlock2_cached_ref_count++;
            hvdxg.unlock2_last_len = 0;
            hvdxg.unlock2_last_ret = 0;
            break;
        }
        memset(&unlock, 0, sizeof(unlock));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(&unlock.hdr,
                                        HV_DXGK_VMBCOMMAND_UNLOCK2,
                                        hvdxg.dxg_process);
        unlock.args = req;
        ret = hvdxg_send_sync_vgpu(&unlock, sizeof(unlock), &status,
                                   sizeof(status), &actual_len);
        hvdxg.unlock2_host_forward_count++;
        hvdxg.unlock2_last_len = actual_len;
        hvdxg.unlock2_last_ret = ret;
        if (actual_len >= sizeof(status))
            hvdxg.unlock2_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.unlock2_last_ret = ret;
        if (hvdxg.unlock2_diag_prints < 32) {
            printf("hyperv-dxg: unlock2 host device=0x%x allocation=0x%x "
                   "len=%u status=0x%x ret=%d forwarded=%u\n",
                   req.device.v, req.allocation.v, actual_len,
                   (uint32)hvdxg.unlock2_last_status, ret,
                   hvdxg.unlock2_host_forward_count);
            hvdxg.unlock2_diag_prints++;
        }
        break;
    }

    case LX_DXCREATESYNCHRONIZATIONOBJECT: {
        struct d3dkmt_createsynchronizationobject2 req;
        struct hvdxg_command_createsyncobject create;
        struct hvdxg_command_createsyncobject_return result;
        struct hvdxg_d3dkmthandle create_process;
        uint32 actual_len = 0;
        uint64 fence_kva = 0;
        uint64 fence_pa = 0;
        uint64 raw_fence_pa = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EPERM;
            break;
        }
        memset(&create, 0, sizeof(create));
        memset(&result, 0, sizeof(result));
        create_process = hvdxg_owner_bound_process_handle(owner);
        hvdxg.syncobject_last_process = create_process.v;
        hvdxg.syncobject_last_owner_process = 0;
        hvdxg.syncobject_last_owner_generation = 0;
        hvdxg.syncobject_last_cmd_len = sizeof(create);
        hvdxg.syncobject_last_result_len = sizeof(result);
        hvdxg.syncobject_last_result_sync_offset =
            offsetof(struct hvdxg_command_createsyncobject_return,
                     sync_object);
        hvdxg.syncobject_last_result_global_offset =
            offsetof(struct hvdxg_command_createsyncobject_return,
                     global_sync_object);
        hvdxg.syncobject_last_result_fence_gpu_offset =
            offsetof(struct hvdxg_command_createsyncobject_return,
                     fence_gpu_va);
        hvdxg.syncobject_last_result_fence_pa_offset =
            offsetof(struct hvdxg_command_createsyncobject_return,
                     fence_storage_address);
        hvdxg.syncobject_last_result_fence_off_offset =
            offsetof(struct hvdxg_command_createsyncobject_return,
                     fence_storage_offset);
        hvdxg.syncobject_last_result_head_len = 0;
        memset(hvdxg.syncobject_last_result_head, 0,
               sizeof(hvdxg.syncobject_last_result_head));
        hvdxg.syncobject_last_args_offset =
            offsetof(struct hvdxg_command_createsyncobject, args);
        hvdxg.syncobject_last_client_hint_offset =
            offsetof(struct hvdxg_command_createsyncobject, client_hint);
        hvdxg.syncobject_last_client_hint = 0;
        hvdxg.syncobject_last_input_shared = req.info.shared_handle.v;
        hvdxg_command_vgpu_init_process(&create.hdr,
                                        HV_DXGK_VMBCOMMAND_CREATESYNCOBJECT,
                                        create_process);
        create.args = req;
        create.args.sync_object.v = 0;
        create.client_hint = 1;
        hvdxg.syncobject_last_client_hint = create.client_hint;
        ret = hvdxg_send_sync_vgpu(&create, sizeof(create), &result,
                                   sizeof(result), &actual_len);
        hvdxg_save_priv_head(hvdxg.syncobject_last_result_head,
                             sizeof(hvdxg.syncobject_last_result_head),
                             &hvdxg.syncobject_last_result_head_len,
                             (const uint8 *)&result,
                             actual_len < sizeof(result) ?
                                 actual_len : sizeof(result));
        hvdxg.syncobject_last_len = actual_len;
        hvdxg.syncobject_last_ret = ret;
        hvdxg.syncobject_last_handle = result.sync_object.v;
        hvdxg.syncobject_last_type = (uint32)req.info.type;
        hvdxg.syncobject_last_flags = req.info.flags.value;
        hvdxg.syncobject_last_global = result.global_sync_object.v;
        hvdxg.syncobject_last_fence_cpu = 0;
        hvdxg.syncobject_last_fence_gpu = result.fence_gpu_va;
        hvdxg.syncobject_last_fence_pa = result.fence_storage_address;
        hvdxg.syncobject_last_fence_off = result.fence_storage_offset;
        if (ret != 0)
            break;
        if (actual_len < sizeof(result) || result.sync_object.v == 0) {
            ret = -EIO;
            break;
        }
        req.sync_object.v = result.sync_object.v;
        if (req.info.flags.shared)
            req.info.shared_handle.v = result.global_sync_object.v;
        if (req.info.type == _D3DDDI_MONITORED_FENCE) {
            raw_fence_pa = result.fence_storage_address;
            req.info.monitored_fence.fence_cpu_virtual_address =
                hvdxg_map_iospace_user_canonical(
                    HV_DXG_FENCE_SOURCE_SYNCOBJECT, raw_fence_pa,
                    sizeof(uint64), 0, &fence_pa, NULL, 0);
            fence_kva = hvdxg_map_iospace_kernel_canonical(
                HV_DXG_FENCE_SOURCE_SYNCOBJECT, raw_fence_pa,
                sizeof(uint64), fence_pa, 0);
            hvdxg_note_fence_offset_candidate(HV_DXG_FENCE_SOURCE_SYNCOBJECT,
                                              raw_fence_pa,
                                              result.fence_storage_offset,
                                              sizeof(uint64));
            hvdxg.syncobject_last_fence_cpu =
                req.info.monitored_fence.fence_cpu_virtual_address;
            if (req.info.monitored_fence.fence_cpu_virtual_address == 0) {
                ret = -ENOMEM;
                break;
            }
            req.info.monitored_fence.fence_gpu_virtual_address =
                result.fence_gpu_va;
        } else if (req.info.type == _D3DDDI_PERIODIC_MONITORED_FENCE) {
            raw_fence_pa = result.fence_storage_address;
            req.info.periodic_monitored_fence.fence_cpu_virtual_address =
                hvdxg_map_iospace_user_canonical(
                    HV_DXG_FENCE_SOURCE_SYNCOBJECT, raw_fence_pa,
                    sizeof(uint64), 0, &fence_pa, NULL, 0);
            fence_kva = hvdxg_map_iospace_kernel_canonical(
                HV_DXG_FENCE_SOURCE_SYNCOBJECT, raw_fence_pa,
                sizeof(uint64), fence_pa, 0);
            hvdxg_note_fence_offset_candidate(HV_DXG_FENCE_SOURCE_SYNCOBJECT,
                                              raw_fence_pa,
                                              result.fence_storage_offset,
                                              sizeof(uint64));
            hvdxg.syncobject_last_fence_cpu =
                req.info.periodic_monitored_fence.fence_cpu_virtual_address;
            if (req.info.periodic_monitored_fence.fence_cpu_virtual_address ==
                0) {
                ret = -ENOMEM;
                break;
            }
            req.info.periodic_monitored_fence.fence_gpu_virtual_address =
                result.fence_gpu_va;
        }
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        if (ret == 0 && hvdxg.syncobject_last_fence_cpu != 0 &&
            hvdxg.syncobject_last_fence_gpu != 0) {
            hvdxg.syncobject_mapped_count++;
            hvdxg.syncobject_mapped_len = actual_len;
            hvdxg.syncobject_mapped_type = (uint32)req.info.type;
            hvdxg.syncobject_mapped_flags = req.info.flags.value;
            hvdxg.syncobject_mapped_fence_cpu =
                hvdxg.syncobject_last_fence_cpu;
            hvdxg.syncobject_mapped_fence_gpu =
                hvdxg.syncobject_last_fence_gpu;
        }
        if (ret == 0 && owner != NULL) {
            hvdxg_track_sync(owner, req.device.v, req.sync_object.v,
                             req.info.type, req.info.flags.value,
                             result.global_sync_object.v,
                             hvdxg.syncobject_last_fence_cpu, fence_kva, 0);
            hvdxg.syncobject_last_owner_process =
                hvdxg_owner_sync_owner_process(owner, req.sync_object.v);
            hvdxg.syncobject_last_owner_generation =
                hvdxg_owner_sync_owner_generation(owner, req.sync_object.v);
        }
        if (ret == 0 && hvdxg.sync_diag_prints < 64) {
            printf("hyperv-dxg: create sync handle=0x%x device=0x%x "
                   "type=%u flags=0x%x global=0x%x monitor=%u\n",
                   req.sync_object.v, req.device.v, (uint32)req.info.type,
                   req.info.flags.value, result.global_sync_object.v,
                   hvdxg_sync_type_is_monitored(req.info.type));
            hvdxg.sync_diag_prints++;
        }
        break;
    }

    case LX_DXSIGNALSYNCHRONIZATIONOBJECT: {
        struct d3dkmt_signalsynchronizationobject2 req;
        uint8 command_buf[sizeof(struct hvdxg_command_signalsyncobject) +
                          D3DDDI_MAX_OBJECT_SIGNALED *
                              sizeof(struct hvdxg_d3dkmthandle) +
                          (D3DDDI_MAX_BROADCAST_CONTEXT + 1) *
                              sizeof(struct hvdxg_d3dkmthandle)];
        struct hvdxg_command_signalsyncobject *signal =
            (struct hvdxg_command_signalsyncobject *)command_buf;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 object_size;
        uint32 context_size;
        uint8 *pos;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.flags.enqueue_cpu_event) {
            hvdxg_note_unsupported_ioctl(
                cmd, 0, (uint32)req.cpu_event_handle, req.object_count);
            ret = -ENOTSUP;
            break;
        }
        if (req.context.v == 0 || req.object_count == 0 ||
            req.object_count > D3DDDI_MAX_OBJECT_SIGNALED ||
            req.context_count >= D3DDDI_MAX_BROADCAST_CONTEXT) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_context(owner, req.context.v)) {
            ret = -EPERM;
            break;
        }
        for (uint32 i = 0; i < req.object_count; i++) {
            if (!hvdxg_owner_has_sync(owner, req.object_array[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        for (uint32 i = 0; ret == 0 && i < req.context_count; i++) {
            if (!hvdxg_owner_has_context(owner, req.contexts[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            break;
        object_size = req.object_count * sizeof(struct hvdxg_d3dkmthandle);
        context_size = req.context_count *
                       sizeof(struct hvdxg_d3dkmthandle);
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg.syncgpu_signal_last_status = 0;
        hvdxg_command_vgpu_init_process(&signal->hdr,
                                        HV_DXGK_VMBCOMMAND_SIGNALSYNCOBJECT,
                                        hvdxg.dxg_process);
        signal->object_count = req.object_count;
        signal->flags = req.flags;
        signal->context_count = req.context_count + 1;
        signal->fence_value = req.fence.fence_value;
        signal->u.device.v = 0;
        pos = (uint8 *)&signal[1];
        memcpy(pos, req.object_array, object_size);
        pos += object_size;
        memcpy(pos, &req.context, sizeof(req.context));
        pos += sizeof(req.context);
        memcpy(pos, req.contexts, context_size);
        ret = hvdxg_send_sync_vgpu(signal,
                                   sizeof(*signal) + object_size +
                                       sizeof(req.context) + context_size,
                                   &status, sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.syncgpu_signal_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.syncgpu_signal_last_len = actual_len;
        hvdxg.syncgpu_signal_last_ret = ret;
        break;
    }

    case LX_DXWAITFORSYNCHRONIZATIONOBJECT: {
        struct d3dkmt_waitforsynchronizationobject2 req;
        uint8 command_buf[sizeof(struct hvdxg_command_waitsyncobjectfromgpu) +
                          D3DDDI_MAX_OBJECT_WAITED_ON *
                              (sizeof(struct hvdxg_d3dkmthandle) +
                               sizeof(uint64))];
        struct hvdxg_command_waitsyncobjectfromgpu *wait =
            (struct hvdxg_command_waitsyncobjectfromgpu *)command_buf;
        struct hvdxg_ntstatus status;
        uint64 fence_values[D3DDDI_MAX_OBJECT_WAITED_ON];
        uint32 actual_len = 0;
        uint32 object_size;
        uint32 fence_size;
        uint8 *pos;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.context.v == 0 || req.object_count == 0 ||
            req.object_count > D3DDDI_MAX_OBJECT_WAITED_ON) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_context(owner, req.context.v)) {
            ret = -EPERM;
            break;
        }
        for (uint32 i = 0; i < req.object_count; i++) {
            if (!hvdxg_owner_has_sync(owner, req.object_array[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            break;
        for (uint32 i = 0; i < req.object_count; i++)
            fence_values[i] = req.fence.fence_value;
        object_size = req.object_count * sizeof(struct hvdxg_d3dkmthandle);
        fence_size = req.object_count * sizeof(uint64);
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg.syncgpu_wait_last_status = 0;
        hvdxg_command_vgpu_init_process(
            &wait->hdr, HV_DXGK_VMBCOMMAND_WAITFORSYNCOBJECTFROMGPU,
            hvdxg.dxg_process);
        wait->context.v = req.context.v;
        wait->object_count = req.object_count;
        wait->legacy_fence_object = 1;
        pos = (uint8 *)wait->fence_values;
        memcpy(pos, fence_values, fence_size);
        pos += fence_size;
        memcpy(pos, req.object_array, object_size);
        ret = hvdxg_send_sync_vgpu(wait,
                                   sizeof(*wait) - sizeof(uint64) +
                                       fence_size + object_size,
                                   &status, sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.syncgpu_wait_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.syncgpu_wait_last_len = actual_len;
        hvdxg.syncgpu_wait_last_ret = ret;
        break;
    }

    case LX_DXSIGNALSYNCHRONIZATIONOBJECTFROMCPU: {
        struct d3dkmt_signalsynchronizationobjectfromcpu req;
        uint8 command_buf[sizeof(struct hvdxg_command_signalsyncobject) +
                          D3DDDI_MAX_OBJECT_SIGNALED *
                              (sizeof(struct hvdxg_d3dkmthandle) +
                               sizeof(uint64))];
        struct hvdxg_command_signalsyncobject *signal =
            (struct hvdxg_command_signalsyncobject *)command_buf;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 object_size;
        uint32 fence_size;
        uint8 *pos;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0 || req.object_count == 0 ||
            req.object_count > D3DDDI_MAX_OBJECT_SIGNALED ||
            req.objects == 0 || req.fence_values == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EPERM;
            break;
        }
        object_size = req.object_count * sizeof(struct hvdxg_d3dkmthandle);
        fence_size = req.object_count * sizeof(uint64);
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg.syncsignal_last_status = 0;
        hvdxg_command_vgpu_init_process(&signal->hdr,
                                        HV_DXGK_VMBCOMMAND_SIGNALSYNCOBJECT,
                                        hvdxg.dxg_process);
        signal->object_count = req.object_count;
        signal->flags = req.flags;
        signal->context_count = 0;
        signal->fence_value = 0;
        signal->u.device.v = req.device.v;
        pos = (uint8 *)&signal[1];
        if (either_copyin(pos, 1, req.objects, object_size) < 0 ||
            either_copyin(pos + object_size, 1, req.fence_values,
                          fence_size) < 0) {
            ret = -EFAULT;
            break;
        }
        {
            struct hvdxg_d3dkmthandle *objects =
                (struct hvdxg_d3dkmthandle *)(uint8 *)&signal[1];
            for (uint32 i = 0; i < req.object_count; i++) {
                if (!hvdxg_owner_has_sync(owner, objects[i].v)) {
                    ret = -EPERM;
                    break;
                }
            }
            if (ret != 0)
                break;
        }
        ret = hvdxg_send_sync_vgpu(signal,
                                   sizeof(*signal) + object_size + fence_size,
                                   &status, sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.syncsignal_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.syncsignal_last_len = actual_len;
        hvdxg.syncsignal_last_ret = ret;
        break;
    }

    case LX_DXSIGNALSYNCHRONIZATIONOBJECTFROMGPU: {
        struct d3dkmt_signalsynchronizationobjectfromgpu req;
        uint8 command_buf[sizeof(struct hvdxg_command_signalsyncobject) +
                          D3DDDI_MAX_OBJECT_SIGNALED *
                              (sizeof(struct hvdxg_d3dkmthandle) +
                               sizeof(uint64)) +
                          sizeof(struct hvdxg_d3dkmthandle)];
        struct hvdxg_command_signalsyncobject *signal =
            (struct hvdxg_command_signalsyncobject *)command_buf;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 object_size;
        uint32 fence_size;
        uint8 *pos;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.context.v == 0 || req.object_count == 0 ||
            req.object_count > D3DDDI_MAX_OBJECT_SIGNALED ||
            req.objects == 0 || req.monitored_fence_values == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_context(owner, req.context.v)) {
            ret = -EPERM;
            break;
        }
        object_size = req.object_count * sizeof(struct hvdxg_d3dkmthandle);
        fence_size = req.object_count * sizeof(uint64);
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg.syncgpu_signal_last_status = 0;
        hvdxg_command_vgpu_init_process(&signal->hdr,
                                        HV_DXGK_VMBCOMMAND_SIGNALSYNCOBJECT,
                                        hvdxg.dxg_process);
        signal->object_count = req.object_count;
        signal->context_count = 1;
        pos = (uint8 *)&signal[1];
        if (either_copyin(pos, 1, req.objects, object_size) < 0) {
            ret = -EFAULT;
            break;
        }
        {
            struct hvdxg_d3dkmthandle *objects =
                (struct hvdxg_d3dkmthandle *)pos;
            for (uint32 i = 0; i < req.object_count; i++) {
                if (!hvdxg_owner_has_sync(owner, objects[i].v)) {
                    ret = -EPERM;
                    break;
                }
            }
            if (ret != 0)
                break;
        }
        pos += object_size;
        memcpy(pos, &req.context, sizeof(req.context));
        pos += sizeof(req.context);
        if (either_copyin(pos, 1, req.monitored_fence_values,
                          fence_size) < 0) {
            ret = -EFAULT;
            break;
        }
        ret = hvdxg_send_sync_vgpu(signal,
                                   sizeof(*signal) + object_size +
                                       sizeof(req.context) + fence_size,
                                   &status, sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.syncgpu_signal_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.syncgpu_signal_last_len = actual_len;
        hvdxg.syncgpu_signal_last_ret = ret;
        break;
    }

    case LX_DXSIGNALSYNCHRONIZATIONOBJECTFROMGPU2: {
        struct d3dkmt_signalsynchronizationobjectfromgpu2 req;
        uint8 command_buf[sizeof(struct hvdxg_command_signalsyncobject) +
                          D3DDDI_MAX_OBJECT_SIGNALED *
                              (sizeof(struct hvdxg_d3dkmthandle) +
                               sizeof(uint64)) +
                          D3DDDI_MAX_BROADCAST_CONTEXT *
                              sizeof(struct hvdxg_d3dkmthandle)];
        struct hvdxg_command_signalsyncobject *signal =
            (struct hvdxg_command_signalsyncobject *)command_buf;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 object_size;
        uint32 context_size;
        uint32 fence_size;
        uint8 *pos;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.flags.enqueue_cpu_event) {
            hvdxg_note_unsupported_ioctl(
                cmd, 0, (uint32)req.cpu_event_handle, req.object_count);
            ret = -ENOTSUP;
            break;
        }
        if (req.object_count == 0 ||
            req.object_count > D3DDDI_MAX_OBJECT_SIGNALED ||
            req.context_count == 0 ||
            req.context_count > D3DDDI_MAX_BROADCAST_CONTEXT ||
            req.objects == 0 || req.contexts == 0 ||
            req.monitored_fence_values == 0) {
            ret = -EINVAL;
            break;
        }
        object_size = req.object_count * sizeof(struct hvdxg_d3dkmthandle);
        context_size = req.context_count *
                       sizeof(struct hvdxg_d3dkmthandle);
        fence_size = req.object_count * sizeof(uint64);
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg.syncgpu_signal_last_status = 0;
        hvdxg_command_vgpu_init_process(&signal->hdr,
                                        HV_DXGK_VMBCOMMAND_SIGNALSYNCOBJECT,
                                        hvdxg.dxg_process);
        signal->object_count = req.object_count;
        signal->flags = req.flags;
        signal->context_count = req.context_count;
        signal->fence_value = 0;
        signal->u.device.v = 0;
        pos = (uint8 *)&signal[1];
        if (either_copyin(pos, 1, req.objects, object_size) < 0) {
            ret = -EFAULT;
            break;
        }
        {
            struct hvdxg_d3dkmthandle *objects =
                (struct hvdxg_d3dkmthandle *)pos;
            for (uint32 i = 0; i < req.object_count; i++) {
                if (!hvdxg_owner_has_sync(owner, objects[i].v)) {
                    ret = -EPERM;
                    break;
                }
            }
            if (ret != 0)
                break;
        }
        pos += object_size;
        if (either_copyin(pos, 1, req.contexts, context_size) < 0) {
            ret = -EFAULT;
            break;
        }
        {
            struct hvdxg_d3dkmthandle *contexts =
                (struct hvdxg_d3dkmthandle *)pos;
            for (uint32 i = 0; i < req.context_count; i++) {
                if (!hvdxg_owner_has_context(owner, contexts[i].v)) {
                    ret = -EPERM;
                    break;
                }
            }
            if (ret != 0)
                break;
        }
        pos += context_size;
        if (either_copyin(pos, 1, req.monitored_fence_values,
                          fence_size) < 0) {
            ret = -EFAULT;
            break;
        }
        ret = hvdxg_send_sync_vgpu(signal,
                                   sizeof(*signal) + object_size +
                                       context_size + fence_size,
                                   &status, sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.syncgpu_signal_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.syncgpu_signal_last_len = actual_len;
        hvdxg.syncgpu_signal_last_ret = ret;
        break;
    }

    case LX_DXWAITFORSYNCHRONIZATIONOBJECTFROMCPU: {
        struct d3dkmt_waitforsynchronizationobjectfromcpu req;
        struct hvdxg_d3dkmthandle objects[D3DDDI_MAX_OBJECT_WAITED_ON];
        uint64 fence_values[D3DDDI_MAX_OBJECT_WAITED_ON];
        uint64 event_id = 0;
        struct vfs_file *async_event_file = NULL;
        uint32 actual_len = 0;
        uint32 object_size;
        uint32 fence_size;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0 || req.object_count == 0 ||
            req.object_count > D3DDDI_MAX_OBJECT_WAITED_ON ||
            req.objects == 0 || req.fence_values == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EPERM;
            break;
        }
        object_size = req.object_count * sizeof(struct hvdxg_d3dkmthandle);
        fence_size = req.object_count * sizeof(uint64);
        memset(objects, 0, sizeof(objects));
        memset(fence_values, 0, sizeof(fence_values));
        if (either_copyin(objects, 1, req.objects, object_size) < 0 ||
            either_copyin(fence_values, 1, req.fence_values, fence_size) < 0) {
            ret = -EFAULT;
            break;
        }
        for (uint32 i = 0; i < req.object_count; i++) {
            if (!hvdxg_owner_has_sync(owner, objects[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            break;
        hvdxg_note_cpu_wait_state(owner, objects, fence_values,
                                  req.object_count, 0,
                                  req.async_event != 0, 0);
        if (hvdxg_wait_cpu_fences_already_satisfied(
                owner, objects, fence_values, req.object_count,
                req.flags.wait_any)) {
            hvdxg.syncwait_last_len = 0;
            hvdxg.syncwait_last_ret = 0;
            hvdxg.syncwait_last_status = 0;
            hvdxg_note_cpu_wait_state(owner, objects, fence_values,
                                      req.object_count, 0,
                                      req.async_event != 0, 2);
            ret = 0;
            break;
        }
        if (req.async_event != 0) {
            async_event_file =
                vfs_fdtable_get_file(current->fdtable, (int)req.async_event);
            if (async_event_file == NULL) {
                ret = -EINVAL;
                break;
            }
            if (!eventfd_file_is_eventfd(async_event_file)) {
                vfs_fput(async_event_file);
                ret = -EINVAL;
                break;
            }
            event_id = hvdxg_alloc_host_event_file(async_event_file, 1);
        } else {
            event_id = hvdxg_alloc_host_event();
        }
        if (event_id == 0) {
            if (async_event_file != NULL)
                vfs_fput(async_event_file);
            ret = -ENOMEM;
            break;
        }
        hvdxg_note_cpu_wait_state(owner, objects, fence_values,
                                  req.object_count, event_id,
                                  req.async_event != 0, 0);
        ret = hvdxg_send_waitsyncobjectfromcpu(&req, objects, fence_values,
                                               event_id, object_size,
                                               fence_size, &actual_len);
        if ((ret == 0 || ret == HV_DXG_STATUS_PENDING) &&
            req.async_event != 0) {
            hvdxg_pump_events_ms(20);
            hvdxg_note_cpu_wait_state(owner, objects, fence_values,
                                      req.object_count, event_id, 1, 5);
        }
        if ((ret == 0 || ret == HV_DXG_STATUS_PENDING) &&
            req.async_event == 0) {
            ret = hvdxg_wait_host_event_or_cpu_fence(
                owner, objects, fence_values, req.object_count,
                req.flags.wait_any, event_id, HV_DXG_HOST_EVENT_TIMEOUT_MS);
            hvdxg.syncwait_last_ret = ret;
        } else if (ret != 0 && ret != HV_DXG_STATUS_PENDING) {
            hvdxg_note_cpu_wait_state(owner, objects, fence_values,
                                      req.object_count, event_id,
                                      req.async_event != 0, 4);
        }
        if ((ret != 0 && ret != HV_DXG_STATUS_PENDING) ||
            req.async_event == 0)
            hvdxg_remove_host_event(event_id);
        break;
    }

    case LX_DXWAITFORSYNCHRONIZATIONOBJECTFROMGPU: {
        struct d3dkmt_waitforsynchronizationobjectfromgpu req;
        uint8 command_buf[sizeof(struct hvdxg_command_waitsyncobjectfromgpu) +
                          D3DDDI_MAX_OBJECT_WAITED_ON *
                              (sizeof(struct hvdxg_d3dkmthandle) +
                               sizeof(uint64))];
        struct hvdxg_command_waitsyncobjectfromgpu *wait =
            (struct hvdxg_command_waitsyncobjectfromgpu *)command_buf;
        struct hvdxg_ntstatus status;
        struct hvdxg_d3dkmthandle objects[D3DDDI_MAX_OBJECT_WAITED_ON];
        uint64 fences[D3DDDI_MAX_OBJECT_WAITED_ON];
        uint32 actual_len = 0;
        uint32 object_size;
        uint32 fence_size;
        int monitored_fence = 0;
        uint8 *pos;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.context.v == 0 || req.object_count == 0 ||
            req.object_count > D3DDDI_MAX_OBJECT_WAITED_ON ||
            req.objects == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_context(owner, req.context.v)) {
            ret = -EPERM;
            break;
        }
        object_size = req.object_count * sizeof(struct hvdxg_d3dkmthandle);
        fence_size = req.object_count * sizeof(uint64);
        memset(objects, 0, sizeof(objects));
        memset(fences, 0, sizeof(fences));
        if (either_copyin(objects, 1, req.objects, object_size) < 0) {
            ret = -EFAULT;
            break;
        }
        monitored_fence =
            hvdxg_owner_sync_is_monitored(owner, objects[0].v);
        if (monitored_fence) {
            if (req.monitored_fence_values == 0) {
                ret = -EINVAL;
                break;
            }
            if (either_copyin(fences, 1, req.monitored_fence_values,
                              fence_size) < 0) {
                ret = -EFAULT;
                break;
            }
        } else {
            if (req.object_count != 1) {
                ret = -EINVAL;
                break;
            }
            fences[0] = req.fence_value;
        }
        for (uint32 i = 0; i < req.object_count; i++) {
            if (!hvdxg_owner_has_sync(owner, objects[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            break;
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg.syncgpu_wait_last_status = 0;
        hvdxg_command_vgpu_init_process(
            &wait->hdr, HV_DXGK_VMBCOMMAND_WAITFORSYNCOBJECTFROMGPU,
            hvdxg.dxg_process);
        wait->context.v = req.context.v;
        wait->object_count = req.object_count;
        wait->legacy_fence_object = monitored_fence ? 0 : 1;
        pos = (uint8 *)wait->fence_values;
        memcpy(pos, fences, fence_size);
        pos += fence_size;
        memcpy(pos, objects, object_size);
        hvdxg.syncgpu_wait_last_context = req.context.v;
        hvdxg.syncgpu_wait_last_object = objects[0].v;
        hvdxg.syncgpu_wait_last_object_count = req.object_count;
        hvdxg.syncgpu_wait_last_object_type =
            hvdxg_owner_sync_type(owner, objects[0].v);
        hvdxg.syncgpu_wait_last_legacy = wait->legacy_fence_object;
        hvdxg.syncgpu_wait_last_fence = fences[0];
        hvdxg.syncgpu_wait_last_cmd_len =
            sizeof(*wait) - sizeof(uint64) + fence_size + object_size;
        ret = hvdxg_send_sync_vgpu(wait,
                                   sizeof(*wait) - sizeof(uint64) +
                                       fence_size + object_size,
                                   &status, sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.syncgpu_wait_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.syncgpu_wait_last_len = actual_len;
        hvdxg.syncgpu_wait_last_ret = ret;
        break;
    }

    case LX_DXDESTROYSYNCHRONIZATIONOBJECT: {
        struct d3dkmt_destroysynchronizationobject req;
        struct hvdxg_command_destroysyncobject destroy;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.sync_object.v == 0) {
            ret = -EINVAL;
            break;
        }
        hvdxg.destroysync_last_handle = req.sync_object.v;
        hvdxg.destroysync_last_device =
            hvdxg_owner_sync_device(owner, req.sync_object.v);
        hvdxg.destroysync_last_type =
            hvdxg_owner_sync_type(owner, req.sync_object.v);
        hvdxg.destroysync_last_flags =
            hvdxg_owner_sync_flags(owner, req.sync_object.v);
        hvdxg.destroysync_last_global =
            hvdxg_owner_sync_global_shared(owner, req.sync_object.v);
        hvdxg.destroysync_last_monitor_fence =
            hvdxg_owner_sync_is_monitor_fence_handle(owner, req.sync_object.v);
        hvdxg.destroysync_last_cmd_len = 0;
        hvdxg.destroysync_last_wire_len = 0;
        hvdxg.destroysync_last_ext = 0;
        hvdxg.destroysync_last_ext_offset = 0;
        if (!hvdxg_owner_has_sync(owner, req.sync_object.v)) {
            ret = -EINVAL;
            hvdxg.destroysync_last_ret = ret;
            hvdxg.destroysync_last_len = 0;
            break;
        }
        if (hvdxg.destroysync_last_monitor_fence) {
            /*
             * WSL stores paging/hwqueue progress fences as
             * HMGRENTRY_TYPE_MONITOREDFENCE, not DXGSYNCOBJECT.  The
             * destroy-sync ioctl therefore rejects them locally and never
             * sends DXGK_VMBCOMMAND_DESTROYSYNCOBJECT to the host.
             */
            ret = -EINVAL;
            hvdxg.destroysync_last_ret = ret;
            hvdxg.destroysync_last_len = 0;
            break;
        }
        memset(&destroy, 0, sizeof(destroy));
        memset(&status, 0, sizeof(status));
        hvdxg.destroysync_last_cmd_len = sizeof(destroy);
        hvdxg_command_vm_init(&destroy.hdr,
                              HV_DXGK_VMBCOMMAND_DESTROYSYNCOBJECT);
        destroy.hdr.process = hvdxg_owner_bound_process_handle(owner);
        destroy.sync_object.v = req.sync_object.v;
        if (hvdxg.sync_diag_prints < 64) {
            printf("hyperv-dxg: destroy sync handle=0x%x process=0x%x "
                   "device=0x%x type=%u flags=0x%x global=0x%x "
                   "monitor=%u cmd_len=%u\n",
                   req.sync_object.v, destroy.hdr.process.v,
                   hvdxg.destroysync_last_device,
                   hvdxg.destroysync_last_type,
                   hvdxg.destroysync_last_flags,
                   hvdxg.destroysync_last_global,
                   hvdxg.destroysync_last_monitor_fence,
                   hvdxg.destroysync_last_cmd_len);
            hvdxg.sync_diag_prints++;
        }
        if (owner != NULL)
            hvdxg_untrack_sync(owner, req.sync_object.v);
        ret = hvdxg_send_sync_global(&destroy, sizeof(destroy), &status,
                                     sizeof(status), &actual_len);
        hvdxg.destroysync_last_cmd_len = hvdxg.global_send_last_cmd_len;
        hvdxg.destroysync_last_wire_len = hvdxg.global_send_last_wire_len;
        hvdxg.destroysync_last_ext = hvdxg.global_send_last_ext;
        hvdxg.destroysync_last_ext_offset =
            hvdxg.global_send_last_ext_offset;
        if (actual_len >= sizeof(status))
            hvdxg.destroysync_last_status = status.v;
        if (ret == 0 && actual_len == 0) {
            ret = 0;
        } else if (ret == 0 && actual_len < sizeof(status)) {
            ret = -EOVERFLOW;
        } else if (ret == 0 && actual_len >= sizeof(status)) {
            ret = hvdxg_ntstatus_to_errno(status);
        }
        hvdxg.destroysync_last_len = actual_len;
        hvdxg.destroysync_last_ret = ret;
        break;
    }

    case LX_DXCREATECONTEXTVIRTUAL: {
        struct d3dkmt_createcontextvirtual req;
        uint8 *command_buf = NULL;
        struct hvdxg_command_createcontextvirtual *create;
        uint32 command_len;
        uint32 actual_len = 0;
        uint32 private_max = HV_DXG_VM_BUS_PACKET_MAX -
                             sizeof(struct hvdxg_command_createcontextvirtual) +
                             1;
        struct hvdxg_ntstatus short_status;
        struct hvdxg_d3dkmthandle create_process;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0 ||
            req.priv_drv_data_size > private_max ||
            (req.priv_drv_data_size != 0 && req.priv_drv_data == 0)) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EPERM;
            break;
        }
        command_len = sizeof(struct hvdxg_command_createcontextvirtual);
        if (req.priv_drv_data_size != 0)
            command_len += req.priv_drv_data_size - 1;
        command_buf = kvmalloc(command_len);
        if (command_buf == NULL) {
            ret = -ENOMEM;
            break;
        }
        memset(command_buf, 0, command_len);
        create = (struct hvdxg_command_createcontextvirtual *)command_buf;
        create_process = hvdxg_owner_bound_process_handle(owner);
        hvdxg_command_vgpu_init_process(&create->hdr,
                                        HV_DXGK_VMBCOMMAND_CREATECONTEXTVIRTUAL,
                                        create_process);
        create->device.v = req.device.v;
        create->node_ordinal = req.node_ordinal;
        create->engine_affinity = req.engine_affinity;
        create->flags = req.flags;
        create->client_hint = (uint32)req.client_hint;
        create->priv_drv_data_size = req.priv_drv_data_size;
        hvdxg.createcontext_last_device = req.device.v;
        hvdxg.createcontext_last_node = req.node_ordinal;
        hvdxg.createcontext_last_engine = req.engine_affinity;
        hvdxg.createcontext_last_flags = req.flags.value;
        hvdxg.createcontext_last_hint = (uint32)req.client_hint;
        hvdxg.createcontext_last_priv_size = req.priv_drv_data_size;
        hvdxg.createcontext_last_priv_head_len = 0;
        memset(hvdxg.createcontext_last_priv_head, 0,
               sizeof(hvdxg.createcontext_last_priv_head));
        if (req.priv_drv_data_size != 0 &&
            either_copyin(create->priv_drv_data, 1, req.priv_drv_data,
                          req.priv_drv_data_size) < 0) {
            ret = -EFAULT;
            goto createcontext_done;
        }
        if (req.priv_drv_data_size != 0)
            hvdxg_save_priv_head(hvdxg.createcontext_last_priv_head,
                                 sizeof(hvdxg.createcontext_last_priv_head),
                                 &hvdxg.createcontext_last_priv_head_len,
                                 create->priv_drv_data,
                                 req.priv_drv_data_size);
        ret = hvdxg_send_sync_vgpu(create, command_len, command_buf,
                                   command_len, &actual_len);
        hvdxg.createcontext_last_len = actual_len;
        hvdxg.createcontext_last_ret = ret;
        hvdxg.createcontext_last_handle = create->context.v;
        if (ret != 0) {
            hvdxg.createcontext_fail_len = actual_len;
            hvdxg.createcontext_fail_ret = ret;
            hvdxg.createcontext_fail_status = 0;
            goto createcontext_done;
        }
        if (actual_len < sizeof(*create) - 1 || create->context.v == 0) {
            memset(&short_status, 0, sizeof(short_status));
            if (actual_len >= sizeof(short_status)) {
                memcpy(&short_status, command_buf, sizeof(short_status));
                ret = hvdxg_ntstatus_to_errno(short_status);
                if (ret >= 0)
                    ret = -EIO;
            } else {
                ret = -EIO;
            }
            hvdxg.createcontext_fail_len = actual_len;
            hvdxg.createcontext_fail_ret = ret;
            hvdxg.createcontext_fail_status = short_status.v;
            hvdxg.createcontext_last_ret = ret;
            goto createcontext_done;
        }
        req.context.v = create->context.v;
        hvdxg.createcontext_last_handle = req.context.v;
        if (req.priv_drv_data_size != 0 &&
            actual_len >= sizeof(*create) + req.priv_drv_data_size - 1)
            hvdxg_save_priv_head(hvdxg.createcontext_last_priv_head,
                                 sizeof(hvdxg.createcontext_last_priv_head),
                                 &hvdxg.createcontext_last_priv_head_len,
                                 create->priv_drv_data,
                                 req.priv_drv_data_size);
        if (req.priv_drv_data_size != 0 &&
            actual_len >= sizeof(*create) + req.priv_drv_data_size - 1 &&
            either_copyout(1, req.priv_drv_data, create->priv_drv_data,
                           req.priv_drv_data_size) < 0) {
            ret = -EFAULT;
            goto createcontext_done;
        }
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        if (ret == 0 && owner != NULL) {
            if (hvdxg_track_object(owner, HV_DXG_OBJECT_CONTEXT,
                                   req.context.v, req.device.v,
                                   req.device.v) != 0 ||
                hvdxg_track_u32_grow(&owner->contexts,
                                      &owner->context_count,
                                      &owner->context_capacity,
                                      req.context.v) != 0) {
                hvdxg_untrack_object(owner, HV_DXG_OBJECT_CONTEXT,
                                     req.context.v);
                (void)hvdxg_destroy_context_host(req.context.v);
                ret = -ENOMEM;
            }
        }
createcontext_done:
        if (command_buf)
            kvfree(command_buf);
        break;
    }

    case LX_DXCREATEHWQUEUE: {
        struct d3dkmt_createhwqueue req;
        uint8 *command_buf = NULL;
        struct hvdxg_command_createhwqueue *create;
        uint32 command_len;
        uint32 actual_len = 0;
        uint32 private_max = HV_DXG_VM_BUS_PACKET_MAX -
                             sizeof(struct hvdxg_command_createhwqueue) + 1;
        uint64 fence_kva = 0;
        uint64 fence_pa = 0;
        struct hvdxg_d3dkmthandle create_process;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.context.v == 0 ||
            req.priv_drv_data_size > private_max ||
            (req.priv_drv_data_size != 0 && req.priv_drv_data == 0)) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_context(owner, req.context.v)) {
            ret = -EPERM;
            break;
        }
        command_len = sizeof(struct hvdxg_command_createhwqueue);
        if (req.priv_drv_data_size != 0)
            command_len += req.priv_drv_data_size - 1;
        command_buf = kvmalloc(command_len);
        if (command_buf == NULL) {
            ret = -ENOMEM;
            break;
        }
        memset(command_buf, 0, command_len);
        create = (struct hvdxg_command_createhwqueue *)command_buf;
        create_process = hvdxg_owner_bound_process_handle(owner);
        hvdxg_command_vgpu_init_process(&create->hdr,
                                        HV_DXGK_VMBCOMMAND_CREATEHWQUEUE,
                                        create_process);
        create->context.v = req.context.v;
        create->flags = req.flags;
        create->priv_drv_data_size = req.priv_drv_data_size;
        hvdxg.createhwqueue_last_context = req.context.v;
        hvdxg.createhwqueue_last_flags = req.flags.value;
        hvdxg.createhwqueue_last_priv_size = req.priv_drv_data_size;
        hvdxg.createhwqueue_last_priv_head_len = 0;
        memset(hvdxg.createhwqueue_last_priv_head, 0,
               sizeof(hvdxg.createhwqueue_last_priv_head));
        if (req.priv_drv_data_size != 0 &&
            either_copyin(create->priv_drv_data, 1, req.priv_drv_data,
                          req.priv_drv_data_size) < 0) {
            ret = -EFAULT;
            goto createhwqueue_done;
        }
        if (req.priv_drv_data_size != 0)
            hvdxg_save_priv_head(hvdxg.createhwqueue_last_priv_head,
                                 sizeof(hvdxg.createhwqueue_last_priv_head),
                                 &hvdxg.createhwqueue_last_priv_head_len,
                                 create->priv_drv_data,
                                 req.priv_drv_data_size);
        ret = hvdxg_send_sync_vgpu(create, command_len, command_buf,
                                   command_len, &actual_len);
        hvdxg.createhwqueue_last_len = actual_len;
        hvdxg.createhwqueue_last_ret = ret;
        hvdxg.createhwqueue_last_status =
            actual_len >= sizeof(create->status) ? create->status.v : 0;
        if (ret == 0 && actual_len >= sizeof(create->status))
            ret = hvdxg_ntstatus_to_errno(create->status);
        if (ret != 0) {
            hvdxg.createhwqueue_last_ret = ret;
            goto createhwqueue_done;
        }
        if (actual_len < sizeof(*create) - 1 || create->hwqueue.v == 0) {
            ret = -EIO;
            hvdxg.createhwqueue_last_ret = ret;
            goto createhwqueue_done;
        }
        req.queue.v = create->hwqueue.v;
        req.queue_progress_fence.v = create->hwqueue_progress_fence.v;
        req.queue_progress_fence_gpu_va =
            create->hwqueue_progress_fence_gpuva;
        req.queue_progress_fence_cpu_va =
            create->hwqueue_progress_fence_cpuva != 0 ?
            hvdxg_map_iospace_user_canonical(
                HV_DXG_FENCE_SOURCE_HWQUEUE,
                create->hwqueue_progress_fence_cpuva, PGSIZE,
                0, &fence_pa, NULL, 1) : 0;
        if (create->hwqueue_progress_fence_cpuva != 0)
            fence_kva = hvdxg_map_iospace_kernel_canonical(
                HV_DXG_FENCE_SOURCE_HWQUEUE,
                create->hwqueue_progress_fence_cpuva, PGSIZE, fence_pa, 1);
        hvdxg.createhwqueue_last_queue = req.queue.v;
        hvdxg.createhwqueue_last_fence = req.queue_progress_fence.v;
        hvdxg.createhwqueue_last_fence_cpu = req.queue_progress_fence_cpu_va;
        hvdxg.createhwqueue_last_fence_gpu = req.queue_progress_fence_gpu_va;
        if (create->hwqueue_progress_fence_cpuva != 0 &&
            req.queue_progress_fence_cpu_va == 0) {
            ret = -ENOMEM;
            hvdxg.createhwqueue_last_ret = ret;
            goto createhwqueue_done;
        }
        if (req.priv_drv_data_size != 0 &&
            actual_len >= sizeof(*create) + req.priv_drv_data_size - 1)
            hvdxg_save_priv_head(hvdxg.createhwqueue_last_priv_head,
                                 sizeof(hvdxg.createhwqueue_last_priv_head),
                                 &hvdxg.createhwqueue_last_priv_head_len,
                                 create->priv_drv_data,
                                 req.priv_drv_data_size);
        if (req.priv_drv_data_size != 0 &&
            actual_len >= sizeof(*create) + req.priv_drv_data_size - 1 &&
            either_copyout(1, req.priv_drv_data, create->priv_drv_data,
                           req.priv_drv_data_size) < 0) {
            ret = -EFAULT;
            goto createhwqueue_done;
        }
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        hvdxg.createhwqueue_last_ret = ret;
        if (ret == 0 && owner != NULL) {
            uint32 device = hvdxg_owner_object_device(
                owner, HV_DXG_OBJECT_CONTEXT, req.context.v);

            hvdxg_track_hwqueue(owner, req.context.v, device, req.queue.v,
                                req.queue_progress_fence.v);
            hvdxg_track_sync(owner, device, req.queue_progress_fence.v,
                             _D3DDDI_MONITORED_FENCE,
                             0, req.queue_progress_fence.v,
                             req.queue_progress_fence_cpu_va, fence_kva, 1);
        }
createhwqueue_done:
        if (command_buf)
            kvfree(command_buf);
        break;
    }

    case LX_DXDESTROYHWQUEUE: {
        struct d3dkmt_destroyhwqueue req;
        struct hvdxg_command_destroyhwqueue destroy;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.queue.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_hwqueue(owner, req.queue.v)) {
            ret = -EPERM;
            break;
        }
        memset(&destroy, 0, sizeof(destroy));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(&destroy.hdr,
                                        HV_DXGK_VMBCOMMAND_DESTROYHWQUEUE,
                                        hvdxg.dxg_process);
        destroy.hwqueue.v = req.queue.v;
        ret = hvdxg_send_sync_vgpu(&destroy, sizeof(destroy), &status,
                                   sizeof(status), &actual_len);
        if (ret == 0 && actual_len < sizeof(status))
            ret = -EOVERFLOW;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.destroyhwqueue_last_len = actual_len;
        hvdxg.destroyhwqueue_last_ret = ret;
        if (ret == 0 && owner != NULL) {
            uint32 sync = hvdxg_untrack_hwqueue(owner, req.queue.v);
            if (sync != 0)
                hvdxg_untrack_sync(owner, sync);
        }
        break;
    }

    case LX_DXDESTROYCONTEXT: {
        struct d3dkmt_destroycontext req;
        struct hvdxg_command_destroycontext destroy;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.context.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_context(owner, req.context.v)) {
            ret = -EPERM;
            break;
        }
        memset(&destroy, 0, sizeof(destroy));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(&destroy.hdr,
                                        HV_DXGK_VMBCOMMAND_DESTROYCONTEXT,
                                        hvdxg.dxg_process);
        destroy.context.v = req.context.v;
        ret = hvdxg_send_sync_vgpu(&destroy, sizeof(destroy), &status,
                                   sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.destroycontext_last_status = status.v;
        if (ret == 0 && actual_len < sizeof(status))
            ret = -EOVERFLOW;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.destroycontext_last_len = actual_len;
        hvdxg.destroycontext_last_ret = ret;
        if (ret == 0 && owner != NULL) {
            hvdxg_untrack_object(owner, HV_DXG_OBJECT_CONTEXT,
                                 req.context.v);
            hvdxg_untrack_u32(owner->contexts, &owner->context_count,
                              req.context.v);
        }
        break;
    }

    case LX_DXESCAPE: {
        struct d3dkmt_escape req;
        uint8 command_buf[sizeof(struct hvdxg_command_escape) +
                          HV_DXG_IOCTL_PRIVATE_MAX - 1];
        uint8 result_buf[HV_DXG_IOCTL_PRIVATE_MAX];
        struct hvdxg_command_escape *escape =
            (struct hvdxg_command_escape *)command_buf;
        uint32 command_len;
        uint32 actual_len = 0;
        uint32 host_adapter;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.escape_last_len = 0;
        hvdxg.escape_last_ret = 0;
        hvdxg.escape_last_type = (uint32)req.type;
        hvdxg.escape_last_flags = req.flags.value;
        hvdxg.escape_last_size = req.priv_drv_data_size;
        if (hvdxg_resolve_adapter_handle(owner, req.adapter.v,
                                         &host_adapter) != 0 ||
            req.priv_drv_data_size > HV_DXG_IOCTL_PRIVATE_MAX ||
            (req.priv_drv_data_size != 0 && req.priv_drv_data == 0)) {
            ret = -EINVAL;
            hvdxg.escape_last_ret = ret;
            break;
        }
        memset(command_buf, 0, sizeof(command_buf));
        memset(result_buf, 0, sizeof(result_buf));
        hvdxg_command_vgpu_init_process(
            &escape->hdr, HV_DXGK_VMBCOMMAND_ESCAPE, hvdxg.dxg_process);
        escape->adapter.v = host_adapter;
        escape->device.v = req.device.v;
        escape->type = req.type;
        escape->flags = req.flags;
        escape->priv_drv_data_size = req.priv_drv_data_size;
        escape->context.v = req.context.v;
        if (req.priv_drv_data_size != 0 &&
            either_copyin(escape->priv_drv_data, 1, req.priv_drv_data,
                          req.priv_drv_data_size) < 0) {
            ret = -EFAULT;
            break;
        }
        command_len = sizeof(*escape) - 1 + req.priv_drv_data_size;
        ret = hvdxg_send_sync_vgpu(escape, command_len, result_buf,
                                   req.priv_drv_data_size, &actual_len);
        if (ret == 0 && req.priv_drv_data_size != 0 && actual_len != 0) {
            uint32 copy_len = actual_len;
            if (copy_len > req.priv_drv_data_size)
                copy_len = req.priv_drv_data_size;
            ret = either_copyout(1, req.priv_drv_data, result_buf,
                                 copy_len) < 0 ? -EFAULT : 0;
        }
        hvdxg.escape_last_len = actual_len;
        hvdxg.escape_last_ret = ret;
        break;
    }

    case LX_DXSHAREOBJECTS: {
        struct d3dkmt_shareobjects req;
        struct hvdxg_d3dkmthandle object;
        struct hvdxg_shared_object *shared = NULL;
        struct hvdxg_tracked_resource *resource = NULL;
        uint32 host_nt_handle = 0;
        uint32 device = 0;
        uint32 kind = 0;
        uint32 current_process = 0;
        uint32 current_generation = 0;
        uint32 process = 0;
        uint32 host_object = 0;
        uint32 fallback_object = 0;
        uint32 used_host_object = 0;
        int fd = -1;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        current_process = hvdxg_owner_bound_process_handle(owner).v;
        current_generation = hvdxg_open_process_generation(owner);
        process = current_process;
        memset(&req, 0, sizeof(req));
        memset(&object, 0, sizeof(object));
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.sharedhandle_last_cmd = (uint32)cmd;
        hvdxg.sharedhandle_last_device = 0;
        hvdxg.sharedhandle_last_object = 0;
        hvdxg.sharedhandle_last_nt_handle = 0;
        hvdxg.sharedhandle_last_count = req.object_count;
        hvdxg.sharedhandle_last_global_share = 0;
        hvdxg.sharedhandle_last_runtime_d3d12_flags = 0;
        hvdxg.sharedhandle_last_kind = 0;
        hvdxg.sharedhandle_last_fops_kind = 0;
        hvdxg.sharedhandle_last_raw_device = 0;
        hvdxg.sharedhandle_last_raw_object = 0;
        hvdxg.sharedhandle_last_host_device = 0;
        hvdxg.sharedhandle_last_host_device_found = 0;
        hvdxg.sharedhandle_last_host_object = 0;
        hvdxg.sharedhandle_last_object_found = 0;
        hvdxg.sharedhandle_last_raw_resource_found = 0;
        hvdxg.sharedhandle_last_raw_allocation_found = 0;
        hvdxg.sharedhandle_last_raw_sync_found = 0;
        hvdxg.sharedhandle_last_parent = 0;
        hvdxg.sharedhandle_last_current_process = current_process;
        hvdxg.sharedhandle_last_current_generation = current_generation;
        hvdxg.sharedhandle_last_creator_process = 0;
        hvdxg.sharedhandle_last_creator_generation = 0;
        hvdxg.sharedhandle_last_owner_process = 0;
        hvdxg.sharedhandle_last_owner_generation = 0;
        hvdxg.sharedhandle_last_owner_refs = 0;
        hvdxg.sharedhandle_last_owner_used = 0;
        hvdxg.sharedhandle_last_object_type = HV_DXG_OBJECT_NONE;
        hvdxg.sharedhandle_last_object_device = 0;
        hvdxg.sharedhandle_last_allocation = 0;
        hvdxg.sharedhandle_last_allocation_found = 0;
        hvdxg.sharedhandle_last_allocation_owner_process = 0;
        hvdxg.sharedhandle_last_allocation_owner_generation = 0;
        hvdxg.sharedhandle_last_allocation_owner_refs = 0;
        hvdxg.sharedhandle_last_map_va = 0;
        hvdxg.sharedhandle_last_map_pages = 0;
        hvdxg.sharedhandle_last_map_fence = 0;
        hvdxg.sharedhandle_last_map_ret = 0;
        hvdxg.sharedhandle_last_map_status = 0;
        hvdxg.sharedhandle_last_resident_paging_queue = 0;
        hvdxg.sharedhandle_last_resident_sync = 0;
        hvdxg.sharedhandle_last_resident_fence = 0;
        hvdxg.sharedhandle_last_resident_current = 0;
        hvdxg.sharedhandle_last_resident_wait_result = 0;
        hvdxg.sharedhandle_last_resident_wait_ret = 0;
        hvdxg.sharedhandle_last_resident_missing = 0;
        hvdxg.sharedhandle_last_resident_enforced = 0;
        hvdxg.sharedhandle_last_create_flags = 0;
        hvdxg.sharedhandle_last_alloc_count = 0;
        hvdxg.sharedhandle_last_sealed = 0;
        hvdxg.sharedhandle_last_sync_type = 0;
        hvdxg.sharedhandle_last_sync_flags = 0;
        hvdxg.sharedhandle_last_sync_global = 0;
        hvdxg.sharedhandle_last_sync_monitor_fence = 0;
        hvdxg.sharedhandle_last_sync_fence_cpu = 0;
        hvdxg.sharedhandle_last_sync_fence_kva = 0;
        hvdxg.sharedsync_export_fd = 0;
        hvdxg.sharedsync_export_ret = 0;
        hvdxg.sharedsync_export_cloexec = 0;
        hvdxg.sharedsync_export_fops_kind = 0;
        hvdxg.sharedsync_export_fd_kind = 0;
        hvdxg.sharedsync_export_device = 0;
        hvdxg.sharedsync_export_host_device = 0;
        hvdxg.sharedsync_export_object = 0;
        hvdxg.sharedsync_export_host_object = 0;
        hvdxg.sharedsync_export_sync_type = 0;
        hvdxg.sharedsync_export_sync_flags = 0;
        hvdxg.sharedsync_export_monitor_fence = 0;
        hvdxg.sharedsync_export_global_share = 0;
        hvdxg.sharedsync_export_global_zero = 0;
        hvdxg.sharedsync_export_host_shared_handle = 0;
        hvdxg.sharedsync_export_host_nt_handle = 0;
        hvdxg.sharedsync_export_nt_refs = 0;
        hvdxg.sharedsync_export_shared_owner_object = 0;
        hvdxg.sharedsync_export_cache_process = 0;
        hvdxg.sharedsync_export_owner_process = 0;
        hvdxg.sharedsync_export_owner_generation = 0;
        hvdxg.sharedsync_export_owner_refs = 0;
        hvdxg.shareobjects_last_desired_access = req.desired_access;
        hvdxg.shareobjects_last_object_attr = req.object_attr;
        hvdxg.shareobjects_last_attr_len = 0;
        hvdxg.shareobjects_last_attr_ret = 0;
        memset(hvdxg.shareobjects_last_attr_head, 0,
               sizeof(hvdxg.shareobjects_last_attr_head));
        if (req.object_attr != 0) {
            hvdxg.shareobjects_last_attr_len =
                sizeof(hvdxg.shareobjects_last_attr_head);
            if (either_copyin(hvdxg.shareobjects_last_attr_head, 1,
                              req.object_attr,
                              sizeof(hvdxg.shareobjects_last_attr_head)) < 0)
                hvdxg.shareobjects_last_attr_ret = -EFAULT;
        }
        hvdxg.sharedresource_owner_exists = 0;
        hvdxg.sharedresource_owner_cached = 0;
        hvdxg.sharedresource_owner_reused = 0;
        hvdxg.sharedresource_owner_nt = 0;
        hvdxg.sharedresource_owner_refs = 0;
        hvdxg.sharedresource_owner_sealed = 0;
        hvdxg.sharedresource_owner_object = 0;
        hvdxg.sharedresource_owner_process = 0;
        hvdxg.sharedresource_open_global = 0;
        hvdxg.sharedresource_pre_nt_sealable = 0;
        hvdxg.sharedresource_pre_nt_seal_ret = 0;
        hvdxg.sharedresource_pre_nt_seal_applied = 0;
        hvdxg.sharedresource_pre_nt_seal_before = 0;
        hvdxg.sharedresource_pre_nt_seal_after = 0;
        hvdxg.sharedresource_pre_nt_seal_actual_ret = 0;
        hvdxg.sharedresource_nt_resource_host = 0;
        hvdxg.sharedresource_nt_alloc_host = 0;
        hvdxg.sharedresource_nt_resource_flags = 0;
        hvdxg.sharedresource_nt_alloc_flags = 0;
        hvdxg.sharedresource_nt_alloc_in_hash = 0;
        hvdxg.sharedresource_nt_alloc_out_hash = 0;
        hvdxg.sharedresource_nt_alloc_size = 0;
        hvdxg.sharedresource_nt_meta_before = 0;
        hvdxg.sharedresource_nt_meta_after = 0;
        hvdxg.sharedresource_nt_seal_before = 0;
        hvdxg.sharedresource_nt_seal_after = 0;
        hvdxg.sharedresource_nt_host_seal_before = 0;
        hvdxg.sharedresource_nt_host_seal_after = 0;
        hvdxg_note_shared_resource_metadata(NULL);
        hvdxg_reset_shared_resource_record();
        hvdxg.shareobject_last_len = 0;
        hvdxg.shareobject_last_cmd_len = 0;
        hvdxg.shareobject_last_wire_len = 0;
        hvdxg.shareobject_last_ext = 0;
        hvdxg.shareobject_last_ext_offset = 0;
        hvdxg.shareobject_last_device_offset = 0;
        hvdxg.shareobject_last_object_offset = 0;
        hvdxg.shareobject_last_result_len = 0;
        hvdxg.shareobject_last_head_len = 0;
        memset(hvdxg.shareobject_last_head, 0,
               sizeof(hvdxg.shareobject_last_head));
        hvdxg.shareobject_last_completion_type = 0;
        hvdxg.shareobject_last_completion_len = 0;
        memset(hvdxg.shareobject_last_completion_prefix, 0,
               sizeof(hvdxg.shareobject_last_completion_prefix));
        hvdxg.shareobject_last_ret = 0;
        hvdxg.shareobject_last_status = 0;
        hvdxg.shareobject_last_process = process;
        hvdxg.shareobject_last_device = 0;
        hvdxg.shareobject_last_object = 0;
        hvdxg.shareobject_last_reserved = 0;
        hvdxg.shareobject_last_nt_handle = 0;
        hvdxg.shareobject_diag_attempted = 0;
        hvdxg.shareobject_diag_valid_nt = 0;
        hvdxg.shareobject_diag_kind = 0;
        hvdxg.shareobject_diag_reason = 0;
        hvdxg.ntshared_obj_found = 0;
        hvdxg.ntshared_obj_exact = 0;
        hvdxg.ntshared_obj_local = 0;
        hvdxg.ntshared_obj_host = 0;
        hvdxg.ntshared_obj_type = HV_DXG_OBJECT_NONE;
        hvdxg.ntshared_obj_parent = 0;
        hvdxg.ntshared_obj_device = 0;
        hvdxg.ntshared_obj_owner_process = 0;
        hvdxg.ntshared_obj_owner_generation = 0;
        hvdxg.ntshared_obj_generation = 0;
        hvdxg.ntshared_obj_stale = 0;
        hvdxg.ntshared_obj_destroyed = 0;
        hvdxg_reset_ntshared_runtime_diag();
        if (req.object_count != 1 || req.objects == 0 ||
            req.shared_handle == 0) {
            ret = -EINVAL;
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        if (either_copyin(&object, 1, req.objects, sizeof(object)) < 0) {
            ret = -EFAULT;
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        hvdxg.sharedhandle_last_object = object.v;
        hvdxg.sharedhandle_last_raw_object = object.v;
        hvdxg.sharedhandle_last_raw_resource_found =
            hvdxg_owner_find_object(owner, HV_DXG_OBJECT_RESOURCE,
                                    object.v) != NULL ? 1 : 0;
        hvdxg.sharedhandle_last_raw_allocation_found =
            hvdxg_owner_find_object(owner, HV_DXG_OBJECT_ALLOCATION,
                                    object.v) != NULL ? 1 : 0;
        hvdxg.sharedhandle_last_raw_sync_found =
            hvdxg_owner_find_object(owner, HV_DXG_OBJECT_SYNC,
                                    object.v) != NULL ? 1 : 0;
        if (object.v == 0) {
            ret = -EINVAL;
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        resource = hvdxg_owner_find_resource(owner, 0, object.v);
        if (resource != NULL) {
            hvdxg.sharedhandle_last_host_object =
                hvdxg_owner_host_object_handle(
                    owner, HV_DXG_OBJECT_RESOURCE, object.v,
                    &hvdxg.sharedhandle_last_parent);
            hvdxg.sharedhandle_last_object_found =
                hvdxg.sharedhandle_last_host_object != 0 ? 1 : 0;
            hvdxg.sharedhandle_last_object_type =
                HV_DXG_OBJECT_RESOURCE;
            hvdxg.sharedhandle_last_object_device =
                hvdxg_owner_object_device(owner, HV_DXG_OBJECT_RESOURCE,
                                          object.v);
            if (hvdxg.sharedhandle_last_host_object == 0) {
                ret = -EINVAL;
                hvdxg.sharedhandle_last_ret = ret;
                break;
            }
            ret = hvdxg_prepare_resource_nt_metadata(owner, resource);
            if (ret != 0) {
                hvdxg.sharedhandle_last_ret = ret;
                break;
            }
            kind = HV_DXG_SHARED_OBJECT_RESOURCE;
            device = resource->device;
            hvdxg.sharedhandle_last_host_device =
                hvdxg_owner_host_object_handle(
                    owner, HV_DXG_OBJECT_DEVICE, resource->device, NULL);
            hvdxg.sharedhandle_last_host_device_found =
                hvdxg.sharedhandle_last_host_device != 0 ? 1 : 0;
            if (hvdxg.sharedhandle_last_host_device == 0)
                hvdxg.sharedhandle_last_host_device = resource->device;
            hvdxg.sharedhandle_last_device = device;
            hvdxg.sharedhandle_last_global_share = resource->global_share;
            hvdxg.sharedresource_owner_exists =
                resource->shared_metadata_created;
            hvdxg.sharedresource_owner_cached =
                resource->host_shared_handle_nt != 0 ? 1 : 0;
            hvdxg.sharedresource_owner_nt =
                resource->host_shared_handle_nt;
            hvdxg.sharedresource_owner_refs = resource->host_shared_refs;
            hvdxg.sharedresource_owner_sealed =
                resource->host_shared_sealed;
            hvdxg.sharedresource_owner_object =
                resource->host_shared_object;
            hvdxg.sharedresource_owner_process =
                resource->host_shared_process;
            hvdxg_note_shared_resource_metadata(resource);
            hvdxg.sharedhandle_last_runtime_d3d12_flags =
                resource->runtime_d3d12_flags;
            hvdxg.sharedhandle_last_create_flags =
                resource->create_flags_value;
            hvdxg.sharedhandle_last_alloc_count =
                resource->allocation_count;
            if (resource->allocation_count != 0)
                hvdxg.sharedhandle_last_allocation =
                    resource->allocation_handles[0];
            {
                struct hvdxg_tracked_allocation *a = NULL;

                if (hvdxg.sharedhandle_last_allocation != 0)
                    a = hvdxg_owner_find_allocation(
                        owner, resource->device, resource->resource,
                        hvdxg.sharedhandle_last_allocation);
                if (a == NULL) {
                    hvdxg.sharedhandle_last_allocation =
                        hvdxg_owner_first_allocation(
                            owner, resource->device, resource->resource,
                            &hvdxg.sharedhandle_last_allocation_found);
                    if (hvdxg.sharedhandle_last_allocation != 0)
                        a = hvdxg_owner_find_allocation(
                            owner, resource->device, resource->resource,
                            hvdxg.sharedhandle_last_allocation);
                }

                if (a != NULL) {
                    hvdxg.sharedhandle_last_allocation_found = 1;
                    if (a->owner_process == 0) {
                        a->owner_process = resource->owner_process;
                        a->owner_generation =
                            resource->owner_generation;
                        a->owner_refs = resource->owner_refs;
                    }
                    hvdxg.sharedhandle_last_allocation_owner_process =
                        a->owner_process;
                    hvdxg.sharedhandle_last_allocation_owner_generation =
                        a->owner_generation;
                    hvdxg.sharedhandle_last_allocation_owner_refs =
                        a->owner_refs;
                    hvdxg.allocation_last_owner_process =
                        a->owner_process;
                    hvdxg.allocation_last_owner_generation =
                        a->owner_generation;
                    ret = hvdxg_prepare_allocation_for_share(
                        owner, resource, a);
                    if (ret != 0)
                        hvdxg.sharedhandle_last_ret = ret;
                }
            }
            hvdxg.sharedresource_pre_nt_seal_ret =
                resource->shared_metadata_created &&
                hvdxg.sharedhandle_last_host_object != 0 ? 0 : -EINVAL;
            hvdxg.sharedresource_pre_nt_sealable =
                hvdxg.sharedresource_pre_nt_seal_ret == 0 ? 1 : 0;
            if (ret != 0)
                break;
            hvdxg.sharedresource_pre_nt_seal_before = resource->sealed;
            hvdxg.sharedresource_pre_nt_seal_applied = 0;
            hvdxg.sharedresource_pre_nt_seal_actual_ret = 0;
            hvdxg.sharedresource_pre_nt_seal_actual_ret =
                hvdxg.sharedresource_pre_nt_seal_ret;
            hvdxg.sharedresource_pre_nt_seal_after = resource->sealed;
            if (resource->existing_sysmem)
                hvdxg_note_existing_sysmem_share(resource, 2);
            hvdxg.sharedhandle_last_owner_process =
                resource->owner_process;
            hvdxg.sharedhandle_last_owner_generation =
                resource->owner_generation;
            hvdxg.sharedhandle_last_owner_refs = resource->owner_refs;
            hvdxg.sharedhandle_last_creator_process =
                resource->owner_process;
            hvdxg.sharedhandle_last_creator_generation =
                resource->owner_generation;
            hvdxg.sharedhandle_last_sealed = resource->sealed;
        } else if (hvdxg_owner_has_sync(owner, object.v)) {
            uint32 sync_flags = hvdxg_owner_sync_flags(owner, object.v);

            hvdxg.sharedhandle_last_sync_type =
                hvdxg_owner_sync_type(owner, object.v);
            hvdxg.sharedhandle_last_sync_flags = sync_flags;
            hvdxg.sharedhandle_last_sync_global =
                hvdxg_owner_sync_global_shared(owner, object.v);
            hvdxg.sharedhandle_last_sync_monitor_fence =
                hvdxg_owner_sync_is_monitor_fence_handle(owner, object.v);
            hvdxg.sharedhandle_last_sync_fence_cpu =
                hvdxg_owner_sync_fence_cpu_va(owner, object.v);
            hvdxg.sharedhandle_last_sync_fence_kva =
                hvdxg_owner_sync_fence_kva(owner, object.v);
            hvdxg.sharedhandle_last_host_object =
                hvdxg_owner_host_object_handle(
                    owner, HV_DXG_OBJECT_SYNC, object.v,
                    &hvdxg.sharedhandle_last_parent);
            hvdxg.sharedhandle_last_object_found =
                hvdxg.sharedhandle_last_host_object != 0 ? 1 : 0;
            hvdxg.sharedhandle_last_object_type = HV_DXG_OBJECT_SYNC;
            hvdxg.sharedhandle_last_object_device =
                hvdxg_owner_object_device(owner, HV_DXG_OBJECT_SYNC,
                                          object.v);
            if ((sync_flags & 0x1U) == 0) {
                ret = -EINVAL;
                hvdxg.sharedhandle_last_ret = ret;
                break;
            }
            kind = HV_DXG_SHARED_OBJECT_SYNC;
            device = hvdxg_owner_sync_device(owner, object.v);
            hvdxg.sharedhandle_last_host_device =
                hvdxg_owner_host_object_handle(
                    owner, HV_DXG_OBJECT_DEVICE, device, NULL);
            hvdxg.sharedhandle_last_host_device_found =
                hvdxg.sharedhandle_last_host_device != 0 ? 1 : 0;
            if (hvdxg.sharedhandle_last_host_device == 0)
                hvdxg.sharedhandle_last_host_device = device;
            hvdxg.sharedhandle_last_device = device;
            hvdxg.sharedhandle_last_global_share =
                hvdxg_owner_sync_global_shared(owner, object.v);
            fallback_object = hvdxg.sharedhandle_last_global_share;
            hvdxg.sharedhandle_last_owner_process =
                hvdxg_owner_sync_owner_process(owner, object.v);
            hvdxg.sharedhandle_last_owner_generation =
                hvdxg_owner_sync_owner_generation(owner, object.v);
            hvdxg.sharedhandle_last_owner_refs =
                hvdxg_owner_sync_owner_refs(owner, object.v);
            hvdxg.sharedhandle_last_creator_process =
                hvdxg.sharedhandle_last_owner_process;
            hvdxg.sharedhandle_last_creator_generation =
                hvdxg.sharedhandle_last_owner_generation;
            hvdxg.sharedsync_export_ret = -EINPROGRESS;
            hvdxg.sharedsync_export_fd_kind = HV_DXG_SHARED_OBJECT_SYNC;
            hvdxg.sharedsync_export_device = device;
            hvdxg.sharedsync_export_host_device =
                hvdxg.sharedhandle_last_host_device;
            hvdxg.sharedsync_export_object = object.v;
            hvdxg.sharedsync_export_host_object =
                hvdxg.sharedhandle_last_host_object;
            hvdxg.sharedsync_export_sync_type =
                hvdxg.sharedhandle_last_sync_type;
            hvdxg.sharedsync_export_sync_flags = sync_flags;
            hvdxg.sharedsync_export_monitor_fence =
                hvdxg.sharedhandle_last_sync_monitor_fence;
            hvdxg.sharedsync_export_global_share =
                hvdxg.sharedhandle_last_sync_global;
            hvdxg.sharedsync_export_global_zero =
                hvdxg.sharedhandle_last_sync_global == 0 ? 1 : 0;
            hvdxg.sharedsync_export_host_shared_handle =
                hvdxg.sharedhandle_last_sync_global;
            hvdxg.sharedsync_export_shared_owner_object =
                hvdxg.sharedhandle_last_host_object;
            hvdxg.sharedsync_export_owner_process =
                hvdxg.sharedhandle_last_owner_process;
            hvdxg.sharedsync_export_owner_generation =
                hvdxg.sharedhandle_last_owner_generation;
            hvdxg.sharedsync_export_owner_refs =
                hvdxg.sharedhandle_last_owner_refs;
        } else {
            ret = -EINVAL;
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        hvdxg.sharedhandle_last_kind = kind;
        if (device == 0) {
            ret = -EINVAL;
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        process = current_process;
        hvdxg.sharedhandle_last_owner_used = 0;
        host_object = hvdxg.sharedhandle_last_host_object != 0 ?
                      hvdxg.sharedhandle_last_host_object : object.v;
        hvdxg.sharedhandle_last_device = device;
        hvdxg.sharedhandle_last_object = host_object;
        if (resource != NULL) {
            uint32 cache_object =
                resource->host_shared_handle_nt != 0 &&
                resource->host_shared_object != 0 ?
                resource->host_shared_object : host_object;

            resource->host_shared_process = process;
            resource->host_shared_object = cache_object;
            if (resource->host_shared_refs == 0)
                resource->host_shared_refs = 1;
            if (resource->sealed)
                resource->host_shared_sealed = resource->sealed;
            if (hvdxg_ntshared_cache_get(kind, process, cache_object,
                                         &host_nt_handle)) {
                host_object = cache_object;
                resource->host_shared_handle_nt = host_nt_handle;
                resource->host_shared_process = process;
                resource->host_shared_object = host_object;
                resource->host_shared_refs = hvdxg.ntshared_cache_last_refs;
                hvdxg.sharedresource_owner_cached = 1;
                hvdxg.sharedresource_owner_reused = 1;
                ret = 0;
            } else {
                struct hvdxg_tracked_allocation *a = NULL;
                struct hvdxg_object_entry *res_entry;
                struct hvdxg_object_entry *alloc_entry = NULL;

                resource->host_shared_process = process;
                resource->host_shared_object = host_object;
                if (resource->host_shared_refs == 0)
                    resource->host_shared_refs = 1;
                resource->host_shared_sealed = resource->sealed;
                if (hvdxg.sharedhandle_last_allocation != 0)
                    a = hvdxg_owner_find_allocation(
                        owner, resource->device, resource->resource,
                        hvdxg.sharedhandle_last_allocation);
                res_entry = hvdxg_owner_find_object(
                    owner, HV_DXG_OBJECT_RESOURCE, resource->resource);
                if (hvdxg.sharedhandle_last_allocation != 0)
                    alloc_entry = hvdxg_owner_find_object(
                        owner, HV_DXG_OBJECT_ALLOCATION,
                        hvdxg.sharedhandle_last_allocation);

                hvdxg_note_ntshared_object_entry(owner, process, host_object);
                hvdxg.ntshared_obj_owner_process =
                    resource->owner_process;
                hvdxg.ntshared_obj_owner_generation =
                    resource->owner_generation;
                hvdxg.sharedresource_nt_resource_host =
                    res_entry != NULL ?
                    (uint32)res_entry->host_handle : host_object;
                hvdxg.sharedresource_nt_alloc_host =
                    alloc_entry != NULL ?
                    (uint32)alloc_entry->host_handle :
                    hvdxg.sharedhandle_last_allocation;
                hvdxg.sharedresource_nt_resource_flags =
                    resource->host_create_flags_value != 0 ?
                    resource->host_create_flags_value :
                    resource->create_flags_value;
                hvdxg.sharedresource_nt_alloc_flags =
                    a != NULL ? a->flags :
                    (resource->allocation_count != 0 ?
                     resource->allocation_flags[0] : 0);
                hvdxg.sharedresource_nt_alloc_in_hash =
                    hvdxg_hash_bytes(hvdxg.d3d12_shared_alloc_priv_head,
                                     hvdxg.d3d12_shared_alloc_priv_head_len);
                hvdxg.sharedresource_nt_alloc_out_hash =
                    hvdxg_hash_bytes(hvdxg.d3d12_shared_alloc_out_priv_head,
                                     hvdxg.d3d12_shared_alloc_out_priv_head_len);
                hvdxg.sharedresource_nt_alloc_size =
                    a != NULL ? a->size :
                    (resource->allocation_count != 0 ?
                     resource->allocation_sizes[0] : 0);
                hvdxg.sharedresource_nt_meta_before =
                    resource->shared_metadata_created;
                hvdxg.sharedresource_nt_seal_before = resource->sealed;
                hvdxg.sharedresource_nt_host_seal_before =
                    resource->host_shared_sealed;
                hvdxg.ntshared_cleanup_cached_before =
                    resource->host_shared_handle_nt != 0 ? 1 : 0;
                hvdxg.ntshared_cleanup_refs_before =
                    resource->host_shared_refs;
                hvdxg.ntshared_cleanup_object_before =
                    resource->host_shared_object;
                hvdxg.ntshared_cleanup_nt_before =
                    resource->host_shared_handle_nt;
                hvdxg.ntshared_cleanup_sealed_before =
                    resource->host_shared_sealed;
                hvdxg.ntshared_cleanup_cache_inserts_before =
                    hvdxg.ntshared_cache_inserts;
                if (resource->host_shared_handle_nt != 0)
                    hvdxg_clear_resource_nt_shared_handle(resource, 0);
                hvdxg_note_ntshared_runtime_resource(
                    owner, object.v, process, host_object, resource, a,
                    res_entry, alloc_entry);
                ret = hvdxg_flush_heap_transitions_process(process);
                if (ret != 0) {
                    hvdxg.ntshared_cleanup_ret = (uint32)ret;
                    hvdxg.sharedhandle_last_ret = ret;
                    break;
                }
                ret = hvdxg_create_nt_shared_object(
                    process, host_object, fallback_object,
                    &used_host_object, &host_nt_handle,
                    resource->create_flags_value == 0x47);
                hvdxg_note_ntshared_runtime_return(resource, ret,
                                                   host_nt_handle);
                hvdxg.sharedresource_nt_meta_after =
                    resource->shared_metadata_created;
                hvdxg.sharedresource_nt_seal_after = resource->sealed;
                hvdxg.sharedresource_nt_host_seal_after =
                    resource->host_shared_sealed;
                if (resource->existing_sysmem)
                    hvdxg_note_existing_sysmem_share(resource, 3);
                hvdxg.ntshared_cleanup_ret = (uint32)ret;
                hvdxg.ntshared_cleanup_cached_after =
                    resource->host_shared_handle_nt != 0 ? 1 : 0;
                hvdxg.ntshared_cleanup_refs_after =
                    resource->host_shared_refs;
                hvdxg.ntshared_cleanup_object_after =
                    resource->host_shared_object;
                hvdxg.ntshared_cleanup_nt_after =
                    resource->host_shared_handle_nt;
                hvdxg.ntshared_cleanup_sealed_after =
                    resource->host_shared_sealed;
                hvdxg.ntshared_cleanup_cache_inserts_after =
                    hvdxg.ntshared_cache_inserts;
                if (ret == 0) {
                    if (used_host_object != 0)
                        host_object = used_host_object;
                    resource->host_shared_handle_nt = host_nt_handle;
                    resource->host_shared_process = process;
                    resource->host_shared_object = host_object;
                    resource->host_shared_refs = 1;
                    hvdxg_ntshared_cache_insert(kind, process, host_object,
                                                host_nt_handle);
                }
            }
        } else if (!hvdxg_ntshared_cache_get(kind, process, host_object,
                                             &host_nt_handle)) {
            if (fallback_object != 0 &&
                hvdxg_ntshared_cache_get(kind, process, fallback_object,
                                         &host_nt_handle)) {
                host_object = fallback_object;
                ret = 0;
            } else {
                hvdxg_note_ntshared_object_entry(owner, process, host_object);
                hvdxg.ntshared_obj_owner_process =
                    hvdxg_owner_sync_owner_process(owner, object.v);
                hvdxg.ntshared_obj_owner_generation =
                    hvdxg_owner_sync_owner_generation(owner, object.v);
                ret = hvdxg_create_nt_shared_object(
                    process, host_object, fallback_object,
                    &used_host_object, &host_nt_handle, 0);
                if (ret == 0 && used_host_object != 0)
                    host_object = used_host_object;
            }
            if (ret == 0)
                hvdxg_ntshared_cache_insert(kind, process, host_object,
                                            host_nt_handle);
        } else {
            ret = 0;
        }
        if (kind == HV_DXG_SHARED_OBJECT_SYNC) {
            hvdxg.sharedsync_export_host_object = host_object;
            hvdxg.sharedsync_export_host_nt_handle = host_nt_handle;
            hvdxg.sharedsync_export_shared_owner_object = host_object;
            hvdxg.sharedsync_export_cache_process = process;
            hvdxg.sharedsync_export_nt_refs =
                hvdxg_ntshared_cache_refs(kind, process, host_object,
                                          host_nt_handle);
            hvdxg.sharedsync_export_ret = ret;
        }
        hvdxg.sharedhandle_last_object = host_object;
        if (resource != NULL) {
            hvdxg.sharedresource_owner_exists =
                resource->shared_metadata_created;
            hvdxg.sharedresource_owner_cached =
                resource->host_shared_handle_nt != 0 ? 1 : 0;
            hvdxg.sharedresource_owner_nt =
                resource->host_shared_handle_nt;
            hvdxg.sharedresource_owner_refs = resource->host_shared_refs;
            hvdxg.sharedresource_owner_sealed =
                resource->host_shared_sealed;
            hvdxg.sharedresource_owner_object =
                resource->host_shared_object;
            hvdxg.sharedresource_owner_process =
                resource->host_shared_process;
        }
        if (ret != 0) {
            if (hv_cmdline_enabled("dxg_shareobject_diagnostic")) {
                hvdxg_share_object_with_host_diagnostic(
                    process,
                    hvdxg.sharedhandle_last_host_device != 0 ?
                        hvdxg.sharedhandle_last_host_device : device,
                    host_object, kind, 1);
            } else {
                hvdxg.shareobject_diag_attempted = 0;
                hvdxg.shareobject_diag_kind = kind;
                hvdxg.shareobject_diag_reason = 2;
            }
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        if (resource != NULL) {
            hvdxg_note_shared_resource_record(
                "nt-success-preseal", 1, resource, kind, process,
                host_object, host_nt_handle, host_nt_handle, 0,
                object.v == resource->resource ? 1 : 0);
            ret = resource->sealed ? 0 :
                  hvdxg_seal_resource_from_owner(owner, resource);
            hvdxg.sharedhandle_last_sealed = resource->sealed;
            if (ret == 0) {
                resource->host_shared_sealed = resource->sealed;
                hvdxg.sharedresource_owner_sealed =
                    resource->host_shared_sealed;
                hvdxg.sharedresource_nt_seal_after = resource->sealed;
                hvdxg.sharedresource_nt_host_seal_after =
                    resource->host_shared_sealed;
            }
            hvdxg.ntshared_runtime_sealed_after = resource->sealed;
            hvdxg.ntshared_runtime_host_sealed_after =
                resource->host_shared_sealed;
            if (ret == 0) {
                hvdxg.sharedresource_record_seal_before_fd =
                    resource->sealed;
                hvdxg_note_shared_resource_record(
                    "sealed-before-fd", 2, resource, kind, process,
                    host_object, host_nt_handle, host_nt_handle, 0,
                    object.v == resource->resource ? 1 : 0);
            }
            if (resource->existing_sysmem)
                hvdxg_note_existing_sysmem_share(resource, 4);
            if (ret != 0) {
                uint32 destroy_handle = 0;

                destroy_handle = hvdxg_release_nt_shared_object_ref(
                    kind, process, host_object, host_nt_handle);
                if (resource != NULL && destroy_handle != 0)
                    hvdxg_clear_resource_nt_shared_handle(resource,
                                                          destroy_handle);
                hvdxg.sharedhandle_last_ret = ret;
                break;
            }
        }
        shared = kvmalloc(sizeof(*shared));
        if (shared == NULL) {
            uint32 destroy_handle = 0;

            destroy_handle = hvdxg_release_nt_shared_object_ref(
                kind, process, host_object, host_nt_handle);
            if (resource != NULL && destroy_handle != 0)
                hvdxg_clear_resource_nt_shared_handle(resource, destroy_handle);
            ret = -ENOMEM;
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        memset(shared, 0, sizeof(*shared));
        shared->kind = kind;
        shared->device = device;
        shared->object = object.v;
        shared->cache_process = process;
        shared->cache_object = host_object;
        if (kind == HV_DXG_SHARED_OBJECT_SYNC)
            shared->global_share =
                hvdxg_owner_sync_global_shared(owner, object.v);
        else
            shared->global_share = host_nt_handle;
        shared->host_nt_handle = host_nt_handle;
        shared->nt_handle = host_nt_handle;
        hvdxg.sharedresource_open_global =
            kind == HV_DXG_SHARED_OBJECT_RESOURCE ? shared->global_share : 0;
        if (kind == HV_DXG_SHARED_OBJECT_SYNC) {
            if (shared->global_share == 0) {
                hvdxg.sharedsync_export_global_zero = 1;
                hvdxg.sharedsync_export_ret = -EINVAL;
                (void)hvdxg_release_nt_shared_object_ref(
                    kind, process, host_object, host_nt_handle);
                kvfree(shared);
                ret = -EINVAL;
                hvdxg.sharedhandle_last_ret = ret;
                break;
            }
            shared->sync_type = hvdxg_owner_sync_type(owner, object.v);
            shared->sync_flags = hvdxg_owner_sync_flags(owner, object.v);
            shared->sync_owner_process =
                hvdxg_owner_sync_owner_process(owner, object.v);
            shared->sync_owner_generation =
                hvdxg_owner_sync_owner_generation(owner, object.v);
            shared->sync_owner_refs =
                hvdxg_owner_sync_owner_refs(owner, object.v);
            hvdxg.sharedsync_export_fd_kind = shared->kind;
            hvdxg.sharedsync_export_fops_kind = HV_DXG_SHARED_FOPS_SYNC;
            hvdxg.sharedsync_export_sync_type = shared->sync_type;
            hvdxg.sharedsync_export_global_share = shared->global_share;
            hvdxg.sharedsync_export_global_zero =
                shared->global_share == 0 ? 1 : 0;
            hvdxg.sharedsync_export_host_shared_handle =
                shared->global_share;
            hvdxg.sharedsync_export_host_nt_handle =
                shared->host_nt_handle;
            hvdxg.sharedsync_export_cache_process =
                shared->cache_process;
            hvdxg.sharedsync_export_shared_owner_object =
                shared->cache_object;
        } else {
            ret = hvdxg_clone_resource(&shared->resource, resource);
            if (ret != 0) {
                uint32 destroy_handle = 0;

                destroy_handle = hvdxg_release_nt_shared_object_ref(
                    kind, process, host_object, host_nt_handle);
                if (resource != NULL && destroy_handle != 0)
                    hvdxg_clear_resource_nt_shared_handle(resource,
                                                          destroy_handle);
                kvfree(shared);
                hvdxg.sharedhandle_last_ret = ret;
                break;
            }
        }
        fd = vfs_custom_fd_alloc(
            kind == HV_DXG_SHARED_OBJECT_SYNC ?
                &hvdxg_shared_sync_file_ops :
                &hvdxg_shared_resource_file_ops,
            shared, O_RDWR);
        hvdxg.sharedhandle_last_fops_kind =
            kind == HV_DXG_SHARED_OBJECT_SYNC ?
                HV_DXG_SHARED_FOPS_SYNC :
                HV_DXG_SHARED_FOPS_RESOURCE;
        if (kind == HV_DXG_SHARED_OBJECT_SYNC) {
            hvdxg.sharedsync_export_fd = fd < 0 ? 0 : (uint32)fd;
            hvdxg.sharedsync_export_ret = fd < 0 ? fd : 0;
            hvdxg.sharedsync_export_nt_refs =
                hvdxg_ntshared_cache_refs(shared->kind,
                                          shared->cache_process,
                                          shared->cache_object,
                                          shared->host_nt_handle);
        }
        if (fd >= 0 && current != NULL && current->fdtable != NULL) {
            spin_lock(&current->fdtable->lock);
            if (vfs_fdtable_set_fdflags(current->fdtable, fd,
                                        FD_CLOEXEC) == 0 &&
                kind == HV_DXG_SHARED_OBJECT_SYNC)
                hvdxg.sharedsync_export_cloexec = 1;
            spin_unlock(&current->fdtable->lock);
        }
        if (fd < 0) {
            uint32 destroy_handle = 0;

            destroy_handle = hvdxg_release_nt_shared_object_ref(
                kind, process, host_object, host_nt_handle);
            if (resource != NULL && destroy_handle != 0)
                hvdxg_clear_resource_nt_shared_handle(resource, destroy_handle);
            if (kind == HV_DXG_SHARED_OBJECT_RESOURCE)
                hvdxg_free_tracked_resource(&shared->resource);
            kvfree(shared);
            ret = fd;
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        {
            uint64 fd64 = (uint64)fd;

            ret = either_copyout(1, req.shared_handle, &fd64,
                                 sizeof(fd64)) < 0 ? -EFAULT : 0;
        }
        if (ret != 0) {
            struct vfs_file *f;

            hvdxg.sharedhandle_copyout_failures++;
            hvdxg.sharedhandle_copyout_last_kind = kind;
            hvdxg.sharedhandle_copyout_last_process = process;
            hvdxg.sharedhandle_copyout_last_object = host_object;
            hvdxg.sharedhandle_copyout_last_nt = host_nt_handle;
            hvdxg.sharedhandle_copyout_last_fd = (uint32)fd;
            hvdxg.sharedhandle_copyout_last_reclaimed = 0;
            hvdxg.sharedhandle_copyout_last_ret = ret;
            if (kind == HV_DXG_SHARED_OBJECT_SYNC)
                hvdxg.sharedsync_export_ret = ret;
            spin_lock(&current->fdtable->lock);
            f = vfs_fdtable_dealloc_fd(current->fdtable, fd);
            spin_unlock(&current->fdtable->lock);
            if (f != NULL) {
                hvdxg.sharedhandle_copyout_last_reclaimed = 1;
                vfs_file_maybe_last_fd_close(f);
                vfs_fput(f);
            }
            hvdxg.sharedhandle_copyout_last_refs_after =
                hvdxg_ntshared_cache_refs(kind, process, host_object,
                                          host_nt_handle);
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        hvdxg.sharedhandle_last_device = device;
        hvdxg.sharedhandle_last_nt_handle = host_nt_handle;
        hvdxg.sharedhandle_last_global_share = shared->global_share;
        if (kind == HV_DXG_SHARED_OBJECT_RESOURCE) {
            hvdxg.sharedresource_record_fd_publish_count++;
            hvdxg_note_shared_resource_record(
                "fd-published", 3, &shared->resource, kind,
                shared->cache_process, shared->cache_object,
                shared->global_share, shared->host_nt_handle, 0,
                object.v == shared->resource.resource ? 1 : 0);
        }
        if (kind == HV_DXG_SHARED_OBJECT_SYNC) {
            hvdxg.sharedsync_export_ret = ret;
            hvdxg.sharedsync_export_fd = (uint32)fd;
            hvdxg.sharedsync_export_nt_refs =
                hvdxg_ntshared_cache_refs(shared->kind,
                                          shared->cache_process,
                                          shared->cache_object,
                                          shared->host_nt_handle);
        }
        hvdxg.sharedhandle_last_ret = ret;
        break;
    }

    case LX_DXOPENSYNCOBJECTFROMNTHANDLE2: {
        struct d3dkmt_opensyncobjectfromnthandle2 req;
        struct hvdxg_shared_object *shared;
        struct vfs_file *shared_file = NULL;
        struct hvdxg_command_opensyncobject open;
        struct hvdxg_command_opensyncobject_return result;
        uint32 actual_len = 0;
        uint64 fence_kva = 0;
        uint64 fence_pa = 0;
        struct hvdxg_d3dkmthandle open_process;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0) {
            hvdxg.opensync_last_gate = 7;
            hvdxg.opensync_last_ret = ret;
            break;
        }
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            hvdxg.opensync_last_gate = 6;
            hvdxg.opensync_last_ret = ret;
            break;
        }
        hvdxg.opensync_ioctl_count++;
        hvdxg.sharedhandle_last_cmd = (uint32)cmd;
        hvdxg.sharedhandle_last_ret = -ENOTSUP;
        hvdxg.sharedhandle_last_device = req.device.v;
        hvdxg.sharedhandle_last_object = req.sync_object.v;
        hvdxg.sharedhandle_last_nt_handle = req.nt_handle;
        hvdxg.sharedhandle_last_count = 1;
        hvdxg.opensync_last_cmd_len = 0;
        hvdxg.opensync_last_wire_len = 0;
        hvdxg.opensync_last_ext = 0;
        hvdxg.opensync_last_ext_offset = 0;
        hvdxg.opensync_last_result_len = sizeof(result);
        hvdxg.opensync_last_actual_len = 0;
        hvdxg.opensync_last_ret = -ENOTSUP;
        hvdxg.opensync_last_status = 0;
        hvdxg.opensync_last_process = 0;
        hvdxg.opensync_last_device = req.device.v;
        hvdxg.opensync_last_device_host = 0;
        hvdxg.opensync_last_device_owner = 0;
        hvdxg.opensync_last_device_owner_generation = 0;
        hvdxg.opensync_last_device_generation = 0;
        hvdxg.opensync_last_global = 0;
        hvdxg.opensync_last_input_nt = req.nt_handle;
        hvdxg.opensync_last_host_nt = 0;
        hvdxg.opensync_last_object = req.sync_object.v;
        hvdxg.opensync_last_cache_object = 0;
        hvdxg.opensync_last_source_device = 0;
        hvdxg.opensync_last_source_device_host = 0;
        hvdxg.opensync_last_source_owner = 0;
        hvdxg.opensync_last_source_owner_generation = 0;
        hvdxg.opensync_last_source_flags = 0;
        hvdxg.opensync_last_same_device = 0;
        hvdxg.opensync_last_adapter_match = 0;
        hvdxg.opensync_last_adapter_low = hvdxg.adapter_luid.a;
        hvdxg.opensync_last_adapter_high = hvdxg.adapter_luid.b;
        hvdxg.opensync_last_host_adapter_low = hvdxg.host_adapter_luid.a;
        hvdxg.opensync_last_host_adapter_high = hvdxg.host_adapter_luid.b;
        hvdxg.opensync_last_flags = req.flags.value;
        hvdxg.opensync_last_wire_flags = 0;
        hvdxg.opensync_last_forced_flags = 0;
        hvdxg.opensync_last_fops_kind = 0;
        hvdxg.opensync_last_sync_type = 0;
        hvdxg.opensync_last_result_sync = 0;
        hvdxg.opensync_last_gpu_va = 0;
        hvdxg.opensync_last_cpu_pa = 0;
        hvdxg.opensync_last_fd_kind = 0;
        hvdxg.opensync_last_fd_refs = 0;
        hvdxg.opensync_last_gate = 1;
        hvdxg.opensync_last_current_tgid =
            current != NULL ? (uint64)thread_tgid(current) : 0;
        hvdxg.opensync_last_owner_tgid =
            owner != NULL && owner->process_state != NULL ?
            owner->process_state->tgid : 0;
        hvdxg.opensync_last_owner_generation =
            owner != NULL && owner->process_state != NULL ?
            owner->process_state->generation : 0;
        hvdxg.opensync_last_namespace_mismatch =
            hvdxg.opensync_last_owner_tgid != 0 &&
            hvdxg.opensync_last_current_tgid != 0 &&
            hvdxg.opensync_last_owner_tgid !=
                hvdxg.opensync_last_current_tgid;
        if (hvdxg.opensync_last_namespace_mismatch) {
            ret = -EINVAL;
            hvdxg.opensync_last_namespace_rejects++;
            hvdxg.sharedhandle_last_ret = ret;
            hvdxg.opensync_last_ret = ret;
            hvdxg.opensync_last_gate = 8;
            break;
        }
        if (req.device.v == 0 || !hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EINVAL;
            hvdxg.sharedhandle_last_ret = ret;
            hvdxg.opensync_last_ret = ret;
            break;
        }
        {
            struct hvdxg_object_entry *target_entry =
                hvdxg_owner_find_object(owner, HV_DXG_OBJECT_DEVICE,
                                        req.device.v);

            hvdxg.opensync_last_device_host =
                hvdxg_owner_host_object_handle(
                    owner, HV_DXG_OBJECT_DEVICE, req.device.v, NULL);
            hvdxg.opensync_last_device_owner =
                hvdxg_open_host_process(owner);
            hvdxg.opensync_last_device_owner_generation =
                hvdxg_open_process_generation(owner);
            hvdxg.opensync_last_device_generation =
                target_entry != NULL ? target_entry->generation : 0;
        }
        shared = hvdxg_shared_object_from_fd((int)req.nt_handle, 0,
                                             &shared_file);
        if (shared != NULL) {
            hvdxg.opensync_last_global = shared->global_share;
            hvdxg.opensync_last_host_nt = shared->host_nt_handle;
            hvdxg.opensync_last_object = shared->object;
            hvdxg.opensync_last_cache_object = shared->cache_object;
            hvdxg.opensync_last_source_device = shared->device;
            hvdxg.opensync_last_source_device_host =
                hvdxg_owner_host_object_handle(
                    owner, HV_DXG_OBJECT_DEVICE, shared->device, NULL);
            hvdxg.opensync_last_source_owner =
                shared->sync_owner_process != 0 ?
                shared->sync_owner_process :
                hvdxg_owner_sync_owner_process(owner, shared->object);
            hvdxg.opensync_last_source_owner_generation =
                shared->sync_owner_generation != 0 ?
                shared->sync_owner_generation :
                hvdxg_owner_sync_owner_generation(owner, shared->object);
            hvdxg.opensync_last_source_flags =
                shared->sync_flags != 0 ? shared->sync_flags :
                hvdxg_owner_sync_flags(owner, shared->object);
            hvdxg.opensync_last_same_device =
                req.device.v != 0 && shared->device == req.device.v;
            hvdxg.opensync_last_adapter_match =
                hvdxg_luid_nonzero(hvdxg.adapter_luid) &&
                hvdxg_luid_nonzero(hvdxg.host_adapter_luid) ? 1 : 0;
            hvdxg.opensync_last_fd_kind = shared->kind;
            hvdxg.opensync_last_fops_kind =
                hvdxg_shared_object_fops_kind(shared_file);
            hvdxg.opensync_last_sync_type = shared->sync_type != 0 ?
                shared->sync_type : hvdxg_owner_sync_type(owner,
                                                          shared->object);
            hvdxg.opensync_last_fd_refs = hvdxg_ntshared_cache_refs(
                shared->kind, shared->cache_process,
                shared->cache_object != 0 ? shared->cache_object :
                shared->object,
                shared->host_nt_handle);
        }
        if (shared == NULL || shared->kind != HV_DXG_SHARED_OBJECT_SYNC ||
            shared->global_share == 0) {
            ret = -EINVAL;
            hvdxg.sharedhandle_last_ret = ret;
            hvdxg.opensync_last_ret = ret;
            hvdxg.opensync_last_gate = 2;
            goto opensync_done;
        }
        hvdxg.opensync_last_gate = 3;
        memset(&open, 0, sizeof(open));
        memset(&result, 0, sizeof(result));
        open_process = hvdxg_owner_bound_process_handle(owner);
        hvdxg_command_vm_init(&open.hdr,
                              HV_DXGK_VMBCOMMAND_OPENSYNCOBJECT);
        open.hdr.process = open_process;
        open.device.v = req.device.v;
        open.global_sync_object.v = shared->global_share;
        open.flags = req.flags;
        hvdxg.opensync_last_wire_flags = open.flags.value;
        if (shared->sync_type == _D3DDDI_MONITORED_FENCE)
            open.engine_affinity = req.monitored_fence.engine_affinity;
        ret = hvdxg_send_sync_global(&open, sizeof(open), &result,
                                     sizeof(result), &actual_len);
        hvdxg.opensync_last_cmd_len = hvdxg.global_send_last_cmd_len;
        hvdxg.opensync_last_wire_len = hvdxg.global_send_last_wire_len;
        hvdxg.opensync_last_ext = hvdxg.global_send_last_ext;
        hvdxg.opensync_last_ext_offset = hvdxg.global_send_last_ext_offset;
        hvdxg.opensync_last_result_len = sizeof(result);
        hvdxg.opensync_last_actual_len = actual_len;
        hvdxg.opensync_last_process = open_process.v;
        hvdxg.opensync_last_status = result.status.v;
        hvdxg.opensync_last_result_sync = result.sync_object.v;
        hvdxg.opensync_last_gpu_va = result.gpu_virtual_address;
        hvdxg.opensync_last_cpu_pa = result.guest_cpu_physical_address;
        if (ret == 0 && actual_len < sizeof(result))
            ret = -EOVERFLOW;
        if (ret == 0)
            ret = hvdxg_ntstatus_to_errno(result.status);
        hvdxg.opensync_last_ret = ret;
        hvdxg.opensync_last_gate = ret == 0 ? 5 : 4;
        if (ret != 0) {
            hvdxg.sharedhandle_last_ret = ret;
            goto opensync_done;
        }
        req.sync_object.v = result.sync_object.v;
        if (shared->sync_type == _D3DDDI_MONITORED_FENCE) {
            req.monitored_fence.fence_value_cpu_va =
                hvdxg_map_iospace_user_canonical(
                    HV_DXG_FENCE_SOURCE_OPEN_SYNC,
                    result.guest_cpu_physical_address, PGSIZE,
                    0, &fence_pa, NULL, 1);
            fence_kva = hvdxg_map_iospace_kernel_canonical(
                HV_DXG_FENCE_SOURCE_OPEN_SYNC,
                result.guest_cpu_physical_address, PGSIZE, fence_pa, 1);
            if (req.monitored_fence.fence_value_cpu_va == 0) {
                ret = -ENOMEM;
                hvdxg.sharedhandle_last_ret = ret;
                goto opensync_done;
            }
            req.monitored_fence.fence_value_gpu_va =
                result.gpu_virtual_address;
        }
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        if (ret == 0 && owner != NULL)
            hvdxg_track_sync(owner, req.device.v, req.sync_object.v,
                             shared->sync_type, req.flags.value,
                             shared->global_share,
                             req.monitored_fence.fence_value_cpu_va,
                             fence_kva, 0);
        hvdxg.sharedhandle_last_object = req.sync_object.v;
        hvdxg.sharedhandle_last_ret = ret;
        hvdxg.opensync_last_ret = ret;
opensync_done:
        if (shared_file != NULL)
            vfs_fput(shared_file);
        break;
    }

    case LX_DXQUERYRESOURCEINFOFROMNTHANDLE: {
        struct d3dkmt_queryresourceinfofromnthandle req;
        struct hvdxg_shared_object *shared;
        struct vfs_file *shared_file = NULL;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.sharedhandle_last_cmd = (uint32)cmd;
        hvdxg.sharedhandle_last_ret = -ENOTSUP;
        hvdxg.sharedhandle_last_device = req.device.v;
        hvdxg.sharedhandle_last_object = 0;
        hvdxg.sharedhandle_last_nt_handle = req.nt_handle;
        hvdxg.sharedhandle_last_count = req.allocation_count;
        hvdxg.queryresource_last_seen++;
        hvdxg.queryresource_last_ret = -ENOTSUP;
        hvdxg.queryresource_last_device = req.device.v;
        hvdxg.queryresource_last_nt = req.nt_handle;
        hvdxg.queryresource_last_fd_kind = 0;
        hvdxg.queryresource_last_fops_kind = 0;
        hvdxg.queryresource_last_sync_probe = 0;
        hvdxg.queryresource_last_global = 0;
        hvdxg.queryresource_last_host_nt = 0;
        hvdxg.queryresource_last_refs = 0;
        hvdxg.queryresource_last_object = 0;
        hvdxg.queryresource_last_cache_object = 0;
        hvdxg.queryresource_last_alloc_count = req.allocation_count;
        hvdxg.queryresource_last_runtime_size =
            req.private_runtime_data_size;
        hvdxg.queryresource_last_resource_size =
            req.resource_priv_drv_data_size;
        hvdxg.queryresource_last_total_size =
            req.total_priv_drv_data_size;
        if (req.device.v == 0 || !hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EINVAL;
            hvdxg.sharedhandle_last_ret = ret;
            hvdxg.queryresource_last_ret = ret;
            break;
        }
        shared = hvdxg_shared_object_from_fd((int)req.nt_handle, 0,
                                             &shared_file);
        if (shared != NULL) {
            hvdxg.sharedhandle_last_kind = shared->kind;
            hvdxg.queryresource_last_fd_kind = shared->kind;
            hvdxg.queryresource_last_fops_kind =
                hvdxg_shared_object_fops_kind(shared_file);
            hvdxg.queryresource_last_sync_probe =
                shared->kind == HV_DXG_SHARED_OBJECT_SYNC ? 1 : 0;
            hvdxg.queryresource_last_global = shared->global_share;
            hvdxg.queryresource_last_host_nt = shared->host_nt_handle;
            hvdxg.queryresource_last_object = shared->object;
            hvdxg.queryresource_last_cache_object = shared->cache_object;
            hvdxg.queryresource_last_refs =
                hvdxg_ntshared_cache_refs(
                    shared->kind, shared->cache_process,
                    shared->cache_object != 0 ? shared->cache_object :
                    shared->object, shared->host_nt_handle);
        }
        if (shared == NULL ||
            shared->kind != HV_DXG_SHARED_OBJECT_RESOURCE) {
            /*
             * WSL's dxgkio_query_resource_info_nt rejects dxgsyncobj fds
             * here.  Keep the same errno, but record the actual fd kind so
             * the runtime's resource-before-sync probe is visible.
             */
            ret = -EINVAL;
            hvdxg.sharedhandle_last_ret = ret;
            hvdxg.queryresource_last_ret = ret;
            goto queryresource_done;
        }
        if (shared->resource.existing_sysmem)
            hvdxg_note_existing_sysmem_share(&shared->resource, 5);
        ret = hvdxg_seal_resource(&shared->resource);
        if (ret != 0) {
            hvdxg.sharedhandle_last_ret = ret;
            hvdxg.queryresource_last_ret = ret;
            goto queryresource_done;
        }
        if (shared->resource.existing_sysmem)
            hvdxg_note_existing_sysmem_share(&shared->resource, 6);
        hvdxg.sharedresource_record_query_count++;
        hvdxg_note_shared_resource_record(
            "queryresource", 4, &shared->resource, shared->kind,
            shared->cache_process, shared->cache_object,
            shared->global_share, shared->host_nt_handle, 0,
            shared->object == shared->resource.resource ? 1 : 0);
        req.private_runtime_data_size =
            shared->resource.private_runtime_data_size;
        req.resource_priv_drv_data_size =
            shared->resource.resource_priv_drv_data_size;
        req.total_priv_drv_data_size =
            shared->resource.total_priv_drv_data_size;
        req.allocation_count = shared->resource.allocation_count;
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        hvdxg.sharedhandle_last_count = req.allocation_count;
        hvdxg.sharedhandle_last_object = shared->object;
        hvdxg.sharedhandle_last_ret = ret;
        hvdxg.queryresource_last_ret = ret;
        hvdxg.queryresource_last_alloc_count = req.allocation_count;
        hvdxg.queryresource_last_runtime_size =
            req.private_runtime_data_size;
        hvdxg.queryresource_last_resource_size =
            req.resource_priv_drv_data_size;
        hvdxg.queryresource_last_total_size =
            req.total_priv_drv_data_size;
queryresource_done:
        if (shared_file != NULL)
            vfs_fput(shared_file);
        break;
    }

    case LX_DXOPENRESOURCEFROMNTHANDLE: {
        struct d3dkmt_openresourcefromnthandle req;
        struct hvdxg_shared_object *shared;
        struct hvdxg_command_openresource open;
        struct hvdxg_command_openresource_return *result = NULL;
        struct d3dddi_openallocationinfo2 open_alloc[HV_DXG_ALLOCATION_MAX];
        struct vfs_file *shared_file = NULL;
        uint32 result_len;
        uint32 actual_len = 0;
        uint32 alloc_private_offset = 0;
        struct hvdxg_tracked_resource opened;
        int host_resource_opened = 0;
        struct hvdxg_d3dkmthandle open_process;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.sharedhandle_last_cmd = (uint32)cmd;
        hvdxg.sharedhandle_last_ret = -ENOTSUP;
        hvdxg.sharedhandle_last_device = req.device.v;
        hvdxg.sharedhandle_last_object = req.resource.v;
        hvdxg.sharedhandle_last_nt_handle = req.nt_handle;
        hvdxg.sharedhandle_last_count = req.allocation_count;
        hvdxg.openresource_last_cmd_len = 0;
        hvdxg.openresource_last_wire_len = 0;
        hvdxg.openresource_last_ext = 0;
        hvdxg.openresource_last_ext_offset = 0;
        hvdxg.openresource_last_result_len = 0;
        hvdxg.openresource_last_actual_len = 0;
        hvdxg.openresource_last_ret = -ENOTSUP;
        hvdxg.openresource_last_status = 0;
        hvdxg.openresource_last_process = 0;
        hvdxg.openresource_last_device = req.device.v;
        hvdxg.openresource_last_global = 0;
        hvdxg.openresource_last_alloc_count = req.allocation_count;
        hvdxg.openresource_last_total_priv = req.total_priv_drv_data_size;
        hvdxg.openresource_last_result_resource = req.resource.v;
        hvdxg.openresource_last_result_alloc0 = 0;
        hvdxg.openresource_last_seal_before = 0;
        hvdxg.openresource_last_seal_after = 0;
        hvdxg.openresource_last_fd_kind = 0;
        hvdxg.openresource_last_fd_refs = 0;
        hvdxg.openresource_last_route_global = 0;
        hvdxg.openresource_last_fops_kind = 0;
        if (req.device.v == 0 || !hvdxg_owner_has_device(owner, req.device.v) ||
            req.allocation_count == 0 ||
            req.allocation_count > HV_DXG_ALLOCATION_MAX ||
            req.open_alloc_info == 0) {
            ret = -EINVAL;
            hvdxg.sharedhandle_last_ret = ret;
            hvdxg.openresource_last_ret = ret;
            break;
        }
        shared = hvdxg_shared_object_from_fd((int)req.nt_handle,
                                             HV_DXG_SHARED_OBJECT_RESOURCE,
                                             &shared_file);
        if (shared != NULL && shared->resource.existing_sysmem)
            hvdxg_note_existing_sysmem_share(&shared->resource, 7);
        if (shared == NULL || shared->global_share == 0 ||
            req.allocation_count != shared->resource.allocation_count ||
            req.private_runtime_data_size <
                (int32)shared->resource.private_runtime_data_size ||
            req.resource_priv_drv_data_size <
                shared->resource.resource_priv_drv_data_size ||
            req.total_priv_drv_data_size <
                shared->resource.total_priv_drv_data_size) {
            ret = -EINVAL;
            hvdxg.sharedhandle_last_ret = ret;
            hvdxg.openresource_last_ret = ret;
            goto openresource_done;
        }
        hvdxg.openresource_last_global = shared->global_share;
        hvdxg.openresource_last_total_priv =
            shared->resource.total_priv_drv_data_size;
        hvdxg.openresource_last_fd_kind = shared->kind;
        hvdxg.openresource_last_fops_kind =
            hvdxg_shared_object_fops_kind(shared_file);
        hvdxg.openresource_last_fd_refs = hvdxg_ntshared_cache_refs(
            shared->kind, shared->cache_process,
            shared->cache_object != 0 ? shared->cache_object :
            shared->object,
            shared->host_nt_handle);
        if (hvdxg.openresource_last_fd_refs == 0)
            hvdxg.openresource_last_fd_refs =
                shared->resource.host_shared_refs;
        hvdxg.openresource_last_seal_before = shared->resource.sealed;
        ret = hvdxg_seal_resource(&shared->resource);
        hvdxg.openresource_last_seal_after = shared->resource.sealed;
        if (shared->resource.existing_sysmem)
            hvdxg_note_existing_sysmem_share(&shared->resource, 8);
        if (ret != 0) {
            hvdxg.sharedhandle_last_ret = ret;
            hvdxg.openresource_last_ret = ret;
            goto openresource_done;
        }
        hvdxg.sharedresource_record_open_count++;
        hvdxg_note_shared_resource_record(
            "openresource-prehost", 5, &shared->resource, shared->kind,
            shared->cache_process, shared->cache_object,
            shared->global_share, shared->host_nt_handle, 0,
            shared->object == shared->resource.resource ? 1 : 0);
        result_len = sizeof(*result) +
                     (req.allocation_count - 1) *
                         sizeof(result->allocations[0]);
        result = kvmalloc(result_len);
        if (result == NULL) {
            ret = -ENOMEM;
            hvdxg.sharedhandle_last_ret = ret;
            hvdxg.openresource_last_ret = ret;
            goto openresource_done;
        }
        memset(&open, 0, sizeof(open));
        memset(result, 0, result_len);
        open_process = hvdxg_owner_bound_process_handle(owner);
        hvdxg_command_vgpu_init_process(&open.hdr,
                                        HV_DXGK_VMBCOMMAND_OPENRESOURCE,
                                        open_process);
        open.device.v = req.device.v;
        open.nt_security_sharing = 1;
        open.global_share.v = shared->global_share;
        hvdxg.sharedresource_open_global = shared->global_share;
        open.allocation_count = req.allocation_count;
        open.total_priv_drv_data_size =
            shared->resource.total_priv_drv_data_size;
        ret = hvdxg_send_sync_vgpu(&open, sizeof(open), result,
                                   result_len, &actual_len);
        hvdxg.openresource_last_cmd_len = hvdxg.vgpu_send_last_cmd_len;
        hvdxg.openresource_last_wire_len = hvdxg.vgpu_send_last_wire_len;
        hvdxg.openresource_last_ext = hvdxg.vgpu_send_last_ext;
        hvdxg.openresource_last_ext_offset =
            hvdxg.vgpu_send_last_ext_offset;
        hvdxg.openresource_last_route_global =
            hvdxg.vgpu_send_last_route_global;
        hvdxg.openresource_last_result_len = result_len;
        hvdxg.openresource_last_actual_len = actual_len;
        hvdxg.openresource_last_process = open_process.v;
        hvdxg.openresource_last_status = result->status.v;
        hvdxg.openresource_last_result_resource = result->resource.v;
        hvdxg.openresource_last_result_alloc0 =
            req.allocation_count != 0 ? result->allocations[0].v : 0;
        if (ret == 0 && actual_len < result_len)
            ret = -EOVERFLOW;
        if (ret == 0)
            ret = hvdxg_ntstatus_to_errno(result->status);
        hvdxg.openresource_last_ret = ret;
        if (ret != 0)
            goto openresource_done;
        hvdxg_note_shared_resource_record(
            "openresource-host-ok", 6, &shared->resource, shared->kind,
            shared->cache_process, shared->cache_object,
            shared->global_share, shared->host_nt_handle, 0,
            shared->object == shared->resource.resource ? 1 : 0);
        host_resource_opened = result->resource.v != 0;
        memset(open_alloc, 0, sizeof(open_alloc));
        alloc_private_offset = 0;
        for (uint32 i = 0; i < req.allocation_count; i++) {
            uint32 private_size = shared->resource.alloc_priv_sizes[i];

            if (alloc_private_offset > shared->resource.total_priv_drv_data_size ||
                private_size >
                    shared->resource.total_priv_drv_data_size -
                        alloc_private_offset) {
                ret = -EOVERFLOW;
                goto openresource_done;
            }
            open_alloc[i].allocation.v = result->allocations[i].v;
            open_alloc[i].priv_drv_data_size = private_size;
            if (req.total_priv_drv_data != 0)
                open_alloc[i].priv_drv_data =
                    req.total_priv_drv_data + alloc_private_offset;
            alloc_private_offset += private_size;
        }
        if (alloc_private_offset != shared->resource.total_priv_drv_data_size) {
            ret = -EOVERFLOW;
            goto openresource_done;
        }
        if (shared->resource.private_runtime_data_size != 0 &&
            either_copyout(1, req.private_runtime_data,
                           shared->resource.private_runtime_data,
                           shared->resource.private_runtime_data_size) < 0) {
            ret = -EFAULT;
            goto openresource_done;
        }
        if (shared->resource.resource_priv_drv_data_size != 0 &&
            either_copyout(1, req.resource_priv_drv_data,
                           shared->resource.resource_priv_drv_data,
                           shared->resource.resource_priv_drv_data_size) < 0) {
            ret = -EFAULT;
            goto openresource_done;
        }
        alloc_private_offset = 0;
        for (uint32 i = 0; i < req.allocation_count; i++) {
            uint32 private_size = shared->resource.alloc_priv_sizes[i];

            if (private_size != 0 &&
                either_copyout(1, req.total_priv_drv_data + alloc_private_offset,
                               shared->resource.total_priv_drv_data +
                                   alloc_private_offset,
                               private_size) < 0) {
                ret = -EFAULT;
                goto openresource_done;
            }
            alloc_private_offset += private_size;
        }
        if (either_copyout(1, req.open_alloc_info, open_alloc,
                           req.allocation_count * sizeof(open_alloc[0])) < 0) {
            ret = -EFAULT;
            goto openresource_done;
        }
        req.resource.v = result->resource.v;
        req.total_priv_drv_data_size =
            shared->resource.total_priv_drv_data_size;
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        if (ret == 0 && owner != NULL) {
            if (hvdxg_clone_resource(&opened, &shared->resource) == 0) {
                struct hvdxg_tracked_resource *slot;

                opened.device = req.device.v;
                opened.resource = req.resource.v;
                opened.global_share = shared->global_share;
                opened.owner_process = hvdxg_open_host_process(owner);
                opened.owner_generation =
                    hvdxg_open_process_generation(owner);
                opened.owner_refs = hvdxg_open_process_refs(owner);
                opened.sealed = 1;
                opened.opened_from_shared = 1;
                opened.open_count = 1;
                for (uint32 i = 0; i < req.allocation_count; i++)
                    opened.allocation_handles[i] =
                        result->allocations[i].v;
                slot = hvdxg_owner_find_resource(owner, opened.device,
                                                 opened.resource);
                if (slot == NULL &&
                    hvdxg_grow_table((void **)&owner->resources,
                                     &owner->resource_capacity,
                                     owner->resource_count + 1,
                                     sizeof(owner->resources[0]),
                                     HV_DXG_RESOURCE_TRACKED_MAX) == 0)
                    slot = &owner->resources[owner->resource_count++];
                if (slot != NULL) {
                    hvdxg_free_tracked_resource(slot);
                    *slot = opened;
                    if (hvdxg_track_object(owner, HV_DXG_OBJECT_RESOURCE,
                                           opened.resource,
                                           opened.device,
                                           opened.device) == 0)
                        hvdxg.sharedresource_open_tracked++;
                    else
                        ret = -ENOMEM;
                } else {
                    hvdxg_free_tracked_resource(&opened);
                    ret = -ENOMEM;
                }
            } else {
                ret = -ENOMEM;
            }
            if (ret == 0) {
                for (uint32 i = 0; i < req.allocation_count; i++) {
                    ret = hvdxg_track_allocation(
                        owner, req.device.v, req.resource.v,
                        result->allocations[i].v,
                        shared->resource.allocation_sizes[i],
                        shared->resource.allocation_flags[i],
                        0, NULL, 0);
                    if (ret != 0)
                        break;
                }
            }
        }
openresource_done:
        if (ret != 0 && owner != NULL && req.resource.v != 0) {
            hvdxg_untrack_allocation(owner, req.device.v, req.resource.v, 0);
            hvdxg_untrack_resource(owner, req.device.v, req.resource.v);
        }
        if (ret != 0 && host_resource_opened && req.device.v != 0 &&
            req.resource.v != 0)
            (void)hvdxg_destroy_allocation_host_process(
                open_process.v, req.device.v, req.resource.v, 0,
                HV_DXG_DESTROY_ALLOC_CTX_HELPER);
        if (shared_file != NULL)
            vfs_fput(shared_file);
        if (result != NULL)
            kvfree(result);
        hvdxg.sharedhandle_last_object = req.resource.v;
        hvdxg.sharedhandle_last_ret = ret;
        hvdxg.openresource_last_ret = ret;
        break;
    }

    case LX_DXSHAREOBJECTWITHHOST: {
        struct d3dkmt_shareobjectwithhost req;
        struct hvdxg_tracked_resource *resource = NULL;
        uint32 current_process;
        uint32 current_generation;
        uint32 process;
        uint32 host_device;
        uint32 host_object;
        uint32 actual_len = 0;
        int32 status = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        current_process = hvdxg_owner_bound_process_handle(owner).v;
        current_generation = hvdxg_open_process_generation(owner);
        process = current_process;
        hvdxg.shareobject_last_len = 0;
        hvdxg.shareobject_last_cmd_len = 0;
        hvdxg.shareobject_last_wire_len = 0;
        hvdxg.shareobject_last_ext = 0;
        hvdxg.shareobject_last_ext_offset = 0;
        hvdxg.shareobject_last_device_offset = 0;
        hvdxg.shareobject_last_object_offset = 0;
        hvdxg.shareobject_last_result_len = 0;
        hvdxg.shareobject_last_head_len = 0;
        memset(hvdxg.shareobject_last_head, 0,
               sizeof(hvdxg.shareobject_last_head));
        hvdxg.shareobject_last_completion_type = 0;
        hvdxg.shareobject_last_completion_len = 0;
        memset(hvdxg.shareobject_last_completion_prefix, 0,
               sizeof(hvdxg.shareobject_last_completion_prefix));
        hvdxg.shareobject_last_ret = 0;
        hvdxg.shareobject_last_status = 0;
        hvdxg.shareobject_last_process = process;
        hvdxg.shareobject_last_device = req.device_handle.v;
        hvdxg.shareobject_last_object = req.object_handle.v;
        hvdxg.shareobject_last_reserved = 0;
        hvdxg.shareobject_last_nt_handle = 0;
        hvdxg.shareobject_diag_attempted = 0;
        hvdxg.shareobject_diag_valid_nt = 0;
        hvdxg.shareobject_diag_kind = 0;
        hvdxg.shareobject_diag_reason = 0;
        hvdxg.sharedhandle_last_cmd = (uint32)cmd;
        hvdxg.sharedhandle_last_ret = 0;
        hvdxg.sharedhandle_last_device = req.device_handle.v;
        hvdxg.sharedhandle_last_object = req.object_handle.v;
        hvdxg.sharedhandle_last_nt_handle = 0;
        hvdxg.sharedhandle_last_count = 1;
        hvdxg.sharedhandle_last_global_share = 0;
        hvdxg.sharedhandle_last_runtime_d3d12_flags = 0;
        hvdxg.sharedhandle_last_kind = 0;
        hvdxg.sharedhandle_last_fops_kind = 0;
        hvdxg.sharedhandle_last_raw_device = req.device_handle.v;
        hvdxg.sharedhandle_last_raw_object = req.object_handle.v;
        hvdxg.sharedhandle_last_host_device = 0;
        hvdxg.sharedhandle_last_host_device_found = 0;
        hvdxg.sharedhandle_last_host_object = 0;
        hvdxg.sharedhandle_last_object_found = 0;
        hvdxg.sharedhandle_last_raw_resource_found = 0;
        hvdxg.sharedhandle_last_raw_allocation_found = 0;
        hvdxg.sharedhandle_last_raw_sync_found = 0;
        hvdxg.sharedhandle_last_parent = 0;
        hvdxg.sharedhandle_last_current_process = current_process;
        hvdxg.sharedhandle_last_current_generation = current_generation;
        hvdxg.sharedhandle_last_creator_process = 0;
        hvdxg.sharedhandle_last_creator_generation = 0;
        hvdxg.sharedhandle_last_owner_process = 0;
        hvdxg.sharedhandle_last_owner_generation = 0;
        hvdxg.sharedhandle_last_owner_refs = 0;
        hvdxg.sharedhandle_last_owner_used = 0;
        hvdxg.sharedhandle_last_object_type = HV_DXG_OBJECT_NONE;
        hvdxg.sharedhandle_last_object_device = 0;
        hvdxg.sharedhandle_last_allocation = 0;
        hvdxg.sharedhandle_last_allocation_found = 0;
        hvdxg.sharedhandle_last_allocation_owner_process = 0;
        hvdxg.sharedhandle_last_allocation_owner_generation = 0;
        hvdxg.sharedhandle_last_allocation_owner_refs = 0;
        hvdxg.sharedhandle_last_map_va = 0;
        hvdxg.sharedhandle_last_map_pages = 0;
        hvdxg.sharedhandle_last_map_fence = 0;
        hvdxg.sharedhandle_last_map_ret = 0;
        hvdxg.sharedhandle_last_map_status = 0;
        hvdxg.sharedhandle_last_resident_paging_queue = 0;
        hvdxg.sharedhandle_last_resident_sync = 0;
        hvdxg.sharedhandle_last_resident_fence = 0;
        hvdxg.sharedhandle_last_resident_current = 0;
        hvdxg.sharedhandle_last_resident_wait_result = 0;
        hvdxg.sharedhandle_last_resident_wait_ret = 0;
        hvdxg.sharedhandle_last_resident_missing = 0;
        hvdxg.sharedhandle_last_resident_enforced = 0;
        hvdxg.sharedhandle_last_create_flags = 0;
        hvdxg.sharedhandle_last_alloc_count = 0;
        hvdxg.sharedhandle_last_sealed = 0;
        hvdxg.sharedhandle_last_sync_type = 0;
        hvdxg.sharedhandle_last_sync_flags = 0;
        hvdxg.sharedhandle_last_sync_global = 0;
        hvdxg.sharedhandle_last_sync_monitor_fence = 0;
        hvdxg.sharedhandle_last_sync_fence_cpu = 0;
        hvdxg.sharedhandle_last_sync_fence_kva = 0;
        hvdxg.shareobjects_last_desired_access = 0;
        hvdxg.shareobjects_last_object_attr = 0;
        hvdxg.shareobjects_last_attr_len = 0;
        hvdxg.shareobjects_last_attr_ret = 0;
        memset(hvdxg.shareobjects_last_attr_head, 0,
               sizeof(hvdxg.shareobjects_last_attr_head));
        if (req.device_handle.v == 0 || req.object_handle.v == 0) {
            ret = -EINVAL;
            hvdxg.shareobject_last_ret = ret;
            break;
        }
        hvdxg.sharedhandle_last_raw_resource_found =
            hvdxg_owner_find_object(owner, HV_DXG_OBJECT_RESOURCE,
                                    req.object_handle.v) != NULL ? 1 : 0;
        hvdxg.sharedhandle_last_raw_allocation_found =
            hvdxg_owner_find_object(owner, HV_DXG_OBJECT_ALLOCATION,
                                    req.object_handle.v) != NULL ? 1 : 0;
        hvdxg.sharedhandle_last_raw_sync_found =
            hvdxg_owner_find_object(owner, HV_DXG_OBJECT_SYNC,
                                    req.object_handle.v) != NULL ? 1 : 0;
        resource = hvdxg_owner_find_resource(owner, 0, req.object_handle.v);
        if (resource != NULL) {
            hvdxg.sharedhandle_last_kind =
                HV_DXG_SHARED_OBJECT_RESOURCE;
            hvdxg.sharedhandle_last_host_device =
                hvdxg_owner_host_object_handle(
                    owner, HV_DXG_OBJECT_DEVICE, resource->device, NULL);
            hvdxg.sharedhandle_last_host_device_found =
                hvdxg.sharedhandle_last_host_device != 0 ? 1 : 0;
            if (hvdxg.sharedhandle_last_host_device == 0)
                hvdxg.sharedhandle_last_host_device = resource->device;
            hvdxg.sharedhandle_last_host_object =
                hvdxg_owner_host_object_handle(
                    owner, HV_DXG_OBJECT_RESOURCE, req.object_handle.v,
                    &hvdxg.sharedhandle_last_parent);
            hvdxg.sharedhandle_last_object_found =
                hvdxg.sharedhandle_last_host_object != 0 ? 1 : 0;
            hvdxg.sharedhandle_last_object_type =
                HV_DXG_OBJECT_RESOURCE;
            hvdxg.sharedhandle_last_object_device =
                hvdxg_owner_object_device(owner, HV_DXG_OBJECT_RESOURCE,
                                          req.object_handle.v);
            if (hvdxg.sharedhandle_last_host_object == 0) {
                ret = -EINVAL;
                hvdxg.shareobject_last_ret = ret;
                break;
            }
            ret = hvdxg_prepare_resource_nt_metadata(owner, resource);
            if (ret != 0) {
                hvdxg.shareobject_last_ret = ret;
                break;
            }
            hvdxg.sharedhandle_last_owner_process =
                resource->owner_process;
            hvdxg.sharedhandle_last_owner_generation =
                resource->owner_generation;
            hvdxg.sharedhandle_last_owner_refs = resource->owner_refs;
            hvdxg.sharedhandle_last_creator_process =
                resource->owner_process;
            hvdxg.sharedhandle_last_creator_generation =
                resource->owner_generation;
            hvdxg.sharedhandle_last_global_share =
                resource->global_share;
            hvdxg.sharedhandle_last_runtime_d3d12_flags =
                resource->runtime_d3d12_flags;
            hvdxg.sharedhandle_last_create_flags =
                resource->create_flags_value;
            hvdxg.sharedhandle_last_alloc_count =
                resource->allocation_count;
            if (resource->allocation_count != 0)
                hvdxg.sharedhandle_last_allocation =
                    resource->allocation_handles[0];
            {
                struct hvdxg_tracked_allocation *a = NULL;

                if (hvdxg.sharedhandle_last_allocation != 0)
                    a = hvdxg_owner_find_allocation(
                        owner, resource->device, resource->resource,
                        hvdxg.sharedhandle_last_allocation);
                if (a == NULL) {
                    hvdxg.sharedhandle_last_allocation =
                        hvdxg_owner_first_allocation(
                            owner, resource->device, resource->resource,
                            &hvdxg.sharedhandle_last_allocation_found);
                    if (hvdxg.sharedhandle_last_allocation != 0)
                        a = hvdxg_owner_find_allocation(
                            owner, resource->device, resource->resource,
                            hvdxg.sharedhandle_last_allocation);
                }
                if (a != NULL) {
                    hvdxg.sharedhandle_last_allocation_found = 1;
                    hvdxg.sharedhandle_last_allocation_owner_process =
                        a->owner_process;
                    hvdxg.sharedhandle_last_allocation_owner_generation =
                        a->owner_generation;
                    hvdxg.sharedhandle_last_allocation_owner_refs =
                        a->owner_refs;
                }
            }
            hvdxg.sharedhandle_last_sealed = resource->sealed;
        } else if (hvdxg_owner_has_sync(owner, req.object_handle.v)) {
            hvdxg.sharedhandle_last_kind = HV_DXG_SHARED_OBJECT_SYNC;
            hvdxg.sharedhandle_last_sync_type =
                hvdxg_owner_sync_type(owner, req.object_handle.v);
            hvdxg.sharedhandle_last_sync_flags =
                hvdxg_owner_sync_flags(owner, req.object_handle.v);
            hvdxg.sharedhandle_last_sync_global =
                hvdxg_owner_sync_global_shared(owner, req.object_handle.v);
            hvdxg.sharedhandle_last_sync_monitor_fence =
                hvdxg_owner_sync_is_monitor_fence_handle(
                    owner, req.object_handle.v);
            hvdxg.sharedhandle_last_sync_fence_cpu =
                hvdxg_owner_sync_fence_cpu_va(owner, req.object_handle.v);
            hvdxg.sharedhandle_last_sync_fence_kva =
                hvdxg_owner_sync_fence_kva(owner, req.object_handle.v);
            hvdxg.sharedhandle_last_host_device =
                hvdxg_owner_host_object_handle(
                    owner, HV_DXG_OBJECT_DEVICE,
                    hvdxg_owner_sync_device(owner, req.object_handle.v),
                    NULL);
            hvdxg.sharedhandle_last_host_device_found =
                hvdxg.sharedhandle_last_host_device != 0 ? 1 : 0;
            if (hvdxg.sharedhandle_last_host_device == 0)
                hvdxg.sharedhandle_last_host_device =
                    hvdxg_owner_sync_device(owner, req.object_handle.v);
            hvdxg.sharedhandle_last_host_object =
                hvdxg_owner_host_object_handle(
                    owner, HV_DXG_OBJECT_SYNC, req.object_handle.v,
                    &hvdxg.sharedhandle_last_parent);
            hvdxg.sharedhandle_last_object_found =
                hvdxg.sharedhandle_last_host_object != 0 ? 1 : 0;
            hvdxg.sharedhandle_last_object_type = HV_DXG_OBJECT_SYNC;
            hvdxg.sharedhandle_last_object_device =
                hvdxg_owner_object_device(owner, HV_DXG_OBJECT_SYNC,
                                          req.object_handle.v);
            hvdxg.sharedhandle_last_owner_process =
                hvdxg_owner_sync_owner_process(owner, req.object_handle.v);
            hvdxg.sharedhandle_last_owner_generation =
                hvdxg_owner_sync_owner_generation(owner,
                                                  req.object_handle.v);
            hvdxg.sharedhandle_last_owner_refs =
                hvdxg_owner_sync_owner_refs(owner, req.object_handle.v);
            hvdxg.sharedhandle_last_creator_process =
                hvdxg.sharedhandle_last_owner_process;
            hvdxg.sharedhandle_last_creator_generation =
                hvdxg.sharedhandle_last_owner_generation;
            hvdxg.sharedhandle_last_global_share =
                hvdxg_owner_sync_global_shared(owner,
                                               req.object_handle.v);
        }
        process = current_process;
        hvdxg.sharedhandle_last_owner_used = 0;
        host_device = hvdxg.sharedhandle_last_host_device != 0 ?
                      hvdxg.sharedhandle_last_host_device :
                      req.device_handle.v;
        host_object = hvdxg.sharedhandle_last_host_object != 0 ?
                      hvdxg.sharedhandle_last_host_object :
                      req.object_handle.v;
        hvdxg.shareobject_last_process = process;
        hvdxg.shareobject_last_device = host_device;
        hvdxg.shareobject_last_object = host_object;
        hvdxg.sharedhandle_last_device = host_device;
        hvdxg.sharedhandle_last_object = host_object;
        ret = hvdxg_share_object_with_host(process, host_device, host_object,
                                           req.reserved,
                                           &req.object_vail_nt_handle,
                                           &actual_len, &status);
        hvdxg.shareobject_last_len = actual_len;
        hvdxg.shareobject_last_ret = ret;
        hvdxg.shareobject_last_status = status;
        hvdxg.shareobject_last_nt_handle = req.object_vail_nt_handle;
        if (ret == 0) {
            if (either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0)
                ret = -EFAULT;
        }
        hvdxg.shareobject_last_ret = ret;
        hvdxg.sharedhandle_last_nt_handle = req.object_vail_nt_handle;
        hvdxg.sharedhandle_last_ret = ret;
        break;
    }

    case LX_DXQUERYVIDEOMEMORYINFO: {
        struct d3dkmt_queryvideomemoryinfo req;
        struct hvdxg_command_queryvideomemoryinfo query;
        struct hvdxg_command_queryvideomemoryinfo_return result;
        uint32 actual_len = 0;
        uint32 host_adapter;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.process != 0 ||
            hvdxg_resolve_adapter_handle(owner, req.adapter.v,
                                         &host_adapter) != 0) {
            ret = -EINVAL;
            break;
        }
        memset(&query, 0, sizeof(query));
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(&query.hdr,
                                        HV_DXGK_VMBCOMMAND_QUERYVIDEOMEMORYINFO,
                                        hvdxg.dxg_process);
        query.adapter.v = host_adapter;
        query.memory_segment_group = (uint32)req.memory_segment_group;
        query.physical_adapter_index = req.physical_adapter_index;
        ret = hvdxg_send_sync_vgpu(&query, sizeof(query), &result,
                                   sizeof(result), &actual_len);
        if (ret != 0)
            break;
        if (actual_len < sizeof(result)) {
            ret = -EOVERFLOW;
            break;
        }
        req.budget = result.budget;
        req.current_usage = result.current_usage;
        req.current_reservation = result.current_reservation;
        req.available_for_reservation = result.available_for_reservation;
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        break;
    }

    case LX_DXQUERYSTATISTICS: {
        struct d3dkmt_querystatistics req;
        struct hvdxg_command_querystatistics query;
        struct hvdxg_command_querystatistics_return result;
        struct hvdxg_winluid luid;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.statistics_last_type = (uint32)req.type;
        hvdxg.statistics_last_len = 0;
        hvdxg.statistics_last_ret = 0;
        hvdxg.statistics_last_status = 0;
        luid.a = req.adapter_luid.a;
        luid.b = req.adapter_luid.b;
        if (req.process != 0 || !hvdxg_luid_equal(luid, hvdxg.adapter_luid)) {
            ret = -EINVAL;
            hvdxg.statistics_last_ret = ret;
            break;
        }
        memset(&query, 0, sizeof(query));
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(&query.hdr,
                                        HV_DXGK_VMBCOMMAND_QUERYSTATISTICS,
                                        hvdxg.dxg_process);
        query.args = req;
        query.args.adapter_luid.a = hvdxg.host_adapter_luid.a;
        query.args.adapter_luid.b = hvdxg.host_adapter_luid.b;
        ret = hvdxg_send_sync_vgpu(&query, sizeof(query), &result,
                                   sizeof(result), &actual_len);
        if (actual_len >= sizeof(result.status))
            hvdxg.statistics_last_status = result.status.v;
        if (ret == 0 && actual_len >= sizeof(result.status))
            ret = hvdxg_ntstatus_to_errno(result.status);
        if (ret == 0 && actual_len >= sizeof(result)) {
            req.result = result.result;
            ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
                  -EFAULT : 0;
        }
        hvdxg.statistics_last_len = actual_len;
        hvdxg.statistics_last_ret = ret;
        break;
    }

    case LX_DXCREATESYNCFILE: {
        struct d3dkmt_createsyncfile req;
        struct hvdxg_sync_file_object *sync_file = NULL;
        struct d3dkmt_waitforsynchronizationobjectfromcpu wait_req;
        struct hvdxg_d3dkmthandle wait_object;
        uint32 host_nt_handle = 0;
        uint32 host_object = 0;
        uint32 used_host_object = 0;
        uint32 process = 0;
        uint32 actual_len = 0;
        uint64 event_id = 0;
        int fd = -1;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.syncfile_last_cmd = (uint32)cmd;
        hvdxg.syncfile_last_device = req.device.v;
        hvdxg.syncfile_last_object = req.monitored_fence.v;
        hvdxg.syncfile_last_context = 0;
        hvdxg.syncfile_last_handle = req.sync_file_handle;
        hvdxg.syncfile_last_fence = req.fence_value;
        hvdxg.syncfile_last_global = 0;
        hvdxg.syncfile_last_host_nt = 0;
        hvdxg.syncfile_last_source_flags = 0;
        hvdxg.syncfile_last_open_flags = 0;
        hvdxg.syncfile_last_len = 0;
        hvdxg.syncfile_last_status = 0;
        hvdxg.syncfile_last_out_sync = 0;
        hvdxg.syncfile_last_cpu_va = 0;
        hvdxg.syncfile_last_gpu_va = 0;
        if (req.device.v == 0 || req.monitored_fence.v == 0 ||
            !hvdxg_owner_has_device(owner, req.device.v) ||
            !hvdxg_owner_has_sync(owner, req.monitored_fence.v)) {
            ret = -EINVAL;
            hvdxg.syncfile_last_ret = ret;
            break;
        }
        if (!hvdxg_owner_sync_is_monitored(owner, req.monitored_fence.v) ||
            hvdxg_owner_sync_global_shared(owner, req.monitored_fence.v) == 0) {
            ret = -EINVAL;
            hvdxg.syncfile_last_ret = ret;
            break;
        }
        host_object = hvdxg_owner_host_object_handle(
            owner, HV_DXG_OBJECT_SYNC, req.monitored_fence.v, NULL);
        if (host_object == 0)
            host_object = req.monitored_fence.v;
        process = hvdxg_owner_bound_process_handle(owner).v;
        if (!hvdxg_ntshared_cache_get(HV_DXG_SHARED_OBJECT_SYNC, process,
                                      host_object, &host_nt_handle)) {
            ret = hvdxg_create_nt_shared_object(
                process, host_object,
                hvdxg_owner_sync_global_shared(owner,
                                               req.monitored_fence.v),
                &used_host_object, &host_nt_handle, 0);
            if (ret == 0) {
                if (used_host_object != 0)
                    host_object = used_host_object;
                hvdxg_ntshared_cache_insert(HV_DXG_SHARED_OBJECT_SYNC,
                                            process, host_object,
                                            host_nt_handle);
            }
        }
        if (ret != 0) {
            hvdxg.syncfile_last_ret = ret;
            break;
        }
        event_id = hvdxg_alloc_host_event();
        if (event_id == 0) {
            ret = -ENOMEM;
            hvdxg_release_nt_shared_object_ref(HV_DXG_SHARED_OBJECT_SYNC,
                                               process, host_object,
                                               host_nt_handle);
            hvdxg.syncfile_last_ret = ret;
            break;
        }
        memset(&wait_req, 0, sizeof(wait_req));
        memset(&wait_object, 0, sizeof(wait_object));
        wait_object.v = req.monitored_fence.v;
        wait_req.device = req.device;
        wait_req.object_count = 1;
        ret = hvdxg_send_waitsyncobjectfromcpu(
            &wait_req, &wait_object, &req.fence_value, event_id,
            sizeof(wait_object), sizeof(req.fence_value), &actual_len);
        hvdxg.syncfile_last_len = actual_len;
        hvdxg.syncfile_last_status = hvdxg.syncwait_last_status;
        if (ret != 0 && ret != HV_DXG_STATUS_PENDING) {
            hvdxg_remove_host_event(event_id);
            hvdxg_release_nt_shared_object_ref(HV_DXG_SHARED_OBJECT_SYNC,
                                               process, host_object,
                                               host_nt_handle);
            hvdxg.syncfile_last_ret = ret;
            break;
        }
        sync_file = kvmalloc(sizeof(*sync_file));
        if (sync_file == NULL) {
            hvdxg_remove_host_event(event_id);
            hvdxg_release_nt_shared_object_ref(HV_DXG_SHARED_OBJECT_SYNC,
                                               process, host_object,
                                               host_nt_handle);
            ret = -ENOMEM;
            hvdxg.syncfile_last_ret = ret;
            break;
        }
        memset(sync_file, 0, sizeof(*sync_file));
        sync_file->device = req.device.v;
        sync_file->sync_object = req.monitored_fence.v;
        sync_file->global_share =
            hvdxg_owner_sync_global_shared(owner, req.monitored_fence.v);
        sync_file->host_nt_handle = host_nt_handle;
        sync_file->cache_process = process;
        sync_file->cache_object = host_object;
        sync_file->sync_type =
            hvdxg_owner_sync_type(owner, req.monitored_fence.v);
        sync_file->sync_flags =
            hvdxg_owner_sync_flags(owner, req.monitored_fence.v);
        sync_file->fence_value = req.fence_value;
        sync_file->event_id = event_id;
        fd = vfs_custom_fd_alloc(&hvdxg_sync_file_ops, sync_file, O_RDWR);
        if (fd < 0) {
            hvdxg_remove_host_event(event_id);
            hvdxg_release_nt_shared_object_ref(HV_DXG_SHARED_OBJECT_SYNC,
                                               process, host_object,
                                               host_nt_handle);
            kvfree(sync_file);
            ret = fd;
            hvdxg.syncfile_last_ret = ret;
            break;
        }
        if (current != NULL && current->fdtable != NULL) {
            spin_lock(&current->fdtable->lock);
            (void)vfs_fdtable_set_fdflags(current->fdtable, fd, FD_CLOEXEC);
            spin_unlock(&current->fdtable->lock);
        }
        req.sync_file_handle = (uint64)fd;
        hvdxg.syncfile_last_handle = req.sync_file_handle;
        hvdxg.syncfile_last_global = sync_file->global_share;
        hvdxg.syncfile_last_host_nt = sync_file->host_nt_handle;
        hvdxg.syncfile_last_source_flags = sync_file->sync_flags;
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        if (ret != 0) {
            struct vfs_file *f;

            spin_lock(&current->fdtable->lock);
            f = vfs_fdtable_dealloc_fd(current->fdtable, fd);
            spin_unlock(&current->fdtable->lock);
            if (f != NULL) {
                vfs_file_maybe_last_fd_close(f);
                vfs_fput(f);
            }
        }
        hvdxg.syncfile_last_ret = ret;
        break;
    }

    case LX_DXWAITSYNCFILE: {
        struct d3dkmt_waitsyncfile req;
        struct hvdxg_sync_file_object *sync_file;
        struct vfs_file *sync_file_vfs = NULL;
        struct hvdxg_d3dkmthandle opened;
        struct hvdxg_command_waitsyncobjectfromgpu *wait;
        uint8 command_buf[sizeof(struct hvdxg_command_waitsyncobjectfromgpu) +
                          sizeof(uint64) +
                          sizeof(struct hvdxg_d3dkmthandle)];
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 device;
        uint64 cpu_va = 0;
        uint64 gpu_va = 0;
        uint8 *pos;
        struct d3dddi_synchronizationobject_flags open_flags;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.syncfile_last_cmd = (uint32)cmd;
        hvdxg.syncfile_last_device = 0;
        hvdxg.syncfile_last_object = 0;
        hvdxg.syncfile_last_context = req.context.v;
        hvdxg.syncfile_last_handle = req.sync_file_handle;
        hvdxg.syncfile_last_fence = 0;
        hvdxg.syncfile_last_global = 0;
        hvdxg.syncfile_last_host_nt = 0;
        hvdxg.syncfile_last_source_flags = 0;
        hvdxg.syncfile_last_open_flags = 0;
        hvdxg.syncfile_last_len = 0;
        hvdxg.syncfile_last_status = 0;
        hvdxg.syncfile_last_out_sync = 0;
        hvdxg.syncfile_last_cpu_va = 0;
        hvdxg.syncfile_last_gpu_va = 0;
        if (req.context.v == 0 ||
            !hvdxg_owner_has_context(owner, req.context.v)) {
            ret = -EINVAL;
            hvdxg.syncfile_last_ret = ret;
            break;
        }
        sync_file = hvdxg_sync_file_from_fd((int)req.sync_file_handle,
                                            &sync_file_vfs);
        if (sync_file == NULL || sync_file->global_share == 0) {
            ret = -EINVAL;
            hvdxg.syncfile_last_ret = ret;
            if (sync_file_vfs != NULL)
                vfs_fput(sync_file_vfs);
            break;
        }
        device = hvdxg_owner_object_device(owner, HV_DXG_OBJECT_CONTEXT,
                                           req.context.v);
        if (device == 0) {
            ret = -EINVAL;
            hvdxg.syncfile_last_ret = ret;
            vfs_fput(sync_file_vfs);
            break;
        }
        memset(&open_flags, 0, sizeof(open_flags));
        open_flags.shared = 1;
        open_flags.nt_security_sharing = 1;
        open_flags.no_signal = 1;
        ret = hvdxg_open_sync_file_on_device(
            owner, device, sync_file, open_flags, 0, 0, &opened,
            NULL, NULL, NULL);
        hvdxg.syncfile_last_device = device;
        hvdxg.syncfile_last_object = opened.v;
        hvdxg.syncfile_last_fence = sync_file->fence_value;
        hvdxg.syncfile_last_global = sync_file->global_share;
        hvdxg.syncfile_last_host_nt = sync_file->host_nt_handle;
        hvdxg.syncfile_last_source_flags = sync_file->sync_flags;
        hvdxg.syncfile_last_open_flags = open_flags.value;
        hvdxg.syncfile_last_len = hvdxg.opensync_last_actual_len;
        hvdxg.syncfile_last_status = hvdxg.opensync_last_status;
        hvdxg.syncfile_last_out_sync = opened.v;
        hvdxg.syncfile_last_cpu_va = cpu_va;
        hvdxg.syncfile_last_gpu_va = gpu_va;
        if (ret != 0) {
            hvdxg.syncfile_last_ret = ret;
            vfs_fput(sync_file_vfs);
            break;
        }
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        wait = (struct hvdxg_command_waitsyncobjectfromgpu *)command_buf;
        hvdxg_command_vgpu_init_process(
            &wait->hdr, HV_DXGK_VMBCOMMAND_WAITFORSYNCOBJECTFROMGPU,
            hvdxg.dxg_process);
        wait->context.v = req.context.v;
        wait->object_count = 1;
        wait->legacy_fence_object = 0;
        pos = (uint8 *)wait->fence_values;
        memcpy(pos, &sync_file->fence_value, sizeof(sync_file->fence_value));
        pos += sizeof(sync_file->fence_value);
        memcpy(pos, &opened, sizeof(opened));
        ret = hvdxg_send_sync_vgpu(
            wait, sizeof(*wait) - sizeof(uint64) +
                      sizeof(sync_file->fence_value) + sizeof(opened),
            &status, sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.syncfile_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.syncfile_last_len = actual_len;
        if (opened.v != 0) {
            struct hvdxg_command_destroysyncobject destroy;
            struct hvdxg_ntstatus destroy_status;
            uint32 destroy_len = 0;
            int destroy_ret;

            memset(&destroy, 0, sizeof(destroy));
            memset(&destroy_status, 0, sizeof(destroy_status));
            hvdxg_command_vm_init(&destroy.hdr,
                                  HV_DXGK_VMBCOMMAND_DESTROYSYNCOBJECT);
            destroy.hdr.process = hvdxg_owner_bound_process_handle(owner);
            destroy.sync_object = opened;
            destroy_ret = hvdxg_send_sync_global(
                &destroy, sizeof(destroy), &destroy_status,
                sizeof(destroy_status), &destroy_len);
            if (destroy_ret == 0 && destroy_len >= sizeof(destroy_status))
                destroy_ret = hvdxg_ntstatus_to_errno(destroy_status);
            hvdxg.destroysync_last_handle = opened.v;
            hvdxg.destroysync_last_device = device;
            hvdxg.destroysync_last_type = sync_file->sync_type;
            hvdxg.destroysync_last_flags = open_flags.value;
            hvdxg.destroysync_last_global = sync_file->global_share;
            hvdxg.destroysync_last_monitor_fence = 0;
            hvdxg.destroysync_last_cmd_len = hvdxg.global_send_last_cmd_len;
            hvdxg.destroysync_last_wire_len = hvdxg.global_send_last_wire_len;
            hvdxg.destroysync_last_ext = hvdxg.global_send_last_ext;
            hvdxg.destroysync_last_ext_offset =
                hvdxg.global_send_last_ext_offset;
            hvdxg.destroysync_last_len = destroy_len;
            hvdxg.destroysync_last_status = destroy_status.v;
            hvdxg.destroysync_last_ret = destroy_ret;
        }
        vfs_fput(sync_file_vfs);
        hvdxg.syncfile_last_ret = ret;
        break;
    }

    case LX_DXOPENSYNCOBJECTFROMSYNCFILE: {
        struct d3dkmt_opensyncobjectfromsyncfile req;
        struct hvdxg_sync_file_object *sync_file;
        struct vfs_file *sync_file_vfs = NULL;
        struct hvdxg_d3dkmthandle opened;
        uint64 cpu_va = 0;
        uint64 gpu_va = 0;
        uint64 fence_kva = 0;
        struct d3dddi_synchronizationobject_flags open_flags;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.syncfile_last_cmd = (uint32)cmd;
        hvdxg.syncfile_last_device = req.device.v;
        hvdxg.syncfile_last_object = req.syncobj.v;
        hvdxg.syncfile_last_context = 0;
        hvdxg.syncfile_last_handle = req.sync_file_handle;
        hvdxg.syncfile_last_fence = req.fence_value;
        hvdxg.syncfile_last_global = 0;
        hvdxg.syncfile_last_host_nt = 0;
        hvdxg.syncfile_last_source_flags = 0;
        hvdxg.syncfile_last_open_flags = 0;
        hvdxg.syncfile_last_len = 0;
        hvdxg.syncfile_last_status = 0;
        hvdxg.syncfile_last_out_sync = 0;
        hvdxg.syncfile_last_cpu_va = 0;
        hvdxg.syncfile_last_gpu_va = 0;
        if (req.device.v == 0 ||
            !hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EINVAL;
            hvdxg.syncfile_last_ret = ret;
            break;
        }
        sync_file = hvdxg_sync_file_from_fd((int)req.sync_file_handle,
                                            &sync_file_vfs);
        if (sync_file == NULL || sync_file->global_share == 0) {
            ret = -EINVAL;
            hvdxg.syncfile_last_ret = ret;
            if (sync_file_vfs != NULL)
                vfs_fput(sync_file_vfs);
            break;
        }
        memset(&open_flags, 0, sizeof(open_flags));
        open_flags.shared = 1;
        open_flags.nt_security_sharing = 1;
        open_flags.no_signal = 1;
        ret = hvdxg_open_sync_file_on_device(
            owner, req.device.v, sync_file, open_flags, 0, 1, &opened,
            &cpu_va, &gpu_va, &fence_kva);
        hvdxg.syncfile_last_global = sync_file->global_share;
        hvdxg.syncfile_last_host_nt = sync_file->host_nt_handle;
        hvdxg.syncfile_last_source_flags = sync_file->sync_flags;
        hvdxg.syncfile_last_open_flags = open_flags.value;
        hvdxg.syncfile_last_len = hvdxg.opensync_last_actual_len;
        hvdxg.syncfile_last_status = hvdxg.opensync_last_status;
        hvdxg.syncfile_last_out_sync = opened.v;
        hvdxg.syncfile_last_cpu_va = cpu_va;
        hvdxg.syncfile_last_gpu_va = gpu_va;
        if (ret == 0) {
            req.syncobj.v = opened.v;
            req.fence_value = sync_file->fence_value;
            req.fence_value_cpu_va = cpu_va;
            req.fence_value_gpu_va = gpu_va;
            ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
                  -EFAULT : 0;
        }
        if (ret == 0 && owner != NULL)
            hvdxg_track_sync(owner, req.device.v, req.syncobj.v,
                             sync_file->sync_type, open_flags.value,
                             sync_file->global_share, cpu_va, fence_kva, 0);
        vfs_fput(sync_file_vfs);
        hvdxg.syncfile_last_object = req.syncobj.v;
        hvdxg.syncfile_last_fence = req.fence_value;
        hvdxg.syncfile_last_ret = ret;
        break;
    }

    case LX_DXCHANGEVIDEOMEMORYRESERVATION: {
        struct d3dkmt_changevideomemoryreservation req;
        struct d3dkmt_changevideomemoryreservation wire_req;
        struct hvdxg_command_changevideomemoryreservation change;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 host_adapter;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.process != 0 ||
            hvdxg_resolve_adapter_handle(owner, req.adapter.v,
                                         &host_adapter) != 0) {
            ret = -EINVAL;
            break;
        }
        wire_req = req;
        wire_req.adapter.v = 0;
        wire_req.process = 0;
        memset(&change, 0, sizeof(change));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(
            &change.hdr, HV_DXGK_VMBCOMMAND_CHANGEVIDEOMEMORYRESERVATION,
            hvdxg.dxg_process);
        change.args = wire_req;
        ret = hvdxg_send_sync_vgpu(&change, sizeof(change), &status,
                                   sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.vidmem_reservation_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.vidmem_reservation_last_len = actual_len;
        hvdxg.vidmem_reservation_last_ret = ret;
        hvdxg.vidmem_reservation_last_group = req.memory_segment_group;
        hvdxg.vidmem_reservation_last_value = req.reservation;
        break;
    }

    case LX_DXMARKDEVICEASERROR: {
        struct d3dkmt_markdeviceaserror req;
        struct hvdxg_command_markdeviceaserror mark;
        struct hvdxg_ntstatus status;
        struct hvdxg_d3dkmthandle process;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.markdevice_last_len = 0;
        hvdxg.markdevice_last_ret = 0;
        hvdxg.markdevice_last_status = 0;
        hvdxg.markdevice_last_device = req.device.v;
        hvdxg.markdevice_last_reason = (uint32)req.reason;
        hvdxg.markdevice_last_cmd_len = sizeof(mark);
        process.v = hvdxg_open_host_process(owner);
        if (process.v == 0)
            process = hvdxg.dxg_process;
        hvdxg.markdevice_last_process = process.v;
        if (req.device.v == 0 ||
            !hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EINVAL;
            hvdxg.markdevice_last_ret = ret;
            break;
        }
        memset(&mark, 0, sizeof(mark));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(
            &mark.hdr, HV_DXGK_VMBCOMMAND_MARKDEVICEASERROR, process);
        mark.args = req;
        ret = hvdxg_send_sync_vgpu(&mark, sizeof(mark), &status,
                                   sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.markdevice_last_status = status.v;
        if (ret == 0 && actual_len < sizeof(status))
            ret = -EOVERFLOW;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.markdevice_last_len = actual_len;
        hvdxg.markdevice_last_ret = ret;
        break;
    }

    case LX_DXSUBMITSIGNALSYNCOBJECTSTOHWQUEUE: {
        struct d3dkmt_submitsignalsyncobjectstohwqueue req;
        uint8 command_buf[sizeof(struct hvdxg_command_signalsyncobject) +
                          D3DDDI_MAX_OBJECT_SIGNALED *
                              (sizeof(struct hvdxg_d3dkmthandle) +
                               sizeof(uint64)) +
                          D3DDDI_MAX_BROADCAST_CONTEXT *
                              sizeof(struct hvdxg_d3dkmthandle)];
        struct hvdxg_command_signalsyncobject *signal =
            (struct hvdxg_command_signalsyncobject *)command_buf;
        struct hvdxg_ntstatus status;
        struct hvdxg_d3dkmthandle hwqueues[D3DDDI_MAX_BROADCAST_CONTEXT];
        struct hvdxg_d3dkmthandle objects[D3DDDI_MAX_OBJECT_SIGNALED];
        uint64 fences[D3DDDI_MAX_OBJECT_SIGNALED];
        uint32 actual_len = 0;
        uint32 object_size;
        uint32 hwqueue_size;
        uint32 fence_size;
        uint8 *pos;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.hwqueue_count == 0 ||
            req.hwqueue_count > D3DDDI_MAX_BROADCAST_CONTEXT ||
            req.object_count == 0 ||
            req.object_count > D3DDDI_MAX_OBJECT_SIGNALED ||
            req.hwqueues == 0 || req.objects == 0 ||
            req.fence_values == 0) {
            ret = -EINVAL;
            break;
        }
        hwqueue_size = req.hwqueue_count *
                       sizeof(struct hvdxg_d3dkmthandle);
        object_size = req.object_count * sizeof(struct hvdxg_d3dkmthandle);
        fence_size = req.object_count * sizeof(uint64);
        if (either_copyin(hwqueues, 1, req.hwqueues, hwqueue_size) < 0 ||
            either_copyin(objects, 1, req.objects, object_size) < 0 ||
            either_copyin(fences, 1, req.fence_values, fence_size) < 0) {
            ret = -EFAULT;
            break;
        }
        for (uint32 i = 0; i < req.hwqueue_count; i++) {
            if (!hvdxg_owner_has_hwqueue(owner, hwqueues[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        for (uint32 i = 0; ret == 0 && i < req.object_count; i++) {
            if (!hvdxg_owner_has_sync(owner, objects[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            break;
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg.syncgpu_signal_last_status = 0;
        hvdxg_command_vgpu_init_process(&signal->hdr,
                                        HV_DXGK_VMBCOMMAND_SIGNALSYNCOBJECT,
                                        hvdxg.dxg_process);
        signal->object_count = req.object_count;
        signal->flags = req.flags;
        signal->context_count = req.hwqueue_count;
        signal->fence_value = 0;
        signal->u.device.v = 0;
        pos = (uint8 *)&signal[1];
        memcpy(pos, objects, object_size);
        pos += object_size;
        memcpy(pos, hwqueues, hwqueue_size);
        pos += hwqueue_size;
        memcpy(pos, fences, fence_size);
        ret = hvdxg_send_sync_vgpu(signal,
                                   sizeof(*signal) + object_size +
                                       hwqueue_size + fence_size,
                                   &status, sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.syncgpu_signal_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.syncgpu_signal_last_len = actual_len;
        hvdxg.syncgpu_signal_last_ret = ret;
        break;
    }

    case LX_DXSUBMITWAITFORSYNCOBJECTSTOHWQUEUE: {
        struct d3dkmt_submitwaitforsyncobjectstohwqueue req;
        uint8 command_buf[sizeof(struct hvdxg_command_waitsyncobjectfromgpu) +
                          D3DDDI_MAX_OBJECT_WAITED_ON *
                              (sizeof(struct hvdxg_d3dkmthandle) +
                               sizeof(uint64))];
        struct hvdxg_command_waitsyncobjectfromgpu *wait =
            (struct hvdxg_command_waitsyncobjectfromgpu *)command_buf;
        struct hvdxg_ntstatus status;
        struct hvdxg_d3dkmthandle objects[D3DDDI_MAX_OBJECT_WAITED_ON];
        uint64 fences[D3DDDI_MAX_OBJECT_WAITED_ON];
        uint32 actual_len = 0;
        uint32 object_size;
        uint32 fence_size;
        uint8 *pos;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.hwqueue.v == 0 || req.object_count == 0 ||
            req.object_count > D3DDDI_MAX_OBJECT_WAITED_ON ||
            req.objects == 0 || req.fence_values == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_hwqueue(owner, req.hwqueue.v)) {
            ret = -EPERM;
            break;
        }
        object_size = req.object_count * sizeof(struct hvdxg_d3dkmthandle);
        fence_size = req.object_count * sizeof(uint64);
        if (either_copyin(objects, 1, req.objects, object_size) < 0 ||
            either_copyin(fences, 1, req.fence_values, fence_size) < 0) {
            ret = -EFAULT;
            break;
        }
        for (uint32 i = 0; i < req.object_count; i++) {
            if (!hvdxg_owner_has_sync(owner, objects[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            break;
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg.syncgpu_wait_last_status = 0;
        hvdxg_command_vgpu_init_process(
            &wait->hdr, HV_DXGK_VMBCOMMAND_WAITFORSYNCOBJECTFROMGPU,
            hvdxg.dxg_process);
        wait->context.v = req.hwqueue.v;
        wait->object_count = req.object_count;
        wait->legacy_fence_object = 0;
        pos = (uint8 *)wait->fence_values;
        memcpy(pos, fences, fence_size);
        pos += fence_size;
        memcpy(pos, objects, object_size);
        ret = hvdxg_send_sync_vgpu(wait,
                                   sizeof(*wait) - sizeof(uint64) +
                                       fence_size + object_size,
                                   &status, sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.syncgpu_wait_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.syncgpu_wait_last_len = actual_len;
        hvdxg.syncgpu_wait_last_ret = ret;
        break;
    }

    case LX_DXUPDATEALLOCPROPERTY: {
        struct d3dddi_updateallocproperty req;
        struct hvdxg_command_updateallocationproperty update;
        struct hvdxg_command_updateallocationproperty_return result;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.paging_queue.v == 0 || req.allocation.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_pagingqueue(owner, req.paging_queue.v) ||
            !hvdxg_owner_has_allocation(owner, 0, 0, req.allocation.v)) {
            ret = -EPERM;
            break;
        }
        memset(&update, 0, sizeof(update));
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(
            &update.hdr, HV_DXGK_VMBCOMMAND_UPDATEALLOCATIONPROPERTY,
            hvdxg.dxg_process);
        update.args = req;
        ret = hvdxg_send_sync_vgpu(&update, sizeof(update), &result,
                                   sizeof(result), &actual_len);
        if (actual_len >= sizeof(result))
            hvdxg.updateallocproperty_last_fence =
                result.paging_fence_value;
        if (actual_len >= sizeof(result.paging_fence_value) +
                          sizeof(result.status))
            hvdxg.updateallocproperty_last_status = result.status.v;
        if (ret == 0 &&
            actual_len >= sizeof(result.paging_fence_value) +
                          sizeof(result.status))
            ret = hvdxg_ntstatus_to_errno(result.status);
        hvdxg.updateallocproperty_last_len = actual_len;
        hvdxg.updateallocproperty_last_ret = ret;
        hvdxg.updateallocproperty_last_allocation = req.allocation.v;
        if ((ret == 0 || ret == HV_DXG_STATUS_PENDING) &&
            actual_len >= sizeof(result)) {
            req.paging_fence_value = result.paging_fence_value;
            if (either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0)
                ret = -EFAULT;
        }
        break;
    }

    case LX_DXQUERYCLOCKCALIBRATION: {
        struct d3dkmt_queryclockcalibration req;
        struct d3dkmt_queryclockcalibration wire_req;
        struct hvdxg_command_queryclockcalibration query;
        struct hvdxg_command_queryclockcalibration_return result;
        uint32 actual_len = 0;
        uint32 host_adapter;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (hvdxg_resolve_adapter_handle(owner, req.adapter.v,
                                         &host_adapter) != 0) {
            ret = -EINVAL;
            break;
        }
        wire_req = req;
        wire_req.adapter.v = host_adapter;
        memset(&query, 0, sizeof(query));
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(
            &query.hdr, HV_DXGK_VMBCOMMAND_QUERYCLOCKCALIBRATION,
            hvdxg.dxg_process);
        query.args = wire_req;
        ret = hvdxg_send_sync_vgpu(&query, sizeof(query), &result,
                                   sizeof(result), &actual_len);
        if (actual_len >= sizeof(result.status))
            hvdxg.clockcalibration_last_status = result.status.v;
        if (ret == 0 && actual_len >= sizeof(result))
            ret = hvdxg_ntstatus_to_errno(result.status);
        hvdxg.clockcalibration_last_len = actual_len;
        hvdxg.clockcalibration_last_ret = ret;
        hvdxg.clockcalibration_last_node = req.node_ordinal;
        hvdxg.clockcalibration_last_gpu_frequency =
            result.clock_data.gpu_frequency;
        hvdxg.clockcalibration_last_gpu_counter =
            result.clock_data.gpu_clock_counter;
        hvdxg.clockcalibration_last_cpu_counter =
            result.clock_data.cpu_clock_counter;
        if (ret == 0 && actual_len >= sizeof(result)) {
            req.clock_data = result.clock_data;
            ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
                  -EFAULT : 0;
        }
        break;
    }

    case LX_DXENUMPROCESSES: {
        struct d3dkmt_enumprocesses req;

        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg_note_unsupported_ioctl((uint32)cmd, 0, 0,
                                     (uint32)req.buffer_count);
        ret = -ENOTSUP;
        break;
    }

    case LX_DXCLOSEADAPTER: {
        struct d3dkmt_closeadapter req;
        uint32 host_adapter;

        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.closeadapter_ioctl_count++;
        ret = hvdxg_close_local_adapter_handle(owner, req.adapter_handle.v,
                                               &host_adapter);
        hvdxg.closeadapter_last_len = 0;
        hvdxg.closeadapter_last_ret = ret;
        hvdxg.closeadapter_last_status = 0;
        if (ret == 0) {
            uint32 order = ++hvdxg.cleanup_order_seq;

            hvdxg.closeadapter_local_count++;
            hvdxg.closeadapter_last_order = order;
            if (hvdxg.cleanup_last_destroy_order != 0 &&
                order > hvdxg.cleanup_last_destroy_order)
                hvdxg.closeadapter_after_destroy_count++;
        } else {
            hvdxg.closeadapter_invalid_count++;
        }
        break;
    }

    case LX_DXQUERYADAPTERINFO: {
        ret = hvdxg_ioctl_queryadapterinfo((uint64)arg, owner);
        break;
    }

    case LX_ISFEATUREENABLED: {
        struct d3dkmt_isfeatureenabled req;
        struct hvdxg_command_isfeatureenabled feature;
        struct hvdxg_command_isfeatureenabled_return result;
        uint32 actual_len = 0;
        uint32 host_adapter;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.feature_last_id = (uint32)req.feature_id;
        hvdxg.feature_last_result = 0;
        hvdxg.feature_last_status = 0;
        memset(&result, 0, sizeof(result));
        if (hvdxg_resolve_adapter_handle(owner, req.adapter.v,
                                         &host_adapter) != 0) {
            ret = -EINVAL;
            hvdxg.feature_last_len = 0;
            hvdxg.feature_last_ret = ret;
            break;
        }
        memset(&feature, 0, sizeof(feature));
        hvdxg_command_vgpu_init_process(
            &feature.hdr, HV_DXGK_VMBCOMMAND_ISFEATUREENABLED,
            hvdxg.dxg_process);
        feature.feature_id = req.feature_id;
        ret = hvdxg_send_sync_vgpu(&feature, sizeof(feature), &result,
                                   sizeof(result), &actual_len);
        if (actual_len >= sizeof(result.status))
            hvdxg.feature_last_status = result.status.v;
        if (ret == 0 && actual_len >= sizeof(result.status))
            ret = hvdxg_ntstatus_to_errno(result.status);
        if (ret == 0 && actual_len >= sizeof(result)) {
            req.result = result.result;
            hvdxg.feature_last_result = req.result.value;
            ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
                  -EFAULT : 0;
        }
        hvdxg.feature_last_len = actual_len;
        hvdxg.feature_last_ret = ret;
        break;
    }

    default:
        ret = -ENOTSUP;
        break;
    }

    hvdxg.ioctl_last_ret = ret;
    hvdxg_note_ioctl_history(cmd, ret);
    hvdxg_note_ioctl_timing(cmd, r_time() - start_ticks);
    if (ret >= 0)
        hvdxg.ioctl_successes++;
    if (process_locked)
        mutex_unlock(&hvdxg.process_lock);
    return ret;
}
