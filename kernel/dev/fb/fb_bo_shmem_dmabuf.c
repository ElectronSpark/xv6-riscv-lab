
/*
 * GEM/BO, PRIME, and dma-buf support over xv6 shmem-style page arrays.
 *
 * Some private FB_GPU_TTM_* ioctl/status names remain for ABI compatibility
 * with existing xv6 GPU probes. They describe placement metadata and
 * dma_resv-like diagnostics only; backing storage is sysmem pages allocated
 * through fb_shmem_alloc_pages(), not a Linux TTM memory manager.
 */

static struct fb_gpu_bo_entry *fb_bo_lookup_locked(uint32 handle)
{
    if (handle == 0)
        return NULL;
    for (int i = 0; i < FB_GPU_MAX_BOS; i++) {
        if (fb_state.bos[i].in_use && !fb_state.bos[i].dead &&
            fb_state.bos[i].handle == handle)
            return &fb_state.bos[i];
    }
    return NULL;
}

static struct fb_gpu_render_owner *
fb_gpu_render_owner_lookup_locked(uint64 owner_id, pid_t owner_tgid)
{
    if (owner_id == 0)
        return NULL;
    for (int i = 0; i < FB_GPU_MAX_RENDER_OWNERS; i++) {
        struct fb_gpu_render_owner *owner = fb_state.render_owners[i];
        if (owner == NULL)
            continue;
        if (owner->id == owner_id &&
            (owner_tgid <= 0 || owner->tgid == owner_tgid))
            return owner;
    }
    return NULL;
}

static int fb_gpu_render_owner_register(struct fb_gpu_render_owner *owner)
{
    if (owner == NULL || owner->id == 0)
        return -EINVAL;
    spin_lock(&fb_state.lock);
    for (int i = 0; i < FB_GPU_MAX_RENDER_OWNERS; i++) {
        if (fb_state.render_owners[i] == NULL) {
            fb_state.render_owners[i] = owner;
            spin_unlock(&fb_state.lock);
            return 0;
        }
    }
    spin_unlock(&fb_state.lock);
    return -ENOSPC;
}

static void fb_gpu_render_owner_unregister(struct fb_gpu_render_owner *owner)
{
    if (owner == NULL)
        return;
    spin_lock(&fb_state.lock);
    for (int i = 0; i < FB_GPU_MAX_RENDER_OWNERS; i++) {
        if (fb_state.render_owners[i] == owner)
            fb_state.render_owners[i] = NULL;
    }
    spin_unlock(&fb_state.lock);
}

static struct fb_gpu_bo_entry *
fb_bo_owner_handle_lookup_locked(struct fb_gpu_render_owner *owner,
                                 uint32 handle)
{
    if (owner == NULL || handle == 0)
        return NULL;
    for (int i = 0; i < FB_GPU_MAX_BOS; i++) {
        if (owner->bo_handles[i].in_use &&
            owner->bo_handles[i].handle == handle) {
            struct fb_gpu_bo_entry *bo = owner->bo_handles[i].bo;
            if (bo != NULL && bo->in_use && !bo->dead)
                return bo;
            return NULL;
        }
    }
    return NULL;
}

static int fb_bo_owner_matches(const struct fb_gpu_bo_entry *bo,
                               uint64 owner_id, pid_t owner_tgid);

static struct fb_gpu_bo_entry *fb_bo_lookup_owned_locked(uint32 handle,
                                                        uint64 owner_id,
                                                        pid_t owner_tgid)
{
    struct fb_gpu_render_owner *owner =
        fb_gpu_render_owner_lookup_locked(owner_id, owner_tgid);
    struct fb_gpu_bo_entry *bo;

    if (owner != NULL)
        return fb_bo_owner_handle_lookup_locked(owner, handle);
    bo = fb_bo_lookup_locked(handle);
    if (bo != NULL && !fb_bo_owner_matches(bo, owner_id, owner_tgid))
        bo = NULL;
    return bo;
}

static uint32 fb_bo_owner_alloc_handle_locked(struct fb_gpu_render_owner *owner)
{
    uint32 start;
    uint32 handle;

    if (owner == NULL)
        return 0;
    if (owner->next_bo_handle == 0)
        owner->next_bo_handle = 1;
    start = owner->next_bo_handle;
    handle = start;
    do {
        if (handle == 0)
            handle = 1;
        if (fb_bo_owner_handle_lookup_locked(owner, handle) == NULL) {
            owner->next_bo_handle = handle + 1;
            if (owner->next_bo_handle == 0)
                owner->next_bo_handle = 1;
            return handle;
        }
        handle++;
        if (handle == 0)
            handle = 1;
    } while (handle != start);
    return 0;
}

static int fb_bo_owner_add_handle_locked(struct fb_gpu_render_owner *owner,
                                         struct fb_gpu_bo_entry *bo,
                                         uint32 *handle, int borrowed)
{
    uint32 local_handle;

    if (owner == NULL || bo == NULL || handle == NULL)
        return -EINVAL;
    local_handle = fb_bo_owner_alloc_handle_locked(owner);
    if (local_handle == 0)
        return -ENOSPC;
    for (int i = 0; i < FB_GPU_MAX_BOS; i++) {
        if (!owner->bo_handles[i].in_use) {
            owner->bo_handles[i].in_use = 1;
            owner->bo_handles[i].borrowed = borrowed != 0;
            owner->bo_handles[i].handle = local_handle;
            owner->bo_handles[i].bo = bo;
            if (borrowed)
                bo->refs++;
            *handle = local_handle;
            return 0;
        }
    }
    return -ENOSPC;
}

static int fb_bo_owner_remove_handle_locked(struct fb_gpu_render_owner *owner,
                                            uint32 handle,
                                            struct fb_gpu_bo_entry **bo_out,
                                            int *borrowed_out)
{
    if (bo_out != NULL)
        *bo_out = NULL;
    if (borrowed_out != NULL)
        *borrowed_out = 0;
    if (owner == NULL || handle == 0)
        return -ENOENT;
    for (int i = 0; i < FB_GPU_MAX_BOS; i++) {
        if (!owner->bo_handles[i].in_use ||
            owner->bo_handles[i].handle != handle)
            continue;
        if (bo_out != NULL)
            *bo_out = owner->bo_handles[i].bo;
        if (borrowed_out != NULL)
            *borrowed_out = owner->bo_handles[i].borrowed;
        memset(&owner->bo_handles[i], 0, sizeof(owner->bo_handles[i]));
        return 0;
    }
    return -ENOENT;
}

static int fb_bo_owner_matches(const struct fb_gpu_bo_entry *bo,
                               uint64 owner_id, pid_t owner_tgid)
{
    if (bo == NULL)
        return 0;
    if (owner_id != 0)
        return bo->owner_id == owner_id;
    if (owner_tgid > 0 && bo->owner_id != 0)
        return bo->owner_tgid == owner_tgid;
    return 1;
}

static void fb_sg_table_build(struct fb_gpu_sg_table *sgt, page_t **pages,
                              uint32 npages, uint64 size)
{
    uint32 last;

    if (sgt == NULL)
        return;
    memset(sgt, 0, sizeof(*sgt));
    if (pages == NULL || npages == 0 || size == 0)
        return;
    sgt->pages = pages;
    sgt->nents = npages;
    sgt->orig_nents = npages;
    sgt->total_len = size;
    if (pages[0] != NULL)
        sgt->dma_addr_base = __page_to_pa(pages[0]);
    last = npages - 1;
    if (pages[last] != NULL)
        sgt->dma_addr_last = __page_to_pa(pages[last]);
}

static void fb_sg_table_first_entry(const struct fb_gpu_sg_table *sgt,
                                    struct fb_gpu_sg_entry *entry)
{
    if (entry == NULL)
        return;
    memset(entry, 0, sizeof(*entry));
    if (sgt == NULL || sgt->pages == NULL || sgt->nents == 0 ||
        sgt->pages[0] == NULL)
        return;
    entry->page = sgt->pages[0];
    entry->dma_addr = __page_to_pa(entry->page);
    entry->offset = 0;
    entry->length = sgt->total_len < PGSIZE ? (uint32)sgt->total_len :
        PGSIZE;
}

static void fb_gem_copy_to_bo_locked(struct fb_gpu_bo_entry *bo,
                                     struct fb_gpu_gem_object *gem)
{
    if (bo == NULL || gem == NULL)
        return;
    bo->gem = gem;
    bo->gem_id = gem->id;
    bo->width = gem->width;
    bo->height = gem->height;
    bo->pitch = gem->pitch;
    bo->npages = gem->npages;
    bo->size = gem->size;
    bo->last_fence = gem->last_fence;
    bo->signaled_fence = gem->signaled_fence;
    bo->ttm_placement = gem->ttm_placement;
    bo->ttm_mem_type = gem->ttm_mem_type;
    bo->ttm_pin_count = gem->ttm_pin_count;
    bo->ttm_reservation_seq = gem->ttm_reservation_seq;
    bo->ttm_tt_populated = gem->ttm_tt_populated;
    bo->ttm_sg_nents = gem->ttm_sg_nents;
    bo->ttm_dma_addr_base = gem->ttm_dma_addr_base;
    bo->sgt = gem->sgt;
    bo->ttm_lru_seq = gem->ttm_lru_seq;
    bo->ttm_move_count = gem->ttm_move_count;
    bo->ttm_resv_count = gem->ttm_resv_count;
    bo->ttm_resv_seq = gem->ttm_resv_seq;
    bo->ttm_resv_exclusive_fence = gem->ttm_resv_exclusive_fence;
    bo->ttm_resv_owner_id = gem->ttm_resv_owner_id;
    bo->ttm_resv_owner_tgid = gem->ttm_resv_owner_tgid;
    bo->ttm_resv_shared_count = gem->ttm_resv_shared_count;
    bo->ttm_resv_shared_next = gem->ttm_resv_shared_next;
    bo->ttm_resv_waiters = gem->ttm_resv_waiters;
    bo->ttm_resv_wakeup_seq = gem->ttm_resv_wakeup_seq;
    memmove(bo->ttm_resv_shared, gem->ttm_resv_shared,
            sizeof(bo->ttm_resv_shared));
    bo->nouveau_domain = gem->nouveau_domain;
    bo->nouveau_tile_mode = gem->nouveau_tile_mode;
    bo->nouveau_tile_flags = gem->nouveau_tile_flags;
    bo->virtio_resource_id = gem->virtio_resource_id;
    bo->virtio_resource_owner_id = gem->virtio_resource_owner_id;
    bo->virtio_resource_owner_tgid = gem->virtio_resource_owner_tgid;
    bo->pages = gem->pages;
}

static struct fb_gpu_gem_object *
fb_gem_alloc_locked(uint32 width, uint32 height, uint32 pitch, uint64 size,
                    page_t **pages, uint32 npages)
{
    struct fb_gpu_gem_object *gem = NULL;
    uint32 next;

    for (int i = 0; i < FB_GPU_MAX_BOS; i++) {
        if (!fb_state.gems[i].in_use) {
            gem = &fb_state.gems[i];
            break;
        }
    }
    if (gem == NULL)
        return NULL;

    next = fb_state.next_gem_id++;
    if (fb_state.next_gem_id == 0)
        fb_state.next_gem_id = 1;
    memset(gem, 0, sizeof(*gem));
    gem->in_use = 1;
    gem->id = next;
    gem->refs = 1;
    gem->width = width;
    gem->height = height;
    gem->pitch = pitch;
    gem->npages = npages;
    gem->size = size;
    gem->pages = pages;
    gem->ttm_placement = FB_TTM_PL_SYSTEM;
    gem->ttm_mem_type = FB_TTM_MEM_SYSTEM;
    gem->ttm_reservation_seq = 1;
    gem->ttm_tt_populated = 0;
    fb_sg_table_build(&gem->sgt, pages, npages, size);
    gem->ttm_sg_nents = gem->sgt.nents;
    gem->ttm_dma_addr_base = gem->sgt.dma_addr_base;
    gem->ttm_lru_seq = ++fb_state.ttm_lru_clock;
    gem->ttm_move_count = 0;
    gem->nouveau_domain = NOUVEAU_GEM_DOMAIN_CPU |
                          NOUVEAU_GEM_DOMAIN_MAPPABLE |
                          NOUVEAU_GEM_DOMAIN_COHERENT;
    gem->nouveau_tile_mode = 0;
    gem->nouveau_tile_flags = 0;
    gem->metadata.format = FB_GPU_BO_FORMAT_XRGB8888;
    gem->metadata.modifier = FB_GPU_BO_MOD_LINEAR;
    gem->metadata.plane_count = 1;
    gem->metadata.offsets[0] = 0;
    gem->metadata.strides[0] = pitch;
    return gem;
}

static int fb_bo_set_virtio_resource(uint32 handle, uint64 owner_id,
                                     pid_t owner_tgid,
                                     uint32 virtio_resource_id)
{
    struct fb_gpu_bo_entry *bo;

    if (handle == 0 || virtio_resource_id == 0)
        return -EINVAL;

    spin_lock(&fb_state.lock);
    bo = fb_bo_lookup_owned_locked(handle, owner_id, owner_tgid);
    if (bo == NULL) {
        spin_unlock(&fb_state.lock);
        return -ENOENT;
    }
    if (!fb_bo_owner_matches(bo, owner_id, owner_tgid)) {
        spin_unlock(&fb_state.lock);
        return -EPERM;
    }
    bo->virtio_resource_id = virtio_resource_id;
    bo->virtio_resource_owner_id = owner_id;
    bo->virtio_resource_owner_tgid = owner_tgid;
    if (bo->gem != NULL) {
        bo->gem->virtio_resource_id = virtio_resource_id;
        bo->gem->virtio_resource_owner_id = owner_id;
        bo->gem->virtio_resource_owner_tgid = owner_tgid;
    }
    spin_unlock(&fb_state.lock);
    return 0;
}

static void fb_gem_get_locked(struct fb_gpu_gem_object *gem)
{
    if (gem != NULL && gem->in_use)
        gem->refs++;
}

static void fb_gem_put(struct fb_gpu_gem_object *gem)
{
    page_t **pages = NULL;
    uint32 npages = 0;
    uint32 virtio_resource_id = 0;

    if (gem == NULL)
        return;
    spin_lock(&fb_state.lock);
    if (gem->refs > 0)
        gem->refs--;
    if (gem->refs == 0) {
        pages = gem->pages;
        npages = gem->npages;
        virtio_resource_id = gem->virtio_resource_id;
        memset(gem, 0, sizeof(*gem));
    }
    spin_unlock(&fb_state.lock);
    if (virtio_resource_id != 0)
        virtio_gpu_user_resource_export_put(virtio_resource_id);
    fb_shmem_release_pages(pages, npages);
}

static int fb_bo_apply_dmabuf_metadata_locked(
    struct fb_gpu_bo_entry *bo, uint32 format, uint64 modifier,
    uint32 plane_count, const uint32 offsets[4], const uint32 strides[4])
{
    struct fb_gpu_dmabuf_metadata *meta;

    if (bo == NULL || bo->gem == NULL || format == 0)
        return 0;
    if (modifier != DRM_FORMAT_MOD_LINEAR || plane_count == 0 ||
        plane_count > 4)
        return -EINVAL;
    if (format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_ARGB8888 &&
        format != DRM_FORMAT_NV12)
        return -EINVAL;
    if ((format == DRM_FORMAT_NV12 && plane_count != 2) ||
        (format != DRM_FORMAT_NV12 && plane_count != 1))
        return -EINVAL;
    for (uint32 i = 0; i < plane_count; i++) {
        if (strides[i] == 0)
            return -EINVAL;
        if (i == 0 && offsets[i] != 0)
            return -EINVAL;
    }

    meta = &bo->gem->metadata;
    meta->format = format;
    meta->modifier = modifier;
    meta->plane_count = plane_count;
    memmove(meta->offsets, offsets, sizeof(meta->offsets));
    memmove(meta->strides, strides, sizeof(meta->strides));
    return 0;
}

static uint32 fb_ttm_mem_type_for_placement(uint32 placement)
{
    if ((placement & FB_TTM_PL_VRAM) != 0)
        return FB_TTM_MEM_VRAM;
    if ((placement & FB_TTM_PL_TT) != 0)
        return FB_TTM_MEM_TT;
    if ((placement & FB_TTM_PL_STOLEN) != 0)
        return FB_TTM_MEM_STOLEN;
    return FB_TTM_MEM_SYSTEM;
}

static struct fb_ttm_resource_manager *fb_ttm_manager_locked(uint32 mem_type)
{
    struct fb_ttm_resource_manager *mgr;

    if (mem_type > FB_TTM_MEM_STOLEN)
        mem_type = FB_TTM_MEM_SYSTEM;
    mgr = &fb_state.ttm_mgr[mem_type];
    if (mgr->limit == 0) {
        mgr->mem_type = mem_type;
        switch (mem_type) {
        case FB_TTM_MEM_TT:
            mgr->limit = 512ULL * 1024 * 1024;
            break;
        case FB_TTM_MEM_VRAM:
            mgr->limit = 256ULL * 1024 * 1024;
            break;
        case FB_TTM_MEM_STOLEN:
            mgr->limit = fb_state.fb_size != 0 ?
                fb_state.fb_size : 64ULL * 1024 * 1024;
            break;
        default:
            mgr->limit = (uint64)-1;
            break;
        }
    }
    return mgr;
}

static void fb_ttm_account_locked(uint32 mem_type, uint64 size, int add)
{
    uint64 *counter;
    struct fb_ttm_resource_manager *mgr = fb_ttm_manager_locked(mem_type);

    switch (mem_type) {
    case FB_TTM_MEM_TT:
        counter = &fb_state.stats.ttm_tt_bytes;
        break;
    case FB_TTM_MEM_VRAM:
        counter = &fb_state.stats.ttm_vram_bytes;
        break;
    case FB_TTM_MEM_STOLEN:
        counter = &fb_state.stats.ttm_stolen_bytes;
        break;
    default:
        counter = &fb_state.stats.ttm_system_bytes;
        break;
    }
    if (add) {
        *counter += size;
        mgr->used += size;
    } else if (*counter >= size) {
        *counter -= size;
        mgr->used = mgr->used >= size ? mgr->used - size : 0;
    } else {
        *counter = 0;
        mgr->used = 0;
    }
}

static int fb_ttm_valid_placement(uint32 placement)
{
    return placement != 0 &&
        (placement & ~(FB_TTM_PL_SYSTEM | FB_TTM_PL_TT |
                       FB_TTM_PL_VRAM | FB_TTM_PL_STOLEN)) == 0;
}

static void fb_ttm_propagate_gem_locked(struct fb_gpu_bo_entry *bo);
static int fb_ttm_reserve_locked(struct fb_gpu_bo_entry *bo,
                                 uint64 owner_id, pid_t owner_tgid);
static int fb_ttm_unreserve_locked(struct fb_gpu_bo_entry *bo,
                                   uint64 owner_id, pid_t owner_tgid);

static int fb_ttm_cpu_copy_fallback_path(uint32 old_mem_type,
                                         uint32 new_mem_type)
{
    return old_mem_type == FB_TTM_MEM_SYSTEM ||
        new_mem_type == FB_TTM_MEM_SYSTEM ||
        old_mem_type == FB_TTM_MEM_TT ||
        new_mem_type == FB_TTM_MEM_TT;
}

/*
 * Match manager_moves[] semantics: diagnostics are bucketed by destination
 * domain.  This records the path selected by the current xv6 backing model,
 * without granting native-acceleration credit or changing migration behavior.
 */
static void fb_ttm_note_move_diag_locked(uint32 old_mem_type,
                                         uint32 new_mem_type)
{
    struct fb_ttm_resource_manager *mgr;

    if (new_mem_type > FB_TTM_MEM_STOLEN)
        new_mem_type = FB_TTM_MEM_SYSTEM;
    mgr = fb_ttm_manager_locked(new_mem_type);
    if (old_mem_type == new_mem_type) {
        mgr->metadata_noop_moves++;
        fb_state.stats.ttm_metadata_noop_moves[new_mem_type]++;
    } else if (fb_ttm_cpu_copy_fallback_path(old_mem_type, new_mem_type)) {
        mgr->cpu_copy_fallback_moves++;
        fb_state.stats.ttm_cpu_copy_fallback_moves[new_mem_type]++;
    } else {
        mgr->unsupported_hw_copy_moves++;
        fb_state.stats.ttm_unsupported_hw_copy_moves[new_mem_type]++;
    }
}

static int fb_ttm_resv_owner_matches(const struct fb_gpu_gem_object *gem,
                                     uint64 owner_id, pid_t owner_tgid)
{
    if (gem == NULL || gem->ttm_resv_owner_id == 0)
        return 1;
    if (owner_id != 0)
        return gem->ttm_resv_owner_id == owner_id;
    if (owner_tgid > 0)
        return gem->ttm_resv_owner_tgid == owner_tgid;
    return 0;
}

static void fb_ttm_resv_note_enabled_locked(void)
{
    fb_state.stats.ttm_resv_shared_slots = FB_GPU_RESV_SHARED_SLOTS;
    fb_state.stats.dmabuf_poll_semantics = 1;
    fb_state.stats.dmabuf_shared_fence_semantics =
        FB_GPU_RESV_SHARED_SLOTS;
    fb_state.stats.dmabuf_wait_queue_semantics = 1;
}

static uint64
fb_ttm_resv_latest_shared_fence_locked(const struct fb_gpu_gem_object *gem)
{
    uint64 latest = 0;

    if (gem == NULL)
        return 0;
    for (uint32 i = 0; i < gem->ttm_resv_shared_count &&
         i < FB_GPU_RESV_SHARED_SLOTS; i++) {
        if (gem->ttm_resv_shared[i].fence > latest)
            latest = gem->ttm_resv_shared[i].fence;
    }
    return latest;
}

static void fb_ttm_resv_sync_bo_from_gem_locked(
    struct fb_gpu_bo_entry *bo, const struct fb_gpu_gem_object *gem)
{
    if (bo == NULL || gem == NULL)
        return;
    bo->ttm_resv_count = gem->ttm_resv_count;
    bo->ttm_resv_seq = gem->ttm_resv_seq;
    bo->ttm_resv_exclusive_fence = gem->ttm_resv_exclusive_fence;
    bo->ttm_resv_owner_id = gem->ttm_resv_owner_id;
    bo->ttm_resv_owner_tgid = gem->ttm_resv_owner_tgid;
    bo->ttm_resv_shared_count = gem->ttm_resv_shared_count;
    bo->ttm_resv_shared_next = gem->ttm_resv_shared_next;
    bo->ttm_resv_waiters = gem->ttm_resv_waiters;
    bo->ttm_resv_wakeup_seq = gem->ttm_resv_wakeup_seq;
    memmove(bo->ttm_resv_shared, gem->ttm_resv_shared,
            sizeof(bo->ttm_resv_shared));
}

static void fb_ttm_resv_note_attach_locked(uint32 attach_point)
{
    switch (attach_point) {
    case FB_GPU_RESV_ATTACH_DMABUF_EXPORT:
        fb_state.stats.ttm_resv_attach_dmabuf_export++;
        break;
    case FB_GPU_RESV_ATTACH_DMABUF_IMPORT:
        fb_state.stats.ttm_resv_attach_dmabuf_import++;
        break;
    case FB_GPU_RESV_ATTACH_PRIME_EXPORT:
        fb_state.stats.ttm_resv_attach_prime_export++;
        break;
    case FB_GPU_RESV_ATTACH_PRIME_IMPORT:
        fb_state.stats.ttm_resv_attach_prime_import++;
        break;
    case FB_GPU_RESV_ATTACH_KMS_PIN:
        fb_state.stats.ttm_resv_attach_kms_pin++;
        break;
    case FB_GPU_RESV_ATTACH_KMS_UNPIN:
        fb_state.stats.ttm_resv_attach_kms_unpin++;
        break;
    case FB_GPU_RESV_ATTACH_SYNCOBJ_SIGNAL:
        fb_state.stats.ttm_resv_attach_syncobj_signal++;
        fb_state.stats.syncobj_resv_attach++;
        break;
    case FB_GPU_RESV_ATTACH_SYNCOBJ_WAIT:
        fb_state.stats.ttm_resv_attach_syncobj_wait++;
        fb_state.stats.syncobj_resv_attach++;
        break;
    case FB_GPU_RESV_ATTACH_SYNC_FILE_EXPORT:
        fb_state.stats.ttm_resv_attach_sync_file_export++;
        fb_state.stats.syncobj_resv_attach++;
        break;
    case FB_GPU_RESV_ATTACH_SYNC_FILE_IMPORT:
        fb_state.stats.ttm_resv_attach_sync_file_import++;
        fb_state.stats.syncobj_resv_attach++;
        break;
    default:
        break;
    }
    fb_state.stats.ttm_resv_last_attach_point = attach_point;
}

static void fb_ttm_resv_mark_stale_locked(void)
{
    fb_state.stats.ttm_resv_stale_fence_rejects++;
    fb_state.stats.ttm_validate_failures++;
}

static void fb_ttm_resv_wakeup_locked(struct fb_gpu_gem_object *gem,
                                      uint32 source)
{
    uint32 waiters;

    if (gem == NULL)
        return;
    waiters = gem->ttm_resv_waiters;
    gem->ttm_resv_wakeup_seq++;
    if (gem->dmabuf_poll_armed != 0) {
        gem->dmabuf_poll_fired = 1;
        gem->dmabuf_poll_last_source = source;
        fb_state.stats.dmabuf_poll_callbacks_fired++;
        fb_state.stats.dmabuf_poll_last_callback_source = source;
        fb_state.stats.dmabuf_poll_last_callback_target_fence =
            gem->dmabuf_poll_target_fence;
        fb_state.stats.dmabuf_poll_last_callback_wakeup_seq =
            gem->ttm_resv_wakeup_seq;
    }
    if (waiters != 0) {
        fb_state.stats.ttm_resv_wait_wakeups += waiters;
        wakeup_on_chan(&gem->ttm_resv_wakeup_seq);
    }
}

static void fb_ttm_resv_note_issued_fence_locked(struct fb_gpu_gem_object *gem,
                                                 uint64 fence)
{
    if (gem != NULL && fence != 0 && gem->last_fence < fence)
        gem->last_fence = fence;
}

static void fb_ttm_resv_signal_bookkeeping_fence_locked(
    struct fb_gpu_gem_object *gem, uint64 fence)
{
    if (gem == NULL || fence == 0)
        return;
    fb_ttm_resv_note_issued_fence_locked(gem, fence);
    if (gem->signaled_fence < fence)
        gem->signaled_fence = fence;
}

static int fb_ttm_resv_owner_explicitly_matches(
    const struct fb_gpu_gem_object *gem, uint64 owner_id, pid_t owner_tgid)
{
    if (gem == NULL || gem->ttm_resv_owner_id == 0)
        return 0;
    if (owner_id != 0)
        return gem->ttm_resv_owner_id == owner_id;
    if (owner_tgid > 0)
        return gem->ttm_resv_owner_tgid == owner_tgid;
    return 0;
}

static void fb_ttm_resv_release_owner_if_last_locked(
    struct fb_gpu_bo_entry *bo)
{
    struct fb_gpu_gem_object *gem;
    int owner_still_has_handle = 0;

    if (bo == NULL || bo->gem == NULL)
        return;
    gem = bo->gem;
    if (!fb_ttm_resv_owner_explicitly_matches(gem, bo->owner_id,
                                              bo->owner_tgid))
        return;
    for (uint32 i = 0; i < FB_GPU_MAX_BOS; i++) {
        struct fb_gpu_bo_entry *peer = &fb_state.bos[i];

        if (peer == bo || !peer->in_use || peer->gem != gem)
            continue;
        if (peer->owner_id == bo->owner_id &&
            peer->owner_tgid == bo->owner_tgid) {
            owner_still_has_handle = 1;
            break;
        }
    }
    if (owner_still_has_handle)
        return;
    gem->ttm_resv_count = 0;
    gem->ttm_resv_owner_id = 0;
    gem->ttm_resv_owner_tgid = 0;
    gem->ttm_resv_seq++;
    gem->ttm_resv_exclusive_fence++;
    fb_ttm_resv_signal_bookkeeping_fence_locked(
        gem, gem->ttm_resv_exclusive_fence);
    fb_state.stats.ttm_resv_releases++;
    fb_state.stats.ttm_resv_exclusive_fences++;
    fb_ttm_resv_wakeup_locked(gem,
                              FB_GPU_DMABUF_POLL_WAKE_EXCLUSIVE_RELEASE);
    fb_ttm_propagate_gem_locked(bo);
}

static int fb_ttm_resv_wait_owner_locked(struct fb_gpu_gem_object *gem,
                                         uint64 owner_id, pid_t owner_tgid)
{
    if (gem == NULL)
        return -EINVAL;
    if (owner_id == 0 && owner_tgid <= 0 &&
        gem->ttm_resv_owner_id != 0) {
        fb_ttm_resv_note_enabled_locked();
        fb_state.stats.ttm_resv_waits++;
        fb_state.stats.ttm_resv_conflicts++;
        fb_state.stats.ttm_validate_failures++;
        return -EBUSY;
    }
    if (!fb_ttm_resv_owner_matches(gem, owner_id, owner_tgid)) {
        fb_ttm_resv_note_enabled_locked();
        fb_state.stats.ttm_resv_waits++;
        fb_state.stats.ttm_resv_conflicts++;
        fb_state.stats.ttm_resv_wait_queued++;
        fb_state.stats.ttm_validate_failures++;
        return -EBUSY;
    }
    return 0;
}

static uint64 fb_ttm_resv_ww_next_stamp_locked(void)
{
    uint64 stamp = fb_state.next_ww_acquire_stamp++;

    if (fb_state.next_ww_acquire_stamp == 0)
        fb_state.next_ww_acquire_stamp = 1;
    if (stamp == 0)
        stamp = fb_state.next_ww_acquire_stamp++;
    return stamp;
}

static void fb_ttm_resv_ww_ctx_init_locked(
    struct fb_gpu_ww_acquire_ctx *ctx, uint64 owner_id, pid_t owner_tgid)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->stamp = fb_ttm_resv_ww_next_stamp_locked();
    ctx->owner_id = owner_id;
    ctx->owner_tgid = owner_tgid;
    fb_ttm_resv_note_enabled_locked();
    fb_state.stats.ttm_resv_ww_contexts++;
}

static int fb_ttm_resv_ww_acquire_one_locked(
    struct fb_gpu_ww_acquire_ctx *ctx, struct fb_gpu_bo_entry *bo)
{
    int ret;

    if (ctx == NULL || bo == NULL || bo->gem == NULL)
        return -EINVAL;
    ret = fb_ttm_reserve_locked(bo, ctx->owner_id, ctx->owner_tgid);
    if (ret != 0) {
        ctx->contended++;
        return ret;
    }
    ctx->acquired++;
    fb_state.stats.ttm_resv_ww_ordered_acquires++;
    if (ctx->acquired > fb_state.stats.ttm_resv_ww_max_acquired)
        fb_state.stats.ttm_resv_ww_max_acquired = ctx->acquired;
    return 0;
}

static void fb_ttm_resv_ww_release_one_locked(
    struct fb_gpu_ww_acquire_ctx *ctx, struct fb_gpu_bo_entry *bo)
{
    if (ctx == NULL || bo == NULL || bo->gem == NULL)
        return;
    if (fb_ttm_unreserve_locked(bo, ctx->owner_id, ctx->owner_tgid) == 0) {
        if (ctx->acquired > 0)
            ctx->acquired--;
        fb_state.stats.ttm_resv_ww_release_balance++;
    }
}

static int fb_ttm_resv_ww_validate_pair_locked(
    struct fb_gpu_bo_entry *bo, struct fb_gpu_bo_entry *peer,
    uint64 owner_id, pid_t owner_tgid)
{
    struct fb_gpu_ww_acquire_ctx ctx;
    struct fb_gpu_bo_entry *first;
    struct fb_gpu_bo_entry *second;
    int reversed;
    int ret;

    if (bo == NULL || peer == NULL || bo == peer ||
        bo->gem == NULL || peer->gem == NULL) {
        fb_state.stats.ttm_resv_ww_validate_failures++;
        fb_state.stats.ttm_validate_failures++;
        return -EINVAL;
    }

    fb_ttm_resv_ww_ctx_init_locked(&ctx, owner_id, owner_tgid);
    reversed = bo->gem->id > peer->gem->id;
    first = reversed ? peer : bo;
    second = reversed ? bo : peer;

    if (reversed) {
        fb_state.stats.ttm_resv_ww_deadlock_retries++;
        fb_state.stats.ttm_resv_ww_wound_backoffs++;
        ctx.retries++;
    }

    ret = fb_ttm_resv_ww_acquire_one_locked(&ctx, first);
    if (ret != 0)
        goto fail;
    ret = fb_ttm_resv_ww_acquire_one_locked(&ctx, second);
    if (ret != 0) {
        fb_ttm_resv_ww_release_one_locked(&ctx, first);
        goto fail;
    }

    fb_state.stats.ttm_resv_ww_multi_object++;
    fb_ttm_resv_ww_release_one_locked(&ctx, second);
    fb_ttm_resv_ww_release_one_locked(&ctx, first);
    return 0;

fail:
    fb_state.stats.ttm_resv_ww_validate_failures++;
    fb_state.stats.ttm_validate_failures++;
    return ret;
}

static void fb_ttm_resv_record_shared_locked(struct fb_gpu_bo_entry *bo,
                                             uint32 attach_point,
                                             uint32 exporter_tag)
{
    struct fb_gpu_gem_object *gem;
    struct fb_gpu_resv_shared_fence *slot;
    uint64 fence;
    uint32 index;

    if (bo == NULL || bo->gem == NULL)
        return;
    gem = bo->gem;
    fence = gem->last_fence != 0 ? gem->last_fence :
        (gem->ttm_resv_exclusive_fence != 0 ?
            gem->ttm_resv_exclusive_fence :
            (gem->ttm_resv_seq + 1 != 0 ? gem->ttm_resv_seq + 1 :
                gem->ttm_reservation_seq));
    if (fence == 0) {
        fb_ttm_resv_mark_stale_locked();
        return;
    }
    fb_ttm_resv_signal_bookkeeping_fence_locked(gem, fence);

    fb_ttm_resv_note_enabled_locked();
    index = gem->ttm_resv_shared_next % FB_GPU_RESV_SHARED_SLOTS;
    if (gem->ttm_resv_shared_count == FB_GPU_RESV_SHARED_SLOTS)
        fb_state.stats.ttm_resv_shared_replaced++;
    else
        gem->ttm_resv_shared_count++;
    gem->ttm_resv_shared_next =
        (index + 1) % FB_GPU_RESV_SHARED_SLOTS;

    slot = &gem->ttm_resv_shared[index];
    memset(slot, 0, sizeof(*slot));
    slot->seq = ++gem->ttm_resv_seq;
    slot->fence = fence;
    slot->owner_id = bo->owner_id;
    slot->owner_tgid = bo->owner_tgid;
    slot->attach_point = attach_point;
    slot->exporter_tag = exporter_tag;

    fb_state.stats.ttm_resv_shared_fences++;
    fb_state.stats.ttm_resv_shared_used = gem->ttm_resv_shared_count;
    fb_state.stats.ttm_resv_last_shared_fence = fence;
    fb_state.stats.dmabuf_last_ttm_resv_shared_fence = fence;
    fb_state.stats.dmabuf_last_ttm_resv_shared_count =
        gem->ttm_resv_shared_count;
    fb_ttm_resv_note_attach_locked(attach_point);
    fb_ttm_resv_wakeup_locked(gem,
                              FB_GPU_DMABUF_POLL_WAKE_SHARED_ATTACH);
    fb_ttm_resv_sync_bo_from_gem_locked(bo, gem);
    fb_ttm_propagate_gem_locked(bo);
}

static uint64 fb_ttm_resv_record_owner_locked(uint64 owner_id,
                                              pid_t owner_tgid,
                                              uint32 attach_point,
                                              uint32 exporter_tag)
{
    uint64 attached = 0;

    for (uint32 i = 0; i < FB_GPU_MAX_BOS; i++) {
        struct fb_gpu_bo_entry *bo = &fb_state.bos[i];

        if (!bo->in_use || bo->gem == NULL ||
            !fb_bo_owner_matches(bo, owner_id, owner_tgid))
            continue;
        fb_ttm_resv_record_shared_locked(bo, attach_point, exporter_tag);
        attached++;
    }
    return attached;
}

static int fb_ttm_resv_wait_fence_locked(struct fb_gpu_gem_object *gem,
                                         uint64 target)
{
    int ret;

    if (gem == NULL || target == 0)
        return 0;
    fb_ttm_resv_note_enabled_locked();
    if (target > gem->last_fence) {
        fb_ttm_resv_mark_stale_locked();
        return -EAGAIN;
    }
    while (gem->signaled_fence < target) {
        fb_state.stats.ttm_resv_waits++;
        fb_state.stats.ttm_resv_wait_queued++;
        gem->ttm_resv_waiters++;
        ret = sleep_on_chan_interruptible(&gem->ttm_resv_wakeup_seq,
                                          &fb_state.lock);
        if (gem->ttm_resv_waiters > 0)
            gem->ttm_resv_waiters--;
        if (ret != 0)
            return ret;
    }
    return 0;
}

static int fb_ttm_resv_check_locked(struct fb_gpu_bo_entry *bo,
                                    uint64 owner_id, pid_t owner_tgid)
{
    if (bo == NULL || bo->gem == NULL)
        return -EINVAL;
    return fb_ttm_resv_wait_owner_locked(bo->gem, owner_id, owner_tgid);
}

static int fb_ttm_reserve_locked(struct fb_gpu_bo_entry *bo,
                                 uint64 owner_id, pid_t owner_tgid)
{
    struct fb_gpu_gem_object *gem;
    int ret;

    if (bo == NULL || bo->gem == NULL)
        return -EINVAL;
    gem = bo->gem;
    ret = fb_ttm_resv_wait_owner_locked(gem, owner_id, owner_tgid);
    if (ret != 0)
        return ret;
    gem->ttm_resv_owner_id = owner_id;
    gem->ttm_resv_owner_tgid = owner_tgid;
    gem->ttm_resv_count++;
    gem->ttm_resv_seq++;
    gem->ttm_resv_exclusive_fence++;
    fb_ttm_resv_signal_bookkeeping_fence_locked(
        gem, gem->ttm_resv_exclusive_fence);
    bo->ttm_resv_count = gem->ttm_resv_count;
    bo->ttm_resv_seq = gem->ttm_resv_seq;
    bo->ttm_resv_exclusive_fence = gem->ttm_resv_exclusive_fence;
    bo->ttm_resv_owner_id = gem->ttm_resv_owner_id;
    bo->ttm_resv_owner_tgid = gem->ttm_resv_owner_tgid;
    fb_state.stats.ttm_resv_acquires++;
    fb_state.stats.ttm_resv_exclusive_fences++;
    fb_ttm_resv_wakeup_locked(gem,
                              FB_GPU_DMABUF_POLL_WAKE_EXCLUSIVE_ACQUIRE);
    fb_ttm_propagate_gem_locked(bo);
    return 0;
}

static int fb_ttm_unreserve_locked(struct fb_gpu_bo_entry *bo,
                                   uint64 owner_id, pid_t owner_tgid)
{
    struct fb_gpu_gem_object *gem;

    if (bo == NULL || bo->gem == NULL)
        return -EINVAL;
    gem = bo->gem;
    if (gem->ttm_resv_count == 0 ||
        !fb_ttm_resv_owner_matches(gem, owner_id, owner_tgid)) {
        fb_state.stats.ttm_validate_failures++;
        return -EINVAL;
    }
    gem->ttm_resv_count--;
    gem->ttm_resv_seq++;
    gem->ttm_resv_exclusive_fence++;
    fb_ttm_resv_signal_bookkeeping_fence_locked(
        gem, gem->ttm_resv_exclusive_fence);
    if (gem->ttm_resv_count == 0) {
        gem->ttm_resv_owner_id = 0;
        gem->ttm_resv_owner_tgid = 0;
    }
    bo->ttm_resv_count = gem->ttm_resv_count;
    bo->ttm_resv_seq = gem->ttm_resv_seq;
    bo->ttm_resv_exclusive_fence = gem->ttm_resv_exclusive_fence;
    bo->ttm_resv_owner_id = gem->ttm_resv_owner_id;
    bo->ttm_resv_owner_tgid = gem->ttm_resv_owner_tgid;
    fb_state.stats.ttm_resv_releases++;
    fb_state.stats.ttm_resv_exclusive_fences++;
    fb_ttm_resv_wakeup_locked(gem,
                              FB_GPU_DMABUF_POLL_WAKE_EXCLUSIVE_RELEASE);
    fb_ttm_propagate_gem_locked(bo);
    return 0;
}

static void fb_ttm_propagate_gem_locked(struct fb_gpu_bo_entry *bo)
{
    struct fb_gpu_gem_object *gem;

    if (bo == NULL || bo->gem == NULL)
        return;
    gem = bo->gem;
    gem->ttm_mem_type = bo->ttm_mem_type;
    gem->ttm_placement = bo->ttm_placement;
    gem->ttm_pin_count = bo->ttm_pin_count;
    gem->ttm_reservation_seq = bo->ttm_reservation_seq;
    gem->ttm_tt_populated = bo->ttm_tt_populated;
    gem->ttm_sg_nents = bo->ttm_sg_nents;
    gem->ttm_dma_addr_base = bo->ttm_dma_addr_base;
    gem->sgt = bo->sgt;
    gem->ttm_lru_seq = bo->ttm_lru_seq;
    gem->ttm_move_count = bo->ttm_move_count;
    gem->nouveau_domain = bo->nouveau_domain;
    gem->nouveau_tile_mode = bo->nouveau_tile_mode;
    gem->nouveau_tile_flags = bo->nouveau_tile_flags;

    for (int i = 0; i < FB_GPU_MAX_BOS; i++) {
        struct fb_gpu_bo_entry *peer = &fb_state.bos[i];

        if (!peer->in_use || peer->gem != gem)
            continue;
        peer->ttm_mem_type = gem->ttm_mem_type;
        peer->ttm_placement = gem->ttm_placement;
        peer->ttm_pin_count = gem->ttm_pin_count;
        peer->ttm_reservation_seq = gem->ttm_reservation_seq;
        peer->ttm_tt_populated = gem->ttm_tt_populated;
        peer->ttm_sg_nents = gem->ttm_sg_nents;
        peer->ttm_dma_addr_base = gem->ttm_dma_addr_base;
        peer->sgt = gem->sgt;
        peer->ttm_lru_seq = gem->ttm_lru_seq;
        peer->ttm_move_count = gem->ttm_move_count;
        peer->ttm_resv_count = gem->ttm_resv_count;
        peer->ttm_resv_seq = gem->ttm_resv_seq;
        peer->ttm_resv_exclusive_fence = gem->ttm_resv_exclusive_fence;
        peer->ttm_resv_owner_id = gem->ttm_resv_owner_id;
        peer->ttm_resv_owner_tgid = gem->ttm_resv_owner_tgid;
        peer->ttm_resv_shared_count = gem->ttm_resv_shared_count;
        peer->ttm_resv_shared_next = gem->ttm_resv_shared_next;
        peer->ttm_resv_waiters = gem->ttm_resv_waiters;
        peer->ttm_resv_wakeup_seq = gem->ttm_resv_wakeup_seq;
        memmove(peer->ttm_resv_shared, gem->ttm_resv_shared,
                sizeof(peer->ttm_resv_shared));
        peer->nouveau_domain = gem->nouveau_domain;
        peer->nouveau_tile_mode = gem->nouveau_tile_mode;
        peer->nouveau_tile_flags = gem->nouveau_tile_flags;
    }
}

static void fb_ttm_set_metadata_locked(struct fb_gpu_bo_entry *bo)
{
    if (bo == NULL)
        return;
    bo->ttm_tt_populated = bo->ttm_mem_type == FB_TTM_MEM_TT;
    fb_sg_table_build(&bo->sgt, bo->pages, bo->npages, bo->size);
    bo->ttm_sg_nents = bo->sgt.nents;
    bo->ttm_dma_addr_base = bo->sgt.dma_addr_base;
}

static void fb_ttm_move_bo_locked(struct fb_gpu_bo_entry *bo,
                                  uint32 placement, uint32 mem_type)
{
    struct fb_ttm_resource_manager *mgr;
    uint32 old_mem_type;

    if (bo == NULL)
        return;
    old_mem_type = bo->ttm_mem_type;
    if (mem_type != old_mem_type) {
        if (bo->stats_accounted) {
            fb_ttm_account_locked(old_mem_type, bo->size, 0);
            fb_ttm_account_locked(mem_type, bo->size, 1);
        }
        bo->ttm_mem_type = mem_type;
        bo->ttm_move_count++;
        mgr = fb_ttm_manager_locked(mem_type);
        mgr->moves++;
        fb_state.stats.ttm_metadata_only_moves++;
        fb_state.stats.ttm_move_bytes += bo->size;
    }
    fb_ttm_note_move_diag_locked(old_mem_type, mem_type);
    bo->ttm_placement = placement;
    bo->ttm_reservation_seq++;
    bo->ttm_lru_seq = ++fb_state.ttm_lru_clock;
    fb_ttm_set_metadata_locked(bo);
    mgr = fb_ttm_manager_locked(bo->ttm_mem_type);
    mgr->lru_seq = bo->ttm_lru_seq;
    fb_ttm_propagate_gem_locked(bo);
}

static int fb_ttm_evict_one_locked(uint32 mem_type, uint64 owner_id,
                                   pid_t owner_tgid)
{
    struct fb_gpu_bo_entry *victim = NULL;
    struct fb_ttm_resource_manager *mgr;
    uint32 busy = 0;
    uint32 pinned = 0;

    if (mem_type == FB_TTM_MEM_SYSTEM)
        return 0;
    for (int i = 0; i < FB_GPU_MAX_BOS; i++) {
        struct fb_gpu_bo_entry *bo = &fb_state.bos[i];

        if (!bo->in_use || bo->ttm_mem_type != mem_type ||
            !bo->stats_accounted)
            continue;
        if (bo->ttm_pin_count != 0) {
            pinned++;
            continue;
        }
        if (bo->gem != NULL &&
            !fb_ttm_resv_owner_matches(bo->gem, owner_id, owner_tgid)) {
            busy++;
            continue;
        }
        if (victim == NULL || bo->ttm_lru_seq < victim->ttm_lru_seq)
            victim = bo;
    }
    if (victim == NULL) {
        fb_state.stats.ttm_resv_evict_pinned_rejects += pinned;
        fb_state.stats.ttm_resv_evict_busy_rejects += busy;
        fb_state.stats.ttm_validate_failures++;
        return -ENOENT;
    }

    fb_ttm_move_bo_locked(victim, FB_TTM_PL_SYSTEM, FB_TTM_MEM_SYSTEM);
    mgr = fb_ttm_manager_locked(mem_type);
    mgr->evictions++;
    fb_state.ttm_evictions++;
    return 0;
}

static int fb_ttm_make_room_locked(struct fb_gpu_bo_entry *bo, uint32 mem_type,
                                   uint64 owner_id, pid_t owner_tgid)
{
    struct fb_ttm_resource_manager *mgr = fb_ttm_manager_locked(mem_type);

    if (bo == NULL || !bo->stats_accounted)
        return 0;
    if (mem_type == bo->ttm_mem_type || mgr->limit == (uint64)-1)
        return 0;
    for (int tries = 0; tries < FB_GPU_MAX_BOS &&
         mgr->used + bo->size > mgr->limit; tries++) {
        int ret = fb_ttm_evict_one_locked(mem_type, owner_id, owner_tgid);

        if (ret != 0)
            return ret;
    }
    if (mgr->used + bo->size > mgr->limit) {
        fb_state.stats.ttm_validate_failures++;
        return -ENOSPC;
    }
    return 0;
}

static int fb_ttm_validate_placement_locked(struct fb_gpu_bo_entry *bo,
                                            uint32 placement,
                                            uint64 owner_id,
                                            pid_t owner_tgid)
{
    uint32 mem_type;
    int ret;

    if (bo == NULL || !bo->in_use || !fb_ttm_valid_placement(placement)) {
        fb_state.stats.ttm_validate_failures++;
        return -EINVAL;
    }
    ret = fb_ttm_resv_check_locked(bo, owner_id, owner_tgid);
    if (ret != 0)
        return ret;
    mem_type = fb_ttm_mem_type_for_placement(placement);
    ret = fb_ttm_make_room_locked(bo, mem_type, owner_id, owner_tgid);
    if (ret != 0)
        return ret;
    fb_ttm_move_bo_locked(bo, placement, mem_type);
    return 0;
}

static int fb_ttm_pin_locked(struct fb_gpu_bo_entry *bo, uint64 owner_id,
                             pid_t owner_tgid)
{
    int ret = fb_ttm_resv_check_locked(bo, owner_id, owner_tgid);

    if (ret != 0)
        return ret;
    if (bo == NULL || !bo->in_use)
        return -EINVAL;
    if (bo->ttm_pin_count == 0)
        fb_state.stats.ttm_pinned_bytes += bo->size;
    bo->ttm_pin_count++;
    bo->ttm_reservation_seq++;
    bo->ttm_lru_seq = ++fb_state.ttm_lru_clock;
    fb_ttm_propagate_gem_locked(bo);
    return 0;
}

static int fb_ttm_unpin_locked(struct fb_gpu_bo_entry *bo, uint64 owner_id,
                               pid_t owner_tgid)
{
    int ret = fb_ttm_resv_check_locked(bo, owner_id, owner_tgid);

    if (ret != 0)
        return ret;
    if (bo == NULL || !bo->in_use || bo->ttm_pin_count == 0) {
        fb_state.stats.ttm_validate_failures++;
        return -EINVAL;
    }
    bo->ttm_pin_count--;
    if (bo->ttm_pin_count == 0) {
        if (fb_state.stats.ttm_pinned_bytes >= bo->size)
            fb_state.stats.ttm_pinned_bytes -= bo->size;
        else
            fb_state.stats.ttm_pinned_bytes = 0;
    }
    bo->ttm_reservation_seq++;
    bo->ttm_lru_seq = ++fb_state.ttm_lru_clock;
    fb_ttm_propagate_gem_locked(bo);
    return 0;
}

static void fb_ttm_fill_validate_locked(struct fb_gpu_ttm_validate *req,
                                        const struct fb_gpu_bo_entry *bo)
{
    struct fb_gpu_sg_entry first;

    req->placement = bo->ttm_placement;
    req->mem_type = bo->ttm_mem_type;
    req->pin_count = bo->ttm_pin_count;
    req->tt_populated = bo->ttm_tt_populated;
    req->sg_nents = bo->ttm_sg_nents;
    req->peer_handle = 0;
    req->size = bo->size;
    req->dma_addr_base = bo->ttm_dma_addr_base;
    fb_sg_table_first_entry(&bo->sgt, &first);
    req->sg_total_len = bo->sgt.total_len;
    req->sg_dma_addr_first = first.dma_addr;
    req->sg_dma_addr_last = bo->sgt.dma_addr_last;
    req->reservation_seq = bo->ttm_reservation_seq;
    req->lru_seq = bo->ttm_lru_seq;
    req->move_count = bo->ttm_move_count;
    for (uint32 i = 0; i < 4; i++)
        req->manager_bytes[i] = fb_ttm_manager_locked(i)->used;
    req->evictions = fb_state.ttm_evictions;
    req->metadata_only_moves = fb_state.stats.ttm_metadata_only_moves;
    req->real_copy_moves = fb_state.stats.ttm_real_copy_moves;
    req->move_bytes = fb_state.stats.ttm_move_bytes;
    req->native_accel_credit = fb_state.stats.ttm_native_accel_credit;
    for (uint32 i = 0; i < 4; i++) {
        struct fb_ttm_resource_manager *mgr = fb_ttm_manager_locked(i);

        req->manager_moves[i] = mgr->moves;
        req->cpu_copy_fallback_moves[i] = mgr->cpu_copy_fallback_moves;
        req->metadata_noop_moves[i] = mgr->metadata_noop_moves;
        req->unsupported_hw_copy_moves[i] =
            mgr->unsupported_hw_copy_moves;
        req->real_copy_moves_by_domain[i] = mgr->real_copy_moves;
    }
    req->resv_count = bo->gem != NULL ? bo->gem->ttm_resv_count : 0;
    req->resv_seq = bo->gem != NULL ? bo->gem->ttm_resv_seq : 0;
    req->resv_exclusive_fence =
        bo->gem != NULL ? bo->gem->ttm_resv_exclusive_fence : 0;
    req->resv_waits = fb_state.stats.ttm_resv_waits;
    req->resv_conflicts = fb_state.stats.ttm_resv_conflicts;
    req->resv_shared_slots = FB_GPU_RESV_SHARED_SLOTS;
    req->resv_shared_count =
        bo->gem != NULL ? bo->gem->ttm_resv_shared_count : 0;
    req->resv_latest_shared_fence =
        bo->gem != NULL ? fb_ttm_resv_latest_shared_fence_locked(bo->gem) : 0;
    req->resv_wait_wakeups = fb_state.stats.ttm_resv_wait_wakeups;
    req->resv_stale_fence_rejects =
        fb_state.stats.ttm_resv_stale_fence_rejects;
    req->resv_attach_syncobj_signal =
        fb_state.stats.ttm_resv_attach_syncobj_signal;
    req->resv_attach_syncobj_wait =
        fb_state.stats.ttm_resv_attach_syncobj_wait;
    req->resv_attach_sync_file_export =
        fb_state.stats.ttm_resv_attach_sync_file_export;
    req->resv_attach_sync_file_import =
        fb_state.stats.ttm_resv_attach_sync_file_import;
    req->resv_evict_pinned_rejects =
        fb_state.stats.ttm_resv_evict_pinned_rejects;
    req->resv_evict_busy_rejects =
        fb_state.stats.ttm_resv_evict_busy_rejects;
    req->syncobj_wait_queued = fb_state.stats.syncobj_wait_queued;
    req->syncobj_wait_wakeups = fb_state.stats.syncobj_wait_wakeups;
    req->syncobj_timeout_waits = fb_state.stats.syncobj_timeout_waits;
    req->syncobj_stale_wait_rejects =
        fb_state.stats.syncobj_stale_wait_rejects;
}

static int fb_bo_set_ttm_placement(uint32 handle, uint64 owner_id,
                                   pid_t owner_tgid, uint32 placement)
{
    struct fb_gpu_bo_entry *bo;
    int ret;

    spin_lock(&fb_state.lock);
    bo = fb_bo_lookup_owned_locked(handle, owner_id, owner_tgid);
    if (bo == NULL) {
        fb_state.stats.ttm_validate_failures++;
        spin_unlock(&fb_state.lock);
        return -ENOENT;
    }
    if (!fb_bo_owner_matches(bo, owner_id, owner_tgid)) {
        fb_state.stats.ttm_validate_failures++;
        spin_unlock(&fb_state.lock);
        return -EPERM;
    }
    ret = fb_ttm_validate_placement_locked(bo, placement, owner_id,
                                           owner_tgid);
    spin_unlock(&fb_state.lock);
    return ret;
}

static int fb_bo_set_nouveau_metadata(uint32 handle, uint64 owner_id,
                                      pid_t owner_tgid, uint32 domain,
                                      uint32 tile_mode, uint32 tile_flags)
{
    struct fb_gpu_bo_entry *bo;

    spin_lock(&fb_state.lock);
    bo = fb_bo_lookup_owned_locked(handle, owner_id, owner_tgid);
    if (bo == NULL) {
        spin_unlock(&fb_state.lock);
        return -ENOENT;
    }
    if (!fb_bo_owner_matches(bo, owner_id, owner_tgid)) {
        spin_unlock(&fb_state.lock);
        return -EPERM;
    }
    bo->nouveau_domain = domain;
    bo->nouveau_tile_mode = tile_mode;
    bo->nouveau_tile_flags = tile_flags;
    bo->ttm_reservation_seq++;
    fb_ttm_propagate_gem_locked(bo);
    spin_unlock(&fb_state.lock);
    return 0;
}

static void fb_bo_put(struct fb_gpu_bo_entry *bo)
{
    struct fb_gpu_gem_object *gem = NULL;

    if (bo == NULL)
        return;

    spin_lock(&fb_state.lock);
    if (bo->refs > 0)
        bo->refs--;
    if (bo->dead && bo->refs == 0) {
        gem = bo->gem;
        memset(bo, 0, sizeof(*bo));
    }
    spin_unlock(&fb_state.lock);

    fb_gem_put(gem);
}

static struct fb_gpu_bo_entry *fb_bo_get_owned(uint32 handle, uint64 owner_id,
                                               pid_t owner_tgid)
{
    struct fb_gpu_bo_entry *bo;

    spin_lock(&fb_state.lock);
    bo = fb_bo_lookup_owned_locked(handle, owner_id, owner_tgid);
    if (bo != NULL)
        bo->refs++;
    spin_unlock(&fb_state.lock);
    return bo;
}

static int fb_read_current_kms_framebuffer(uint32 x, uint32 y, uint32 w,
                                           uint32 h, void *dst,
                                           uint32 dst_pitch,
                                           uint32 *screen_width,
                                           uint32 *screen_height,
                                           uint32 *screen_pitch)
{
    struct fb_gpu_kms_fb_entry fb_snapshot;
    struct fb_gpu_bo_entry *bo;
    uint64 base_offset;
    int swap_rb;
    static int read_logs;

    if (dst == NULL || w == 0 || h == 0 || dst_pitch < w * sizeof(uint32))
        return -EINVAL;

    memset(&fb_snapshot, 0, sizeof(fb_snapshot));
    spin_lock(&fb_state.lock);
    if (fb_state.current_kms_fb_id != 0) {
        for (uint32 i = 0; i < FB_GPU_MAX_KMS_FBS; i++) {
            struct fb_gpu_kms_fb_entry *fb = &fb_state.kms_fbs[i];

            if (fb->in_use && fb->fb_id == fb_state.current_kms_fb_id) {
                fb_snapshot = *fb;
                break;
            }
        }
    }
    spin_unlock(&fb_state.lock);

    if (!fb_snapshot.in_use || fb_snapshot.bo_handle == 0 ||
        fb_snapshot.width == 0 || fb_snapshot.height == 0 ||
        fb_snapshot.pitch == 0)
        return -ENOENT;
    if (!fb_scanout_format_supported(fb_snapshot.pixel_format,
                                     fb_snapshot.modifier))
        return -EOPNOTSUPP;
    if (x > fb_snapshot.width || w > fb_snapshot.width - x ||
        y > fb_snapshot.height || h > fb_snapshot.height - y)
        return -EINVAL;

    bo = fb_bo_get_owned(fb_snapshot.bo_handle, fb_snapshot.owner_id,
                         fb_snapshot.owner_tgid);
    if (bo == NULL)
        return -ENOENT;
    if (bo->pages == NULL || bo->npages == 0 ||
        fb_snapshot.pitch < fb_snapshot.width * sizeof(uint32)) {
        fb_bo_put(bo);
        return -EINVAL;
    }

    base_offset = fb_snapshot.offsets[0] + (uint64)y * fb_snapshot.pitch +
                  (uint64)x * sizeof(uint32);
    if (base_offset >= bo->size ||
        base_offset + (uint64)(h - 1) * fb_snapshot.pitch +
            (uint64)w * sizeof(uint32) > bo->size) {
        fb_bo_put(bo);
        return -EINVAL;
    }

    if (bo->virtio_resource_id != 0) {
        struct fb_gpu_virgl_transfer transfer;
        uint64 row_off;
        uint32 src_y;
        uint32 src_x;

        src_y = (uint32)(base_offset / fb_snapshot.pitch);
        row_off = base_offset - (uint64)src_y * fb_snapshot.pitch;
        src_x = (uint32)(row_off / sizeof(uint32));
        if ((row_off & 3) == 0 && src_x <= bo->width &&
            w <= bo->width - src_x && src_y <= bo->height &&
            h <= bo->height - src_y) {
            uint64 resource_owner_id = bo->virtio_resource_owner_id ?
                bo->virtio_resource_owner_id : bo->owner_id;
            pid_t resource_owner_tgid = bo->virtio_resource_owner_tgid ?
                bo->virtio_resource_owner_tgid : bo->owner_tgid;

            memset(&transfer, 0, sizeof(transfer));
            transfer.resource_id = bo->virtio_resource_id;
            transfer.x = src_x;
            transfer.y = src_y;
            transfer.z = 0;
            transfer.w = w;
            transfer.h = h;
            transfer.d = 1;
            transfer.level = 0;
            transfer.offset = base_offset;
            transfer.stride = fb_snapshot.pitch;
            transfer.layer_stride =
                (uint64)fb_snapshot.pitch * fb_snapshot.height;
            (void)virtio_gpu_user_transfer(resource_owner_id,
                                           resource_owner_tgid,
                                           &transfer, 1);
        }
    }

    swap_rb = fb_scanout_format_needs_rb_swap(fb_snapshot.pixel_format);
    for (uint32 row = 0; row < h; row++) {
        uint64 src_off = base_offset + (uint64)row * fb_snapshot.pitch;
        uint8 *dst_row = (uint8 *)dst + (uint64)row * dst_pitch;
        uint32 remaining = w * sizeof(uint32);
        uint32 copied = 0;

        while (remaining > 0) {
            uint32 page_idx = src_off / PGSIZE;
            uint32 page_off = src_off & (PGSIZE - 1);
            uint32 chunk = PGSIZE - page_off;
            uint8 *src;

            if (page_idx >= bo->npages || bo->pages[page_idx] == NULL) {
                fb_bo_put(bo);
                return -EINVAL;
            }
            if (chunk > remaining)
                chunk = remaining;
            src = (uint8 *)PA2VA(__page_to_pa(bo->pages[page_idx])) +
                  page_off;
            fb_copy_scanout_chunk(dst_row + copied, src, chunk, swap_rb);
            src_off += chunk;
            copied += chunk;
            remaining -= chunk;
        }
    }

    if (screen_width != NULL)
        *screen_width = fb_snapshot.width;
    if (screen_height != NULL)
        *screen_height = fb_snapshot.height;
    if (screen_pitch != NULL)
        *screen_pitch = fb_snapshot.pitch;
    if (fb_cmdline_enabled("virtio_gpu_scanout_read_diag") &&
        read_logs < 12) {
        printf("FB: scanout-read used current KMS fb=%u bo=%u resource=%u rect=%u,%u %ux%u\n",
               fb_snapshot.fb_id, fb_snapshot.bo_handle,
               bo->virtio_resource_id, x, y, w, h);
        read_logs++;
    }
    fb_bo_put(bo);
    return 0;
}

static void *fb_bo_page_for_owner(uint32 handle, uint64 owner_id,
                                  pid_t owner_tgid, uint64 page_index)
{
    struct fb_gpu_render_owner *owner;
    struct fb_gpu_bo_entry *bo;
    void *pa = NULL;
    int owner_handle = 0;

    spin_lock(&fb_state.lock);
    owner = fb_gpu_render_owner_lookup_locked(owner_id, owner_tgid);
    if (owner != NULL) {
        bo = fb_bo_owner_handle_lookup_locked(owner, handle);
        owner_handle = bo != NULL;
    } else {
        bo = fb_bo_lookup_owned_locked(handle, owner_id, owner_tgid);
    }
    if (bo != NULL &&
        (owner_handle || fb_bo_owner_matches(bo, owner_id, owner_tgid)) &&
        page_index < bo->npages && bo->pages[page_index] != NULL) {
        pa = (void *)__page_to_pa(bo->pages[page_index]);
        if (page_ref_inc(pa) <= 0)
            pa = NULL;
    }
    spin_unlock(&fb_state.lock);
    return pa;
}

static int fb_bo_register(uint64 owner_id, pid_t owner_tgid,
                          uint32 width, uint32 height, uint32 pitch,
                          uint64 size, page_t **pages, uint32 npages,
                          uint32 *handle)
{
    struct fb_gpu_gem_object *gem;
    struct fb_gpu_render_owner *owner;

    spin_lock(&fb_state.lock);
    struct fb_gpu_bo_entry *bo = NULL;
    for (int i = 0; i < FB_GPU_MAX_BOS; i++) {
        if (!fb_state.bos[i].in_use) {
            bo = &fb_state.bos[i];
            break;
        }
    }
    if (bo == NULL) {
        fb_state.stats.rejected_blits++;
        spin_unlock(&fb_state.lock);
        return -ENOSPC;
    }

    gem = fb_gem_alloc_locked(width, height, pitch, size, pages, npages);
    if (gem == NULL) {
        fb_state.stats.rejected_blits++;
        spin_unlock(&fb_state.lock);
        return -ENOSPC;
    }

    uint32 next = fb_state.next_bo_handle++;
    if (fb_state.next_bo_handle == 0)
        fb_state.next_bo_handle = 1;
    memset(bo, 0, sizeof(*bo));
    bo->in_use = 1;
    bo->handle = next;
    bo->stats_accounted = 1;
    bo->owner_id = owner_id;
    bo->owner_tgid = owner_tgid;
    bo->refs = 1;
    fb_gem_copy_to_bo_locked(bo, gem);
    fb_state.stats.bo_handles++;
    fb_state.stats.bo_live_bytes += size;
    fb_ttm_account_locked(bo->ttm_mem_type, size, 1);
    if (fb_state.stats.bo_handles > fb_state.stats.bo_peak_handles)
        fb_state.stats.bo_peak_handles = fb_state.stats.bo_handles;
    if (fb_state.stats.bo_live_bytes > fb_state.stats.bo_peak_bytes)
        fb_state.stats.bo_peak_bytes = fb_state.stats.bo_live_bytes;
    owner = fb_gpu_render_owner_lookup_locked(owner_id, owner_tgid);
    if (owner != NULL) {
        int ret = fb_bo_owner_add_handle_locked(owner, bo, handle, 0);
        if (ret != 0) {
            bo->in_use = 0;
            bo->dead = 1;
            fb_state.stats.bo_handles--;
            if (bo->stats_accounted &&
                fb_state.stats.bo_live_bytes >= bo->size)
                fb_state.stats.bo_live_bytes -= bo->size;
            else if (bo->stats_accounted)
                fb_state.stats.bo_live_bytes = 0;
            fb_ttm_account_locked(bo->ttm_mem_type, bo->size, 0);
            spin_unlock(&fb_state.lock);
            fb_bo_put(bo);
            return ret;
        }
    } else {
        *handle = next;
    }
    spin_unlock(&fb_state.lock);
    return 0;
}

static int fb_bo_owner_has_owner0_import_wrapper_locked(
    struct fb_gpu_render_owner *owner)
{
    if (owner == NULL)
        return 0;

    for (int i = 0; i < FB_GPU_MAX_BOS; i++) {
        struct fb_gpu_bo_entry *bo;
        struct fb_gpu_gem_object *gem;

        if (!owner->bo_handles[i].in_use ||
            owner->bo_handles[i].borrowed)
            continue;
        bo = owner->bo_handles[i].bo;
        if (bo == NULL || !bo->in_use || bo->dead ||
            bo->owner_id != owner->id || bo->owner_tgid != owner->tgid)
            continue;
        gem = bo->gem;
        if (gem != NULL && gem->virtio_resource_owner_id == 0 &&
            gem->virtio_resource_owner_tgid == owner->tgid)
            return 1;
    }
    return 0;
}

static int fb_bo_register_gem(uint64 owner_id, pid_t owner_tgid,
                              struct fb_gpu_gem_object *gem,
                              uint32 *handle)
{
    struct fb_gpu_bo_entry *bo = NULL;
    struct fb_gpu_render_owner *owner;
    uint32 next;

    if (gem == NULL || handle == NULL)
        return -EINVAL;

    spin_lock(&fb_state.lock);
    if (!gem->in_use || gem->dead) {
        spin_unlock(&fb_state.lock);
        return -ENOENT;
    }
    owner = fb_gpu_render_owner_lookup_locked(owner_id, owner_tgid);
    if (owner != NULL &&
        fb_bo_owner_has_owner0_import_wrapper_locked(owner)) {
        for (int i = 0; i < FB_GPU_MAX_BOS; i++) {
            struct fb_gpu_bo_entry *candidate = &fb_state.bos[i];

            if (!candidate->in_use || candidate->dead ||
                candidate->gem != gem)
                continue;
            if (candidate->owner_id != 0 || candidate->owner_tgid <= 0)
                continue;
            if (gem->virtio_resource_owner_id != 0)
                continue;
            int ret = fb_bo_owner_add_handle_locked(owner, candidate,
                                                    handle, 1);
            spin_unlock(&fb_state.lock);
            return ret;
        }
    }
    for (int i = 0; i < FB_GPU_MAX_BOS; i++) {
        if (!fb_state.bos[i].in_use) {
            bo = &fb_state.bos[i];
            break;
        }
    }
    if (bo == NULL) {
        fb_state.stats.rejected_blits++;
        spin_unlock(&fb_state.lock);
        return -ENOSPC;
    }

    next = fb_state.next_bo_handle++;
    if (fb_state.next_bo_handle == 0)
        fb_state.next_bo_handle = 1;
    fb_gem_get_locked(gem);
    memset(bo, 0, sizeof(*bo));
    bo->in_use = 1;
    bo->handle = next;
    bo->owner_id = owner_id;
    bo->owner_tgid = owner_tgid;
    bo->refs = 1;
    fb_gem_copy_to_bo_locked(bo, gem);
    fb_state.stats.bo_handles++;
    if (fb_state.stats.bo_handles > fb_state.stats.bo_peak_handles)
        fb_state.stats.bo_peak_handles = fb_state.stats.bo_handles;
    if (owner != NULL) {
        int ret = fb_bo_owner_add_handle_locked(owner, bo, handle, 0);
        if (ret != 0) {
            bo->in_use = 0;
            bo->dead = 1;
            fb_state.stats.bo_handles--;
            spin_unlock(&fb_state.lock);
            fb_bo_put(bo);
            return ret;
        }
    } else {
        *handle = next;
    }
    spin_unlock(&fb_state.lock);
    return 0;
}

static int fb_gem_name_in_use_locked(uint32 name)
{
    if (name == 0)
        return 1;
    for (int i = 0; i < FB_GPU_MAX_BOS; i++) {
        struct fb_gpu_gem_object *gem = &fb_state.gems[i];
        if (gem->in_use && !gem->dead && gem->flink_name == name)
            return 1;
    }
    return 0;
}

static int fb_gem_flink(uint32 handle, uint64 owner_id, pid_t owner_tgid,
                        uint32 *name_out)
{
    struct fb_gpu_bo_entry *bo;
    int ret = 0;

    if (handle == 0 || name_out == NULL)
        return -EINVAL;
    bo = fb_bo_get_owned(handle, owner_id, owner_tgid);
    if (bo == NULL)
        return -ENOENT;

    spin_lock(&fb_state.lock);
    if (bo->gem == NULL || !bo->gem->in_use || bo->gem->dead) {
        ret = -ENOENT;
    } else if (bo->gem->flink_name == 0) {
        uint32 name = 0;
        for (int attempt = 0; attempt < FB_GPU_MAX_BOS + 1; attempt++) {
            name = fb_state.next_flink_name++;
            if (fb_state.next_flink_name == 0)
                fb_state.next_flink_name = 1;
            if (!fb_gem_name_in_use_locked(name))
                break;
            name = 0;
        }
        if (name == 0)
            ret = -ENOSPC;
        else
            bo->gem->flink_name = name;
    }
    if (ret == 0)
        *name_out = bo->gem->flink_name;
    spin_unlock(&fb_state.lock);

    fb_bo_put(bo);
    return ret;
}

static int fb_gem_open_name(uint64 owner_id, pid_t owner_tgid, uint32 name,
                            uint32 *handle_out, uint64 *size_out)
{
    struct fb_gpu_gem_object *gem = NULL;
    uint64 size = 0;
    int ret;

    if (name == 0 || handle_out == NULL || size_out == NULL)
        return -EINVAL;

    spin_lock(&fb_state.lock);
    for (int i = 0; i < FB_GPU_MAX_BOS; i++) {
        struct fb_gpu_gem_object *candidate = &fb_state.gems[i];
        if (candidate->in_use && !candidate->dead &&
            candidate->flink_name == name) {
            gem = candidate;
            fb_gem_get_locked(gem);
            size = gem->size;
            break;
        }
    }
    spin_unlock(&fb_state.lock);
    if (gem == NULL)
        return -ENOENT;

    ret = fb_bo_register_gem(owner_id, owner_tgid, gem, handle_out);
    fb_gem_put(gem);
    if (ret != 0)
        return ret;
    *size_out = size;
    return 0;
}

static int fb_dmabuf_get_gem(struct dma_buf *dbuf,
                             struct fb_gpu_gem_object **gem_out);
static void *fb_dmabuf_fault(struct dma_buf *dbuf, uint64 offset);
static void fb_dmabuf_release(struct dma_buf *dbuf);
static struct vfs_file_ops fb_dmabuf_file_ops;

static const struct dma_buf_ops fb_dmabuf_ops = {
    .get_gem = fb_dmabuf_get_gem,
    .fault = fb_dmabuf_fault,
    .release = fb_dmabuf_release,
};

static const struct dma_buf_ops fb_test_dmabuf_ops = {
    .get_gem = fb_dmabuf_get_gem,
    .fault = fb_dmabuf_fault,
    .release = fb_dmabuf_release,
};

static void fb_dmabuf_snapshot_locked(struct fb_gpu_dmabuf_object *dmabuf)
{
    struct fb_gpu_gem_object *gem;

    if (dmabuf == NULL || dmabuf->gem == NULL)
        return;
    gem = dmabuf->gem;
    dmabuf->ttm_resv_seq_snapshot = gem->ttm_resv_seq;
    dmabuf->ttm_resv_exclusive_fence_snapshot =
        gem->ttm_resv_exclusive_fence;
    dmabuf->ttm_resv_shared_count_snapshot = gem->ttm_resv_shared_count;
    dmabuf->ttm_resv_shared_fence_snapshot =
        fb_ttm_resv_latest_shared_fence_locked(gem);
    fb_state.stats.dmabuf_resv_snapshots++;
    fb_state.stats.dmabuf_last_exporter_tag = dmabuf->exporter_tag;
    fb_state.stats.dmabuf_last_importer_tag = dmabuf->importer_tag;
    fb_state.stats.dmabuf_last_ttm_resv_seq =
        dmabuf->ttm_resv_seq_snapshot;
    fb_state.stats.dmabuf_last_ttm_resv_exclusive_fence =
        dmabuf->ttm_resv_exclusive_fence_snapshot;
    fb_state.stats.dmabuf_last_ttm_resv_shared_fence =
        dmabuf->ttm_resv_shared_fence_snapshot;
    fb_state.stats.dmabuf_last_ttm_resv_shared_count =
        dmabuf->ttm_resv_shared_count_snapshot;
}

static struct fb_gpu_dmabuf_object *fb_dmabuf_create_from_bo_ops(
    struct fb_gpu_bo_entry *bo, uint32 exporter_tag,
    const struct dma_buf_ops *ops)
{
    struct fb_gpu_dmabuf_object *dmabuf;
    struct dma_buf *dbuf;

    dmabuf = kvmalloc(sizeof(*dmabuf));
    if (dmabuf == NULL)
        return NULL;
    dbuf = kvmalloc(sizeof(*dbuf));
    if (dbuf == NULL) {
        kvfree(dmabuf);
        return NULL;
    }
    memset(dmabuf, 0, sizeof(*dmabuf));
    memset(dbuf, 0, sizeof(*dbuf));
    dmabuf->exporter_tag = exporter_tag;
    dmabuf->dbuf = dbuf;
    dbuf->ops = ops != NULL ? ops : &fb_dmabuf_ops;
    dbuf->priv = dmabuf;

    spin_lock(&fb_state.lock);
    if (bo == NULL || bo->gem == NULL || !bo->gem->in_use ||
        bo->gem->dead) {
        spin_unlock(&fb_state.lock);
        kvfree(dbuf);
        kvfree(dmabuf);
        return NULL;
    }
    dmabuf->gem = bo->gem;
    dbuf->size = bo->gem->size;
    fb_gem_get_locked(dmabuf->gem);
    fb_ttm_resv_record_shared_locked(
        bo, exporter_tag == FB_GPU_DMABUF_TAG_DRM_PRIME ?
            FB_GPU_RESV_ATTACH_PRIME_EXPORT :
            FB_GPU_RESV_ATTACH_DMABUF_EXPORT,
        exporter_tag);
    fb_dmabuf_snapshot_locked(dmabuf);
    spin_unlock(&fb_state.lock);
    return dmabuf;
}

static struct fb_gpu_dmabuf_object *
fb_dmabuf_create_from_bo(struct fb_gpu_bo_entry *bo, uint32 exporter_tag)
{
    return fb_dmabuf_create_from_bo_ops(bo, exporter_tag, &fb_dmabuf_ops);
}

static struct fb_gpu_dmabuf_object *
fb_test_dmabuf_create_from_bo(struct fb_gpu_bo_entry *bo, uint32 exporter_tag)
{
    return fb_dmabuf_create_from_bo_ops(bo, exporter_tag,
                                        &fb_test_dmabuf_ops);
}

static void fb_dmabuf_note_export(struct fb_gpu_dmabuf_object *dmabuf)
{
    spin_lock(&fb_state.lock);
    fb_state.stats.bo_fd_exports++;
    fb_state.stats.dmabuf_exports++;
    if (dmabuf != NULL)
        fb_state.stats.dmabuf_last_exporter_tag = dmabuf->exporter_tag;
    spin_unlock(&fb_state.lock);
}

static void fb_dmabuf_live_open_locked(struct fb_gpu_dmabuf_object *dmabuf)
{
    if (dmabuf == NULL || dmabuf->live_accounted)
        return;
    fb_state.stats.bo_fd_live++;
    if (fb_state.stats.bo_fd_live > fb_state.stats.bo_fd_peak)
        fb_state.stats.bo_fd_peak = fb_state.stats.bo_fd_live;
    fb_state.stats.dmabuf_live++;
    if (fb_state.stats.dmabuf_live > fb_state.stats.dmabuf_peak)
        fb_state.stats.dmabuf_peak = fb_state.stats.dmabuf_live;
    dmabuf->live_accounted = 1;
}

static void fb_dmabuf_live_close_locked(struct fb_gpu_dmabuf_object *dmabuf)
{
    if (dmabuf == NULL || !dmabuf->live_accounted)
        return;
    if (fb_state.stats.bo_fd_live > 0)
        fb_state.stats.bo_fd_live--;
    if (fb_state.stats.dmabuf_live > 0)
        fb_state.stats.dmabuf_live--;
    dmabuf->live_accounted = 0;
}

static void fb_dmabuf_note_import_locked(struct fb_gpu_dmabuf_object *dmabuf,
                                         struct fb_gpu_bo_entry *bo,
                                         uint32 importer_tag)
{
    fb_state.stats.dmabuf_import_attempts++;
    fb_state.stats.dmabuf_local_imports++;
    fb_state.stats.dmabuf_local_only_import_path = 1;
    if (dmabuf != NULL) {
        dmabuf->importer_tag = importer_tag;
        dmabuf->import_count++;
        dmabuf->attachment_count++;
        if (dmabuf->gem != NULL &&
            dmabuf->ttm_resv_seq_snapshot != 0 &&
            dmabuf->ttm_resv_seq_snapshot < dmabuf->gem->ttm_resv_seq)
            fb_ttm_resv_mark_stale_locked();
    }
    if (bo != NULL) {
        fb_ttm_resv_record_shared_locked(
            bo, importer_tag == FB_GPU_DMABUF_TAG_DRM_PRIME ?
                FB_GPU_RESV_ATTACH_PRIME_IMPORT :
                FB_GPU_RESV_ATTACH_DMABUF_IMPORT,
            importer_tag);
    }
    if (dmabuf != NULL)
        fb_dmabuf_snapshot_locked(dmabuf);
    if (bo != NULL) {
        bo->dmabuf_attachment_accounted = 1;
        bo->dmabuf_importer_tag = importer_tag;
    }
    fb_state.stats.bo_fd_imports++;
    fb_state.stats.dmabuf_imports++;
    fb_state.stats.dmabuf_attachments++;
    fb_state.stats.dmabuf_live_attachments++;
    if (fb_state.stats.dmabuf_live_attachments >
        fb_state.stats.dmabuf_peak_attachments)
        fb_state.stats.dmabuf_peak_attachments =
            fb_state.stats.dmabuf_live_attachments;
    fb_state.stats.dmabuf_last_importer_tag = importer_tag;
}

static void fb_dmabuf_note_bad_fd_reject(void)
{
    spin_lock(&fb_state.lock);
    fb_state.stats.dmabuf_import_attempts++;
    fb_state.stats.dmabuf_local_only_import_path = 1;
    fb_state.stats.dmabuf_bad_fd_rejects++;
    spin_unlock(&fb_state.lock);
}

static void fb_dmabuf_note_foreign_fd_reject(void)
{
    spin_lock(&fb_state.lock);
    fb_state.stats.dmabuf_import_attempts++;
    fb_state.stats.dmabuf_foreign_import_attempts++;
    fb_state.stats.dmabuf_foreign_import_rejects++;
    fb_state.stats.dmabuf_local_only_import_path = 1;
    fb_state.stats.dmabuf_foreign_fd_rejects++;
    spin_unlock(&fb_state.lock);
}

static int fb_dmabuf_get_gem(struct dma_buf *dbuf,
                             struct fb_gpu_gem_object **gem_out)
{
    struct fb_gpu_dmabuf_object *dmabuf;

    if (gem_out != NULL)
        *gem_out = NULL;
    if (dbuf == NULL || gem_out == NULL)
        return -EINVAL;
    dmabuf = (struct fb_gpu_dmabuf_object *)dbuf->priv;
    if (dmabuf == NULL || dmabuf->gem == NULL)
        return -ENOENT;

    spin_lock(&fb_state.lock);
    if (!dmabuf->gem->in_use || dmabuf->gem->dead) {
        spin_unlock(&fb_state.lock);
        return -ENOENT;
    }
    fb_gem_get_locked(dmabuf->gem);
    *gem_out = dmabuf->gem;
    spin_unlock(&fb_state.lock);
    return 0;
}

static struct fb_gpu_dmabuf_object *fb_dmabuf_from_dma_buf(struct dma_buf *dbuf)
{
    if (dbuf == NULL || dbuf->ops == NULL)
        return NULL;
    return (struct fb_gpu_dmabuf_object *)dbuf->priv;
}

static struct dma_buf *fb_dma_buf_from_file(struct vfs_file *file)
{
    if (file == NULL || file->ops != &fb_dmabuf_file_ops ||
        file->private_data == NULL)
        return NULL;
    return (struct dma_buf *)file->private_data;
}

static int fb_dma_buf_get_gem(struct dma_buf *dbuf,
                              struct fb_gpu_gem_object **gem_out)
{
    if (dbuf == NULL || dbuf->ops == NULL || dbuf->ops->get_gem == NULL)
        return -EINVAL;
    return dbuf->ops->get_gem(dbuf, gem_out);
}

static void *fb_dma_buf_fault(struct dma_buf *dbuf, uint64 offset)
{
    if (dbuf == NULL || dbuf->ops == NULL || dbuf->ops->fault == NULL)
        return NULL;
    return dbuf->ops->fault(dbuf, offset);
}

static void *fb_dmabuf_fault(struct dma_buf *dbuf, uint64 offset)
{
    struct fb_gpu_dmabuf_object *dmabuf;
    struct fb_gpu_gem_object *gem;
    uint64 page_index;
    void *pa = NULL;

    if (dbuf == NULL || (offset & (PGSIZE - 1)) != 0 ||
        offset >= dbuf->size)
        return NULL;

    dmabuf = (struct fb_gpu_dmabuf_object *)dbuf->priv;
    if (dmabuf == NULL)
        return NULL;

    page_index = offset / PGSIZE;
    spin_lock(&fb_state.lock);
    gem = dmabuf->gem;
    if (gem != NULL && gem->in_use && !gem->dead &&
        page_index < gem->npages && gem->pages[page_index] != NULL) {
        pa = (void *)__page_to_pa(gem->pages[page_index]);
        if (page_ref_inc(pa) <= 0)
            pa = NULL;
    }
    spin_unlock(&fb_state.lock);
    return pa;
}

static void fb_dmabuf_put(struct dma_buf *dbuf)
{
    if (dbuf == NULL)
        return;
    if (dbuf->ops != NULL && dbuf->ops->release != NULL)
        dbuf->ops->release(dbuf);
}

static void fb_dmabuf_release(struct dma_buf *dbuf)
{
    struct fb_gpu_gem_object *gem;
    struct fb_gpu_dmabuf_object *dmabuf;

    dmabuf = fb_dmabuf_from_dma_buf(dbuf);
    if (dmabuf == NULL)
        return;
    gem = dmabuf->gem;
    spin_lock(&fb_state.lock);
    fb_dmabuf_live_close_locked(dmabuf);
    fb_state.stats.dmabuf_releases++;
    fb_state.stats.dmabuf_last_exporter_tag = dmabuf->exporter_tag;
    fb_state.stats.dmabuf_last_importer_tag = dmabuf->importer_tag;
    spin_unlock(&fb_state.lock);
    fb_gem_put(gem);
    kvfree(dbuf);
    kvfree(dmabuf);
}
