/*
 * Hyper-V DXG: cdev read, iospace mapping, sysmem pinning, u32 table helpers.
 *
 * Part of the dev/hyperv unity translation unit (included by module.c).
 * Split out of the former hyperv_dxg_objects_shared.c for readability;
 * include order in module.c preserves the original definition order.
 */

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

