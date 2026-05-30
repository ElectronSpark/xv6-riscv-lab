/*
 * Hyper-V DXG: handle manager (hmgr), object table, object/sync/process tracking.
 *
 * Part of the dev/hyperv unity translation unit (included by module.c).
 * Split out of the former hyperv_dxg_objects_shared.c for readability;
 * include order in module.c preserves the original definition order.
 */

#define HV_DXG_HMGR_INSTANCE_BITS 6U
#define HV_DXG_HMGR_INDEX_BITS 24U
#define HV_DXG_HMGR_UNIQUE_BITS 2U
#define HV_DXG_HMGR_INSTANCE_SHIFT 0U
#define HV_DXG_HMGR_INDEX_SHIFT \
    (HV_DXG_HMGR_INSTANCE_SHIFT + HV_DXG_HMGR_INSTANCE_BITS)
#define HV_DXG_HMGR_UNIQUE_SHIFT \
    (HV_DXG_HMGR_INDEX_SHIFT + HV_DXG_HMGR_INDEX_BITS)
#define HV_DXG_HMGR_INSTANCE_MASK \
    (((1U << HV_DXG_HMGR_INSTANCE_BITS) - 1U) << \
     HV_DXG_HMGR_INSTANCE_SHIFT)
#define HV_DXG_HMGR_INDEX_MASK \
    (((1U << HV_DXG_HMGR_INDEX_BITS) - 1U) << \
     HV_DXG_HMGR_INDEX_SHIFT)
#define HV_DXG_HMGR_UNIQUE_MASK \
    (((1U << HV_DXG_HMGR_UNIQUE_BITS) - 1U) << \
     HV_DXG_HMGR_UNIQUE_SHIFT)
#define HV_DXG_HMGR_MIN_FREE_ENTRIES 128U

static uint32 hvdxg_hmgr_index(uint64 handle)
{
    return (uint32)((handle & HV_DXG_HMGR_INDEX_MASK) >>
                    HV_DXG_HMGR_INDEX_SHIFT);
}

static uint32 hvdxg_hmgr_unique(uint64 handle)
{
    return (uint32)((handle & HV_DXG_HMGR_UNIQUE_MASK) >>
                    HV_DXG_HMGR_UNIQUE_SHIFT);
}

static uint32 hvdxg_hmgr_instance(uint64 handle)
{
    return (uint32)((handle & HV_DXG_HMGR_INSTANCE_MASK) >>
                    HV_DXG_HMGR_INSTANCE_SHIFT);
}

static struct hvdxg_object_entry *
hvdxg_owner_find_object(struct hvdxg_open_state *owner, uint32 type,
                        uint64 handle);

static int hvdxg_hmgr_handle_index_unique_valid(
    struct hvdxg_open_state *owner, uint32 type, uint32 handle)
{
    struct hvdxg_object_entry *entry;

    if (owner == NULL || handle == 0)
        return 0;
    entry = hvdxg_owner_find_object(owner, type, handle);
    return entry != NULL &&
           entry->index == hvdxg_hmgr_index(handle) &&
           entry->unique == hvdxg_hmgr_unique(handle) &&
           entry->instance == hvdxg_hmgr_instance(handle);
}

static int hvdxg_hmgr_object_ref_active(
    struct hvdxg_open_state *owner, uint32 type, uint32 handle)
{
    struct hvdxg_object_entry *entry;

    entry = hvdxg_owner_find_object(owner, type, handle);
    return entry != NULL && entry->refs != 0;
}

static uint32 hvdxg_make_local_adapter_handle(uint32 index, uint32 unique)
{
    return (unique << HV_DXG_HMGR_UNIQUE_SHIFT) |
           (index << HV_DXG_HMGR_INDEX_SHIFT) | 1U;
}

static void hvdxg_note_hmgr_min_free(void)
{
    hvdxg.object_table_min_free_entries = HV_DXG_HMGR_MIN_FREE_ENTRIES;
    hvdxg.local_adapter_min_free_entries = HV_DXG_HMGR_MIN_FREE_ENTRIES;
}

static int hvdxg_hmgr_reuse_ready(uint32 alloc_serial,
                                  uint32 destroyed_serial);

static uint32 hvdxg_process_adapter_index(
    struct hvdxg_process_state *process,
    const struct hvdxg_process_adapter *adapter)
{
    if (process == NULL || process->adapters == NULL || adapter == NULL)
        return 0xffffffffU;
    return (uint32)(adapter - process->adapters);
}

static struct hvdxg_process_adapter *
hvdxg_process_find_adapter(struct hvdxg_process_state *process,
                           uint32 host_adapter)
{
    if (process == NULL || process->adapters == NULL || host_adapter == 0)
        return NULL;
    for (uint32 i = 0; i < process->adapter_count; i++) {
        if (process->adapters[i].destroyed == 0 &&
            process->adapters[i].host_adapter_handle == host_adapter)
            return &process->adapters[i];
    }
    return NULL;
}

static struct hvdxg_process_adapter *
hvdxg_process_get_adapter(struct hvdxg_process_state *process,
                          uint32 host_adapter, int *created_out)
{
    struct hvdxg_process_adapter *adapter;
    int ret;

    if (created_out != NULL)
        *created_out = 0;
    if (process == NULL || host_adapter == 0)
        return NULL;
    adapter = hvdxg_process_find_adapter(process, host_adapter);
    if (adapter != NULL) {
        adapter->refs++;
        return adapter;
    }
    ret = hvdxg_grow_table((void **)&process->adapters,
                           &process->adapter_capacity,
                           process->adapter_count + 1,
                           sizeof(process->adapters[0]),
                           HV_DXG_OPEN_TRACKED_MAX);
    if (ret != 0)
        return NULL;
    adapter = &process->adapters[process->adapter_count++];
    memset(adapter, 0, sizeof(*adapter));
    adapter->host_adapter_handle = host_adapter;
    adapter->adapter_luid = hvdxg_user_adapter_luid(NULL);
    adapter->host_adapter_luid = hvdxg.host_adapter_luid;
    adapter->host_vgpu_luid = hvdxg_ext_adapter_luid(NULL);
    adapter->refs = 1;
    if (process->next_adapter_generation == 0)
        process->next_adapter_generation = 1;
    adapter->generation = process->next_adapter_generation++;
    if (created_out != NULL)
        *created_out = 1;
    return adapter;
}

static int hvdxg_process_adapter_add_device(
    struct hvdxg_process_adapter *adapter, uint32 device)
{
    int ret;

    if (adapter == NULL || device == 0)
        return 0;
    ret = hvdxg_track_u32_grow(&adapter->devices,
                               &adapter->device_count,
                               &adapter->device_capacity,
                               device);
    hvdxg.process_adapter_last_add_device = device;
    hvdxg.process_adapter_last_device_count = adapter->device_count;
    hvdxg.process_adapter_last_destroyed = adapter->destroyed;
    return ret;
}

static void hvdxg_process_adapter_remove_device(
    struct hvdxg_process_state *process, uint32 device)
{
    if (process == NULL || process->adapters == NULL || device == 0)
        return;
    for (uint32 i = 0; i < process->adapter_count; i++) {
        hvdxg_untrack_u32(process->adapters[i].devices,
                          &process->adapters[i].device_count, device);
        hvdxg.process_adapter_last_remove_device = device;
        hvdxg.process_adapter_last_device_count =
            process->adapters[i].device_count;
        hvdxg.process_adapter_last_destroyed =
            process->adapters[i].destroyed;
    }
}

static struct hvdxg_local_adapter_entry *
hvdxg_process_find_local_adapter(struct hvdxg_process_state *process,
                                 uint32 handle, int include_destroyed)
{
    if (process == NULL || process->local_adapters == NULL || handle == 0)
        return NULL;
    for (uint32 i = 0; i < process->local_adapter_count; i++) {
        if (process->local_adapters[i].handle == handle &&
            (include_destroyed ||
             process->local_adapters[i].destroyed == 0))
            return &process->local_adapters[i];
    }
    return NULL;
}

static struct hvdxg_process_adapter *
hvdxg_process_adapter_by_local_handle(struct hvdxg_process_state *process,
                                      uint32 handle,
                                      struct hvdxg_local_adapter_entry **le_out)
{
    struct hvdxg_local_adapter_entry *local;

    if (le_out != NULL)
        *le_out = NULL;
    local = hvdxg_process_find_local_adapter(process, handle, 0);
    if (local == NULL)
        return NULL;
    if (le_out != NULL)
        *le_out = local;
    if (process == NULL || process->adapters == NULL ||
        local->adapter_index >= process->adapter_count)
        return NULL;
    if (process->adapters[local->adapter_index].destroyed != 0)
        return NULL;
    return &process->adapters[local->adapter_index];
}

static uint32 hvdxg_alloc_process_local_adapter_handle(
    struct hvdxg_process_state *process, uint32 adapter_index)
{
    struct hvdxg_local_adapter_entry *slot = NULL;
    uint32 index = 0;
    uint32 serial;
    uint32 handle = 0;
    int ret;

    if (process == NULL || adapter_index >= process->adapter_count)
        return 0;
    hvdxg_note_hmgr_min_free();
    process->local_adapter_alloc_serial++;
    if (process->local_adapter_alloc_serial == 0)
        process->local_adapter_alloc_serial++;
    serial = process->local_adapter_alloc_serial;
    for (uint32 i = 0; i < process->local_adapter_count; i++) {
        if (process->local_adapters[i].destroyed == 0)
            continue;
        if (!hvdxg_hmgr_reuse_ready(
                serial, process->local_adapters[i].destroyed_serial)) {
            hvdxg.local_adapter_reuse_delayed++;
            continue;
        }
        slot = &process->local_adapters[i];
        hvdxg.local_adapter_reuse_allowed++;
        break;
    }
    if (slot == NULL) {
        if (process->local_adapter_count >=
            (1U << HV_DXG_HMGR_INDEX_BITS))
            return 0;
        ret = hvdxg_grow_table((void **)&process->local_adapters,
                               &process->local_adapter_capacity,
                               process->local_adapter_count + 1,
                               sizeof(process->local_adapters[0]),
                               HV_DXG_OPEN_TRACKED_MAX);
        if (ret != 0)
            return 0;
        index = process->local_adapter_count++;
        slot = &process->local_adapters[index];
        slot->unique = 1;
    } else {
        index = (uint32)(slot - process->local_adapters);
        if (slot->unique >= ((1U << HV_DXG_HMGR_UNIQUE_BITS) - 1U))
            slot->unique = 1;
        else
            slot->unique++;
    }
    handle = hvdxg_make_local_adapter_handle(index, slot->unique);
    memset(slot, 0, sizeof(*slot));
    slot->unique = hvdxg_hmgr_unique(handle);
    slot->handle = handle;
    slot->adapter_index = adapter_index;
    if (process->next_local_adapter_generation == 0)
        process->next_local_adapter_generation = 1;
    slot->generation = process->next_local_adapter_generation++;
    return handle;
}

static struct hvdxg_object_entry **hvdxg_owner_object_table(
    struct hvdxg_open_state *owner)
{
    if (owner == NULL)
        return NULL;
    if (owner->process_state != NULL)
        return &owner->process_state->objects;
    return &owner->objects;
}

static uint32 *hvdxg_owner_object_count(struct hvdxg_open_state *owner)
{
    if (owner == NULL)
        return NULL;
    if (owner->process_state != NULL)
        return &owner->process_state->object_count;
    return &owner->object_count;
}

static uint32 *hvdxg_owner_object_capacity(struct hvdxg_open_state *owner)
{
    if (owner == NULL)
        return NULL;
    if (owner->process_state != NULL)
        return &owner->process_state->object_capacity;
    return &owner->object_capacity;
}

#define HV_DXG_HMGR_FREE_NONE 0xffffffffU

static uint32 *hvdxg_owner_object_free_count(struct hvdxg_open_state *owner)
{
    if (owner == NULL)
        return NULL;
    if (owner->process_state != NULL)
        return &owner->process_state->object_free_count;
    return &owner->object_free_count;
}

static uint32 *hvdxg_owner_object_free_head(struct hvdxg_open_state *owner)
{
    if (owner == NULL)
        return NULL;
    if (owner->process_state != NULL)
        return &owner->process_state->object_free_head;
    return &owner->object_free_head;
}

static uint32 *hvdxg_owner_object_free_tail(struct hvdxg_open_state *owner)
{
    if (owner == NULL)
        return NULL;
    if (owner->process_state != NULL)
        return &owner->process_state->object_free_tail;
    return &owner->object_free_tail;
}

static uint32 hvdxg_open_host_process(struct hvdxg_open_state *owner);
static uint32 hvdxg_open_process_generation(struct hvdxg_open_state *owner);
static uint32 hvdxg_open_process_refs(struct hvdxg_open_state *owner);
static const char *hvdxg_early_bind_source_name(uint32 source);

static uint32 *hvdxg_owner_object_generation(struct hvdxg_open_state *owner)
{
    if (owner == NULL)
        return NULL;
    if (owner->process_state != NULL)
        return &owner->process_state->next_object_generation;
    return &owner->next_generation;
}

static uint32 *hvdxg_owner_object_alloc_serial(
    struct hvdxg_open_state *owner)
{
    if (owner == NULL)
        return NULL;
    if (owner->process_state != NULL)
        return &owner->process_state->object_alloc_serial;
    return &owner->object_alloc_serial;
}

static uint32 *hvdxg_owner_object_destroy_serial(
    struct hvdxg_open_state *owner)
{
    if (owner == NULL)
        return NULL;
    if (owner->process_state != NULL)
        return &owner->process_state->object_destroy_serial;
    return &owner->object_destroy_serial;
}

static int hvdxg_hmgr_reuse_ready(uint32 alloc_serial,
                                  uint32 destroyed_serial)
{
    if (destroyed_serial == 0)
        return 0;
    return alloc_serial - destroyed_serial >=
           HV_DXG_HMGR_MIN_FREE_ENTRIES;
}

static void hvdxg_hmgr_init_free_entry(struct hvdxg_object_entry *entry,
                                       uint32 index)
{
    memset(entry, 0, sizeof(*entry));
    entry->index = index;
    entry->free_prev = HV_DXG_HMGR_FREE_NONE;
    entry->free_next = HV_DXG_HMGR_FREE_NONE;
}

static int hvdxg_hmgr_append_free_index(struct hvdxg_open_state *owner,
                                        uint32 index)
{
    struct hvdxg_object_entry **objects = hvdxg_owner_object_table(owner);
    uint32 *count = hvdxg_owner_object_count(owner);
    uint32 *free_count = hvdxg_owner_object_free_count(owner);
    uint32 *free_head = hvdxg_owner_object_free_head(owner);
    uint32 *free_tail = hvdxg_owner_object_free_tail(owner);
    struct hvdxg_object_entry *entry;

    if (objects == NULL || *objects == NULL || count == NULL ||
        free_count == NULL || free_head == NULL || free_tail == NULL ||
        index >= *count)
        return -EINVAL;
    entry = &(*objects)[index];
    if (entry->on_free_list)
        return 0;
    entry->free_prev = *free_tail;
    entry->free_next = HV_DXG_HMGR_FREE_NONE;
    entry->on_free_list = 1;
    if (*free_tail != HV_DXG_HMGR_FREE_NONE)
        (*objects)[*free_tail].free_next = index;
    else
        *free_head = index;
    *free_tail = index;
    (*free_count)++;
    return 0;
}

static int hvdxg_hmgr_remove_free_index(struct hvdxg_open_state *owner,
                                        uint32 index)
{
    struct hvdxg_object_entry **objects = hvdxg_owner_object_table(owner);
    uint32 *count = hvdxg_owner_object_count(owner);
    uint32 *free_count = hvdxg_owner_object_free_count(owner);
    uint32 *free_head = hvdxg_owner_object_free_head(owner);
    uint32 *free_tail = hvdxg_owner_object_free_tail(owner);
    struct hvdxg_object_entry *entry;

    if (objects == NULL || *objects == NULL || count == NULL ||
        free_count == NULL || free_head == NULL || free_tail == NULL ||
        index >= *count)
        return -EINVAL;
    entry = &(*objects)[index];
    if (!entry->on_free_list)
        return 0;
    if (entry->free_prev != HV_DXG_HMGR_FREE_NONE)
        (*objects)[entry->free_prev].free_next = entry->free_next;
    else
        *free_head = entry->free_next;
    if (entry->free_next != HV_DXG_HMGR_FREE_NONE)
        (*objects)[entry->free_next].free_prev = entry->free_prev;
    else
        *free_tail = entry->free_prev;
    entry->free_prev = HV_DXG_HMGR_FREE_NONE;
    entry->free_next = HV_DXG_HMGR_FREE_NONE;
    entry->on_free_list = 0;
    if (*free_count != 0)
        (*free_count)--;
    return 0;
}

static int hvdxg_hmgr_expand_object_table(struct hvdxg_open_state *owner,
                                          uint32 needed)
{
    struct hvdxg_object_entry **objects = hvdxg_owner_object_table(owner);
    uint32 *count = hvdxg_owner_object_count(owner);
    uint32 *capacity = hvdxg_owner_object_capacity(owner);
    uint32 old_count;
    uint32 target;
    int ret;

    if (objects == NULL || count == NULL || capacity == NULL)
        return -EINVAL;
    if (needed > HV_DXG_OBJECT_TABLE_MAX)
        return -EOVERFLOW;
    old_count = *count;
    if (old_count == 0) {
        uint32 *free_count = hvdxg_owner_object_free_count(owner);
        uint32 *free_head = hvdxg_owner_object_free_head(owner);
        uint32 *free_tail = hvdxg_owner_object_free_tail(owner);

        if (free_count != NULL && *free_count == 0) {
            if (free_head != NULL)
                *free_head = HV_DXG_HMGR_FREE_NONE;
            if (free_tail != NULL)
                *free_tail = HV_DXG_HMGR_FREE_NONE;
        }
    }
    target = needed;
    if (target < old_count + HV_DXG_HMGR_MIN_FREE_ENTRIES + 1 &&
        old_count < HV_DXG_OBJECT_TABLE_MAX) {
        target = old_count + HV_DXG_HMGR_MIN_FREE_ENTRIES + 1;
        if (target > HV_DXG_OBJECT_TABLE_MAX)
            target = HV_DXG_OBJECT_TABLE_MAX;
    }
    ret = hvdxg_grow_table((void **)objects, capacity, target,
                           sizeof((*objects)[0]),
                           HV_DXG_OBJECT_TABLE_MAX);
    if (ret != 0)
        return ret;
    for (uint32 i = old_count; i < target; i++) {
        hvdxg_hmgr_init_free_entry(&(*objects)[i], i);
        (*count)++;
        ret = hvdxg_hmgr_append_free_index(owner, i);
        if (ret != 0)
            return ret;
    }
    return 0;
}

static void hvdxg_note_object_free_list(struct hvdxg_open_state *owner)
{
    uint32 *free_count = hvdxg_owner_object_free_count(owner);
    uint32 *free_head = hvdxg_owner_object_free_head(owner);
    uint32 *free_tail = hvdxg_owner_object_free_tail(owner);

    hvdxg.object_table_free_count =
        free_count != NULL ? *free_count : 0;
    hvdxg.object_table_free_head =
        free_head != NULL ? *free_head : HV_DXG_HMGR_FREE_NONE;
    hvdxg.object_table_free_tail =
        free_tail != NULL ? *free_tail : HV_DXG_HMGR_FREE_NONE;
}

static uint32 *hvdxg_object_type_active_counter(uint32 type)
{
    switch (type) {
    case HV_DXG_OBJECT_ADAPTER:
        return &hvdxg.object_type_active_adapter;
    case HV_DXG_OBJECT_DEVICE:
        return &hvdxg.object_type_active_device;
    case HV_DXG_OBJECT_CONTEXT:
        return &hvdxg.object_type_active_context;
    case HV_DXG_OBJECT_HWQUEUE:
        return &hvdxg.object_type_active_hwqueue;
    case HV_DXG_OBJECT_PAGINGQUEUE:
        return &hvdxg.object_type_active_pagingqueue;
    case HV_DXG_OBJECT_SYNC:
        return &hvdxg.object_type_active_sync;
    case HV_DXG_OBJECT_ALLOCATION:
        return &hvdxg.object_type_active_allocation;
    case HV_DXG_OBJECT_RESOURCE:
        return &hvdxg.object_type_active_resource;
    case HV_DXG_OBJECT_GPUVA:
        return &hvdxg.object_type_active_gpuva;
    default:
        return NULL;
    }
}

static void hvdxg_note_object_type_active(uint32 type, int delta)
{
    uint32 *counter = hvdxg_object_type_active_counter(type);

    if (counter == NULL)
        return;
    if (delta > 0) {
        (*counter)++;
    } else if (delta < 0 && *counter != 0) {
        (*counter)--;
    }
}

static void hvdxg_note_object_table_scope(struct hvdxg_open_state *owner)
{
    uint32 *count;

    hvdxg.object_table_scope = 0;
    hvdxg.object_table_scope_process_generation = 0;
    hvdxg.object_table_scope_object_count = 0;
    hvdxg.object_table_scope_local_adapter_count = 0;
    if (owner == NULL)
        return;
    count = hvdxg_owner_object_count(owner);
    if (owner->process_state != NULL) {
        hvdxg.object_table_scope = 1;
        hvdxg.object_table_scope_process_generation =
            owner->process_state->generation;
        hvdxg.object_table_scope_local_adapter_count =
            owner->process_state->local_adapter_count;
    } else {
        hvdxg.object_table_scope = 2;
    }
    hvdxg.object_table_scope_object_count = count != NULL ? *count : 0;
}

static void hvdxg_note_object_lifecycle(uint32 type,
                                        const struct hvdxg_object_entry *entry,
                                        uint32 unique_before,
                                        uint32 unique_after,
                                        uint32 destroyed_before,
                                        uint32 destroyed_after)
{
    hvdxg.object_table_last_lifecycle_type = type;
    hvdxg.object_table_last_lifecycle_index =
        entry != NULL ? entry->index : 0;
    hvdxg.object_table_last_lifecycle_unique_before = unique_before;
    hvdxg.object_table_last_lifecycle_unique_after = unique_after;
    hvdxg.object_table_last_lifecycle_instance =
        entry != NULL ? entry->instance : 0;
    hvdxg.object_table_last_lifecycle_destroyed_before = destroyed_before;
    hvdxg.object_table_last_lifecycle_destroyed_after = destroyed_after;
    hvdxg.object_table_last_lifecycle_on_free_list =
        entry != NULL ? entry->on_free_list : 0;
}

static struct hvdxg_object_entry *
hvdxg_owner_find_object(struct hvdxg_open_state *owner, uint32 type,
                        uint64 handle)
{
    struct hvdxg_object_entry **objects = hvdxg_owner_object_table(owner);
    uint32 *count = hvdxg_owner_object_count(owner);
    struct hvdxg_object_entry *entry;
    uint32 index;
    uint32 unique;
    uint32 instance;

    if (owner == NULL || handle == 0 || type == HV_DXG_OBJECT_NONE)
        return NULL;
    if (objects == NULL || *objects == NULL || count == NULL)
        return NULL;
    index = hvdxg_hmgr_index(handle);
    unique = hvdxg_hmgr_unique(handle);
    instance = hvdxg_hmgr_instance(handle);
    if (index >= *count)
        return NULL;
    entry = &(*objects)[index];
    if (entry->type != type || entry->destroyed != 0 ||
        entry->handle != handle || entry->index != index ||
        entry->unique != unique || entry->instance != instance)
        return NULL;
    return entry;
}

static struct hvdxg_object_entry *
hvdxg_owner_find_object_any(struct hvdxg_open_state *owner, uint64 handle,
                            int include_destroyed)
{
    struct hvdxg_object_entry **objects = hvdxg_owner_object_table(owner);
    uint32 *count = hvdxg_owner_object_count(owner);

    if (owner == NULL || handle == 0)
        return NULL;
    if (objects == NULL || *objects == NULL || count == NULL)
        return NULL;
    if (hvdxg_hmgr_index(handle) < *count) {
        struct hvdxg_object_entry *entry =
            &(*objects)[hvdxg_hmgr_index(handle)];

        if (entry->handle == handle &&
            (include_destroyed || entry->destroyed == 0))
            return entry;
    }
    for (uint32 i = 0; i < *count; i++) {
        if ((*objects)[i].host_handle == handle &&
            (include_destroyed || (*objects)[i].destroyed == 0))
            return &(*objects)[i];
    }
    return NULL;
}

static void hvdxg_note_ntshared_object_entry(
    struct hvdxg_open_state *owner, uint32 process, uint32 object)
{
    struct hvdxg_object_entry *entry;

    hvdxg.ntshared_obj_found = 0;
    hvdxg.ntshared_obj_exact = 0;
    hvdxg.ntshared_obj_local = 0;
    hvdxg.ntshared_obj_host = 0;
    hvdxg.ntshared_obj_type = HV_DXG_OBJECT_NONE;
    hvdxg.ntshared_obj_parent = 0;
    hvdxg.ntshared_obj_device = 0;
    hvdxg.ntshared_obj_owner_process = process;
    hvdxg.ntshared_obj_owner_generation =
        hvdxg_open_process_generation(owner);
    hvdxg.ntshared_obj_generation = 0;
    hvdxg.ntshared_obj_stale = 0;
    hvdxg.ntshared_obj_destroyed = 0;

    entry = hvdxg_owner_find_object_any(owner, object, 1);
    if (entry == NULL)
        return;
    hvdxg.ntshared_obj_found = 1;
    hvdxg.ntshared_obj_exact = entry->handle == object ? 1 : 0;
    hvdxg.ntshared_obj_local = (uint32)entry->handle;
    hvdxg.ntshared_obj_host = (uint32)entry->host_handle;
    hvdxg.ntshared_obj_type = entry->type;
    hvdxg.ntshared_obj_parent = (uint32)entry->parent;
    hvdxg.ntshared_obj_device = entry->device;
    hvdxg.ntshared_obj_generation = entry->generation;
    hvdxg.ntshared_obj_destroyed = entry->destroyed;
    hvdxg.ntshared_obj_stale =
        entry->destroyed != 0 || entry->generation == 0 ? 1 : 0;
}

static void hvdxg_note_createdevice_object(struct hvdxg_open_state *owner,
                                           uint32 device)
{
    struct hvdxg_object_entry *entry;

    hvdxg.createdevice_object_found = 0;
    hvdxg.createdevice_object_type = HV_DXG_OBJECT_NONE;
    hvdxg.createdevice_object_local = 0;
    hvdxg.createdevice_object_host = 0;
    hvdxg.createdevice_object_parent = 0;
    hvdxg.createdevice_object_device = 0;
    hvdxg.createdevice_object_generation = 0;
    hvdxg.createdevice_object_destroyed = 0;
    entry = hvdxg_owner_find_object_any(owner, device, 1);
    if (entry == NULL)
        return;
    hvdxg.createdevice_object_found = 1;
    hvdxg.createdevice_object_type = entry->type;
    hvdxg.createdevice_object_local = (uint32)entry->handle;
    hvdxg.createdevice_object_host = (uint32)entry->host_handle;
    hvdxg.createdevice_object_parent = (uint32)entry->parent;
    hvdxg.createdevice_object_device = entry->device;
    hvdxg.createdevice_object_generation = entry->generation;
    hvdxg.createdevice_object_destroyed = entry->destroyed;
}

static void hvdxg_note_createdevice_unwind(uint32 process, uint32 device,
                                           int ret)
{
    hvdxg.createdevice_unwind_attempts++;
    hvdxg.createdevice_unwind_process = process;
    hvdxg.createdevice_unwind_device = device;
    hvdxg.createdevice_unwind_ret = ret;
    if (ret == 0)
        hvdxg.createdevice_unwind_successes++;
}

static void hvdxg_note_createcontext_unwind(uint32 process, uint32 context,
                                            int ret)
{
    hvdxg.createcontext_unwind_attempts++;
    hvdxg.createcontext_unwind_process = process;
    hvdxg.createcontext_unwind_context = context;
    hvdxg.createcontext_unwind_ret = ret;
    if (ret == 0)
        hvdxg.createcontext_unwind_successes++;
}

static void hvdxg_note_createhwqueue_unwind(uint32 process, uint32 queue,
                                            uint32 fence, int ret)
{
    hvdxg.createhwqueue_unwind_attempts++;
    hvdxg.createhwqueue_unwind_process = process;
    hvdxg.createhwqueue_unwind_queue = queue;
    hvdxg.createhwqueue_unwind_fence = fence;
    hvdxg.createhwqueue_unwind_ret = ret;
    if (ret == 0)
        hvdxg.createhwqueue_unwind_successes++;
}

static int hvdxg_owner_has_object(struct hvdxg_open_state *owner,
                                  uint32 type, uint64 handle)
{
    if (owner == NULL || handle == 0)
        return 0;
    if (hvdxg_owner_find_object(owner, type, handle) != NULL)
        return 1;
    hvdxg.object_table_denied++;
    return 0;
}

static int hvdxg_track_object(struct hvdxg_open_state *owner, uint32 type,
                              uint64 handle, uint64 parent, uint32 device)
{
    struct hvdxg_object_entry **objects = hvdxg_owner_object_table(owner);
    uint32 *count = hvdxg_owner_object_count(owner);
    uint32 *next_generation = hvdxg_owner_object_generation(owner);
    uint32 *alloc_serial = hvdxg_owner_object_alloc_serial(owner);
    uint32 *free_count = hvdxg_owner_object_free_count(owner);
    struct hvdxg_object_entry *slot;
    uint32 index;
    int ret;

    if (owner == NULL || handle == 0 || type == HV_DXG_OBJECT_NONE)
        return 0;
    if (objects == NULL || count == NULL || next_generation == NULL ||
        alloc_serial == NULL || free_count == NULL)
        return -EINVAL;
    hvdxg_note_hmgr_min_free();
    (*alloc_serial)++;
    index = hvdxg_hmgr_index(handle);
    if (index >= HV_DXG_OBJECT_TABLE_MAX) {
        hvdxg.object_table_drops++;
        return -EOVERFLOW;
    }
    slot = hvdxg_owner_find_object(owner, type, handle);
    if (slot != NULL) {
        slot->host_handle = handle;
        slot->parent = parent;
        slot->device = device;
        if (slot->refs == 0)
            slot->refs = 1;
        return 0;
    }
    ret = hvdxg_hmgr_expand_object_table(owner, index + 1);
    if (ret != 0) {
        hvdxg.object_table_drops++;
        return ret;
    }
    slot = &(*objects)[index];
    if (slot->type != HV_DXG_OBJECT_NONE && slot->destroyed == 0) {
        hvdxg.object_table_denied++;
        return -EEXIST;
    }
    if (slot->destroyed != 0) {
        if (slot->handle == handle) {
            hvdxg.object_table_reuse_delayed++;
            return -EAGAIN;
        }
        if (*free_count <= HV_DXG_HMGR_MIN_FREE_ENTRIES) {
            ret = hvdxg_hmgr_expand_object_table(
                owner, *count + HV_DXG_HMGR_MIN_FREE_ENTRIES + 1);
            if (ret != 0) {
                hvdxg.object_table_reuse_delayed++;
                return ret;
            }
            slot = &(*objects)[index];
        }
        hvdxg.object_table_reuse_allowed++;
    }
    if (slot->on_free_list) {
        ret = hvdxg_hmgr_remove_free_index(owner, index);
        if (ret != 0) {
            hvdxg.object_table_drops++;
            return ret;
        }
    }
    memset(slot, 0, sizeof(*slot));
    slot->type = type;
    slot->handle = handle;
    slot->host_handle = handle;
    slot->parent = parent;
    slot->device = device;
    slot->refs = 1;
    slot->index = hvdxg_hmgr_index(handle);
    slot->unique = hvdxg_hmgr_unique(handle);
    slot->instance = hvdxg_hmgr_instance(handle);
    slot->destroyed_serial = 0;
    slot->free_prev = HV_DXG_HMGR_FREE_NONE;
    slot->free_next = HV_DXG_HMGR_FREE_NONE;
    if (*next_generation == 0)
        *next_generation = 1;
    slot->generation = (*next_generation)++;
    hvdxg.object_table_generation = slot->generation;
    if (*count > hvdxg.object_table_max)
        hvdxg.object_table_max = *count;
    hvdxg_note_object_type_active(type, 1);
    hvdxg_note_object_lifecycle(type, slot, 0, slot->unique, 0,
                                slot->destroyed);
    hvdxg_note_object_free_list(owner);
    hvdxg_note_object_table_scope(owner);
    return 0;
}

static int hvdxg_get_local_adapter_handle(struct hvdxg_open_state *owner,
                                          uint32 *adapter_out)
{
    struct hvdxg_process_adapter *adapter;
    struct hvdxg_process_state *process;
    uint32 adapter_index;
    uint32 local;
    int created = 0;

    if (adapter_out == NULL || hvdxg.host_adapter_handle == 0)
        return -EINVAL;
    if (owner == NULL)
        return -EINVAL;
    process = owner->process_state;
    if (process == NULL)
        return -EINVAL;
    adapter = hvdxg_process_get_adapter(process, hvdxg.host_adapter_handle,
                                        &created);
    if (adapter == NULL)
        return -ENOMEM;
    adapter_index = hvdxg_process_adapter_index(process, adapter);
    local = hvdxg_alloc_process_local_adapter_handle(process, adapter_index);
    if (local == 0) {
        if (adapter->refs > 0)
            adapter->refs--;
        if (adapter->refs == 0)
            adapter->destroyed = 1;
        return -ENOMEM;
    }
    adapter->local_handle_count++;
    hvdxg.local_adapter_last_result = created ? 2 : 1;
    hvdxg.local_adapter_last_handle = local;
    hvdxg.local_adapter_last_host = adapter->host_adapter_handle;
    hvdxg.local_adapter_last_refs = adapter->refs;
    hvdxg.local_adapter_last_locals = adapter->local_handle_count;
    hvdxg.local_adapter_last_generation = adapter->generation;
    *adapter_out = local;
    return 0;
}

static int hvdxg_resolve_adapter_handle(struct hvdxg_open_state *owner,
                                        uint32 adapter, uint32 *host_out)
{
    struct hvdxg_object_entry *entry;
    struct hvdxg_process_adapter *process_adapter;

    if (host_out != NULL)
        *host_out = 0;
    if (adapter == 0 || hvdxg.host_adapter_handle == 0)
        return -EINVAL;
    if (owner != NULL && owner->process_state != NULL) {
        process_adapter = hvdxg_process_adapter_by_local_handle(
            owner->process_state, adapter, NULL);
        if (process_adapter != NULL) {
            hvdxg.local_adapter_namespace_hits++;
            hvdxg.local_adapter_last_result = 3;
            hvdxg.local_adapter_last_handle = adapter;
            hvdxg.local_adapter_last_host =
                process_adapter->host_adapter_handle;
            hvdxg.local_adapter_last_refs = process_adapter->refs;
            hvdxg.local_adapter_last_locals =
                process_adapter->local_handle_count;
            hvdxg.local_adapter_last_generation =
                process_adapter->generation;
            if (host_out != NULL)
                *host_out = process_adapter->host_adapter_handle;
            return 0;
        }
        hvdxg.local_adapter_namespace_misses++;
        hvdxg.local_adapter_last_result = 4;
        hvdxg.local_adapter_last_handle = adapter;
        hvdxg.local_adapter_last_host = 0;
        hvdxg.local_adapter_last_refs = 0;
        hvdxg.local_adapter_last_locals = 0;
        hvdxg.local_adapter_last_generation = 0;
    }
    if ((owner == NULL || owner->process_state == NULL) &&
        adapter == hvdxg.host_adapter_handle) {
        if (host_out != NULL)
            *host_out = hvdxg.host_adapter_handle;
        return 0;
    }
    entry = hvdxg_owner_find_object(owner, HV_DXG_OBJECT_ADAPTER, adapter);
    if (entry == NULL || entry->host_handle != hvdxg.host_adapter_handle)
        return -EINVAL;
    if (host_out != NULL)
        *host_out = (uint32)entry->host_handle;
    return 0;
}

static int hvdxg_resolve_local_adapter_handle(
    struct hvdxg_open_state *owner, uint32 adapter, uint32 *host_out,
    struct hvdxg_process_adapter **adapter_out)
{
    struct hvdxg_process_adapter *process_adapter;

    if (host_out != NULL)
        *host_out = 0;
    if (adapter_out != NULL)
        *adapter_out = NULL;
    if (adapter == 0 || hvdxg.host_adapter_handle == 0 ||
        owner == NULL || owner->process_state == NULL)
        return -EINVAL;
    process_adapter = hvdxg_process_adapter_by_local_handle(
        owner->process_state, adapter, NULL);
    if (process_adapter == NULL) {
        hvdxg.local_adapter_namespace_misses++;
        hvdxg.local_adapter_last_result = 4;
        hvdxg.local_adapter_last_handle = adapter;
        hvdxg.local_adapter_last_host = 0;
        hvdxg.local_adapter_last_refs = 0;
        hvdxg.local_adapter_last_locals = 0;
        hvdxg.local_adapter_last_generation = 0;
        return -EINVAL;
    }
    hvdxg.local_adapter_namespace_hits++;
    hvdxg.local_adapter_last_result = 3;
    hvdxg.local_adapter_last_handle = adapter;
    hvdxg.local_adapter_last_host = process_adapter->host_adapter_handle;
    hvdxg.local_adapter_last_refs = process_adapter->refs;
    hvdxg.local_adapter_last_locals = process_adapter->local_handle_count;
    hvdxg.local_adapter_last_generation = process_adapter->generation;
    if (host_out != NULL)
        *host_out = process_adapter->host_adapter_handle;
    if (adapter_out != NULL)
        *adapter_out = process_adapter;
    return 0;
}

static int hvdxg_close_local_adapter_handle(struct hvdxg_open_state *owner,
                                            uint32 adapter,
                                            uint32 *host_out,
                                            uint32 *final_close_out)
{
    struct hvdxg_local_adapter_entry *local = NULL;
    struct hvdxg_process_adapter *process_adapter;

    if (host_out != NULL)
        *host_out = 0;
    if (final_close_out != NULL)
        *final_close_out = 0;
    if (adapter == 0 || hvdxg.host_adapter_handle == 0)
        return -EINVAL;
    if (owner == NULL) {
        if (adapter != hvdxg.host_adapter_handle)
            return -EINVAL;
        if (host_out != NULL)
            *host_out = hvdxg.host_adapter_handle;
        if (final_close_out != NULL)
            *final_close_out = 1;
        return 0;
    }
    if (owner->process_state == NULL)
        return hvdxg_resolve_adapter_handle(owner, adapter, host_out);
    process_adapter = hvdxg_process_adapter_by_local_handle(
        owner->process_state, adapter, &local);
    if (process_adapter == NULL || local == NULL) {
        hvdxg.local_adapter_namespace_misses++;
        hvdxg.local_adapter_last_result = 4;
        hvdxg.local_adapter_last_handle = adapter;
        hvdxg.local_adapter_last_host = 0;
        hvdxg.local_adapter_last_refs = 0;
        hvdxg.local_adapter_last_locals = 0;
        hvdxg.local_adapter_last_generation = 0;
        return -EINVAL;
    }
    local->destroyed = 1;
    owner->process_state->local_adapter_destroy_serial++;
    if (owner->process_state->local_adapter_destroy_serial == 0)
        owner->process_state->local_adapter_destroy_serial++;
    local->destroyed_serial =
        owner->process_state->local_adapter_alloc_serial;
    if (process_adapter->local_handle_count > 0)
        process_adapter->local_handle_count--;
    if (process_adapter->refs > 0)
        process_adapter->refs--;
    if (process_adapter->refs == 0) {
        process_adapter->destroyed = 1;
        hvdxg.process_adapter_final_close_destroyed++;
        if (final_close_out != NULL)
            *final_close_out = 1;
    }
    hvdxg.local_adapter_namespace_hits++;
    hvdxg.local_adapter_last_result = 5;
    hvdxg.local_adapter_last_handle = adapter;
    hvdxg.local_adapter_last_host = process_adapter->host_adapter_handle;
    hvdxg.local_adapter_last_refs = process_adapter->refs;
    hvdxg.local_adapter_last_locals = process_adapter->local_handle_count;
    hvdxg.local_adapter_last_generation = process_adapter->generation;
    if (host_out != NULL)
        *host_out = process_adapter->host_adapter_handle;
    return 0;
}

struct hvdxg_queryadapter_context {
    struct hvdxg_process_state *process;
    struct hvdxg_process_adapter *process_adapter;
    uint32 host_adapter;
    uint32 resolve_source;
    uint32 local_namespace;
};

static int hvdxg_resolve_queryadapter_context(
    struct hvdxg_open_state *owner, uint32 adapter, uint32 *host_out,
    uint32 *source_out, struct hvdxg_queryadapter_context *context)
{
    struct hvdxg_process_adapter *process_adapter;

    if (host_out != NULL)
        *host_out = 0;
    if (source_out != NULL)
        *source_out = 0;
    if (context != NULL)
        memset(context, 0, sizeof(*context));
    if (adapter == 0 || hvdxg.host_adapter_handle == 0)
        return -EINVAL;
    if (owner == NULL) {
        if (adapter != hvdxg.host_adapter_handle)
            return -EINVAL;
        if (host_out != NULL)
            *host_out = hvdxg.host_adapter_handle;
        if (source_out != NULL)
            *source_out = 1;
        if (context != NULL) {
            context->host_adapter = hvdxg.host_adapter_handle;
            context->resolve_source = 1;
            context->local_namespace = 1;
        }
        return 0;
    }
    if (owner->process_state == NULL)
        return -EINVAL;
    process_adapter = hvdxg_process_adapter_by_local_handle(
        owner->process_state, adapter, NULL);
    if (process_adapter == NULL) {
        hvdxg.local_adapter_namespace_misses++;
        hvdxg.local_adapter_last_result = 4;
        hvdxg.local_adapter_last_handle = adapter;
        hvdxg.local_adapter_last_host = 0;
        hvdxg.local_adapter_last_refs = 0;
        hvdxg.local_adapter_last_locals = 0;
        hvdxg.local_adapter_last_generation = 0;
        if (context != NULL) {
            context->process = owner->process_state;
            context->local_namespace = 3;
        }
        return -EINVAL;
    }
    hvdxg.local_adapter_namespace_hits++;
    hvdxg.local_adapter_last_result = 3;
    hvdxg.local_adapter_last_handle = adapter;
    hvdxg.local_adapter_last_host = process_adapter->host_adapter_handle;
    hvdxg.local_adapter_last_refs = process_adapter->refs;
    hvdxg.local_adapter_last_locals = process_adapter->local_handle_count;
    hvdxg.local_adapter_last_generation = process_adapter->generation;
    if (host_out != NULL)
        *host_out = process_adapter->host_adapter_handle;
    if (source_out != NULL)
        *source_out = 4;
    if (context != NULL) {
        context->process = owner->process_state;
        context->process_adapter = process_adapter;
        context->host_adapter = process_adapter->host_adapter_handle;
        context->resolve_source = 4;
        context->local_namespace = 2;
    }
    return 0;
}

static void hvdxg_note_queryadapter_adapter_object(
    struct hvdxg_open_state *owner, uint32 adapter,
    struct hvdxg_queryadapter_context *context)
{
    struct hvdxg_object_entry *entry = NULL;

    hvdxg.queryadapter_last_adapter_object = 0;
    hvdxg.queryadapter_last_adapter_object_host = 0;
    hvdxg.queryadapter_last_adapter_object_owner = 0;
    hvdxg.queryadapter_last_adapter_object_owner_generation = 0;
    hvdxg.queryadapter_last_adapter_object_generation = 0;

    if (context != NULL && context->process_adapter != NULL) {
        hvdxg.queryadapter_last_adapter_object = adapter;
        hvdxg.queryadapter_last_adapter_object_host =
            context->process_adapter->host_adapter_handle;
        hvdxg.queryadapter_last_adapter_object_owner =
            context->process != NULL ? context->process->host_process.v : 0;
        hvdxg.queryadapter_last_adapter_object_owner_generation =
            context->process != NULL ? context->process->generation : 0;
        hvdxg.queryadapter_last_adapter_object_generation =
            context->process_adapter->generation;
        return;
    }
    if (owner != NULL)
        entry = hvdxg_owner_find_object(owner, HV_DXG_OBJECT_ADAPTER,
                                        adapter);
    if (entry == NULL)
        return;

    hvdxg.queryadapter_last_adapter_object = (uint32)entry->handle;
    hvdxg.queryadapter_last_adapter_object_host =
        (uint32)entry->host_handle;
    hvdxg.queryadapter_last_adapter_object_owner =
        hvdxg_open_host_process(owner);
    hvdxg.queryadapter_last_adapter_object_owner_generation =
        hvdxg_open_process_generation(owner);
    hvdxg.queryadapter_last_adapter_object_generation = entry->generation;
}

static int hvdxg_untrack_object(struct hvdxg_open_state *owner, uint32 type,
                                uint64 handle)
{
    struct hvdxg_object_entry **objects = hvdxg_owner_object_table(owner);
    uint32 *count = hvdxg_owner_object_count(owner);
    uint32 *alloc_serial = hvdxg_owner_object_alloc_serial(owner);
    uint32 *destroy_serial = hvdxg_owner_object_destroy_serial(owner);
    struct hvdxg_object_entry *entry;
    uint32 index;

    if (owner == NULL || handle == 0 || type == HV_DXG_OBJECT_NONE)
        return 0;
    if (objects == NULL || *objects == NULL || count == NULL ||
        alloc_serial == NULL || destroy_serial == NULL)
        return 0;
    index = hvdxg_hmgr_index(handle);
    if (index >= *count)
        return 0;
    entry = &(*objects)[index];
    if (entry->type != type || entry->handle != handle ||
        entry->destroyed != 0)
        return 0;
    {
        uint32 unique_before = entry->unique;
        uint32 destroyed_before = entry->destroyed;

        entry->destroyed = 1;
        entry->refs = 0;
        (*destroy_serial)++;
        if (*destroy_serial == 0)
            (*destroy_serial)++;
        entry->destroyed_serial = *alloc_serial;
        entry->unique = (entry->unique + 1) & HV_DXG_HMGR_UNIQUE_MASK;
        if (entry->unique == 0)
            entry->unique = 1;
        (void)hvdxg_hmgr_append_free_index(owner, index);
        hvdxg_note_object_type_active(type, -1);
        hvdxg_note_object_lifecycle(type, entry, unique_before,
                                    entry->unique, destroyed_before,
                                    entry->destroyed);
        hvdxg.object_table_free_while_destroyed++;
    }
    hvdxg_note_object_free_list(owner);
    hvdxg_note_object_table_scope(owner);
    return 1;
}

enum {
    HV_DXG_CLEANUP_NONE = 0,
    HV_DXG_CLEANUP_HWQUEUE = 1,
    HV_DXG_CLEANUP_SYNC = 2,
    HV_DXG_CLEANUP_CONTEXT = 3,
    HV_DXG_CLEANUP_GPUVA = 4,
    HV_DXG_CLEANUP_ALLOCATION = 5,
    HV_DXG_CLEANUP_PAGINGQUEUE = 6,
    HV_DXG_CLEANUP_DEVICE = 7,
    HV_DXG_CLEANUP_RESOURCE = 8,
};

static int hvdxg_destroy_device_host(uint32 device);
static int hvdxg_destroy_device_host_process(uint32 process, uint32 device);
static int hvdxg_destroy_allocation_host(uint32 device, uint32 resource,
                                         uint32 allocation, uint32 context);
static int hvdxg_destroy_allocation_host_process(
    uint32 process, uint32 device, uint32 resource,
    uint32 allocation, uint32 context);
static int hvdxg_destroy_context_host_process(uint32 process, uint32 context);
static int hvdxg_destroy_pagingqueue_host_process(uint32 process,
                                                  uint32 paging_queue);
static int hvdxg_destroy_hwqueue_host_process(uint32 process, uint32 hwqueue);
static int hvdxg_flush_device_host(uint32 device);
static void hvdxg_sync_shared_resource_records(
    struct hvdxg_tracked_resource *resource);
static void hvdxg_cleanup_note_ret(int *cleanup_ret, int op_ret,
                                   uint32 op, uint32 handle);
static void hvdxg_cleanup_reset_wsl_order(void);
static void hvdxg_cleanup_mark_wsl_order(uint32 op);
static void hvdxg_cleanup_finalize_wsl_order(void);

static int hvdxg_owner_has_active_process_objects(struct hvdxg_open_state *owner)
{
    struct hvdxg_object_entry *objects;
    uint32 count;

    if (owner == NULL || owner->process_state == NULL)
        return 0;
    objects = owner->process_state->objects;
    count = owner->process_state->object_count;
    if (objects == NULL)
        return 0;
    for (uint32 i = 0; i < count; i++) {
        if (objects[i].type != HV_DXG_OBJECT_NONE &&
            objects[i].destroyed == 0)
            return 1;
    }
    return 0;
}

static void hvdxg_cleanup_process_object_type(struct hvdxg_open_state *owner,
                                              uint32 type, int *ret)
{
    struct hvdxg_object_entry *objects;
    uint32 count;

    if (owner == NULL || owner->process_state == NULL)
        return;
    objects = owner->process_state->objects;
    count = owner->process_state->object_count;
    if (objects == NULL)
        return;

    for (uint32 i = count; i > 0; i--) {
        struct hvdxg_object_entry *entry = &objects[i - 1];
        int op_ret = 0;
        uint32 op = HV_DXG_CLEANUP_NONE;

        if (entry->destroyed != 0 || entry->type != type ||
            entry->handle == 0)
            continue;
        entry->destroyed = 1;
        entry->refs = 0;
        switch (type) {
        case HV_DXG_OBJECT_ADAPTER:
            /* Adapter wrappers are process-local aliases for the host adapter. */
            break;
        case HV_DXG_OBJECT_HWQUEUE:
            /*
             * WSL's process teardown drops HW queue handles locally and
             * leaves final host cleanup to DESTROYDEVICE/DESTROYPROCESS.
             */
            break;
        case HV_DXG_OBJECT_GPUVA:
            /* GPU VA reservations are not explicitly freed on WSL process exit. */
            break;
        case HV_DXG_OBJECT_CONTEXT:
            /* Context host handles are released by device/process teardown. */
            break;
        case HV_DXG_OBJECT_ALLOCATION:
            op = HV_DXG_CLEANUP_ALLOCATION;
            /*
             * WSL's process teardown destroys resource-owned allocations
             * through the resource handle, then frees child allocation handles
             * locally.  Only standalone allocations get their own host destroy.
             */
            if (entry->parent != 0) {
                hvdxg.cleanup_resource_alloc_skips++;
                op_ret = 0;
            } else {
                hvdxg.cleanup_standalone_alloc_destroys++;
                op_ret = hvdxg_destroy_allocation_host(
                    entry->device, 0, (uint32)entry->handle,
                    HV_DXG_DESTROY_ALLOC_CTX_FILE_CLEANUP);
            }
            break;
        case HV_DXG_OBJECT_RESOURCE:
            op = HV_DXG_CLEANUP_RESOURCE;
            hvdxg.cleanup_resource_host_destroys++;
            op_ret = hvdxg_destroy_allocation_host(
                entry->device, (uint32)entry->handle, 0,
                HV_DXG_DESTROY_ALLOC_CTX_FILE_CLEANUP);
            for (uint32 j = 0; j < count; j++) {
                struct hvdxg_object_entry *child = &objects[j];

                if (child->destroyed == 0 &&
                    child->type == HV_DXG_OBJECT_ALLOCATION &&
                    child->parent == entry->handle) {
                    child->destroyed = 1;
                    child->refs = 0;
                    hvdxg.cleanup_resource_child_locals++;
                }
            }
            break;
        case HV_DXG_OBJECT_PAGINGQUEUE:
            /* Paging queues are local-only during WSL process teardown. */
            break;
        case HV_DXG_OBJECT_SYNC:
            /* Sync objects are local-only unless explicitly destroyed by ioctl. */
            break;
        case HV_DXG_OBJECT_DEVICE:
            op = HV_DXG_CLEANUP_DEVICE;
            (void)hvdxg_flush_device_host((uint32)entry->handle);
            op_ret = hvdxg_destroy_device_host((uint32)entry->handle);
            break;
        default:
            break;
        }
        if (op != HV_DXG_CLEANUP_NONE)
            hvdxg_cleanup_note_ret(ret, op_ret, op, (uint32)entry->handle);
    }
}

static void hvdxg_cleanup_process_objects(struct hvdxg_open_state *owner,
                                          int *ret)
{
    if (owner == NULL || owner->process_state == NULL)
        return;

    hvdxg_cleanup_process_object_type(owner, HV_DXG_OBJECT_HWQUEUE, ret);
    hvdxg_cleanup_process_object_type(owner, HV_DXG_OBJECT_GPUVA, ret);
    hvdxg_cleanup_process_object_type(owner, HV_DXG_OBJECT_CONTEXT, ret);
    hvdxg_cleanup_process_object_type(owner, HV_DXG_OBJECT_RESOURCE, ret);
    hvdxg_cleanup_process_object_type(owner, HV_DXG_OBJECT_ALLOCATION, ret);
    hvdxg_cleanup_process_object_type(owner, HV_DXG_OBJECT_PAGINGQUEUE, ret);
    hvdxg_cleanup_process_object_type(owner, HV_DXG_OBJECT_SYNC, ret);
    hvdxg_cleanup_process_object_type(owner, HV_DXG_OBJECT_DEVICE, ret);
    hvdxg_cleanup_process_object_type(owner, HV_DXG_OBJECT_ADAPTER, ret);
}

static uint32 hvdxg_open_host_process(struct hvdxg_open_state *owner)
{
    if (owner != NULL && owner->dxg_process.v != 0)
        return owner->dxg_process.v;
    return hvdxg.dxg_process.v;
}

static struct hvdxg_d3dkmthandle
hvdxg_owner_bound_process_handle(struct hvdxg_open_state *owner)
{
    if (owner != NULL && owner->process_state != NULL &&
        owner->process_state->host_process.v != 0)
        return owner->process_state->host_process;
    if (owner != NULL && owner->dxg_process.v != 0)
        return owner->dxg_process;
    return hvdxg.dxg_process;
}

static uint32 hvdxg_open_process_generation(struct hvdxg_open_state *owner)
{
    if (owner != NULL && owner->process_state != NULL)
        return owner->process_state->generation;
    return hvdxg.dxg_process_generation;
}

static uint32 hvdxg_open_process_refs(struct hvdxg_open_state *owner)
{
    if (owner != NULL && owner->process_state != NULL)
        return owner->process_state->process_refs;
    return 0;
}

static void hvdxg_stop_sync_mapping(struct hvdxg_tracked_sync *sync);

static int hvdxg_track_sync(struct hvdxg_open_state *owner,
                            uint32 device, uint32 sync, uint32 type,
                            uint32 flags, uint32 global_shared,
                            uint64 fence_cpu_va, uint64 fence_kva,
                            uint64 fence_gpu_va,
                            uint32 fence_gpu_va_source,
                            uint64 fence_map_size,
                            uint32 monitor_fence_handle)
{
    int ret;

    if (owner == NULL || sync == 0)
        return -EINVAL;
    ret = hvdxg_track_object(owner, HV_DXG_OBJECT_SYNC, sync, device, device);
    if (ret != 0)
        return ret;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync) {
            if ((owner->sync_objects[i].fence_cpu_va != 0 &&
                 owner->sync_objects[i].fence_cpu_va != fence_cpu_va) ||
                (owner->sync_objects[i].fence_kva != 0 &&
                 owner->sync_objects[i].fence_kva != fence_kva))
                hvdxg_stop_sync_mapping(&owner->sync_objects[i]);
            owner->sync_objects[i].type = type;
            owner->sync_objects[i].device = device;
            owner->sync_objects[i].owner_process =
                hvdxg_open_host_process(owner);
            owner->sync_objects[i].owner_generation =
                hvdxg_open_process_generation(owner);
            owner->sync_objects[i].owner_refs =
                hvdxg_open_process_refs(owner);
            owner->sync_objects[i].flags = flags;
            owner->sync_objects[i].global_shared = global_shared;
            owner->sync_objects[i].monitor_fence_handle =
                monitor_fence_handle ? 1 : 0;
            owner->sync_objects[i].fence_cpu_va = fence_cpu_va;
            owner->sync_objects[i].fence_kva = fence_kva;
            owner->sync_objects[i].fence_gpu_va = fence_gpu_va;
            owner->sync_objects[i].fence_gpu_va_source =
                fence_gpu_va != 0 ? fence_gpu_va_source :
                HV_DXG_SYNC_FENCE_GPUVA_NONE;
            owner->sync_objects[i].fence_map_size = fence_map_size;
            owner->sync_objects[i].fence_cpu_vm =
                fence_cpu_va != 0 && current != NULL ? current->vm : NULL;
            return 0;
        }
    }
    ret = hvdxg_grow_table((void **)&owner->sync_objects,
                           &owner->sync_object_capacity,
                           owner->sync_object_count + 1,
                           sizeof(owner->sync_objects[0]),
                           HV_DXG_OPEN_TRACKED_MAX);
    if (ret != 0) {
        hvdxg_untrack_object(owner, HV_DXG_OBJECT_SYNC, sync);
        return ret;
    }
    uint32 i = owner->sync_object_count++;

    owner->sync_objects[i].sync = sync;
    owner->sync_objects[i].type = type;
    owner->sync_objects[i].device = device;
    owner->sync_objects[i].owner_process =
        hvdxg_open_host_process(owner);
    owner->sync_objects[i].owner_generation =
        hvdxg_open_process_generation(owner);
    owner->sync_objects[i].owner_refs =
        hvdxg_open_process_refs(owner);
    owner->sync_objects[i].flags = flags;
    owner->sync_objects[i].global_shared = global_shared;
    owner->sync_objects[i].monitor_fence_handle =
        monitor_fence_handle ? 1 : 0;
    owner->sync_objects[i].fence_cpu_va = fence_cpu_va;
    owner->sync_objects[i].fence_kva = fence_kva;
    owner->sync_objects[i].fence_gpu_va = fence_gpu_va;
    owner->sync_objects[i].fence_gpu_va_source =
        fence_gpu_va != 0 ? fence_gpu_va_source :
        HV_DXG_SYNC_FENCE_GPUVA_NONE;
    owner->sync_objects[i].fence_map_size = fence_map_size;
    owner->sync_objects[i].fence_cpu_vm =
        fence_cpu_va != 0 && current != NULL ? current->vm : NULL;
    return 0;
}

static void hvdxg_unmap_sync_mapping_raw(uint64 fence_cpu_va,
                                         uint64 fence_kva,
                                         uint64 map_size,
                                         void *fence_cpu_vm)
{
    int user_ret = 0;
    int kernel_ret = 0;

    if (fence_cpu_va == 0 && fence_kva == 0)
        return;
    if (map_size == 0)
        map_size = PGSIZE;
    hvdxg.fence_unmap_attempts++;
    hvdxg.fence_unmap_last_user_va = fence_cpu_va;
    hvdxg.fence_unmap_last_kva = fence_kva;
    hvdxg.fence_unmap_last_size = map_size;
    if (fence_cpu_va != 0) {
        uint64 map_base = PGROUNDDOWN(fence_cpu_va);
        vm_t *vm = (vm_t *)fence_cpu_vm;

        user_ret = vm != NULL ? vm_munmap_region(vm, map_base,
                                                 (size_t)map_size) :
                   -EINVAL;
    }
    if (fence_kva != 0) {
        uint64 map_base = PGROUNDDOWN(fence_kva);

        kernel_ret = kernel_vm != NULL ?
                     vm_munmap_region(kernel_vm, map_base,
                                      (size_t)map_size) :
                     -EINVAL;
    }
    hvdxg.fence_unmap_last_user_ret = user_ret;
    hvdxg.fence_unmap_last_kernel_ret = kernel_ret;
    if (user_ret == 0 && kernel_ret == 0)
        hvdxg.fence_unmap_successes++;
    else
        hvdxg.fence_unmap_failures++;
}

static void hvdxg_stop_sync_mapping(struct hvdxg_tracked_sync *sync)
{
    if (sync == NULL)
        return;
    hvdxg_unmap_sync_mapping_raw(sync->fence_cpu_va, sync->fence_kva,
                                 sync->fence_map_size,
                                 sync->fence_cpu_vm);
    sync->fence_cpu_va = 0;
    sync->fence_kva = 0;
    sync->fence_gpu_va = 0;
    sync->fence_gpu_va_source = HV_DXG_SYNC_FENCE_GPUVA_NONE;
    sync->fence_map_size = 0;
    sync->fence_cpu_vm = NULL;
}

static void hvdxg_untrack_sync(struct hvdxg_open_state *owner, uint32 sync)
{
    if (owner == NULL || sync == 0)
        return;
    hvdxg_untrack_object(owner, HV_DXG_OBJECT_SYNC, sync);
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync) {
            uint32 last = owner->sync_object_count - 1;

            hvdxg_stop_sync_mapping(&owner->sync_objects[i]);
            owner->sync_objects[i] = owner->sync_objects[last];
            memset(&owner->sync_objects[last], 0,
                   sizeof(owner->sync_objects[0]));
            owner->sync_object_count--;
            return;
        }
    }
}

static uint32 hvdxg_owner_sync_type(struct hvdxg_open_state *owner,
                                    uint32 sync)
{
    if (owner == NULL || sync == 0)
        return 0;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync)
            return owner->sync_objects[i].type;
    }
    return 0;
}

static uint32 hvdxg_owner_sync_device(struct hvdxg_open_state *owner,
                                      uint32 sync)
{
    if (owner == NULL || sync == 0)
        return 0;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync)
            return owner->sync_objects[i].device;
    }
    return 0;
}

static uint32 hvdxg_owner_sync_flags(struct hvdxg_open_state *owner,
                                     uint32 sync)
{
    if (owner == NULL || sync == 0)
        return 0;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync)
            return owner->sync_objects[i].flags;
    }
    return 0;
}

static uint32 hvdxg_owner_sync_global_shared(struct hvdxg_open_state *owner,
                                             uint32 sync)
{
    if (owner == NULL || sync == 0)
        return 0;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync)
            return owner->sync_objects[i].global_shared;
    }
    return 0;
}

static uint32 hvdxg_owner_sync_owner_process(struct hvdxg_open_state *owner,
                                             uint32 sync)
{
    if (owner == NULL || sync == 0)
        return 0;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync)
            return owner->sync_objects[i].owner_process;
    }
    return 0;
}

static uint32 hvdxg_owner_sync_owner_generation(struct hvdxg_open_state *owner,
                                                uint32 sync)
{
    if (owner == NULL || sync == 0)
        return 0;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync)
            return owner->sync_objects[i].owner_generation;
    }
    return 0;
}

static uint32 hvdxg_owner_sync_owner_refs(struct hvdxg_open_state *owner,
                                          uint32 sync)
{
    if (owner == NULL || sync == 0)
        return 0;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync)
            return owner->sync_objects[i].owner_refs;
    }
    return 0;
}

static uint64 hvdxg_owner_sync_fence_kva(struct hvdxg_open_state *owner,
                                         uint32 sync)
{
    if (owner == NULL || sync == 0)
        return 0;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync)
            return owner->sync_objects[i].fence_kva;
    }
    return 0;
}

static uint64 hvdxg_owner_sync_fence_cpu_va(struct hvdxg_open_state *owner,
                                            uint32 sync)
{
    if (owner == NULL || sync == 0)
        return 0;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync)
            return owner->sync_objects[i].fence_cpu_va;
    }
    return 0;
}

static uint64 hvdxg_owner_sync_fence_map_size(struct hvdxg_open_state *owner,
                                              uint32 sync)
{
    if (owner == NULL || sync == 0)
        return 0;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync)
            return owner->sync_objects[i].fence_map_size;
    }
    return 0;
}

static uint64 hvdxg_owner_sync_fence_gpu_va(struct hvdxg_open_state *owner,
                                            uint32 sync)
{
    if (owner == NULL || sync == 0)
        return 0;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync)
            return owner->sync_objects[i].fence_gpu_va;
    }
    return 0;
}

static uint32 hvdxg_owner_sync_fence_gpu_va_source(
    struct hvdxg_open_state *owner, uint32 sync)
{
    if (owner == NULL || sync == 0)
        return HV_DXG_SYNC_FENCE_GPUVA_NONE;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync)
            return owner->sync_objects[i].fence_gpu_va_source;
    }
    return HV_DXG_SYNC_FENCE_GPUVA_NONE;
}

static uint64 hvdxg_sync_cpu_fence_value(uint64 fence_kva)
{
    volatile uint64 *current_value;
    uint64 value;

    if (fence_kva == 0)
        return 0;
    current_value = (volatile uint64 *)fence_kva;
    value = *current_value;
    if (value == 0xffffffffffffffffULL) {
        hvdxg.fence_value_max_seen++;
        hvdxg.fence_value_last_kva = fence_kva;
        hvdxg.fence_value_last_current = value;
        if (hvdxg.fence_map_last_kva == fence_kva) {
            hvdxg.fence_map_max_source = hvdxg.fence_map_last_source;
            hvdxg.fence_map_max_mode = hvdxg.fence_map_last_mode;
            hvdxg.fence_map_max_raw_pa = hvdxg.fence_map_last_raw_pa;
            hvdxg.fence_map_max_canonical_pa =
                hvdxg.fence_map_last_canonical_pa;
            hvdxg.fence_map_max_offset_candidate_pa =
                hvdxg.fence_map_last_offset_candidate_pa;
            hvdxg.fence_map_max_offset_candidate_current =
                hvdxg.fence_map_last_offset_candidate_current;
        }
    }
    return value;
}

static uint64 hvdxg_owner_sync_fence_value(struct hvdxg_open_state *owner,
                                           uint32 sync)
{
    return hvdxg_sync_cpu_fence_value(
        hvdxg_owner_sync_fence_kva(owner, sync));
}

static uint32 hvdxg_owner_sync_is_monitor_fence_handle(
    struct hvdxg_open_state *owner, uint32 sync)
{
    if (owner == NULL || sync == 0)
        return 0;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync)
            return owner->sync_objects[i].monitor_fence_handle;
    }
    return 0;
}

static uint32 hvdxg_sync_type_is_monitored(uint32 type)
{
    return type == _D3DDDI_MONITORED_FENCE ||
           type == _D3DDDI_PERIODIC_MONITORED_FENCE;
}

static int hvdxg_sync_cpu_fence_satisfied(uint64 fence_kva,
                                          uint64 fence_value)
{
    uint64 fence_current;

    if (fence_kva == 0)
        return 0;
    fence_current = hvdxg_sync_cpu_fence_value(fence_kva);
    if (fence_current == 0xffffffffffffffffULL) {
        hvdxg.fence_value_last_target = fence_value;
        return 0;
    }
    return fence_current >= fence_value;
}

static int hvdxg_wait_cpu_fences_already_satisfied(
    struct hvdxg_open_state *owner,
    const struct hvdxg_d3dkmthandle *objects,
    const uint64 *fence_values, uint32 object_count, int wait_any)
{
    int satisfied = wait_any ? 0 : 1;

    for (uint32 i = 0; i < object_count; i++) {
        uint64 fence_kva =
            hvdxg_owner_sync_fence_kva(owner, objects[i].v);
        int one_satisfied =
            hvdxg_sync_cpu_fence_satisfied(fence_kva, fence_values[i]);

        if (wait_any && one_satisfied)
            return 1;
        if (!wait_any && !one_satisfied)
            satisfied = 0;
    }
    return satisfied;
}

static int hvdxg_owner_sync_is_monitored(struct hvdxg_open_state *owner,
                                         uint32 sync)
{
    uint32 type = hvdxg_owner_sync_type(owner, sync);

    return type == _D3DDDI_MONITORED_FENCE ||
           type == _D3DDDI_PERIODIC_MONITORED_FENCE;
}

static int hvdxg_track_hwqueue(struct hvdxg_open_state *owner,
                               uint32 context, uint32 device,
                               uint32 queue, uint32 sync_object)
{
    int ret;

    if (owner == NULL || queue == 0)
        return -EINVAL;
    ret = hvdxg_track_object(owner, HV_DXG_OBJECT_HWQUEUE, queue,
                             context, device);
    if (ret != 0)
        return ret;
    for (uint32 i = 0; i < owner->hwqueue_count; i++) {
        if (owner->hwqueues[i].queue == queue) {
            owner->hwqueues[i].context = context;
            owner->hwqueues[i].device = device;
            owner->hwqueues[i].sync_object = sync_object;
            return 0;
        }
    }
    ret = hvdxg_grow_table((void **)&owner->hwqueues,
                           &owner->hwqueue_capacity,
                           owner->hwqueue_count + 1,
                           sizeof(owner->hwqueues[0]),
                           HV_DXG_OPEN_TRACKED_MAX);
    if (ret != 0) {
        hvdxg_untrack_object(owner, HV_DXG_OBJECT_HWQUEUE, queue);
        hvdxg.track_hwqueue_drops++;
        return ret;
    }
    uint32 i = owner->hwqueue_count++;
    owner->hwqueues[i].queue = queue;
    owner->hwqueues[i].context = context;
    owner->hwqueues[i].device = device;
    owner->hwqueues[i].sync_object = sync_object;
    if (owner->hwqueue_count > hvdxg.track_hwqueue_max)
        hvdxg.track_hwqueue_max = owner->hwqueue_count;
    return 0;
}

static uint32 hvdxg_untrack_hwqueue(struct hvdxg_open_state *owner,
                                    uint32 queue)
{
    if (owner == NULL || queue == 0)
        return 0;
    hvdxg_untrack_object(owner, HV_DXG_OBJECT_HWQUEUE, queue);
    for (uint32 i = 0; i < owner->hwqueue_count; i++) {
        if (owner->hwqueues[i].queue == queue) {
            uint32 sync = owner->hwqueues[i].sync_object;
            uint32 last = owner->hwqueue_count - 1;

            owner->hwqueues[i] = owner->hwqueues[last];
            memset(&owner->hwqueues[last], 0,
                   sizeof(owner->hwqueues[0]));
            owner->hwqueue_count--;
            return sync;
        }
    }
    return 0;
}

