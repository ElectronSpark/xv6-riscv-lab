static int virtio_gpu_backing_order(uint64 bytes, uint32 *alloc_len)
{
    uint64 len = PGSIZE;
    int order = 0;

    while (len < bytes && order < PAGE_BUDDY_MAX_ORDER) {
        order++;
        len <<= 1;
    }

    if (len < bytes || len > UINT32_MAX)
        return -1;

    *alloc_len = (uint32)len;
    return order;
}

static int virtio_gpu_map_pages_current(page_t **pages, uint32 npages,
                                        uint64 size, uint64 *addr_out)
{
    vm_t *vm;
    vma_t *vma;
    uint64 addr;
    uint64 flags;
    uint64 pte_flags;

    if (pages == NULL || npages == 0 || size == 0 || addr_out == NULL ||
        current == NULL || current->vm == NULL)
        return -EINVAL;

    vm = current->vm;
    flags = PROT_READ | PROT_WRITE | VMA_FLAG_USER |
            VMA_FLAG_DONTFORK | VMA_FLAG_DONTDUMP;

    vm_wlock(vm);
    addr = vm_find_free_range(vm, (size_t)size, 0);
    if (addr == 0) {
        vm_wunlock(vm);
        return -ENOMEM;
    }

    vma = vma_alloc(vm, addr, size, flags);
    if (vma == NULL) {
        vm_wunlock(vm);
        return -ENOMEM;
    }
    if (anon_vma_prepare(vma) != 0) {
        vma_free(vm, vma);
        vm_wunlock(vm);
        return -ENOMEM;
    }

    pte_flags = vma2pte_flags(flags);
    for (uint32 i = 0; i < npages; i++) {
        uint64 va = addr + (uint64)i * PGSIZE;
        uint64 pa = __page_to_pa(pages[i]);

        if (page_ref_inc((void *)pa) <= 0) {
            vma_free(vm, vma);
            vm_wunlock(vm);
            return -ENOMEM;
        }
        if (mappages(vm->pagetable, va, PGSIZE, pa, pte_flags) != 0) {
            page_ref_dec((void *)pa);
            vma_free(vm, vma);
            vm_wunlock(vm);
            return -ENOMEM;
        }
        page_add_anon_rmap(pages[i], vma, va);
    }

    vm_wunlock(vm);
    *addr_out = addr;
    return 0;
}

static int virtio_gpu_resource_create_2d(struct virtio_gpu *g, uint32 width,
                                         uint32 height, uint32 format,
                                         struct virtio_gpu_resource **out)
{
    struct virtio_gpu_resource_create_2d *create =
        (struct virtio_gpu_resource_create_2d *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    uint64 bytes = (uint64)width * height * sizeof(uint32);
    uint32 alloc_len;

    if (width == 0 || height == 0 || bytes == 0 ||
        bytes / sizeof(uint32) / width != height) {
        virtio_gpu_count_failure(g);
        return -1;
    }
    int order = virtio_gpu_backing_order(bytes, &alloc_len);
    if (order < 0) {
        virtio_gpu_count_failure(g);
        printf("virtio_gpu: resource backing too large %ux%u bytes=%lu\n",
               width, height, bytes);
        return -1;
    }

    spin_lock(&g->lock);
    struct virtio_gpu_resource *res = virtio_gpu_alloc_resource_slot(g);
    if (res == NULL) {
        g->stats.failures++;
        spin_unlock(&g->lock);
        return -1;
    }
    uint32 id = g->next_resource_id++;
    if (g->next_resource_id == 0)
        g->next_resource_id = 1;
    spin_unlock(&g->lock);

    void *backing = page_alloc(order, PAGE_TYPE_ANON);
    if (backing == NULL) {
        virtio_gpu_count_failure(g);
        printf("virtio_gpu: resource backing alloc failed order=%d bytes=%lu\n",
               order, bytes);
        return -1;
    }
    memset(backing, 0, alloc_len);

    memset(create, 0, sizeof(*create));
    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create->resource_id = id;
    create->format = format;
    create->width = width;
    create->height = height;
    if (virtio_gpu_submit(g, create, sizeof(*create), NULL, 0, false, resp,
                          sizeof(*resp), VIRTIO_GPU_RESP_OK_NODATA) != 0) {
        page_free(backing, order);
        return -1;
    }

    spin_lock(&g->lock);
    memset(res, 0, sizeof(*res));
    res->in_use = 1;
    res->id = id;
    res->owner_tgid = 0;
    res->width = width;
    res->height = height;
    res->format = format;
    res->backing = backing;
    res->backing_len = (uint32)bytes;
    res->alloc_len = alloc_len;
    res->backing_order = (uint32)order;
    g->stats.resources++;
    g->stats.resource_bytes += bytes;
    spin_unlock(&g->lock);

    *out = res;
    return 0;
}

static int virtio_gpu_resource_create_3d(struct virtio_gpu *g,
                                         uint64 owner_id,
                                         pid_t owner_tgid,
                                         struct fb_gpu_virgl_resource_create *req,
                                         struct virtio_gpu_resource **out)
{
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    uint64 bytes = req->size;
    uint32 npages;
    page_t **pages = NULL;

    if (req->width == 0 || req->height == 0)
        return -EINVAL;
    if (bytes == 0)
        bytes = (uint64)req->width * req->height * sizeof(uint32);
    if (bytes == 0 || bytes > 64ULL * 1024 * 1024)
        return -EINVAL;
    npages = (uint32)PGROUNDUP(bytes) / PGSIZE;

    if (fb_shmem_alloc_pages(npages, &pages) != 0)
        return -ENOMEM;

    spin_lock(&g->lock);
    struct virtio_gpu_resource *res = virtio_gpu_alloc_resource_slot(g);
    if (res == NULL) {
        g->stats.failures++;
        spin_unlock(&g->lock);
        fb_shmem_release_pages(pages, npages);
        return -ENOSPC;
    }
    uint32 id = g->next_resource_id++;
    if (g->next_resource_id == 0)
        g->next_resource_id = 1;
    spin_unlock(&g->lock);

    memset(create, 0, sizeof(*create));
    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = id;
    create->target = req->target;
    create->format = req->format;
    create->bind = req->bind;
    create->width = req->width;
    create->height = req->height;
    create->depth = req->depth ? req->depth : 1;
    create->array_size = req->array_size ? req->array_size : 1;
    create->last_level = req->last_level;
    create->nr_samples = req->nr_samples;
    create->flags = req->flags;
    if (virtio_gpu_submit(g, create, sizeof(*create), NULL, 0, false, resp,
                          sizeof(*resp), VIRTIO_GPU_RESP_OK_NODATA) != 0) {
        fb_shmem_release_pages(pages, npages);
        return -EIO;
    }

    spin_lock(&g->lock);
    memset(res, 0, sizeof(*res));
    res->in_use = 1;
    res->id = id;
    res->owner_id = owner_id;
    res->owner_tgid = owner_tgid;
    res->width = req->width;
    res->height = req->height;
    res->depth = create->depth;
    res->last_level = req->last_level;
    res->format = req->format;
    res->backing_len = (uint32)bytes;
    res->alloc_len = npages * PGSIZE;
    res->pages = pages;
    res->npages = npages;
    res->ctx_id = req->ctx_id;
    res->is_3d = 1;
    res->target = req->target;
    res->bind = req->bind;
    g->stats.resources++;
    g->stats.resource_bytes += bytes;
    spin_unlock(&g->lock);

    *out = res;
    return 0;
}

static int virtio_gpu_resource_create_blob(
    struct virtio_gpu *g, uint64 owner_id, pid_t owner_tgid,
    struct fb_gpu_virgl_blob_create *req,
    struct virtio_gpu_resource **out, int allow_host_decline)
{
    struct virtio_gpu_resource_create_blob *create =
        (struct virtio_gpu_resource_create_blob *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    struct virtio_gpu_mem_entry *entries = NULL;
    struct virtio_gpu_resource *res = NULL;
    page_t **pages = NULL;
    uint64 rounded;
    uint64 bytes;
    uint32 npages;
    uint32 entries_len;
    uint32 pitch;
    uint32 id;
    int guest_backed;
    uint32 allowed_flags;
    int ret;

    if (!(g->driver_features0 & (1u << VIRTIO_GPU_F_RESOURCE_BLOB)))
        return -EOPNOTSUPP;
    if (req == NULL || req->size == 0)
        return -EINVAL;
    if (req->blob_mem != VIRTIO_GPU_BLOB_MEM_GUEST &&
        req->blob_mem != VIRTIO_GPU_BLOB_MEM_HOST3D &&
        req->blob_mem != VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST)
        return -EOPNOTSUPP;
    guest_backed = req->blob_mem == VIRTIO_GPU_BLOB_MEM_GUEST ||
        req->blob_mem == VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST;
    allowed_flags = VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE |
        VIRTIO_GPU_BLOB_FLAG_USE_SHAREABLE |
        VIRTIO_GPU_BLOB_FLAG_USE_CROSS_DEVICE;
    if ((req->blob_flags & ~allowed_flags) != 0)
        return -EOPNOTSUPP;
    if (req->blob_flags != 0 && !virtio_gpu_host_visible_operational(g))
        return -EOPNOTSUPP;
    if (req->blob_mem == VIRTIO_GPU_BLOB_MEM_HOST3D &&
        (req->blob_flags & VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE) == 0)
        return -EINVAL;

    bytes = req->size;
    if (bytes > 64ULL * 1024 * 1024)
        return -EINVAL;
    rounded = PGROUNDUP(bytes);
    npages = (uint32)(rounded / PGSIZE);
    if (npages == 0 || (guest_backed && npages > UINT32_MAX / sizeof(*entries)))
        return -EINVAL;

    if (guest_backed) {
        ret = fb_shmem_alloc_pages(npages, &pages);
        if (ret != 0)
            return ret;

        entries_len = npages * sizeof(*entries);
        entries = kvmalloc(entries_len);
        if (entries == NULL) {
            fb_shmem_release_pages(pages, npages);
            return -ENOMEM;
        }
        memset(entries, 0, entries_len);
        for (uint32 i = 0; i < npages; i++) {
            entries[i].addr = __page_to_pa(pages[i]);
            entries[i].length = PGSIZE;
        }
    } else {
        npages = 0;
        entries_len = 0;
    }

    spin_lock(&g->lock);
    res = virtio_gpu_alloc_resource_slot(g);
    if (res == NULL) {
        g->stats.failures++;
        spin_unlock(&g->lock);
        if (entries != NULL)
            kvfree(entries);
        if (pages != NULL)
            fb_shmem_release_pages(pages, npages);
        return -ENOSPC;
    }
    id = g->next_resource_id++;
    if (g->next_resource_id == 0)
        g->next_resource_id = 1;
    spin_unlock(&g->lock);

    memset(create, 0, sizeof(*create));
    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    create->hdr.ctx_id = req->ctx_id;
    create->resource_id = id;
    create->blob_mem = req->blob_mem;
    create->blob_flags = req->blob_flags;
    create->nr_entries = guest_backed ? npages : 0;
    create->blob_id = req->blob_id;
    create->size = bytes;
    printf("virtio_gpu: resource blob create resource=%u ctx=%u blob_mem=%u flags=0x%x blob_id=%lu size=%lu entries=%u\n",
           id, req->ctx_id, req->blob_mem, req->blob_flags, req->blob_id,
           bytes, npages);
    if (allow_host_decline)
        ret = virtio_gpu_submit_accepting_error(
            g, create, sizeof(*create), entries, entries_len, false, resp,
            sizeof(*resp), VIRTIO_GPU_RESP_OK_NODATA,
            VIRTIO_GPU_RESP_ERR_UNSPEC);
    else
        ret = virtio_gpu_submit(g, create, sizeof(*create), entries,
                                entries_len, false, resp, sizeof(*resp),
                                VIRTIO_GPU_RESP_OK_NODATA);
    if (entries != NULL)
        kvfree(entries);
    if (ret != 0) {
        if (ret > 0)
            printf("virtio_gpu: resource blob create host rejected resource=%u response=0x%x\n",
                   id, resp->type);
        else
            printf("virtio_gpu: resource blob create submit failed resource=%u ret=%d\n",
                   id, ret);
        if (pages != NULL)
            fb_shmem_release_pages(pages, npages);
        return -EIO;
    }

    pitch = rounded >= PGSIZE ? PGSIZE : (uint32)rounded;
    if (pitch == 0)
        pitch = PGSIZE;

    spin_lock(&g->lock);
    memset(res, 0, sizeof(*res));
    res->in_use = 1;
    res->id = id;
    res->owner_id = owner_id;
    res->owner_tgid = owner_tgid;
    res->width = pitch / 4;
    res->height = (uint32)((rounded + pitch - 1) / pitch);
    res->depth = 1;
    res->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    res->backing_len = (uint32)bytes;
    res->alloc_len = (uint32)rounded;
    res->pages = pages;
    res->npages = npages;
    res->ctx_id = req->ctx_id;
    res->is_blob = 1;
    res->blob_mem = req->blob_mem;
    res->blob_flags = req->blob_flags;
    res->blob_id = req->blob_id;
    g->stats.resources++;
    g->stats.resource_bytes += bytes;
    spin_unlock(&g->lock);

    req->resource_id = id;
    req->size = res->alloc_len;
    req->addr = 0;
    *out = res;
    return 0;
}

static int virtio_gpu_resource_map_blob(struct virtio_gpu *g,
                                        struct virtio_gpu_resource *res,
                                        uint64 offset,
                                        uint32 *map_info_out)
{
    struct virtio_gpu_resource_map_blob *map =
        (struct virtio_gpu_resource_map_blob *)g->cmd_page;
    struct virtio_gpu_resp_map_info *resp =
        (struct virtio_gpu_resp_map_info *)g->resp_page;
    uint64 map_size;
    int ret;

    if (g == NULL || res == NULL || res->id == 0)
        return -EINVAL;
    if (!virtio_gpu_host_visible_operational(g))
        return -EOPNOTSUPP;
    if (res->host_visible_mapped) {
        if (map_info_out != NULL)
            *map_info_out = res->host_visible_map_info;
        return 0;
    }
    map_size = PGROUNDUP(res->alloc_len);
    if (map_size == 0 || offset + map_size < offset ||
        offset + map_size > g->host_visible_length)
        return -ENOSPC;

    memset(map, 0, sizeof(*map));
    map->hdr.type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    map->hdr.ctx_id = res->ctx_id;
    map->resource_id = res->id;
    map->offset = offset;
    ret = virtio_gpu_submit(g, map, sizeof(*map), NULL, 0, false, resp,
                            sizeof(*resp), VIRTIO_GPU_RESP_OK_MAP_INFO);
    if (ret != 0) {
        printf("virtio_gpu: resource map blob failed resource=%u offset=0x%lx ret=%d\n",
               res->id, offset, ret);
        return -EIO;
    }

    spin_lock(&g->lock);
    res->host_visible_mapped = 1;
    res->host_visible_offset = offset;
    res->host_visible_size = map_size;
    res->host_visible_map_info = resp->map_info & VIRTIO_GPU_MAP_CACHE_MASK;
    g->host_visible_blob_ok = 1;
    g->host_visible_mapped_once = 1;
    spin_unlock(&g->lock);
    if (map_info_out != NULL)
        *map_info_out = resp->map_info & VIRTIO_GPU_MAP_CACHE_MASK;
    printf("virtio_gpu: resource map blob ok resource=%u offset=0x%lx size=%lu map_info=0x%x\n",
           res->id, offset, map_size,
           resp->map_info & VIRTIO_GPU_MAP_CACHE_MASK);
    return 0;
}

static int virtio_gpu_resource_unmap_blob(struct virtio_gpu *g,
                                          struct virtio_gpu_resource *res)
{
    struct virtio_gpu_resource_unmap_blob *unmap =
        (struct virtio_gpu_resource_unmap_blob *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;

    if (g == NULL || res == NULL || !res->host_visible_mapped)
        return 0;

    memset(unmap, 0, sizeof(*unmap));
    unmap->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB;
    unmap->hdr.ctx_id = res->ctx_id;
    unmap->resource_id = res->id;
    if (virtio_gpu_submit(g, unmap, sizeof(*unmap), NULL, 0, false, resp,
                          sizeof(*resp), VIRTIO_GPU_RESP_OK_NODATA) != 0)
        return -EIO;

    spin_lock(&g->lock);
    res->host_visible_mapped = 0;
    res->host_visible_offset = 0;
    res->host_visible_size = 0;
    res->host_visible_map_info = 0;
    spin_unlock(&g->lock);
    return 0;
}

static int virtio_gpu_resource_create_3d_backing(struct virtio_gpu *g,
                                                 uint32 width,
                                                 uint32 height,
                                                 uint32 format,
                                                 uint32 bind,
                                                 struct virtio_gpu_resource **out)
{
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    struct virtio_gpu_resource *res;
    uint64 bytes = (uint64)width * height * sizeof(uint32);
    uint32 alloc_len = PGSIZE;
    int order = 0;
    void *backing;

    if (width == 0 || height == 0 || bytes == 0 ||
        bytes > 64ULL * 1024 * 1024)
        return -EINVAL;
    while (alloc_len < bytes) {
        if (order >= PAGE_BUDDY_MAX_ORDER)
            return -ENOMEM;
        order++;
        alloc_len <<= 1;
    }

    backing = page_alloc(order, PAGE_TYPE_ANON);
    if (backing == NULL)
        return -ENOMEM;

    spin_lock(&g->lock);
    res = virtio_gpu_alloc_resource_slot(g);
    if (res == NULL) {
        g->stats.failures++;
        spin_unlock(&g->lock);
        page_free(backing, order);
        return -ENOSPC;
    }
    uint32 id = g->next_resource_id++;
    if (g->next_resource_id == 0)
        g->next_resource_id = 1;
    spin_unlock(&g->lock);

    memset(create, 0, sizeof(*create));
    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = id;
    create->target = VIRTIO_GPU_PIPE_TEXTURE_2D;
    create->format = format;
    create->bind = bind;
    create->width = width;
    create->height = height;
    create->depth = 1;
    create->array_size = 1;
    if (virtio_gpu_submit(g, create, sizeof(*create), NULL, 0, false, resp,
                          sizeof(*resp), VIRTIO_GPU_RESP_OK_NODATA) != 0) {
        page_free(backing, order);
        return -EIO;
    }

    spin_lock(&g->lock);
    memset(res, 0, sizeof(*res));
    res->in_use = 1;
    res->id = id;
    res->owner_tgid = 0;
    res->width = width;
    res->height = height;
    res->depth = 1;
    res->format = format;
    res->backing = backing;
    res->backing_len = (uint32)bytes;
    res->alloc_len = alloc_len;
    res->backing_order = (uint32)order;
    res->is_3d = 1;
    res->target = VIRTIO_GPU_PIPE_TEXTURE_2D;
    res->bind = bind;
    g->stats.resources++;
    g->stats.resource_bytes += bytes;
    spin_unlock(&g->lock);

    *out = res;
    return 0;
}

static int virtio_gpu_resource_attach_backing(struct virtio_gpu *g,
                                              struct virtio_gpu_resource *res)
{
    struct virtio_gpu_resource_attach_backing *attach =
        (struct virtio_gpu_resource_attach_backing *)g->cmd_page;
    struct virtio_gpu_mem_entry *entry =
        (struct virtio_gpu_mem_entry *)g->data_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;

    memset(entry, 0, sizeof(*entry));
    entry->addr = (uint64)res->backing;
    entry->length = res->backing_len;

    memset(attach, 0, sizeof(*attach));
    attach->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach->resource_id = res->id;
    attach->nr_entries = 1;
    if (virtio_gpu_submit(g, attach, sizeof(*attach), entry, sizeof(*entry),
                          false, resp, sizeof(*resp),
                          VIRTIO_GPU_RESP_OK_NODATA) != 0)
        return -1;

    spin_lock(&g->lock);
    res->attached = 1;
    spin_unlock(&g->lock);
    return 0;
}

static int virtio_gpu_resource_attach_pages(struct virtio_gpu *g,
                                            struct virtio_gpu_resource *res)
{
    struct virtio_gpu_resource_attach_backing *attach =
        (struct virtio_gpu_resource_attach_backing *)g->cmd_page;
    struct virtio_gpu_mem_entry *entry;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    uint32 entries_len;
    uint32 alloc_len = PGSIZE;
    int order = 0;
    int ret;

    if (res->pages == NULL || res->npages == 0 ||
        res->npages > UINT32_MAX / sizeof(*entry))
        return -1;

    entries_len = res->npages * sizeof(*entry);
    while (alloc_len < entries_len) {
        if (order >= PAGE_BUDDY_MAX_ORDER)
            return -1;
        order++;
        alloc_len <<= 1;
    }

    entry = page_alloc(order, PAGE_TYPE_ANON);
    if (entry == NULL)
        return -1;

    memset(entry, 0, entries_len);
    for (uint32 i = 0; i < res->npages; i++) {
        entry[i].addr = __page_to_pa(res->pages[i]);
        entry[i].length = PGSIZE;
    }

    memset(attach, 0, sizeof(*attach));
    attach->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach->resource_id = res->id;
    attach->nr_entries = res->npages;
    ret = virtio_gpu_submit(g, attach, sizeof(*attach), entry, entries_len,
                            false, resp, sizeof(*resp),
                            VIRTIO_GPU_RESP_OK_NODATA);
    page_free(entry, order);
    if (ret != 0)
        return -1;

    spin_lock(&g->lock);
    res->attached = 1;
    spin_unlock(&g->lock);
    return 0;
}

static int virtio_gpu_set_scanout(struct virtio_gpu *g, uint32 scanout_id,
                                  struct virtio_gpu_resource *res,
                                  uint32 x, uint32 y, uint32 width,
                                  uint32 height)
{
    struct virtio_gpu_set_scanout *set_scanout =
        (struct virtio_gpu_set_scanout *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;

    memset(set_scanout, 0, sizeof(*set_scanout));
    set_scanout->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    set_scanout->r.x = x;
    set_scanout->r.y = y;
    set_scanout->r.width = width;
    set_scanout->r.height = height;
    set_scanout->scanout_id = scanout_id;
    set_scanout->resource_id = res ? res->id : 0;

    return virtio_gpu_submit(g, set_scanout, sizeof(*set_scanout), NULL, 0,
                             false, resp, sizeof(*resp),
                             VIRTIO_GPU_RESP_OK_NODATA);
}

static int virtio_gpu_set_scanout_async_prepare(
    uint32 scanout_id, uint32 resource_id, uint32 x, uint32 y, uint32 width,
    uint32 height, struct virtio_gpu_async_submit *prep)
{
    struct virtio_gpu_set_scanout *cmd;
    struct virtio_gpu_ctrl_hdr *resp;

    memset(prep, 0, sizeof(*prep));
    cmd = kalloc();
    resp = kalloc();
    if (cmd == NULL || resp == NULL) {
        if (cmd != NULL)
            kfree(cmd);
        if (resp != NULL)
            kfree(resp);
        return -ENOMEM;
    }

    memset(cmd, 0, sizeof(*cmd));
    memset(resp, 0, sizeof(*resp));
    cmd->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd->r.x = x;
    cmd->r.y = y;
    cmd->r.width = width;
    cmd->r.height = height;
    cmd->scanout_id = scanout_id;
    cmd->resource_id = resource_id;

    prep->ctx_id = 0;
    prep->fence_id = 0;
    prep->type = VIRTIO_GPU_CMD_SET_SCANOUT;
    prep->expected = VIRTIO_GPU_RESP_OK_NODATA;
    prep->cmd = cmd;
    prep->cmd_len = sizeof(*cmd);
    prep->data = NULL;
    prep->data_len = 0;
    prep->data_order = 0;
    prep->resp = resp;
    prep->resp_len = sizeof(*resp);
    return 0;
}

static int virtio_gpu_resource_transfer_2d(struct virtio_gpu *g,
                                           struct virtio_gpu_resource *res,
                                           uint32 x, uint32 y, uint32 width,
                                           uint32 height)
{
    struct virtio_gpu_transfer_to_host_2d *transfer =
        (struct virtio_gpu_transfer_to_host_2d *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;

    memset(transfer, 0, sizeof(*transfer));
    transfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    transfer->r.x = x;
    transfer->r.y = y;
    transfer->r.width = width;
    transfer->r.height = height;
    transfer->offset = (uint64)y * res->width * sizeof(uint32) +
                       (uint64)x * sizeof(uint32);
    transfer->resource_id = res->id;
    return virtio_gpu_submit(g, transfer, sizeof(*transfer), NULL, 0, false,
                             resp, sizeof(*resp),
                             VIRTIO_GPU_RESP_OK_NODATA);
}

static int virtio_gpu_resource_transfer_3d(struct virtio_gpu *g,
                                           struct virtio_gpu_resource *res,
                                           uint32 x, uint32 y, uint32 width,
                                           uint32 height)
{
    struct virtio_gpu_transfer_host_3d *transfer =
        (struct virtio_gpu_transfer_host_3d *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    uint32 stride;

    if (res == NULL || res->width == 0 || res->height == 0 ||
        width == 0 || height == 0)
        return -1;

    stride = res->width * sizeof(uint32);
    memset(transfer, 0, sizeof(*transfer));
    transfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    transfer->box.x = x;
    transfer->box.y = y;
    transfer->box.z = 0;
    transfer->box.w = width;
    transfer->box.h = height;
    transfer->box.d = 1;
    transfer->offset = (uint64)y * stride + (uint64)x * sizeof(uint32);
    transfer->resource_id = res->id;
    transfer->level = 0;
    transfer->stride = stride;
    transfer->layer_stride = (uint64)stride * res->height;

    return virtio_gpu_submit(g, transfer, sizeof(*transfer), NULL, 0, false,
                             resp, sizeof(*resp),
                             VIRTIO_GPU_RESP_OK_NODATA);
}

static int virtio_gpu_resource_transfer_scanout(struct virtio_gpu *g,
                                                struct virtio_gpu_resource *res,
                                                uint32 x, uint32 y,
                                                uint32 width, uint32 height)
{
    if (res != NULL && res->is_3d)
        return virtio_gpu_resource_transfer_3d(g, res, x, y, width, height);
    return virtio_gpu_resource_transfer_2d(g, res, x, y, width, height);
}

static int virtio_gpu_resource_flush(struct virtio_gpu *g,
                                     struct virtio_gpu_resource *res,
                                     uint32 x, uint32 y, uint32 width,
                                     uint32 height)
{
    struct virtio_gpu_resource_flush *flush =
        (struct virtio_gpu_resource_flush *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;

    memset(flush, 0, sizeof(*flush));
    flush->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush->r.x = x;
    flush->r.y = y;
    flush->r.width = width;
    flush->r.height = height;
    flush->resource_id = res->id;
    return virtio_gpu_submit(g, flush, sizeof(*flush), NULL, 0, false, resp,
                             sizeof(*resp), VIRTIO_GPU_RESP_OK_NODATA);
}

/*
 * Fire-and-forget RESOURCE_FLUSH for the bound scanout resource.
 *
 * The synchronous virtio_gpu_resource_flush() path blocks the compositor's
 * present loop on the host's flush acknowledgement, which on the WSL D3D12
 * virgl host costs ~15ms per frame and serialises the desktop at roughly half
 * the application's render rate.  Linux/Alpine instead pipelines: it posts the
 * flush and continues servicing the event loop while the host composites in
 * parallel.  This routes the flush through the single-slot async submit so the
 * present returns immediately; the next ctrlq submission drains it (waiting for
 * the prior flush only if the host has not finished yet).  Gated by the
 * "virtio_gpu_async_scanout_flush" cmdline flag so the synchronous behaviour
 * remains the default.
 */
static int virtio_gpu_resource_flush_async(struct virtio_gpu *g,
                                           struct virtio_gpu_resource *res,
                                           uint32 x, uint32 y, uint32 width,
                                           uint32 height)
{
    struct virtio_gpu_resource_flush *cmd;
    struct virtio_gpu_ctrl_hdr *resp;
    struct virtio_gpu_async_submit prep;

    memset(&prep, 0, sizeof(prep));
    cmd = kalloc();
    resp = kalloc();
    if (cmd == NULL || resp == NULL) {
        if (cmd != NULL)
            kfree(cmd);
        if (resp != NULL)
            kfree(resp);
        return -ENOMEM;
    }

    memset(cmd, 0, sizeof(*cmd));
    memset(resp, 0, sizeof(struct virtio_gpu_ctrl_hdr));
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd->r.x = x;
    cmd->r.y = y;
    cmd->r.width = width;
    cmd->r.height = height;
    cmd->resource_id = res->id;

    prep.ctx_id = 0;
    prep.fence_id = 0;
    prep.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    prep.expected = VIRTIO_GPU_RESP_OK_NODATA;
    prep.cmd = cmd;
    prep.cmd_len = sizeof(*cmd);
    prep.data = NULL;
    prep.data_len = 0;
    prep.data_order = 0;
    prep.resp = resp;
    prep.resp_len = sizeof(struct virtio_gpu_ctrl_hdr);

    if (virtio_gpu_async_post_prepared(
            g, &prep, VIRTIO_GPU_ASYNC_REASON_FLUSH) != 0) {
        virtio_gpu_async_submit_free(&prep);
        return -1;
    }
    return 0;
}

static int virtio_gpu_resource_flush_async_prepare(
    uint32 resource_id, uint32 x, uint32 y, uint32 width, uint32 height,
    struct virtio_gpu_async_submit *prep)
{
    struct virtio_gpu_resource_flush *cmd;
    struct virtio_gpu_ctrl_hdr *resp;

    memset(prep, 0, sizeof(*prep));
    cmd = kalloc();
    resp = kalloc();
    if (cmd == NULL || resp == NULL) {
        if (cmd != NULL)
            kfree(cmd);
        if (resp != NULL)
            kfree(resp);
        return -ENOMEM;
    }

    memset(cmd, 0, sizeof(*cmd));
    memset(resp, 0, sizeof(struct virtio_gpu_ctrl_hdr));
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd->r.x = x;
    cmd->r.y = y;
    cmd->r.width = width;
    cmd->r.height = height;
    cmd->resource_id = resource_id;

    prep->ctx_id = 0;
    prep->fence_id = 0;
    prep->type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    prep->expected = VIRTIO_GPU_RESP_OK_NODATA;
    prep->cmd = cmd;
    prep->cmd_len = sizeof(*cmd);
    prep->data = NULL;
    prep->data_len = 0;
    prep->data_order = 0;
    prep->resp = resp;
    prep->resp_len = sizeof(struct virtio_gpu_ctrl_hdr);
    return 0;
}

/*
 * Fire-and-forget TRANSFER_TO_HOST_3D for the compositor's CPU-damaged chrome.
 *
 * The synchronous transfer path (virtio_gpu_user_transfer) routes through
 * virtio_gpu_submit(), which first drains the entire async ring -- so the small
 * per-frame chrome upload ends up blocking on the previous frame's RESOURCE_FLUSH
 * completing on the host (a ~vsync wait on the single in-order control queue).
 * Posting the transfer asynchronously lets the guest continue encoding the
 * gl-compose command stream while the host drains the queue, so host queue
 * processing overlaps guest CPU work.  The transfer references the resource's
 * already-attached backing pages (no inline data descriptor), exactly like the
 * synchronous TRANSFER_TO_HOST_3D command.  Gated by "virtio_gpu_async_fb_transfer".
 */
static int virtio_gpu_transfer_to_host_3d_async(struct virtio_gpu *g,
                                                struct virtio_gpu_resource *res,
                                                uint32 ctx_id, uint32 x,
                                                uint32 y, uint32 width,
                                                uint32 height, uint32 z,
                                                uint32 depth, uint32 level,
                                                uint64 offset, uint32 stride,
                                                uint32 layer_stride)
{
    struct virtio_gpu_async_submit *a;
    struct virtio_gpu_transfer_host_3d *cmd;
    struct virtio_gpu_ctrl_hdr *resp;
    uint32 transfer_stride;

    if (res == NULL || res->width == 0 || res->height == 0 ||
        width == 0 || height == 0 || depth == 0)
        return -1;

    if (virtio_gpu_async_make_room(g, VIRTIO_GPU_ASYNC_REASON_TRANSFER) != 0)
        return -1;

    cmd = kalloc();
    resp = kalloc();
    if (cmd == NULL || resp == NULL) {
        if (cmd != NULL)
            kfree(cmd);
        if (resp != NULL)
            kfree(resp);
        return -ENOMEM;
    }

    transfer_stride = stride ? stride : res->width * sizeof(uint32);
    memset(cmd, 0, sizeof(*cmd));
    memset(resp, 0, sizeof(struct virtio_gpu_ctrl_hdr));
    cmd->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    cmd->hdr.ctx_id = ctx_id;
    cmd->box.x = x;
    cmd->box.y = y;
    cmd->box.z = z;
    cmd->box.w = width;
    cmd->box.h = height;
    cmd->box.d = depth;
    cmd->offset = offset;
    cmd->resource_id = res->id;
    cmd->level = level;
    cmd->stride = transfer_stride;
    cmd->layer_stride = layer_stride ? layer_stride :
        (uint64)transfer_stride * res->height;

    a = virtio_gpu_async_reserve_slot(g);
    if (a == NULL) {
        kfree(cmd);
        kfree(resp);
        return -1;
    }
    a->ctx_id = ctx_id;
    a->fence_id = 0;
    a->type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    a->expected = VIRTIO_GPU_RESP_OK_NODATA;
    a->cmd = cmd;
    a->cmd_len = sizeof(*cmd);
    a->data = NULL;
    a->data_len = 0;
    a->data_order = 0;
    a->resp = resp;
    a->resp_len = sizeof(struct virtio_gpu_ctrl_hdr);

    virtio_gpu_async_post_locked(g, a);
    return 0;
}

static int virtio_gpu_resource_unref(struct virtio_gpu *g,
                                     struct virtio_gpu_resource *res)
{
    struct virtio_gpu_resource_unref *unref =
        (struct virtio_gpu_resource_unref *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    uint32 id = res->id;
    uint32 backing_len = res->backing_len;
    uint32 backing_order = res->backing_order;
    void *backing = res->backing;
    page_t **pages = res->pages;
    uint32 npages = res->npages;

    if (res->host_visible_mapped &&
        virtio_gpu_resource_unmap_blob(g, res) != 0)
        return -1;

    memset(unref, 0, sizeof(*unref));
    unref->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    unref->resource_id = id;
    if (virtio_gpu_submit(g, unref, sizeof(*unref), NULL, 0, false, resp,
                          sizeof(*resp), VIRTIO_GPU_RESP_OK_NODATA) != 0)
        return -1;

    spin_lock(&g->lock);
    memset(res, 0, sizeof(*res));
    if (g->stats.resources > 0)
        g->stats.resources--;
    if (g->stats.resource_bytes >= backing_len)
        g->stats.resource_bytes -= backing_len;
    spin_unlock(&g->lock);
    if (backing != NULL)
        page_free(backing, backing_order);
    fb_shmem_release_pages(pages, npages);
    return 0;
}

static int virtio_gpu_get_display_info(struct virtio_gpu *g,
                                       uint32 *width, uint32 *height,
                                       int log)
{
    struct virtio_gpu_ctrl_hdr *cmd =
        (struct virtio_gpu_ctrl_hdr *)g->cmd_page;
    struct virtio_gpu_resp_display_info *resp =
        (struct virtio_gpu_resp_display_info *)g->resp_page;
    uint32 first_w = 0, first_h = 0;

    memset(cmd, 0, sizeof(*cmd));
    cmd->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    if (virtio_gpu_submit(g, cmd, sizeof(*cmd), NULL, 0, false, resp,
                          sizeof(*resp),
                          VIRTIO_GPU_RESP_OK_DISPLAY_INFO) != 0)
        return -1;

    if (log)
        printf("virtio_gpu: display info ok");
    for (int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        if (!resp->pmodes[i].enabled)
            continue;
        if (first_w == 0 || first_h == 0) {
            first_w = resp->pmodes[i].r.width;
            first_h = resp->pmodes[i].r.height;
        }
        if (log)
            printf(" scanout%d=%ux%u+%u+%u", i, resp->pmodes[i].r.width,
                   resp->pmodes[i].r.height, resp->pmodes[i].r.x,
                   resp->pmodes[i].r.y);
    }
    if (log)
        printf("\n");
    if (width != NULL)
        *width = first_w;
    if (height != NULL)
        *height = first_h;
    return 0;
}

static int virtio_gpu_submit_display_info(struct virtio_gpu *g)
{
    uint32 width = 0, height = 0;
    int ret = virtio_gpu_get_display_info(g, &width, &height, 1);

    if (ret == 0 && (g->scanout_width == 0 || g->scanout_height == 0)) {
        g->scanout_width = width;
        g->scanout_height = height;
    }
    return ret;
}

static int virtio_gpu_parse_edid_preferred(const uint8 *edid, uint32 size,
                                           uint32 *width, uint32 *height,
                                           uint32 *refresh_millihz)
{
    static const uint8 edid_header[8] =
        {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    uint8 sum = 0;

    if (edid == NULL || size < 128 || memcmp(edid, edid_header, 8) != 0)
        return -1;
    for (uint32 i = 0; i < 128; i++)
        sum = (uint8)(sum + edid[i]);
    if (sum != 0)
        return -1;

    for (uint32 off = 54; off + 18 <= 126; off += 18) {
        const uint8 *d = edid + off;
        uint32 pixel_clock = (uint32)d[0] | ((uint32)d[1] << 8);
        uint32 hactive;
        uint32 hblank;
        uint32 vactive;
        uint32 vblank;
        uint64 total;

        if (pixel_clock == 0)
            continue;
        hactive = (uint32)d[2] | (((uint32)d[4] & 0xf0) << 4);
        hblank = (uint32)d[3] | (((uint32)d[4] & 0x0f) << 8);
        vactive = (uint32)d[5] | (((uint32)d[7] & 0xf0) << 4);
        vblank = (uint32)d[6] | (((uint32)d[7] & 0x0f) << 8);
        if (hactive < 320 || hactive > 8192 ||
            vactive < 200 || vactive > 4320 ||
            hblank == 0 || vblank == 0)
            continue;
        total = (uint64)(hactive + hblank) * (uint64)(vactive + vblank);
        if (total == 0)
            continue;
        if (width != NULL)
            *width = hactive;
        if (height != NULL)
            *height = vactive;
        if (refresh_millihz != NULL)
            *refresh_millihz = (uint32)(((uint64)pixel_clock * 10000000ULL +
                                         total / 2) / total);
        return 0;
    }
    return -1;
}

static int virtio_gpu_get_edid_mode(struct virtio_gpu *g, uint32 *width,
                                    uint32 *height, uint32 *refresh_millihz,
                                    int log)
{
    struct virtio_gpu_cmd_get_edid *cmd =
        (struct virtio_gpu_cmd_get_edid *)g->cmd_page;
    struct virtio_gpu_resp_edid *resp =
        (struct virtio_gpu_resp_edid *)g->resp_page;
    uint32 w = 0, h = 0, hz = 0;

    if (!(g->driver_features0 & (1u << VIRTIO_GPU_F_EDID)))
        return -1;

    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_GET_EDID;
    cmd->scanout = 0;

    if (virtio_gpu_submit(g, cmd, sizeof(*cmd), NULL, 0, false, resp,
                          sizeof(*resp), VIRTIO_GPU_RESP_OK_EDID) != 0)
        return -1;
    if (resp->size > sizeof(resp->edid))
        resp->size = sizeof(resp->edid);
    if (virtio_gpu_parse_edid_preferred(resp->edid, resp->size,
                                        &w, &h, &hz) != 0)
        return -1;
    if (width != NULL)
        *width = w;
    if (height != NULL)
        *height = h;
    if (refresh_millihz != NULL)
        *refresh_millihz = hz;
    g->edid_width = w;
    g->edid_height = h;
    g->edid_refresh_millihz = hz;
    if (log)
        printf("virtio_gpu: edid preferred %ux%u@%u.%03uHz\n",
               w, h, hz / 1000, hz % 1000);
    return 0;
}

static int virtio_gpu_submit_capset(struct virtio_gpu *g, uint32 capset_id,
                                    uint32 version, uint32 max_size, void *out)
{
    struct virtio_gpu_get_capset *cmd =
        (struct virtio_gpu_get_capset *)g->cmd_page;
    struct virtio_gpu_resp_capset *resp =
        (struct virtio_gpu_resp_capset *)g->resp_page;
    uint32 data_len = max_size;

    if (data_len > PGSIZE - sizeof(*resp))
        data_len = PGSIZE - sizeof(*resp);

    memset(cmd, 0, sizeof(*cmd));
    memset(resp, 0, PGSIZE);
    cmd->hdr.type = VIRTIO_GPU_CMD_GET_CAPSET;
    cmd->capset_id = capset_id;
    cmd->capset_version = version;

    int ret = virtio_gpu_submit(g, cmd, sizeof(*cmd), NULL, 0, false, resp,
                                sizeof(*resp) + data_len,
                                VIRTIO_GPU_RESP_OK_CAPSET);
    if (ret == 0 && out != NULL)
        memcpy(out, resp->capset_data, data_len);
    return ret;
}

static void virtio_gpu_query_capsets(struct virtio_gpu *g)
{
    uint32 num_capsets = g->config->num_capsets;

    g->num_capsets = num_capsets;
    if (num_capsets == 0) {
        printf("virtio_gpu: no 3D capsets advertised\n");
        return;
    }

    printf("virtio_gpu: querying %u capset(s)\n", num_capsets);
    for (uint32 i = 0; i < num_capsets && i < VIRTIO_GPU_MAX_CAPSETS; i++) {
        struct virtio_gpu_get_capset_info *cmd =
            (struct virtio_gpu_get_capset_info *)g->cmd_page;
        struct virtio_gpu_resp_capset_info *resp =
            (struct virtio_gpu_resp_capset_info *)g->resp_page;

        memset(cmd, 0, sizeof(*cmd));
        memset(resp, 0, sizeof(*resp));
        cmd->hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
        cmd->capset_index = i;

        if (virtio_gpu_submit(g, cmd, sizeof(*cmd), NULL, 0, false, resp,
                              sizeof(*resp),
                              VIRTIO_GPU_RESP_OK_CAPSET_INFO) != 0)
            continue;

        printf("virtio_gpu: capset[%u] id=%u version=%u size=%u\n",
               i, resp->capset_id, resp->capset_max_version,
               resp->capset_max_size);

        if (virtio_gpu_capset_supported(resp->capset_id)) {
            uint32 capset_id = resp->capset_id;
            uint32 capset_version = resp->capset_max_version;
            uint32 capset_size = resp->capset_max_size;

            if (virtio_gpu_submit_capset(g, capset_id, capset_version,
                                         capset_size, NULL) == 0) {
                spin_lock(&g->lock);
                g->capsets[i].valid = 1;
                g->capsets[i].id = capset_id;
                g->capsets[i].version = capset_version;
                g->capsets[i].size = capset_size;
                if (virtio_gpu_capset_preferred(g->virgl_capset_id,
                                                g->virgl_capset_version,
                                                capset_id, capset_version)) {
                    g->virgl_capset_id = capset_id;
                    g->virgl_capset_version = capset_version;
                    g->virgl_capset_size = capset_size;
                    g->stats.virgl = capset_id;
                    g->stats.virgl_version = capset_version;
                    g->stats.virgl_size = capset_size;
                }
                spin_unlock(&g->lock);
                printf("virtio_gpu: virgl capset ready id=%u version=%u size=%u\n",
                       capset_id, capset_version, capset_size);
            }
        }
    }

    if (num_capsets > VIRTIO_GPU_MAX_CAPSETS)
        printf("virtio_gpu: capset list truncated at %u entries\n",
               VIRTIO_GPU_MAX_CAPSETS);
    if (g->virgl_capset_id == 0)
        printf("virtio_gpu: no virgl capset found\n");
