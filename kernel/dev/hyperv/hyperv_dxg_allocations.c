/*
 * Hyper-V DXG: queues, allocation tracking, resource linking.
 *
 * Part of the dev/hyperv unity translation unit (included by module.c).
 * Split out of the former hyperv_dxg_objects_shared.c for readability;
 * include order in module.c preserves the original definition order.
 */

static int hvdxg_track_pagingqueue(struct hvdxg_open_state *owner,
                                   uint32 device, uint32 queue,
                                   uint32 sync_object, uint64 fence_pa)
{
    int ret;

    if (owner == NULL || queue == 0)
        return -EINVAL;
    ret = hvdxg_track_object(owner, HV_DXG_OBJECT_PAGINGQUEUE, queue,
                             sync_object, device);
    if (ret != 0)
        return ret;
    for (uint32 i = 0; i < owner->paging_queue_count; i++) {
        if (owner->paging_queues[i].queue == queue) {
            owner->paging_queues[i].device = device;
            owner->paging_queues[i].sync_object = sync_object;
            owner->paging_queues[i].fence_pa = fence_pa;
            return 0;
        }
    }
    ret = hvdxg_grow_table((void **)&owner->paging_queues,
                           &owner->paging_queue_capacity,
                           owner->paging_queue_count + 1,
                           sizeof(owner->paging_queues[0]),
                           HV_DXG_OPEN_TRACKED_MAX);
    if (ret != 0) {
        hvdxg_untrack_object(owner, HV_DXG_OBJECT_PAGINGQUEUE, queue);
        hvdxg.track_pagingqueue_drops++;
        return ret;
    }
    uint32 i = owner->paging_queue_count++;
    owner->paging_queues[i].queue = queue;
    owner->paging_queues[i].device = device;
    owner->paging_queues[i].sync_object = sync_object;
    owner->paging_queues[i].fence_pa = fence_pa;
    if (owner->paging_queue_count > hvdxg.track_pagingqueue_max)
        hvdxg.track_pagingqueue_max = owner->paging_queue_count;
    return 0;
}

static uint32 hvdxg_untrack_pagingqueue(struct hvdxg_open_state *owner,
                                        uint32 queue)
{
    if (owner == NULL || queue == 0)
        return 0;
    hvdxg_untrack_object(owner, HV_DXG_OBJECT_PAGINGQUEUE, queue);
    for (uint32 i = 0; i < owner->paging_queue_count; i++) {
        if (owner->paging_queues[i].queue == queue) {
            uint32 sync = owner->paging_queues[i].sync_object;
            uint32 last = owner->paging_queue_count - 1;

            owner->paging_queues[i] = owner->paging_queues[last];
            memset(&owner->paging_queues[last], 0,
                   sizeof(owner->paging_queues[0]));
            owner->paging_queue_count--;
            return sync;
        }
    }
    return 0;
}

static uint64 hvdxg_owner_pagingqueue_fence_pa(struct hvdxg_open_state *owner,
                                               uint32 queue)
{
    if (owner == NULL || queue == 0)
        return 0;
    for (uint32 i = 0; i < owner->paging_queue_count; i++) {
        if (owner->paging_queues[i].queue == queue)
            return owner->paging_queues[i].fence_pa;
    }
    return 0;
}

static uint32 hvdxg_owner_pagingqueue_sync(struct hvdxg_open_state *owner,
                                           uint32 queue)
{
    if (owner == NULL || queue == 0)
        return 0;
    for (uint32 i = 0; i < owner->paging_queue_count; i++) {
        if (owner->paging_queues[i].queue == queue)
            return owner->paging_queues[i].sync_object;
    }
    return 0;
}

static int hvdxg_owner_has_device(struct hvdxg_open_state *owner,
                                  uint32 device)
{
    return hvdxg_owner_has_object(owner, HV_DXG_OBJECT_DEVICE, device);
}

static int hvdxg_owner_has_pagingqueue(struct hvdxg_open_state *owner,
                                       uint32 paging_queue)
{
    return hvdxg_owner_has_object(owner, HV_DXG_OBJECT_PAGINGQUEUE,
                                  paging_queue);
}

static int hvdxg_owner_has_sync(struct hvdxg_open_state *owner, uint32 sync)
{
    return hvdxg_owner_has_object(owner, HV_DXG_OBJECT_SYNC, sync);
}

static int hvdxg_owner_has_context(struct hvdxg_open_state *owner,
                                   uint32 context)
{
    return hvdxg_owner_has_object(owner, HV_DXG_OBJECT_CONTEXT, context);
}

static int hvdxg_owner_has_hwqueue(struct hvdxg_open_state *owner,
                                   uint32 hwqueue)
{
    return hvdxg_owner_has_object(owner, HV_DXG_OBJECT_HWQUEUE, hwqueue);
}

static uint32 hvdxg_owner_object_device(struct hvdxg_open_state *owner,
                                        uint32 type, uint64 handle)
{
    struct hvdxg_object_entry *entry;

    if (owner == NULL || handle == 0)
        return 0;
    entry = hvdxg_owner_find_object(owner, type, handle);
    return entry != NULL ? entry->device : 0;
}

static int hvdxg_track_allocation(struct hvdxg_open_state *owner,
                                  uint32 device, uint32 resource,
                                  uint32 allocation, uint64 size,
                                  uint32 flags, uint64 sysmem,
                                  uint64 *sysmem_pages,
                                  uint32 sysmem_page_count)
{
    int track_ret;

    if (owner == NULL || device == 0 ||
        (resource == 0 && allocation == 0))
        return -EINVAL;
    if (allocation != 0) {
        track_ret = hvdxg_track_object(owner, HV_DXG_OBJECT_ALLOCATION,
                                       allocation, resource, device);
        if (track_ret != 0)
            return track_ret;
    }
    for (uint32 i = 0; i < owner->allocation_count; i++) {
        if (owner->allocations[i].device == device &&
            owner->allocations[i].resource == resource &&
            owner->allocations[i].allocation == allocation) {
            if (owner->allocations[i].sysmem_pages != sysmem_pages)
                hvdxg_unpin_tracked_allocation(&owner->allocations[i]);
            owner->allocations[i].owner_process =
                hvdxg_open_host_process(owner);
            owner->allocations[i].owner_generation =
                hvdxg_open_process_generation(owner);
            owner->allocations[i].owner_refs =
                hvdxg_open_process_refs(owner);
            hvdxg.allocation_last_owner_process =
                owner->allocations[i].owner_process;
            hvdxg.allocation_last_owner_generation =
                owner->allocations[i].owner_generation;
            owner->allocations[i].size = size;
            owner->allocations[i].flags = flags;
            owner->allocations[i].sysmem = sysmem;
            owner->allocations[i].sysmem_pages = sysmem_pages;
            owner->allocations[i].sysmem_page_count = sysmem_page_count;
            if (sysmem != 0) {
                owner->allocations[i].cpu_va = sysmem;
                owner->allocations[i].map_size = 0;
                owner->allocations[i].cpu_vm = current ? current->vm : NULL;
            } else if (owner->allocations[i].map_size == 0) {
                owner->allocations[i].cpu_va = 0;
                owner->allocations[i].cpu_vm = NULL;
            }
            return 0;
        }
    }
    if (hvdxg_grow_table((void **)&owner->allocations,
                         &owner->allocation_capacity,
                         owner->allocation_count + 1,
                         sizeof(owner->allocations[0]),
                         HV_DXG_OPEN_TRACKED_MAX) == 0) {
        struct hvdxg_tracked_allocation *slot =
            &owner->allocations[owner->allocation_count++];

        memset(slot, 0, sizeof(*slot));
        slot->device = device;
        slot->resource = resource;
        slot->allocation = allocation;
        slot->owner_process = hvdxg_open_host_process(owner);
        slot->owner_generation = hvdxg_open_process_generation(owner);
        slot->owner_refs = hvdxg_open_process_refs(owner);
        hvdxg.allocation_last_owner_process = slot->owner_process;
        hvdxg.allocation_last_owner_generation = slot->owner_generation;
        slot->size = size;
        slot->flags = flags;
        slot->sysmem = sysmem;
        slot->sysmem_pages = sysmem_pages;
        slot->sysmem_page_count = sysmem_page_count;
        if (sysmem != 0) {
            slot->cpu_va = sysmem;
            slot->cpu_vm = current ? current->vm : NULL;
        }
        if (owner->allocation_count > hvdxg.track_allocation_max)
            hvdxg.track_allocation_max = owner->allocation_count;
        return 0;
    } else {
        hvdxg.track_allocation_drops++;
        return -ENOMEM;
    }
}

static struct hvdxg_tracked_resource *
hvdxg_owner_find_resource(struct hvdxg_open_state *owner, uint32 device,
                          uint32 resource);

static struct hvdxg_tracked_allocation *
hvdxg_owner_find_allocation(struct hvdxg_open_state *owner, uint32 device,
	                            uint32 resource, uint32 allocation)
{
    if (owner == NULL || (resource == 0 && allocation == 0))
        return NULL;
    for (uint32 i = 0; i < owner->allocation_count; i++) {
        if ((device == 0 || owner->allocations[i].device == device) &&
            (resource == 0 || owner->allocations[i].resource == resource) &&
            (allocation == 0 ||
             owner->allocations[i].allocation == allocation))
            return &owner->allocations[i];
    }
    return NULL;
}

static void hvdxg_link_resource_allocation(struct hvdxg_open_state *owner,
                                           uint32 device, uint32 resource,
                                           uint32 allocation, uint64 size,
                                           uint32 flags)
{
    struct hvdxg_tracked_resource *r;
    struct hvdxg_tracked_allocation *a;
    uint32 slot = HV_DXG_ALLOCATION_MAX;

    if (owner == NULL || device == 0 || resource == 0 || allocation == 0)
        return;
    r = hvdxg_owner_find_resource(owner, device, resource);
    if (r == NULL)
        return;
    if (r->allocation_count > HV_DXG_ALLOCATION_MAX)
        return;
    if (r->owner_process == 0) {
        r->owner_process = hvdxg_open_host_process(owner);
        r->owner_generation = hvdxg_open_process_generation(owner);
        r->owner_refs = hvdxg_open_process_refs(owner);
    }
    for (uint32 i = 0; i < r->allocation_count; i++) {
        if (r->allocation_handles[i] == allocation) {
            slot = i;
            break;
        }
    }
    if (slot == HV_DXG_ALLOCATION_MAX) {
        if (r->allocation_count >= HV_DXG_ALLOCATION_MAX)
            return;
        slot = r->allocation_count++;
    }
    r->allocation_handles[slot] = allocation;
    r->allocation_sizes[slot] = size;
    r->allocation_num_pages[slot] = hvdxg_allocation_num_pages(size);
    r->allocation_flags[slot] = flags;
    r->allocation_cached[slot] =
        (r->create_flags_value & (1U << 9)) != 0 ? 1 : 0;
    hvdxg_sync_shared_resource_records(r);

    a = hvdxg_owner_find_allocation(owner, device, resource, allocation);
    if (a != NULL && a->owner_process == 0) {
        a->owner_process = r->owner_process;
        a->owner_generation = r->owner_generation;
        a->owner_refs = r->owner_refs;
    }
}

static int hvdxg_owner_has_allocation(struct hvdxg_open_state *owner,
	                                      uint32 device, uint32 resource,
	                                      uint32 allocation)
{
    struct hvdxg_object_entry *obj;

    if (owner == NULL || (resource == 0 && allocation == 0))
        return 0;
    if (allocation != 0) {
        obj = hvdxg_owner_find_object(owner, HV_DXG_OBJECT_ALLOCATION,
                                      allocation);
        if (obj == NULL) {
            hvdxg.object_table_denied++;
            return 0;
        }
        return (device == 0 || obj->device == device) &&
               (resource == 0 || obj->parent == resource);
    }
    for (uint32 i = 0; i < owner->allocation_count; i++) {
        if ((device == 0 || owner->allocations[i].device == device) &&
            (resource == 0 || owner->allocations[i].resource == resource) &&
            (allocation == 0 ||
             owner->allocations[i].allocation == allocation))
            return 1;
    }
    hvdxg.object_table_denied++;
    return 0;
}

static uint64 hvdxg_owner_allocation_size(struct hvdxg_open_state *owner,
                                          uint32 device, uint32 resource,
                                          uint32 allocation)
{
    struct hvdxg_tracked_allocation *a =
        hvdxg_owner_find_allocation(owner, device, resource, allocation);

    return a != NULL ? a->size : 0;
}

static void hvdxg_note_allocation_resident(struct hvdxg_open_state *owner,
                                           uint32 allocation,
                                           uint32 paging_queue,
                                           uint64 fence_value)
{
    struct hvdxg_tracked_allocation *a;

    if (owner == NULL || allocation == 0 || paging_queue == 0 ||
        fence_value == 0)
        return;
    a = hvdxg_owner_find_allocation(owner, 0, 0, allocation);
    if (a == NULL)
        return;
    a->resident_paging_queue = paging_queue;
    a->resident_sync_object =
        hvdxg_owner_pagingqueue_sync(owner, paging_queue);
    a->resident_fence_value = fence_value;
    a->resident_wait_result = 0;
    a->resident_wait_ret = 0;
    a->resident_wait_current =
        hvdxg_owner_sync_fence_value(owner, a->resident_sync_object);
}

static void hvdxg_note_allocation_mapgpuva(struct hvdxg_open_state *owner,
                                           uint32 allocation,
                                           uint32 paging_queue,
                                           uint64 gpu_va, uint64 pages,
                                           uint64 fence_value, int ret,
                                           uint32 status)
{
    struct hvdxg_tracked_allocation *a;

    if (owner == NULL || allocation == 0)
        return;
    a = hvdxg_owner_find_allocation(owner, 0, 0, allocation);
    if (a == NULL)
        return;
    a->map_paging_queue = paging_queue;
    a->map_sync_object = hvdxg_owner_pagingqueue_sync(owner, paging_queue);
    a->map_ret = ret;
    a->map_status = (int32)status;
    a->map_gpu_va = gpu_va;
    a->map_pages = pages;
    a->map_fence_value = fence_value;
}

static void hvdxg_note_allocation_wait(struct hvdxg_open_state *owner,
                                       uint32 sync_object,
                                       uint64 fence_value, int ret,
                                       uint32 result)
{
    if (owner == NULL || sync_object == 0 || fence_value == 0)
        return;
    for (uint32 i = 0; i < owner->allocation_count; i++) {
        struct hvdxg_tracked_allocation *a = &owner->allocations[i];

        if (a->resident_sync_object != sync_object ||
            a->resident_fence_value == 0 ||
            a->resident_fence_value > fence_value)
            continue;
        a->resident_wait_result = result;
        a->resident_wait_ret = ret;
        a->resident_wait_current =
            hvdxg_owner_sync_fence_value(owner, sync_object);
    }
}

static int hvdxg_prepare_allocation_for_share(
    struct hvdxg_open_state *owner, struct hvdxg_tracked_resource *resource,
    struct hvdxg_tracked_allocation *a)
{
    struct d3dkmt_waitforsynchronizationobjectfromcpu wait_req;
    struct hvdxg_d3dkmthandle object;
    uint64 fence_value;
    uint64 event_id;
    uint32 actual_len = 0;
    int ret;

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

    if (owner == NULL || resource == NULL || a == NULL)
        return 0;

    hvdxg.sharedhandle_last_map_va = a->map_gpu_va;
    hvdxg.sharedhandle_last_map_pages = a->map_pages;
    hvdxg.sharedhandle_last_map_fence = a->map_fence_value;
    hvdxg.sharedhandle_last_map_ret = a->map_ret;
    hvdxg.sharedhandle_last_map_status = (uint32)a->map_status;
    hvdxg.sharedhandle_last_resident_paging_queue =
        a->resident_paging_queue;
    hvdxg.sharedhandle_last_resident_sync = a->resident_sync_object;
    hvdxg.sharedhandle_last_resident_fence = a->resident_fence_value;
    hvdxg.sharedhandle_last_resident_wait_result =
        a->resident_wait_result;
    hvdxg.sharedhandle_last_resident_wait_ret = a->resident_wait_ret;
    hvdxg.sharedhandle_last_resident_current =
        hvdxg_owner_sync_fence_value(owner, a->resident_sync_object);
    a->resident_wait_current =
        hvdxg.sharedhandle_last_resident_current;

    if (resource->create_flags_value != 0x47)
        return 0;
    if (a->map_gpu_va == 0 || a->resident_sync_object == 0 ||
        a->resident_fence_value == 0) {
        hvdxg.sharedhandle_last_resident_missing = 1;
        return -EAGAIN;
    }
    if (hvdxg.sharedhandle_last_resident_current !=
            0xffffffffffffffffULL &&
        hvdxg.sharedhandle_last_resident_current >=
            a->resident_fence_value)
        return 0;
    if (a->resident_wait_result != 0 && a->resident_wait_ret == 0)
        return 0;

    event_id = hvdxg_alloc_host_event();
    if (event_id == 0)
        return -ENOMEM;
    memset(&wait_req, 0, sizeof(wait_req));
    wait_req.device.v = resource->device;
    wait_req.object_count = 1;
    object.v = a->resident_sync_object;
    fence_value = a->resident_fence_value;
    ret = hvdxg_send_waitsyncobjectfromcpu(owner, &wait_req, &object,
                                           &fence_value, event_id,
                                           sizeof(object),
                                           sizeof(fence_value),
                                           &actual_len);
    if (ret == 0 || ret == HV_DXG_STATUS_PENDING) {
        ret = hvdxg_wait_host_event_or_cpu_fence(
            owner, &object, &fence_value, 1, 0, event_id,
            HV_DXG_SHARE_RESIDENCY_WAIT_MS);
    }
    hvdxg_remove_host_event(event_id);
    hvdxg.sharedhandle_last_resident_enforced = 1;
    hvdxg.sharedhandle_last_resident_wait_ret = ret;
    hvdxg.sharedhandle_last_resident_current =
        hvdxg_owner_sync_fence_value(owner, a->resident_sync_object);
    if (ret == 0) {
        hvdxg_note_allocation_wait(owner, a->resident_sync_object,
                                   a->resident_fence_value, ret, 1);
        hvdxg.sharedhandle_last_resident_wait_result = 1;
        return 0;
    }
    hvdxg.sharedhandle_last_resident_missing = 2;
    return ret;
}

static int hvdxg_unmap_tracked_allocation(
    struct hvdxg_tracked_allocation *a)
{
    uint64 map_va;

    if (a == NULL || a->cpu_va == 0 || a->map_size == 0)
        return 0;
    if (current == NULL || current->vm == NULL)
        return 0;
    if (a->cpu_vm != NULL && a->cpu_vm != current->vm)
        return 0;
    map_va = PGROUNDDOWN(a->cpu_va);
    if (vm_munmap_region(current->vm, map_va, (size_t)a->map_size) != 0)
        return -EINVAL;
    a->cpu_va = 0;
    a->map_size = 0;
    a->lock_refcount = 0;
    a->cpu_vm = NULL;
    return 0;
}

static void hvdxg_untrack_allocation(struct hvdxg_open_state *owner,
                                     uint32 device, uint32 resource,
                                     uint32 allocation)
{
    if (owner == NULL)
        return;
    for (uint32 i = 0; i < owner->allocation_count; i++) {
        if ((device == 0 || owner->allocations[i].device == device) &&
            (resource == 0 || owner->allocations[i].resource == resource) &&
            (allocation == 0 ||
             owner->allocations[i].allocation == allocation)) {
            if (owner->allocations[i].allocation != 0)
                hvdxg_untrack_object(owner, HV_DXG_OBJECT_ALLOCATION,
                                     owner->allocations[i].allocation);
            (void)hvdxg_unmap_tracked_allocation(&owner->allocations[i]);
            hvdxg_unpin_tracked_allocation(&owner->allocations[i]);
            owner->allocations[i] =
                owner->allocations[owner->allocation_count - 1];
            memset(&owner->allocations[owner->allocation_count - 1], 0,
                   sizeof(owner->allocations[0]));
            owner->allocation_count--;
            i--;
        }
    }
}

static void hvdxg_free_tracked_resource(struct hvdxg_tracked_resource *r)
{
    if (r == NULL)
        return;
    if (r->private_runtime_data != NULL)
        kvfree(r->private_runtime_data);
    if (r->resource_priv_drv_data != NULL)
        kvfree(r->resource_priv_drv_data);
    if (r->total_priv_drv_data != NULL)
        kvfree(r->total_priv_drv_data);
    memset(r, 0, sizeof(*r));
}

static int hvdxg_clone_resource(struct hvdxg_tracked_resource *dst,
                                const struct hvdxg_tracked_resource *src);
static uint32 hvdxg_alloc_shared_parent_id(void);
static void hvdxg_note_shared_parent(struct hvdxg_shared_object *shared);

static struct hvdxg_tracked_resource *
hvdxg_shared_parent_resource(struct hvdxg_shared_object *shared)
{
    if (shared != NULL && shared->resource_parent != NULL)
        return &shared->resource_parent->resource;
    return shared != NULL ? &shared->resource : NULL;
}

static void hvdxg_note_shared_resource_parent(
    struct hvdxg_shared_resource_parent *parent)
{
    if (parent == NULL)
        return;
    hvdxg.sharedresource_parent_last_id = parent->id;
    hvdxg.sharedresource_parent_last_refs = parent->refs;
    hvdxg.sharedresource_parent_last_fd_refs = parent->fd_refs;
    hvdxg.sharedresource_parent_last_children = parent->child_count;
    hvdxg.sharedresource_parent_last_child = parent->last_child_resource;
    hvdxg.sharedresource_parent_last_sealed_gen =
        parent->sealed_generation;
}

static void hvdxg_shared_parent_put(
    struct hvdxg_shared_resource_parent *parent)
{
    if (parent == NULL)
        return;
    hvdxg_note_shared_resource_parent(parent);
    if (parent->refs != 0)
        return;
    hvdxg_free_tracked_resource(&parent->resource);
    kvfree(parent);
}

static struct hvdxg_shared_resource_parent *
hvdxg_shared_parent_create(struct hvdxg_tracked_resource *resource,
                           uint32 cache_process, uint32 cache_object,
                           uint32 global_share, uint32 host_nt_handle)
{
    struct hvdxg_shared_resource_parent *parent;
    int ret;

    if (resource == NULL)
        return NULL;
    parent = kvmalloc(sizeof(*parent));
    if (parent == NULL)
        return NULL;
    memset(parent, 0, sizeof(*parent));
    ret = hvdxg_clone_resource(&parent->resource, resource);
    if (ret != 0) {
        kvfree(parent);
        return NULL;
    }
    parent->id = hvdxg_alloc_shared_parent_id();
    parent->refs = 1;
    parent->fd_refs = 1;
    parent->host_nt_refs = 1;
    parent->cache_process = cache_process;
    parent->cache_object = cache_object;
    parent->global_share = global_share;
    parent->host_nt_handle = host_nt_handle;
    parent->creator_child_resource = resource->resource;
    parent->resource.global_share = global_share;
    parent->resource.host_shared_handle_nt = host_nt_handle;
    parent->resource.host_shared_process = cache_process;
    parent->resource.host_shared_object = cache_object;
    if (parent->resource.host_shared_refs == 0)
        parent->resource.host_shared_refs = 1;
    parent->resource.host_shared_sealed = parent->resource.sealed;
    parent->sealed_generation = parent->resource.sealed_generation;
    parent->allocation_count = parent->resource.allocation_count;
    parent->resource.shared_parent_id = parent->id;
    parent->resource.shared_parent_refs_snapshot = parent->refs;
    hvdxg.sharedresource_parent_publish_count++;
    hvdxg_note_shared_resource_parent(parent);
    return parent;
}

static int hvdxg_shared_parent_add_resource(
    struct hvdxg_shared_resource_parent *parent,
    struct hvdxg_tracked_resource *resource)
{
    struct hvdxg_shared_parent_child *slot = NULL;

    if (parent == NULL || resource == NULL || resource->resource == 0)
        return -EINVAL;
    if (resource->shared_parent == parent)
        return 0;
    if (resource->shared_parent != NULL)
        return -EINVAL;
    for (uint32 i = 0; i < HV_DXG_SHARED_PARENT_CHILD_MAX; i++) {
        struct hvdxg_shared_parent_child *child = &parent->children[i];

        if (child->resource == resource->resource &&
            child->device == resource->device)
            return 0;
        if (slot == NULL && child->resource == 0)
            slot = child;
    }
    if (slot == NULL)
        return -ENOSPC;
    slot->device = resource->device;
    slot->resource = resource->resource;
    slot->owner_process = resource->owner_process;
    slot->owner_generation = resource->owner_generation;
    parent->refs++;
    parent->child_refs++;
    parent->child_count++;
    parent->last_child_device = resource->device;
    parent->last_child_resource = resource->resource;
    parent->sealed_generation = parent->resource.sealed_generation;
    parent->allocation_count = parent->resource.allocation_count;
    if (parent->global_share != 0)
        resource->global_share = parent->global_share;
    if (parent->host_nt_handle != 0) {
        resource->host_shared_handle_nt = parent->host_nt_handle;
        resource->host_shared_process = parent->cache_process;
        resource->host_shared_object = parent->cache_object;
        if (resource->host_shared_refs == 0)
            resource->host_shared_refs = 1;
        resource->host_shared_sealed = resource->sealed;
    }
    resource->shared_parent = parent;
    resource->shared_parent_id = parent->id;
    resource->shared_parent_refs_snapshot = parent->refs;
    hvdxg_note_shared_resource_parent(parent);
    return 0;
}

static void hvdxg_shared_parent_remove_resource(
    struct hvdxg_tracked_resource *resource)
{
    struct hvdxg_shared_resource_parent *parent;
    int removed = 0;

    if (resource == NULL || resource->shared_parent == NULL)
        return;
    parent = resource->shared_parent;
    for (uint32 i = 0; i < HV_DXG_SHARED_PARENT_CHILD_MAX; i++) {
        struct hvdxg_shared_parent_child *child = &parent->children[i];

        if (child->resource == resource->resource &&
            child->device == resource->device) {
            memset(child, 0, sizeof(*child));
            removed = 1;
            break;
        }
    }
    resource->shared_parent = NULL;
    resource->shared_parent_id = 0;
    resource->shared_parent_refs_snapshot = 0;
    if (removed) {
        if (parent->child_count != 0)
            parent->child_count--;
        if (parent->child_refs != 0)
            parent->child_refs--;
        if (parent->refs != 0)
            parent->refs--;
        hvdxg.sharedresource_parent_release_count++;
    }
    hvdxg_note_shared_resource_parent(parent);
    hvdxg_shared_parent_put(parent);
}

static void hvdxg_untrack_resource(struct hvdxg_open_state *owner,
                                   uint32 device, uint32 resource)
{
    if (owner == NULL || resource == 0)
        return;
    hvdxg_untrack_object(owner, HV_DXG_OBJECT_RESOURCE, resource);
    for (uint32 i = 0; i < owner->resource_count; i++) {
        if ((device == 0 || owner->resources[i].device == device) &&
            owner->resources[i].resource == resource) {
            uint32 last = owner->resource_count - 1;

            hvdxg_shared_parent_remove_resource(&owner->resources[i]);
            hvdxg_free_tracked_resource(&owner->resources[i]);
            if (i != last)
                owner->resources[i] = owner->resources[last];
            memset(&owner->resources[last], 0, sizeof(owner->resources[0]));
            owner->resource_count--;
            return;
        }
    }
}

static struct hvdxg_tracked_resource *
hvdxg_owner_find_resource(struct hvdxg_open_state *owner, uint32 device,
                          uint32 resource)
{
    if (owner == NULL || resource == 0)
        return NULL;
    for (uint32 i = 0; i < owner->resource_count; i++) {
        if ((device == 0 || owner->resources[i].device == device) &&
            owner->resources[i].resource == resource)
            return &owner->resources[i];
    }
    return NULL;
}

static void hvdxg_sync_shared_resource_records(
    struct hvdxg_tracked_resource *resource)
{
    if (resource == NULL)
        return;
    resource->shared_records_valid = 1;
    resource->shared_resource_record.create_flags_value =
        resource->create_flags_value;
    resource->shared_resource_record.host_create_flags_value =
        resource->host_create_flags_value;
    resource->shared_resource_record.private_runtime_data_size =
        resource->private_runtime_data_size;
    resource->shared_resource_record.resource_priv_drv_data_size =
        resource->resource_priv_drv_data_size;
    resource->shared_resource_record.total_priv_drv_data_size =
        resource->total_priv_drv_data_size;
    resource->shared_resource_record.sealed_generation =
        resource->sealed_generation;
    resource->shared_resource_record.create_shared =
        resource->create_shared;
    resource->shared_resource_record.nt_security_sharing =
        resource->nt_security_sharing;
    resource->shared_resource_record.sealed = resource->sealed;
    resource->shared_resource_record.opened_from_shared =
        resource->opened_from_shared;
    resource->shared_resource_record.total_priv_from_host =
        resource->total_priv_from_host;
    for (uint32 i = 0; i < HV_DXG_ALLOCATION_MAX; i++) {
        resource->shared_allocation_records[i].allocation =
            resource->allocation_handles[i];
        resource->shared_allocation_records[i].priv_drv_data_size =
            resource->alloc_priv_sizes[i];
        resource->shared_allocation_records[i].size =
            resource->allocation_sizes[i];
        resource->shared_allocation_records[i].num_pages =
            resource->allocation_num_pages[i];
        resource->shared_allocation_records[i].flags =
            resource->allocation_flags[i];
        resource->shared_allocation_records[i].cached =
            resource->allocation_cached[i];
    }
}

static uint32 hvdxg_owner_first_allocation(struct hvdxg_open_state *owner,
                                           uint32 device, uint32 resource,
                                           uint32 *found)
{
    if (found != NULL)
        *found = 0;
    if (owner == NULL || resource == 0)
        return 0;
    for (uint32 i = 0; i < owner->allocation_count; i++) {
        if ((device == 0 || owner->allocations[i].device == device) &&
            owner->allocations[i].resource == resource &&
            owner->allocations[i].allocation != 0) {
            if (found != NULL)
                *found = 1;
            return owner->allocations[i].allocation;
        }
    }
    return 0;
}

static int hvdxg_refresh_resource_allocations(
    struct hvdxg_open_state *owner, struct hvdxg_tracked_resource *resource)
{
    uint32 count = 0;
    uint32 tracked_count = 0;
    uint32 expected_private = 0;

    if (owner == NULL || resource == NULL || resource->resource == 0)
        return -EINVAL;
    hvdxg.sharedresource_seal_last_resource = resource->resource;
    hvdxg.sharedresource_seal_last_generation = resource->sealed_generation;
    hvdxg.sharedresource_seal_missing_alloc = 0;
    hvdxg.sharedresource_seal_extra_alloc = 0;
    hvdxg.sharedresource_seal_tracked_allocs = 0;
    hvdxg.sharedresource_seal_expected_private = 0;
    hvdxg.sharedresource_seal_actual_private =
        resource->total_priv_drv_data_size;
    hvdxg.sharedresource_seal_verify_ret = 0;
    for (uint32 i = 0; i < owner->allocation_count; i++) {
        struct hvdxg_tracked_allocation *a = &owner->allocations[i];
        int listed = 0;

        if (a->device != resource->device ||
            a->resource != resource->resource ||
            a->allocation == 0)
            continue;
        tracked_count++;
        for (uint32 j = 0; j < resource->allocation_count &&
             j < HV_DXG_ALLOCATION_MAX; j++) {
            if (resource->allocation_handles[j] == a->allocation) {
                listed = 1;
                break;
            }
        }
        if (!listed)
            hvdxg.sharedresource_seal_extra_alloc = a->allocation;
    }
    hvdxg.sharedresource_seal_tracked_allocs = tracked_count;
    if (resource->allocation_count == 0 ||
        resource->allocation_count > HV_DXG_ALLOCATION_MAX) {
        hvdxg.sharedresource_seal_verify_ret = -EINVAL;
        return -EINVAL;
    }
    for (uint32 i = 0; i < resource->allocation_count; i++) {
        struct hvdxg_tracked_allocation *a;

        if (resource->allocation_handles[i] == 0) {
            hvdxg.sharedresource_seal_missing_alloc = i + 1;
            hvdxg.sharedresource_seal_verify_ret = -ENOENT;
            return -ENOENT;
        }
        a = hvdxg_owner_find_allocation(owner, resource->device,
                                        resource->resource,
                                        resource->allocation_handles[i]);
        if (a == NULL) {
            hvdxg.sharedresource_seal_missing_alloc =
                resource->allocation_handles[i];
            hvdxg.sharedresource_seal_verify_ret = -ENOENT;
            return -ENOENT;
        }
        if (a->owner_process == 0) {
            a->owner_process = resource->owner_process;
            a->owner_generation = resource->owner_generation;
            a->owner_refs = resource->owner_refs;
        }
        if (resource->owner_process == 0) {
            resource->owner_process = a->owner_process;
            resource->owner_generation = a->owner_generation;
            resource->owner_refs = a->owner_refs;
        }
        resource->allocation_sizes[i] = a->size;
        resource->allocation_num_pages[i] =
            hvdxg_allocation_num_pages(a->size);
        resource->allocation_flags[i] = a->flags;
        resource->allocation_cached[i] =
            (resource->create_flags_value & (1U << 9)) != 0 ? 1 : 0;
        expected_private += resource->alloc_priv_sizes[i];
        if (i == 0) {
            hvdxg.allocation_last_owner_process = a->owner_process;
            hvdxg.allocation_last_owner_generation =
                a->owner_generation;
        }
        count++;
    }
    hvdxg.sharedresource_seal_expected_private = expected_private;
    if (tracked_count != resource->allocation_count &&
        hvdxg.sharedresource_seal_extra_alloc == 0)
        hvdxg.sharedresource_seal_extra_alloc = tracked_count;
    if (count != resource->allocation_count) {
        hvdxg.sharedresource_seal_verify_ret = -ENOENT;
        return -ENOENT;
    }
    if (tracked_count != resource->allocation_count) {
        hvdxg.sharedresource_seal_verify_ret = -EINVAL;
        return -EINVAL;
    }
    if (expected_private != resource->total_priv_drv_data_size) {
        hvdxg.sharedresource_seal_verify_ret = -EINVAL;
        return -EINVAL;
    }
    hvdxg.sharedresource_seal_allocs = count;
    hvdxg.sharedresource_seal_private =
        resource->total_priv_drv_data_size;
    hvdxg.sharedresource_seal_verify_ret = 0;
    hvdxg_sync_shared_resource_records(resource);
    return 0;
}

static int hvdxg_prepare_resource_nt_metadata(
    struct hvdxg_open_state *owner, struct hvdxg_tracked_resource *resource)
{
    struct hvdxg_tracked_allocation *alloc = NULL;
    uint32 allocation;
    uint32 found = 0;

    if (owner == NULL || resource == NULL || resource->resource == 0)
        return -EINVAL;
    if (resource->owner_process == 0) {
        resource->owner_process = hvdxg_open_host_process(owner);
        resource->owner_generation = hvdxg_open_process_generation(owner);
        resource->owner_refs = hvdxg_open_process_refs(owner);
    }
    if (resource->create_shared && resource->nt_security_sharing &&
        !resource->shared_metadata_created) {
        resource->shared_metadata_created = 1;
        resource->host_shared_process = resource->owner_process;
        resource->host_shared_object = resource->resource;
        resource->host_shared_refs = 1;
        hvdxg.sharedresource_created++;
    }
    allocation = resource->allocation_count != 0 ?
                 resource->allocation_handles[0] : 0;
    if (allocation != 0)
        alloc = hvdxg_owner_find_allocation(owner, resource->device,
                                            resource->resource, allocation);
    if (alloc == NULL) {
        allocation = hvdxg_owner_first_allocation(
            owner, resource->device, resource->resource, &found);
        if (allocation != 0)
            alloc = hvdxg_owner_find_allocation(owner, resource->device,
                                                resource->resource,
                                                allocation);
    }
    if (alloc == NULL)
        return 0;
    if (resource->allocation_count == 0) {
        resource->allocation_count = 1;
        resource->allocation_handles[0] = allocation;
    }
    if (resource->allocation_handles[0] == 0)
        resource->allocation_handles[0] = allocation;
    resource->allocation_sizes[0] = alloc->size;
    resource->allocation_num_pages[0] =
        hvdxg_allocation_num_pages(alloc->size);
    resource->allocation_flags[0] = alloc->flags;
    resource->allocation_cached[0] =
        (resource->create_flags_value & (1U << 9)) != 0 ? 1 : 0;
    hvdxg_sync_shared_resource_records(resource);
    if (alloc->owner_process == 0) {
        alloc->owner_process = resource->owner_process;
        alloc->owner_generation = resource->owner_generation;
        alloc->owner_refs = resource->owner_refs;
    }
    if (resource->owner_process == 0) {
        resource->owner_process = alloc->owner_process;
        resource->owner_generation = alloc->owner_generation;
        resource->owner_refs = alloc->owner_refs;
    }
    hvdxg.allocation_last_owner_process = alloc->owner_process;
    hvdxg.allocation_last_owner_generation = alloc->owner_generation;
    hvdxg.sharedhandle_last_allocation = allocation;
    hvdxg.sharedhandle_last_allocation_found = 1;
    hvdxg.sharedhandle_last_allocation_owner_process = alloc->owner_process;
    hvdxg.sharedhandle_last_allocation_owner_generation =
        alloc->owner_generation;
    hvdxg.sharedhandle_last_allocation_owner_refs = alloc->owner_refs;
    return 0;
}

static uint32 hvdxg_existing_sysmem_share_reason(
    const struct hvdxg_tracked_resource *resource)
{
    if (resource == NULL)
        return 1;
    if (!resource->existing_sysmem)
        return 2;
    if (resource->resource == 0)
        return 3;
    if (!resource->create_shared)
        return 4;
    if (!resource->nt_security_sharing)
        return 5;
    if (!resource->shared_metadata_created)
        return 6;
    if (resource->allocation_count == 0)
        return 7;
    if (resource->total_priv_drv_data_size == 0)
        return 8;
    return 0;
}

static void hvdxg_note_existing_sysmem_share(
    const struct hvdxg_tracked_resource *resource, uint32 stage)
{
    uint32 reason;

    hvdxg.existing_sysmem_share_stage = stage;
    hvdxg.existing_sysmem_share_resource =
        resource != NULL ? resource->resource : 0;
    hvdxg.existing_sysmem_share_global =
        resource != NULL ? resource->global_share : 0;
    hvdxg.existing_sysmem_share_flags =
        resource != NULL ? resource->create_flags_value : 0;
    hvdxg.existing_sysmem_share_host_flags =
        resource != NULL ? resource->host_create_flags_value : 0;
    hvdxg.existing_sysmem_share_metadata =
        resource != NULL ? resource->shared_metadata_created : 0;
    hvdxg.existing_sysmem_share_sealed =
        resource != NULL ? resource->sealed : 0;
    hvdxg.existing_sysmem_share_nt =
        resource != NULL ? resource->host_shared_handle_nt : 0;
    hvdxg.existing_sysmem_share_alloc_count =
        resource != NULL ? resource->allocation_count : 0;
    hvdxg.existing_sysmem_share_runtime_priv =
        resource != NULL ? resource->private_runtime_data_size : 0;
    hvdxg.existing_sysmem_share_resource_priv =
        resource != NULL ? resource->resource_priv_drv_data_size : 0;
    hvdxg.existing_sysmem_share_total_priv =
        resource != NULL ? resource->total_priv_drv_data_size : 0;
    hvdxg.existing_sysmem_share_alloc_priv =
        resource != NULL && resource->allocation_count != 0 ?
        resource->alloc_priv_sizes[0] : 0;
    hvdxg.existing_sysmem_share_pfnmap_pages =
        resource != NULL ? resource->existing_sysmem_pfnmap_pages : 0;
    hvdxg.existing_sysmem_share_vram =
        resource != NULL ? resource->existing_sysmem_vram : 0;
    hvdxg.existing_sysmem_share_va =
        resource != NULL ? resource->existing_sysmem_va : 0;
    hvdxg.existing_sysmem_share_size =
        resource != NULL ? resource->existing_sysmem_size : 0;
    reason = hvdxg_existing_sysmem_share_reason(resource);
    hvdxg.existing_sysmem_share_reason = reason;
    hvdxg.existing_sysmem_share_shareable = reason == 0 ? 1 : 0;
}

static void hvdxg_reset_ntshared_runtime_diag(void)
{
    hvdxg.ntshared_runtime_seen = 0;
    hvdxg.ntshared_runtime_user_object = 0;
    hvdxg.ntshared_runtime_user_device = 0;
    hvdxg.ntshared_runtime_kind = 0;
    hvdxg.ntshared_runtime_host_object = 0;
    hvdxg.ntshared_runtime_host_device = 0;
    hvdxg.ntshared_runtime_entry_found = 0;
    hvdxg.ntshared_runtime_entry_exact = 0;
    hvdxg.ntshared_runtime_entry_type = HV_DXG_OBJECT_NONE;
    hvdxg.ntshared_runtime_entry_local = 0;
    hvdxg.ntshared_runtime_entry_host = 0;
    hvdxg.ntshared_runtime_entry_parent = 0;
    hvdxg.ntshared_runtime_entry_device = 0;
    hvdxg.ntshared_runtime_entry_generation = 0;
    hvdxg.ntshared_runtime_entry_destroyed = 0;
    hvdxg.ntshared_runtime_owner_process = 0;
    hvdxg.ntshared_runtime_owner_generation = 0;
    hvdxg.ntshared_runtime_owner_refs = 0;
    hvdxg.ntshared_runtime_resource = 0;
    hvdxg.ntshared_runtime_resource_host = 0;
    hvdxg.ntshared_runtime_resource_flags = 0;
    hvdxg.ntshared_runtime_alloc = 0;
    hvdxg.ntshared_runtime_alloc_host = 0;
    hvdxg.ntshared_runtime_alloc_flags = 0;
    hvdxg.ntshared_runtime_alloc_size = 0;
    hvdxg.ntshared_runtime_alloc_owner_process = 0;
    hvdxg.ntshared_runtime_alloc_owner_generation = 0;
    hvdxg.ntshared_runtime_alloc_owner_refs = 0;
    hvdxg.ntshared_runtime_meta_created = 0;
    hvdxg.ntshared_runtime_sealed_before = 0;
    hvdxg.ntshared_runtime_sealed_after = 0;
    hvdxg.ntshared_runtime_host_sealed_before = 0;
    hvdxg.ntshared_runtime_host_sealed_after = 0;
    hvdxg.ntshared_runtime_cmd_len = 0;
    hvdxg.ntshared_runtime_wire_len = 0;
    hvdxg.ntshared_runtime_ext = 0;
    hvdxg.ntshared_runtime_ext_offset = 0;
    hvdxg.ntshared_runtime_object_offset = 0;
    hvdxg.ntshared_runtime_result_len = 0;
    hvdxg.ntshared_runtime_return_len = 0;
    hvdxg.ntshared_runtime_return_ret = 0;
    hvdxg.ntshared_runtime_return_raw = 0;
    hvdxg.ntshared_runtime_return_handle = 0;
    hvdxg.ntshared_pre_resource_type = HV_DXG_OBJECT_NONE;
    hvdxg.ntshared_pre_resource_local = 0;
    hvdxg.ntshared_pre_resource_host = 0;
    hvdxg.ntshared_pre_resource_generation = 0;
    hvdxg.ntshared_pre_resource_destroyed = 0;
    hvdxg.ntshared_pre_resource_refs = 0;
    hvdxg.ntshared_pre_resource_index = 0;
    hvdxg.ntshared_pre_resource_unique = 0;
    hvdxg.ntshared_pre_resource_instance = 0;
    hvdxg.ntshared_pre_resource_sealed = 0;
    hvdxg.ntshared_pre_resource_open_count = 0;
    hvdxg.ntshared_pre_alloc_type = HV_DXG_OBJECT_NONE;
    hvdxg.ntshared_pre_alloc_local = 0;
    hvdxg.ntshared_pre_alloc_host = 0;
    hvdxg.ntshared_pre_alloc_generation = 0;
    hvdxg.ntshared_pre_alloc_destroyed = 0;
    hvdxg.ntshared_pre_alloc_refs = 0;
    hvdxg.ntshared_pre_alloc_index = 0;
    hvdxg.ntshared_pre_alloc_unique = 0;
    hvdxg.ntshared_pre_alloc_instance = 0;
    hvdxg.ntshared_pre_device_type = HV_DXG_OBJECT_NONE;
    hvdxg.ntshared_pre_device_local = 0;
    hvdxg.ntshared_pre_device_host = 0;
    hvdxg.ntshared_pre_device_generation = 0;
    hvdxg.ntshared_pre_device_destroyed = 0;
    hvdxg.ntshared_pre_device_refs = 0;
    hvdxg.ntshared_pre_device_index = 0;
    hvdxg.ntshared_pre_device_unique = 0;
    hvdxg.ntshared_pre_device_instance = 0;
    hvdxg.ntshared_pre_shared_owner_found = 0;
    hvdxg.ntshared_pre_shared_owner_type = HV_DXG_OBJECT_NONE;
    hvdxg.ntshared_pre_shared_owner_local = 0;
    hvdxg.ntshared_pre_shared_owner_host = 0;
    hvdxg.ntshared_pre_shared_owner_generation = 0;
    hvdxg.ntshared_pre_shared_owner_destroyed = 0;
    hvdxg.ntshared_pre_shared_owner_index = 0;
    hvdxg.ntshared_pre_shared_owner_unique = 0;
    hvdxg.ntshared_pre_shared_owner_instance = 0;
    hvdxg.ntshared_pre_shared_owner_object = 0;
    hvdxg.ntshared_pre_shared_owner_process = 0;
    hvdxg.ntshared_pre_shared_owner_refs = 0;
    hvdxg.ntshared_pre_shared_owner_nt = 0;
    hvdxg.ntshared_pre_shared_owner_sealed = 0;
    hvdxg.ntshared_pre_runtime_size = 0;
    hvdxg.ntshared_pre_runtime_hash = 0;
    memset(hvdxg.ntshared_pre_runtime_w, 0,
           sizeof(hvdxg.ntshared_pre_runtime_w));
    hvdxg.ntshared_pre_resource_priv_size = 0;
    hvdxg.ntshared_pre_resource_priv_hash = 0;
    memset(hvdxg.ntshared_pre_resource_priv_w, 0,
           sizeof(hvdxg.ntshared_pre_resource_priv_w));
    hvdxg.ntshared_pre_total_priv_size = 0;
    hvdxg.ntshared_pre_total_priv_hash = 0;
    memset(hvdxg.ntshared_pre_total_priv_w, 0,
           sizeof(hvdxg.ntshared_pre_total_priv_w));
    hvdxg.ntshared_pre_alloc_out_size = 0;
    hvdxg.ntshared_pre_alloc_out_hash = 0;
    memset(hvdxg.ntshared_pre_alloc_out_w, 0,
           sizeof(hvdxg.ntshared_pre_alloc_out_w));
    hvdxg.ntshared_pre_create_seq = 0;
    hvdxg.ntshared_pre_first_nt_seq = 0;
    hvdxg.ntshared_pre_event_seq = 0;
    hvdxg.ntshared_model_process_state = 0;
    hvdxg.ntshared_model_open_process = 0;
    hvdxg.ntshared_model_open_generation = 0;
    hvdxg.ntshared_model_open_refs = 0;
    hvdxg.ntshared_model_global_process = 0;
    hvdxg.ntshared_model_command_process = 0;
    hvdxg.ntshared_model_resource_owner = 0;
    hvdxg.ntshared_model_resource_generation = 0;
    hvdxg.ntshared_model_alloc_owner = 0;
    hvdxg.ntshared_model_alloc_generation = 0;
    hvdxg.ntshared_model_cmd_eq_open = 0;
    hvdxg.ntshared_model_cmd_eq_global = 0;
    hvdxg.ntshared_model_cmd_eq_resource = 0;
    hvdxg.ntshared_model_cmd_eq_alloc = 0;
    hvdxg.ntshared_model_open_objects = 0;
    hvdxg.ntshared_model_process_objects = 0;
    hvdxg.ntshared_model_resources = 0;
    hvdxg.ntshared_model_allocations = 0;
    hvdxg.ntshared_model_devices = 0;
    hvdxg.ntshared_model_contexts = 0;
    hvdxg.ntshared_model_process_live = 0;
    hvdxg.ntshared_model_process_generation = 0;
    hvdxg.ntshared_cleanup_ret = 0;
    hvdxg.ntshared_cleanup_cached_before = 0;
    hvdxg.ntshared_cleanup_cached_after = 0;
    hvdxg.ntshared_cleanup_refs_before = 0;
    hvdxg.ntshared_cleanup_refs_after = 0;
    hvdxg.ntshared_cleanup_object_before = 0;
    hvdxg.ntshared_cleanup_object_after = 0;
    hvdxg.ntshared_cleanup_nt_before = 0;
    hvdxg.ntshared_cleanup_nt_after = 0;
    hvdxg.ntshared_cleanup_sealed_before = 0;
    hvdxg.ntshared_cleanup_sealed_after = 0;
    hvdxg.ntshared_cleanup_cache_inserts_before = 0;
    hvdxg.ntshared_cleanup_cache_inserts_after = 0;
}

static void hvdxg_note_ntshared_runtime_resource(
    struct hvdxg_open_state *owner, uint32 user_object, uint32 process,
    uint32 host_object, struct hvdxg_tracked_resource *resource,
    struct hvdxg_tracked_allocation *allocation,
    struct hvdxg_object_entry *resource_entry,
    struct hvdxg_object_entry *allocation_entry)
{
    struct hvdxg_object_entry *entry;
    struct hvdxg_object_entry *device_entry = NULL;
    struct hvdxg_object_entry *shared_owner_entry = NULL;

    hvdxg.ntshared_runtime_seen = 1;
    hvdxg.ntshared_runtime_user_object = user_object;
    hvdxg.ntshared_runtime_kind = HV_DXG_SHARED_OBJECT_RESOURCE;
    hvdxg.ntshared_runtime_host_object = host_object;
    hvdxg.ntshared_runtime_owner_process = process;
    if (resource != NULL) {
        hvdxg.ntshared_runtime_user_device = resource->device;
        hvdxg.ntshared_runtime_host_device =
            hvdxg.sharedhandle_last_host_device;
        hvdxg.ntshared_runtime_owner_process = resource->owner_process;
        hvdxg.ntshared_runtime_owner_generation =
            resource->owner_generation;
        hvdxg.ntshared_runtime_owner_refs = resource->owner_refs;
        hvdxg.ntshared_runtime_resource = resource->resource;
        hvdxg.ntshared_runtime_resource_host =
            resource_entry != NULL ?
            (uint32)resource_entry->host_handle : host_object;
        hvdxg.ntshared_runtime_resource_flags =
            resource->host_create_flags_value != 0 ?
            resource->host_create_flags_value : resource->create_flags_value;
        hvdxg.ntshared_runtime_alloc =
            hvdxg.sharedhandle_last_allocation;
        hvdxg.ntshared_runtime_alloc_flags =
            allocation != NULL ? allocation->flags :
            (resource->allocation_count != 0 ?
             resource->allocation_flags[0] : 0);
        hvdxg.ntshared_runtime_alloc_size =
            allocation != NULL ? allocation->size :
            (resource->allocation_count != 0 ?
             resource->allocation_sizes[0] : 0);
        hvdxg.ntshared_runtime_alloc_owner_process =
            allocation != NULL ? allocation->owner_process : 0;
        hvdxg.ntshared_runtime_alloc_owner_generation =
            allocation != NULL ? allocation->owner_generation : 0;
        hvdxg.ntshared_runtime_alloc_owner_refs =
            allocation != NULL ? allocation->owner_refs : 0;
        hvdxg.ntshared_runtime_meta_created =
            resource->shared_metadata_created;
        hvdxg.ntshared_runtime_sealed_before = resource->sealed;
        hvdxg.ntshared_runtime_sealed_after = resource->sealed;
        hvdxg.ntshared_runtime_host_sealed_before =
            resource->host_shared_sealed;
        hvdxg.ntshared_runtime_host_sealed_after =
            resource->host_shared_sealed;
        hvdxg.ntshared_pre_resource_type =
            resource_entry != NULL ? resource_entry->type :
            HV_DXG_OBJECT_NONE;
        hvdxg.ntshared_pre_resource_local =
            resource_entry != NULL ? (uint32)resource_entry->handle :
            resource->resource;
        hvdxg.ntshared_pre_resource_host =
            resource_entry != NULL ? (uint32)resource_entry->host_handle :
            host_object;
        hvdxg.ntshared_pre_resource_generation =
            resource_entry != NULL ? resource_entry->generation : 0;
        hvdxg.ntshared_pre_resource_destroyed =
            resource_entry != NULL ? resource_entry->destroyed : 0;
        hvdxg.ntshared_pre_resource_refs =
            resource_entry != NULL ? resource_entry->refs : 0;
        hvdxg.ntshared_pre_resource_index =
            resource_entry != NULL ? resource_entry->index : 0;
        hvdxg.ntshared_pre_resource_unique =
            resource_entry != NULL ? resource_entry->unique : 0;
        hvdxg.ntshared_pre_resource_instance =
            resource_entry != NULL ? resource_entry->instance : 0;
        hvdxg.ntshared_pre_resource_sealed = resource->sealed;
        hvdxg.ntshared_pre_resource_open_count = resource->open_count;
        hvdxg.ntshared_pre_alloc_type =
            allocation_entry != NULL ? allocation_entry->type :
            HV_DXG_OBJECT_NONE;
        hvdxg.ntshared_pre_alloc_local =
            allocation_entry != NULL ? (uint32)allocation_entry->handle :
            hvdxg.ntshared_runtime_alloc;
        hvdxg.ntshared_pre_alloc_host =
            allocation_entry != NULL ? (uint32)allocation_entry->host_handle :
            hvdxg.ntshared_runtime_alloc;
        hvdxg.ntshared_pre_alloc_generation =
            allocation_entry != NULL ? allocation_entry->generation : 0;
        hvdxg.ntshared_pre_alloc_destroyed =
            allocation_entry != NULL ? allocation_entry->destroyed : 0;
        hvdxg.ntshared_pre_alloc_refs =
            allocation_entry != NULL ? allocation_entry->refs : 0;
        hvdxg.ntshared_pre_alloc_index =
            allocation_entry != NULL ? allocation_entry->index : 0;
        hvdxg.ntshared_pre_alloc_unique =
            allocation_entry != NULL ? allocation_entry->unique : 0;
        hvdxg.ntshared_pre_alloc_instance =
            allocation_entry != NULL ? allocation_entry->instance : 0;
        device_entry = hvdxg_owner_find_object(
            owner, HV_DXG_OBJECT_DEVICE, resource->device);
        hvdxg.ntshared_pre_device_type =
            device_entry != NULL ? device_entry->type : HV_DXG_OBJECT_NONE;
        hvdxg.ntshared_pre_device_local =
            device_entry != NULL ? (uint32)device_entry->handle :
            resource->device;
        hvdxg.ntshared_pre_device_host =
            device_entry != NULL ? (uint32)device_entry->host_handle :
            hvdxg.sharedhandle_last_host_device;
        hvdxg.ntshared_pre_device_generation =
            device_entry != NULL ? device_entry->generation : 0;
        hvdxg.ntshared_pre_device_destroyed =
            device_entry != NULL ? device_entry->destroyed : 0;
        hvdxg.ntshared_pre_device_refs =
            device_entry != NULL ? device_entry->refs : 0;
        hvdxg.ntshared_pre_device_index =
            device_entry != NULL ? device_entry->index : 0;
        hvdxg.ntshared_pre_device_unique =
            device_entry != NULL ? device_entry->unique : 0;
        hvdxg.ntshared_pre_device_instance =
            device_entry != NULL ? device_entry->instance : 0;
        hvdxg.ntshared_pre_shared_owner_object =
            resource->host_shared_object;
        hvdxg.ntshared_pre_shared_owner_process =
            resource->host_shared_process;
        hvdxg.ntshared_pre_shared_owner_refs = resource->host_shared_refs;
        hvdxg.ntshared_pre_shared_owner_nt =
            resource->host_shared_handle_nt;
        hvdxg.ntshared_pre_shared_owner_sealed =
            resource->host_shared_sealed;
        shared_owner_entry = hvdxg_owner_find_object_any(
            owner, resource->host_shared_object, 1);
        if (shared_owner_entry != NULL) {
            hvdxg.ntshared_pre_shared_owner_found = 1;
            hvdxg.ntshared_pre_shared_owner_type =
                shared_owner_entry->type;
            hvdxg.ntshared_pre_shared_owner_local =
                (uint32)shared_owner_entry->handle;
            hvdxg.ntshared_pre_shared_owner_host =
                (uint32)shared_owner_entry->host_handle;
            hvdxg.ntshared_pre_shared_owner_generation =
                shared_owner_entry->generation;
            hvdxg.ntshared_pre_shared_owner_destroyed =
                shared_owner_entry->destroyed;
            hvdxg.ntshared_pre_shared_owner_index =
                shared_owner_entry->index;
            hvdxg.ntshared_pre_shared_owner_unique =
                shared_owner_entry->unique;
            hvdxg.ntshared_pre_shared_owner_instance =
                shared_owner_entry->instance;
        }
        hvdxg.ntshared_pre_runtime_size =
            resource->private_runtime_data_size;
        hvdxg.ntshared_pre_runtime_hash =
            hvdxg_hash_bytes(resource->private_runtime_data,
                             resource->private_runtime_data_size);
        hvdxg.ntshared_pre_resource_priv_size =
            resource->resource_priv_drv_data_size;
        hvdxg.ntshared_pre_resource_priv_hash =
            hvdxg_hash_bytes(resource->resource_priv_drv_data,
                             resource->resource_priv_drv_data_size);
        hvdxg.ntshared_pre_total_priv_size =
            resource->total_priv_drv_data_size;
        hvdxg.ntshared_pre_total_priv_hash =
            hvdxg_hash_bytes(resource->total_priv_drv_data,
                             resource->total_priv_drv_data_size);
        hvdxg.ntshared_pre_alloc_out_size =
            hvdxg.d3d12_shared_alloc_out_priv_head_len;
        hvdxg.ntshared_pre_alloc_out_hash =
            hvdxg_hash_bytes(hvdxg.d3d12_shared_alloc_out_priv_head,
                             hvdxg.d3d12_shared_alloc_out_priv_head_len);
        for (uint32 i = 0; i < 4; i++) {
            uint32 off = i * sizeof(uint32);

            hvdxg.ntshared_pre_runtime_w[i] =
                hvdxg_read_u32_at(resource->private_runtime_data,
                                  resource->private_runtime_data_size, off);
            hvdxg.ntshared_pre_resource_priv_w[i] =
                hvdxg_read_u32_at(resource->resource_priv_drv_data,
                                  resource->resource_priv_drv_data_size, off);
            hvdxg.ntshared_pre_total_priv_w[i] =
                hvdxg_read_u32_at(resource->total_priv_drv_data,
                                  resource->total_priv_drv_data_size, off);
            hvdxg.ntshared_pre_alloc_out_w[i] =
                hvdxg_read_u32_at(hvdxg.d3d12_shared_alloc_out_priv_head,
                                  hvdxg.d3d12_shared_alloc_out_priv_head_len,
                                  off);
        }
        hvdxg.ntshared_pre_create_seq = hvdxg.d3d12_shared_create_seq;
        hvdxg.ntshared_pre_first_nt_seq = hvdxg.d3d12_shared_first_nt_seq;
        hvdxg.ntshared_pre_event_seq = hvdxg.d3d12_shared_event_seq;
        hvdxg.ntshared_model_process_state =
            owner != NULL && owner->process_state != NULL ? 1U : 0U;
        hvdxg.ntshared_model_open_process =
            hvdxg_open_host_process(owner);
        hvdxg.ntshared_model_open_generation =
            hvdxg_open_process_generation(owner);
        hvdxg.ntshared_model_open_refs = hvdxg_open_process_refs(owner);
        hvdxg.ntshared_model_global_process = hvdxg.dxg_process.v;
        hvdxg.ntshared_model_command_process = process;
        hvdxg.ntshared_model_resource_owner = resource->owner_process;
        hvdxg.ntshared_model_resource_generation =
            resource->owner_generation;
        hvdxg.ntshared_model_alloc_owner =
            allocation != NULL ? allocation->owner_process : 0;
        hvdxg.ntshared_model_alloc_generation =
            allocation != NULL ? allocation->owner_generation : 0;
        hvdxg.ntshared_model_cmd_eq_open =
            process != 0 && process == hvdxg.ntshared_model_open_process;
        hvdxg.ntshared_model_cmd_eq_global =
            process != 0 && process == hvdxg.dxg_process.v;
        hvdxg.ntshared_model_cmd_eq_resource =
            process != 0 && process == resource->owner_process;
        hvdxg.ntshared_model_cmd_eq_alloc =
            allocation != NULL && process != 0 &&
            process == allocation->owner_process;
        hvdxg.ntshared_model_open_objects =
            owner != NULL ? owner->object_count : 0;
        hvdxg.ntshared_model_process_objects =
            owner != NULL && owner->process_state != NULL ?
            owner->process_state->object_count : 0;
        hvdxg.ntshared_model_resources =
            owner != NULL ? owner->resource_count : 0;
        hvdxg.ntshared_model_allocations =
            owner != NULL ? owner->allocation_count : 0;
        hvdxg.ntshared_model_devices =
            owner != NULL ? owner->device_count : 0;
        hvdxg.ntshared_model_contexts =
            owner != NULL ? owner->context_count : 0;
        hvdxg.ntshared_model_process_live = hvdxg.process_live;
        hvdxg.ntshared_model_process_generation = hvdxg.process_generation;
    }
    hvdxg.ntshared_runtime_alloc_host =
        allocation_entry != NULL ? (uint32)allocation_entry->host_handle :
        hvdxg.ntshared_runtime_alloc;

    entry = hvdxg_owner_find_object_any(owner, user_object, 1);
    if (entry == NULL)
        return;
    hvdxg.ntshared_runtime_entry_found = 1;
    hvdxg.ntshared_runtime_entry_exact =
        entry->handle == user_object ? 1 : 0;
    hvdxg.ntshared_runtime_entry_type = entry->type;
    hvdxg.ntshared_runtime_entry_local = (uint32)entry->handle;
    hvdxg.ntshared_runtime_entry_host = (uint32)entry->host_handle;
    hvdxg.ntshared_runtime_entry_parent = (uint32)entry->parent;
    hvdxg.ntshared_runtime_entry_device = entry->device;
    hvdxg.ntshared_runtime_entry_generation = entry->generation;
    hvdxg.ntshared_runtime_entry_destroyed = entry->destroyed;
}

static void hvdxg_note_ntshared_runtime_return(
    struct hvdxg_tracked_resource *resource, int ret, uint32 handle)
{
    hvdxg.ntshared_runtime_cmd_len =
        hvdxg.ntshared_last_create_cmd_len;
    hvdxg.ntshared_runtime_wire_len =
        hvdxg.ntshared_create_attempt_wire_len[0];
    hvdxg.ntshared_runtime_ext = hvdxg.ntshared_create_attempt_ext[0];
    hvdxg.ntshared_runtime_ext_offset =
        hvdxg.ntshared_create_attempt_ext_offset[0];
    hvdxg.ntshared_runtime_object_offset =
        hvdxg.ntshared_last_create_object_offset;
    hvdxg.ntshared_runtime_result_len =
        hvdxg.ntshared_last_create_result_len;
    hvdxg.ntshared_runtime_return_len = hvdxg.ntshared_last_create_len;
    hvdxg.ntshared_runtime_return_ret = ret;
    hvdxg.ntshared_runtime_return_raw = hvdxg.ntshared_last_create_raw0;
    hvdxg.ntshared_runtime_return_handle = handle;
    if (resource != NULL) {
        hvdxg.ntshared_runtime_meta_created =
            resource->shared_metadata_created;
        hvdxg.ntshared_runtime_sealed_after = resource->sealed;
        hvdxg.ntshared_runtime_host_sealed_after =
            resource->host_shared_sealed;
    }
}

static int hvdxg_seal_resource(struct hvdxg_tracked_resource *resource)
{
    if (resource == NULL)
        return -EINVAL;
    if (resource->sealed) {
        hvdxg.sharedresource_seal_reuses++;
        return 0;
    }
    if (!resource->create_shared || !resource->nt_security_sharing ||
        !resource->shared_metadata_created ||
        resource->allocation_count == 0 ||
        resource->allocation_count > HV_DXG_ALLOCATION_MAX ||
        resource->total_priv_drv_data_size == 0) {
        hvdxg.sharedresource_seal_denied++;
        return -EINVAL;
    }
    resource->sealed = 1;
    resource->sealed_generation = ++hvdxg.sharedresource_seals;
    if (resource->open_count == 0)
        resource->open_count = 1;
    hvdxg_sync_shared_resource_records(resource);
    return 0;
}

static int hvdxg_seal_resource_from_owner(struct hvdxg_open_state *owner,
                                          struct hvdxg_tracked_resource *resource)
{
    int ret;

    if (resource == NULL)
        return -EINVAL;
    if (!resource->sealed) {
        ret = hvdxg_refresh_resource_allocations(owner, resource);
        if (ret != 0) {
            hvdxg.sharedresource_seal_denied++;
            return ret;
        }
    }
    return hvdxg_seal_resource(resource);
}

static int hvdxg_copy_user_bytes(uint8 **dst, uint64 src, uint32 size)
{
    *dst = NULL;
    if (size == 0)
        return 0;
    if (src == 0)
        return -EINVAL;
    *dst = kvmalloc(size);
    if (*dst == NULL)
        return -ENOMEM;
    if (either_copyin(*dst, 1, src, size) < 0) {
        kvfree(*dst);
        *dst = NULL;
        return -EFAULT;
    }
    return 0;
}

static int hvdxg_copy_kernel_bytes(uint8 **dst, const uint8 *src, uint32 size)
{
    *dst = NULL;
    if (size == 0)
        return 0;
    if (src == NULL)
        return -EINVAL;
    *dst = kvmalloc(size);
    if (*dst == NULL)
        return -ENOMEM;
    memmove(*dst, src, size);
    return 0;
}

static int hvdxg_get_standard_alloc_priv_data(
    uint32 device, const struct d3dkmt_createstandardallocation *standard_alloc,
    uint32 *alloc_priv_size, uint8 **alloc_priv_data,
    uint32 *res_priv_size, uint8 **res_priv_data)
{
    struct hvdxg_command_getstandardallocprivdata query;
    struct hvdxg_command_getstandardallocprivdata_return *result = NULL;
    uint32 result_len = sizeof(*result);
    uint32 actual_len = 0;
    uint32 alloc_size = 0;
    uint32 res_size = 0;
    int ret;

    if (alloc_priv_size != NULL)
        *alloc_priv_size = 0;
    if (alloc_priv_data != NULL)
        *alloc_priv_data = NULL;
    if (res_priv_size != NULL)
        *res_priv_size = 0;
    if (res_priv_data != NULL)
        *res_priv_data = NULL;
    if (device == 0 || standard_alloc == NULL || alloc_priv_size == NULL ||
        alloc_priv_data == NULL || res_priv_size == NULL ||
        res_priv_data == NULL)
        return -EINVAL;

    memset(&query, 0, sizeof(query));
    hvdxg_command_vgpu_init_process(
        &query.hdr, HV_DXGK_VMBCOMMAND_DDIGETSTANDARDALLOCATIONDRIVERDATA,
        hvdxg.dxg_process);
    query.alloc_type = _D3DKMDT_STANDARDALLOCATION_GDISURFACE;
    query.gdi_surface.type = _D3DKMDT_GDISURFACE_TEXTURE_CROSSADAPTER;
    query.gdi_surface.width =
        (uint32)standard_alloc->existing_heap_data.size;
    query.gdi_surface.height = 1;
    query.gdi_surface.format = _D3DDDIFMT_UNKNOWN;

    hvdxg.stdalloc_last_len = 0;
    hvdxg.stdalloc_last_ret = 0;
    hvdxg.stdalloc_last_status = 0;
    hvdxg.stdalloc_last_alloc_size = 0;
    hvdxg.stdalloc_last_res_size = 0;
    result = kvmalloc(result_len);
    if (result == NULL)
        return -ENOMEM;
    memset(result, 0, result_len);

    ret = hvdxg_send_sync_vgpu(&query, sizeof(query), result, result_len,
                               &actual_len);
    hvdxg.stdalloc_last_len = actual_len;
    hvdxg.stdalloc_last_ret = ret;
    if (ret != 0)
        goto done;
    if (actual_len < sizeof(*result)) {
        ret = -EOVERFLOW;
        goto done;
    }
    hvdxg.stdalloc_last_status = result->status.v;
    ret = hvdxg_ntstatus_to_errno(result->status);
    if (ret != 0)
        goto done;
    alloc_size = result->priv_driver_data_size;
    res_size = result->priv_driver_resource_size;
    hvdxg.stdalloc_last_alloc_size = alloc_size;
    hvdxg.stdalloc_last_res_size = res_size;
    if (alloc_size == 0 ||
        alloc_size > HV_DXG_VM_BUS_PACKET_MAX ||
        res_size > HV_DXG_VM_BUS_PACKET_MAX) {
        ret = -EINVAL;
        goto done;
    }
    kvfree(result);
    result_len = sizeof(*result) + alloc_size + res_size;
    if (result_len > HV_DXG_VM_BUS_PACKET_MAX) {
        ret = -EOVERFLOW;
        result = NULL;
        goto done;
    }
    result = kvmalloc(result_len);
    if (result == NULL) {
        ret = -ENOMEM;
        goto done;
    }
    memset(result, 0, result_len);
    query.priv_driver_data_size = alloc_size;
    query.priv_driver_resource_size = res_size;
    actual_len = 0;
    ret = hvdxg_send_sync_vgpu(&query, sizeof(query), result, result_len,
                               &actual_len);
    hvdxg.stdalloc_last_len = actual_len;
    hvdxg.stdalloc_last_ret = ret;
    if (ret != 0)
        goto done;
    if (actual_len < sizeof(*result) + alloc_size + res_size) {
        ret = -EOVERFLOW;
        goto done;
    }
    hvdxg.stdalloc_last_status = result->status.v;
    ret = hvdxg_ntstatus_to_errno(result->status);
    if (ret != 0)
        goto done;
    if (result->priv_driver_data_size != alloc_size ||
        result->priv_driver_resource_size != res_size) {
        ret = -EOVERFLOW;
        goto done;
    }
    ret = hvdxg_copy_kernel_bytes(alloc_priv_data,
                                  (const uint8 *)&result[1],
                                  alloc_size);
    if (ret != 0)
        goto done;
    ret = hvdxg_copy_kernel_bytes(res_priv_data,
                                  (const uint8 *)&result[1] + alloc_size,
                                  res_size);
    if (ret != 0) {
        if (*alloc_priv_data != NULL) {
            kvfree(*alloc_priv_data);
            *alloc_priv_data = NULL;
        }
        goto done;
    }
    *alloc_priv_size = alloc_size;
    *res_priv_size = res_size;

done:
    hvdxg.stdalloc_last_ret = ret;
    if (result != NULL)
        kvfree(result);
    return ret;
}

static int hvdxg_track_resource(struct hvdxg_open_state *owner,
                                struct d3dkmt_createallocation *req,
                                struct d3dkmt_createallocationflags requested_flags,
                                struct d3dddi_allocationinfo2 *alloc_info,
                                struct hvdxg_command_createallocation_return *result,
                                const uint8 *alloc_private_data,
                                const uint32 *alloc_private_sizes,
                                uint32 total_alloc_private,
                                uint32 alloc_private_from_host,
                                const uint8 *runtime_private_data,
                                const uint8 *resource_priv_data,
                                uint32 resource_priv_data_size)
{
    struct hvdxg_tracked_resource tmp;
    struct hvdxg_tracked_resource *slot;
    int ret;

    if (owner == NULL || req == NULL || result == NULL ||
        req->resource.v == 0 || !req->flags.create_resource)
        return 0;
    if (req->alloc_count == 0 || req->alloc_count > HV_DXG_ALLOCATION_MAX)
        return -EINVAL;

    memset(&tmp, 0, sizeof(tmp));
    tmp.device = req->device.v;
    tmp.resource = req->resource.v;
    tmp.global_share = req->global_share.v != 0 ?
                       req->global_share.v : result->global_share.v;
    tmp.owner_process = hvdxg_open_host_process(owner);
    tmp.owner_generation = hvdxg_open_process_generation(owner);
    tmp.owner_refs = hvdxg_open_process_refs(owner);
    tmp.allocation_count = req->alloc_count;
    tmp.create_flags_value = requested_flags.value;
    tmp.host_create_flags_value = result->flags.value;
    tmp.create_shared = requested_flags.create_shared;
    tmp.nt_security_sharing = requested_flags.nt_security_sharing;
    tmp.shared_metadata_created =
        tmp.create_shared && tmp.nt_security_sharing ? 1 : 0;
    if (tmp.shared_metadata_created) {
        tmp.host_shared_process = tmp.owner_process;
        tmp.host_shared_object = tmp.resource;
        tmp.host_shared_refs = 1;
        tmp.host_shared_sealed = 0;
    }
    if (requested_flags.value == 0x47) {
        hvdxg.d3d12_shared_track_shared = tmp.create_shared;
        hvdxg.d3d12_shared_track_nt = tmp.nt_security_sharing;
        hvdxg.d3d12_shared_track_metadata = tmp.shared_metadata_created;
        hvdxg.d3d12_shared_track_sent_bytes =
            runtime_private_data != NULL ? 1 : 0;
        hvdxg.d3d12_shared_track_alloc_from_host =
            alloc_private_from_host;
    }
    tmp.open_count = 1;
    tmp.private_runtime_data_size = req->private_runtime_data_size;
    tmp.resource_priv_drv_data_size = resource_priv_data_size;
    tmp.total_priv_drv_data_size = total_alloc_private;
    tmp.total_priv_from_host = alloc_private_from_host ? 1 : 0;
    for (uint32 i = 0; i < req->alloc_count; i++) {
        tmp.allocation_handles[i] = alloc_info[i].allocation.v;
        tmp.alloc_priv_sizes[i] = alloc_private_sizes != NULL ?
                                  alloc_private_sizes[i] :
                                  alloc_info[i].priv_drv_data_size;
        tmp.allocation_sizes[i] =
            result->allocation_info[i].allocation_size;
        tmp.allocation_num_pages[i] =
            hvdxg_allocation_num_pages(
                result->allocation_info[i].allocation_size);
        tmp.allocation_flags[i] =
            result->allocation_info[i].allocation_flags;
        tmp.allocation_cached[i] =
            requested_flags.create_cached ? 1 : 0;
        if (alloc_info[i].sysmem != 0) {
            tmp.existing_sysmem = 1;
            tmp.existing_sysmem_va = alloc_info[i].sysmem;
            tmp.existing_sysmem_size =
                result->allocation_info[i].allocation_size;
            if (alloc_info[i].allocation.v ==
                    hvdxg.existing_sysmem_last_allocation) {
                tmp.existing_sysmem_pfnmap_pages =
                    hvdxg.existing_sysmem_last_pfnmap_pages;
                tmp.existing_sysmem_vram =
                    hvdxg.existing_sysmem_last_vram;
            }
        }
    }
    if (req->flags.standard_allocation && req->alloc_count == 1 &&
        total_alloc_private != 0)
        tmp.alloc_priv_sizes[0] = total_alloc_private;

    if (runtime_private_data != NULL) {
        ret = hvdxg_copy_kernel_bytes(&tmp.private_runtime_data,
                                      runtime_private_data,
                                      tmp.private_runtime_data_size);
    } else {
        ret = hvdxg_copy_user_bytes(&tmp.private_runtime_data,
                                    req->private_runtime_data,
                                    tmp.private_runtime_data_size);
    }
    if (ret != 0)
        goto fail;
    tmp.runtime_d3d12_flags = hvdxg_runtime_d3d12_resource_flags(
        tmp.private_runtime_data, tmp.private_runtime_data_size);
    ret = hvdxg_copy_kernel_bytes(&tmp.resource_priv_drv_data,
                                  resource_priv_data,
                                  tmp.resource_priv_drv_data_size);
    if (ret != 0)
        goto fail;
    ret = hvdxg_copy_kernel_bytes(&tmp.total_priv_drv_data,
                                  alloc_private_data,
                                  tmp.total_priv_drv_data_size);
    if (ret != 0)
        goto fail;
    hvdxg_sync_shared_resource_records(&tmp);

    slot = hvdxg_owner_find_resource(owner, tmp.device, tmp.resource);
    if (slot == NULL) {
        if (hvdxg_grow_table((void **)&owner->resources,
                             &owner->resource_capacity,
                             owner->resource_count + 1,
                             sizeof(owner->resources[0]),
                             HV_DXG_RESOURCE_TRACKED_MAX) != 0)
            goto fail;
        slot = &owner->resources[owner->resource_count++];
    } else {
        hvdxg_shared_parent_remove_resource(slot);
        hvdxg_free_tracked_resource(slot);
    }
    *slot = tmp;
    if (tmp.shared_metadata_created)
        hvdxg.sharedresource_created++;
    if (slot->existing_sysmem)
        hvdxg_note_existing_sysmem_share(slot, 1);
    hvdxg.allocation_last_owner_process = slot->owner_process;
    hvdxg.allocation_last_owner_generation = slot->owner_generation;
    ret = hvdxg_track_object(owner, HV_DXG_OBJECT_RESOURCE,
                             tmp.resource, tmp.device, tmp.device);
    if (ret != 0)
        goto fail_slot;
    return 0;

fail:
    hvdxg_free_tracked_resource(&tmp);
    return ret != 0 ? ret : -ENOMEM;

fail_slot:
    hvdxg_untrack_resource(owner, tmp.device, tmp.resource);
    return ret;
}

static int hvdxg_clone_resource(struct hvdxg_tracked_resource *dst,
                                const struct hvdxg_tracked_resource *src)
{
    int ret;

    memset(dst, 0, sizeof(*dst));
    *dst = *src;
    dst->private_runtime_data = NULL;
    dst->resource_priv_drv_data = NULL;
    dst->total_priv_drv_data = NULL;
    ret = hvdxg_copy_kernel_bytes(&dst->private_runtime_data,
                                  src->private_runtime_data,
                                  src->private_runtime_data_size);
    if (ret != 0)
        goto fail;
    ret = hvdxg_copy_kernel_bytes(&dst->resource_priv_drv_data,
                                  src->resource_priv_drv_data,
                                  src->resource_priv_drv_data_size);
    if (ret != 0)
        goto fail;
    ret = hvdxg_copy_kernel_bytes(&dst->total_priv_drv_data,
                                  src->total_priv_drv_data,
                                  src->total_priv_drv_data_size);
    if (ret != 0)
        goto fail;
    hvdxg_sync_shared_resource_records(dst);
    return 0;

fail:
    hvdxg_free_tracked_resource(dst);
    return ret;
}

static void hvdxg_ntshared_cache_note(uint32 kind, uint32 process,
                                      uint32 object, uint32 handle,
                                      uint32 refs)
{
    hvdxg.ntshared_cache_last_kind = kind;
    hvdxg.ntshared_cache_last_process = process;
    hvdxg.ntshared_cache_last_object = object;
    hvdxg.ntshared_cache_last_handle = handle;
    hvdxg.ntshared_cache_last_refs = refs;
}

static int hvdxg_ntshared_cache_get(uint32 kind, uint32 process,
                                    uint32 object, uint32 *handle_out)
{
    if (handle_out != NULL)
        *handle_out = 0;
    if (kind == 0 || process == 0 || object == 0 || handle_out == NULL)
        return 0;
    for (uint32 i = 0; i < HV_DXG_NTSHARED_CACHE_MAX; i++) {
        struct hvdxg_ntshared_cache_entry *entry =
            &hvdxg.ntshared_cache[i];

        if (entry->kind == kind && entry->process == process &&
            entry->object == object && entry->host_nt_handle != 0) {
            entry->refs++;
            *handle_out = entry->host_nt_handle;
            hvdxg.ntshared_cache_hits++;
            hvdxg_ntshared_cache_note(kind, process, object,
                                      entry->host_nt_handle, entry->refs);
            return 1;
        }
    }
    hvdxg.ntshared_cache_misses++;
    hvdxg_ntshared_cache_note(kind, process, object, 0, 0);
    return 0;
}

static void hvdxg_ntshared_cache_insert(uint32 kind, uint32 process,
                                        uint32 object, uint32 handle)
{
    if (kind == 0 || process == 0 || object == 0 || handle == 0)
        return;
    for (uint32 i = 0; i < HV_DXG_NTSHARED_CACHE_MAX; i++) {
        struct hvdxg_ntshared_cache_entry *entry =
            &hvdxg.ntshared_cache[i];

        if (entry->kind == 0 || (entry->kind == kind &&
            entry->process == process && entry->object == object)) {
            entry->kind = kind;
            entry->process = process;
            entry->object = object;
            entry->host_nt_handle = handle;
            entry->refs = 1;
            hvdxg.ntshared_cache_inserts++;
            hvdxg_ntshared_cache_note(kind, process, object, handle, 1);
            return;
        }
    }
    hvdxg.ntshared_cache_full++;
    hvdxg_ntshared_cache_note(kind, process, object, handle, 0);
}

static uint32 hvdxg_ntshared_cache_put(uint32 kind, uint32 process,
                                       uint32 object, uint32 handle)
{
    if (kind == 0 || process == 0 || object == 0 || handle == 0)
        return handle;
    for (uint32 i = 0; i < HV_DXG_NTSHARED_CACHE_MAX; i++) {
        struct hvdxg_ntshared_cache_entry *entry =
            &hvdxg.ntshared_cache[i];

        if (entry->kind == kind && entry->process == process &&
            entry->object == object && entry->host_nt_handle == handle) {
            if (entry->refs > 1) {
                entry->refs--;
                hvdxg.ntshared_cache_releases++;
                hvdxg_ntshared_cache_note(kind, process, object, handle,
                                          entry->refs);
                return 0;
            }
            memset(entry, 0, sizeof(*entry));
            hvdxg.ntshared_cache_releases++;
            hvdxg.ntshared_cache_destroys++;
            hvdxg_ntshared_cache_note(kind, process, object, handle, 0);
            return handle;
        }
    }
    hvdxg_ntshared_cache_note(kind, process, object, handle, 0);
    return handle;
}

static uint32 hvdxg_ntshared_cache_refs(uint32 kind, uint32 process,
                                        uint32 object, uint32 handle)
{
    if (kind == 0 || process == 0 || object == 0 || handle == 0)
        return 0;
    for (uint32 i = 0; i < HV_DXG_NTSHARED_CACHE_MAX; i++) {
        struct hvdxg_ntshared_cache_entry *entry =
            &hvdxg.ntshared_cache[i];

        if (entry->kind == kind && entry->process == process &&
            entry->object == object && entry->host_nt_handle == handle)
            return entry->refs;
    }
    return 0;
}

static void hvdxg_snapshot_last_completion(uint16 *type_out, uint32 *len_out,
                                           uint8 prefix[8]);

static void hvdxg_note_ntshared_create_wire(uint32 attempt,
                                            const void *command,
                                            uint32 cmd_len,
                                            uint32 result_len,
                                            uint32 ext,
                                            int ext_host_vgpu_luid)
{
    uint8 wire[HV_DXG_NTSHARED_RAW_BYTES];
    uint32 copied = 0;
    uint32 n;

    if (attempt >= HV_DXG_NTSHARED_ATTEMPT_MAX || command == NULL)
        return;
    memset(hvdxg.ntshared_create_attempt_head[attempt], 0,
           sizeof(hvdxg.ntshared_create_attempt_head[attempt]));
    hvdxg.ntshared_create_attempt_head_len[attempt] = 0;
    hvdxg.ntshared_create_attempt_result_len[attempt] = result_len;
    hvdxg.ntshared_create_attempt_cmdid[attempt] =
        ((const struct hvdxg_command_vm_to_host *)command)->command_id;
    hvdxg.ntshared_create_attempt_command[attempt] =
        ((const struct hvdxg_command_vm_to_host *)command)->command_type;

    memset(wire, 0, sizeof(wire));
    if (ext != 0) {
        struct hvdxg_ext_header hdr;

        memset(&hdr, 0, sizeof(hdr));
        hdr.command_offset = sizeof(hdr);
        if (ext_host_vgpu_luid)
            hdr.vgpu_luid = hvdxg_ext_adapter_luid(NULL);
        n = sizeof(hdr) < sizeof(wire) ? sizeof(hdr) : sizeof(wire);
        memcpy(wire, &hdr, n);
        copied = n;
    }
    if (copied < sizeof(wire)) {
        n = cmd_len < sizeof(wire) - copied ?
            cmd_len : sizeof(wire) - copied;
        memcpy(wire + copied, command, n);
        copied += n;
    }
    memcpy(hvdxg.ntshared_create_attempt_head[attempt], wire, copied);
    hvdxg.ntshared_create_attempt_head_len[attempt] = copied;
}

static void hvdxg_note_shareobject_wire(const void *command, uint32 cmd_len,
                                        uint32 result_len)
{
    uint8 wire[HV_DXG_NTSHARED_RAW_BYTES];
    uint32 copied = 0;
    uint32 n;
    uint32 ext = hvdxg.use_ext_header ? 1 : 0;
    uint32 ext_offset = ext != 0 ? sizeof(struct hvdxg_ext_header) : 0;

    hvdxg.shareobject_last_wire_len = cmd_len + ext_offset;
    hvdxg.shareobject_last_ext = ext;
    hvdxg.shareobject_last_ext_offset = ext_offset;
    hvdxg.shareobject_last_result_len = result_len;
    hvdxg.shareobject_last_head_len = 0;
    memset(hvdxg.shareobject_last_head, 0,
           sizeof(hvdxg.shareobject_last_head));

    memset(wire, 0, sizeof(wire));
    if (ext != 0) {
        struct hvdxg_ext_header hdr;

        memset(&hdr, 0, sizeof(hdr));
        hdr.command_offset = sizeof(hdr);
        n = sizeof(hdr) < sizeof(wire) ? sizeof(hdr) : sizeof(wire);
        memcpy(wire, &hdr, n);
        copied = n;
    }
    if (copied < sizeof(wire)) {
        n = cmd_len < sizeof(wire) - copied ?
            cmd_len : sizeof(wire) - copied;
        memcpy(wire + copied, command, n);
        copied += n;
    }
    memcpy(hvdxg.shareobject_last_head, wire, copied);
    hvdxg.shareobject_last_head_len = copied;
}

static void hvdxg_note_ntshared_create_attempt(uint32 attempt, uint32 cmd_len,
                                               uint32 wire_len, uint32 ext,
                                               uint32 ext_offset,
                                               uint32 object_offset,
                                               uint32 actual_len, int ret,
                                               uint32 raw0,
                                               uint32 status, uint32 process,
                                               uint32 object)
{
    if (attempt >= HV_DXG_NTSHARED_ATTEMPT_MAX)
        return;
    if (hvdxg.ntshared_create_attempts < attempt + 1)
        hvdxg.ntshared_create_attempts = attempt + 1;
    hvdxg.ntshared_create_attempt_cmd_len[attempt] = cmd_len;
    hvdxg.ntshared_create_attempt_wire_len[attempt] = wire_len;
    hvdxg.ntshared_create_attempt_ext[attempt] = ext;
    hvdxg.ntshared_create_attempt_ext_offset[attempt] = ext_offset;
    hvdxg.ntshared_create_attempt_object_offset[attempt] = object_offset;
    hvdxg.ntshared_create_attempt_len[attempt] = actual_len;
    hvdxg.ntshared_create_attempt_ret[attempt] = ret;
    hvdxg.ntshared_create_attempt_raw0[attempt] = raw0;
    hvdxg.ntshared_create_attempt_status[attempt] = status;
    hvdxg.ntshared_create_attempt_process[attempt] = process;
    hvdxg.ntshared_create_attempt_object[attempt] = object;
    if (hvdxg.d3d12_shared_first_nt_seq == 0 && object != 0 &&
        (object == hvdxg.d3d12_shared_alloc_resource_out ||
         object == hvdxg.d3d12_shared_alloc_allocation))
        hvdxg.d3d12_shared_first_nt_seq =
            ++hvdxg.d3d12_shared_event_seq;
}

static int hvdxg_send_create_nt_shared_object_attempt(
    uint32 attempt, const void *command, uint32 cmd_len, uint32 object_offset,
    uint32 process, uint32 object, struct hvdxg_d3dkmthandle *result,
    uint32 *actual_len, int force_ext_header, int ext_host_vgpu_luid,
    int suppress_ext_header, uint32 label)
{
    int ret;
    uint32 ext = !suppress_ext_header &&
                 (hvdxg.use_ext_header || force_ext_header) ? 1 : 0;
    uint32 ext_offset = ext != 0 ? sizeof(struct hvdxg_ext_header) : 0;
    uint32 wire_len = cmd_len + ext_offset;

    if (result == NULL || actual_len == NULL)
        return -EINVAL;
    result->v = 0;
    *actual_len = 0;
    hvdxg_note_ntshared_create_wire(attempt, command, cmd_len,
                                    sizeof(*result), ext,
                                    ext_host_vgpu_luid);
    if (attempt < HV_DXG_NTSHARED_ATTEMPT_MAX)
        hvdxg.ntshared_create_attempt_label[attempt] = label;
    ret = hvdxg_send_sync_global_ex(command, cmd_len, result,
                                    sizeof(*result), actual_len,
                                    force_ext_header,
                                    ext_host_vgpu_luid,
                                    suppress_ext_header);
    if (attempt < HV_DXG_NTSHARED_ATTEMPT_MAX) {
        hvdxg.ntshared_create_attempt_channel[attempt] =
            hvdxg.global_send_ntshared.channel;
        hvdxg.ntshared_create_attempt_monitor[attempt] =
            hvdxg.global_send_ntshared.monitor_allocated;
        hvdxg.ntshared_create_attempt_monitorid[attempt] =
            hvdxg.global_send_ntshared.monitorid;
        hvdxg.ntshared_create_attempt_dedicated[attempt] =
            hvdxg.global_send_ntshared.dedicated;
    }
    hvdxg.ntshared_last_create_cmd_len = cmd_len;
    hvdxg.ntshared_last_create_object_offset = object_offset;
    hvdxg.ntshared_last_create_len = *actual_len;
    hvdxg.ntshared_last_create_object = object;
    hvdxg.ntshared_last_create_raw0 = result->v;
    hvdxg_snapshot_last_completion(
        &hvdxg.ntshared_last_create_completion_type,
        &hvdxg.ntshared_last_create_completion_len,
        hvdxg.ntshared_last_create_completion_prefix);
    if (ret == 0 && *actual_len == 0) {
        hvdxg.ntshared_last_create_zero_len = 1;
        ret = -EOVERFLOW;
    } else if (ret == 0 && *actual_len < sizeof(*result)) {
        ret = -EOVERFLOW;
    } else if (ret == 0 && result->v == 0) {
        ret = -EIO;
    }
    hvdxg_note_ntshared_create_attempt(
        attempt, cmd_len, wire_len, ext, ext_offset, object_offset,
        *actual_len, ret, result->v, result->v, process, object);
    return ret;
}

static uint32 hvdxg_owner_host_object_handle(struct hvdxg_open_state *owner,
                                             uint32 type, uint32 handle,
                                             uint64 *parent_out)
{
    struct hvdxg_object_entry *entry;

    if (parent_out != NULL)
        *parent_out = 0;
    entry = hvdxg_owner_find_object(owner, type, handle);
    if (entry == NULL)
        return 0;
    if (parent_out != NULL)
        *parent_out = entry->parent;
    return (uint32)(entry->host_handle != 0 ? entry->host_handle :
                    entry->handle);
}

static void hvdxg_snapshot_last_completion(uint16 *type_out, uint32 *len_out,
                                           uint8 prefix[8])
{
    uint32 prefix_len;

    if (type_out != NULL)
        *type_out = hvdxg.completion_type;
    if (len_out != NULL)
        *len_out = hvdxg.completion_len;
    if (prefix == NULL)
        return;
    memset(prefix, 0, 8);
    prefix_len = hvdxg.completion_len < 8 ? hvdxg.completion_len : 8;
    if (prefix_len != 0)
        memcpy(prefix, hvdxg.completion_buf, prefix_len);
}

static int hvdxg_ntshared_default_ext_header(void)
{
    return hvdxg.use_ext_header ||
           hvdxg.active_vmbus_version >= HV_DXG_VMBUS_INTERFACE_VERSION;
}

static int hvdxg_create_nt_shared_object(uint32 process, uint32 object,
                                         uint32 fallback_object,
                                         uint32 *used_object,
                                         uint32 *shared_handle,
                                         int prefer_wsl_layout)
{
    struct hvdxg_command_createntsharedobject_32 create;
    struct hvdxg_d3dkmthandle result;
    uint32 cmd_len = 0;
    uint32 object_offset = 0;
    uint32 actual_len = 0;
    int use_ext_header;
    int ret = -EIO;

    if (shared_handle != NULL)
        *shared_handle = 0;
    if (used_object != NULL)
        *used_object = 0;
    if (process == 0 || object == 0 || shared_handle == NULL)
        return -EINVAL;

    memset(&result, 0, sizeof(result));
    hvdxg.ntshared_last_create_len = 0;
    hvdxg.ntshared_last_create_cmd_len = sizeof(create);
    hvdxg.ntshared_last_create_object_offset =
        offsetof(struct hvdxg_command_createntsharedobject_32, object);
    hvdxg.ntshared_last_create_result_len = sizeof(result);
    hvdxg.ntshared_last_create_completion_type = 0;
    hvdxg.ntshared_last_create_completion_len = 0;
    memset(hvdxg.ntshared_last_create_completion_prefix, 0,
           sizeof(hvdxg.ntshared_last_create_completion_prefix));
    hvdxg.ntshared_last_create_ret = 0;
    hvdxg.ntshared_last_create_process = process;
    hvdxg.ntshared_last_create_type =
        HV_DXGK_VMBCOMMAND_CREATENTSHAREDOBJECT;
    hvdxg.ntshared_last_create_channel = HV_DXGKVMB_VM_TO_HOST;
    hvdxg.ntshared_last_create_object = object;
    hvdxg.ntshared_last_create_handle = 0;
    hvdxg.ntshared_last_create_raw0 = 0;
    hvdxg.ntshared_last_create_zero_len = 0;
    hvdxg.ntshared_last_create_side_effect = 0;
    hvdxg.ntshared_last_create_share_fallback = 0;
    hvdxg.ntshared_last_create_share_valid = 0;
    memset(&hvdxg.global_send_ntshared_ext, 0,
           sizeof(hvdxg.global_send_ntshared_ext));
    hvdxg.ntshared_create_attempts = 0;
    memset(hvdxg.ntshared_create_attempt_len, 0,
           sizeof(hvdxg.ntshared_create_attempt_len));
    memset(hvdxg.ntshared_create_attempt_result_len, 0,
           sizeof(hvdxg.ntshared_create_attempt_result_len));
    memset(hvdxg.ntshared_create_attempt_cmdid, 0,
           sizeof(hvdxg.ntshared_create_attempt_cmdid));
    memset(hvdxg.ntshared_create_attempt_command, 0,
           sizeof(hvdxg.ntshared_create_attempt_command));
    memset(hvdxg.ntshared_create_attempt_cmd_len, 0,
           sizeof(hvdxg.ntshared_create_attempt_cmd_len));
    memset(hvdxg.ntshared_create_attempt_wire_len, 0,
           sizeof(hvdxg.ntshared_create_attempt_wire_len));
    memset(hvdxg.ntshared_create_attempt_ext, 0,
           sizeof(hvdxg.ntshared_create_attempt_ext));
    memset(hvdxg.ntshared_create_attempt_ext_offset, 0,
           sizeof(hvdxg.ntshared_create_attempt_ext_offset));
    memset(hvdxg.ntshared_create_attempt_object_offset, 0,
           sizeof(hvdxg.ntshared_create_attempt_object_offset));
    memset(hvdxg.ntshared_create_attempt_ret, 0,
           sizeof(hvdxg.ntshared_create_attempt_ret));
    memset(hvdxg.ntshared_create_attempt_raw0, 0,
           sizeof(hvdxg.ntshared_create_attempt_raw0));
    memset(hvdxg.ntshared_create_attempt_status, 0,
           sizeof(hvdxg.ntshared_create_attempt_status));
    memset(hvdxg.ntshared_create_attempt_process, 0,
           sizeof(hvdxg.ntshared_create_attempt_process));
    memset(hvdxg.ntshared_create_attempt_object, 0,
           sizeof(hvdxg.ntshared_create_attempt_object));
    memset(hvdxg.ntshared_create_attempt_label, 0,
           sizeof(hvdxg.ntshared_create_attempt_label));
    memset(hvdxg.ntshared_create_attempt_channel, 0,
           sizeof(hvdxg.ntshared_create_attempt_channel));
    memset(hvdxg.ntshared_create_attempt_monitor, 0,
           sizeof(hvdxg.ntshared_create_attempt_monitor));
    memset(hvdxg.ntshared_create_attempt_monitorid, 0,
           sizeof(hvdxg.ntshared_create_attempt_monitorid));
    memset(hvdxg.ntshared_create_attempt_dedicated, 0,
           sizeof(hvdxg.ntshared_create_attempt_dedicated));
    memset(hvdxg.ntshared_create_attempt_head_len, 0,
           sizeof(hvdxg.ntshared_create_attempt_head_len));
    memset(hvdxg.ntshared_create_attempt_head, 0,
           sizeof(hvdxg.ntshared_create_attempt_head));

    (void)fallback_object;
    (void)prefer_wsl_layout;
    use_ext_header = hvdxg_ntshared_default_ext_header();

    memset(&create, 0, sizeof(create));
    hvdxg_command_vm_init(&create.hdr,
                          HV_DXGK_VMBCOMMAND_CREATENTSHAREDOBJECT);
    create.hdr.process.v = process;
    create.object.v = object;
    cmd_len = sizeof(create);
    object_offset =
        offsetof(struct hvdxg_command_createntsharedobject_32, object);
    hvdxg.ntshared_last_create_cmd_len = cmd_len;
    hvdxg.ntshared_last_create_object_offset = object_offset;
    ret = hvdxg_send_create_nt_shared_object_attempt(
        0, &create, cmd_len, object_offset, process, object,
        &result, &actual_len, use_ext_header, 0, !use_ext_header,
        use_ext_header ?
            HV_DXG_NTSHARED_LABEL_WSL_EXT32_ZERO_LUID_NATURAL :
            HV_DXG_NTSHARED_LABEL_WSL_NOEXT32_NATURAL);
    if (ret == 0) {
        *shared_handle = result.v;
        if (used_object != NULL)
            *used_object = object;
        hvdxg.ntshared_last_create_handle = result.v;
        hvdxg.ntshared_last_create_ret = 0;
        return 0;
    }
    hvdxg.ntshared_last_create_ret = ret;
    return ret;
}

static int hvdxg_flush_heap_transitions_process(uint32 process)
{
    struct hvdxg_command_flushheaptransitions flush;
    struct hvdxg_d3dkmthandle process_handle;
    struct hvdxg_ntstatus status;
    uint32 actual_len = 0;
    int ret;

    if (process == 0)
        return -EINVAL;

    memset(&flush, 0, sizeof(flush));
    memset(&status, 0, sizeof(status));
    process_handle.v = process;
    hvdxg_command_vgpu_init_process(
        &flush.hdr, HV_DXGK_VMBCOMMAND_FLUSHHEAPTRANSITIONS,
        process_handle);
    ret = hvdxg_send_sync_vgpu(&flush, sizeof(flush), &status,
                               sizeof(status), &actual_len);
    if (actual_len >= sizeof(status))
        hvdxg.cacheops_last_status = status.v;
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg.cacheops_last_len = actual_len;
    hvdxg.cacheops_last_ret = ret;
    hvdxg.cacheops_last_allocation = 0;
    return ret;
}

static int hvdxg_destroy_nt_shared_object(uint32 shared_handle)
{
    struct hvdxg_command_destroyntsharedobject destroy;
    struct hvdxg_ntstatus status;
    uint32 actual_len = 0;
    int ret;

    if (shared_handle == 0)
        return 0;

    memset(&destroy, 0, sizeof(destroy));
    memset(&status, 0, sizeof(status));
    hvdxg.ntshared_last_destroy_len = 0;
    hvdxg.ntshared_last_destroy_ret = 0;
    hvdxg.ntshared_last_destroy_status = 0;
    hvdxg.ntshared_last_destroy_handle = shared_handle;
    hvdxg_command_vm_init(&destroy.hdr,
                          HV_DXGK_VMBCOMMAND_DESTROYNTSHAREDOBJECT);
    destroy.shared_handle.v = shared_handle;

    ret = hvdxg_send_sync_global(&destroy, sizeof(destroy), &status,
                                 sizeof(status), &actual_len);
    hvdxg.ntshared_last_destroy_len = actual_len;
    hvdxg.ntshared_last_destroy_status = status.v;
    if (ret == 0 && actual_len < sizeof(status))
        ret = -EOVERFLOW;
    else if (ret == 0)
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg.ntshared_last_destroy_ret = ret;
    return ret;
}

static void hvdxg_clear_resource_nt_shared_handle(
    struct hvdxg_tracked_resource *resource, uint32 handle)
{
    if (resource == NULL)
        return;
    if (handle != 0 && resource->host_shared_handle_nt != handle)
        return;
    resource->host_shared_handle_nt = 0;
}

static uint32 hvdxg_alloc_shared_parent_id(void)
{
    if (hvdxg.sharedresource_parent_next_id == 0)
        hvdxg.sharedresource_parent_next_id = 1;
    return hvdxg.sharedresource_parent_next_id++;
}

static void hvdxg_note_shared_parent(struct hvdxg_shared_object *shared)
{
    if (shared == NULL || shared->kind != HV_DXG_SHARED_OBJECT_RESOURCE)
        return;
    if (shared->resource_parent != NULL) {
        hvdxg_note_shared_resource_parent(shared->resource_parent);
        return;
    }
    hvdxg.sharedresource_parent_last_id = shared->parent_id;
    hvdxg.sharedresource_parent_last_refs = shared->parent_refs;
    hvdxg.sharedresource_parent_last_fd_refs = shared->fd_refs;
    hvdxg.sharedresource_parent_last_children =
        shared->opened_child_count;
    hvdxg.sharedresource_parent_last_child =
        shared->opened_child_last_resource;
    hvdxg.sharedresource_parent_last_sealed_gen =
        shared->parent_sealed_generation;
}

static uint32 hvdxg_release_nt_shared_object_ref(uint32 kind, uint32 process,
                                                 uint32 object, uint32 handle)
{
    uint32 destroy_handle;

    destroy_handle = hvdxg_ntshared_cache_put(kind, process, object, handle);
    if (destroy_handle != 0)
        (void)hvdxg_destroy_nt_shared_object(destroy_handle);
    return destroy_handle;
}

static int hvdxg_defer_shared_resource_destroy(uint32 process, uint32 object,
                                               uint32 nt_handle,
                                               uint32 device,
                                               uint32 resource)
{
    struct hvdxg_deferred_shared_destroy *slot = NULL;

    if (process == 0 || object == 0 || nt_handle == 0 ||
        device == 0 || resource == 0)
        return -EINVAL;

    for (uint32 i = 0; i < HV_DXG_DEFERRED_SHARED_DESTROY_MAX; i++) {
        struct hvdxg_deferred_shared_destroy *entry =
            &hvdxg.deferred_shared_destroy[i];

        if (!entry->valid) {
            if (slot == NULL)
                slot = entry;
            continue;
        }
        if (entry->process == process && entry->object == object &&
            entry->nt_handle == nt_handle && entry->device == device &&
            entry->resource == resource) {
            slot = entry;
            break;
        }
    }
    if (slot == NULL) {
        hvdxg.deferred_shared_destroy_failed++;
        hvdxg.deferred_shared_destroy_last_ret = -ENOSPC;
        return -ENOSPC;
    }

    memset(slot, 0, sizeof(*slot));
    slot->valid = 1;
    slot->process = process;
    slot->object = object;
    slot->nt_handle = nt_handle;
    slot->device = device;
    slot->resource = resource;
    hvdxg.deferred_shared_destroy_queued++;
    hvdxg.deferred_shared_destroy_last_process = process;
    hvdxg.deferred_shared_destroy_last_object = object;
    hvdxg.deferred_shared_destroy_last_nt = nt_handle;
    hvdxg.deferred_shared_destroy_last_device = device;
    hvdxg.deferred_shared_destroy_last_resource = resource;
    hvdxg.deferred_shared_destroy_last_ret = 0;
    return 0;
}

static void hvdxg_flush_deferred_shared_resource_destroy(
    uint32 process, uint32 object, uint32 nt_handle)
{
    for (uint32 i = 0; i < HV_DXG_DEFERRED_SHARED_DESTROY_MAX; i++) {
        struct hvdxg_deferred_shared_destroy *entry =
            &hvdxg.deferred_shared_destroy[i];
        int ret;

        if (!entry->valid || entry->process != process ||
            entry->object != object || entry->nt_handle != nt_handle)
            continue;

        ret = hvdxg_destroy_allocation_host_process(
            entry->process, entry->device, entry->resource, 0,
            HV_DXG_DESTROY_ALLOC_CTX_IOCTL);
        hvdxg.deferred_shared_destroy_last_process = entry->process;
        hvdxg.deferred_shared_destroy_last_object = entry->object;
        hvdxg.deferred_shared_destroy_last_nt = entry->nt_handle;
        hvdxg.deferred_shared_destroy_last_device = entry->device;
        hvdxg.deferred_shared_destroy_last_resource = entry->resource;
        hvdxg.deferred_shared_destroy_last_ret = ret;
        if (ret == 0) {
            hvdxg.deferred_shared_destroy_flushed++;
            memset(entry, 0, sizeof(*entry));
        } else {
            hvdxg.deferred_shared_destroy_failed++;
        }
    }
}

static uint32 hvdxg_shared_object_fops_kind(struct vfs_file *file);

static int hvdxg_shared_object_release(struct vfs_inode *ip,
                                       struct vfs_file *file)
{
    struct hvdxg_shared_object *shared =
        file != NULL ? (struct hvdxg_shared_object *)file->private_data : NULL;

    (void)ip;
    if (shared != NULL) {
        uint32 refs_before = 0;
        uint32 destroy_handle = 0;

        refs_before = hvdxg_ntshared_cache_refs(
            shared->kind, shared->cache_process,
            shared->cache_object != 0 ? shared->cache_object :
            shared->object,
            shared->host_nt_handle);
        if (refs_before == 0 &&
            shared->kind == HV_DXG_SHARED_OBJECT_RESOURCE) {
            struct hvdxg_tracked_resource *resource =
                hvdxg_shared_parent_resource(shared);

            refs_before = resource != NULL ?
                resource->host_shared_refs : 0;
        }
        hvdxg.sharedclose_last_kind = shared->kind;
        hvdxg.sharedclose_last_process = shared->cache_process;
        hvdxg.sharedclose_last_object =
            shared->cache_object != 0 ? shared->cache_object :
            shared->object;
        hvdxg.sharedclose_last_cache_object = shared->cache_object;
        hvdxg.sharedclose_last_nt_handle = shared->host_nt_handle;
        hvdxg.sharedclose_last_global = shared->global_share;
        hvdxg.sharedclose_last_refs_before = refs_before;
        hvdxg.sharedclose_last_refs_after = refs_before;
        hvdxg.sharedclose_last_fops_kind =
            hvdxg_shared_object_fops_kind(file);
        hvdxg.sharedclose_last_host_shared_handle =
            shared->global_share;
        hvdxg.sharedclose_last_destroy_handle = 0;
        hvdxg.sharedclose_last_destroy_ret = 0;
        hvdxg.sharedclose_last_destroy_status = 0;
        hvdxg.sharedclose_last_destroy_actual_len = 0;
        hvdxg.sharedclose_last_destroy_handle_offset =
            offsetof(struct hvdxg_command_destroyntsharedobject,
                     shared_handle);
        hvdxg.sharedclose_last_destroy_cmd_len = 0;
        hvdxg.sharedclose_last_destroy_wire_len = 0;
        hvdxg.sharedclose_last_destroy_ext = 0;
        hvdxg.sharedclose_last_destroy_ext_offset = 0;
        hvdxg.sharedclose_last_destroy_result_len = 0;
        if (shared->host_nt_handle != 0) {
            destroy_handle = hvdxg_release_nt_shared_object_ref(
                shared->kind, shared->cache_process,
                shared->cache_object != 0 ? shared->cache_object :
                shared->object,
                shared->host_nt_handle);
            if (shared->kind == HV_DXG_SHARED_OBJECT_RESOURCE &&
                destroy_handle != 0) {
                hvdxg_flush_deferred_shared_resource_destroy(
                    shared->cache_process,
                    shared->cache_object != 0 ? shared->cache_object :
                    shared->object,
                    shared->host_nt_handle);
            }
            hvdxg.sharedclose_last_refs_after =
                hvdxg_ntshared_cache_refs(
                    shared->kind, shared->cache_process,
                    shared->cache_object != 0 ? shared->cache_object :
                    shared->object, shared->host_nt_handle);
            hvdxg.sharedclose_last_destroy_handle = destroy_handle;
            hvdxg.sharedclose_last_destroy_ret =
                destroy_handle != 0 ? hvdxg.ntshared_last_destroy_ret : 0;
            hvdxg.sharedclose_last_destroy_status =
                destroy_handle != 0 ?
                hvdxg.ntshared_last_destroy_status : 0;
            hvdxg.sharedclose_last_destroy_actual_len =
                destroy_handle != 0 ? hvdxg.ntshared_last_destroy_len : 0;
            if (destroy_handle != 0) {
                hvdxg.sharedclose_last_destroy_cmd_len =
                    hvdxg.global_send_destroynt.cmd_len;
                hvdxg.sharedclose_last_destroy_wire_len =
                    hvdxg.global_send_destroynt.wire_len;
                hvdxg.sharedclose_last_destroy_ext =
                    hvdxg.global_send_destroynt.ext;
                hvdxg.sharedclose_last_destroy_ext_offset =
                    hvdxg.global_send_destroynt.ext_offset;
                hvdxg.sharedclose_last_destroy_result_len =
                    hvdxg.global_send_destroynt.result_len;
            }
        }
        if (shared->kind == HV_DXG_SHARED_OBJECT_RESOURCE) {
            if (shared->resource_parent != NULL) {
                if (destroy_handle != 0)
                    hvdxg_clear_resource_nt_shared_handle(
                        &shared->resource_parent->resource,
                        destroy_handle);
                if (shared->resource_parent->fd_refs != 0)
                    shared->resource_parent->fd_refs--;
                if (shared->resource_parent->host_nt_refs != 0)
                    shared->resource_parent->host_nt_refs--;
                if (shared->resource_parent->refs != 0)
                    shared->resource_parent->refs--;
                hvdxg_note_shared_resource_parent(shared->resource_parent);
                hvdxg_shared_parent_put(shared->resource_parent);
                shared->resource_parent = NULL;
            } else {
                if (shared->fd_refs != 0)
                    shared->fd_refs--;
                if (shared->parent_refs != 0)
                    shared->parent_refs--;
                hvdxg_note_shared_parent(shared);
            }
            hvdxg.sharedresource_parent_release_count++;
        }
        if (shared->kind > 0 &&
            shared->kind < HV_DXG_SHARED_OBJECT_KIND_MAX) {
            uint32 kind = shared->kind;

            hvdxg.sharedclose_kind_seen[kind]++;
            hvdxg.sharedclose_kind_fops[kind] =
                hvdxg.sharedclose_last_fops_kind;
            hvdxg.sharedclose_kind_refs_before[kind] =
                hvdxg.sharedclose_last_refs_before;
            hvdxg.sharedclose_kind_refs_after[kind] =
                hvdxg.sharedclose_last_refs_after;
            hvdxg.sharedclose_kind_destroy_handle[kind] =
                hvdxg.sharedclose_last_destroy_handle;
            hvdxg.sharedclose_kind_destroy_ret[kind] =
                hvdxg.sharedclose_last_destroy_ret;
            hvdxg.sharedclose_kind_destroy_status[kind] =
                hvdxg.sharedclose_last_destroy_status;
            hvdxg.sharedclose_kind_destroy_actual_len[kind] =
                hvdxg.sharedclose_last_destroy_actual_len;
            hvdxg.sharedclose_kind_destroy_cmd_len[kind] =
                hvdxg.sharedclose_last_destroy_cmd_len;
            hvdxg.sharedclose_kind_destroy_wire_len[kind] =
                hvdxg.sharedclose_last_destroy_wire_len;
            hvdxg.sharedclose_kind_destroy_ext[kind] =
                hvdxg.sharedclose_last_destroy_ext;
            hvdxg.sharedclose_kind_destroy_result_len[kind] =
                hvdxg.sharedclose_last_destroy_result_len;
        }
        if (shared->kind == HV_DXG_SHARED_OBJECT_RESOURCE &&
            shared->resource_parent == NULL)
            hvdxg_free_tracked_resource(&shared->resource);
        kvfree(shared);
        file->private_data = NULL;
    }
    return 0;
}

