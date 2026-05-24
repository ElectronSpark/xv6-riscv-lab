    transfer.resource_id = req.bo_handle;
    transfer.x = req.box.x;
    transfer.y = req.box.y;
    transfer.z = req.box.z;
    transfer.w = req.box.w;
    transfer.h = req.box.h;
    transfer.d = req.box.d ? req.box.d : 1;
    transfer.level = req.level;
    transfer.offset = req.offset;
    transfer.stride = req.stride;
    transfer.layer_stride = req.layer_stride;
    return virtio_gpu_user_transfer(owner->id, owner->tgid, &transfer,
                                    from_host);
}

static struct pci_device_info *gpu_nouveau_device(void)
{
    return gpu_nouveau_pci.probed ? gpu_nouveau_pci.pdev : NULL;
}

static int gpu_nouveau_require_device(void)
{
    return gpu_nouveau_device() != NULL ? 0 : -ENODEV;
}

static void gpu_nouveau_stat_inc(uint64 *counter)
{
    spin_lock(&fb_state.lock);
    (*counter)++;
    spin_unlock(&fb_state.lock);
}

static void gpu_nouveau_stat_dec(uint64 *counter)
{
    spin_lock(&fb_state.lock);
    if (*counter != 0)
        (*counter)--;
    spin_unlock(&fb_state.lock);
}

static void gpu_nouveau_stat_add(uint64 *counter, uint64 value)
{
    spin_lock(&fb_state.lock);
    *counter += value;
    spin_unlock(&fb_state.lock);
}

static void gpu_nouveau_stat_set(uint64 *counter, uint64 value)
{
    spin_lock(&fb_state.lock);
    *counter = value;
    spin_unlock(&fb_state.lock);
}

static uint64 gpu_nouveau_resource_len(struct pci_device_info *dev, int bar)
{
    if (dev == NULL)
        return 0;
    return pci_resource_len(dev, bar);
}

static uint64 gpu_nouveau_gart_aperture_len(struct pci_device_info *dev)
{
    uint64 bar0;

    if (dev == NULL)
        return 0;
    bar0 = gpu_nouveau_resource_len(dev, 0);
    if (bar0 != 0)
        return bar0;
    return gpu_nouveau_resource_len(dev, 1);
}

static void gpu_nouveau_pci_publish_core_state(struct pci_device_info *pdev)
{
    if (pdev == NULL)
        return;
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_enable_count,
                         pdev->enable_count);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_master_enabled,
                         pdev->master_enabled);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_irq_vectors,
                         pdev->irq_vector_count);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_runtime_suspended,
                         pdev->runtime_suspended);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_suspend_count,
                         pdev->suspend_count);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_resume_count,
                         pdev->resume_count);
    gpu_nouveau_stat_set(
        &fb_state.stats.nouveau_pci_runtime_pm_balanced,
        pdev->runtime_suspended == 0 &&
            pdev->suspend_count == pdev->resume_count);
    gpu_nouveau_stat_set(
        &fb_state.stats.nouveau_pci_dma_mask_configured,
        pdev->dma_mask_configured);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_dma_mask_bits,
                         pdev->dma_mask_bits);
    gpu_nouveau_stat_set(
        &fb_state.stats.nouveau_pci_coherent_dma_mask_configured,
        pdev->coherent_dma_mask_configured);
    gpu_nouveau_stat_set(
        &fb_state.stats.nouveau_pci_coherent_dma_mask_bits,
        pdev->coherent_dma_mask_bits);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_bar0_claimed,
                         pdev->resource_claimed[0]);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_bar1_claimed,
                         pdev->resource_claimed[1]);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_irq_mode,
                         pdev->irq_flags);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_irq_vector_valid,
                         pdev->irq_vectors_allocated &&
                             pdev->irq_vector_count > 0);
    /*
     * xv6 can discover and reserve a PCI IRQ vector here, but Nouveau has no
     * interrupt handler or delivery path yet.  Keep these zero until that
     * path exists so the diagnostics cannot imply Linux-equivalent IRQs.
     */
    gpu_nouveau_stat_set(
        &fb_state.stats.nouveau_pci_irq_handler_registered, 0);
    gpu_nouveau_stat_set(
        &fb_state.stats.nouveau_pci_irq_delivery_enabled, 0);
    gpu_nouveau_stat_set(
        &fb_state.stats.nouveau_pci_irq_delivery_claimed, 0);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_legacy_irq_fallback,
                         pdev->irq_flags == PCI_IRQ_LEGACY);
}

static int gpu_nouveau_pci_probe(struct pci_device_info *pdev,
                                 const struct pci_device_id *id)
{
    struct hyperv_dxg_status dxg;
    uint64 bar0_len;
    uint64 bar1_len;
    int ret;
    int irq;

    (void)id;
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_pci_probes);
    if (pdev == NULL)
        return -ENODEV;
    if ((pdev->class_code & 0xff0000U) != (PCI_CLASS_DISPLAY << 16)) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_pci_probe_failures);
        gpu_nouveau_stat_inc(
            &fb_state.stats.nouveau_pci_probe_reject_class);
        return -ENODEV;
    }
    bar0_len = gpu_nouveau_resource_len(pdev, 0);
    bar1_len = gpu_nouveau_resource_len(pdev, 1);
    if (hyperv_dxg_get_status(&dxg) == 0 &&
        (dxg.global_present || dxg.vgpu_present) &&
        bar0_len == 0 && bar1_len == 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_pci_probe_failures);
        gpu_nouveau_stat_inc(
            &fb_state.stats.nouveau_pci_probe_reject_dxg_present);
        return -ENODEV;
    }
    /*
     * DDA must appear as a real NVIDIA display function with at least one
     * usable BAR.  A GPU-P/DXG render partition may coexist with DDA on the
     * host, so DXG presence alone must not disable Nouveau once a physical
     * BAR-backed NVIDIA function is assigned to the VM.
     */
    if (bar0_len == 0 && bar1_len == 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_pci_probe_failures);
        gpu_nouveau_stat_inc(
            &fb_state.stats.nouveau_pci_probe_reject_no_bars);
        return -ENODEV;
    }

    ret = pci_enable_device(pdev);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_pci_probe_failures);
        gpu_nouveau_stat_inc(
            &fb_state.stats.nouveau_pci_probe_enable_failures);
        return ret;
    }

    ret = pci_set_dma_mask(pdev, ~0ULL);
    if (ret == 0)
        ret = pci_set_consistent_dma_mask(pdev, ~0ULL);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_pci_probe_failures);
        pci_disable_device(pdev);
        return ret;
    }

    if (bar0_len != 0) {
        ret = pci_request_region(pdev, 0, "nouveau");
        if (ret != 0) {
            gpu_nouveau_stat_inc(
                &fb_state.stats.nouveau_pci_bar_claim_failures);
            gpu_nouveau_stat_inc(
                &fb_state.stats.nouveau_pci_probe_failures);
            pci_disable_device(pdev);
            return ret;
        }
        gpu_nouveau_pci.bar0_claimed = 1;
    }
    if (bar1_len != 0) {
        ret = pci_request_region(pdev, 1, "nouveau");
        if (ret != 0) {
            gpu_nouveau_stat_inc(
                &fb_state.stats.nouveau_pci_bar_claim_failures);
            gpu_nouveau_stat_inc(
                &fb_state.stats.nouveau_pci_probe_failures);
            if (gpu_nouveau_pci.bar0_claimed) {
                pci_release_region(pdev, 0);
                gpu_nouveau_pci.bar0_claimed = 0;
                gpu_nouveau_stat_inc(
                    &fb_state.stats.nouveau_pci_bar_releases);
            }
            pci_disable_device(pdev);
            return ret;
        }
        gpu_nouveau_pci.bar1_claimed = 1;
    }
    pci_set_master(pdev);

    gpu_nouveau_pci.bar0_len = bar0_len;
    gpu_nouveau_pci.bar1_len = bar1_len;
    gpu_nouveau_pci.bar0 = pci_iomap(pdev, 0, 0);
    gpu_nouveau_pci.bar1 = pci_iomap(pdev, 1, 0);
    gpu_nouveau_pci.irq_pin = pdev->irq_pin;
    gpu_nouveau_pci.msi_cap = pdev->msi_cap;
    gpu_nouveau_pci.msix_cap = pdev->msix_cap;
    if (pdev->msi_cap != 0 || pdev->msix_cap != 0) {
        uint32 msi_flags = 0;

        if (pdev->msi_cap != 0)
            msi_flags |= PCI_IRQ_MSI;
        if (pdev->msix_cap != 0)
            msi_flags |= PCI_IRQ_MSIX;
        gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_msi_requested,
                             msi_flags);
        irq = pci_alloc_irq_vectors(pdev, 1, 1, msi_flags);
        if (irq == -ENOTSUP)
            gpu_nouveau_stat_inc(
                &fb_state.stats.nouveau_pci_msi_fail_closed);
    }
    irq = pci_alloc_irq_vectors(pdev, 0, 1, PCI_IRQ_LEGACY);
    if (irq < 0)
        gpu_nouveau_stat_inc(
            &fb_state.stats.nouveau_pci_irq_request_failures);
    gpu_nouveau_pci.irq_vector = irq > 0 ? pci_irq_vector(pdev, 0) : 0;
    gpu_nouveau_pci.pdev = pdev;
    gpu_nouveau_pci.probed = 1;
    pci_set_drvdata(pdev, &gpu_nouveau_pci);
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_pci_probe_accepts);

    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_bar0_len,
                         gpu_nouveau_pci.bar0_len);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_bar1_len,
                         gpu_nouveau_pci.bar1_len);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_irq,
                         gpu_nouveau_pci.irq_vector);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_irq_pin,
                         gpu_nouveau_pci.irq_pin);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_msi_cap,
                         gpu_nouveau_pci.msi_cap);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_msix_cap,
                         gpu_nouveau_pci.msix_cap);
    gpu_nouveau_pci_publish_core_state(pdev);
    gpu_nouveau_stat_set(
        &fb_state.stats.nouveau_pci_native_present_credit, 0);
    printf("Nouveau: PCI probe accepted %02x:%02x.%u device=0x%x class=0x%lx bar0=0x%lx len=0x%lx claimed=%d bar1=0x%lx len=0x%lx claimed=%d irq=%d pin=%u irqmode=0x%x irq_handler=0 irq_delivery=0 dma=%u/%u coherent=%u/%u msi=0x%x msix=0x%x\n",
           pdev->bus, pdev->dev, pdev->func, pdev->device_id,
           (uint64)pdev->class_code, pci_resource_start(pdev, 0),
           gpu_nouveau_pci.bar0_len, gpu_nouveau_pci.bar0_claimed,
           pci_resource_start(pdev, 1), gpu_nouveau_pci.bar1_len,
           gpu_nouveau_pci.bar1_claimed, gpu_nouveau_pci.irq_vector,
           gpu_nouveau_pci.irq_pin, pdev->irq_flags,
           pdev->dma_mask_configured, pdev->dma_mask_bits,
           pdev->coherent_dma_mask_configured,
           pdev->coherent_dma_mask_bits, gpu_nouveau_pci.msi_cap,
           gpu_nouveau_pci.msix_cap);
    return 0;
}

static void gpu_nouveau_pci_remove(struct pci_device_info *pdev)
{
    int registered;

    if (pdev == NULL || pci_get_drvdata(pdev) != &gpu_nouveau_pci)
        return;
    if (pdev->runtime_suspended)
        gpu_nouveau_stat_inc(
            &fb_state.stats.nouveau_pci_remove_runtime_suspended);
    pci_iounmap(pdev, gpu_nouveau_pci.bar0);
    pci_iounmap(pdev, gpu_nouveau_pci.bar1);
    pci_free_irq_vectors(pdev);
    pci_clear_master(pdev);
    pci_disable_device(pdev);
    if (gpu_nouveau_pci.bar0_claimed) {
        pci_release_region(pdev, 0);
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_pci_bar_releases);
    }
    if (gpu_nouveau_pci.bar1_claimed) {
        pci_release_region(pdev, 1);
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_pci_bar_releases);
    }
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_pci_removes);
    gpu_nouveau_pci_publish_core_state(pdev);
    registered = gpu_nouveau_pci.registered;
    memset(&gpu_nouveau_pci, 0, sizeof(gpu_nouveau_pci));
    gpu_nouveau_pci.registered = registered;
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_bar0_len, 0);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_bar1_len, 0);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_irq, 0);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_irq_pin, 0);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_msi_cap, 0);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_msix_cap, 0);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_bar0_claimed, 0);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_bar1_claimed, 0);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_irq_mode, 0);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_irq_vector_valid, 0);
    gpu_nouveau_stat_set(
        &fb_state.stats.nouveau_pci_irq_handler_registered, 0);
    gpu_nouveau_stat_set(
        &fb_state.stats.nouveau_pci_irq_delivery_enabled, 0);
    gpu_nouveau_stat_set(
        &fb_state.stats.nouveau_pci_irq_delivery_claimed, 0);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_pci_legacy_irq_fallback, 0);
    gpu_nouveau_stat_set(
        &fb_state.stats.nouveau_pci_native_present_credit, 0);
}

static int gpu_nouveau_pci_suspend(struct pci_device_info *pdev)
{
    if (pdev == NULL || pci_get_drvdata(pdev) != &gpu_nouveau_pci)
        return -ENODEV;
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_pci_suspends);
    gpu_nouveau_pci_publish_core_state(pdev);
    return 0;
}

static int gpu_nouveau_pci_resume(struct pci_device_info *pdev)
{
    if (pdev == NULL || pci_get_drvdata(pdev) != &gpu_nouveau_pci)
        return -ENODEV;
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_pci_resumes);
    gpu_nouveau_pci_publish_core_state(pdev);
    return 0;
}

static void gpu_nouveau_register_pci_driver(void)
{
    int ret;

    if (gpu_nouveau_pci.registered)
        return;
    ret = pci_register_driver(&gpu_nouveau_pci_driver);
    if (ret == 0) {
        gpu_nouveau_pci.registered = 1;
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_pci_registered);
    } else {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_pci_probe_failures);
        printf("Nouveau: PCI driver registration failed ret=%d\n", ret);
    }
}

static int gpu_nouveau_getparam(uint64 arg)
{
    struct drm_nouveau_getparam_compat req;
    struct pci_device_info *dev = gpu_nouveau_device();
    uint64 source = FB_GPU_NOUVEAU_GETPARAM_SOURCE_NONE;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (dev == NULL) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_getparam_fail_closed);
        gpu_nouveau_stat_set(&fb_state.stats.nouveau_getparam_last_source,
                             FB_GPU_NOUVEAU_GETPARAM_SOURCE_NONE);
        return -ENODEV;
    }

    switch (req.param) {
    case NOUVEAU_GETPARAM_PCI_VENDOR:
        req.value = dev->vendor_id;
        source = FB_GPU_NOUVEAU_GETPARAM_SOURCE_DDA_PCI;
        break;
    case NOUVEAU_GETPARAM_PCI_DEVICE:
        req.value = dev->device_id;
        source = FB_GPU_NOUVEAU_GETPARAM_SOURCE_DDA_PCI;
        break;
    case NOUVEAU_GETPARAM_BUS_TYPE:
        req.value = 2;
        source = FB_GPU_NOUVEAU_GETPARAM_SOURCE_DDA_PCI;
        break;
    case NOUVEAU_GETPARAM_FB_SIZE:
    case NOUVEAU_GETPARAM_VRAM_BAR_SIZE:
        req.value = gpu_nouveau_resource_len(dev, 1);
        source = FB_GPU_NOUVEAU_GETPARAM_SOURCE_DDA_PCI;
        break;
    case NOUVEAU_GETPARAM_AGP_SIZE:
        req.value = gpu_nouveau_gart_aperture_len(dev);
        source = FB_GPU_NOUVEAU_GETPARAM_SOURCE_DDA_PCI;
        break;
    case NOUVEAU_GETPARAM_CHIPSET_ID:
        req.value = dev->device_id & 0xff;
        source = FB_GPU_NOUVEAU_GETPARAM_SOURCE_DDA_PCI;
        break;
    case NOUVEAU_GETPARAM_VM_VRAM_BASE:
        req.value = pci_resource_start(dev, 1);
        source = FB_GPU_NOUVEAU_GETPARAM_SOURCE_DDA_PCI;
        break;
    case NOUVEAU_GETPARAM_GRAPH_UNITS:
    case NOUVEAU_GETPARAM_PTIMER_TIME:
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_getparam_fail_closed);
        gpu_nouveau_stat_set(&fb_state.stats.nouveau_getparam_last_source,
                             FB_GPU_NOUVEAU_GETPARAM_SOURCE_NONE);
        return -ENOTSUP;
    case NOUVEAU_GETPARAM_HAS_BO_USAGE:
        req.value = 1;
        source = FB_GPU_NOUVEAU_GETPARAM_SOURCE_DRIVER_CAP;
        break;
    case NOUVEAU_GETPARAM_HAS_PAGEFLIP:
        req.value = 0;
        source = FB_GPU_NOUVEAU_GETPARAM_SOURCE_DRIVER_CAP;
        break;
    case NOUVEAU_GETPARAM_EXEC_PUSH_MAX:
        req.value = NOUVEAU_GEM_MAX_PUSH;
        source = FB_GPU_NOUVEAU_GETPARAM_SOURCE_DRIVER_CAP;
        break;
    case NOUVEAU_GETPARAM_VRAM_USED:
        spin_lock(&fb_state.lock);
        req.value = fb_state.stats.ttm_vram_bytes;
        spin_unlock(&fb_state.lock);
        source = FB_GPU_NOUVEAU_GETPARAM_SOURCE_DRIVER_CAP;
        break;
    case NOUVEAU_GETPARAM_HAS_VMA_TILEMODE:
        req.value = 0;
        source = FB_GPU_NOUVEAU_GETPARAM_SOURCE_DRIVER_CAP;
        break;
    default:
        return -EINVAL;
    }

    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_getparams);
    gpu_nouveau_stat_set(&fb_state.stats.nouveau_getparam_last_source,
                         source);
    if (source == FB_GPU_NOUVEAU_GETPARAM_SOURCE_DDA_PCI)
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_getparam_dda_facts);
    else if (source == FB_GPU_NOUVEAU_GETPARAM_SOURCE_DRIVER_CAP)
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_getparam_driver_caps);
    else
        gpu_nouveau_stat_inc(
            &fb_state.stats.nouveau_getparam_synthetic_facts);
    return 0;
}

static int gpu_nouveau_channel_alloc(struct fb_gpu_render_owner *owner,
                                     uint64 arg)
{
    struct drm_nouveau_channel_alloc_compat req;
    int ret = gpu_nouveau_require_device();

    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return ret;
    }
    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (owner->nouveau_channel != 0)
        return -EBUSY;
    if (req.nr_subchan > 8)
        return -EINVAL;

    memset(owner->nouveau_objects, 0, sizeof(owner->nouveau_objects));
    owner->nouveau_channel = 1;
    owner->nouveau_channel_handle = 0;
    owner->nouveau_pushbuf_domains =
        NOUVEAU_GEM_DOMAIN_GART |
        NOUVEAU_GEM_DOMAIN_MAPPABLE |
        NOUVEAU_GEM_DOMAIN_COHERENT;
    owner->nouveau_next_notifier_offset = PGSIZE;
    req.channel = 0;
    req.pushbuf_domains = owner->nouveau_pushbuf_domains;
    req.notifier_handle = 0;
    for (uint32 i = 0; i < req.nr_subchan; i++) {
        if (req.subchan[i].handle == 0 || req.subchan[i].grclass == 0)
            return -EINVAL;
    }
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_channel_allocs);
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_channel_active);
    return 0;
}

static int gpu_nouveau_channel_valid(struct fb_gpu_render_owner *owner,
                                     int32 channel)
{
    return owner != NULL && owner->nouveau_channel != 0 &&
           channel == (int32)owner->nouveau_channel_handle;
}

static int gpu_nouveau_class_supported(uint32 class_id)
{
    switch (class_id) {
    case 0x006e: /* NV10_CHANNEL_DMA / software class remap */
    case 0x016e:
    case 0x506e:
    case 0x906e:
    case 0x902d: /* FERMI_TWOD_A */
    case 0x9039: /* FERMI_MEMORY_TO_MEMORY_FORMAT_A */
    case 0xa040: /* KEPLER_INLINE_TO_MEMORY_A */
    case 0xa140: /* KEPLER_INLINE_TO_MEMORY_B */
        return 1;
    default:
        return 0;
    }
}

static int gpu_nouveau_object_find(struct fb_gpu_render_owner *owner,
                                   uint32 handle)
{
    for (uint32 i = 0; i < NELEM(owner->nouveau_objects); i++) {
        if (owner->nouveau_objects[i].handle == handle)
            return (int)i;
    }
    return -1;
}

static int gpu_nouveau_object_alloc(struct fb_gpu_render_owner *owner,
                                    uint32 handle, uint32 class_id,
                                    uint32 kind, uint32 size,
                                    uint32 *offset_out)
{
    if (handle == 0 || gpu_nouveau_object_find(owner, handle) >= 0)
        return -EINVAL;
    for (uint32 i = 0; i < NELEM(owner->nouveau_objects); i++) {
        if (owner->nouveau_objects[i].handle == 0) {
            owner->nouveau_objects[i].handle = handle;
            owner->nouveau_objects[i].class_id = class_id;
            owner->nouveau_objects[i].kind = kind;
            owner->nouveau_objects[i].size = size;
            if (kind == 1) {
                owner->nouveau_objects[i].offset =
                    owner->nouveau_next_notifier_offset;
                owner->nouveau_next_notifier_offset +=
                    FB_GPU_ALIGN_UP(size ? size : 1, PGSIZE);
            }
            if (offset_out != NULL)
                *offset_out = owner->nouveau_objects[i].offset;
            return 0;
        }
    }
    return -ENOSPC;
}

static int gpu_nouveau_object_free(struct fb_gpu_render_owner *owner,
                                   uint32 handle, uint32 *kind_out)
{
    int idx = gpu_nouveau_object_find(owner, handle);

    if (idx < 0)
        return -ENOENT;
    if (kind_out != NULL)
        *kind_out = owner->nouveau_objects[idx].kind;
    memset(&owner->nouveau_objects[idx], 0, sizeof(owner->nouveau_objects[idx]));
    return 0;
}

static uint32 gpu_nouveau_object_count(struct fb_gpu_render_owner *owner)
{
    uint32 count = 0;

    if (owner == NULL)
        return 0;
    for (uint32 i = 0; i < NELEM(owner->nouveau_objects); i++) {
        if (owner->nouveau_objects[i].handle != 0)
            count++;
    }
    return count;
}

static int gpu_nouveau_channel_free(struct fb_gpu_render_owner *owner,
                                    uint64 arg)
{
    struct drm_nouveau_channel_free_compat req;
    int ret = gpu_nouveau_require_device();

    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return ret;
    }
    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (!gpu_nouveau_channel_valid(owner, req.channel))
        return -EINVAL;
    gpu_nouveau_stat_add(&fb_state.stats.nouveau_close_object_reclaims,
                         gpu_nouveau_object_count(owner));
    memset(owner->nouveau_objects, 0, sizeof(owner->nouveau_objects));
    owner->nouveau_channel = 0;
    owner->nouveau_channel_handle = 0;
    owner->nouveau_pushbuf_domains = 0;
    owner->nouveau_next_notifier_offset = 0;
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_channel_frees);
    gpu_nouveau_stat_dec(&fb_state.stats.nouveau_channel_active);
    return 0;
}

static int gpu_nouveau_grobj_alloc(struct fb_gpu_render_owner *owner,
                                   uint64 arg)
{
    struct drm_nouveau_grobj_alloc_compat req;
    int ret = gpu_nouveau_require_device();

    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return ret;
    }
    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (!gpu_nouveau_channel_valid(owner, req.channel) ||
        !gpu_nouveau_class_supported((uint32)req.class)) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_object_rejects);
        return -EINVAL;
    }
    ret = gpu_nouveau_object_alloc(owner, req.handle, (uint32)req.class,
                                   2, 0, NULL);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_object_rejects);
        return ret;
    }
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_grobj_allocs);
    return 0;
}

static int gpu_nouveau_notifier_alloc(struct fb_gpu_render_owner *owner,
                                      uint64 arg)
{
    struct drm_nouveau_notifierobj_alloc_compat req;
    uint32 offset = 0;
    int ret = gpu_nouveau_require_device();

    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return ret;
    }
    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (!gpu_nouveau_channel_valid(owner, (int32)req.channel) ||
        req.size == 0 || req.size > 1024 * 1024) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_object_rejects);
        return -EINVAL;
    }
    ret = gpu_nouveau_object_alloc(owner, req.handle, 0, 1, req.size,
                                   &offset);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_object_rejects);
        return ret;
    }
    req.offset = offset;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0) {
        uint32 kind;

        (void)gpu_nouveau_object_free(owner, req.handle, &kind);
        return -EFAULT;
    }
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_notifier_allocs);
    return 0;
}

static int gpu_nouveau_gpuobj_free(struct fb_gpu_render_owner *owner,
                                   uint64 arg)
{
    struct drm_nouveau_gpuobj_free_compat req;
    uint32 kind = 0;
    int ret = gpu_nouveau_require_device();

    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return ret;
    }
    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (!gpu_nouveau_channel_valid(owner, req.channel))
        return -EINVAL;
    ret = gpu_nouveau_object_free(owner, req.handle, &kind);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_object_rejects);
        return ret;
    }
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_gpuobj_frees);
    return 0;
}

static uint32 gpu_nouveau_destroy_owner(struct fb_gpu_render_owner *owner)
{
    uint32 objects = gpu_nouveau_object_count(owner);

    if (owner == NULL)
        return 0;
    if (objects != 0)
        gpu_nouveau_stat_add(&fb_state.stats.nouveau_close_object_reclaims,
                             objects);
    if (owner->nouveau_channel != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_channel_frees);
        gpu_nouveau_stat_dec(&fb_state.stats.nouveau_channel_active);
    }
    memset(owner->nouveau_objects, 0, sizeof(owner->nouveau_objects));
    owner->nouveau_channel = 0;
    owner->nouveau_channel_handle = 0;
    owner->nouveau_pushbuf_domains = 0;
    owner->nouveau_next_notifier_offset = 0;
    owner->nouveau_vm_initialized = 0;
    return objects;
}

static int gpu_nouveau_gem_new(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_nouveau_gem_new_compat req;
    uint64 size;
    uint32 npages;
    uint32 handle;
    uint32 requested_domain;
    uint32 actual_domain;
    uint32 placement;
    page_t **pages;
    int ret = gpu_nouveau_require_device();

    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return ret;
    }
    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.info.handle != 0 || req.info.size == 0 ||
        req.info.size > 64ULL * 1024ULL * 1024ULL ||
        (req.info.domain & ~NOUVEAU_GEM_VALID_DOMAINS) != 0)
        return -EINVAL;
    if (req.align != 0 && (req.align & (req.align - 1)) != 0)
        return -EINVAL;

    size = FB_GPU_ALIGN_UP(req.info.size, PGSIZE);
    npages = size / PGSIZE;
    ret = fb_bo_alloc_pages(npages, &pages);
    if (ret != 0)
        return ret;
    ret = fb_bo_register(owner->id, owner->tgid, (uint32)(size / 4), 1,
                         (uint32)size, size, pages, npages, &handle);
    if (ret != 0) {
        fb_bo_release_pages(pages, npages);
        return ret;
    }

    req.info.handle = handle;
    if (req.info.domain == 0)
        req.info.domain = NOUVEAU_GEM_DOMAIN_GART |
                          NOUVEAU_GEM_DOMAIN_MAPPABLE |
                          NOUVEAU_GEM_DOMAIN_COHERENT;
    requested_domain = req.info.domain;
    if ((req.info.domain & NOUVEAU_GEM_DOMAIN_VRAM) != 0) {
        placement = FB_TTM_PL_VRAM;
        actual_domain = (requested_domain &
                         ~(NOUVEAU_GEM_DOMAIN_GART |
                           NOUVEAU_GEM_DOMAIN_CPU)) |
                        NOUVEAU_GEM_DOMAIN_VRAM;
    } else if ((req.info.domain & NOUVEAU_GEM_DOMAIN_GART) != 0) {
        placement = FB_TTM_PL_TT;
        actual_domain = (requested_domain &
                         ~(NOUVEAU_GEM_DOMAIN_VRAM |
                           NOUVEAU_GEM_DOMAIN_CPU)) |
                        NOUVEAU_GEM_DOMAIN_GART;
    } else {
        placement = FB_TTM_PL_SYSTEM;
        actual_domain = (requested_domain &
                         ~(NOUVEAU_GEM_DOMAIN_VRAM |
                           NOUVEAU_GEM_DOMAIN_GART)) |
                        NOUVEAU_GEM_DOMAIN_CPU;
    }
    ret = fb_bo_set_ttm_placement(handle, owner->id, owner->tgid,
                                  placement);
    if (ret != 0) {
        (void)fb_bo_destroy_owned(handle, owner->id, owner->tgid);
        return ret;
    }
    ret = fb_bo_set_nouveau_metadata(handle, owner->id, owner->tgid,
                                     actual_domain, req.info.tile_mode,
                                     req.info.tile_flags);
    if (ret != 0) {
        (void)fb_bo_destroy_owned(handle, owner->id, owner->tgid);
        return ret;
    }
    req.info.domain = actual_domain;
    req.info.size = size;
    req.info.offset = 0;
    req.info.map_handle = GPU_DRM_MMAP_OFFSET(handle);
    if (either_copyout(1, arg, &req, sizeof(req)) < 0) {
        (void)fb_bo_destroy_owned(handle, owner->id, owner->tgid);
        return -EFAULT;
    }
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_gem_news);
    return 0;
}

static int gpu_nouveau_gem_info(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_nouveau_gem_info_compat req;
    struct fb_gpu_bo_entry *bo;
    int ret = gpu_nouveau_require_device();

    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return ret;
    }
    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    bo = fb_bo_get_owned(req.handle, owner->id, owner->tgid);
    if (bo == NULL)
        return -ENOENT;
    req.domain = bo->nouveau_domain;
    req.size = bo->size;
    req.offset = 0;
    req.map_handle = GPU_DRM_MMAP_OFFSET(req.handle);
    req.tile_mode = bo->nouveau_tile_mode;
    req.tile_flags = bo->nouveau_tile_flags;
    fb_bo_put(bo);
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_gem_infos);
    return 0;
}

static int gpu_nouveau_cpu_prep(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_nouveau_gem_cpu_prep_compat req;
    struct fb_gpu_bo_entry *bo;
    int ret = gpu_nouveau_require_device();

    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return ret;
    }
    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if ((req.flags & ~(NOUVEAU_GEM_CPU_PREP_NOWAIT |
                       NOUVEAU_GEM_CPU_PREP_WRITE)) != 0)
        return -EINVAL;
    bo = fb_bo_get_owned(req.handle, owner->id, owner->tgid);
    if (bo == NULL)
        return -ENOENT;
    fb_bo_put(bo);
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_cpu_preps);
    return 0;
}

static int gpu_nouveau_cpu_fini(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_nouveau_gem_cpu_fini_compat req;
    struct fb_gpu_bo_entry *bo;
    int ret = gpu_nouveau_require_device();

    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return ret;
    }
    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    bo = fb_bo_get_owned(req.handle, owner->id, owner->tgid);
    if (bo == NULL)
        return -ENOENT;
    fb_bo_put(bo);
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_cpu_finis);
    return 0;
}

static int gpu_nouveau_vm_init(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_nouveau_vm_init_compat req;
    int ret = gpu_nouveau_require_device();

    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return ret;
    }
    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (owner->nouveau_channel != 0)
        return -ENOSYS;
    owner->nouveau_vm_initialized = 1;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_vm_inits);
    return 0;
}

static uint32 gpu_nouveau_bo_domain(const struct fb_gpu_bo_entry *bo)
{
    if (bo == NULL)
        return 0;
    if (bo->nouveau_domain != 0)
        return bo->nouveau_domain;
    if (bo->ttm_mem_type == FB_TTM_MEM_VRAM)
        return NOUVEAU_GEM_DOMAIN_VRAM |
               NOUVEAU_GEM_DOMAIN_MAPPABLE |
               NOUVEAU_GEM_DOMAIN_COHERENT;
    if (bo->ttm_mem_type == FB_TTM_MEM_TT)
        return NOUVEAU_GEM_DOMAIN_GART |
               NOUVEAU_GEM_DOMAIN_MAPPABLE |
               NOUVEAU_GEM_DOMAIN_COHERENT;
    return NOUVEAU_GEM_DOMAIN_CPU |
           NOUVEAU_GEM_DOMAIN_MAPPABLE |
           NOUVEAU_GEM_DOMAIN_COHERENT;
}

static int gpu_nouveau_validate_pushbuf_buffers(
    struct fb_gpu_render_owner *owner, struct drm_nouveau_gem_pushbuf_compat *req,
    struct drm_nouveau_gem_pushbuf_bo_compat **buffers_out)
{
    struct drm_nouveau_gem_pushbuf_bo_compat *buffers = NULL;
    uint64 bytes;

    *buffers_out = NULL;
    if (req->nr_buffers == 0)
        return 0;
    if (req->buffers == 0)
        return -EINVAL;
    bytes = (uint64)req->nr_buffers * sizeof(buffers[0]);
    if (bytes / sizeof(buffers[0]) != req->nr_buffers)
        return -EOVERFLOW;
    buffers = kvmalloc(bytes);
    if (buffers == NULL)
        return -ENOMEM;
    if (either_copyin(buffers, 1, req->buffers, bytes) < 0) {
        kvfree(buffers);
        return -EFAULT;
    }

    for (uint32 i = 0; i < req->nr_buffers; i++) {
        struct drm_nouveau_gem_pushbuf_bo_compat *pb = &buffers[i];
        struct fb_gpu_bo_entry *bo;
        uint32 domain;

        if (pb->handle == 0 ||
            (pb->read_domains & ~NOUVEAU_GEM_VALID_DOMAINS) != 0 ||
            (pb->write_domains & ~NOUVEAU_GEM_VALID_DOMAINS) != 0 ||
            (pb->valid_domains & ~NOUVEAU_GEM_VALID_DOMAINS) != 0) {
            kvfree(buffers);
            return -EINVAL;
        }
        bo = fb_bo_get_owned(pb->handle, owner->id, owner->tgid);
        if (bo == NULL) {
            kvfree(buffers);
            return -ENOENT;
        }
        domain = gpu_nouveau_bo_domain(bo);
        if (pb->valid_domains != 0 &&
            (domain & pb->valid_domains &
             (NOUVEAU_GEM_DOMAIN_VRAM | NOUVEAU_GEM_DOMAIN_GART |
              NOUVEAU_GEM_DOMAIN_CPU)) == 0) {
            fb_bo_put(bo);
            kvfree(buffers);
            return -EINVAL;
        }
        pb->presumed.valid = 1;
        pb->presumed.domain = domain;
        pb->presumed.offset = GPU_DRM_MMAP_OFFSET(pb->handle);
        fb_bo_put(bo);
    }

    if (either_copyout(1, req->buffers, buffers, bytes) < 0) {
        kvfree(buffers);
        return -EFAULT;
    }
    *buffers_out = buffers;
    return 0;
}

static int gpu_nouveau_validate_relocs(
    const struct drm_nouveau_gem_pushbuf_compat *req)
{
    struct drm_nouveau_gem_pushbuf_reloc_compat *relocs;
    uint64 bytes;
    int ret = 0;

    if (req->nr_relocs == 0)
        return 0;
    if (req->relocs == 0)
        return -EINVAL;
    bytes = (uint64)req->nr_relocs * sizeof(relocs[0]);
    if (bytes / sizeof(relocs[0]) != req->nr_relocs)
        return -EOVERFLOW;
    relocs = kvmalloc(bytes);
    if (relocs == NULL)
        return -ENOMEM;
    if (either_copyin(relocs, 1, req->relocs, bytes) < 0) {
        kvfree(relocs);
        return -EFAULT;
    }
    for (uint32 i = 0; i < req->nr_relocs; i++) {
        if (relocs[i].reloc_bo_index >= req->nr_buffers ||
            relocs[i].bo_index >= req->nr_buffers ||
            (relocs[i].flags & ~(NOUVEAU_GEM_RELOC_LOW |
                                 NOUVEAU_GEM_RELOC_HIGH |
                                 NOUVEAU_GEM_RELOC_OR)) != 0) {
            ret = -EINVAL;
            break;
        }
    }
    kvfree(relocs);
    return ret;
}

static int gpu_nouveau_validate_pushes(
    const struct drm_nouveau_gem_pushbuf_compat *req, int *has_commands)
{
    struct drm_nouveau_gem_pushbuf_push_compat *pushes;
    uint64 bytes;
    int ret = 0;

    *has_commands = 0;
    if (req->nr_push == 0)
        return 0;
    if (req->push == 0)
        return -EINVAL;
    bytes = (uint64)req->nr_push * sizeof(pushes[0]);
    if (bytes / sizeof(pushes[0]) != req->nr_push)
        return -EOVERFLOW;
    pushes = kvmalloc(bytes);
    if (pushes == NULL)
        return -ENOMEM;
    if (either_copyin(pushes, 1, req->push, bytes) < 0) {
        kvfree(pushes);
        return -EFAULT;
    }
    for (uint32 i = 0; i < req->nr_push; i++) {
        if (pushes[i].bo_index >= req->nr_buffers ||
            pushes[i].pad != 0) {
            ret = -EINVAL;
            break;
        }
        if (pushes[i].length != 0)
            *has_commands = 1;
    }
    kvfree(pushes);
    return ret;
}

static int gpu_nouveau_pushbuf(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_nouveau_gem_pushbuf_compat req;
    struct drm_nouveau_gem_pushbuf_bo_compat *buffers = NULL;
    int has_commands = 0;
    int ret = gpu_nouveau_require_device();

    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return ret;
    }
    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.nr_buffers > NOUVEAU_GEM_MAX_BUFFERS ||
        req.nr_relocs > NOUVEAU_GEM_MAX_RELOCS ||
        req.nr_push > NOUVEAU_GEM_MAX_PUSH)
        return -EINVAL;
    if (req.channel != 0 || owner->nouveau_channel == 0)
        return -EINVAL;
    ret = gpu_nouveau_validate_pushbuf_buffers(owner, &req, &buffers);
    if (ret != 0)
        return ret;
    ret = gpu_nouveau_validate_relocs(&req);
    if (ret == 0)
        ret = gpu_nouveau_validate_pushes(&req, &has_commands);
    if (ret != 0) {
        kvfree(buffers);
        return ret;
    }
    if (req.nr_relocs == 0 && !has_commands) {
        req.vram_available = 0;
        req.gart_available = 256ULL * 1024ULL * 1024ULL;
        if (either_copyout(1, arg, &req, sizeof(req)) < 0) {
            kvfree(buffers);
            return -EFAULT;
        }
        kvfree(buffers);
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_pushbuf_noops);
        return 0;
    }
    kvfree(buffers);
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_unsupported);
    return -EOPNOTSUPP;
}

static int gpu_nouveau_sync_array(struct fb_gpu_render_owner *owner,
                                  uint32 count, uint64 ptr, int signal)
{
    if (count == 0)
        return 0;
    if (owner == NULL)
        return -EBADF;
    if (ptr == 0 || count > 64)
        return -EINVAL;

    for (uint32 i = 0; i < count; i++) {
        struct drm_nouveau_sync_compat sync;
        struct fb_gpu_syncobj_entry *obj;
        struct fb_gpu_syncobj_state_entry *state;
        uint32 type;
        uint64 point;

        if (either_copyin(&sync, 1,
                          ptr + (uint64)i * sizeof(sync),
                          sizeof(sync)) < 0)
            return -EFAULT;
        type = sync.flags & DRM_NOUVEAU_SYNC_TYPE_MASK;
        if (sync.handle == 0 ||
            (sync.flags & ~DRM_NOUVEAU_SYNC_TYPE_MASK) != 0 ||
            (type != DRM_NOUVEAU_SYNC_SYNCOBJ &&
             type != DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ))
            return -EINVAL;
        point = type == DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ ?
                sync.timeline_value : 1;
        if (point == 0)
            point = 1;

        spin_lock(&fb_state.lock);
        obj = gpu_syncobj_lookup_locked(sync.handle, owner);
        state = obj != NULL ? gpu_syncobj_state_locked(obj->state_index) :
            NULL;
        if (obj == NULL || state == NULL) {
            spin_unlock(&fb_state.lock);
            return -ENOENT;
        }
        if (signal) {
            gpu_syncobj_signal_state_locked(state, point);
            gpu_syncobj_attach_owner_resv_locked(
                owner, FB_GPU_RESV_ATTACH_SYNCOBJ_SIGNAL);
        } else {
            if (!state->signaled || state->timeline_value < point) {
                fb_state.stats.syncobj_stale_wait_rejects++;
                spin_unlock(&fb_state.lock);
                return -EBUSY;
            }
            fb_state.stats.syncobj_waits++;
            gpu_syncobj_attach_owner_resv_locked(
                owner, FB_GPU_RESV_ATTACH_SYNCOBJ_WAIT);
        }
        spin_unlock(&fb_state.lock);
    }
    return 0;
}

static int gpu_nouveau_complete_fence_only_submit(
    struct fb_gpu_render_owner *owner, uint32 wait_count, uint64 wait_ptr,
    uint32 sig_count, uint64 sig_ptr)
{
    int ret;

    ret = gpu_nouveau_sync_array(owner, wait_count, wait_ptr, 0);
    if (ret != 0)
        return ret;
    return gpu_nouveau_sync_array(owner, sig_count, sig_ptr, 1);
}

static int gpu_nouveau_validate_vm_bind_ops(
    struct fb_gpu_render_owner *owner,
    const struct drm_nouveau_vm_bind_compat *req)
{
    struct drm_nouveau_vm_bind_op_compat *ops;
    uint64 bytes;
    int ret = 0;

    if (req->op_count == 0)
        return 0;
    if (owner == NULL)
        return -EBADF;
    if (req->op_ptr == 0 || req->op_count > 64)
        return -EINVAL;
    bytes = (uint64)req->op_count * sizeof(ops[0]);
    if (bytes / sizeof(ops[0]) != req->op_count)
        return -EOVERFLOW;
    ops = kvmalloc(bytes);
    if (ops == NULL)
        return -ENOMEM;
    if (either_copyin(ops, 1, req->op_ptr, bytes) < 0) {
        kvfree(ops);
        return -EFAULT;
    }

    for (uint32 i = 0; i < req->op_count; i++) {
        struct drm_nouveau_vm_bind_op_compat *op = &ops[i];
        struct fb_gpu_bo_entry *bo = NULL;

        if ((op->op != DRM_NOUVEAU_VM_BIND_OP_MAP &&
             op->op != DRM_NOUVEAU_VM_BIND_OP_UNMAP) ||
            (op->flags & ~DRM_NOUVEAU_VM_BIND_SPARSE) != 0 ||
            op->pad != 0 || op->range == 0 ||
            (op->addr & (PGSIZE - 1)) != 0 ||
            (op->bo_offset & (PGSIZE - 1)) != 0) {
            ret = -EINVAL;
            break;
        }
        if (op->op == DRM_NOUVEAU_VM_BIND_OP_MAP &&
            (op->flags & DRM_NOUVEAU_VM_BIND_SPARSE) == 0 &&
            op->handle == 0) {
            ret = -EINVAL;
            break;
        }
        if (op->handle != 0) {
            bo = fb_bo_get_owned(op->handle, owner->id, owner->tgid);
            if (bo == NULL) {
                ret = -ENOENT;
                break;
            }
            if (op->bo_offset >= bo->size ||
                op->range > bo->size - op->bo_offset)
                ret = -EINVAL;
            fb_bo_put(bo);
            if (ret != 0)
                break;
        }
    }
    kvfree(ops);
    return ret;
}

static int gpu_nouveau_validate_exec_pushes(
    const struct drm_nouveau_exec_compat *req)
{
    struct drm_nouveau_exec_push_compat *pushes;
    uint64 bytes;
    int ret = 0;

    if (req->push_count == 0)
        return 0;
    if (req->push_ptr == 0 || req->push_count > NOUVEAU_GEM_MAX_PUSH)
        return -EINVAL;
    bytes = (uint64)req->push_count * sizeof(pushes[0]);
    if (bytes / sizeof(pushes[0]) != req->push_count)
        return -EOVERFLOW;
    pushes = kvmalloc(bytes);
    if (pushes == NULL)
        return -ENOMEM;
    if (either_copyin(pushes, 1, req->push_ptr, bytes) < 0) {
        kvfree(pushes);
        return -EFAULT;
    }
    for (uint32 i = 0; i < req->push_count; i++) {
        if (pushes[i].va == 0 || pushes[i].va_len == 0 ||
            (pushes[i].flags & ~DRM_NOUVEAU_EXEC_PUSH_NO_PREFETCH) != 0) {
            ret = -EINVAL;
            break;
        }
    }
    kvfree(pushes);
    return ret;
}

static int gpu_nouveau_bind_or_exec(struct fb_gpu_render_owner *owner,
                                    uint64 arg, int exec)
{
    struct drm_nouveau_vm_bind_compat bind_req;
    struct drm_nouveau_exec_compat exec_req;
    int ret = gpu_nouveau_require_device();

    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return ret;
    }
    if (owner == NULL)
        return -EBADF;
    if (exec) {
        if (either_copyin(&exec_req, 1, arg, sizeof(exec_req)) < 0)
            return -EFAULT;
        if (exec_req.channel != 0 || owner->nouveau_channel == 0)
            return -EINVAL;
        if (exec_req.push_count == 0) {
            ret = gpu_nouveau_complete_fence_only_submit(
                owner, exec_req.wait_count, exec_req.wait_ptr,
                exec_req.sig_count, exec_req.sig_ptr);
            if (ret != 0)
                return ret;
            gpu_nouveau_stat_inc(&fb_state.stats.nouveau_exec_noops);
            return 0;
        }
        ret = gpu_nouveau_validate_exec_pushes(&exec_req);
        if (ret != 0)
            return ret;
    } else {
        if (either_copyin(&bind_req, 1, arg, sizeof(bind_req)) < 0)
            return -EFAULT;
        if (!owner->nouveau_vm_initialized)
            return -EINVAL;
        if ((bind_req.flags & ~DRM_NOUVEAU_VM_BIND_RUN_ASYNC) != 0)
            return -EINVAL;
        if (bind_req.op_count == 0) {
            if ((bind_req.wait_count != 0 || bind_req.sig_count != 0) &&
                (bind_req.flags & DRM_NOUVEAU_VM_BIND_RUN_ASYNC) == 0)
                return -EINVAL;
            ret = gpu_nouveau_complete_fence_only_submit(
                owner, bind_req.wait_count, bind_req.wait_ptr,
                bind_req.sig_count, bind_req.sig_ptr);
            if (ret != 0)
                return ret;
            gpu_nouveau_stat_inc(&fb_state.stats.nouveau_vm_bind_noops);
            return 0;
        }
        ret = gpu_nouveau_validate_vm_bind_ops(owner, &bind_req);
        if (ret != 0)
            return ret;
    }
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_unsupported);
    return -EOPNOTSUPP;
}
