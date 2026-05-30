/*
 * Hyper-V DXG: QueryAdapter admission and adapter hardware-id resolution.
 *
 * Part of the dev/hyperv unity translation unit (included by module.c).
 * Split out of the former hyperv_dxg_state_diag.c for readability;
 * include order in module.c preserves the original definition order.
 */

static void hvdxg_write_utf16_string(uint8 *dst, uint32 dst_size,
                                     const char *string)
{
    uint16 *out = (uint16 *)dst;
    uint32 max_chars = dst_size / sizeof(uint16);
    uint32 i = 0;

    if (max_chars == 0)
        return;
    while (i + 1 < max_chars && string[i] != '\0') {
        out[i] = (uint16)string[i];
        i++;
    }
    out[i] = 0;
}

static int hvdxg_queryadapter_admission_type(uint32 type);
static void hvdxg_note_queryadapter_admission(uint32 kind, uint32 type,
                                              uint32 size, uint32 out_len,
                                              int32 ret, uint32 status,
                                              uint32 route, uint32 source,
                                              uint32 adapter,
                                              uint32 host_adapter,
                                              uint32 head_hash);

static void hvdxg_note_queryadapter_history(uint32 type, uint32 size,
                                            int32 ret, uint32 status)
{
    uint32 slot = hvdxg.queryadapter_history_index %
                  HV_DXG_QUERY_HISTORY_MAX;
    uint32 head_word = 0;

    hvdxg.queryadapter_history_type[slot] = type;
    hvdxg.queryadapter_history_size[slot] = size;
    hvdxg.queryadapter_history_len[slot] = hvdxg.queryadapter_last_len;
    hvdxg.queryadapter_history_ret[slot] = ret;
    hvdxg.queryadapter_history_status[slot] = status;
    hvdxg.queryadapter_history_route[slot] = hvdxg.queryadapter_send_route;
    hvdxg.queryadapter_history_adapter[slot] =
        hvdxg.queryadapter_last_adapter;
    hvdxg.queryadapter_history_host_adapter[slot] =
        hvdxg.queryadapter_last_host_adapter;
    hvdxg.queryadapter_history_head_len[slot] =
        hvdxg.queryadapter_return_head_len;
    memset(hvdxg.queryadapter_history_head[slot], 0,
           sizeof(hvdxg.queryadapter_history_head[slot]));
    if (hvdxg.queryadapter_return_head_len != 0)
        memcpy(hvdxg.queryadapter_history_head[slot],
               hvdxg.queryadapter_return_head,
               hvdxg.queryadapter_return_head_len);
    hvdxg.queryadapter_history_index++;
    for (uint32 i = 0; i < hvdxg.queryadapter_return_head_len && i < 4; i++)
        head_word |= (uint32)hvdxg.queryadapter_return_head[i] << (i * 8);
    if (hvdxg_queryadapter_admission_type(type))
        hvdxg_note_queryadapter_admission(
            HV_DXG_QAI_ADMISSION_KIND_QAI, type, size,
            hvdxg.queryadapter_last_len, ret, status,
            hvdxg.queryadapter_send_route, hvdxg.queryadapter_source_last,
            hvdxg.queryadapter_last_adapter,
            hvdxg.queryadapter_last_host_adapter, head_word);
}

static void hvdxg_reset_queryadapter_return_head(void)
{
    hvdxg.queryadapter_return_head_len = 0;
    memset(hvdxg.queryadapter_return_head, 0,
           sizeof(hvdxg.queryadapter_return_head));
}

static void hvdxg_note_queryadapter_return_head(const void *payload,
                                                uint32 payload_len)
{
    uint32 head_len = payload_len;

    if (head_len > sizeof(hvdxg.queryadapter_return_head))
        head_len = sizeof(hvdxg.queryadapter_return_head);
    hvdxg_reset_queryadapter_return_head();
    hvdxg.queryadapter_return_head_len = head_len;
    if (payload != NULL && head_len != 0)
        memcpy(hvdxg.queryadapter_return_head, payload, head_len);
}

static int hvdxg_queryadapter_admission_type(uint32 type)
{
    return type == HV_DXG_QAITYPE_SELECTED_ADAPTER ||
           type == HV_DXG_QAITYPE_PHASE1_TYPE27 ||
           type == _KMTQAITYPE_ADAPTERTYPE ||
           type == _KMTQAITYPE_PHYSICALADAPTERCOUNT ||
           type == HV_DXG_QAITYPE_ADAPTER_HARDWARE_ID ||
           type == HV_DXG_QAITYPE_CHECKDRIVERUPDATESTATUS_RENDER ||
           type == _KMTQAITYPE_ADAPTERTYPE_RENDER;
}

static void hvdxg_note_queryadapter_admission(uint32 kind, uint32 type,
                                              uint32 size, uint32 out_len,
                                              int32 ret, uint32 status,
                                              uint32 route, uint32 source,
                                              uint32 adapter,
                                              uint32 host_adapter,
                                              uint32 head_hash)
{
    uint32 slot = hvdxg.queryadapter_admission_history_index %
                  HV_DXG_QAI_ADMISSION_HISTORY_MAX;

    hvdxg.queryadapter_admission_kind[slot] = kind;
    hvdxg.queryadapter_admission_type[slot] = type;
    hvdxg.queryadapter_admission_size[slot] = size;
    hvdxg.queryadapter_admission_len[slot] = out_len;
    hvdxg.queryadapter_admission_ret[slot] = ret;
    hvdxg.queryadapter_admission_status[slot] = status;
    hvdxg.queryadapter_admission_route[slot] = route;
    hvdxg.queryadapter_admission_source[slot] = source;
    hvdxg.queryadapter_admission_adapter[slot] = adapter;
    hvdxg.queryadapter_admission_host[slot] = host_adapter;
    hvdxg.queryadapter_admission_head_hash[slot] = head_hash;
    hvdxg.queryadapter_admission_history_index++;
}

static uint32 hvdxg_read_u32(const void *ptr)
{
    uint32 value;

    memcpy(&value, ptr, sizeof(value));
    return value;
}

static int hvdxg_known_display_vendor(uint32 vendor)
{
    return vendor == HV_DXG_VENDOR_INTEL ||
           vendor == HV_DXG_VENDOR_NVIDIA ||
           vendor == 0x1002U;
}

static int hvdxg_sane_adapter_hardware_id(uint32 vendor, uint32 device)
{
    return vendor != 0 && vendor <= 0xffffU && device != 0;
}

static int hvdxg_synthetic_adapter_hardware_id(uint32 vendor, uint32 device)
{
    return vendor == PCI_VENDOR_MICROSOFT &&
           device == PCI_DEVICE_MS_VIRTUAL_RENDER;
}

static int hvdxg_real_adapter_hardware_id(uint32 vendor, uint32 device)
{
    return hvdxg_sane_adapter_hardware_id(vendor, device) &&
           !hvdxg_synthetic_adapter_hardware_id(vendor, device);
}

static int hvdxg_type31_active_v40_ext(void)
{
    return hvdxg.use_ext_header &&
           hvdxg.active_vmbus_version >= HV_DXG_VMBUS_INTERFACE_VERSION;
}

static int hvdxg_have_cached_adapter_hardware_id(void)
{
    return hvdxg.adapter_hardware_raw_size ==
               HV_DXG_PHYSICAL_ADAPTER_DEVICE_IDS_SIZE &&
           hvdxg_real_adapter_hardware_id(hvdxg.adapter_vendor_id,
                                          hvdxg.adapter_device_id);
}

static void hvdxg_note_adapter_hardware_v40_short(uint32 actual_len,
                                                  int ret, int32 status)
{
    hvdxg.adapter_hardware_v40_short_zero++;
    hvdxg.adapter_hardware_v40_short_len = actual_len;
    hvdxg.adapter_hardware_v40_short_ret = ret;
    hvdxg.adapter_hardware_v40_short_status = status;
    hvdxg.adapter_hardware_cache_available =
        hvdxg_have_cached_adapter_hardware_id() ? 1 : 0;
    if (hvdxg_synthetic_adapter_hardware_id(hvdxg.pci_vendor,
                                            hvdxg.pci_dxg_device))
        hvdxg.adapter_hardware_synthetic_rejected = 1;
}

static const char *hvdxg_adapter_hardware_source_name(void)
{
    if (hvdxg.adapter_hardware_fallback_source == 1)
        return "cached-real";
    if (hvdxg.adapter_hardware_fallback_source == 2)
        return "legacy-noext-vgpu-query";
    if (hvdxg.adapter_hardware_fallback_source == 3)
        return "temp-v27-openadapter";
    if (!hvdxg.adapter_hardware_fallback &&
        hvdxg_real_adapter_hardware_id(hvdxg.adapter_vendor_id,
                                       hvdxg.adapter_device_id)) {
        if (hvdxg_type31_active_v40_ext())
            return "host-v40-type31";
        return "host-real";
    }
    if (hvdxg.adapter_hardware_synthetic_rejected)
        return "rejected-synthetic-pci";
    return "none";
}

static int hvdxg_normalize_adapter_hardware_id(uint8 *private_data,
                                               uint32 private_data_size)
{
    uint32 words[7];
    uint32 count;
    uint32 vendor;
    uint32 device;

    hvdxg.adapter_hardware_raw_size = private_data_size;
    hvdxg.adapter_hardware_payload_len = private_data_size;
    hvdxg.adapter_hardware_cache_available =
        hvdxg_have_cached_adapter_hardware_id() ? 1 : 0;
    hvdxg.adapter_hardware_normalized = 0;
    hvdxg.adapter_hardware_fallback = 0;
    hvdxg.adapter_hardware_fallback_source = 0;
    if (private_data_size < 12 || private_data == NULL)
        return 0;

    count = private_data_size / sizeof(uint32);
    if (count > sizeof(words) / sizeof(words[0]))
        count = sizeof(words) / sizeof(words[0]);
    memset(words, 0, sizeof(words));
    for (uint32 i = 0; i < count; i++)
        words[i] = hvdxg_read_u32(private_data + i * sizeof(uint32));

    /*
     * Type-31 PHYSICALADAPTERDEVICEIDS is an indexed payload on WSL:
     * word 0 is the physical-adapter index, word 1 is VendorID, and word 2
     * is DeviceID.  Keep the host bytes unchanged for user mode; diagnostics
     * keep normalized vendor/device separately.
     */
    if (!hvdxg_known_display_vendor(words[0]) &&
        hvdxg_known_display_vendor(words[1])) {
        hvdxg.adapter_hardware_normalized = 1;
        vendor = words[1];
        device = words[2];
    } else {
        vendor = words[0];
        device = words[1];
    }

    if (!hvdxg_real_adapter_hardware_id(vendor, device)) {
        if (hvdxg_synthetic_adapter_hardware_id(vendor, device))
            hvdxg.adapter_hardware_synthetic_rejected = 1;
        return 0;
    }

    memset(hvdxg.adapter_hardware_raw, 0,
           sizeof(hvdxg.adapter_hardware_raw));
    for (uint32 i = 0; i < count; i++)
        hvdxg.adapter_hardware_raw[i] = words[i];
    hvdxg.adapter_vendor_id = vendor;
    hvdxg.adapter_device_id = device;
    hvdxg.adapter_hardware_cache_available = 1;
    return 1;
}

static int hvdxg_accept_legacy_adapter_hardware_id(uint8 *result_buf,
                                                   uint32 actual_len,
                                                   uint32 private_data_size)
{
    struct hvdxg_ntstatus *status;
    uint8 *payload;
    int status_ret;

    if (result_buf == NULL ||
        private_data_size != HV_DXG_PHYSICAL_ADAPTER_DEVICE_IDS_SIZE)
        return 0;

    if (actual_len >= private_data_size + sizeof(*status) &&
        hvdxg_ntstatus_plausible(*(struct hvdxg_ntstatus *)result_buf)) {
        status = (struct hvdxg_ntstatus *)result_buf;
        status_ret = hvdxg_ntstatus_to_errno(*status);
        if (status_ret != 0)
            return 0;
        payload = result_buf + sizeof(*status);
    } else if (actual_len >= private_data_size) {
        payload = result_buf;
    } else {
        return 0;
    }

    if (!hvdxg_normalize_adapter_hardware_id(payload, private_data_size))
        return 0;
    if (payload != result_buf)
        memmove(result_buf, payload, private_data_size);
    hvdxg.adapter_hardware_fallback = 1;
    hvdxg.adapter_hardware_fallback_count++;
    hvdxg.adapter_hardware_fallback_source = 2;
    return 1;
}

static int hvdxg_adapter_hardware_primary_too_short(uint8 *result_buf,
                                                    uint32 actual_len,
                                                    uint32 private_data_size)
{
    if (private_data_size != HV_DXG_PHYSICAL_ADAPTER_DEVICE_IDS_SIZE)
        return 0;
    if (actual_len < private_data_size)
        return 1;
    if (actual_len > private_data_size &&
        actual_len < private_data_size + sizeof(struct hvdxg_ntstatus) &&
        actual_len >= sizeof(struct hvdxg_ntstatus) &&
        hvdxg_ntstatus_plausible(*(struct hvdxg_ntstatus *)result_buf))
        return 1;
    return 0;
}

static int hvdxg_queryadapter_type55_zero_completion(uint32 type, uint32 size,
                                                     uint32 actual_len,
                                                     int ret)
{
    (void)size;
    return type == HV_DXG_QAITYPE_CHECKDRIVERUPDATESTATUS_RENDER &&
           actual_len == 0 && ret == 0;
}

static void hvdxg_note_queryadapter_zero_success(uint32 type, uint32 size,
                                                 uint32 host_len,
                                                 int host_ret,
                                                 int32 host_status,
                                                 int user_ret)
{
    hvdxg.queryadapter_zero_success_type = type;
    hvdxg.queryadapter_zero_success_size = size;
    hvdxg.queryadapter_zero_success_count++;
    hvdxg.queryadapter_zero_success_host_type = type;
    hvdxg.queryadapter_zero_success_host_len = host_len;
    hvdxg.queryadapter_zero_success_host_ret = host_ret;
    hvdxg.queryadapter_zero_success_host_status = host_status;
    hvdxg.queryadapter_zero_success_user_ret = user_ret;
}

static int hvdxg_queryadapter_adaptertype_zero_completion(uint32 type,
                                                          uint32 size,
                                                          uint32 actual_len,
                                                          int ret)
{
    return (type == _KMTQAITYPE_ADAPTERTYPE ||
            type == _KMTQAITYPE_ADAPTERTYPE_RENDER) &&
           size >= sizeof(struct d3dkmt_adaptertype) &&
           actual_len == 0 && ret == 0;
}

static int hvdxg_queryadapter_physicalcount_fallback(uint32 type,
                                                     uint32 size,
                                                     uint32 actual_len,
                                                     int ret)
{
    return type == _KMTQAITYPE_PHYSICALADAPTERCOUNT &&
           size >= sizeof(uint32) && actual_len == 0 && ret == -EOVERFLOW;
}

static int hvdxg_try_fallback_adapter_hardware_id(uint8 *private_data,
                                                  uint32 private_data_size)
{
    uint32 words[7];
    uint32 vendor;
    uint32 device;
    uint32 count;

    if (private_data == NULL ||
        private_data_size != HV_DXG_PHYSICAL_ADAPTER_DEVICE_IDS_SIZE)
        return 0;
    if (hvdxg_synthetic_adapter_hardware_id(hvdxg.pci_vendor,
                                            hvdxg.pci_dxg_device))
        hvdxg.adapter_hardware_synthetic_rejected = 1;
    hvdxg.adapter_hardware_cache_available =
        hvdxg_have_cached_adapter_hardware_id() ? 1 : 0;
    if (!hvdxg.adapter_hardware_cache_available)
        return 0;

    vendor = hvdxg.adapter_vendor_id;
    device = hvdxg.adapter_device_id;

    memset(words, 0, sizeof(words));
    count = sizeof(hvdxg.adapter_hardware_raw) /
            sizeof(hvdxg.adapter_hardware_raw[0]);
    for (uint32 i = 0; i < count; i++)
        words[i] = hvdxg.adapter_hardware_raw[i];
    if (hvdxg_known_display_vendor(words[0])) {
        words[6] = words[5];
        words[5] = words[4];
        words[4] = words[3];
        words[3] = words[2];
        words[2] = device;
        words[1] = vendor;
        words[0] = 0;
    }
    memcpy(private_data, words, sizeof(words));

    memset(hvdxg.adapter_hardware_raw, 0,
           sizeof(hvdxg.adapter_hardware_raw));
    for (uint32 i = 0; i < count; i++)
        hvdxg.adapter_hardware_raw[i] = words[i];
    hvdxg.adapter_hardware_raw_size = private_data_size;
    hvdxg.adapter_hardware_payload_len = private_data_size;
    hvdxg.adapter_hardware_normalized = 2;
    hvdxg.adapter_hardware_fallback = 1;
    hvdxg.adapter_hardware_fallback_count++;
    hvdxg.adapter_hardware_fallback_source = 1;
    hvdxg.adapter_vendor_id = vendor;
    hvdxg.adapter_device_id = device;
    return 1;
}

static int hvdxg_try_v27_openadapter_hardware_id(
    struct hvdxg_open_state *owner,
    struct hvdxg_command_queryadapterinfo_wsl *query, uint32 command_len,
    uint8 *result_buf, uint32 result_len, uint32 private_data_size,
    uint32 *actual_len)
{
    struct hvdxg_command_openadapter open;
    struct hvdxg_command_openadapter_return open_ret;
    struct hvdxg_command_closeadapter close;
    struct hvdxg_ntstatus close_status;
    uint8 temp_result[sizeof(struct hvdxg_ntstatus) +
                      HV_DXG_PHYSICAL_ADAPTER_DEVICE_IDS_SIZE];
    uint32 saved_host_adapter_handle = hvdxg.host_adapter_handle;
    struct hvdxg_winluid saved_adapter_luid = hvdxg.adapter_luid;
    struct hvdxg_winluid saved_host_adapter_luid = hvdxg.host_adapter_luid;
    struct hvdxg_winluid saved_host_vgpu_luid = hvdxg.host_vgpu_luid;
    uint32 saved_adapter_vendor_id = hvdxg.adapter_vendor_id;
    uint32 saved_adapter_device_id = hvdxg.adapter_device_id;
    uint32 saved_hardware_raw[7];
    uint32 saved_hardware_raw_size = hvdxg.adapter_hardware_raw_size;
    uint32 saved_hardware_normalized = hvdxg.adapter_hardware_normalized;
    uint32 saved_hardware_fallback = hvdxg.adapter_hardware_fallback;
    uint32 saved_hardware_fallback_count =
        hvdxg.adapter_hardware_fallback_count;
    uint32 saved_hardware_fallback_source =
        hvdxg.adapter_hardware_fallback_source;
    uint32 saved_hardware_synthetic_rejected =
        hvdxg.adapter_hardware_synthetic_rejected;
    uint32 saved_hardware_cache_available =
        hvdxg.adapter_hardware_cache_available;
    uint32 saved_hardware_payload_len =
        hvdxg.adapter_hardware_payload_len;
    uint32 temp_handle = 0;
    uint32 open_actual = 0;
    uint32 query_actual = 0;
    uint32 close_actual = 0;
    uint32 words[7];
    uint8 *payload = NULL;
    int success = 0;
    int ret = -EINVAL;

    memcpy(saved_hardware_raw, hvdxg.adapter_hardware_raw,
           sizeof(saved_hardware_raw));
    hvdxg.adapter_hardware_temp_v27_attempts++;
    hvdxg.adapter_hardware_temp_v27_last_ret = 0;
    hvdxg.adapter_hardware_temp_v27_last_status = 0;
    hvdxg.adapter_hardware_temp_v27_open_len = 0;
    hvdxg.adapter_hardware_temp_v27_query_len = 0;
    hvdxg.adapter_hardware_temp_v27_close_len = 0;
    hvdxg.adapter_hardware_temp_v27_close_ret = 0;
    hvdxg.adapter_hardware_temp_v27_close_status = 0;
    hvdxg.adapter_hardware_temp_v27_handle = 0;
    hvdxg.adapter_hardware_temp_v27_restored_version = 0;
    hvdxg.adapter_hardware_temp_v27_restored_ext = 0;

    if (query == NULL || result_buf == NULL || actual_len == NULL ||
        private_data_size != HV_DXG_PHYSICAL_ADAPTER_DEVICE_IDS_SIZE ||
        result_len < private_data_size + sizeof(struct hvdxg_ntstatus)) {
        ret = -EINVAL;
        goto restore;
    }
    if (!hvdxg.vgpu_open_ok || hvdxg.vgpu_out_ring == NULL ||
        hvdxg.dxg_process.v == 0) {
        ret = -ENODEV;
        goto restore;
    }
    if (owner != NULL &&
        (owner->device_count != 0 || owner->context_count != 0 ||
         owner->hwqueue_count != 0 || owner->allocation_count != 0)) {
        ret = -EBUSY;
        goto restore;
    }

    memset(&open, 0, sizeof(open));
    hvdxg_command_vgpu_init(&open.hdr, HV_DXGK_VMBCOMMAND_OPENADAPTER);
    open.vmbus_interface_version = HV_DXG_VMBUS_INTERFACE_VERSION_OLD;
    open.vmbus_last_compatible_interface_version =
        HV_DXG_VMBUS_LAST_COMPATIBLE_INTERFACE_VERSION;
    if (hvdxg_luid_nonzero(saved_adapter_luid))
        open.guest_adapter_luid = saved_adapter_luid;
    else
        open.guest_adapter_luid = hvdxg_luid_from_guid(&hvdxg.vgpu_instance);

    memset(&open_ret, 0, sizeof(open_ret));
    ret = hvdxg_send_sync_vgpu_flags(
        &open, sizeof(open), &open_ret, sizeof(open_ret), &open_actual,
        HV_DXG_SEND_SYNC_VGPU_F_NO_EXT_HEADER |
        HV_DXG_SEND_SYNC_VGPU_F_FORCE_VGPU);
    hvdxg.adapter_hardware_temp_v27_open_len = open_actual;
    if (ret != 0)
        goto restore;
    if (open_actual < sizeof(open_ret)) {
        ret = -EOVERFLOW;
        goto restore;
    }
    hvdxg.adapter_hardware_temp_v27_last_status = open_ret.status.v;
    if (open_ret.status.v < 0) {
        ret = hvdxg_ntstatus_to_errno(open_ret.status);
        goto close_temp;
    }
    temp_handle = open_ret.host_adapter_handle.v;
    hvdxg.adapter_hardware_temp_v27_handle = temp_handle;
    if (temp_handle == 0) {
        ret = -ENODEV;
        goto close_temp;
    }

    memset(temp_result, 0, sizeof(temp_result));
    ret = hvdxg_send_sync_vgpu_flags(
        query, command_len, temp_result, sizeof(temp_result), &query_actual,
        HV_DXG_SEND_SYNC_VGPU_F_NO_EXT_HEADER |
        HV_DXG_SEND_SYNC_VGPU_F_FORCE_VGPU);
    hvdxg.adapter_hardware_temp_v27_query_len = query_actual;
    if (actual_len != NULL)
        *actual_len = query_actual;
    if (query_actual >= sizeof(struct hvdxg_ntstatus) &&
        hvdxg_ntstatus_plausible(*(struct hvdxg_ntstatus *)temp_result))
        hvdxg.adapter_hardware_temp_v27_last_status =
            ((struct hvdxg_ntstatus *)temp_result)->v;
    if (ret != 0)
        goto close_temp;

    if (query_actual == private_data_size) {
        payload = temp_result;
    } else if (query_actual == private_data_size +
                            sizeof(struct hvdxg_ntstatus) &&
               hvdxg_ntstatus_plausible(
                   *(struct hvdxg_ntstatus *)temp_result)) {
        struct hvdxg_ntstatus *status =
            (struct hvdxg_ntstatus *)temp_result;

        ret = hvdxg_ntstatus_to_errno(*status);
        if (ret != 0)
            goto close_temp;
        payload = temp_result + sizeof(*status);
    } else {
        ret = query_actual < private_data_size ? -EOVERFLOW : -EINVAL;
        goto close_temp;
    }

    memset(words, 0, sizeof(words));
    for (uint32 i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        words[i] = hvdxg_read_u32(payload + i * sizeof(uint32));
    if (hvdxg_known_display_vendor(words[0])) {
        uint32 vendor = words[0];
        uint32 device = words[1];

        words[6] = words[5];
        words[5] = words[4];
        words[4] = words[3];
        words[3] = words[2];
        words[2] = device;
        words[1] = vendor;
        words[0] = 0;
    }
    memcpy(result_buf, words, sizeof(words));
    if (!hvdxg_normalize_adapter_hardware_id(result_buf,
                                             private_data_size)) {
        ret = -EINVAL;
        goto close_temp;
    }
    success = 1;
    ret = 0;

close_temp:
    if (temp_handle != 0 && temp_handle != saved_host_adapter_handle) {
        memset(&close, 0, sizeof(close));
        memset(&close_status, 0, sizeof(close_status));
        hvdxg_command_vgpu_init(&close.hdr,
                                HV_DXGK_VMBCOMMAND_CLOSEADAPTER);
        close.host_handle.v = temp_handle;
        hvdxg.adapter_hardware_temp_v27_close_ret =
            hvdxg_send_sync_vgpu_flags(
                &close, sizeof(close), &close_status, sizeof(close_status),
                &close_actual,
                HV_DXG_SEND_SYNC_VGPU_F_NO_EXT_HEADER |
                HV_DXG_SEND_SYNC_VGPU_F_FORCE_VGPU);
        hvdxg.adapter_hardware_temp_v27_close_len = close_actual;
        if (close_actual >= sizeof(close_status))
            hvdxg.adapter_hardware_temp_v27_close_status =
                close_status.v;
    }

restore:
    hvdxg.host_adapter_handle = saved_host_adapter_handle;
    hvdxg.adapter_luid = saved_adapter_luid;
    hvdxg.host_adapter_luid = saved_host_adapter_luid;
    hvdxg.host_vgpu_luid = saved_host_vgpu_luid;
    hvdxg.adapter_vendor_id = saved_adapter_vendor_id;
    hvdxg.adapter_device_id = saved_adapter_device_id;
    memcpy(hvdxg.adapter_hardware_raw, saved_hardware_raw,
           sizeof(hvdxg.adapter_hardware_raw));
    hvdxg.adapter_hardware_raw_size = saved_hardware_raw_size;
    hvdxg.adapter_hardware_normalized = saved_hardware_normalized;
    hvdxg.adapter_hardware_fallback = saved_hardware_fallback;
    hvdxg.adapter_hardware_fallback_count =
        saved_hardware_fallback_count;
    hvdxg.adapter_hardware_fallback_source =
        saved_hardware_fallback_source;
    hvdxg.adapter_hardware_synthetic_rejected =
        saved_hardware_synthetic_rejected;
    hvdxg.adapter_hardware_cache_available =
        saved_hardware_cache_available;
    hvdxg.adapter_hardware_payload_len =
        saved_hardware_payload_len;
    hvdxg.adapter_hardware_temp_v27_restored_version =
        hvdxg.active_vmbus_version;
    hvdxg.adapter_hardware_temp_v27_restored_ext =
        hvdxg.use_ext_header;

    if (success) {
        if (!hvdxg_normalize_adapter_hardware_id(result_buf,
                                                 private_data_size)) {
            ret = -EINVAL;
            success = 0;
        } else {
            hvdxg.adapter_hardware_fallback = 1;
            hvdxg.adapter_hardware_fallback_count =
                saved_hardware_fallback_count + 1;
            hvdxg.adapter_hardware_fallback_source = 3;
        }
    }
    hvdxg.adapter_hardware_temp_v27_last_ret = ret;
    if (success)
        hvdxg.adapter_hardware_temp_v27_successes++;
    else
        hvdxg.adapter_hardware_temp_v27_failures++;
    return success;
}

static int hvdxg_try_adapter_hardware_failure_fallbacks(
    struct hvdxg_open_state *owner,
    struct hvdxg_command_queryadapterinfo_wsl *query, uint32 command_len,
    uint8 *result_buf, uint32 result_len, uint32 private_data_size,
    uint32 *actual_len, int allow_cached, int allow_temp_v27)
{
    uint32 temp_actual = 0;

    if (allow_cached &&
        hvdxg_try_fallback_adapter_hardware_id(result_buf,
                                               private_data_size)) {
        if (actual_len != NULL)
            *actual_len = private_data_size;
        return 1;
    }
    if (allow_temp_v27 &&
        hvdxg_try_v27_openadapter_hardware_id(owner, query, command_len,
            result_buf, result_len, private_data_size, &temp_actual)) {
        if (actual_len != NULL)
            *actual_len = private_data_size;
        return 1;
    }
    return 0;
}

