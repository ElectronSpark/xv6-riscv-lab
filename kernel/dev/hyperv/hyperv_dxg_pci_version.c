/*
 * Hyper-V DXG: cmdline host LUID, vmbus version negotiation, PCI guestcaps discovery.
 *
 * Part of the dev/hyperv unity translation unit (included by module.c).
 * Split out of the former hyperv_dxg_state_diag.c for readability;
 * include order in module.c preserves the original definition order.
 */

static void hvdxg_apply_cmdline_host_luid(void)
{
    char value[32];
    const char *p;
    uint32 high;
    uint32 low;

    if (cmdline_get_param("dxg_host_vgpu_luid", value, sizeof(value)) != 0)
        return;
    p = value;
    high = hvdxg_parse_hex32(p, &p);
    low = 0;
    if (*p == ':' || *p == '-')
        low = hvdxg_parse_hex32(p + 1, NULL);
    else
        low = high;
    hvdxg.host_vgpu_luid.a = low;
    hvdxg.host_vgpu_luid.b = high;
}

static void hvdxg_note_missing_pci_guestcaps_once(void);
static const char *hvdxg_pci_guestcaps_source_name(void);

static uint32 hvdxg_clamp_vmbus_version(uint32 version)
{
    return version >= HV_DXG_VMBUS_INTERFACE_VERSION ?
           HV_DXG_VMBUS_INTERFACE_VERSION :
           HV_DXG_VMBUS_INTERFACE_VERSION_OLD;
}

static int hvdxg_host_v40_signal(void)
{
    return hvdxg.pci_dxg_vmbus_version >= HV_DXG_VMBUS_INTERFACE_VERSION;
}

static void hvdxg_set_active_vmbus_version(uint32 version, uint32 source,
                                           uint32 last_compat)
{
    hvdxg.active_vmbus_version = version;
    hvdxg.active_vmbus_source = source;
    hvdxg.active_vmbus_last_compat = last_compat;
    hvdxg.use_ext_header =
        version >= HV_DXG_VMBUS_INTERFACE_VERSION ? 1 : 0;
}

static int hvdxg_write_pci_vmbus_version(uint32 version)
{
    int ret = -ENODEV;
    uint32 readback = 0;

    hvdxg.pci_dxg_vmbus_write_attempted = 1;
    hvdxg.pci_dxg_vmbus_write_value = version;
    hvdxg.pci_dxg_vmbus_write_readback = 0;
    hvdxg.pci_dxg_vmbus_write_config_supported = 0;
    hvdxg.pci_dxg_vmbus_write_config_ret = -ENODEV;
    hvdxg.pci_dxg_vmbus_write_verify_ret = 0;

    if (hvdxg.pci_guestcaps_source == 2) {
        hvdxg.pci_dxg_vmbus_write_config_supported =
            hvpci.backend_registered && hvpci.config_window_ok ? 1 : 0;
        ret = hvpci_config_write(&hvpci, hvdxg.pci_guestcaps_token,
                                 HV_DXG_PCI_VMBUS_VERSION_OFFSET, 4,
                                 version);
        hvdxg.pci_dxg_vmbus_write_config_ret = ret;
        if (ret == 0) {
            hvdxg.pci_dxg_vmbus_writes++;
            hvdxg.pci_dxg_vmbus_write_verify_ret =
                hvpci_config_read(&hvpci, hvdxg.pci_guestcaps_token,
                                  HV_DXG_PCI_VMBUS_VERSION_OFFSET, 4,
                                  &readback);
        }
    } else if (hvdxg.pci_guestcaps_source == 1) {
        hvdxg.pci_dxg_vmbus_write_config_supported = 1;
        ret = pci_config_try_write32((uint8)hvdxg.pci_bus,
                                     (uint8)hvdxg.pci_dev,
                                     (uint8)hvdxg.pci_func,
                                     HV_DXG_PCI_VMBUS_VERSION_OFFSET,
                                     version);
        hvdxg.pci_dxg_vmbus_write_config_ret = ret;
        if (ret == 0) {
            hvdxg.pci_dxg_vmbus_writes++;
            readback = pci_config_read32((uint8)hvdxg.pci_bus,
                                         (uint8)hvdxg.pci_dev,
                                         (uint8)hvdxg.pci_func,
                                         HV_DXG_PCI_VMBUS_VERSION_OFFSET);
        }
    }

    hvdxg.pci_dxg_vmbus_write_readback = readback;
    hvdxg.pci_dxg_vmbus_write_ret = ret;
    return hvdxg.pci_dxg_vmbus_write_ret;
}

static void hvdxg_note_pci_vmbus_version(uint32 source)
{
    uint32 negotiated;

    hvdxg.pci_guestcaps_source = source;
    negotiated = hvdxg_clamp_vmbus_version(hvdxg.pci_dxg_vmbus_version);
    hvdxg.pci_dxg_vmbus_negotiated_version = negotiated;
    if (hvdxg.pci_dxg_vmbus_version >= HV_DXG_VMBUS_INTERFACE_VERSION)
        (void)hvdxg_write_pci_vmbus_version(negotiated);
    hvdxg_set_active_vmbus_version(negotiated, source,
                                   HV_DXG_VMBUS_LAST_COMPATIBLE_INTERFACE_VERSION);
}

void hyperv_dxg_note_pci(uint32 domain, uint32 bus, uint32 dev, uint32 func,
                         uint32 vendor, uint32 device, uint32 class_code,
                         uint32 guid0, uint32 guid1, uint32 guid2,
                         uint32 guid3, uint32 vmbus_version,
                         uint32 luid_low, uint32 luid_high,
                         uint32 guestcaps_offset, uint32 guestcaps_value,
                         uint32 guestcaps_readback, int guestcaps_ret)
{
    hvdxg.pci_domain = domain;
    hvdxg.pci_bus = bus;
    hvdxg.pci_dev = dev;
    hvdxg.pci_func = func;
    hvdxg.pci_vendor = vendor;
    hvdxg.pci_dxg_device = device;
    hvdxg.pci_class = class_code;
    hvdxg.pci_dxg_vmbus_version = vmbus_version;
    hvdxg.pci_dxg_guid[0] = guid0;
    hvdxg.pci_dxg_guid[1] = guid1;
    hvdxg.pci_dxg_guid[2] = guid2;
    hvdxg.pci_dxg_guid[3] = guid3;
    hvdxg.pci_host_vgpu_luid.a = luid_low;
    hvdxg.pci_host_vgpu_luid.b = luid_high;
    if (luid_low != 0 || luid_high != 0)
        hvdxg.host_vgpu_luid = hvdxg.pci_host_vgpu_luid;
    hvdxg.pci_guestcaps_found = 1;
    hvdxg.pci_guestcaps_scan_done = 1;
    hvdxg.pci_guestcaps_write_attempted = 1;
    hvdxg.pci_guestcaps_write_verified =
        guestcaps_readback == guestcaps_value ? 1 : 0;
    hvdxg.pci_guestcaps_attempts = 1;
    hvdxg.pci_guestcaps_writes = 1;
    hvdxg.pci_guestcaps_offset = guestcaps_offset;
    hvdxg.pci_guestcaps_value = guestcaps_value;
    hvdxg.pci_guestcaps_readback = guestcaps_readback;
    hvdxg.pci_guestcaps_ret =
        guestcaps_ret == 0 && guestcaps_readback != guestcaps_value ?
        -EIO : guestcaps_ret;
    hvdxg.pci_guestcaps_before_probe =
        hvdxg.probe_attempts == 0 && !hvdxg.d3dkmt_ready ? 1 : 0;
    hvdxg.pci_guestcaps_busdevfn = (bus << 16) | (dev << 8) | func;
    hvdxg.pci_guestcaps_token = hvdxg.pci_guestcaps_busdevfn;
    hvdxg_note_pci_vmbus_version(1);
}

static int hvdxg_hvpci_child_is_dxg(uint32 i)
{
    uint32 class_code;

    if (i >= hvpci.child_count || i >= HVPCI_CHILD_MAX)
        return 0;
    if (!hvpci.child[i].registered)
        return 0;
    if (hvpci.child[i].vendor_id != PCI_VENDOR_MICROSOFT)
        return 0;
    class_code = hvpci.child[i].class_code;
    if (hvpci.child[i].device_id == PCI_DEVICE_MS_VIRTUAL_RENDER ||
        hvpci.child[i].device_id == PCI_DEVICE_MS_COMPUTE_ACCELERATOR)
        return 1;
    return (class_code >> 16) == 0x03;
}

static int hvdxg_try_hvpci_guestcaps(void)
{
    uint32 readback = 0;
    uint32 class_rev = 0;
    uint32 guid0 = 0;
    uint32 guid1 = 0;
    uint32 guid2 = 0;
    uint32 guid3 = 0;
    uint32 vmbus_version = 0;
    uint32 luid0 = 0;
    uint32 luid1 = 0;
    uint32 token;
    uint32 bus;
    uint32 dev;
    uint32 func;
    struct hvdxg_winluid pci_luid;
    int luid_equiv;
    int ret;

    if (!hvpci.backend_registered || !hvpci.config_window_ok)
        return -ENODEV;

    for (uint32 i = 0; i < hvpci.child_count && i < HVPCI_CHILD_MAX; i++) {
        if (!hvdxg_hvpci_child_is_dxg(i))
            continue;

        token = hvpci.child[i].win_slot;
        bus = hvpci.child[i].bus;
        dev = hvpci.child[i].dev;
        func = hvpci.child[i].func;

        hvdxg.pci_guestcaps_attempts++;
        hvdxg.pci_guestcaps_found = 1;
        hvdxg.pci_guestcaps_scan_done = 1;
        hvdxg.pci_guestcaps_write_attempted = 1;
        hvdxg.pci_guestcaps_token = token;
        hvdxg.pci_guestcaps_offset = HV_DXG_PCI_GUESTCAPS_OFFSET;
        hvdxg.pci_guestcaps_value = HV_DXG_PCI_GUESTCAPS_WSL2;
        hvdxg.pci_guestcaps_before_probe =
            hvdxg.probe_attempts == 0 && !hvdxg.d3dkmt_ready ? 1 : 0;
        hvdxg.pci_guestcaps_busdevfn = (bus << 16) | (dev << 8) | func;

        ret = hvpci_config_write(&hvpci, token,
                                 HV_DXG_PCI_GUESTCAPS_OFFSET, 4,
                                 HV_DXG_PCI_GUESTCAPS_WSL2);
        if (ret == 0) {
            hvdxg.pci_guestcaps_writes++;
            ret = hvpci_config_read(&hvpci, token,
                                    HV_DXG_PCI_GUESTCAPS_OFFSET, 4,
                                    &readback);
        }
        hvdxg.pci_guestcaps_readback = readback;

        (void)hvpci_config_read(&hvpci, token, 0x08, 4, &class_rev);
        (void)hvpci_config_read(&hvpci, token, 192, 4, &guid0);
        (void)hvpci_config_read(&hvpci, token, 196, 4, &guid1);
        (void)hvpci_config_read(&hvpci, token, 200, 4, &guid2);
        (void)hvpci_config_read(&hvpci, token, 204, 4, &guid3);
        (void)hvpci_config_read(&hvpci, token, 208, 4, &vmbus_version);
        (void)hvpci_config_read(&hvpci, token, 212, 4, &luid0);
        (void)hvpci_config_read(&hvpci, token, 216, 4, &luid1);
        pci_luid.a = luid0;
        pci_luid.b = luid1;
        /*
         * On the Hyper-V vPCI path, offset 212 is write-only guest caps from
         * the guest's perspective but reads back as the host vGPU LUID low
         * dword. Treat the guestcaps write as accepted when the config write
         * succeeded and the PCI LUID is valid/equivalent; keep raw readback
         * visible for diagnostics.
         */
        luid_equiv = hvdxg_luid_nonzero(pci_luid) &&
            (!hvdxg_luid_nonzero(hvdxg.host_vgpu_luid) ||
             hvdxg_luid_equal(pci_luid, hvdxg.host_vgpu_luid));
        hvdxg.pci_guestcaps_write_verified =
            ret == 0 && luid_equiv ? 1 : 0;
        hvdxg.pci_guestcaps_ret =
            ret == 0 && !luid_equiv ? -EIO : ret;
        hvdxg.pci_domain = 0;
        hvdxg.pci_bus = bus;
        hvdxg.pci_dev = dev;
        hvdxg.pci_func = func;
        hvdxg.pci_vendor = hvpci.child[i].vendor_id;
        hvdxg.pci_dxg_device = hvpci.child[i].device_id;
        hvdxg.pci_class = class_rev != 0xffffffffU && class_rev != 0 ?
                          class_rev >> 8 : hvpci.child[i].class_code;
        hvdxg.pci_dxg_vmbus_version = vmbus_version;
        hvdxg.pci_dxg_guid[0] = guid0;
        hvdxg.pci_dxg_guid[1] = guid1;
        hvdxg.pci_dxg_guid[2] = guid2;
        hvdxg.pci_dxg_guid[3] = guid3;
        hvdxg.pci_host_vgpu_luid = pci_luid;
        if (hvdxg_luid_nonzero(pci_luid))
            hvdxg.host_vgpu_luid = hvdxg.pci_host_vgpu_luid;
        hvdxg_note_pci_vmbus_version(2);
        return hvdxg.pci_guestcaps_ret;
    }
    return -ENODEV;
}

static int hvdxg_try_pci_guestcaps_scan(void)
{
    int ret;

    if (hvdxg.pci_guestcaps_write_verified)
        return 0;
    if (hvdxg.pci_guestcaps_scan_done)
        return hvdxg.pci_guestcaps_ret;

    ret = hvdxg_try_hvpci_guestcaps();
    if (ret != -ENODEV)
        return ret;

    hvdxg.pci_guestcaps_scan_done = 1;
    for (uint32 bus = 0; bus < 256; bus++) {
        for (uint32 dev = 0; dev < 32; dev++) {
            for (uint32 func = 0; func < 8; func++) {
                uint32 id = pci_config_read32((uint8)bus, (uint8)dev,
                                              (uint8)func, 0);
                uint32 vendor = id & 0xffffU;
                uint32 device = (id >> 16) & 0xffffU;
                uint32 class_rev;
                uint32 class_code;
                uint32 guid0;
                uint32 guid1;
                uint32 guid2;
                uint32 guid3;
                uint32 vmbus_version;
                uint32 luid0;
                uint32 luid1;
                uint32 readback;
                uint16 command;
                int write_ret;

                if (vendor == 0xffffU || vendor == 0)
                    continue;
                if (vendor != PCI_VENDOR_MICROSOFT ||
                    (device != PCI_DEVICE_MS_VIRTUAL_RENDER &&
                     device != PCI_DEVICE_MS_COMPUTE_ACCELERATOR))
                    continue;

                class_rev = pci_config_read32((uint8)bus, (uint8)dev,
                                              (uint8)func, 0x08);
                class_code = class_rev >> 8;
                guid0 = pci_config_read32((uint8)bus, (uint8)dev,
                                          (uint8)func, 192);
                guid1 = pci_config_read32((uint8)bus, (uint8)dev,
                                          (uint8)func, 196);
                guid2 = pci_config_read32((uint8)bus, (uint8)dev,
                                          (uint8)func, 200);
                guid3 = pci_config_read32((uint8)bus, (uint8)dev,
                                          (uint8)func, 204);
                command = pci_config_read16((uint8)bus, (uint8)dev,
                                            (uint8)func, 0x04);
                command |= PCIE_CSCMD_MAE | PCIE_CSCMD_BME;
                pci_config_write16((uint8)bus, (uint8)dev, (uint8)func,
                                   0x04, command);
                hvdxg.pci_guestcaps_attempts++;
                hvdxg.pci_guestcaps_write_attempted = 1;
                hvdxg.pci_guestcaps_source = 1;
                pci_config_write32((uint8)bus, (uint8)dev, (uint8)func,
                                   HV_DXG_PCI_GUESTCAPS_OFFSET,
                                   HV_DXG_PCI_GUESTCAPS_WSL2);
                readback = pci_config_read32((uint8)bus, (uint8)dev,
                                             (uint8)func,
                                             HV_DXG_PCI_GUESTCAPS_OFFSET);
                write_ret = readback == HV_DXG_PCI_GUESTCAPS_WSL2 ?
                            0 : -EIO;
                vmbus_version = pci_config_read32((uint8)bus, (uint8)dev,
                                                  (uint8)func, 208);
                luid0 = pci_config_read32((uint8)bus, (uint8)dev,
                                          (uint8)func, 212);
                luid1 = pci_config_read32((uint8)bus, (uint8)dev,
                                          (uint8)func, 216);
                hyperv_dxg_note_pci(0, bus, dev, func, vendor, device,
                                    class_code, guid0, guid1, guid2, guid3,
                                    vmbus_version, luid0, luid1,
                                    HV_DXG_PCI_GUESTCAPS_OFFSET,
                                    HV_DXG_PCI_GUESTCAPS_WSL2, readback,
                                    write_ret);
                return write_ret;
            }
        }
    }

    return -ENODEV;
}

static void hvdxg_note_missing_pci_guestcaps_once(void)
{
    if (hvdxg.pci_guestcaps_attempts != 0)
        return;

    hvdxg.pci_guestcaps_attempts++;
    hvdxg.pci_guestcaps_scan_done = 1;
    hvdxg.pci_guestcaps_found = 0;
    hvdxg.pci_guestcaps_write_attempted = 0;
    hvdxg.pci_guestcaps_write_verified = 0;
    hvdxg.pci_guestcaps_offset = HV_DXG_PCI_GUESTCAPS_OFFSET;
    hvdxg.pci_guestcaps_value = HV_DXG_PCI_GUESTCAPS_WSL2;
    hvdxg.pci_guestcaps_readback = 0;
    hvdxg.pci_guestcaps_ret = -ENODEV;
    hvdxg.pci_guestcaps_before_probe =
        hvdxg.probe_attempts == 0 && !hvdxg.d3dkmt_ready ? 1 : 0;
    hvdxg.pci_guestcaps_busdevfn = 0;
    hvdxg.pci_guestcaps_source = 0;
    hvdxg.pci_guestcaps_token = 0;
    hvdxg.pci_dxg_vmbus_negotiated_version =
        HV_DXG_VMBUS_INTERFACE_VERSION_OLD;
    hvdxg_set_active_vmbus_version(HV_DXG_VMBUS_INTERFACE_VERSION_OLD, 0,
                                   HV_DXG_VMBUS_LAST_COMPATIBLE_INTERFACE_VERSION);
}

static const char *hvdxg_pci_guestcaps_source_name(void)
{
    switch (hvdxg.pci_guestcaps_source) {
    case 1:
        return "legacy-cf8";
    case 2:
        return "hyperv-vpci";
    default:
        return "none/legacy-scan";
    }
}

