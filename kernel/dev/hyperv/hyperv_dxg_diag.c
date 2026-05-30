/*
 * Hyper-V DXG: histories, timing, byte helpers, caches, shared-resource metadata, d3d12 capture, status formatting.
 *
 * Part of the dev/hyperv unity translation unit (included by module.c).
 * Split out of the former hyperv_dxg_state_diag.c for readability;
 * include order in module.c preserves the original definition order.
 */

static void hvdxg_note_allocation_history(uint32 len, int32 ret,
                                          uint32 device, uint32 resource,
                                          uint32 allocation, uint64 size,
                                          uint32 count, uint32 priv_size,
                                          uint32 global_share,
                                          uint32 create_flags)
{
    uint32 slot = hvdxg.allocation_history_index %
                  HV_DXG_RESOURCE_HISTORY_MAX;

    hvdxg.allocation_history_len[slot] = len;
    hvdxg.allocation_history_ret[slot] = ret;
    hvdxg.allocation_history_device[slot] = device;
    hvdxg.allocation_history_resource[slot] = resource;
    hvdxg.allocation_history_allocation[slot] = allocation;
    hvdxg.allocation_history_size[slot] = size;
    hvdxg.allocation_history_count[slot] = count;
    hvdxg.allocation_history_priv[slot] = priv_size;
    hvdxg.allocation_history_global_share[slot] = global_share;
    hvdxg.allocation_history_create_flags[slot] = create_flags;
    hvdxg.allocation_history_index++;
}

static void hvdxg_note_mapgpuva_history(uint32 len, int32 ret, uint32 status,
                                        uint32 paging_queue,
                                        uint32 allocation, uint64 pages,
                                        uint64 protection,
                                        uint64 driver_protection,
                                        uint64 va, uint64 fence)
{
    uint32 slot = hvdxg.mapgpuva_history_index %
                  HV_DXG_RESOURCE_HISTORY_MAX;

    hvdxg.mapgpuva_history_len[slot] = len;
    hvdxg.mapgpuva_history_ret[slot] = ret;
    hvdxg.mapgpuva_history_status[slot] = status;
    hvdxg.mapgpuva_history_paging_queue[slot] = paging_queue;
    hvdxg.mapgpuva_history_allocation[slot] = allocation;
    hvdxg.mapgpuva_history_pages[slot] = pages;
    hvdxg.mapgpuva_history_protection[slot] = protection;
    hvdxg.mapgpuva_history_driver_protection[slot] = driver_protection;
    hvdxg.mapgpuva_history_va[slot] = va;
    hvdxg.mapgpuva_history_fence[slot] = fence;
    hvdxg.mapgpuva_history_index++;
}

static void hvdxg_note_submithwqueue_history(uint32 len, int32 ret,
                                             uint32 queue,
                                             uint32 command_length,
                                             uint32 priv_size,
                                             const uint8 *priv_head,
                                             uint32 priv_head_len)
{
    uint32 slot = hvdxg.submithwqueue_history_index %
                  HV_DXG_RESOURCE_HISTORY_MAX;
    uint32 head_len = priv_head_len < 8 ? priv_head_len : 8;

    hvdxg.submithwqueue_history_len[slot] = len;
    hvdxg.submithwqueue_history_ret[slot] = ret;
    hvdxg.submithwqueue_history_queue[slot] = queue;
    hvdxg.submithwqueue_history_command_length[slot] = command_length;
    hvdxg.submithwqueue_history_priv_size[slot] = priv_size;
    hvdxg.submithwqueue_history_priv_head_len[slot] = head_len;
    memset(hvdxg.submithwqueue_history_priv_head[slot], 0,
           sizeof(hvdxg.submithwqueue_history_priv_head[slot]));
    if (head_len != 0 && priv_head != NULL)
        memcpy(hvdxg.submithwqueue_history_priv_head[slot], priv_head,
               head_len);
    hvdxg.submithwqueue_history_index++;
}

static void hvdxg_note_lock2_history(uint32 len, int32 ret, uint32 status,
                                     uint32 device, uint32 allocation,
                                     uint64 offset, uint64 user_va,
                                     uint64 map_size)
{
    uint32 slot = hvdxg.lock2_history_index %
                  HV_DXG_RESOURCE_HISTORY_MAX;

    hvdxg.lock2_history_len[slot] = len;
    hvdxg.lock2_history_ret[slot] = ret;
    hvdxg.lock2_history_status[slot] = status;
    hvdxg.lock2_history_device[slot] = device;
    hvdxg.lock2_history_allocation[slot] = allocation;
    hvdxg.lock2_history_offset[slot] = offset;
    hvdxg.lock2_history_user_va[slot] = user_va;
    hvdxg.lock2_history_map_size[slot] = map_size;
    hvdxg.lock2_history_index++;
}

#define HV_DXG_QH_ARGS(i) \
    hvdxg.queryadapter_history_type[(i)], \
    hvdxg.queryadapter_history_size[(i)], \
    hvdxg.queryadapter_history_len[(i)], \
    hvdxg.queryadapter_history_ret[(i)], \
    hvdxg.queryadapter_history_status[(i)]

#define HV_DXG_QAI_ADMISSION_ARGS(i) \
    hvdxg.queryadapter_admission_kind[(i)], \
    hvdxg.queryadapter_admission_type[(i)], \
    hvdxg.queryadapter_admission_size[(i)], \
    hvdxg.queryadapter_admission_len[(i)], \
    hvdxg.queryadapter_admission_ret[(i)], \
    hvdxg.queryadapter_admission_status[(i)], \
    hvdxg.queryadapter_admission_route[(i)], \
    hvdxg.queryadapter_admission_source[(i)], \
    hvdxg.queryadapter_admission_adapter[(i)], \
    hvdxg.queryadapter_admission_host[(i)], \
    hvdxg.queryadapter_admission_head_hash[(i)]

#define HV_DXG_AH_ARGS(i) \
    hvdxg.allocation_history_device[(i)], \
    hvdxg.allocation_history_resource[(i)], \
    hvdxg.allocation_history_allocation[(i)], \
    hvdxg.allocation_history_size[(i)], \
    hvdxg.allocation_history_len[(i)], \
    hvdxg.allocation_history_ret[(i)], \
    hvdxg.allocation_history_count[(i)], \
    hvdxg.allocation_history_priv[(i)]

#define HV_DXG_MH_ARGS(i) \
    hvdxg.mapgpuva_history_allocation[(i)], \
    hvdxg.mapgpuva_history_va[(i)], \
    hvdxg.mapgpuva_history_pages[(i)], \
    hvdxg.mapgpuva_history_fence[(i)], \
    hvdxg.mapgpuva_history_len[(i)], \
    hvdxg.mapgpuva_history_ret[(i)], \
    hvdxg.mapgpuva_history_status[(i)], \
    hvdxg.mapgpuva_history_paging_queue[(i)]

#define HV_DXG_LH_ARGS(i) \
    hvdxg.lock2_history_device[(i)], \
    hvdxg.lock2_history_allocation[(i)], \
    hvdxg.lock2_history_offset[(i)], \
    hvdxg.lock2_history_user_va[(i)], \
    hvdxg.lock2_history_map_size[(i)], \
    hvdxg.lock2_history_len[(i)], \
    hvdxg.lock2_history_ret[(i)], \
    hvdxg.lock2_history_status[(i)]

#define HV_DXG_SH_ARGS(i) \
    hvdxg.submithwqueue_history_len[(i)], \
    hvdxg.submithwqueue_history_ret[(i)], \
    hvdxg.submithwqueue_history_queue[(i)], \
    hvdxg.submithwqueue_history_command_length[(i)], \
    hvdxg.submithwqueue_history_priv_size[(i)], \
    hvdxg.submithwqueue_history_priv_head_len[(i)], \
    hvdxg.submithwqueue_history_priv_head[(i)][0], \
    hvdxg.submithwqueue_history_priv_head[(i)][1], \
    hvdxg.submithwqueue_history_priv_head[(i)][2], \
    hvdxg.submithwqueue_history_priv_head[(i)][3], \
    hvdxg.submithwqueue_history_priv_head[(i)][4], \
    hvdxg.submithwqueue_history_priv_head[(i)][5], \
    hvdxg.submithwqueue_history_priv_head[(i)][6], \
    hvdxg.submithwqueue_history_priv_head[(i)][7]

static void hvdxg_note_ioctl_history(uint64 cmd, int ret)
{
    uint32 slot = hvdxg.ioctl_history_index %
                  HV_DXG_IOCTL_HISTORY_MAX;

    hvdxg.ioctl_history_cmd[slot] = (uint32)cmd;
    hvdxg.ioctl_history_nr[slot] = (uint32)(cmd & 0xffU);
    hvdxg.ioctl_history_ret[slot] = ret;
    hvdxg.ioctl_history_index++;
}

static uint64 hvdxg_ticks_to_us(uint64 ticks)
{
    uint64 freq = __timebase_frequency;

    if (freq == 0)
        return ticks;
    return (ticks / freq) * 1000000ULL +
           ((ticks % freq) * 1000000ULL) / freq;
}

static void hvdxg_note_ioctl_timing(uint64 cmd, uint64 ticks)
{
    uint32 nr = (uint32)(cmd & 0xffU);

    hvdxg.ioctl_nr_count[nr]++;
    hvdxg.ioctl_nr_total_ticks[nr] += ticks;
    hvdxg.ioctl_nr_last_ticks[nr] = ticks;
    if (ticks > hvdxg.ioctl_nr_max_ticks[nr])
        hvdxg.ioctl_nr_max_ticks[nr] = ticks;
}

static void hvdxg_note_host_command(uint32 command_type)
{
    uint32 order = 0;

    switch (command_type) {
    case HV_DXGK_VMBCOMMAND_DESTROYALLOCATION:
        hvdxg.host_cmd_destroyallocation++;
        order = ++hvdxg.cleanup_order_seq;
        hvdxg.cleanup_last_destroy_order = order;
        break;
    case HV_DXGK_VMBCOMMAND_DESTROYCONTEXT:
        hvdxg.host_cmd_destroycontext++;
        order = ++hvdxg.cleanup_order_seq;
        hvdxg.cleanup_last_destroy_order = order;
        break;
    case HV_DXGK_VMBCOMMAND_DESTROYHWQUEUE:
        hvdxg.host_cmd_destroyhwqueue++;
        order = ++hvdxg.cleanup_order_seq;
        hvdxg.cleanup_last_destroy_order = order;
        break;
    case HV_DXGK_VMBCOMMAND_DESTROYPAGINGQUEUE:
        hvdxg.host_cmd_destroypagingqueue++;
        order = ++hvdxg.cleanup_order_seq;
        hvdxg.cleanup_last_destroy_order = order;
        break;
    case HV_DXGK_VMBCOMMAND_DESTROYDEVICE:
        hvdxg.host_cmd_destroydevice++;
        order = ++hvdxg.cleanup_order_seq;
        hvdxg.cleanup_last_destroy_order = order;
        break;
    case HV_DXGK_VMBCOMMAND_DESTROYSYNCOBJECT:
        hvdxg.host_cmd_destroysync++;
        order = ++hvdxg.cleanup_order_seq;
        hvdxg.cleanup_last_destroy_order = order;
        break;
    case HV_DXGK_VMBCOMMAND_DESTROYPROCESS:
        hvdxg.host_cmd_destroyprocess++;
        order = ++hvdxg.cleanup_order_seq;
        hvdxg.cleanup_destroyprocess_order = order;
        break;
    case HV_DXGK_VMBCOMMAND_FREEGPUVIRTUALADDRESS:
        hvdxg.host_cmd_freegpuva++;
        order = ++hvdxg.cleanup_order_seq;
        hvdxg.cleanup_last_destroy_order = order;
        break;
    case HV_DXGK_VMBCOMMAND_CLOSEADAPTER:
        hvdxg.closeadapter_host_count++;
        order = ++hvdxg.cleanup_order_seq;
        hvdxg.closeadapter_last_order = order;
        if (hvdxg.cleanup_last_destroy_order != 0 &&
            order > hvdxg.cleanup_last_destroy_order)
            hvdxg.closeadapter_after_destroy_count++;
        break;
    case HV_DXGK_VMBCOMMAND_LOCK2:
        hvdxg.host_cmd_lock2++;
        break;
    case HV_DXGK_VMBCOMMAND_UNLOCK2:
        hvdxg.host_cmd_unlock2++;
        break;
    default:
        break;
    }
}

static void hvdxg_ioctl_timing_top(uint32 *top_nr)
{
    uint64 top_ticks[HV_DXG_IOCTL_TIME_TOP];

    memset(top_nr, 0, sizeof(uint32) * HV_DXG_IOCTL_TIME_TOP);
    memset(top_ticks, 0, sizeof(top_ticks));
    for (uint32 nr = 0; nr < HV_DXG_IOCTL_NR_MAX; nr++) {
        uint64 ticks = hvdxg.ioctl_nr_total_ticks[nr];
        if (hvdxg.ioctl_nr_count[nr] == 0)
            continue;
        for (uint32 slot = 0; slot < HV_DXG_IOCTL_TIME_TOP; slot++) {
            if (ticks <= top_ticks[slot])
                continue;
            for (uint32 move = HV_DXG_IOCTL_TIME_TOP - 1; move > slot;
                 move--) {
                top_ticks[move] = top_ticks[move - 1];
                top_nr[move] = top_nr[move - 1];
            }
            top_ticks[slot] = ticks;
            top_nr[slot] = nr;
            break;
        }
    }
}

static void hvdxg_save_priv_head(uint8 *dst, uint32 dst_cap, uint32 *dst_len,
                                 const uint8 *src, uint32 src_len)
{
    uint32 len = src_len < dst_cap ? src_len : dst_cap;

    *dst_len = len;
    memset(dst, 0, dst_cap);
    if (len != 0)
        memcpy(dst, src, len);
}

static uint32 hvdxg_runtime_d3d12_resource_flags(const uint8 *runtime,
                                                 uint32 runtime_size)
{
    uint32 flags = 0;

    /*
     * The observed WSL/NVIDIA D3D12 runtime blob carries the
     * D3D12_RESOURCE_DESC flags at this offset.  The raw blob is still exposed
     * in /dev/dxg; this extracted value lets the NT-export guard distinguish a
     * render target from a copied shared texture.
     */
    if (runtime != NULL && runtime_size >= 0xf0)
        memcpy(&flags, runtime + 0xec, sizeof(flags));
    return flags;
}

static uint16 hvdxg_read_u16_at(const uint8 *data, uint32 size, uint32 off)
{
    uint16 value = 0;

    if (data != NULL && off <= size && size - off >= sizeof(value))
        memcpy(&value, data + off, sizeof(value));
    return value;
}

static uint32 hvdxg_read_u32_at(const uint8 *data, uint32 size, uint32 off)
{
    uint32 value = 0;

    if (data != NULL && off <= size && size - off >= sizeof(value))
        memcpy(&value, data + off, sizeof(value));
    return value;
}

static uint64 hvdxg_read_u64_at(const uint8 *data, uint32 size, uint32 off)
{
    uint64 value = 0;

    if (data != NULL && off <= size && size - off >= sizeof(value))
        memcpy(&value, data + off, sizeof(value));
    return value;
}

static void hvdxg_write_u32_at(uint8 *data, uint32 size, uint32 off,
                               uint32 value)
{
    if (data != NULL && off <= size && size - off >= sizeof(value))
        memcpy(data + off, &value, sizeof(value));
}

static void hvdxg_write_u64_at(uint8 *data, uint32 size, uint32 off,
                               uint64 value)
{
    if (data != NULL && off <= size && size - off >= sizeof(value))
        memcpy(data + off, &value, sizeof(value));
}

static uint32 hvdxg_hash_bytes(const uint8 *data, uint32 size)
{
    uint32 hash = 2166136261U;

    if (data == NULL)
        return 0;
    for (uint32 i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 16777619U;
    }
    return hash;
}

static int hvdxg_queryadapter_alias_cache_type(uint32 type)
{
    return type == _KMTQAITYPE_UMDRIVERNAME ||
           type == _KMTQAITYPE_ADAPTERTYPE ||
           type == _KMTQAITYPE_PHYSICALADAPTERCOUNT ||
           type == HV_DXG_QAITYPE_ADAPTER_HARDWARE_ID ||
           type == _KMTQAITYPE_ADAPTERTYPE_RENDER;
}

static struct hvdxg_queryadapter_alias_cache_entry *
hvdxg_queryadapter_alias_cache_find(uint32 type, uint32 host_adapter)
{
    if (!hvdxg_queryadapter_alias_cache_type(type) || host_adapter == 0)
        return NULL;
    for (uint32 i = 0; i < HV_DXG_QUERYADAPTER_ALIAS_CACHE_TYPES; i++) {
        struct hvdxg_queryadapter_alias_cache_entry *entry =
            &hvdxg.queryadapter_alias_cache[i];

        if (entry->valid && entry->type == type &&
            entry->host_adapter == host_adapter)
            return entry;
    }
    return NULL;
}

static int hvdxg_queryadapter_type0_cache_load(uint32 requested_size,
                                               uint32 alias_adapter,
                                               uint32 host_adapter,
                                               uint8 *out)
{
    hvdxg.queryadapter_alias_cache_last_type =
        HV_DXG_QAITYPE_SELECTED_ADAPTER;
    hvdxg.queryadapter_alias_cache_last_size = requested_size;
    hvdxg.queryadapter_alias_cache_last_len = 0;
    hvdxg.queryadapter_alias_cache_last_alias = alias_adapter;
    hvdxg.queryadapter_alias_cache_last_host = host_adapter;
    hvdxg.queryadapter_alias_cache_last_hash = 0;
    hvdxg.queryadapter_alias_cache_last_result = 0;

    if (!hvdxg.queryadapter_type0_cache_valid ||
        hvdxg.queryadapter_type0_cache_payload == NULL ||
        hvdxg.queryadapter_type0_cache_host != host_adapter ||
        hvdxg.queryadapter_type0_cache_input_hash !=
            hvdxg.queryadapter_type0_private_hash ||
        hvdxg.queryadapter_type0_cache_size < requested_size ||
        out == NULL) {
        hvdxg.queryadapter_alias_cache_misses++;
        hvdxg.queryadapter_alias_cache_last_result = 2;
        return -ENOENT;
    }

    memcpy(out, hvdxg.queryadapter_type0_cache_payload, requested_size);
    hvdxg.queryadapter_alias_cache_hits++;
    hvdxg.queryadapter_alias_cache_last_len = requested_size;
    hvdxg.queryadapter_alias_cache_last_hash =
        hvdxg.queryadapter_type0_cache_hash;
    hvdxg.queryadapter_alias_cache_last_result = 1;
    hvdxg_note_queryadapter_return_head(out, requested_size);
    return 0;
}

static void hvdxg_queryadapter_type0_cache_store(uint32 size,
                                                 uint32 host_adapter,
                                                 const uint8 *payload)
{
    uint8 *copy;

    if (host_adapter == 0 || payload == NULL || size == 0 ||
        size > HV_DXG_QUERYADAPTER_TYPE0_CACHE_MAX)
        return;

    copy = kvmalloc(size);
    if (copy == NULL) {
        hvdxg.queryadapter_alias_cache_full++;
        hvdxg.queryadapter_alias_cache_last_type =
            HV_DXG_QAITYPE_SELECTED_ADAPTER;
        hvdxg.queryadapter_alias_cache_last_size = size;
        hvdxg.queryadapter_alias_cache_last_len = 0;
        hvdxg.queryadapter_alias_cache_last_alias = 0;
        hvdxg.queryadapter_alias_cache_last_host = host_adapter;
        hvdxg.queryadapter_alias_cache_last_hash = 0;
        hvdxg.queryadapter_alias_cache_last_result = 5;
        return;
    }
    memcpy(copy, payload, size);
    if (hvdxg.queryadapter_type0_cache_payload != NULL)
        kvfree(hvdxg.queryadapter_type0_cache_payload);
    hvdxg.queryadapter_type0_cache_payload = copy;
    hvdxg.queryadapter_type0_cache_valid = 1;
    hvdxg.queryadapter_type0_cache_size = size;
    hvdxg.queryadapter_type0_cache_host = host_adapter;
    hvdxg.queryadapter_type0_cache_input_hash =
        hvdxg.queryadapter_type0_private_hash;
    hvdxg.queryadapter_type0_cache_hash = hvdxg_hash_bytes(payload, size);
    hvdxg.queryadapter_alias_cache_stores++;
    hvdxg.queryadapter_alias_cache_last_type =
        HV_DXG_QAITYPE_SELECTED_ADAPTER;
    hvdxg.queryadapter_alias_cache_last_size = size;
    hvdxg.queryadapter_alias_cache_last_len = size;
    hvdxg.queryadapter_alias_cache_last_alias = 0;
    hvdxg.queryadapter_alias_cache_last_host = host_adapter;
    hvdxg.queryadapter_alias_cache_last_hash =
        hvdxg.queryadapter_type0_cache_hash;
    hvdxg.queryadapter_alias_cache_last_result = 3;
}

static int hvdxg_queryadapter_alias_cache_load(uint32 type,
                                               uint32 requested_size,
                                               uint32 alias_adapter,
                                               uint32 host_adapter,
                                               uint8 *out)
{
    struct hvdxg_queryadapter_alias_cache_entry *entry;

    if (type == HV_DXG_QAITYPE_SELECTED_ADAPTER)
        return hvdxg_queryadapter_type0_cache_load(requested_size,
            alias_adapter, host_adapter, out);

    hvdxg.queryadapter_alias_cache_last_type = type;
    hvdxg.queryadapter_alias_cache_last_size = requested_size;
    hvdxg.queryadapter_alias_cache_last_len = 0;
    hvdxg.queryadapter_alias_cache_last_alias = alias_adapter;
    hvdxg.queryadapter_alias_cache_last_host = host_adapter;
    hvdxg.queryadapter_alias_cache_last_hash = 0;
    hvdxg.queryadapter_alias_cache_last_result = 0;

    entry = hvdxg_queryadapter_alias_cache_find(type, host_adapter);
    if (entry == NULL || entry->size < requested_size ||
        out == NULL || requested_size > HV_DXG_QUERYADAPTER_ALIAS_CACHE_MAX) {
        hvdxg.queryadapter_alias_cache_misses++;
        hvdxg.queryadapter_alias_cache_last_result = 2;
        return -ENOENT;
    }

    memcpy(out, entry->payload, requested_size);
    hvdxg.queryadapter_alias_cache_hits++;
    hvdxg.queryadapter_alias_cache_last_len = requested_size;
    hvdxg.queryadapter_alias_cache_last_hash =
        hvdxg_hash_bytes(entry->payload, entry->size);
    hvdxg.queryadapter_alias_cache_last_result = 1;
    hvdxg_note_queryadapter_return_head(entry->payload, requested_size);
    return 0;
}

static void hvdxg_queryadapter_alias_cache_store(uint32 type,
                                                 uint32 size,
                                                 uint32 host_adapter,
                                                 const uint8 *payload)
{
    struct hvdxg_queryadapter_alias_cache_entry *entry = NULL;

    if (type == HV_DXG_QAITYPE_SELECTED_ADAPTER) {
        hvdxg_queryadapter_type0_cache_store(size, host_adapter, payload);
        return;
    }

    if (!hvdxg_queryadapter_alias_cache_type(type) ||
        host_adapter == 0 || payload == NULL ||
        size == 0 || size > HV_DXG_QUERYADAPTER_ALIAS_CACHE_MAX)
        return;

    entry = hvdxg_queryadapter_alias_cache_find(type, host_adapter);
    if (entry == NULL) {
        for (uint32 i = 0; i < HV_DXG_QUERYADAPTER_ALIAS_CACHE_TYPES; i++) {
            if (!hvdxg.queryadapter_alias_cache[i].valid) {
                entry = &hvdxg.queryadapter_alias_cache[i];
                break;
            }
        }
    }
    if (entry == NULL) {
        hvdxg.queryadapter_alias_cache_full++;
        return;
    }

    memset(entry, 0, sizeof(*entry));
    entry->valid = 1;
    entry->type = type;
    entry->size = size;
    entry->host_adapter = host_adapter;
    memcpy(entry->payload, payload, size);
    entry->raw_hash = hvdxg_hash_bytes(payload, size);
    hvdxg.queryadapter_alias_cache_stores++;
    hvdxg.queryadapter_alias_cache_last_type = type;
    hvdxg.queryadapter_alias_cache_last_size = size;
    hvdxg.queryadapter_alias_cache_last_len = size;
    hvdxg.queryadapter_alias_cache_last_alias = 0;
    hvdxg.queryadapter_alias_cache_last_host = host_adapter;
    hvdxg.queryadapter_alias_cache_last_hash = entry->raw_hash;
    hvdxg.queryadapter_alias_cache_last_result = 3;
}

static int hvdxg_queryadapter_stage_umd_payload(uint32 type, uint32 size,
                                                uint32 alias_adapter,
                                                uint32 host_adapter,
                                                uint8 *out)
{
    uint32 need;

    hvdxg.queryadapter_alias_staged_type = type;
    hvdxg.queryadapter_alias_staged_size = size;
    hvdxg.queryadapter_alias_staged_alias = alias_adapter;
    hvdxg.queryadapter_alias_staged_host = host_adapter;
    hvdxg.queryadapter_alias_staged_path = 0;
    hvdxg.queryadapter_alias_staged_ret = 0;

    if (type != _KMTQAITYPE_UMDRIVERNAME || out == NULL ||
        hvdxg.adapter_vendor_id != HV_DXG_VENDOR_NVIDIA) {
        hvdxg.queryadapter_alias_staged_ret = -ENOENT;
        return -ENOENT;
    }
    need = ((uint32)strlen(HV_DXG_NVIDIA_UMD_DRIVERSTORE_PATH) + 1U) *
           sizeof(uint16);
    if (size < need) {
        hvdxg.queryadapter_alias_staged_ret = -EOVERFLOW;
        return -EOVERFLOW;
    }

    memset(out, 0, size);
    hvdxg_write_utf16_string(out, size, HV_DXG_NVIDIA_UMD_DRIVERSTORE_PATH);
    hvdxg.queryadapter_alias_staged_path = 1;
    hvdxg_note_queryadapter_return_head(out, size);
    return 0;
}

static int hvdxg_rewrite_umd_driver_path(uint8 *data, uint32 size,
                                         uint32 host_status)
{
    const char *path = NULL;
    uint32 path_id = 0;
    uint32 need;

    hvdxg.queryadapter_umd_rewrite_attempted = 1;
    hvdxg.queryadapter_umd_rewrite_rewritten = 0;
    hvdxg.queryadapter_umd_rewrite_path = 0;
    hvdxg.queryadapter_umd_rewrite_vendor = hvdxg.adapter_vendor_id;
    hvdxg.queryadapter_umd_rewrite_original_hash =
        hvdxg_hash_bytes(data, size);
    hvdxg.queryadapter_umd_rewrite_original0 =
        size >= sizeof(uint32) ? hvdxg_read_u32(data) : 0;
    hvdxg.queryadapter_umd_rewrite_original1 =
        size >= 2 * sizeof(uint32) ?
        hvdxg_read_u32(data + sizeof(uint32)) : 0;
    hvdxg.queryadapter_umd_rewrite_host_status = host_status;
    hvdxg.queryadapter_umd_rewrite_ret = 0;

    if (hvdxg.adapter_vendor_id == HV_DXG_VENDOR_NVIDIA) {
        path = HV_DXG_NVIDIA_UMD_DRIVERSTORE_PATH;
        path_id = 1;
    } else if (hvdxg.adapter_vendor_id == HV_DXG_VENDOR_INTEL) {
        path = "/lib/libigd12umd64.so";
        path_id = 2;
    } else if (data != NULL) {
        uint32 chars = size / sizeof(uint16);
        uint16 *words = (uint16 *)data;

        for (uint32 i = 0; i + 1 < chars && path == NULL; i++) {
            uint16 c0 = words[i] >= 'A' && words[i] <= 'Z' ?
                        words[i] + ('a' - 'A') : words[i];
            uint16 c1 = words[i + 1] >= 'A' && words[i + 1] <= 'Z' ?
                        words[i + 1] + ('a' - 'A') : words[i + 1];

            if (c0 == 'n' && c1 == 'v') {
                path = HV_DXG_NVIDIA_UMD_DRIVERSTORE_PATH;
                path_id = 1;
                hvdxg.queryadapter_umd_rewrite_vendor =
                    HV_DXG_VENDOR_NVIDIA;
            } else if (i + 2 < chars && c0 == 'i' && c1 == 'g') {
                uint16 c2 = words[i + 2] >= 'A' &&
                            words[i + 2] <= 'Z' ?
                            words[i + 2] + ('a' - 'A') : words[i + 2];
                if (c2 == 'd') {
                    path = "/lib/libigd12umd64.so";
                    path_id = 2;
                    hvdxg.queryadapter_umd_rewrite_vendor =
                        HV_DXG_VENDOR_INTEL;
                }
            }
        }
    }
    if (path == NULL) {
        hvdxg.queryadapter_umd_rewrite_ret = -ENOTSUP;
        return 0;
    }

    need = ((uint32)strlen(path) + 1U) * sizeof(uint16);
    hvdxg.queryadapter_umd_rewrite_path = path_id;
    if (size < need) {
        hvdxg.queryadapter_umd_rewrite_ret = -EOVERFLOW;
        return -EOVERFLOW;
    }
    memset(data, 0, size);
    hvdxg_write_utf16_string(data, size, path);
    hvdxg.queryadapter_umd_rewrite_rewritten = 1;
    return 0;
}

static void hvdxg_note_queryadapter_type0_input(const uint8 *data,
                                                uint32 size)
{
    uint32 head_len = size < HV_DXG_QUERYADAPTER_TYPE0_SNAPSHOT_BYTES ?
                      size : HV_DXG_QUERYADAPTER_TYPE0_SNAPSHOT_BYTES;
    uint32 tail_len = head_len;

    hvdxg.queryadapter_type0_private_size = size;
    hvdxg.queryadapter_type0_private_hash = hvdxg_hash_bytes(data, size);
    hvdxg.queryadapter_type0_private_head_len = head_len;
    hvdxg.queryadapter_type0_private_tail_len = tail_len;
    memset(hvdxg.queryadapter_type0_private_head, 0,
           sizeof(hvdxg.queryadapter_type0_private_head));
    memset(hvdxg.queryadapter_type0_private_tail, 0,
           sizeof(hvdxg.queryadapter_type0_private_tail));
    if (data != NULL && head_len != 0)
        memcpy(hvdxg.queryadapter_type0_private_head, data, head_len);
    if (data != NULL && tail_len != 0)
        memcpy(hvdxg.queryadapter_type0_private_tail,
               data + size - tail_len, tail_len);

    hvdxg.queryadapter_type0_primary_len = 0;
    hvdxg.queryadapter_type0_primary_ret = 0;
    hvdxg.queryadapter_type0_primary_status = 0;
    hvdxg.queryadapter_type0_fallback_attempted = 0;
    hvdxg.queryadapter_type0_fallback_used = 0;
    hvdxg.queryadapter_type0_fallback_len = 0;
    hvdxg.queryadapter_type0_fallback_ret = 0;
    hvdxg.queryadapter_type0_fallback_status = 0;
    hvdxg.queryadapter_type0_result_route = 0;
    hvdxg.queryadapter_type0_fallback_reason = 0;
    hvdxg.queryadapter_type0_fallback_route = 0;
    hvdxg.queryadapter_umd_rewrite_attempted = 0;
    hvdxg.queryadapter_umd_rewrite_rewritten = 0;
    hvdxg.queryadapter_umd_rewrite_path = 0;
    hvdxg.queryadapter_umd_rewrite_vendor = 0;
    hvdxg.queryadapter_umd_rewrite_original_hash = 0;
    hvdxg.queryadapter_umd_rewrite_original0 = 0;
    hvdxg.queryadapter_umd_rewrite_original1 = 0;
    hvdxg.queryadapter_umd_rewrite_host_status = 0;
    hvdxg.queryadapter_umd_rewrite_ret = 0;
}

static void hvdxg_note_queryadapter_type15_failure(uint32 type, int ret,
                                                   int32 status,
                                                   uint32 process)
{
    if (type != 15U || ret == 0)
        return;
    hvdxg.queryadapter_type15_fail_ret = ret;
    hvdxg.queryadapter_type15_fail_status = status;
    hvdxg.queryadapter_type15_fail_process = process;
    hvdxg.queryadapter_type15_fail_route = hvdxg.queryadapter_send_route;
    hvdxg.queryadapter_type15_fail_ext_luid_low =
        hvdxg.queryadapter_send_ext_luid_low;
    hvdxg.queryadapter_type15_fail_ext_luid_high =
        hvdxg.queryadapter_send_ext_luid_high;
}

static void hvdxg_note_shared_resource_metadata(
    const struct hvdxg_tracked_resource *resource)
{
    if (resource == NULL) {
        hvdxg.sharedresource_meta_track_host = 0;
        hvdxg.sharedresource_meta_runtime_len = 0;
        hvdxg.sharedresource_meta_resource_len = 0;
        hvdxg.sharedresource_meta_total_len = 0;
        hvdxg.sharedresource_meta_alloc0_priv = 0;
        hvdxg.sharedresource_meta_runtime_hash = 0;
        hvdxg.sharedresource_meta_resource_hash = 0;
        hvdxg.sharedresource_meta_total_hash = 0;
        hvdxg.sharedresource_meta_match_in = 0;
        hvdxg.sharedresource_meta_match_out = 0;
        hvdxg.sharedresource_meta_total_w4 = 0;
        hvdxg.sharedresource_meta_total_w8 = 0;
        hvdxg.sharedresource_meta_logical_flags = 0;
        hvdxg.sharedresource_meta_host_result_flags = 0;
        hvdxg.sharedresource_meta_host_flags_ignored = 0;
        return;
    }
    hvdxg.sharedresource_meta_track_host = resource->total_priv_from_host;
    hvdxg.sharedresource_meta_runtime_len =
        resource->private_runtime_data_size;
    hvdxg.sharedresource_meta_resource_len =
        resource->resource_priv_drv_data_size;
    hvdxg.sharedresource_meta_total_len =
        resource->total_priv_drv_data_size;
    hvdxg.sharedresource_meta_alloc0_priv =
        resource->allocation_count != 0 ? resource->alloc_priv_sizes[0] : 0;
    hvdxg.sharedresource_meta_runtime_hash =
        hvdxg_hash_bytes(resource->private_runtime_data,
                         resource->private_runtime_data_size);
    hvdxg.sharedresource_meta_resource_hash =
        hvdxg_hash_bytes(resource->resource_priv_drv_data,
                         resource->resource_priv_drv_data_size);
    hvdxg.sharedresource_meta_total_hash =
        hvdxg_hash_bytes(resource->total_priv_drv_data,
                         resource->total_priv_drv_data_size);
    hvdxg.sharedresource_meta_total_w4 =
        hvdxg_read_u32_at(resource->total_priv_drv_data,
                          resource->total_priv_drv_data_size, 0x10);
    hvdxg.sharedresource_meta_total_w8 =
        hvdxg_read_u32_at(resource->total_priv_drv_data,
                          resource->total_priv_drv_data_size, 0x20);
    hvdxg.sharedresource_meta_logical_flags = resource->create_flags_value;
    hvdxg.sharedresource_meta_host_result_flags =
        resource->host_create_flags_value;
    hvdxg.sharedresource_meta_host_flags_ignored =
        resource->host_create_flags_value != 0 &&
        resource->host_create_flags_value != resource->create_flags_value ?
        1 : 0;
    hvdxg.sharedresource_meta_match_in =
        resource->total_priv_drv_data_size ==
            hvdxg.d3d12_shared_alloc_priv_head_len &&
        resource->total_priv_drv_data_size != 0 &&
        memcmp(resource->total_priv_drv_data,
               hvdxg.d3d12_shared_alloc_priv_head,
               resource->total_priv_drv_data_size) == 0 ? 1 : 0;
    hvdxg.sharedresource_meta_match_out =
        resource->total_priv_drv_data_size ==
            hvdxg.d3d12_shared_alloc_out_priv_head_len &&
        resource->total_priv_drv_data_size != 0 &&
        memcmp(resource->total_priv_drv_data,
               hvdxg.d3d12_shared_alloc_out_priv_head,
               resource->total_priv_drv_data_size) == 0 ? 1 : 0;
}

static uint32 hvdxg_shared_resource_alloc0_hash(
    const struct hvdxg_tracked_resource *resource)
{
    if (resource == NULL || resource->allocation_count == 0 ||
        resource->alloc_priv_sizes[0] == 0 ||
        resource->total_priv_drv_data == NULL)
        return 0;
    if (resource->alloc_priv_sizes[0] > resource->total_priv_drv_data_size)
        return 0;
    return hvdxg_hash_bytes(resource->total_priv_drv_data,
                            resource->alloc_priv_sizes[0]);
}

static uint32 hvdxg_ntshared_cache_refs(uint32 kind, uint32 process,
                                        uint32 object,
                                        uint32 host_nt_handle);

static void hvdxg_reset_shared_resource_record(void)
{
    hvdxg.sharedresource_record_valid = 0;
    hvdxg.sharedresource_record_stage = 0;
    hvdxg.sharedresource_record_key_kind = 0;
    hvdxg.sharedresource_record_key_process = 0;
    hvdxg.sharedresource_record_key_object = 0;
    hvdxg.sharedresource_record_key_global = 0;
    hvdxg.sharedresource_record_key_nt = 0;
    hvdxg.sharedresource_record_source_process = 0;
    hvdxg.sharedresource_record_source_tgid = 0;
    hvdxg.sharedresource_record_source_generation = 0;
    hvdxg.sharedresource_record_device = 0;
    hvdxg.sharedresource_record_resource = 0;
    hvdxg.sharedresource_record_allocation = 0;
    hvdxg.sharedresource_record_adapter_low = 0;
    hvdxg.sharedresource_record_adapter_high = 0;
    hvdxg.sharedresource_record_host_adapter_low = 0;
    hvdxg.sharedresource_record_host_adapter_high = 0;
    hvdxg.sharedresource_record_sealed_generation = 0;
    hvdxg.sharedresource_record_sealed = 0;
    hvdxg.sharedresource_record_seal_before_fd = 0;
    hvdxg.sharedresource_record_alloc_count = 0;
    hvdxg.sharedresource_record_runtime_size = 0;
    hvdxg.sharedresource_record_resource_size = 0;
    hvdxg.sharedresource_record_total_size = 0;
    hvdxg.sharedresource_record_alloc0_priv = 0;
    hvdxg.sharedresource_record_runtime_hash = 0;
    hvdxg.sharedresource_record_resource_hash = 0;
    hvdxg.sharedresource_record_total_hash = 0;
    hvdxg.sharedresource_record_alloc0_hash = 0;
    hvdxg.sharedresource_record_nt_refs = 0;
    hvdxg.sharedresource_record_query_count = 0;
    hvdxg.sharedresource_record_open_count = 0;
    hvdxg.sharedresource_record_fd_publish_count = 0;
    hvdxg.sharedresource_record_local_admit_ret = 0;
    hvdxg.sharedresource_record_local_exact = 0;
    hvdxg.sharedresource_record_mutation_after_seal = 0;
    hvdxg.sharedresource_model_valid = 0;
    hvdxg.sharedresource_model_flat_match = 0;
    hvdxg.sharedresource_model_alloc_count = 0;
    hvdxg.sharedresource_model_alloc0 = 0;
    hvdxg.sharedresource_model_alloc0_priv = 0;
    hvdxg.sharedresource_model_alloc0_size = 0;
    hvdxg.sharedresource_model_alloc0_pages = 0;
    hvdxg.sharedresource_model_alloc0_flags = 0;
    hvdxg.sharedresource_model_alloc0_cached = 0;
    hvdxg.sharedresource_model_runtime_size = 0;
    hvdxg.sharedresource_model_resource_size = 0;
    hvdxg.sharedresource_model_total_size = 0;
    hvdxg.sharedresource_model_sealed = 0;
    hvdxg.sharedresource_model_generation = 0;
}

static void hvdxg_note_shared_resource_record(
    const char *stage_name, uint32 stage,
    const struct hvdxg_tracked_resource *resource,
    uint32 kind, uint32 process, uint32 cache_object, uint32 global_share,
    uint32 host_nt_handle, int32 local_admit_ret, uint32 local_exact)
{
    uint32 runtime_hash = 0;
    uint32 resource_hash = 0;
    uint32 total_hash = 0;
    uint32 alloc0_hash = 0;
    uint64 source_tgid = current != NULL ? (uint64)thread_tgid(current) : 0;
    uint32 mutation = 0;

    (void)stage_name;
    if (resource == NULL)
        return;

    runtime_hash = hvdxg_hash_bytes(resource->private_runtime_data,
                                    resource->private_runtime_data_size);
    resource_hash = hvdxg_hash_bytes(resource->resource_priv_drv_data,
                                     resource->resource_priv_drv_data_size);
    total_hash = hvdxg_hash_bytes(resource->total_priv_drv_data,
                                  resource->total_priv_drv_data_size);
    alloc0_hash = hvdxg_shared_resource_alloc0_hash(resource);
    if (hvdxg.sharedresource_record_valid &&
        hvdxg.sharedresource_record_sealed &&
        hvdxg.sharedresource_record_key_process == process &&
        hvdxg.sharedresource_record_key_object == cache_object &&
        (hvdxg.sharedresource_record_runtime_hash != runtime_hash ||
         hvdxg.sharedresource_record_resource_hash != resource_hash ||
         hvdxg.sharedresource_record_total_hash != total_hash ||
         hvdxg.sharedresource_record_alloc0_hash != alloc0_hash))
        mutation = 1;
    if (hvdxg.sharedresource_record_valid &&
        hvdxg.sharedresource_record_key_process == process &&
        hvdxg.sharedresource_record_key_object == cache_object &&
        hvdxg.sharedresource_record_source_tgid != 0)
        source_tgid = hvdxg.sharedresource_record_source_tgid;

    hvdxg.sharedresource_record_valid = 1;
    hvdxg.sharedresource_record_stage = stage;
    hvdxg.sharedresource_record_key_kind = kind;
    hvdxg.sharedresource_record_key_process = process;
    hvdxg.sharedresource_record_key_object = cache_object;
    hvdxg.sharedresource_record_key_global = global_share;
    hvdxg.sharedresource_record_key_nt = host_nt_handle;
    hvdxg.sharedresource_record_source_process = resource->owner_process;
    hvdxg.sharedresource_record_source_tgid = source_tgid;
    hvdxg.sharedresource_record_source_generation =
        resource->owner_generation;
    hvdxg.sharedresource_record_device = resource->device;
    hvdxg.sharedresource_record_resource = resource->resource;
    hvdxg.sharedresource_record_allocation =
        resource->allocation_count != 0 ? resource->allocation_handles[0] : 0;
    hvdxg.sharedresource_record_adapter_low = hvdxg.adapter_luid.a;
    hvdxg.sharedresource_record_adapter_high = hvdxg.adapter_luid.b;
    hvdxg.sharedresource_record_host_adapter_low = hvdxg.host_adapter_luid.a;
    hvdxg.sharedresource_record_host_adapter_high = hvdxg.host_adapter_luid.b;
    hvdxg.sharedresource_record_sealed_generation =
        resource->sealed_generation;
    hvdxg.sharedresource_record_sealed = resource->sealed;
    hvdxg.sharedresource_record_alloc_count = resource->allocation_count;
    hvdxg.sharedresource_record_runtime_size =
        resource->private_runtime_data_size;
    hvdxg.sharedresource_record_resource_size =
        resource->resource_priv_drv_data_size;
    hvdxg.sharedresource_record_total_size =
        resource->total_priv_drv_data_size;
    hvdxg.sharedresource_record_alloc0_priv =
        resource->allocation_count != 0 ? resource->alloc_priv_sizes[0] : 0;
    hvdxg.sharedresource_record_runtime_hash = runtime_hash;
    hvdxg.sharedresource_record_resource_hash = resource_hash;
    hvdxg.sharedresource_record_total_hash = total_hash;
    hvdxg.sharedresource_record_alloc0_hash = alloc0_hash;
    hvdxg.sharedresource_record_nt_refs =
        hvdxg_ntshared_cache_refs(kind, process, cache_object,
                                  host_nt_handle);
    if (hvdxg.sharedresource_record_nt_refs == 0)
        hvdxg.sharedresource_record_nt_refs = resource->host_shared_refs;
    hvdxg.sharedresource_record_local_admit_ret = local_admit_ret;
    hvdxg.sharedresource_record_local_exact = local_exact;
    hvdxg.sharedresource_model_valid = resource->shared_records_valid;
    hvdxg.sharedresource_model_flat_match =
        resource->shared_records_valid &&
        resource->shared_resource_record.private_runtime_data_size ==
            resource->private_runtime_data_size &&
        resource->shared_resource_record.resource_priv_drv_data_size ==
            resource->resource_priv_drv_data_size &&
        resource->shared_resource_record.total_priv_drv_data_size ==
            resource->total_priv_drv_data_size &&
        resource->shared_resource_record.sealed == resource->sealed &&
        resource->shared_resource_record.sealed_generation ==
            resource->sealed_generation &&
        (resource->allocation_count == 0 ||
         (resource->shared_allocation_records[0].allocation ==
              resource->allocation_handles[0] &&
          resource->shared_allocation_records[0].priv_drv_data_size ==
              resource->alloc_priv_sizes[0] &&
          resource->shared_allocation_records[0].size ==
              resource->allocation_sizes[0] &&
          resource->shared_allocation_records[0].num_pages ==
              resource->allocation_num_pages[0] &&
          resource->shared_allocation_records[0].flags ==
              resource->allocation_flags[0] &&
          resource->shared_allocation_records[0].cached ==
              resource->allocation_cached[0]));
    hvdxg.sharedresource_model_alloc_count = resource->allocation_count;
    hvdxg.sharedresource_model_alloc0 =
        resource->allocation_count != 0 ?
        resource->shared_allocation_records[0].allocation : 0;
    hvdxg.sharedresource_model_alloc0_priv =
        resource->allocation_count != 0 ?
        resource->shared_allocation_records[0].priv_drv_data_size : 0;
    hvdxg.sharedresource_model_alloc0_size =
        resource->allocation_count != 0 ?
        resource->shared_allocation_records[0].size : 0;
    hvdxg.sharedresource_model_alloc0_pages =
        resource->allocation_count != 0 ?
        resource->shared_allocation_records[0].num_pages : 0;
    hvdxg.sharedresource_model_alloc0_flags =
        resource->allocation_count != 0 ?
        resource->shared_allocation_records[0].flags : 0;
    hvdxg.sharedresource_model_alloc0_cached =
        resource->allocation_count != 0 ?
        resource->shared_allocation_records[0].cached : 0;
    hvdxg.sharedresource_model_runtime_size =
        resource->shared_resource_record.private_runtime_data_size;
    hvdxg.sharedresource_model_resource_size =
        resource->shared_resource_record.resource_priv_drv_data_size;
    hvdxg.sharedresource_model_total_size =
        resource->shared_resource_record.total_priv_drv_data_size;
    hvdxg.sharedresource_model_sealed =
        resource->shared_resource_record.sealed;
    hvdxg.sharedresource_model_generation =
        resource->shared_resource_record.sealed_generation;
    if (mutation)
        hvdxg.sharedresource_record_mutation_after_seal = 1;
}

static int hv_cmdline_enabled(const char *key);

static void hvdxg_normalize_d3d12_shared_alloc_priv(
    const struct d3dkmt_createallocation *req,
    const struct d3dddi_allocationinfo2 *alloc_info, uint8 *private_base)
{
    uint8 *runtime;
    uint8 *alloc_priv;
    uint32 alloc_size;
    uint32 w4;
    uint32 w8;
    uint32 width;
    uint32 height;
    uint64 rt8;
    uint64 rt10;

    hvdxg.d3d12_shared_norm_seen = 0;
    hvdxg.d3d12_shared_norm_applied = 0;
    hvdxg.d3d12_shared_norm_reason = 0;
    hvdxg.d3d12_shared_norm_magic0 = 0;
    hvdxg.d3d12_shared_norm_magic3 = 0;
    hvdxg.d3d12_shared_norm_pre_w4 = 0;
    hvdxg.d3d12_shared_norm_post_w4 = 0;
    hvdxg.d3d12_shared_norm_pre_w8 = 0;
    hvdxg.d3d12_shared_norm_post_w8 = 0;
    hvdxg.d3d12_shared_norm_runtime_applied = 0;
    hvdxg.d3d12_shared_norm_width = 0;
    hvdxg.d3d12_shared_norm_height = 0;
    hvdxg.d3d12_shared_norm_pre_rt8 = 0;
    hvdxg.d3d12_shared_norm_post_rt8 = 0;
    hvdxg.d3d12_shared_norm_pre_rt10 = 0;
    hvdxg.d3d12_shared_norm_post_rt10 = 0;
    hvdxg.d3d12_shared_norm_pre_rt38 = 0;
    hvdxg.d3d12_shared_norm_post_rt38 = 0;
    hvdxg.d3d12_shared_norm_pre_rt50 = 0;
    hvdxg.d3d12_shared_norm_post_rt50 = 0;
    hvdxg.d3d12_shared_norm_pre_rt58 = 0;
    hvdxg.d3d12_shared_norm_post_rt58 = 0;

    if (req == NULL || alloc_info == NULL || private_base == NULL)
        return;
    if (req->flags.value != 0x47)
        return;
    hvdxg.d3d12_shared_norm_seen = 1;
    if (req->alloc_count != 1 || req->private_runtime_data_size != 264 ||
        req->priv_drv_data_size != 0 ||
        alloc_info[0].priv_drv_data_size != 594) {
        hvdxg.d3d12_shared_norm_reason = 1;
        return;
    }

    alloc_size = alloc_info[0].priv_drv_data_size;
    runtime = private_base;
    alloc_priv = private_base + req->private_runtime_data_size +
                 req->priv_drv_data_size;
    hvdxg.d3d12_shared_norm_magic0 =
        hvdxg_read_u32_at(alloc_priv, alloc_size, 0);
    hvdxg.d3d12_shared_norm_magic3 =
        hvdxg_read_u32_at(alloc_priv, alloc_size, 12);
    if (hvdxg.d3d12_shared_norm_magic0 != 0x4e564441U ||
        hvdxg.d3d12_shared_norm_magic3 != 0x4e564458U) {
        hvdxg.d3d12_shared_norm_reason = 2;
        return;
    }

    w4 = hvdxg_read_u32_at(alloc_priv, alloc_size, 0x10);
    w8 = hvdxg_read_u32_at(alloc_priv, alloc_size, 0x20);
    width = hvdxg_read_u32_at(alloc_priv, alloc_size, 0x30);
    height = hvdxg_read_u32_at(alloc_priv, alloc_size, 0x34);
    hvdxg.d3d12_shared_norm_pre_w4 = w4;
    hvdxg.d3d12_shared_norm_pre_w8 = w8;
    hvdxg.d3d12_shared_norm_width = width;
    hvdxg.d3d12_shared_norm_height = height;
    hvdxg.d3d12_shared_norm_pre_rt8 =
        hvdxg_read_u64_at(runtime, req->private_runtime_data_size, 0x8);
    hvdxg.d3d12_shared_norm_pre_rt10 =
        hvdxg_read_u64_at(runtime, req->private_runtime_data_size, 0x10);
    hvdxg.d3d12_shared_norm_pre_rt38 =
        hvdxg_read_u64_at(runtime, req->private_runtime_data_size, 0x38);
    hvdxg.d3d12_shared_norm_pre_rt50 =
        hvdxg_read_u64_at(runtime, req->private_runtime_data_size, 0x50);
    hvdxg.d3d12_shared_norm_pre_rt58 =
        hvdxg_read_u64_at(runtime, req->private_runtime_data_size, 0x58);
    if (!hv_cmdline_enabled("dxg_d3d12_private_normalize")) {
        hvdxg.d3d12_shared_norm_post_w4 = w4;
        hvdxg.d3d12_shared_norm_post_w8 = w8;
        hvdxg.d3d12_shared_norm_post_rt8 =
            hvdxg.d3d12_shared_norm_pre_rt8;
        hvdxg.d3d12_shared_norm_post_rt10 =
            hvdxg.d3d12_shared_norm_pre_rt10;
        hvdxg.d3d12_shared_norm_post_rt38 =
            hvdxg.d3d12_shared_norm_pre_rt38;
        hvdxg.d3d12_shared_norm_post_rt50 =
            hvdxg.d3d12_shared_norm_pre_rt50;
        hvdxg.d3d12_shared_norm_post_rt58 =
            hvdxg.d3d12_shared_norm_pre_rt58;
        hvdxg.d3d12_shared_norm_reason = 4;
        return;
    }
    /*
     * Preserve the NVIDIA runtime/private blobs and overlay only the scalar
     * fields under an explicit diagnostic boot flag.  The WSL parity path
     * forwards private bytes verbatim.
     */
    w4 |= 0x8U;
    w8 |= 0x1U;
    rt8 = ((uint64)height << 32) | width;
    rt10 = ((uint64)1U << 32) | 0x57U;
    hvdxg_write_u32_at(alloc_priv, alloc_size, 0x10, w4);
    hvdxg_write_u32_at(alloc_priv, alloc_size, 0x20, w8);
    hvdxg_write_u64_at(runtime, req->private_runtime_data_size, 0x8, rt8);
    hvdxg_write_u64_at(runtime, req->private_runtime_data_size, 0x10, rt10);
    hvdxg.d3d12_shared_norm_post_w4 = w4;
    hvdxg.d3d12_shared_norm_post_w8 = w8;
    hvdxg.d3d12_shared_norm_post_rt8 = rt8;
    hvdxg.d3d12_shared_norm_post_rt10 = rt10;
    hvdxg.d3d12_shared_norm_post_rt38 =
        hvdxg_read_u64_at(runtime, req->private_runtime_data_size, 0x38);
    hvdxg.d3d12_shared_norm_post_rt50 =
        hvdxg_read_u64_at(runtime, req->private_runtime_data_size, 0x50);
    hvdxg.d3d12_shared_norm_post_rt58 =
        hvdxg_read_u64_at(runtime, req->private_runtime_data_size, 0x58);
    hvdxg.d3d12_shared_norm_runtime_applied = 1;
    hvdxg.d3d12_shared_norm_applied = 1;
    hvdxg.d3d12_shared_norm_reason = 3;
}

static void hvdxg_capture_d3d12_runtime_user(
    const struct d3dkmt_createallocation *req)
{
    uint32 copy_len;

    hvdxg.d3d12_shared_runtime_user_copy_ret = 0;
    hvdxg.d3d12_shared_runtime_user_mismatch = 0;
    hvdxg.d3d12_shared_runtime_user_head_len = 0;
    memset(hvdxg.d3d12_shared_runtime_user_head, 0,
           sizeof(hvdxg.d3d12_shared_runtime_user_head));

    if (req == NULL || req->flags.value != 0x47)
        return;
    copy_len = req->private_runtime_data_size;
    if (copy_len > sizeof(hvdxg.d3d12_shared_runtime_user_head))
        copy_len = sizeof(hvdxg.d3d12_shared_runtime_user_head);
    if (copy_len == 0)
        return;
    if (req->private_runtime_data == 0) {
        hvdxg.d3d12_shared_runtime_user_copy_ret = -EINVAL;
        return;
    }
    if (either_copyin(hvdxg.d3d12_shared_runtime_user_head, 1,
                      req->private_runtime_data, copy_len) < 0) {
        hvdxg.d3d12_shared_runtime_user_copy_ret = -EFAULT;
        return;
    }
    hvdxg.d3d12_shared_runtime_user_head_len = copy_len;
}

static void hvdxg_note_d3d12_shared_createallocation_request(
    const struct d3dkmt_createallocation *req,
    const struct d3dddi_allocationinfo2 *alloc_info,
    const uint8 *private_base, uint64 dxg_process)
{
    const uint8 *runtime;
    const uint8 *resource_priv;
    const uint8 *alloc_priv;

    if (req == NULL || alloc_info == NULL || private_base == NULL ||
        req->flags.value != 0x47)
        return;

    runtime = private_base;
    resource_priv = runtime + req->private_runtime_data_size;
    alloc_priv = resource_priv + req->priv_drv_data_size;

    hvdxg.d3d12_shared_create_seq = ++hvdxg.d3d12_shared_event_seq;
    hvdxg.d3d12_shared_first_nt_seq = 0;
    hvdxg.destroyalloc_d3d12_match_count = 0;
    hvdxg.destroyalloc_d3d12_pending_match_count = 0;
    hvdxg.destroyalloc_d3d12_last_match = 0;
    hvdxg.destroyalloc_d3d12_last_seq = 0;
    hvdxg.destroyalloc_d3d12_first_seq = 0;
    hvdxg.destroyalloc_d3d12_first_context = 0;
    hvdxg.destroyalloc_d3d12_first_match = 0;
    hvdxg.destroyalloc_d3d12_first_pending = 0;
    hvdxg.destroyalloc_d3d12_first_before_nt = 0;
    hvdxg.destroyalloc_d3d12_last_before_nt = 0;
    hvdxg.destroyalloc_d3d12_context_mask = 0;
    hvdxg.d3d12_shared_alloc_seen++;
    hvdxg.d3d12_shared_alloc_len = 0;
    hvdxg.d3d12_shared_alloc_ret = 0;
    hvdxg.d3d12_shared_alloc_device = req->device.v;
    hvdxg.d3d12_shared_alloc_resource_in = req->resource.v;
    hvdxg.d3d12_shared_alloc_resource_out = 0;
    hvdxg.d3d12_shared_alloc_allocation = 0;
    hvdxg.d3d12_shared_alloc_count = req->alloc_count;
    hvdxg.d3d12_shared_alloc_flags = req->flags.value;
    hvdxg.d3d12_shared_runtime_d3d12_flags =
        hvdxg_runtime_d3d12_resource_flags(runtime,
                                           req->private_runtime_data_size);
    hvdxg.d3d12_shared_alloc_global_share = req->global_share.v;
    hvdxg.d3d12_shared_allocinfo_offset =
        hvdxg.allocation_last_allocinfo_offset;
    hvdxg.d3d12_shared_runtime_offset =
        hvdxg.allocation_last_private_offset;
    hvdxg.d3d12_shared_resource_priv_offset =
        hvdxg.d3d12_shared_runtime_offset +
        req->private_runtime_data_size;
    hvdxg.d3d12_shared_alloc_priv_offset =
        hvdxg.d3d12_shared_resource_priv_offset + req->priv_drv_data_size;
    hvdxg.d3d12_shared_wire_flags = req->flags.value;
    hvdxg.d3d12_shared_wire_make_resident = 0;
    hvdxg.d3d12_shared_wire_rt_resource =
        req->private_runtime_resource_handle;
    hvdxg.d3d12_shared_result_flags = 0;
    hvdxg.d3d12_shared_result_global_share = 0;
    hvdxg.d3d12_shared_result_vgpu_flags = 0;
    hvdxg.d3d12_shared_result_alloc_flags = 0;
    hvdxg.d3d12_shared_result_min_len = 0;
    hvdxg.d3d12_shared_result_len = 0;
    hvdxg.d3d12_shared_result_flags_offset =
        hvdxg.allocation_last_result_flags_offset;
    hvdxg.d3d12_shared_result_resource_offset =
        hvdxg.allocation_last_result_resource_offset;
    hvdxg.d3d12_shared_result_global_offset =
        hvdxg.allocation_last_result_global_offset;
    hvdxg.d3d12_shared_result_vgpu_offset =
        hvdxg.allocation_last_result_vgpu_offset;
    hvdxg.d3d12_shared_result_allocinfo_offset =
        hvdxg.allocation_last_result_allocinfo_offset;
    hvdxg.d3d12_shared_result_allocinfo_size =
        hvdxg.allocation_last_result_allocinfo_size;
    hvdxg.d3d12_shared_result_head_len = 0;
    hvdxg.d3d12_shared_result_flag_norm = 0;
    hvdxg.d3d12_shared_result_flag_norm_reason = 0;
    hvdxg.d3d12_shared_result_flag_candidate = 0;
    hvdxg.d3d12_shared_result_flag_delta = 0;
    hvdxg.d3d12_shared_track_shared = req->flags.create_shared;
    hvdxg.d3d12_shared_track_nt = req->flags.nt_security_sharing;
    hvdxg.d3d12_shared_track_metadata =
        req->flags.create_shared && req->flags.nt_security_sharing ? 1 : 0;
    hvdxg.d3d12_shared_track_sent_bytes = 0;
    hvdxg.d3d12_shared_track_alloc_from_host = 0;
    hvdxg.d3d12_shared_alloc_process = dxg_process;
    hvdxg.d3d12_shared_alloc_size = 0;
    hvdxg.d3d12_shared_alloc_rt_resource =
        req->private_runtime_resource_handle;
    hvdxg.d3d12_shared_runtime_size = req->private_runtime_data_size;
    hvdxg.d3d12_shared_resource_priv_size = req->priv_drv_data_size;
    hvdxg.d3d12_shared_alloc_priv_size = alloc_info[0].priv_drv_data_size;
    hvdxg.d3d12_shared_alloc_out_priv_size = 0;
    hvdxg.d3d12_shared_runtime_head_len = 0;
    hvdxg.d3d12_shared_resource_priv_head_len = 0;
    hvdxg.d3d12_shared_alloc_priv_head_len = 0;
    hvdxg.d3d12_shared_alloc_out_priv_head_len = 0;
    hvdxg_save_priv_head(hvdxg.d3d12_shared_runtime_head,
                         sizeof(hvdxg.d3d12_shared_runtime_head),
                         &hvdxg.d3d12_shared_runtime_head_len,
                         runtime, req->private_runtime_data_size);
    if (hvdxg.d3d12_shared_runtime_user_copy_ret == 0) {
        uint32 compare_len = hvdxg.d3d12_shared_runtime_head_len;

        if (compare_len > hvdxg.d3d12_shared_runtime_user_head_len)
            compare_len = hvdxg.d3d12_shared_runtime_user_head_len;
        if (hvdxg.d3d12_shared_runtime_user_head_len !=
                hvdxg.d3d12_shared_runtime_head_len ||
            (compare_len != 0 &&
             memcmp(hvdxg.d3d12_shared_runtime_user_head,
                    hvdxg.d3d12_shared_runtime_head, compare_len) != 0))
            hvdxg.d3d12_shared_runtime_user_mismatch = 1;
    }
    hvdxg_save_priv_head(hvdxg.d3d12_shared_resource_priv_head,
                         sizeof(hvdxg.d3d12_shared_resource_priv_head),
                         &hvdxg.d3d12_shared_resource_priv_head_len,
                         resource_priv, req->priv_drv_data_size);
    hvdxg_save_priv_head(hvdxg.d3d12_shared_alloc_priv_head,
                         sizeof(hvdxg.d3d12_shared_alloc_priv_head),
                         &hvdxg.d3d12_shared_alloc_priv_head_len,
                         alloc_priv, alloc_info[0].priv_drv_data_size);
    memset(hvdxg.d3d12_shared_alloc_out_priv_head, 0,
           sizeof(hvdxg.d3d12_shared_alloc_out_priv_head));
    memset(hvdxg.d3d12_shared_result_head, 0,
           sizeof(hvdxg.d3d12_shared_result_head));
}

static void hvdxg_note_d3d12_shared_createallocation_result(
    uint32 len, int32 ret, const struct d3dkmt_createallocation *req,
    uint32 requested_flags,
    const struct d3dddi_allocationinfo2 *alloc_info,
    const struct hvdxg_command_createallocation_return *result,
    const uint8 *alloc_private_data)
{
    uint32 out_size = 0;

    if (req == NULL || requested_flags != 0x47)
        return;

    hvdxg.d3d12_shared_alloc_len = len;
    hvdxg.d3d12_shared_alloc_ret = ret;
    hvdxg.d3d12_shared_alloc_resource_out = req->resource.v;
    hvdxg.d3d12_shared_alloc_global_share = req->global_share.v;
    if (alloc_info != NULL)
        hvdxg.d3d12_shared_alloc_allocation = alloc_info[0].allocation.v;
    if (result != NULL) {
        hvdxg.d3d12_shared_result_min_len =
            hvdxg.allocation_last_result_min_len;
        hvdxg.d3d12_shared_result_len = hvdxg.allocation_last_result_len;
        hvdxg.d3d12_shared_result_flags_offset =
            hvdxg.allocation_last_result_flags_offset;
        hvdxg.d3d12_shared_result_resource_offset =
            hvdxg.allocation_last_result_resource_offset;
        hvdxg.d3d12_shared_result_global_offset =
            hvdxg.allocation_last_result_global_offset;
        hvdxg.d3d12_shared_result_vgpu_offset =
            hvdxg.allocation_last_result_vgpu_offset;
        hvdxg.d3d12_shared_result_allocinfo_offset =
            hvdxg.allocation_last_result_allocinfo_offset;
        hvdxg.d3d12_shared_result_allocinfo_size =
            hvdxg.allocation_last_result_allocinfo_size;
        hvdxg_save_priv_head(hvdxg.d3d12_shared_result_head,
                             sizeof(hvdxg.d3d12_shared_result_head),
                             &hvdxg.d3d12_shared_result_head_len,
                             (const uint8 *)result, len);
        hvdxg.d3d12_shared_alloc_size =
            result->allocation_info[0].allocation_size;
        hvdxg.d3d12_shared_result_flags = result->flags.value;
        hvdxg.d3d12_shared_result_global_share = result->global_share.v;
        hvdxg.d3d12_shared_result_vgpu_flags = result->vgpu_flags;
        hvdxg.d3d12_shared_result_alloc_flags =
            result->allocation_info[0].allocation_flags;
        hvdxg.d3d12_shared_result_flag_delta =
            result->flags.value ^ requested_flags;
        /*
         * Keep host-returned flags byte-for-byte.  WSL traces give us a
         * candidate mask to compare, but no local rule here proves 0x4000 is
         * safe to strip before user copyout or later tracking.
         */
        hvdxg.d3d12_shared_result_flag_norm = 0;
        hvdxg.d3d12_shared_result_flag_norm_reason =
            (result->flags.value & 0x4000U) != 0 ? 1 : 0;
        hvdxg.d3d12_shared_result_flag_candidate =
            result->flags.value & ~0x4000U;
        out_size = result->allocation_info[0].priv_drv_data_size;
    }
    if (alloc_private_data != NULL && out_size != 0) {
        hvdxg.d3d12_shared_alloc_out_priv_size = out_size;
        hvdxg_save_priv_head(hvdxg.d3d12_shared_alloc_out_priv_head,
                             sizeof(hvdxg.d3d12_shared_alloc_out_priv_head),
                             &hvdxg.d3d12_shared_alloc_out_priv_head_len,
                             alloc_private_data, out_size);
    }
}

static void hvdxg_status_append_hex(char *status, size_t status_size,
                                    int *len, const char *prefix,
                                    const uint8 *data, uint32 data_len)
{
    uint32 shown = data_len < HV_DXG_SHARED_ALLOC_HEAD_MAX ?
                   data_len : HV_DXG_SHARED_ALLOC_HEAD_MAX;

    if (*len < 0 || (size_t)*len >= status_size)
        return;
    *len += snprintf(status + *len, status_size - (size_t)*len,
                     "%s_len:%u %s:", prefix, data_len, prefix);
    for (uint32 i = 0; i < shown && *len > 0 &&
         (size_t)*len < status_size; i++) {
        *len += snprintf(status + *len, status_size - (size_t)*len,
                         "%02x", data[i]);
    }
    if ((size_t)*len < status_size)
        *len += snprintf(status + *len, status_size - (size_t)*len, "\n");
}

static void hvdxg_status_append_hex_inline(char *status, size_t status_size,
                                           int *len, const uint8 *data,
                                           uint32 data_len)
{
    if (*len < 0 || (size_t)*len >= status_size)
        return;
    for (uint32 i = 0; i < data_len && (size_t)*len < status_size; i++) {
        *len += snprintf(status + *len, status_size - (size_t)*len,
                         "%02x", data[i]);
    }
}

static void hvdxg_status_append_queryadapter_payloads(char *status,
                                                      size_t status_size,
                                                      int *len)
{
    if (*len < 0 || (size_t)*len >= status_size)
        return;
	    *len += snprintf(status + *len, status_size - (size_t)*len,
	        "dxg_queryadapter_source=last:%u real_host:%u alias_cache:%u "
	        "fallback:%u staged:%u cache_hits:%u cache_misses:%u stores:%u full:%u "
	        "last:type:%u size:%u len:%u alias:0x%x host:0x%x hash:%08x "
	        "result:%u staged_last:type:%u size:%u alias:0x%x host:0x%x "
	        "path:%u ret:%d type0_cache:%u/%u/0x%x/%08x/%08x\n",
        hvdxg.queryadapter_source_last,
        hvdxg.queryadapter_source_real_host,
        hvdxg.queryadapter_source_alias_cache,
        hvdxg.queryadapter_source_fallback,
        hvdxg.queryadapter_source_staged,
        hvdxg.queryadapter_alias_cache_hits,
        hvdxg.queryadapter_alias_cache_misses,
        hvdxg.queryadapter_alias_cache_stores,
        hvdxg.queryadapter_alias_cache_full,
        hvdxg.queryadapter_alias_cache_last_type,
        hvdxg.queryadapter_alias_cache_last_size,
        hvdxg.queryadapter_alias_cache_last_len,
        hvdxg.queryadapter_alias_cache_last_alias,
        hvdxg.queryadapter_alias_cache_last_host,
        hvdxg.queryadapter_alias_cache_last_hash,
        hvdxg.queryadapter_alias_cache_last_result,
        hvdxg.queryadapter_alias_staged_type,
        hvdxg.queryadapter_alias_staged_size,
	        hvdxg.queryadapter_alias_staged_alias,
	        hvdxg.queryadapter_alias_staged_host,
	        hvdxg.queryadapter_alias_staged_path,
	        hvdxg.queryadapter_alias_staged_ret,
	        hvdxg.queryadapter_type0_cache_valid,
	        hvdxg.queryadapter_type0_cache_size,
	        hvdxg.queryadapter_type0_cache_host,
	        hvdxg.queryadapter_type0_cache_input_hash,
	        hvdxg.queryadapter_type0_cache_hash);
    if (*len < 0 || (size_t)*len >= status_size)
        return;
    *len += snprintf(status + *len, status_size - (size_t)*len,
        "dxg_queryadapter_payload_history=index:%u "
        "route:0=none,1=vgpu,2=global\n",
        hvdxg.queryadapter_history_index);
    for (uint32 i = 0; i < HV_DXG_QUERY_HISTORY_MAX &&
         *len >= 0 && (size_t)*len < status_size; i++) {
        *len += snprintf(status + *len, status_size - (size_t)*len,
            "dxg_queryadapter_payload_h%u=type:%u size:%u len:%u "
            "ret:%d status:0x%x route:%u adapter:0x%x host:0x%x "
            "head_len:%u head:",
            i, hvdxg.queryadapter_history_type[i],
            hvdxg.queryadapter_history_size[i],
            hvdxg.queryadapter_history_len[i],
            hvdxg.queryadapter_history_ret[i],
            hvdxg.queryadapter_history_status[i],
            hvdxg.queryadapter_history_route[i],
            hvdxg.queryadapter_history_adapter[i],
            hvdxg.queryadapter_history_host_adapter[i],
            hvdxg.queryadapter_history_head_len[i]);
        hvdxg_status_append_hex_inline(status, status_size, len,
            hvdxg.queryadapter_history_head[i],
            hvdxg.queryadapter_history_head_len[i]);
        if (*len >= 0 && (size_t)*len < status_size)
            *len += snprintf(status + *len,
                             status_size - (size_t)*len, "\n");
    }
}

static void hvdxg_status_append_ioctl_timing(char *status, size_t status_size,
                                             int *len, const uint32 *top_nr)
{
    uint64 avg_us[HV_DXG_IOCTL_TIME_TOP];
    uint64 max_us[HV_DXG_IOCTL_TIME_TOP];
    uint64 last_us[HV_DXG_IOCTL_TIME_TOP];
    uint32 count[HV_DXG_IOCTL_TIME_TOP];

    for (uint32 i = 0; i < HV_DXG_IOCTL_TIME_TOP; i++) {
        uint32 nr = top_nr[i];

        count[i] = hvdxg.ioctl_nr_count[nr];
        avg_us[i] = count[i] == 0 ? 0 :
            hvdxg_ticks_to_us(hvdxg.ioctl_nr_total_ticks[nr] / count[i]);
        max_us[i] = hvdxg_ticks_to_us(hvdxg.ioctl_nr_max_ticks[nr]);
        last_us[i] = hvdxg_ticks_to_us(hvdxg.ioctl_nr_last_ticks[nr]);
    }

    if (*len < 0 || (size_t)*len >= status_size)
        return;
    *len += snprintf(status + *len, status_size - (size_t)*len,
        "d3dkmt_ioctl_time=timebase:%lu "
        "top0:nr:%u count:%u avg_us:%lu max_us:%lu last_us:%lu "
        "top1:nr:%u count:%u avg_us:%lu max_us:%lu last_us:%lu "
        "top2:nr:%u count:%u avg_us:%lu max_us:%lu last_us:%lu "
        "top3:nr:%u count:%u avg_us:%lu max_us:%lu last_us:%lu\n",
        __timebase_frequency,
        top_nr[0], count[0], avg_us[0], max_us[0], last_us[0],
