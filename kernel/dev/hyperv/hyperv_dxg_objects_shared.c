static int hvdxg_read(cdev_t *cdev, bool user, void *buf, size_t count)
{
    return hvdxg_read_status(cdev, user, buf, count, &hvdxg.read_offset,
                             &hvdxg.read_emitted, NULL, NULL);
}

static void hvdxg_command_vm_init(struct hvdxg_command_vm_to_host *hdr,
                                  uint32 command_type);
static int hvdxg_send_sync_global(const void *cmd, uint32 cmd_len,
                                  void *result, uint32 result_len,
                                  uint32 *actual_len_out);

static uint64 hvdxg_align_up(uint64 value, uint64 align)
{
    return (value + align - 1) & ~(align - 1);
}

static int hvdxg_range_overlaps(uint64 base, uint64 size,
                                uint64 other_base, uint64 other_size,
                                uint64 *other_end_out)
{
    uint64 end = base + size;
    uint64 other_end = other_base + other_size;

    if (other_end_out != NULL)
        *other_end_out = other_end;
    if (size == 0 || other_size == 0 || end <= base ||
        other_end <= other_base)
        return 0;
    return base < other_end && other_base < end;
}

static uint64 hvdxg_pick_iospace_base(uint64 size)
{
    uint64 base = HV_DXG_IOSPACE_SEARCH_START;

    if (size == 0 || size > HV_DXG_IOSPACE_SEARCH_END - base)
        return 0;
    base = hvdxg_align_up(base, HV_DXG_IOSPACE_ALIGN);
    while (base + size > base && base + size <= HV_DXG_IOSPACE_SEARCH_END) {
        uint64 next = 0;

        for (int i = 0; i < platform.mem_count; i++) {
            uint64 other_end = 0;
            if (hvdxg_range_overlaps(base, size, platform.mem[i].base,
                                     platform.mem[i].size, &other_end) &&
                other_end > next)
                next = other_end;
        }
        for (int i = 0; i < platform.reserved_count; i++) {
            uint64 other_end = 0;
            if (hvdxg_range_overlaps(base, size, platform.reserved[i].base,
                                     platform.reserved[i].size, &other_end) &&
                other_end > next)
                next = other_end;
        }
        if (platform.has_framebuffer) {
            uint64 other_end = 0;
            if (hvdxg_range_overlaps(base, size, platform.framebuffer_base,
                                     platform.framebuffer_size,
                                     &other_end) && other_end > next)
                next = other_end;
        }
        if (next == 0)
            return base;
        base = hvdxg_align_up(next, HV_DXG_IOSPACE_ALIGN);
    }
    return 0;
}

static int hvdxg_set_iospace_region(void)
{
    struct hvdxg_command_setiospaceregion cmd;
    struct hvdxg_ntstatus status;
    uint32 actual_len = 0;
    uint64 size;
    int ret;

    if (hvdxg.iospace_set)
        return 0;
    if (hvdxg.global_mmio_megabytes == 0) {
        hvdxg.iospace_last_ret = -ENOMEM;
        return -ENOMEM;
    }
    size = (uint64)hvdxg.global_mmio_megabytes << 20;
    size = hvdxg_align_up(size, HV_DXG_IOSPACE_ALIGN);
    hvdxg.iospace_base = hvdxg_pick_iospace_base(size);
    hvdxg.iospace_size = size;
    if (hvdxg.iospace_base == 0) {
        hvdxg.iospace_last_ret = -ENOMEM;
        return -ENOMEM;
    }

    memset(&cmd, 0, sizeof(cmd));
    memset(&status, 0, sizeof(status));
    hvdxg_command_vm_init(&cmd.hdr,
                          HV_DXGK_VMBCOMMAND_SETIOSPACEREGION);
    cmd.start = hvdxg.iospace_base;
    cmd.length = hvdxg.iospace_size;
    ret = hvdxg_send_sync_global(&cmd, sizeof(cmd), &status,
                                 sizeof(status), &actual_len);
    hvdxg.iospace_last_len = actual_len;
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg.iospace_last_ret = ret;
    if (ret == 0)
        hvdxg.iospace_set = 1;
    return ret;
}

static int hvdxg_iospace_contains(uint64 pa, uint64 size)
{
    uint64 end;

    if (!hvdxg.iospace_set || size == 0)
        return 0;
    end = pa + size;
    if (end < pa)
        return 0;
    return pa >= hvdxg.iospace_base &&
           end <= hvdxg.iospace_base + hvdxg.iospace_size;
}

enum {
    HV_DXG_FENCE_MAP_NONE = 0,
    HV_DXG_FENCE_MAP_ABSOLUTE = 1,
    HV_DXG_FENCE_MAP_OFFSET = 2,
};

enum {
    HV_DXG_FENCE_SOURCE_PAGINGQUEUE = 1,
    HV_DXG_FENCE_SOURCE_SYNCOBJECT = 2,
    HV_DXG_FENCE_SOURCE_HWQUEUE = 3,
    HV_DXG_FENCE_SOURCE_OPEN_SYNC = 4,
    HV_DXG_FENCE_SOURCE_LOCK2 = 5,
};

static uint64 hvdxg_map_iospace_user(uint64 pa, uint64 size,
                                     uint64 extra_pte_flags);
static uint64 hvdxg_map_iospace_kernel(uint64 pa, uint64 size);
static uint64 hvdxg_iospace_user_map_size(uint64 user_va, uint64 size)
{
    uint64 page_off = user_va & (PGSIZE - 1);

    if (user_va == 0 || size == 0)
        return 0;
    return PGROUNDUP(page_off + size);
}

static uint64 hvdxg_allocation_num_pages(uint64 size)
{
    if (size == 0)
        return 0;
    return PGROUNDUP(size) >> PGSHIFT;
}

static uint64 hvdxg_canonical_iospace_pa(uint64 raw_pa, uint64 size,
                                         int allow_offset, uint32 *mode)
{
    uint64 pa;

    if (mode != NULL)
        *mode = HV_DXG_FENCE_MAP_NONE;
    if (size == 0 || !hvdxg.iospace_set)
        return 0;
    if (hvdxg_iospace_contains(raw_pa, size)) {
        if (mode != NULL)
            *mode = HV_DXG_FENCE_MAP_ABSOLUTE;
        return raw_pa;
    }
    if (!allow_offset || raw_pa >= hvdxg.iospace_size)
        return 0;
    pa = hvdxg.iospace_base + raw_pa;
    if (pa < hvdxg.iospace_base || !hvdxg_iospace_contains(pa, size))
        return 0;
    if (mode != NULL)
        *mode = HV_DXG_FENCE_MAP_OFFSET;
    return pa;
}

static uint64 hvdxg_map_iospace_user_canonical(uint32 source, uint64 raw_pa,
                                               uint64 size,
                                               uint64 extra_pte_flags,
                                               uint64 *canonical_pa,
                                               uint32 *mode,
                                               int allow_offset)
{
    uint64 pa;
    uint64 user_va = 0;
    uint32 map_mode = HV_DXG_FENCE_MAP_NONE;

    pa = hvdxg_canonical_iospace_pa(raw_pa, size, allow_offset, &map_mode);
    if (canonical_pa != NULL)
        *canonical_pa = pa;
    if (mode != NULL)
        *mode = map_mode;
    if (source != HV_DXG_FENCE_SOURCE_LOCK2) {
        hvdxg.fence_map_last_source = source;
        hvdxg.fence_map_last_mode = map_mode;
        hvdxg.fence_map_last_raw_pa = raw_pa;
        hvdxg.fence_map_last_canonical_pa = pa;
        hvdxg.fence_map_last_offset = 0;
        hvdxg.fence_map_last_offset_candidate_pa = 0;
        hvdxg.fence_map_last_offset_candidate_current = 0;
        hvdxg.fence_map_last_size = size;
        hvdxg.fence_map_last_user_va = 0;
        hvdxg.fence_map_last_kva = 0;
    }
    if (pa != 0)
        user_va = hvdxg_map_iospace_user(pa, size, extra_pte_flags);
    if (source != HV_DXG_FENCE_SOURCE_LOCK2)
        hvdxg.fence_map_last_user_va = user_va;
    if (source != HV_DXG_FENCE_SOURCE_LOCK2 && user_va == 0)
        hvdxg.fence_map_failures++;
    return user_va;
}

static uint64 hvdxg_map_iospace_kernel_canonical(uint32 source, uint64 raw_pa,
                                                 uint64 size,
                                                 uint64 canonical_pa,
                                                 int allow_offset)
{
    uint64 pa = canonical_pa;
    uint64 kva = 0;

    if (pa == 0)
        pa = hvdxg_canonical_iospace_pa(raw_pa, size, allow_offset, NULL);
    if (pa != 0)
        kva = hvdxg_map_iospace_kernel(pa, size);
    if (source != HV_DXG_FENCE_SOURCE_LOCK2)
        hvdxg.fence_map_last_kva = kva;
    return kva;
}

#ifdef PTE_PWT
#define HV_DXG_PTE_WRITE_THROUGH PTE_PWT
#else
#define HV_DXG_PTE_WRITE_THROUGH 0
#endif

static uint64 hvdxg_map_iospace_user(uint64 pa, uint64 size,
                                     uint64 extra_pte_flags)
{
    vm_t *vm;
    vma_t *vma;
    uint64 page_pa;
    uint64 page_off;
    uint64 map_size;
    uint64 addr;
    uint64 flags;
    uint64 pte_flags;

    if (current == NULL || current->vm == NULL)
        return 0;
    page_pa = PGROUNDDOWN(pa);
    page_off = pa - page_pa;
    map_size = PGROUNDUP(page_off + size);
    if (map_size == 0 || !hvdxg_iospace_contains(page_pa, map_size))
        return 0;

    vm = current->vm;
    flags = PROT_READ | PROT_WRITE | VMA_FLAG_USER |
            VMA_FLAG_DONTFORK | VMA_FLAG_DONTDUMP | VMA_FLAG_PFNMAP;

    vm_wlock(vm);
    addr = vm_find_free_range(vm, (size_t)map_size, 0);
    if (addr == 0) {
        vm_wunlock(vm);
        return 0;
    }
    vma = vma_alloc(vm, addr, map_size, flags);
    if (vma == NULL) {
        vm_wunlock(vm);
        return 0;
    }
    pte_flags = vma2pte_flags(flags);
    pte_flags |= extra_pte_flags;
    for (uint64 off = 0; off < map_size; off += PGSIZE) {
        if (mappages(vm->pagetable, addr + off, PGSIZE,
                     page_pa + off, pte_flags) != 0) {
            vma_free(vm, vma);
            vm_wunlock(vm);
            return 0;
        }
    }
    vm_wunlock(vm);
    return addr + page_off;
}

static uint64 hvdxg_map_iospace_kernel(uint64 pa, uint64 size)
{
    uint64 page_pa = PGROUNDDOWN(pa);
    uint64 page_off = pa - page_pa;
    uint64 map_size = PGROUNDUP(page_off + size);
    uint64 map_base;
    vma_t *vma;

    if (map_size == 0 || !hvdxg_iospace_contains(page_pa, map_size) ||
        kernel_vm == NULL)
        return 0;

    vm_wlock(kernel_vm);
    map_base = vm_find_free_range(kernel_vm, (size_t)map_size, 0);
    if (map_base == 0) {
        vm_wunlock(kernel_vm);
        return 0;
    }
    vma = vma_alloc(kernel_vm, map_base, map_size,
                    PROT_READ | PROT_WRITE | VMA_FLAG_KERNEL |
                    VMA_FLAG_PFNMAP);
    vm_wunlock(kernel_vm);
    if (vma == NULL)
        return 0;

    for (uint64 off = 0; off < map_size; off += PGSIZE) {
        if (arch_vm_map(kernel_pagetable, map_base + off, PGSIZE,
                        page_pa + off,
                        PTE_R | PTE_W) != 0)
            return 0;
    }
    arch_tlb_flush();
    return map_base + page_off;
}

static void hvdxg_note_fence_offset_candidate(uint32 source, uint64 direct_pa,
                                              uint64 offset, uint64 size)
{
    uint64 candidate;
    uint64 candidate_kva;

    if (source == HV_DXG_FENCE_SOURCE_LOCK2 ||
        source != hvdxg.fence_map_last_source)
        return;
    hvdxg.fence_map_last_offset = offset;
    hvdxg.fence_map_last_offset_candidate_pa = 0;
    hvdxg.fence_map_last_offset_candidate_current = 0;
    if (offset == 0)
        return;
    candidate = direct_pa + offset;
    if (candidate < direct_pa)
        return;
    hvdxg.fence_map_last_offset_candidate_pa = candidate;
    candidate_kva = hvdxg_map_iospace_kernel(candidate, size);
    if (candidate_kva != 0) {
        volatile uint64 *candidate_value = (volatile uint64 *)candidate_kva;

        hvdxg.fence_map_last_offset_candidate_current = *candidate_value;
    }
}

static void hvdxg_unpin_existing_sysmem_pages(uint64 *pages,
                                              uint32 page_count)
{
    if (pages == NULL)
        return;
    for (uint32 i = 0; i < page_count; i++) {
        if (pages[i] != 0 && __pa_to_page(pages[i]) != NULL) {
            (void)page_ref_dec((void *)pages[i]);
            if (hvdxg.existing_sysmem_active_pages != 0)
                hvdxg.existing_sysmem_active_pages--;
            hvdxg.existing_sysmem_unpin_events++;
        }
    }
    kvfree(pages);
}

static void hvdxg_unpin_tracked_allocation(
    struct hvdxg_tracked_allocation *a)
{
    if (a == NULL || a->sysmem_pages == NULL)
        return;
    hvdxg_unpin_existing_sysmem_pages(a->sysmem_pages,
                                      a->sysmem_page_count);
    a->sysmem_pages = NULL;
    a->sysmem_page_count = 0;
    a->sysmem = 0;
}

static int hvdxg_pin_user_page(vm_t *vm, uint64 uva, int writable,
                               uint64 *out_pa, uint32 *pfnmap_out)
{
    uint8 touch = 0;
    uint64 va = PGROUNDDOWN(uva);
    uint64 pa;
    vma_t *vma;
    int pfnmap = 0;
    int ret = 0;

    if (vm == NULL || out_pa == NULL || va >= UVMTOP)
        return -EFAULT;
    if (pfnmap_out != NULL)
        *pfnmap_out = 0;
    if (vm_copyin(vm, &touch, va, sizeof(touch)) < 0)
        return -EFAULT;
    if (writable && vm_copyout(vm, va, &touch, sizeof(touch)) < 0)
        return -EFAULT;

    vm_rlock(vm);
    vma = vm_find_area(vm, va);
    if (vma == NULL ||
        vma_validate(vma, va, PGSIZE,
                     VMA_FLAG_USER | PROT_READ |
                     (writable ? PROT_WRITE : 0)) != 0) {
        ret = -EFAULT;
        goto out;
    }
    pfnmap = (vma->flags & VMA_FLAG_PFNMAP) != 0;
    pa = walkaddr(vm->pagetable, va);
    if (pa == 0) {
        ret = -EFAULT;
        goto out;
    }
    pa = PGROUNDDOWN(pa);
    if (pfnmap) {
        *out_pa = pa;
        if (pfnmap_out != NULL)
            *pfnmap_out = 1;
        goto out;
    }
    if (page_ref_inc((void *)pa) <= 0) {
        ret = -EFAULT;
        goto out;
    }
    *out_pa = pa;
out:
    vm_runlock(vm);
    return ret;
}

static int hvdxg_pin_existing_sysmem(uint64 sysmem, uint64 alloc_size,
                                     int writable, uint64 **pages_out,
                                     uint32 *page_count_out,
                                     uint32 *pfnmap_pages_out)
{
    uint64 page_count64;
    uint64 *pages = NULL;
    uint32 page_count;
    uint32 pfnmap_pages = 0;

    if (pages_out == NULL || page_count_out == NULL || current == NULL ||
        current->vm == NULL || sysmem == 0 || alloc_size == 0 ||
        (sysmem & (PGSIZE - 1)) != 0 ||
        (alloc_size & (PGSIZE - 1)) != 0)
        return -EINVAL;
    *pages_out = NULL;
    *page_count_out = 0;
    if (pfnmap_pages_out != NULL)
        *pfnmap_pages_out = 0;
    page_count64 = alloc_size >> PGSHIFT;
    if (page_count64 == 0 || page_count64 > 0xffffffffULL)
        return -EINVAL;
    page_count = (uint32)page_count64;
    pages = kvmalloc((size_t)page_count * sizeof(pages[0]));
    if (pages == NULL)
        return -ENOMEM;
    memset(pages, 0, (size_t)page_count * sizeof(pages[0]));
    for (uint32 i = 0; i < page_count; i++) {
        uint32 pfnmap = 0;
        int ret = hvdxg_pin_user_page(current->vm,
                                      sysmem + (uint64)i * PGSIZE,
                                      writable, &pages[i], &pfnmap);

        if (ret != 0) {
            hvdxg_unpin_existing_sysmem_pages(pages, i);
            return ret;
        }
        if (pfnmap)
            pfnmap_pages++;
        else if (__pa_to_page(pages[i]) != NULL) {
            hvdxg.existing_sysmem_active_pages++;
            hvdxg.existing_sysmem_pin_events++;
        }
    }
    *pages_out = pages;
    *page_count_out = page_count;
    if (pfnmap_pages_out != NULL)
        *pfnmap_pages_out = pfnmap_pages;
    return 0;
}

static int hvdxg_set_existing_sysmem_pages(uint32 device, uint32 allocation,
                                           const uint64 *pages,
                                           uint32 page_count)
{
    uint8 command_buf[sizeof(struct hvdxg_command_setexistingsysmempages) +
                      (HV_DXG_SYSMEM_PFNS_PER_PACKET - 1) *
                          sizeof(uint64)];
    struct hvdxg_command_setexistingsysmempages *set =
        (struct hvdxg_command_setexistingsysmempages *)command_buf;
    struct hvdxg_ntstatus status;
    uint64 offset = 0;
    uint32 actual_len = 0;
    int ret = 0;

    if (device == 0 || allocation == 0 || pages == NULL || page_count == 0)
        return -EINVAL;

    while (offset < page_count) {
        uint32 n = page_count - (uint32)offset;
        uint32 command_len;

        if (n > HV_DXG_SYSMEM_PFNS_PER_PACKET)
            n = HV_DXG_SYSMEM_PFNS_PER_PACKET;
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(
            &set->hdr, HV_DXGK_VMBCOMMAND_SETEXISTINGSYSMEMPAGES,
            hvdxg.dxg_process);
        set->device.v = device;
        set->allocation.v = allocation;
        set->num_pages = n;
        set->alloc_offset_in_pages = (uint32)offset;
        for (uint32 i = 0; i < n; i++)
            set->pfn[i] = pages[offset + i] >> PGSHIFT;
        command_len = sizeof(*set) + (n - 1) * sizeof(uint64);
        ret = hvdxg_send_sync_vgpu(set, command_len, &status,
                                   sizeof(status), &actual_len);
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        if (ret != 0)
            return ret;
        offset += n;
    }
    return 0;
}

static uint32 hvdxg_device_for_allocation(uint32 allocation)
{
    if (allocation != 0 && allocation == hvdxg.last_allocation_handle)
        return hvdxg.last_allocation_device;
    return hvdxg.last_device_handle;
}

static void hvdxg_untrack_u32(uint32 *items, uint32 *count, uint32 value)
{
    if (value == 0 || count == NULL || items == NULL)
        return;
    for (uint32 i = 0; i < *count; i++) {
        if (items[i] == value) {
            items[i] = items[*count - 1];
            items[*count - 1] = 0;
            (*count)--;
            return;
        }
    }
}

static int hvdxg_grow_table(void **items, uint32 *capacity, uint32 need,
                            size_t item_size, uint32 initial_capacity)
{
    void *new_items;
    uint32 new_capacity;
    size_t old_size;
    size_t new_size;

    if (items == NULL || capacity == NULL || item_size == 0)
        return -EINVAL;
    if (*capacity >= need)
        return 0;

    new_capacity = *capacity != 0 ? *capacity : initial_capacity;
    if (new_capacity == 0)
        new_capacity = 16;
    while (new_capacity < need) {
        if (new_capacity > 0xffffffffU / 2)
            return -EOVERFLOW;
        new_capacity *= 2;
    }
    if ((size_t)new_capacity > ((size_t)-1) / item_size)
        return -EOVERFLOW;
    old_size = (size_t)(*capacity) * item_size;
    new_size = (size_t)new_capacity * item_size;
    new_items = kvmalloc(new_size);
    if (new_items == NULL)
        return -ENOMEM;
    memset(new_items, 0, new_size);
    if (*items != NULL && old_size != 0) {
        memcpy(new_items, *items, old_size);
        kvfree(*items);
    }
    *items = new_items;
    *capacity = new_capacity;
    return 0;
}

static int hvdxg_track_u32_grow(uint32 **items, uint32 *count,
                                uint32 *capacity, uint32 value)
{
    int ret;

    if (value == 0 || items == NULL || count == NULL || capacity == NULL)
        return 0;
    for (uint32 i = 0; i < *count; i++) {
        if ((*items)[i] == value)
            return 0;
    }
    ret = hvdxg_grow_table((void **)items, capacity, *count + 1,
                           sizeof((*items)[0]), HV_DXG_OPEN_TRACKED_MAX);
    if (ret != 0)
        return ret;
    (*items)[(*count)++] = value;
    return 0;
}

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
    if (adapter == NULL || device == 0)
        return 0;
    return hvdxg_track_u32_grow(&adapter->devices,
                                &adapter->device_count,
                                &adapter->device_capacity,
                                device);
}

static void hvdxg_process_adapter_remove_device(
    struct hvdxg_process_state *process, uint32 device)
{
    if (process == NULL || process->adapters == NULL || device == 0)
        return;
    for (uint32 i = 0; i < process->adapter_count; i++)
        hvdxg_untrack_u32(process->adapters[i].devices,
                          &process->adapters[i].device_count, device);
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
    hvdxg_note_object_free_list(owner);
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
    hvdxg_note_object_free_list(owner);
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

static int hvdxg_shared_object_stat(struct vfs_file *file, struct stat *st)
{
    struct hvdxg_shared_object *shared;

    if (file == NULL || st == NULL)
        return -EINVAL;
    shared = (struct hvdxg_shared_object *)file->private_data;
    if (shared == NULL)
        return -EINVAL;

    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0600;
    st->st_nlink = 1;
    st->st_ino = shared->kind == HV_DXG_SHARED_OBJECT_SYNC ?
        0x64786701ULL : 0x64786702ULL;
    st->st_blksize = 4096;
    return 0;
}

static ssize_t hvdxg_shared_object_readlink(struct vfs_file *file, char *buf,
                                            size_t buflen)
{
    struct hvdxg_shared_object *shared;
    const char *name;

    if (file == NULL || buf == NULL)
        return -EINVAL;
    shared = (struct hvdxg_shared_object *)file->private_data;
    if (shared == NULL)
        return -EINVAL;

    name = shared->kind == HV_DXG_SHARED_OBJECT_SYNC ?
        "anon_inode:dxgsyncobj" : "anon_inode:dxgresource";
    return snprintf(buf, buflen, "%s", name);
}

static struct vfs_file_ops hvdxg_shared_sync_file_ops = {
    .stat = hvdxg_shared_object_stat,
    .readlink = hvdxg_shared_object_readlink,
    .release = hvdxg_shared_object_release,
    .early_release_on_close = 1,
};

static struct vfs_file_ops hvdxg_shared_resource_file_ops = {
    .stat = hvdxg_shared_object_stat,
    .readlink = hvdxg_shared_object_readlink,
    .release = hvdxg_shared_object_release,
    .early_release_on_close = 1,
};

static struct vfs_file_ops hvdxg_file_ops;

static uint32 hvdxg_shared_object_fops_kind(struct vfs_file *file)
{
    if (file == NULL)
        return HV_DXG_SHARED_FOPS_NONE;
    if (file->ops == &hvdxg_shared_sync_file_ops)
        return HV_DXG_SHARED_FOPS_SYNC;
    if (file->ops == &hvdxg_shared_resource_file_ops)
        return HV_DXG_SHARED_FOPS_RESOURCE;
    return HV_DXG_SHARED_FOPS_NONE;
}

enum {
    HV_DXG_BIND_PIN_REASON_OK = 0,
    HV_DXG_BIND_PIN_REASON_BAD_ARGS = 1,
    HV_DXG_BIND_PIN_REASON_NO_FDTABLE = 2,
    HV_DXG_BIND_PIN_REASON_NO_SHARED_FD = 3,
    HV_DXG_BIND_PIN_REASON_BAD_SHARED_FD = 4,
    HV_DXG_BIND_PIN_REASON_BAD_PARENT = 5,
    HV_DXG_BIND_PIN_REASON_BAD_DXG_FD = 6,
    HV_DXG_BIND_PIN_REASON_NO_MATCHING_RESOURCE = 7,
};

static void hvdxg_note_display_bind_pin_diag(
    int ret, uint32 reason, int dxg_fd, int resource_fd, uint32 device,
    uint32 resource, uint32 allocation, uint32 allocation_count,
    struct hvdxg_shared_object *shared, uint32 fops_kind,
    struct hvdxg_tracked_resource *parent_resource,
    struct hvdxg_open_state *owner, struct hvdxg_tracked_resource *opened,
    uint32 selected)
{
    hvdxg.display_bind_pin_last_ret = ret;
    hvdxg.display_bind_pin_last_reason = reason;
    hvdxg.display_bind_pin_last_dxg_fd = dxg_fd;
    hvdxg.display_bind_pin_last_resource_fd = resource_fd;
    hvdxg.display_bind_pin_last_device = device;
    hvdxg.display_bind_pin_last_resource = resource;
    hvdxg.display_bind_pin_last_allocation = allocation;
    hvdxg.display_bind_pin_last_allocation_count = allocation_count;
    hvdxg.display_bind_pin_last_shared_kind = shared != NULL ?
        shared->kind : 0;
    hvdxg.display_bind_pin_last_fops_kind = fops_kind;
    hvdxg.display_bind_pin_last_shared_global = shared != NULL ?
        shared->global_share : 0;
    hvdxg.display_bind_pin_last_parent_present =
        parent_resource != NULL;
    hvdxg.display_bind_pin_last_parent_device =
        parent_resource != NULL ? parent_resource->device : 0;
    hvdxg.display_bind_pin_last_parent_resource =
        parent_resource != NULL ? parent_resource->resource : 0;
    hvdxg.display_bind_pin_last_parent_allocation =
        parent_resource != NULL &&
        parent_resource->allocation_count != 0 ?
        parent_resource->allocation_handles[0] : 0;
    hvdxg.display_bind_pin_last_parent_allocation_count =
        parent_resource != NULL ? parent_resource->allocation_count : 0;
    hvdxg.display_bind_pin_last_parent_global =
        parent_resource != NULL ? parent_resource->global_share : 0;
    hvdxg.display_bind_pin_last_parent_sealed =
        parent_resource != NULL ? parent_resource->sealed : 0;
    hvdxg.display_bind_pin_last_parent_records =
        parent_resource != NULL ?
        parent_resource->shared_records_valid : 0;
    hvdxg.display_bind_pin_last_parent_generation =
        parent_resource != NULL ? parent_resource->sealed_generation : 0;
    hvdxg.display_bind_pin_last_dxg_file_ok = owner != NULL;
    hvdxg.display_bind_pin_last_owner_process =
        owner != NULL ? hvdxg_open_host_process(owner) : 0;
    hvdxg.display_bind_pin_last_owner_generation =
        owner != NULL ? hvdxg_open_process_generation(owner) : 0;
    hvdxg.display_bind_pin_last_owner_refs =
        owner != NULL ? hvdxg_open_process_refs(owner) : 0;
    hvdxg.display_bind_pin_last_opened_present = opened != NULL;
    hvdxg.display_bind_pin_last_opened_device =
        opened != NULL ? opened->device : 0;
    hvdxg.display_bind_pin_last_opened_resource =
        opened != NULL ? opened->resource : 0;
    hvdxg.display_bind_pin_last_opened_allocation =
        opened != NULL && opened->allocation_count != 0 ?
        opened->allocation_handles[0] : 0;
    hvdxg.display_bind_pin_last_opened_allocation_count =
        opened != NULL ? opened->allocation_count : 0;
    hvdxg.display_bind_pin_last_opened_global =
        opened != NULL ? opened->global_share : 0;
    hvdxg.display_bind_pin_last_selected = selected;
}

static struct hvdxg_shared_object *hvdxg_shared_object_from_fd(
    int fd, uint32 kind, struct vfs_file **file_out)
{
    struct vfs_file *f;
    struct hvdxg_shared_object *shared;

    if (file_out != NULL)
        *file_out = NULL;
    if (fd < 0 || current == NULL || current->fdtable == NULL)
        return NULL;
    f = vfs_fdtable_get_file(current->fdtable, fd);
    if (f == NULL)
        return NULL;
    if ((f->ops != &hvdxg_shared_sync_file_ops &&
         f->ops != &hvdxg_shared_resource_file_ops) ||
        f->private_data == NULL) {
        vfs_fput(f);
        return NULL;
    }
    shared = (struct hvdxg_shared_object *)f->private_data;
    if (kind != 0 && shared->kind != kind) {
        vfs_fput(f);
        return NULL;
    }
    if (file_out != NULL)
        *file_out = f;
    else
        vfs_fput(f);
    return shared;
}

int hyperv_dxg_shared_resource_snapshot_from_fd(
    int fd, struct hyperv_dxg_shared_resource_snapshot *snapshot)
{
    struct hvdxg_shared_object *shared;
    struct vfs_file *file = NULL;
    struct hvdxg_tracked_resource *resource;

    if (snapshot == NULL)
        return -EINVAL;
    memset(snapshot, 0, sizeof(*snapshot));

    shared = hvdxg_shared_object_from_fd(fd, 0, &file);
    if (shared == NULL)
        return -EINVAL;

    snapshot->kind = shared->kind;
    snapshot->fops_kind = hvdxg_shared_object_fops_kind(file);
    if (shared->kind != HV_DXG_SHARED_OBJECT_RESOURCE) {
        vfs_fput(file);
        return -EINVAL;
    }

    resource = hvdxg_shared_parent_resource(shared);
    if (resource == NULL) {
        vfs_fput(file);
        return -EINVAL;
    }
    snapshot->device = resource->device;
    snapshot->resource = resource->resource;
    snapshot->allocation_count = resource->allocation_count;
    snapshot->first_allocation = resource->allocation_count != 0 ?
        resource->allocation_handles[0] : 0;
    snapshot->sealed = resource->sealed;
    snapshot->shared_records_valid = resource->shared_records_valid;
    snapshot->generation = resource->sealed_generation;
    snapshot->host_shared_refs = resource->host_shared_refs;
    if (shared->resource_parent != NULL) {
        snapshot->shared_parent_id = shared->resource_parent->id;
        snapshot->shared_parent_refs = shared->resource_parent->refs;
        snapshot->shared_parent_children = shared->resource_parent->child_count;
    } else {
        snapshot->shared_parent_id = shared->parent_id;
        snapshot->shared_parent_refs = shared->parent_refs;
        snapshot->shared_parent_children = shared->opened_child_count;
    }
    vfs_fput(file);
    return snapshot->kind == HV_DXG_SHARED_OBJECT_RESOURCE &&
           snapshot->fops_kind == HV_DXG_SHARED_FOPS_RESOURCE &&
           snapshot->sealed != 0 &&
           snapshot->shared_records_valid != 0 &&
           snapshot->generation != 0 ? 0 : -EINVAL;
}

int hyperv_dxg_shared_resource_snapshot_from_opened_resource(
    int dxg_fd, int resource_fd, uint32 device, uint32 resource,
    uint32 allocation, uint32 allocation_count,
    struct hyperv_dxg_shared_resource_snapshot *snapshot)
{
    struct hvdxg_shared_object *shared;
    struct hvdxg_tracked_resource *opened;
    struct hvdxg_tracked_resource *parent_resource;
    struct vfs_file *shared_file = NULL;
    struct vfs_file *dxg_file = NULL;
    struct hvdxg_open_state *owner;
    uint32 fops_kind;
    int ret = -EINVAL;

    if (snapshot == NULL)
        return -EINVAL;
    memset(snapshot, 0, sizeof(*snapshot));
    if (dxg_fd < 0 || resource_fd < 0 || device == 0 || resource == 0 ||
        allocation == 0 || allocation_count == 0)
        return -EINVAL;
    if (current == NULL || current->fdtable == NULL)
        return -EINVAL;

    shared = hvdxg_shared_object_from_fd(resource_fd,
                                         HV_DXG_SHARED_OBJECT_RESOURCE,
                                         &shared_file);
    if (shared == NULL)
        return -EINVAL;
    fops_kind = hvdxg_shared_object_fops_kind(shared_file);
    if (fops_kind != HV_DXG_SHARED_FOPS_RESOURCE ||
        shared->global_share == 0)
        goto out;
    parent_resource = hvdxg_shared_parent_resource(shared);
    if (parent_resource == NULL || parent_resource->sealed == 0 ||
        parent_resource->shared_records_valid == 0 ||
        parent_resource->sealed_generation == 0)
        goto out;

    dxg_file = vfs_fdtable_get_file(current->fdtable, dxg_fd);
    if (dxg_file == NULL || dxg_file->ops != &hvdxg_file_ops ||
        dxg_file->private_data == NULL)
        goto out;
    owner = (struct hvdxg_open_state *)dxg_file->private_data;
    opened = hvdxg_owner_find_resource(owner, device, resource);
    if (opened == NULL || opened->allocation_count != allocation_count ||
        opened->allocation_count == 0 ||
        opened->allocation_count > HV_DXG_ALLOCATION_MAX ||
        opened->allocation_handles[0] != allocation ||
        opened->global_share != shared->global_share)
        goto out;

    snapshot->kind = shared->kind;
    snapshot->fops_kind = fops_kind;
    snapshot->device = opened->device;
    snapshot->resource = opened->resource;
    snapshot->allocation_count = opened->allocation_count;
    snapshot->first_allocation = opened->allocation_handles[0];
    snapshot->sealed = parent_resource->sealed;
    snapshot->shared_records_valid = parent_resource->shared_records_valid;
    snapshot->generation = parent_resource->sealed_generation;
    snapshot->host_shared_refs = parent_resource->host_shared_refs;
    if (shared->resource_parent != NULL) {
        snapshot->shared_parent_id = shared->resource_parent->id;
        snapshot->shared_parent_refs = shared->resource_parent->refs;
        snapshot->shared_parent_children = shared->resource_parent->child_count;
    } else {
        snapshot->shared_parent_id = shared->parent_id;
        snapshot->shared_parent_refs = shared->parent_refs;
        snapshot->shared_parent_children = shared->opened_child_count;
    }
    ret = 0;

out:
    if (dxg_file != NULL)
        vfs_fput(dxg_file);
    if (shared_file != NULL)
        vfs_fput(shared_file);
    return ret;
}

int hyperv_dxg_display_bind_pin_from_fds(
    int dxg_fd, int resource_fd, uint32 device, uint32 resource,
    uint32 allocation, uint32 allocation_count,
    struct hyperv_dxg_display_bind_pin_snapshot *snapshot)
{
    struct hvdxg_shared_object *shared;
    struct hvdxg_tracked_resource *opened = NULL;
    struct hvdxg_tracked_resource *pinned_resource = NULL;
    struct hvdxg_tracked_resource *parent_resource = NULL;
    struct vfs_file *shared_file = NULL;
    struct vfs_file *dxg_file = NULL;
    struct hvdxg_open_state *owner = NULL;
    struct hvdxg_process_adapter *process_adapter = NULL;
    uint32 fops_kind = HV_DXG_SHARED_FOPS_NONE;
    uint32 reason = HV_DXG_BIND_PIN_REASON_OK;
    uint32 selected = 0;
    int ret = -EINVAL;

    if (snapshot == NULL)
        return -EINVAL;
    memset(snapshot, 0, sizeof(*snapshot));
    if (dxg_fd < 0 || resource_fd < 0 || device == 0 || resource == 0 ||
        allocation == 0 || allocation_count == 0) {
        hvdxg_note_display_bind_pin_diag(
            -EINVAL, HV_DXG_BIND_PIN_REASON_BAD_ARGS, dxg_fd,
            resource_fd, device, resource, allocation, allocation_count,
            NULL, HV_DXG_SHARED_FOPS_NONE, NULL, NULL, NULL, 0);
        return -EINVAL;
    }
    if (current == NULL || current->fdtable == NULL) {
        hvdxg_note_display_bind_pin_diag(
            -EINVAL, HV_DXG_BIND_PIN_REASON_NO_FDTABLE, dxg_fd,
            resource_fd, device, resource, allocation, allocation_count,
            NULL, HV_DXG_SHARED_FOPS_NONE, NULL, NULL, NULL, 0);
        return -EINVAL;
    }

    shared = hvdxg_shared_object_from_fd(resource_fd,
                                         HV_DXG_SHARED_OBJECT_RESOURCE,
                                         &shared_file);
    if (shared == NULL) {
        reason = HV_DXG_BIND_PIN_REASON_NO_SHARED_FD;
        return -EINVAL;
    }
    fops_kind = hvdxg_shared_object_fops_kind(shared_file);
    if (fops_kind != HV_DXG_SHARED_FOPS_RESOURCE ||
        shared->global_share == 0) {
        reason = HV_DXG_BIND_PIN_REASON_BAD_SHARED_FD;
        goto out;
    }
    parent_resource = hvdxg_shared_parent_resource(shared);
    if (parent_resource == NULL || parent_resource->sealed == 0 ||
        parent_resource->shared_records_valid == 0 ||
        parent_resource->sealed_generation == 0) {
        reason = HV_DXG_BIND_PIN_REASON_BAD_PARENT;
        goto out;
    }

    dxg_file = vfs_fdtable_get_file(current->fdtable, dxg_fd);
    if (dxg_file == NULL || dxg_file->ops != &hvdxg_file_ops ||
        dxg_file->private_data == NULL) {
        reason = HV_DXG_BIND_PIN_REASON_BAD_DXG_FD;
        goto out;
    }

    owner = (struct hvdxg_open_state *)dxg_file->private_data;
    opened = hvdxg_owner_find_resource(owner, device, resource);
    if (opened != NULL && opened->allocation_count == allocation_count &&
        opened->allocation_count != 0 &&
        opened->allocation_count <= HV_DXG_ALLOCATION_MAX &&
        opened->allocation_handles[0] == allocation &&
        opened->global_share == shared->global_share) {
        pinned_resource = opened;
        selected = 1;
    } else if (parent_resource->device == device &&
               parent_resource->resource == resource &&
               parent_resource->allocation_count == allocation_count &&
               parent_resource->allocation_count != 0 &&
               parent_resource->allocation_count <=
                   HV_DXG_ALLOCATION_MAX &&
               parent_resource->allocation_handles[0] == allocation &&
               parent_resource->global_share == shared->global_share) {
        pinned_resource = parent_resource;
        selected = 2;
    } else {
        reason = HV_DXG_BIND_PIN_REASON_NO_MATCHING_RESOURCE;
        goto out;
    }

    snapshot->dxg_file_cookie = dxg_file;
    snapshot->resource_file_cookie = shared_file;
    snapshot->dxg_file_pinned = 1;
    snapshot->resource_file_pinned = 1;
    snapshot->kind = shared->kind;
    snapshot->fops_kind = fops_kind;
    snapshot->device = pinned_resource->device;
    snapshot->resource = pinned_resource->resource;
    snapshot->allocation_count = pinned_resource->allocation_count;
    snapshot->first_allocation = pinned_resource->allocation_handles[0];
    snapshot->sealed = parent_resource->sealed;
    snapshot->shared_records_valid = parent_resource->shared_records_valid;
    snapshot->generation = parent_resource->sealed_generation;
    snapshot->host_shared_refs = parent_resource->host_shared_refs;
    if (shared->resource_parent != NULL) {
        snapshot->shared_parent_id = shared->resource_parent->id;
        snapshot->shared_parent_refs = shared->resource_parent->refs;
        snapshot->shared_parent_children = shared->resource_parent->child_count;
    } else {
        snapshot->shared_parent_id = shared->parent_id;
        snapshot->shared_parent_refs = shared->parent_refs;
        snapshot->shared_parent_children = shared->opened_child_count;
    }
    snapshot->process = hvdxg_open_host_process(owner);
    snapshot->process_generation = hvdxg_open_process_generation(owner);
    snapshot->process_refs = hvdxg_open_process_refs(owner);
    if (owner->process_state != NULL)
        process_adapter = hvdxg_process_find_adapter(owner->process_state,
                                                     hvdxg.host_adapter_handle);
    if (process_adapter != NULL) {
        snapshot->process_adapter_generation = process_adapter->generation;
        snapshot->process_adapter_refs = process_adapter->refs;
    }
    snapshot->hmgr_index_unique_valid =
        hvdxg_hmgr_handle_index_unique_valid(owner, HV_DXG_OBJECT_DEVICE,
                                             device) &&
        hvdxg_hmgr_handle_index_unique_valid(owner, HV_DXG_OBJECT_RESOURCE,
                                             resource) &&
        hvdxg_hmgr_handle_index_unique_valid(owner, HV_DXG_OBJECT_ALLOCATION,
                                             allocation);
    snapshot->parent_resource_ref_held =
        snapshot->shared_parent_id != 0 &&
        snapshot->shared_parent_refs != 0;
    snapshot->opened_child_ref_held =
        opened != NULL &&
        snapshot->shared_parent_id != 0 &&
        snapshot->shared_parent_children != 0 &&
        opened->global_share == shared->global_share;
    ret = 0;
    reason = HV_DXG_BIND_PIN_REASON_OK;

out:
    hvdxg_note_display_bind_pin_diag(
        ret, reason, dxg_fd, resource_fd, device, resource, allocation,
        allocation_count, shared, fops_kind, parent_resource, owner, opened,
        selected);
    if (ret != 0) {
        if (dxg_file != NULL)
            vfs_fput(dxg_file);
        if (shared_file != NULL)
            vfs_fput(shared_file);
    }
    return ret;
}

void hyperv_dxg_display_bind_unpin(
    struct hyperv_dxg_display_bind_pin_snapshot *snapshot)
{
    if (snapshot == NULL)
        return;
    if (snapshot->dxg_file_cookie != NULL) {
        vfs_fput((struct vfs_file *)snapshot->dxg_file_cookie);
        snapshot->dxg_file_cookie = NULL;
        snapshot->dxg_file_pinned = 0;
    }
    if (snapshot->resource_file_cookie != NULL) {
        vfs_fput((struct vfs_file *)snapshot->resource_file_cookie);
        snapshot->resource_file_cookie = NULL;
        snapshot->resource_file_pinned = 0;
    }
}

static uint64
hvdxg_display_bind_missing_required_metadata(
    const struct hyperv_dxg_display_bind_request *bind)
{
    uint64 expected;
    uint64 missing = 0;

    if (bind == NULL)
        return FB_GPU_DXG_PRESENT_META_DEVICE |
               FB_GPU_DXG_PRESENT_META_RESOURCE |
               FB_GPU_DXG_PRESENT_META_ALLOCATION |
               FB_GPU_DXG_PRESENT_META_DIMENSIONS |
               FB_GPU_DXG_PRESENT_META_FORMAT |
               FB_GPU_DXG_PRESENT_META_MODIFIER |
               FB_GPU_DXG_PRESENT_META_ADAPTER_LUID |
               FB_GPU_DXG_PRESENT_META_SYNC_OBJECT |
               FB_GPU_DXG_PRESENT_META_FENCE_VALUE;

    expected = FB_GPU_DXG_PRESENT_META_DEVICE |
               FB_GPU_DXG_PRESENT_META_RESOURCE |
               FB_GPU_DXG_PRESENT_META_ALLOCATION |
               FB_GPU_DXG_PRESENT_META_DIMENSIONS |
               FB_GPU_DXG_PRESENT_META_FORMAT |
               FB_GPU_DXG_PRESENT_META_MODIFIER |
               FB_GPU_DXG_PRESENT_META_ADAPTER_LUID;
    if ((bind->flags & FB_GPU_DXG_PRESENT_F_WAIT_SYNC) != 0)
        expected |= FB_GPU_DXG_PRESENT_META_SYNC_OBJECT |
                    FB_GPU_DXG_PRESENT_META_FENCE_VALUE;

    missing |= expected & ~bind->required_metadata;
    if (bind->device == 0)
        missing |= FB_GPU_DXG_PRESENT_META_DEVICE;
    if (bind->resource == 0)
        missing |= FB_GPU_DXG_PRESENT_META_RESOURCE;
    if (bind->allocation == 0 || bind->allocation_count == 0)
        missing |= FB_GPU_DXG_PRESENT_META_ALLOCATION;
    if (bind->width == 0 || bind->height == 0 || bind->pitch == 0)
        missing |= FB_GPU_DXG_PRESENT_META_DIMENSIONS;
    if (bind->format == 0)
        missing |= FB_GPU_DXG_PRESENT_META_FORMAT;
    if (bind->adapter_luid_low == 0 && bind->adapter_luid_high == 0)
        missing |= FB_GPU_DXG_PRESENT_META_ADAPTER_LUID;
    if ((bind->flags & FB_GPU_DXG_PRESENT_F_WAIT_SYNC) != 0) {
        if (bind->sync_object == 0)
            missing |= FB_GPU_DXG_PRESENT_META_SYNC_OBJECT;
        if (bind->fence_value == 0)
            missing |= FB_GPU_DXG_PRESENT_META_FENCE_VALUE;
    }
    return missing;
}

int hyperv_dxg_display_bind_submit_failclosed(
    const struct hyperv_dxg_display_bind_request *bind,
    struct hyperv_dxg_display_bind_result *result)
{
    uint64 block_reason;
    uint64 missing_metadata;
    int pin_valid;

    if (bind == NULL || result == NULL)
        return -EINVAL;

    memset(result, 0, sizeof(*result));
    block_reason = bind->block_reason |
                   FB_GPU_DXG_PRESENT_BLOCK_NO_TRANSPORT |
                   FB_GPU_DXG_PRESENT_BLOCK_DXG_NO_DISPLAY_BIND |
                   FB_GPU_DXG_PRESENT_BLOCK_WSL_ENUM_ONLY |
                   FB_GPU_DXG_PRESENT_BLOCK_NO_COMPLETION;
    result->transport = FB_GPU_DXG_PRESENT_GPUP_DDA_TRANSPORT_NONE;
    result->operation = FB_GPU_DXG_PRESENT_GPUP_DDA_OP_SCANOUT_BIND;
    result->completion_source = FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY;
    result->present_id = 0;
    result->completed_id = 0;
    result->source_generation = bind->source_generation;
    result->resource_generation = bind->resource_generation;
    result->host_abi_present = 0;
    result->sender_present = 0;
    result->completion_present = 0;
    result->no_host_abi = 1;
    result->no_sender = 1;
    result->no_completion = 1;
    result->publication_attempted = 1;
    result->publish_before_send = 0;
    result->transport_pending_id = 0;
    result->command_id = 0;
    result->transaction_id = 0;
    result->channel = 0;
    result->completion_demux_registered = 0;
    result->transport_source = FB_GPU_DXG_DISPLAY_BIND_SOURCE_NONE;
    result->host_saw_packet = 0;
    result->wsl_presenthistory_completion_credit = 0;
    result->resolved_or_cancelled = 1;
    result->refs_released = 1;
    result->no_host_abi_cancelled = 1;
    result->no_host_abi_refs_released = 1;
    result->pending_owner_generation =
        bind->pin_valid ? bind->pin.process_generation : 0;
    result->pending_source_generation = bind->source_generation;
    result->pending_resource_generation = bind->resource_generation;
    result->pending_dxgprocess_generation =
        bind->pin_valid ? bind->pin.process_generation : 0;
    result->pending_process_adapter_generation =
        bind->pin_valid ? bind->pin.process_adapter_generation : 0;
    result->pending_hmgr_index_unique_valid =
        bind->pin_valid ? bind->pin.hmgr_index_unique_valid : 0;
    result->pending_parent_resource_ref_held =
        bind->pin_valid ? bind->pin.parent_resource_ref_held : 0;
    result->pending_opened_child_ref_held =
        bind->pin_valid ? bind->pin.opened_child_ref_held : 0;
    if (bind->pin_valid && bind->pin.dxg_file_cookie != NULL &&
        bind->sync_object != 0) {
        struct hvdxg_open_state *owner =
            (struct hvdxg_open_state *)
                ((struct vfs_file *)bind->pin.dxg_file_cookie)->private_data;

        result->pending_syncobject_ref_held =
            hvdxg_hmgr_handle_index_unique_valid(owner, HV_DXG_OBJECT_SYNC,
                                                 bind->sync_object);
    }
    result->pending_owner_close_cancelled = 0;
    missing_metadata =
        hvdxg_display_bind_missing_required_metadata(bind);
    result->request_missing_metadata = missing_metadata;
    result->request_metadata_complete = missing_metadata == 0;
    result->request_sync_metadata_complete =
        (bind->flags & FB_GPU_DXG_PRESENT_F_WAIT_SYNC) == 0 ||
        (missing_metadata & (FB_GPU_DXG_PRESENT_META_SYNC_OBJECT |
                             FB_GPU_DXG_PRESENT_META_FENCE_VALUE)) == 0;

    pin_valid = bind->pin_valid &&
                bind->pin.dxg_file_pinned != 0 &&
                bind->pin.resource_file_pinned != 0 &&
                bind->pin.sealed != 0 &&
                bind->pin.shared_records_valid != 0 &&
                bind->pin.generation != 0 &&
                bind->pin.device == bind->device &&
                bind->pin.resource == bind->resource &&
                bind->pin.first_allocation == bind->allocation &&
                bind->pin.allocation_count == bind->allocation_count &&
                bind->pin.process != 0 &&
                bind->pin.process_generation != 0 &&
                bind->pin.process_adapter_generation != 0 &&
                bind->pin.hmgr_index_unique_valid != 0 &&
                bind->pin.shared_parent_id != 0 &&
                bind->pin.shared_parent_refs != 0 &&
                bind->pin.shared_parent_children != 0 &&
                bind->pin.parent_resource_ref_held != 0 &&
                bind->pin.opened_child_ref_held != 0 &&
                ((bind->flags & FB_GPU_DXG_PRESENT_F_WAIT_SYNC) == 0 ||
                 result->pending_syncobject_ref_held != 0);
    if (pin_valid)
        result->pin_revalidated = 1;
    else
        block_reason |= FB_GPU_DXG_PRESENT_BLOCK_RESOURCE_FD_UNVERIFIED;

    result->block_reason = block_reason;
    result->status = EOPNOTSUPP;
    return -EOPNOTSUPP;
}

int hyperv_dxg_display_bind_submit(
    const struct hyperv_dxg_display_bind_request *bind,
    struct hyperv_dxg_display_bind_result *result)
{
    return hyperv_dxg_display_bind_submit_failclosed(bind, result);
}

static int hvdxg_sync_file_release(struct vfs_inode *ip,
                                   struct vfs_file *file)
{
    struct hvdxg_sync_file_object *sync_file =
        file != NULL ?
        (struct hvdxg_sync_file_object *)file->private_data : NULL;

    (void)ip;
    if (sync_file != NULL) {
        hvdxg.syncfile_release_count++;
        if (hvdxg.syncfile_live_count != 0)
            hvdxg.syncfile_live_count--;
        if (sync_file->event_id != 0) {
            hvdxg_remove_host_event(sync_file->event_id);
            hvdxg.syncfile_release_event_removed++;
        }
        if (sync_file->host_nt_handle != 0) {
            (void)hvdxg_release_nt_shared_object_ref(
                HV_DXG_SHARED_OBJECT_SYNC, sync_file->cache_process,
                sync_file->cache_object != 0 ? sync_file->cache_object :
                sync_file->sync_object,
                sync_file->host_nt_handle);
            hvdxg.syncfile_release_nt_released++;
        }
        kvfree(sync_file);
        file->private_data = NULL;
    }
    return 0;
}

static int hvdxg_sync_file_stat(struct vfs_file *file, struct stat *st)
{
    if (file == NULL || file->private_data == NULL || st == NULL)
        return -EINVAL;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0600;
    st->st_nlink = 1;
    st->st_ino = 0x64786703ULL;
    st->st_blksize = 4096;
    return 0;
}

static ssize_t hvdxg_sync_file_readlink(struct vfs_file *file, char *buf,
                                        size_t buflen)
{
    if (file == NULL || file->private_data == NULL || buf == NULL)
        return -EINVAL;
    return snprintf(buf, buflen, "anon_inode:sync_file");
}

static struct vfs_file_ops hvdxg_sync_file_ops = {
    .stat = hvdxg_sync_file_stat,
    .readlink = hvdxg_sync_file_readlink,
    .release = hvdxg_sync_file_release,
};

static struct hvdxg_sync_file_object *hvdxg_sync_file_from_fd(
    int fd, struct vfs_file **file_out)
{
    struct vfs_file *f;

    if (file_out != NULL)
        *file_out = NULL;
    if (fd < 0 || current == NULL || current->fdtable == NULL)
        return NULL;
    f = vfs_fdtable_get_file(current->fdtable, fd);
    if (f == NULL)
        return NULL;
    if (f->ops != &hvdxg_sync_file_ops || f->private_data == NULL) {
        vfs_fput(f);
        return NULL;
    }
    if (file_out != NULL)
        *file_out = f;
    else
        vfs_fput(f);
    return (struct hvdxg_sync_file_object *)f->private_data;
}

static int hvdxg_open_sync_file_on_device(
    struct hvdxg_open_state *owner, uint32 device,
    struct hvdxg_sync_file_object *sync_file,
    struct d3dddi_synchronizationobject_flags flags, uint32 engine_affinity,
    int map_fence_va, struct hvdxg_d3dkmthandle *sync_out, uint64 *cpu_va_out,
    uint64 *gpu_va_out, uint64 *kva_out)
{
    struct hvdxg_command_opensyncobject open;
    struct hvdxg_command_opensyncobject_return result;
    struct hvdxg_d3dkmthandle open_process;
    uint32 actual_len = 0;
    uint64 fence_pa = 0;
    uint64 fence_kva = 0;
    int ret;

    if (sync_out != NULL)
        sync_out->v = 0;
    if (cpu_va_out != NULL)
        *cpu_va_out = 0;
    if (gpu_va_out != NULL)
        *gpu_va_out = 0;
    if (kva_out != NULL)
        *kva_out = 0;
    if (owner == NULL || sync_file == NULL || device == 0 ||
        sync_file->global_share == 0 || !hvdxg_owner_has_device(owner, device))
        return -EINVAL;

    memset(&open, 0, sizeof(open));
    memset(&result, 0, sizeof(result));
    open_process = hvdxg_owner_bound_process_handle(owner);
    hvdxg_command_vm_init(&open.hdr, HV_DXGK_VMBCOMMAND_OPENSYNCOBJECT);
    open.hdr.process = open_process;
    open.device.v = device;
    open.global_sync_object.v = sync_file->global_share;
    open.flags = flags;
    open.engine_affinity = engine_affinity;

    hvdxg.opensync_ioctl_count++;
    hvdxg.opensync_last_cmd_len = 0;
    hvdxg.opensync_last_wire_len = 0;
    hvdxg.opensync_last_ext = 0;
    hvdxg.opensync_last_ext_offset = 0;
    hvdxg.opensync_last_result_len = sizeof(result);
    hvdxg.opensync_last_actual_len = 0;
    hvdxg.opensync_last_ret = -ENOTSUP;
    hvdxg.opensync_last_status = 0;
    hvdxg.opensync_last_process = open_process.v;
    hvdxg.opensync_last_device = device;
    hvdxg.opensync_last_device_host =
        hvdxg_owner_host_object_handle(owner, HV_DXG_OBJECT_DEVICE,
                                       device, NULL);
    hvdxg.opensync_last_device_owner = hvdxg_open_host_process(owner);
    hvdxg.opensync_last_device_owner_generation =
        hvdxg_open_process_generation(owner);
    hvdxg.opensync_last_device_generation = 0;
    hvdxg.opensync_last_global = sync_file->global_share;
    hvdxg.opensync_last_input_nt = 0;
    hvdxg.opensync_last_host_nt = sync_file->host_nt_handle;
    hvdxg.opensync_last_object = sync_file->sync_object;
    hvdxg.opensync_last_cache_object = sync_file->cache_object;
    hvdxg.opensync_last_source_device = sync_file->device;
    hvdxg.opensync_last_source_device_host =
        hvdxg_owner_host_object_handle(owner, HV_DXG_OBJECT_DEVICE,
                                       sync_file->device, NULL);
    hvdxg.opensync_last_source_owner = sync_file->cache_process;
    hvdxg.opensync_last_source_owner_generation = 0;
    hvdxg.opensync_last_source_flags = sync_file->sync_flags;
    hvdxg.opensync_last_same_device =
        device != 0 && sync_file->device == device;
    hvdxg.opensync_last_adapter_match =
        hvdxg_luid_nonzero(hvdxg.adapter_luid) &&
        hvdxg_luid_nonzero(hvdxg.host_adapter_luid) ? 1 : 0;
    hvdxg.opensync_last_adapter_low = hvdxg.adapter_luid.a;
    hvdxg.opensync_last_adapter_high = hvdxg.adapter_luid.b;
    hvdxg.opensync_last_host_adapter_low = hvdxg.host_adapter_luid.a;
    hvdxg.opensync_last_host_adapter_high = hvdxg.host_adapter_luid.b;
    hvdxg.opensync_last_flags = flags.value;
    hvdxg.opensync_last_wire_flags = flags.value;
    hvdxg.opensync_last_forced_flags = 0;
    hvdxg.opensync_last_fops_kind = 0;
    hvdxg.opensync_last_sync_type = sync_file->sync_type;
    hvdxg.opensync_last_result_sync = 0;
    hvdxg.opensync_last_gpu_va = 0;
    hvdxg.opensync_last_cpu_pa = 0;
    hvdxg.opensync_last_fd_kind = HV_DXG_SHARED_OBJECT_SYNC;
    hvdxg.opensync_last_fd_refs = hvdxg_ntshared_cache_refs(
        HV_DXG_SHARED_OBJECT_SYNC, sync_file->cache_process,
        sync_file->cache_object != 0 ? sync_file->cache_object :
        sync_file->sync_object,
        sync_file->host_nt_handle);
    hvdxg.opensync_last_gate = 3;
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
        hvdxg.opensync_last_owner_tgid != hvdxg.opensync_last_current_tgid;

    ret = hvdxg_send_sync_global(&open, sizeof(open), &result,
                                 sizeof(result), &actual_len);
    hvdxg.opensync_last_cmd_len = hvdxg.global_send_last_cmd_len;
    hvdxg.opensync_last_wire_len = hvdxg.global_send_last_wire_len;
    hvdxg.opensync_last_ext = hvdxg.global_send_last_ext;
    hvdxg.opensync_last_ext_offset = hvdxg.global_send_last_ext_offset;
    hvdxg.opensync_last_actual_len = actual_len;
    hvdxg.opensync_last_status = result.status.v;
    hvdxg.opensync_last_result_sync = result.sync_object.v;
    hvdxg.opensync_last_gpu_va = result.gpu_virtual_address;
    hvdxg.opensync_last_cpu_pa = result.guest_cpu_physical_address;
    if (ret == 0 && actual_len < sizeof(result))
        ret = -EOVERFLOW;
    if (ret == 0)
        ret = hvdxg_ntstatus_to_errno(result.status);
    if (ret == 0 && map_fence_va &&
        sync_file->sync_type == _D3DDDI_MONITORED_FENCE) {
        uint64 cpu_va;

        cpu_va = hvdxg_map_iospace_user_canonical(
            HV_DXG_FENCE_SOURCE_OPEN_SYNC,
            result.guest_cpu_physical_address, PGSIZE,
            0, &fence_pa, NULL, 1);
        fence_kva = hvdxg_map_iospace_kernel_canonical(
            HV_DXG_FENCE_SOURCE_OPEN_SYNC,
            result.guest_cpu_physical_address, PGSIZE, fence_pa, 1);
        if (cpu_va == 0)
            ret = -ENOMEM;
        if (cpu_va_out != NULL)
            *cpu_va_out = cpu_va;
    }
    if (ret == 0) {
        if (sync_out != NULL)
            *sync_out = result.sync_object;
        if (gpu_va_out != NULL)
            *gpu_va_out = result.gpu_virtual_address;
        if (kva_out != NULL)
            *kva_out = fence_kva;
    }
    hvdxg.opensync_last_ret = ret;
    hvdxg.opensync_last_gate = ret == 0 ? 5 : 4;
    return ret;
}

static int hvdxg_destroy_syncobject_host_for_syncfile(
    struct hvdxg_open_state *owner, uint32 device, uint32 sync,
    uint32 type, uint32 flags, uint32 global_shared)
{
    struct hvdxg_command_destroysyncobject destroy;
    struct hvdxg_ntstatus status;
    uint32 actual_len = 0;
    int ret;

    if (sync == 0)
        return 0;
    memset(&destroy, 0, sizeof(destroy));
    memset(&status, 0, sizeof(status));
    hvdxg_command_vm_init(&destroy.hdr,
                          HV_DXGK_VMBCOMMAND_DESTROYSYNCOBJECT);
    destroy.hdr.process = hvdxg_owner_bound_process_handle(owner);
    destroy.sync_object.v = sync;
    ret = hvdxg_send_sync_global(&destroy, sizeof(destroy), &status,
                                 sizeof(status), &actual_len);
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg.destroysync_last_handle = sync;
    hvdxg.destroysync_last_device = device;
    hvdxg.destroysync_last_type = type;
    hvdxg.destroysync_last_flags = flags;
    hvdxg.destroysync_last_global = global_shared;
    hvdxg.destroysync_last_monitor_fence = 0;
    hvdxg.destroysync_last_cmd_len = hvdxg.global_send_last_cmd_len;
    hvdxg.destroysync_last_wire_len = hvdxg.global_send_last_wire_len;
    hvdxg.destroysync_last_ext = hvdxg.global_send_last_ext;
    hvdxg.destroysync_last_ext_offset = hvdxg.global_send_last_ext_offset;
    hvdxg.destroysync_last_len = actual_len;
    hvdxg.destroysync_last_status = status.v;
    hvdxg.destroysync_last_ret = ret;
    return ret;
}

static int hvdxg_share_object_with_host(uint32 process, uint32 device,
                                        uint32 object, uint64 reserved,
                                        uint64 *nt_handle,
                                        uint32 *actual_len_out,
                                        int32 *status_out)
{
    struct hvdxg_command_shareobjectwithhost share;
    struct hvdxg_command_shareobjectwithhost_return result;
    uint32 actual_len = 0;
    int ret;

    if (nt_handle != NULL)
        *nt_handle = 0;
    if (actual_len_out != NULL)
        *actual_len_out = 0;
    if (status_out != NULL)
        *status_out = 0;
    if (process == 0 || device == 0 || object == 0)
        return -EINVAL;
    hvdxg.shareobject_last_cmd_len = sizeof(share);
    hvdxg.shareobject_last_wire_len = 0;
    hvdxg.shareobject_last_ext = 0;
    hvdxg.shareobject_last_ext_offset = 0;
    hvdxg.shareobject_last_device_offset =
        offsetof(struct hvdxg_command_shareobjectwithhost, device_handle);
    hvdxg.shareobject_last_object_offset =
        offsetof(struct hvdxg_command_shareobjectwithhost, object_handle);
    hvdxg.shareobject_last_result_len = sizeof(result);
    hvdxg.shareobject_last_head_len = 0;
    memset(hvdxg.shareobject_last_head, 0,
           sizeof(hvdxg.shareobject_last_head));
    hvdxg.shareobject_last_completion_type = 0;
    hvdxg.shareobject_last_completion_len = 0;
    memset(hvdxg.shareobject_last_completion_prefix, 0,
           sizeof(hvdxg.shareobject_last_completion_prefix));
    hvdxg.shareobject_last_process = process;
    hvdxg.shareobject_last_device = device;
    hvdxg.shareobject_last_object = object;
    hvdxg.shareobject_last_reserved = 0;
    memset(&share, 0, sizeof(share));
    memset(&result, 0, sizeof(result));
    hvdxg_command_vm_init(&share.hdr,
                          HV_DXGK_VMBCOMMAND_SHAREOBJECTWITHHOST);
    share.hdr.process.v = process;
    share.device_handle.v = device;
    share.object_handle.v = object;
    (void)reserved;
    share.reserved = 0;
    hvdxg_note_shareobject_wire(&share, sizeof(share), sizeof(result));
    ret = hvdxg_send_sync_global(&share, sizeof(share), &result,
                                 sizeof(result), &actual_len);
    hvdxg_snapshot_last_completion(&hvdxg.shareobject_last_completion_type,
                                   &hvdxg.shareobject_last_completion_len,
                                   hvdxg.shareobject_last_completion_prefix);
    if (actual_len_out != NULL)
        *actual_len_out = actual_len;
    if (actual_len >= sizeof(result) && status_out != NULL)
        *status_out = result.status.v;
    if (ret == 0 && actual_len < sizeof(result))
        ret = -EOVERFLOW;
    if (ret == 0)
        ret = hvdxg_ntstatus_to_errno(result.status);
    if (ret == 0 && nt_handle != NULL)
        *nt_handle = result.vail_nt_handle;
    return ret;
}

static void hvdxg_share_object_with_host_diagnostic(uint32 process,
                                                   uint32 device,
                                                   uint32 object,
                                                   uint32 kind,
                                                   uint32 reason)
{
    uint64 nt_handle = 0;
    uint32 actual_len = 0;
    int32 status = 0;
    int ret;

    hvdxg.shareobject_diag_attempted = 0;
    hvdxg.shareobject_diag_valid_nt = 0;
    hvdxg.shareobject_diag_kind = kind;
    hvdxg.shareobject_diag_reason = reason;
    if (process == 0 || device == 0 || object == 0)
        return;
    hvdxg.shareobject_diag_attempted = 1;
    ret = hvdxg_share_object_with_host(process, device, object, 0,
                                       &nt_handle, &actual_len, &status);
    hvdxg.shareobject_last_len = actual_len;
    hvdxg.shareobject_last_ret = ret;
    hvdxg.shareobject_last_status = status;
    hvdxg.shareobject_last_nt_handle = nt_handle;
    hvdxg.shareobject_diag_valid_nt =
        ret == 0 && actual_len >=
        sizeof(struct hvdxg_command_shareobjectwithhost_return) &&
        nt_handle != 0 ? 1 : 0;
}

static int hvdxg_owner_has_gpuva(struct hvdxg_open_state *owner,
                                 uint32 adapter, uint64 base)
{
    struct hvdxg_object_entry *obj;

    if (owner == NULL || base == 0)
        return 0;
    obj = hvdxg_owner_find_object(owner, HV_DXG_OBJECT_GPUVA, base);
    if (obj == NULL) {
        hvdxg.object_table_denied++;
        return 0;
    }
    return adapter == 0 || obj->device == adapter;
}

static int hvdxg_owner_has_gpuva_alias(struct hvdxg_open_state *owner,
                                       uint32 adapter, uint32 host_adapter,
                                       uint64 base)
{
    if (hvdxg_owner_has_gpuva(owner, adapter, base))
        return 1;
    if (host_adapter != 0 && host_adapter != adapter)
        return hvdxg_owner_has_gpuva(owner, host_adapter, base);
    return 0;
}

static int hvdxg_owner_gpuva_contains(struct hvdxg_open_state *owner,
                                      uint32 adapter, uint64 base,
                                      uint64 size)
{
    uint64 end;

    if (owner == NULL || base == 0 || size == 0)
        return 0;
    end = base + size;
    if (end < base)
        return 0;
    for (uint32 i = 0; i < owner->gpuva_count; i++) {
        uint64 va_base = owner->gpuvas[i].base;
        uint64 va_end = va_base + owner->gpuvas[i].size;

        if (va_end < va_base)
            continue;
        if ((adapter == 0 || owner->gpuvas[i].adapter == adapter) &&
            base >= va_base && end <= va_end)
            return 1;
    }
    return 0;
}

static int hvdxg_owner_gpuva_contains_alias(struct hvdxg_open_state *owner,
                                           uint32 adapter,
                                           uint32 host_adapter, uint64 base,
                                           uint64 size)
{
    if (hvdxg_owner_gpuva_contains(owner, adapter, base, size))
        return 1;
    if (host_adapter != 0 && host_adapter != adapter)
        return hvdxg_owner_gpuva_contains(owner, host_adapter, base, size);
    return 0;
}

static void hvdxg_track_gpuva(struct hvdxg_open_state *owner, uint32 adapter,
                              uint64 base, uint64 size, uint64 fence_value,
                              uint64 fence_cpu_pa)
{
    if (owner == NULL || adapter == 0 || base == 0 || size == 0)
        return;
    (void)hvdxg_track_object(owner, HV_DXG_OBJECT_GPUVA, base, size,
                             adapter);
    for (uint32 i = 0; i < owner->gpuva_count; i++) {
        if (owner->gpuvas[i].adapter == adapter &&
            owner->gpuvas[i].base == base) {
            owner->gpuvas[i].size = size;
            owner->gpuvas[i].fence_value = fence_value;
            owner->gpuvas[i].fence_cpu_pa = fence_cpu_pa;
            return;
        }
    }
    if (hvdxg_grow_table((void **)&owner->gpuvas,
                         &owner->gpuva_capacity,
                         owner->gpuva_count + 1,
                         sizeof(owner->gpuvas[0]),
                         HV_DXG_OPEN_TRACKED_MAX) == 0) {
        struct hvdxg_tracked_gpuva *slot =
            &owner->gpuvas[owner->gpuva_count++];

        memset(slot, 0, sizeof(*slot));
        slot->adapter = adapter;
        slot->base = base;
        slot->size = size;
        slot->fence_value = fence_value;
        slot->fence_cpu_pa = fence_cpu_pa;
        if (owner->gpuva_count > hvdxg.track_gpuva_max)
            hvdxg.track_gpuva_max = owner->gpuva_count;
    } else {
        hvdxg.track_gpuva_drops++;
    }
}

static void hvdxg_untrack_gpuva(struct hvdxg_open_state *owner,
                                uint64 base)
{
    if (owner == NULL || base == 0)
        return;
    hvdxg_untrack_object(owner, HV_DXG_OBJECT_GPUVA, base);
    for (uint32 i = 0; i < owner->gpuva_count; i++) {
        if (owner->gpuvas[i].base == base) {
            owner->gpuvas[i] = owner->gpuvas[owner->gpuva_count - 1];
            memset(&owner->gpuvas[owner->gpuva_count - 1], 0,
                   sizeof(owner->gpuvas[0]));
            owner->gpuva_count--;
            return;
        }
    }
}

static int hvdxg_destroy_device_host_process(uint32 process, uint32 device)
{
    struct hvdxg_command_destroydevice destroy;
    struct hvdxg_ntstatus status;
    struct hvdxg_d3dkmthandle process_handle;
    uint32 actual_len = 0;
    int ret;

    if (device == 0)
        return 0;
    memset(&destroy, 0, sizeof(destroy));
    memset(&status, 0, sizeof(status));
    actual_len = 0;
    process_handle.v = process;
    if (process_handle.v == 0)
        process_handle = hvdxg.dxg_process;
    hvdxg_command_vgpu_init_process(&destroy.hdr,
                                    HV_DXGK_VMBCOMMAND_DESTROYDEVICE,
                                    process_handle);
    destroy.device.v = device;
    ret = hvdxg_send_sync_vgpu(&destroy, sizeof(destroy), &status,
                               sizeof(status), &actual_len);
    if (actual_len >= sizeof(status))
        hvdxg.destroydevice_last_status = status.v;
    if (ret == 0 && actual_len < sizeof(status))
        ret = -EOVERFLOW;
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg.destroydevice_last_len = actual_len;
    hvdxg.destroydevice_last_ret = ret;
    return ret;
}

static int hvdxg_destroy_device_host(uint32 device)
{
    return hvdxg_destroy_device_host_process(hvdxg.dxg_process.v, device);
}

static int hvdxg_flush_device_host(uint32 device)
{
    struct hvdxg_command_flushdevice flush;
    struct hvdxg_ntstatus status;
    uint32 actual_len = 0;
    int ret;

    if (device == 0)
        return 0;
    memset(&flush, 0, sizeof(flush));
    memset(&status, 0, sizeof(status));
    hvdxg_command_vgpu_init_process(&flush.hdr,
                                    HV_DXGK_VMBCOMMAND_FLUSHDEVICE,
                                    hvdxg.dxg_process);
    flush.device.v = device;
    flush.reason = HV_DXG_FLUSHSCHEDULER_DEVICE_TERMINATE;
    ret = hvdxg_send_sync_vgpu(&flush, sizeof(flush), &status,
                               sizeof(status), &actual_len);
    if (ret == 0 && actual_len < sizeof(status))
        ret = -EOVERFLOW;
    if (ret == 0)
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg.flushdevice_last_len = actual_len;
    hvdxg.flushdevice_last_ret = ret;
    hvdxg.flushdevice_last_status = status.v;
    hvdxg.flushdevice_last_device = device;
    hvdxg.flushdevice_last_reason = HV_DXG_FLUSHSCHEDULER_DEVICE_TERMINATE;
    return ret;
}

static void hvdxg_note_destroyallocation(uint32 device, uint32 resource,
                                         uint32 allocation, uint32 process,
                                         uint32 context, uint32 count,
                                         uint32 actual_len, int ret,
                                         struct hvdxg_ntstatus status)
{
    uint32 match = 0;
    uint32 pending = 0;
    uint32 before_nt = 0;

    hvdxg.destroyalloc_last_device = device;
    hvdxg.destroyalloc_last_resource = resource;
    hvdxg.destroyalloc_last_allocation = allocation;
    hvdxg.destroyalloc_last_process = process;
    hvdxg.destroyalloc_last_context = context;
    hvdxg.destroyalloc_last_count = count;
    hvdxg.destroyalloc_last_len = actual_len;
    hvdxg.destroyalloc_last_ret = ret;
    hvdxg.destroyalloc_last_status = status.v;

    if (resource != 0 &&
        resource == hvdxg.d3d12_shared_alloc_resource_out)
        match |= 1U;
    if (allocation != 0 &&
        allocation == hvdxg.d3d12_shared_alloc_allocation)
        match |= 2U;
    if (match != 0) {
        uint64 seq = ++hvdxg.d3d12_shared_event_seq;

        hvdxg.destroyalloc_d3d12_last_seq = seq;
        hvdxg.destroyalloc_d3d12_match_count++;
        if (context < 32)
            hvdxg.destroyalloc_d3d12_context_mask |= 1U << context;
        if (hvdxg.sharedresource_owner_nt == 0 &&
            hvdxg.sharedresource_owner_sealed == 0) {
            pending = 1;
            hvdxg.destroyalloc_d3d12_pending_match_count++;
        }
        before_nt = hvdxg.d3d12_shared_first_nt_seq == 0 ||
                    seq < hvdxg.d3d12_shared_first_nt_seq;
        hvdxg.destroyalloc_d3d12_last_before_nt = before_nt;
        if (hvdxg.destroyalloc_d3d12_first_seq == 0) {
            hvdxg.destroyalloc_d3d12_first_seq = seq;
            hvdxg.destroyalloc_d3d12_first_context = context;
            hvdxg.destroyalloc_d3d12_first_match = match;
            hvdxg.destroyalloc_d3d12_first_pending = pending;
            hvdxg.destroyalloc_d3d12_first_before_nt = before_nt;
        }
    }
    hvdxg.destroyalloc_d3d12_last_match = match | (pending ? 4U : 0U);
}

static int hvdxg_destroy_allocation_host_process(
    uint32 process, uint32 device, uint32 resource,
    uint32 allocation, uint32 context)
{
    uint8 command_buf[sizeof(struct hvdxg_command_destroyallocation) +
                      sizeof(struct hvdxg_d3dkmthandle)];
    struct hvdxg_command_destroyallocation *destroy =
        (struct hvdxg_command_destroyallocation *)command_buf;
    struct hvdxg_ntstatus status;
    struct hvdxg_d3dkmthandle process_handle;
    uint32 actual_len = 0;
    uint32 command_len;
    int ret;

    if (process == 0 || device == 0 || (resource == 0 && allocation == 0))
        return 0;
    memset(command_buf, 0, sizeof(command_buf));
    memset(&status, 0, sizeof(status));
    process_handle.v = process;
    hvdxg_command_vgpu_init_process(&destroy->hdr,
                                    HV_DXGK_VMBCOMMAND_DESTROYALLOCATION,
                                    process_handle);
    destroy->device.v = device;
    destroy->resource.v = resource;
    destroy->flags.assume_not_in_use = 1;
    if (resource == 0 && allocation != 0) {
        destroy->alloc_count = 1;
        destroy->allocations[0].v = allocation;
    }
    command_len = sizeof(*destroy) +
                  destroy->alloc_count * sizeof(destroy->allocations[0]);
    ret = hvdxg_send_sync_vgpu(destroy, command_len, &status,
                               sizeof(status), &actual_len);
    if (ret == 0 && actual_len < sizeof(status))
        ret = -EOVERFLOW;
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg_note_destroyallocation(device, resource, allocation,
                                 destroy->hdr.process.v, context,
                                 destroy->alloc_count, actual_len, ret,
                                 status);
    return ret;
}

static int hvdxg_destroy_allocation_host(uint32 device, uint32 resource,
                                         uint32 allocation, uint32 context)
{
    return hvdxg_destroy_allocation_host_process(
        hvdxg.dxg_process.v, device, resource, allocation, context);
}

static int hvdxg_destroy_device_owned_objects(struct hvdxg_open_state *owner,
                                              uint32 device)
{
    int ret = 0;

    if (owner == NULL || device == 0)
        return 0;

    for (;;) {
        int found = 0;

        for (uint32 i = 0; i < owner->sync_object_count; i++) {
            if (owner->sync_objects[i].device != device)
                continue;
            /*
             * WSL dxgdevice_destroy() calls dxgsyncobject_destroy() here,
             * which drops the handle and event mappings locally without a
             * DESTROYSYNCOBJECT VM-bus packet.
             */
            hvdxg_cleanup_note_ret(&ret, 0, HV_DXG_CLEANUP_SYNC,
                                   owner->sync_objects[i].sync);
            hvdxg_untrack_sync(owner, owner->sync_objects[i].sync);
            found = 1;
            break;
        }
        if (!found)
            break;
    }

    for (;;) {
        struct hvdxg_tracked_allocation a;
        int found = 0;

        for (uint32 i = 0; i < owner->allocation_count; i++) {
            a = owner->allocations[i];
            if (a.device != device || a.resource != 0 || a.allocation == 0)
                continue;
            hvdxg_untrack_allocation(owner, device, 0, a.allocation);
            (void)hvdxg_unmap_tracked_allocation(&a);
            hvdxg_unpin_tracked_allocation(&a);
            hvdxg_cleanup_note_ret(
                &ret,
                hvdxg_destroy_allocation_host(
                    device, 0, a.allocation,
                    HV_DXG_DESTROY_ALLOC_CTX_DEVICE_STANDALONE),
                HV_DXG_CLEANUP_ALLOCATION, a.allocation);
            found = 1;
            break;
        }
        if (!found)
            break;
    }

    for (;;) {
        uint32 resource = 0;
        int found = 0;

        for (uint32 i = 0; i < owner->resource_count; i++) {
            resource = owner->resources[i].resource;
            if (owner->resources[i].device == device && resource != 0) {
                found = 1;
                break;
            }
        }
        if (!found)
            break;

        for (;;) {
            int removed = 0;

            for (uint32 i = 0; i < owner->allocation_count; i++) {
                struct hvdxg_tracked_allocation a = owner->allocations[i];

                if (a.device != device || a.resource != resource)
                    continue;
                hvdxg_untrack_allocation(owner, device, resource,
                                         a.allocation);
                (void)hvdxg_unmap_tracked_allocation(&a);
                hvdxg_unpin_tracked_allocation(&a);
                hvdxg_cleanup_note_ret(&ret, 0,
                                       HV_DXG_CLEANUP_ALLOCATION,
                                       a.allocation);
                removed = 1;
                break;
            }
            if (!removed)
                break;
        }
        hvdxg_cleanup_note_ret(
            &ret,
            hvdxg_destroy_allocation_host(
                device, resource, 0,
                HV_DXG_DESTROY_ALLOC_CTX_DEVICE_RESOURCE),
            HV_DXG_CLEANUP_RESOURCE, resource);
        hvdxg_untrack_resource(owner, device, resource);
    }

    for (;;) {
        int found = 0;

        for (uint32 i = 0; i < owner->context_count; i++) {
            uint32 context = owner->contexts[i];
            uint32 sync = 0;

            if (hvdxg_owner_object_device(owner, HV_DXG_OBJECT_CONTEXT,
                                          context) != device)
                continue;
            /*
             * WSL dxgdevice_destroy() releases context handles locally; it
             * does not send DESTROYCONTEXT during process/device teardown.
             */
            hvdxg_cleanup_note_ret(&ret, 0, HV_DXG_CLEANUP_CONTEXT,
                                   context);
            hvdxg_untrack_object(owner, HV_DXG_OBJECT_CONTEXT, context);
            hvdxg_untrack_u32(owner->contexts, &owner->context_count,
                              context);
            for (;;) {
                int removed_hwqueue = 0;

                for (uint32 j = 0; j < owner->hwqueue_count; j++) {
                    uint32 queue;

                    if (owner->hwqueues[j].device != device ||
                        owner->hwqueues[j].context != context)
                        continue;
                    queue = owner->hwqueues[j].queue;
                    sync = hvdxg_untrack_hwqueue(owner, queue);
                    hvdxg_cleanup_note_ret(
                        &ret, 0, HV_DXG_CLEANUP_HWQUEUE, queue);
                    if (sync != 0)
                        hvdxg_untrack_sync(owner, sync);
                    removed_hwqueue = 1;
                    break;
                }
                if (!removed_hwqueue)
                    break;
            }
            found = 1;
            break;
        }
        if (!found)
            break;
    }

    for (;;) {
        int found = 0;

        /*
         * If a context was never tracked successfully, still drop orphaned
         * HW-queue handles locally before paging queues, matching WSL's
         * "children before final device/process" cleanup intent.
         */
        for (uint32 i = 0; i < owner->hwqueue_count; i++) {
            uint32 queue = owner->hwqueues[i].queue;
            uint32 sync;

            if (owner->hwqueues[i].device != device)
                continue;
            sync = hvdxg_untrack_hwqueue(owner, queue);
            hvdxg_cleanup_note_ret(&ret, 0, HV_DXG_CLEANUP_HWQUEUE,
                                   queue);
            if (sync != 0)
                hvdxg_untrack_sync(owner, sync);
            found = 1;
            break;
        }
        if (!found)
            break;
    }

    for (;;) {
        int found = 0;

        for (uint32 i = 0; i < owner->paging_queue_count; i++) {
            uint32 sync;
            uint32 queue = owner->paging_queues[i].queue;

            if (owner->paging_queues[i].device != device)
                continue;
            sync = hvdxg_untrack_pagingqueue(owner,
                                             queue);
            if (sync != 0)
                hvdxg_untrack_sync(owner, sync);
            /*
             * Paging queue and monitored-fence handles are dropped locally
             * during WSL device teardown.
             */
            hvdxg_cleanup_note_ret(&ret, 0, HV_DXG_CLEANUP_PAGINGQUEUE,
                                   queue);
            found = 1;
            break;
        }
        if (!found)
            break;
    }

    for (;;) {
        int found = 0;

        for (uint32 i = 0; i < owner->sync_object_count; i++) {
            if (owner->sync_objects[i].device != device)
                continue;
            hvdxg_untrack_sync(owner, owner->sync_objects[i].sync);
            found = 1;
            break;
        }
        if (!found)
            break;
    }

    return ret;
}

static int hvdxg_destroy_process_adapter_devices(
    struct hvdxg_open_state *owner, uint32 host_adapter)
{
    int ret = 0;

    if (owner == NULL || owner->process_state == NULL ||
        owner->process_state->adapters == NULL || host_adapter == 0)
        return 0;

    for (uint32 i = 0; i < owner->process_state->adapter_count; i++) {
        struct hvdxg_process_adapter *adapter =
            &owner->process_state->adapters[i];

        if (adapter->host_adapter_handle != host_adapter ||
            adapter->refs != 0)
            continue;
        while (adapter->device_count > 0) {
            uint32 device = adapter->devices[--adapter->device_count];

            adapter->devices[adapter->device_count] = 0;
            if (device == 0)
                continue;
            hvdxg_untrack_u32(owner->devices, &owner->device_count,
                              device);
            if (hvdxg_untrack_object(owner, HV_DXG_OBJECT_DEVICE,
                                     device)) {
                int flush_ret = hvdxg_flush_device_host(device);

                if (ret == 0 && flush_ret != 0) {
                    ret = flush_ret;
                    hvdxg.cleanup_failed_op = HV_DXG_CLEANUP_DEVICE;
                    hvdxg.cleanup_failed_handle = device;
                }
                hvdxg_cleanup_note_ret(
                    &ret, hvdxg_destroy_device_owned_objects(owner, device),
                    HV_DXG_CLEANUP_DEVICE, device);
                hvdxg_cleanup_note_ret(
                    &ret, hvdxg_destroy_device_host(device),
                    HV_DXG_CLEANUP_DEVICE, device);
            }
        }
    }
    return ret;
}

static int hvdxg_destroy_createallocation_result(
    uint32 process, uint32 device, uint32 resource,
    const struct hvdxg_command_createallocation_return *result,
    uint32 alloc_count)
{
    uint8 command_buf[sizeof(struct hvdxg_command_destroyallocation) +
                      HV_DXG_ALLOCATION_MAX *
                          sizeof(struct hvdxg_d3dkmthandle)];
    struct hvdxg_command_destroyallocation *destroy =
        (struct hvdxg_command_destroyallocation *)command_buf;
    struct hvdxg_ntstatus status;
    struct hvdxg_d3dkmthandle process_handle;
    uint32 actual_len = 0;
    uint32 command_len;
    int ret;

    if (process == 0 || device == 0 || result == NULL ||
        (resource == 0 && alloc_count == 0) ||
        alloc_count > HV_DXG_ALLOCATION_MAX)
        return 0;
    memset(command_buf, 0, sizeof(command_buf));
    memset(&status, 0, sizeof(status));
    process_handle.v = process;
    hvdxg_command_vgpu_init_process(&destroy->hdr,
                                    HV_DXGK_VMBCOMMAND_DESTROYALLOCATION,
                                    process_handle);
    destroy->device.v = device;
    destroy->resource.v = resource;
    destroy->alloc_count = alloc_count;
    destroy->flags.assume_not_in_use = 1;
    for (uint32 i = 0; i < alloc_count; i++)
        destroy->allocations[i] = result->allocation_info[i].allocation;
    command_len = sizeof(*destroy) +
                  alloc_count * sizeof(destroy->allocations[0]);
    ret = hvdxg_send_sync_vgpu(destroy, command_len, &status,
                               sizeof(status), &actual_len);
    if (ret == 0 && actual_len < sizeof(status))
        ret = -EOVERFLOW;
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg_note_destroyallocation(
        device, resource,
        alloc_count != 0 ? result->allocation_info[0].allocation.v : 0,
        destroy->hdr.process.v, HV_DXG_DESTROY_ALLOC_CTX_CREATE_UNWIND,
        alloc_count, actual_len, ret, status);
    hvdxg.createalloc_unwind_attempts++;
    hvdxg.createalloc_unwind_last_ret = ret;
    if (ret == 0)
        hvdxg.createalloc_unwind_successes++;
    return ret;
}

static int hvdxg_destroy_context_host_process(uint32 process, uint32 context)
{
    struct hvdxg_command_destroycontext destroy;
    struct hvdxg_ntstatus status;
    struct hvdxg_d3dkmthandle process_handle;
    uint32 actual_len = 0;
    int ret;

    if (context == 0)
        return 0;
    memset(&destroy, 0, sizeof(destroy));
    memset(&status, 0, sizeof(status));
    process_handle.v = process;
    if (process_handle.v == 0)
        process_handle = hvdxg.dxg_process;
    hvdxg_command_vgpu_init_process(&destroy.hdr,
                                    HV_DXGK_VMBCOMMAND_DESTROYCONTEXT,
                                    process_handle);
    destroy.context.v = context;
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
    return ret;
}

static int hvdxg_destroy_pagingqueue_host_process(uint32 process,
                                                  uint32 paging_queue)
{
    struct hvdxg_command_destroypagingqueue destroy;
    struct hvdxg_ntstatus status;
    uint32 actual_len = 0;
    int ret;

    if (paging_queue == 0)
        return 0;
    memset(&destroy, 0, sizeof(destroy));
    memset(&status, 0, sizeof(status));
    hvdxg_command_vgpu_init_process(&destroy.hdr,
                                    HV_DXGK_VMBCOMMAND_DESTROYPAGINGQUEUE,
                                    (struct hvdxg_d3dkmthandle){ .v = process });
    destroy.paging_queue.v = paging_queue;
    ret = hvdxg_send_sync_vgpu(&destroy, sizeof(destroy), &status,
                               sizeof(status), &actual_len);
    if (ret == 0 && actual_len < sizeof(status))
        ret = -EOVERFLOW;
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    return ret;
}

static int hvdxg_destroy_hwqueue_host_process(uint32 process, uint32 hwqueue)
{
    struct hvdxg_command_destroyhwqueue destroy;
    struct hvdxg_ntstatus status;
    struct hvdxg_d3dkmthandle process_handle;
    uint32 actual_len = 0;
    int ret;

    if (hwqueue == 0)
        return 0;
    memset(&destroy, 0, sizeof(destroy));
    memset(&status, 0, sizeof(status));
    process_handle.v = process;
    if (process_handle.v == 0)
        process_handle = hvdxg.dxg_process;
    hvdxg_command_vgpu_init_process(&destroy.hdr,
                                    HV_DXGK_VMBCOMMAND_DESTROYHWQUEUE,
                                    process_handle);
    destroy.hwqueue.v = hwqueue;
    ret = hvdxg_send_sync_vgpu(&destroy, sizeof(destroy), &status,
                               sizeof(status), &actual_len);
    if (ret == 0 && actual_len < sizeof(status))
        ret = -EOVERFLOW;
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg.destroyhwqueue_last_len = actual_len;
    hvdxg.destroyhwqueue_last_ret = ret;
    return ret;
}

static void hvdxg_wait_gpuva_fence(const struct hvdxg_tracked_gpuva *gpuva)
{
    if (gpuva == NULL || gpuva->fence_value == 0 ||
        gpuva->fence_cpu_pa == 0)
        return;
    /*
     * The paging fence storage lives in Hyper-V I/O space. It is mapped into
     * the user process, but not into the kernel direct map on all hosts.
     */
    sleep_ms(20);
}

static void hvdxg_cleanup_note_ret(int *cleanup_ret, int op_ret,
                                   uint32 op, uint32 handle)
{
    hvdxg_cleanup_mark_wsl_order(op);
    hvdxg.cleanup_last_op = op;
    hvdxg.cleanup_last_handle = handle;
    if (*cleanup_ret == 0 && op_ret != 0) {
        *cleanup_ret = op_ret;
        hvdxg.cleanup_failed_op = op;
        hvdxg.cleanup_failed_handle = handle;
    }
}

static int hvdxg_cleanup_order_before(uint32 a, uint32 b)
{
    return a == 0 || b == 0 || a <= b;
}

static void hvdxg_cleanup_reset_wsl_order(void)
{
    hvdxg.cleanup_wsl_order_seq = 0;
    hvdxg.cleanup_wsl_order_sync = 0;
    hvdxg.cleanup_wsl_order_allocation = 0;
    hvdxg.cleanup_wsl_order_resource = 0;
    hvdxg.cleanup_wsl_order_context = 0;
    hvdxg.cleanup_wsl_order_hwqueue = 0;
    hvdxg.cleanup_wsl_order_pagingqueue = 0;
    hvdxg.cleanup_wsl_order_gpuva = 0;
    hvdxg.cleanup_wsl_order_device = 0;
    hvdxg.cleanup_wsl_order_process = 0;
    hvdxg.cleanup_wsl_order_valid = 0;
}

static void hvdxg_cleanup_mark_wsl_order(uint32 op)
{
    uint32 order;
    uint32 *slot = NULL;

    switch (op) {
    case HV_DXG_CLEANUP_SYNC:
        slot = &hvdxg.cleanup_wsl_order_sync;
        break;
    case HV_DXG_CLEANUP_ALLOCATION:
        slot = &hvdxg.cleanup_wsl_order_allocation;
        break;
    case HV_DXG_CLEANUP_RESOURCE:
        slot = &hvdxg.cleanup_wsl_order_resource;
        break;
    case HV_DXG_CLEANUP_CONTEXT:
        slot = &hvdxg.cleanup_wsl_order_context;
        break;
    case HV_DXG_CLEANUP_HWQUEUE:
        slot = &hvdxg.cleanup_wsl_order_hwqueue;
        break;
    case HV_DXG_CLEANUP_PAGINGQUEUE:
        slot = &hvdxg.cleanup_wsl_order_pagingqueue;
        break;
    case HV_DXG_CLEANUP_GPUVA:
        slot = &hvdxg.cleanup_wsl_order_gpuva;
        break;
    case HV_DXG_CLEANUP_DEVICE:
        slot = &hvdxg.cleanup_wsl_order_device;
        break;
    case HV_DXG_CLEANUP_NONE:
        slot = &hvdxg.cleanup_wsl_order_process;
        break;
    default:
        return;
    }
    if (slot == NULL || *slot != 0)
        return;
    order = ++hvdxg.cleanup_wsl_order_seq;
    if (order == 0)
        order = ++hvdxg.cleanup_wsl_order_seq;
    *slot = order;
}

static void hvdxg_cleanup_finalize_wsl_order(void)
{
    int valid = 1;

    valid = valid && hvdxg_cleanup_order_before(
        hvdxg.cleanup_wsl_order_sync,
        hvdxg.cleanup_wsl_order_allocation);
    valid = valid && hvdxg_cleanup_order_before(
        hvdxg.cleanup_wsl_order_allocation,
        hvdxg.cleanup_wsl_order_resource);
    valid = valid && hvdxg_cleanup_order_before(
        hvdxg.cleanup_wsl_order_resource,
        hvdxg.cleanup_wsl_order_context);
    valid = valid && hvdxg_cleanup_order_before(
        hvdxg.cleanup_wsl_order_context,
        hvdxg.cleanup_wsl_order_hwqueue);
    valid = valid && hvdxg_cleanup_order_before(
        hvdxg.cleanup_wsl_order_hwqueue,
        hvdxg.cleanup_wsl_order_pagingqueue);
    valid = valid && hvdxg_cleanup_order_before(
        hvdxg.cleanup_wsl_order_pagingqueue,
        hvdxg.cleanup_wsl_order_device);
    valid = valid && hvdxg_cleanup_order_before(
        hvdxg.cleanup_wsl_order_device,
        hvdxg.cleanup_wsl_order_process);
    hvdxg.cleanup_wsl_order_valid = valid ? 1 : 0;
}

static struct hvdxg_process_state *hvdxg_process_get_current(void)
{
    struct hvdxg_process_state *candidate;
    struct hvdxg_process_state *process = NULL;
    uint64 tgid = current ? (uint64)thread_tgid(current) : 1;
    int empty = -1;

    candidate = kvmalloc(sizeof(*candidate));
    if (candidate == NULL)
        return NULL;
    memset(candidate, 0, sizeof(*candidate));
    candidate->tgid = tgid;
    /*
     * WSL keys dxgprocess reuse by TGID, but the CREATEPROCESS packet carries
     * the creator thread pid.  Mesa can open /dev/dxg from worker threads, and
     * hosts may reject a CREATEPROCESS whose process_id does not match that
     * Linux thread identity.
     */
    candidate->pid = current ? (uint64)current->pid : tgid;
    candidate->process_refs = 1;
    candidate->process_mem_refs = 1;

    mutex_lock(&hvdxg.process_lock);
    for (uint32 i = 0; i < HV_DXG_PROCESS_TABLE_MAX; i++) {
        struct hvdxg_process_state *entry = hvdxg.processes[i];

        if (entry == NULL) {
            if (empty < 0)
                empty = (int)i;
            continue;
        }
        if (entry->tgid == tgid && entry->process_refs != 0) {
            entry->process_refs++;
            entry->process_mem_refs++;
            entry->pid = current ? (uint64)current->pid : tgid;
            hvdxg.process_retained_handle = entry->host_process.v;
            hvdxg.process_retained_generation = entry->generation;
            hvdxg.process_retained_tgid = entry->tgid;
            hvdxg.process_retained_refs = entry->process_refs;
            hvdxg.process_object_refs_last = entry->process_refs;
            hvdxg.process_mem_refs_last = entry->process_mem_refs;
            hvdxg.process_reuses++;
            hvdxg.process_shared_reuses++;
            process = entry;
            break;
        }
    }
    if (process == NULL && empty >= 0) {
        uint32 retained_handle;
        uint32 retained_generation;
        uint64 retained_tgid;

        process = candidate;
        process->generation = ++hvdxg.process_generation;
        if (process->generation == 0)
            process->generation = ++hvdxg.process_generation;
        process->guest_process = (uint64)process;
        if (hvdxg.dxg_process_created && hvdxg.dxg_process.v != 0) {
            retained_handle = hvdxg.dxg_process.v;
            retained_tgid = hvdxg.dxg_process_tgid;
            retained_generation = hvdxg.dxg_process_generation;
        } else {
            retained_handle = hvdxg.process_retained_handle;
            retained_tgid = hvdxg.process_retained_tgid;
            retained_generation = hvdxg.process_retained_generation;
        }
        if (retained_handle != 0 &&
            (retained_tgid == 0 || retained_tgid != process->tgid ||
             retained_generation != process->generation)) {
            hvdxg.process_retained_reuse_avoided++;
            hvdxg.process_retained_avoided_tgid = process->tgid;
            hvdxg.process_retained_avoided_source_tgid =
                retained_tgid;
            hvdxg.process_retained_avoided_handle = retained_handle;
            hvdxg.process_retained_avoided_generation =
                process->generation;
            hvdxg.process_retained_avoided_source_generation =
                retained_generation;
        }
        hvdxg.process_isolated_last_handle = 0;
        hvdxg.processes[empty] = process;
        hvdxg.process_live++;
        hvdxg.process_creates++;
        hvdxg.process_isolated_last_tgid = process->tgid;
        hvdxg.process_isolated_last_generation = process->generation;
        hvdxg.process_isolated_source_generation = 0;
        hvdxg.process_isolated_copied_objects = 0;
        hvdxg.process_isolated_source_objects = 0;
        if (hvdxg.process_live > hvdxg.process_live_max)
            hvdxg.process_live_max = hvdxg.process_live;
        hvdxg.process_object_refs_last = process->process_refs;
        hvdxg.process_mem_refs_last = process->process_mem_refs;
        candidate = NULL;
    } else if (process == NULL) {
        hvdxg.process_table_full++;
    }
    mutex_unlock(&hvdxg.process_lock);

    if (candidate != NULL)
        kvfree(candidate);
    return process;
}

static void hvdxg_process_memory_put(struct hvdxg_process_state *process)
{
    int free_process = 0;

    if (process == NULL)
        return;

    mutex_lock(&hvdxg.process_lock);
    if (process->process_mem_refs > 0)
        process->process_mem_refs--;
    hvdxg.process_mem_releases++;
    hvdxg.process_object_refs_last = process->process_refs;
    hvdxg.process_mem_refs_last = process->process_mem_refs;
    if (process->process_mem_refs == 0) {
        free_process = 1;
        hvdxg.process_mem_frees++;
    }
    mutex_unlock(&hvdxg.process_lock);

    if (free_process) {
        if (process->objects != NULL)
            kvfree(process->objects);
        if (process->adapters != NULL) {
            for (uint32 i = 0; i < process->adapter_count; i++) {
                if (process->adapters[i].devices != NULL)
                    kvfree(process->adapters[i].devices);
            }
            kvfree(process->adapters);
        }
        if (process->local_adapters != NULL)
            kvfree(process->local_adapters);
        kvfree(process);
    }
}

static int hvdxg_process_put(struct hvdxg_process_state *process)
{
    int remove_process = 0;
    int destroy_host = 0;
    int ret = 0;
    struct hvdxg_d3dkmthandle host_process;

    if (process == NULL)
        return 0;
    memset(&host_process, 0, sizeof(host_process));

    mutex_lock(&hvdxg.process_lock);
    if (process->process_refs > 0)
        process->process_refs--;
    hvdxg.process_object_refs_last = process->process_refs;
    hvdxg.process_mem_refs_last = process->process_mem_refs;
    if (process->process_refs == 0) {
        hvdxg.process_releases++;
        if (process->host_process_created && process->host_process.v != 0) {
            hvdxg.process_destroy_active_total = 0;
            hvdxg.process_destroy_active_device = 0;
            hvdxg.process_destroy_active_context = 0;
            hvdxg.process_destroy_active_hwqueue = 0;
            hvdxg.process_destroy_active_pagingqueue = 0;
            hvdxg.process_destroy_active_sync = 0;
            hvdxg.process_destroy_active_allocation = 0;
            hvdxg.process_destroy_active_resource = 0;
            hvdxg.process_destroy_active_gpuva = 0;
            for (uint32 i = 0; i < process->object_count; i++) {
                if (process->objects == NULL ||
                    process->objects[i].type == HV_DXG_OBJECT_NONE ||
                    process->objects[i].destroyed != 0)
                    continue;
                hvdxg.process_destroy_active_total++;
                switch (process->objects[i].type) {
                case HV_DXG_OBJECT_ADAPTER:
                    break;
                case HV_DXG_OBJECT_DEVICE:
                    hvdxg.process_destroy_active_device++;
                    break;
                case HV_DXG_OBJECT_CONTEXT:
                    hvdxg.process_destroy_active_context++;
                    break;
                case HV_DXG_OBJECT_HWQUEUE:
                    hvdxg.process_destroy_active_hwqueue++;
                    break;
                case HV_DXG_OBJECT_PAGINGQUEUE:
                    hvdxg.process_destroy_active_pagingqueue++;
                    break;
                case HV_DXG_OBJECT_SYNC:
                    hvdxg.process_destroy_active_sync++;
                    break;
                case HV_DXG_OBJECT_ALLOCATION:
                    hvdxg.process_destroy_active_allocation++;
                    break;
                case HV_DXG_OBJECT_RESOURCE:
                    hvdxg.process_destroy_active_resource++;
                    break;
                case HV_DXG_OBJECT_GPUVA:
                    hvdxg.process_destroy_active_gpuva++;
                    break;
                default:
                    break;
                }
            }
            host_process = process->host_process;
            destroy_host = 1;
            remove_process = 1;
            process->host_process.v = 0;
            process->host_process_created = 0;
            if (hvdxg.dxg_process.v == host_process.v) {
                hvdxg.dxg_process.v = 0;
                hvdxg.dxg_process_created = 0;
                hvdxg.dxg_process_guest = 0;
                hvdxg.dxg_process_pid = 0;
                hvdxg.dxg_process_tgid = 0;
                hvdxg.d3dkmt_ready = 0;
            }
        } else {
            remove_process = 1;
        }
    }
    mutex_unlock(&hvdxg.process_lock);

    if (destroy_host)
        ret = hvdxg_destroy_process_host(host_process);

    if (remove_process) {
        mutex_lock(&hvdxg.process_lock);
        for (uint32 i = 0; i < HV_DXG_PROCESS_TABLE_MAX; i++) {
            if (hvdxg.processes[i] == process) {
                hvdxg.processes[i] = NULL;
                break;
            }
        }
        if (hvdxg.process_live > 0)
            hvdxg.process_live--;
        mutex_unlock(&hvdxg.process_lock);
    }
    hvdxg_process_memory_put(process);
    return ret;
}

static uint32 hvdxg_process_refs(struct hvdxg_process_state *process)
{
    uint32 refs = 0;

    if (process == NULL)
        return 0;
    mutex_lock(&hvdxg.process_lock);
    refs = process->process_refs;
    mutex_unlock(&hvdxg.process_lock);
    return refs;
}

#define HV_DXG_BIND_SOURCE_ENUMADAPTERS2       1U
#define HV_DXG_BIND_SOURCE_ENUMADAPTERS3       2U
#define HV_DXG_BIND_SOURCE_OPENADAPTERFROMLUID 3U

static const char *hvdxg_early_bind_source_name(uint32 source)
{
    switch (source) {
    case HV_DXG_BIND_SOURCE_ENUMADAPTERS2:
        return "enumadapters2";
    case HV_DXG_BIND_SOURCE_ENUMADAPTERS3:
        return "enumadapters3";
    case HV_DXG_BIND_SOURCE_OPENADAPTERFROMLUID:
        return "openadapterfromluid";
    default:
        return "none";
    }
}

static void hvdxg_note_open_createprocess(struct hvdxg_open_state *owner,
                                          int ret)
{
    if (owner == NULL)
        return;
    hvdxg.open_createprocess_attempts++;
    hvdxg.open_createprocess_last_ret = ret;
    hvdxg.open_createprocess_last_guest = owner->dxg_process_guest;
    hvdxg.open_createprocess_last_pid = owner->dxg_process_pid;
    hvdxg.open_createprocess_last_tgid =
        owner->process_state != NULL ? owner->process_state->tgid :
        owner->dxg_process_pid;
    hvdxg.open_createprocess_last_handle = owner->dxg_process.v;
    hvdxg.open_createprocess_last_created = owner->dxg_process_created;
    hvdxg.open_createprocess_last_generation =
        owner->process_state != NULL ? owner->process_state->generation : 0;
    hvdxg.open_createprocess_last_refs =
        owner->process_state != NULL ?
            owner->process_state->process_refs : 0;
    if (ret == 0)
        hvdxg.open_createprocess_successes++;
    else
        hvdxg.open_createprocess_failures++;
}

static int hvdxg_bind_open_process(struct hvdxg_open_state *owner)
{
    struct hvdxg_process_state *process;
    int ret;
    int create_attempt;

    if (owner == NULL)
        return 0;
    process = owner->process_state;
    if (process != NULL) {
        create_attempt = !process->host_process_created ||
                         process->host_process.v == 0;
        hvdxg.dxg_process = process->host_process;
        hvdxg.dxg_process_created = process->host_process_created;
        hvdxg.dxg_process_guest = process->guest_process;
        hvdxg.dxg_process_pid = process->pid;
        hvdxg.dxg_process_tgid = process->tgid;
        hvdxg.dxg_process_generation = process->generation;
        if (create_attempt)
            hvdxg.d3dkmt_ready = 0;
        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0) {
            if (create_attempt) {
                process->host_process.v = 0;
                process->host_process_created = 0;
                owner->dxg_process.v = 0;
                owner->dxg_process_created = 0;
                hvdxg.dxg_process.v = 0;
                hvdxg.dxg_process_created = 0;
                hvdxg.d3dkmt_ready = 0;
                hvdxg_note_open_createprocess(owner, ret);
            }
            return ret;
        }
        process->host_process = hvdxg.dxg_process;
        process->host_process_created = hvdxg.dxg_process_created;
        owner->dxg_process = process->host_process;
        owner->dxg_process_guest = process->guest_process;
        owner->dxg_process_pid = process->pid;
        owner->dxg_process_created = process->host_process_created;
        hvdxg.dxg_process_tgid = process->tgid;
        hvdxg.dxg_process_generation = process->generation;
        if (create_attempt)
            hvdxg_note_open_createprocess(owner, ret);
        return 0;
    }
    if (!owner->dxg_process_created || owner->dxg_process.v == 0) {
        create_attempt = 1;
        hvdxg.dxg_process.v = 0;
        hvdxg.dxg_process_created = 0;
        hvdxg.dxg_process_guest = owner->dxg_process_guest;
        hvdxg.dxg_process_pid = owner->dxg_process_pid;
        hvdxg.dxg_process_tgid = owner->dxg_process_pid;
        hvdxg.dxg_process_generation = 0;
        hvdxg.d3dkmt_ready = 0;
        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0) {
            owner->dxg_process.v = 0;
            owner->dxg_process_created = 0;
            hvdxg.dxg_process.v = 0;
            hvdxg.dxg_process_created = 0;
            hvdxg.d3dkmt_ready = 0;
            hvdxg_note_open_createprocess(owner, ret);
            return ret;
        }
        owner->dxg_process = hvdxg.dxg_process;
        owner->dxg_process_guest = hvdxg.dxg_process_guest;
        owner->dxg_process_pid = hvdxg.dxg_process_pid;
        owner->dxg_process_created = hvdxg.dxg_process_created;
        if (create_attempt)
            hvdxg_note_open_createprocess(owner, ret);
        return 0;
    }
    hvdxg.dxg_process = owner->dxg_process;
    hvdxg.dxg_process_guest = owner->dxg_process_guest;
    hvdxg.dxg_process_pid = owner->dxg_process_pid;
    hvdxg.dxg_process_tgid = owner->dxg_process_pid;
    hvdxg.dxg_process_generation = 0;
    hvdxg.dxg_process_created = owner->dxg_process_created;
    if (!hvdxg.dxg_process_created || hvdxg.dxg_process.v == 0)
        hvdxg.d3dkmt_ready = 0;
    ret = hvdxg_d3dkmt_ensure();
    if (ret != 0) {
        if (!hvdxg.dxg_process_created || hvdxg.dxg_process.v == 0)
            hvdxg.d3dkmt_ready = 0;
        return ret;
    }
    return 0;
}

static int hvdxg_should_defer_open_create(
    struct hvdxg_process_state *process)
{
    if (process == NULL)
        return 0;
    if (process->host_process_created && process->host_process.v != 0)
        return 0;
    return hvdxg.process_retained_handle != 0;
}

static int hvdxg_bind_open_process_early(struct hvdxg_open_state *owner,
                                         uint32 source, uint32 cmd)
{
    int ret;

    hvdxg.early_bind_attempts++;
    hvdxg.early_bind_last_source = source;
    hvdxg.early_bind_last_cmd = cmd;
    hvdxg.early_bind_last_handle = 0;
    hvdxg.early_bind_last_created = 0;
    hvdxg.early_bind_last_generation = 0;
    hvdxg.early_bind_last_refs = 0;

    if (owner != NULL) {
        mutex_lock(&hvdxg.process_lock);
        ret = hvdxg_bind_open_process(owner);
        hvdxg.early_bind_last_handle = owner->dxg_process.v;
        hvdxg.early_bind_last_created = owner->dxg_process_created;
        if (owner->process_state != NULL) {
            hvdxg.early_bind_last_generation =
                owner->process_state->generation;
            hvdxg.early_bind_last_refs =
                owner->process_state->process_refs;
        }
        mutex_unlock(&hvdxg.process_lock);
    } else {
        ret = hvdxg_d3dkmt_ensure();
        hvdxg.early_bind_last_handle = hvdxg.dxg_process.v;
        hvdxg.early_bind_last_created = hvdxg.dxg_process_created;
        hvdxg.early_bind_last_generation = hvdxg.dxg_process_generation;
    }

    hvdxg.early_bind_last_ret = ret;
    if (ret == 0)
        hvdxg.early_bind_successes++;
    else
        hvdxg.early_bind_failures++;
    return ret;
}

static void hvdxg_cleanup_open_state(struct hvdxg_open_state *owner)
{
    int had_tracked;
    int ret = 0;

    if (owner == NULL)
        return;
    if (owner->dxg_process_created && owner->dxg_process.v != 0) {
        hvdxg.dxg_process = owner->dxg_process;
        hvdxg.dxg_process_created = 1;
        hvdxg.dxg_process_guest = owner->dxg_process_guest;
        hvdxg.dxg_process_pid = owner->dxg_process_pid;
        if (owner->process_state != NULL) {
            hvdxg.dxg_process_tgid = owner->process_state->tgid;
            hvdxg.dxg_process_generation = owner->process_state->generation;
        }
        hvdxg.d3dkmt_ready = 1;
    } else if (owner->process_state == NULL && !hvdxg.d3dkmt_ready) {
        return;
    }
    had_tracked = owner->device_count != 0 || owner->allocation_count != 0 ||
                  owner->gpuva_count != 0 || owner->sync_object_count != 0 ||
                  owner->paging_queue_count != 0 ||
                  owner->context_count != 0 || owner->hwqueue_count != 0 ||
                  hvdxg_owner_has_active_process_objects(owner) ||
                  (owner->process_state != NULL &&
                   owner->process_state->local_adapter_count != 0);
    hvdxg.cleanup_last_op = HV_DXG_CLEANUP_NONE;
    hvdxg.cleanup_last_handle = 0;
    hvdxg.cleanup_failed_op = HV_DXG_CLEANUP_NONE;
    hvdxg.cleanup_failed_handle = 0;
    hvdxg.cleanup_had_tracked = had_tracked ? 1 : 0;
    hvdxg_cleanup_reset_wsl_order();

    /*
     * WSL binds all /dev/dxg opens in one TGID to one dxgprocess.  A non-final
     * file close only drops the process reference so a secondary fd can keep
     * using handles created before another fd closes.  The final close tears
     * down the local object graph and sends DESTROYPROCESS after children are
     * destroyed, matching WSL's dxgprocess release ordering.
     */
    if (owner->process_state != NULL &&
        hvdxg_process_refs(owner->process_state) > 1) {
        uint32 handle = owner->dxg_process.v;

        hvdxg_cleanup_note_ret(
            &ret, hvdxg_process_put(owner->process_state),
            HV_DXG_CLEANUP_NONE, handle);
        owner->process_state = NULL;
        owner->dxg_process.v = 0;
        owner->dxg_process_created = 0;
        hvdxg.cleanup_attempts++;
        hvdxg.cleanup_last_ret = ret;
        hvdxg_cleanup_finalize_wsl_order();
        if (ret == 0)
            hvdxg.cleanup_successes++;
        return;
    }

    if (owner->gpuvas == NULL)
        owner->gpuva_count = 0;
    while (owner->gpuva_count > 0) {
        struct hvdxg_tracked_gpuva g =
            owner->gpuvas[--owner->gpuva_count];
        int gpuva_ret;
        int active;

        active = hvdxg_untrack_object(owner, HV_DXG_OBJECT_GPUVA, g.base);
        hvdxg_wait_gpuva_fence(&g);
        /*
         * WSL does not send FREEGPUVIRTUALADDRESS for live reservations from
         * process teardown.  Only explicit D3DKMT free ioctls reach the host.
         */
        gpuva_ret = active ? 0 : 0;
        hvdxg_cleanup_note_ret(&ret, gpuva_ret,
                               HV_DXG_CLEANUP_GPUVA, (uint32)g.base);
    }
    if (owner->hwqueues == NULL)
        owner->hwqueue_count = 0;
    if (owner->sync_objects == NULL)
        owner->sync_object_count = 0;
    if (owner->contexts == NULL)
        owner->context_count = 0;
    if (owner->allocations == NULL)
        owner->allocation_count = 0;
    if (owner->resources == NULL)
        owner->resource_count = 0;
    if (owner->paging_queues == NULL)
        owner->paging_queue_count = 0;
    if (owner->devices == NULL)
        owner->device_count = 0;
    while (owner->device_count > 0) {
        uint32 handle = owner->devices[--owner->device_count];

        if (hvdxg_untrack_object(owner, HV_DXG_OBJECT_DEVICE, handle)) {
            int flush_ret = hvdxg_flush_device_host(handle);

            if (ret == 0 && flush_ret != 0) {
                ret = flush_ret;
                hvdxg.cleanup_failed_op = HV_DXG_CLEANUP_DEVICE;
                hvdxg.cleanup_failed_handle = handle;
            }
            hvdxg_cleanup_note_ret(
                &ret,
                hvdxg_destroy_device_owned_objects(owner, handle),
                HV_DXG_CLEANUP_DEVICE, handle);
            hvdxg_cleanup_note_ret(
                &ret,
                hvdxg_destroy_device_host(handle),
                HV_DXG_CLEANUP_DEVICE, handle);
        }
    }
    if (owner->hwqueues == NULL)
        owner->hwqueue_count = 0;
    while (owner->hwqueue_count > 0) {
        uint32 handle = owner->hwqueues[owner->hwqueue_count - 1].queue;
        uint32 sync = hvdxg_untrack_hwqueue(owner, handle);

        if (sync != 0)
            hvdxg_untrack_sync(owner, sync);
        hvdxg_cleanup_note_ret(&ret, 0, HV_DXG_CLEANUP_HWQUEUE, handle);
    }
    if (owner->contexts == NULL)
        owner->context_count = 0;
    while (owner->context_count > 0) {
        uint32 handle = owner->contexts[--owner->context_count];
        if (hvdxg_untrack_object(owner, HV_DXG_OBJECT_CONTEXT, handle))
            hvdxg_cleanup_note_ret(&ret, 0, HV_DXG_CLEANUP_CONTEXT, handle);
    }
    if (owner->allocations == NULL)
        owner->allocation_count = 0;
    while (owner->allocation_count > 0) {
        struct hvdxg_tracked_allocation a =
            owner->allocations[--owner->allocation_count];
        int active = 0;
        if (a.allocation != 0)
            active = hvdxg_untrack_object(owner, HV_DXG_OBJECT_ALLOCATION,
                                          a.allocation);
        (void)hvdxg_unmap_tracked_allocation(&a);
        hvdxg_unpin_tracked_allocation(&a);
        if (active)
            hvdxg_cleanup_note_ret(
                &ret,
                hvdxg_destroy_allocation_host(a.device, a.resource,
                                             a.allocation,
                                             HV_DXG_DESTROY_ALLOC_CTX_FILE_CLEANUP),
                HV_DXG_CLEANUP_ALLOCATION,
                a.allocation != 0 ? a.allocation : a.resource);
    }
    if (owner->resources == NULL)
        owner->resource_count = 0;
    while (owner->resource_count > 0) {
        struct hvdxg_tracked_resource *r =
            &owner->resources[--owner->resource_count];

        hvdxg_untrack_object(owner, HV_DXG_OBJECT_RESOURCE, r->resource);
        hvdxg_shared_parent_remove_resource(r);
        hvdxg_free_tracked_resource(r);
    }
    if (owner->paging_queues == NULL)
        owner->paging_queue_count = 0;
    while (owner->paging_queue_count > 0) {
        uint32 handle =
            owner->paging_queues[owner->paging_queue_count - 1].queue;
        uint32 sync = hvdxg_untrack_pagingqueue(owner, handle);

        if (sync != 0)
            hvdxg_untrack_sync(owner, sync);
        hvdxg_cleanup_note_ret(&ret, 0, HV_DXG_CLEANUP_PAGINGQUEUE, handle);
    }
    if (owner->sync_objects == NULL)
        owner->sync_object_count = 0;
    while (owner->sync_object_count > 0) {
        uint32 handle =
            owner->sync_objects[owner->sync_object_count - 1].sync;
        uint32 monitor_fence =
            owner->sync_objects[owner->sync_object_count - 1].
                monitor_fence_handle;

        hvdxg_untrack_sync(owner, handle);
        if (monitor_fence) {
            hvdxg_cleanup_note_ret(&ret, 0, HV_DXG_CLEANUP_SYNC, handle);
            continue;
        }
        hvdxg_cleanup_note_ret(&ret, 0, HV_DXG_CLEANUP_SYNC, handle);
    }
    hvdxg_cleanup_process_objects(owner, &ret);
    if (owner->process_state != NULL) {
        uint32 handle = owner->dxg_process.v;

        hvdxg_cleanup_mark_wsl_order(HV_DXG_CLEANUP_NONE);
        hvdxg_cleanup_note_ret(
            &ret, hvdxg_process_put(owner->process_state),
            HV_DXG_CLEANUP_NONE, handle);
        owner->process_state = NULL;
        owner->dxg_process.v = 0;
        owner->dxg_process_created = 0;
    } else if (owner->dxg_process_created && owner->dxg_process.v != 0) {
        uint32 handle = owner->dxg_process.v;

        hvdxg_cleanup_mark_wsl_order(HV_DXG_CLEANUP_NONE);
        hvdxg_cleanup_note_ret(
            &ret, hvdxg_destroy_process_host(owner->dxg_process),
            HV_DXG_CLEANUP_NONE, handle);
        owner->dxg_process.v = 0;
        owner->dxg_process_created = 0;
    }
    hvdxg_cleanup_finalize_wsl_order();
    hvdxg.cleanup_attempts++;
    hvdxg.cleanup_last_ret = ret;
    if (ret == 0)
        hvdxg.cleanup_successes++;
    (void)had_tracked;
}

static void hvdxg_free_open_state(struct hvdxg_open_state *owner)
{
    if (owner == NULL)
        return;
    if (owner->allocations == NULL)
        owner->allocation_count = 0;
    while (owner->allocation_count > 0) {
        struct hvdxg_tracked_allocation *a =
            &owner->allocations[--owner->allocation_count];

        (void)hvdxg_unmap_tracked_allocation(a);
        hvdxg_unpin_tracked_allocation(a);
    }
    if (owner->resources == NULL)
        owner->resource_count = 0;
    while (owner->resource_count > 0) {
        struct hvdxg_tracked_resource *r =
            &owner->resources[--owner->resource_count];

        hvdxg_shared_parent_remove_resource(r);
        hvdxg_free_tracked_resource(r);
    }
    owner->gpuva_count = 0;
    owner->object_count = 0;
    owner->object_free_count = 0;
    owner->object_free_head = HV_DXG_HMGR_FREE_NONE;
    owner->object_free_tail = HV_DXG_HMGR_FREE_NONE;
    if (owner->read_status != NULL)
        kvfree(owner->read_status);
    if (owner->allocations != NULL)
        kvfree(owner->allocations);
    if (owner->resources != NULL)
        kvfree(owner->resources);
    if (owner->gpuvas != NULL)
        kvfree(owner->gpuvas);
    if (owner->devices != NULL)
        kvfree(owner->devices);
    if (owner->contexts != NULL)
        kvfree(owner->contexts);
    if (owner->objects != NULL)
        kvfree(owner->objects);
    if (owner->hwqueues != NULL)
        kvfree(owner->hwqueues);
    if (owner->paging_queues != NULL)
        kvfree(owner->paging_queues);
    if (owner->sync_objects != NULL)
        kvfree(owner->sync_objects);
    kvfree(owner);
}
