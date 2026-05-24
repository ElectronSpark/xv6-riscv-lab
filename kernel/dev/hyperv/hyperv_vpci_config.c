static void hvpci_copy_prefix(uint8 dst[8], const void *src, uint32 len)
{
    uint32 n = len < 8 ? len : 8;

    memset(dst, 0, 8);
    if (src != NULL && n != 0)
        memcpy(dst, src, n);
}

static int hvpci_range_overlaps(uint64 start, uint64 size, uint64 other_start,
                                uint64 other_size)
{
    uint64 end;
    uint64 other_end;

    if (size == 0 || other_size == 0)
        return 0;
    end = start + size;
    other_end = other_start + other_size;
    if (end < start || other_end < other_start)
        return 1;
    return start < other_end && other_start < end;
}

static int hvpci_mmio_candidate_usable(uint64 start, uint32 size)
{
    if (start < HVPCI_STATIC_MMIO_BASE ||
        start + size > HVPCI_STATIC_MMIO_END)
        return 0;
    for (int i = 0; i < platform.mem_count; i++) {
        if (hvpci_range_overlaps(start, size, platform.mem[i].base,
                                 platform.mem[i].size))
            return 0;
    }
    for (int i = 0; i < platform.reserved_count; i++) {
        if (hvpci_range_overlaps(start, size, platform.reserved[i].base,
                                 platform.reserved[i].size))
            return 0;
    }
    if (platform.has_framebuffer &&
        hvpci_range_overlaps(start, size, platform.framebuffer_base,
                             platform.framebuffer_size))
        return 0;
    return 1;
}

static int hvpci_config_window_init(void)
{
    uint64 size = HVPCI_CONFIG_MMIO_LENGTH;

    hvpci.config_window_ok = 0;
    hvpci.config_window_source = 0;
    hvpci.config_window_pa = 0;
    hvpci.config_window_va = 0;
    hvpci.config_window_size = 0;
    hvpci.config_window_ret = -ENODEV;
    hvpci.config_window_candidate = HVPCI_STATIC_MMIO_BASE;
    hvpci.config_window_limit = HVPCI_STATIC_MMIO_END;

    for (uint64 base = HVPCI_STATIC_MMIO_BASE;
         base + size <= HVPCI_STATIC_MMIO_END; base += size) {
        hvpci.config_window_candidate = base;
        if (!hvpci_mmio_candidate_usable(base, (uint32)size)) {
            hvpci.config_window_rejects++;
            continue;
        }
        /*
         * x86 maps this static device-MMIO aperture uncached during early
         * paging setup. The config protocol uses page 0 as the Windows slot
         * selector and page 1 as the selected function's config page.
         */
        hvpci.config_window_ok = 1;
        hvpci.config_window_source = 1;
        hvpci.config_window_pa = base;
        hvpci.config_window_va = base;
        hvpci.config_window_size = (uint32)size;
        hvpci.config_window_ret = 0;
        return 0;
    }
    return hvpci.config_window_ret;
}

static int hvpci_config_read(void *ctx, uint32 token, uint16 offset,
                             uint8 size, uint32 *value)
{
    volatile uint32 *slot_select;
    volatile uint8 *cfg8;
    uint32 raw;
    int ret = 0;
    (void)ctx;

    hvpci.config_read_count++;
    hvpci.config_last_token = token;
    hvpci.config_last_offset = offset;
    hvpci.config_last_size = size;
    hvpci.config_last_ret = 0;
    if (value == NULL) {
        hvpci.config_reject_count++;
        hvpci.config_last_ret = -EINVAL;
        return -EINVAL;
    }
    *value = 0xffffffffU;
    if (!hvpci.config_window_ok || hvpci.config_window_va == 0 ||
        offset + size > HVPCI_CONFIG_PAGE_OFFSET ||
        (size != 1 && size != 2 && size != 4) ||
        (size == 2 && (offset & 1U)) ||
        (size == 4 && (offset & 3U))) {
        hvpci.config_reject_count++;
        hvpci.config_last_ret = -ENOTSUP;
        return -ENOTSUP;
    }

    slot_select = (volatile uint32 *)hvpci.config_window_va;
    cfg8 = (volatile uint8 *)(hvpci.config_window_va +
                              HVPCI_CONFIG_PAGE_OFFSET);
    spin_lock(&hvpci.config_lock);
    *slot_select = token;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    if (size == 1)
        raw = *(volatile uint8 *)(cfg8 + offset);
    else if (size == 2)
        raw = *(volatile uint16 *)(cfg8 + offset);
    else
        raw = *(volatile uint32 *)(cfg8 + offset);
    spin_unlock(&hvpci.config_lock);
    if (size == 1)
        raw &= 0xffU;
    else if (size == 2)
        raw &= 0xffffU;
    *value = raw;
    hvpci.config_last_value = raw;
    hvpci.config_last_ret = ret;
    return ret;
}

static int hvpci_config_write(void *ctx, uint32 token, uint16 offset,
                              uint8 size, uint32 value)
{
    volatile uint32 *slot_select;
    volatile uint8 *cfg8;
    (void)ctx;

    hvpci.config_write_count++;
    hvpci.config_last_token = token;
    hvpci.config_last_offset = offset;
    hvpci.config_last_size = size;
    hvpci.config_last_value = value;
    hvpci.config_last_ret = 0;
    if (!hvpci.config_window_ok || hvpci.config_window_va == 0 ||
        offset + size > HVPCI_CONFIG_PAGE_OFFSET ||
        (size != 1 && size != 2 && size != 4) ||
        (size == 2 && (offset & 1U)) ||
        (size == 4 && (offset & 3U))) {
        hvpci.config_reject_count++;
        hvpci.config_last_ret = -ENOTSUP;
        return -ENOTSUP;
    }

    slot_select = (volatile uint32 *)hvpci.config_window_va;
    cfg8 = (volatile uint8 *)(hvpci.config_window_va +
                              HVPCI_CONFIG_PAGE_OFFSET);
    spin_lock(&hvpci.config_lock);
    *slot_select = token;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    if (size == 1)
        *(volatile uint8 *)(cfg8 + offset) = (uint8)value;
    else if (size == 2)
        *(volatile uint16 *)(cfg8 + offset) = (uint16)value;
    else
        *(volatile uint32 *)(cfg8 + offset) = value;
    spin_unlock(&hvpci.config_lock);
    return 0;
}

static const struct pci_config_backend_ops hvpci_config_ops = {
    .read = hvpci_config_read,
    .write = hvpci_config_write,
};

static int hvpci_register_backend(void)
{
    int ret;

    if (hvpci.backend_registered)
        return hvpci.backend_index;

    ret = pci_register_config_backend("hyperv-vpci", &hvpci_config_ops,
                                      &hvpci);
    hvpci.backend_index = ret;
    hvpci.backend_registered = ret >= 0;
    return ret;
}

static void hvpci_record_child(uint32 index, const void *desc,
                               uint32 desc_size)
{
    const struct hvpci_function_description *d = desc;
    uint32 class_code;
    uint16 subsystem_vendor_id;
    uint16 subsystem_id;

    if (index >= HVPCI_CHILD_MAX || desc == NULL ||
        desc_size < sizeof(struct hvpci_function_description))
        return;

    subsystem_vendor_id = (uint16)(d->subsystem_id & 0xffffU);
    subsystem_id = (uint16)(d->subsystem_id >> 16);
    class_code = ((uint32)d->base_class << 16) |
                 ((uint32)d->subclass << 8) | d->prog_intf;
    hvpci.child[index].win_slot = d->win_slot;
    hvpci.child[index].vendor_id = d->vendor_id;
    hvpci.child[index].device_id = d->device_id;
    hvpci.child[index].class_code = class_code;
    hvpci.child[index].revision_id = d->revision_id;
    hvpci.child[index].subsystem_vendor_id = subsystem_vendor_id;
    hvpci.child[index].subsystem_id = subsystem_id;
    hvpci.child[index].registered = 0;
}

static void hvpci_register_children(void)
{
    uint32 limit;

    if (!hvpci.backend_registered)
        return;

    limit = hvpci.child_count < HVPCI_CHILD_MAX ?
            hvpci.child_count : HVPCI_CHILD_MAX;
    for (uint32 i = 0; i < limit; i++) {
        struct pci_virtual_child child;
        int ret;

        if (hvpci.child[i].registered)
            continue;
        memset(&child, 0, sizeof(child));
        child.backend_index = (uint32)hvpci.backend_index;
        child.backend_token = hvpci.child[i].win_slot;
        child.vendor_id = hvpci.child[i].vendor_id;
        child.device_id = hvpci.child[i].device_id;
        child.class_code = hvpci.child[i].class_code;
        child.revision_id = hvpci.child[i].revision_id;
        child.header_type = 0;
        child.subsystem_vendor_id = hvpci.child[i].subsystem_vendor_id;
        child.subsystem_id = hvpci.child[i].subsystem_id;
        child.irq_line = 0xff;

        ret = pci_register_virtual_child(&child, &hvpci.child[i].bus,
                                         &hvpci.child[i].dev,
                                         &hvpci.child[i].func);
        hvpci.register_last_ret = ret < 0 ? (uint32)(-ret) : 0;
        if (ret == 0) {
            hvpci.child[i].registered = 1;
            hvpci.registered_count++;
        }
    }
}

static int hvpci_parse_relations_desc(const uint8 *buf, uint32 len,
                                      uint32 count_offset,
                                      uint32 desc_offset, uint32 desc_size)
{
    uint32 count;
    uint32 max_count;
    uint32 parsed;

    if (buf == NULL || len < count_offset + sizeof(uint32) ||
        desc_size < sizeof(struct hvpci_function_description))
        return -EINVAL;

    memcpy(&count, buf + count_offset, sizeof(count));
    if (count > 1024)
        return -EOVERFLOW;
    if (count != 0 &&
        (len < desc_offset || (len - desc_offset) / desc_size < count))
        return -EOVERFLOW;

    hvpci.relations_seen++;
    hvpci.relations_len = len;
    hvpci.relations_count = count;
    hvpci.relations_desc_size = desc_size;
    hvpci.relations_count_offset = count_offset;
    hvpci.relations_desc_offset = desc_offset;
    hvpci.child_count = count < HVPCI_CHILD_MAX ? count : HVPCI_CHILD_MAX;
    hvpci.relations_parse_ok = 1;

    max_count = hvpci.child_count;
    parsed = 0;
    for (uint32 i = 0; i < max_count; i++) {
        const uint8 *desc = buf + desc_offset + i * desc_size;

        hvpci_record_child(i, desc, desc_size);
        parsed++;
    }
    hvpci.child_count = parsed;
    hvpci_register_children();
    return 0;
}

static int hvpci_parse_bus_relations(const uint8 *buf, uint32 len)
{
    uint32 msg_type;
    int ret;

    hvpci.relations_parse_ok = 0;
    hvpci.relations_len = len;
    hvpci_copy_prefix(hvpci.relations_prefix, buf, len);

    if (buf == NULL || len < sizeof(struct vmpacket_descriptor) +
        sizeof(uint32))
        return -EINVAL;
    memcpy(&msg_type, buf + sizeof(struct vmpacket_descriptor),
           sizeof(msg_type));
    if (msg_type != HVPCI_BUS_RELATIONS &&
        msg_type != HVPCI_BUS_RELATIONS2)
        return -EINVAL;

    if (msg_type == HVPCI_BUS_RELATIONS2) {
        ret = hvpci_parse_relations_desc(
            buf, len, sizeof(struct vmpacket_descriptor) + sizeof(uint32),
            sizeof(struct vmpacket_descriptor) + sizeof(uint32) +
                sizeof(uint32),
            sizeof(struct hvpci_function_description2));
        if (ret == 0)
            return 0;
    }

    return hvpci_parse_relations_desc(
        buf, len, sizeof(struct vmpacket_descriptor) + sizeof(uint32),
        sizeof(struct vmpacket_descriptor) + sizeof(uint32) + sizeof(uint32),
        sizeof(struct hvpci_function_description));
}
