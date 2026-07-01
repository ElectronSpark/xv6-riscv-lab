enum { FB_GPU_DESTROY_BATCH = 64 };

void fb_gpu_destroy_owner(pid_t owner_tgid)
{
    uint32 handles[FB_GPU_DESTROY_BATCH];

    if (owner_tgid <= 0)
        return;

    for (;;) {
        int n = 0;

        spin_lock(&fb_state.lock);
        for (int i = 0; i < FB_GPU_MAX_BOS && n < FB_GPU_DESTROY_BATCH; i++) {
            struct fb_gpu_bo_entry *bo = &fb_state.bos[i];

            if (!bo->in_use || bo->owner_tgid != owner_tgid)
                continue;
            handles[n++] = bo->handle;
        }
        spin_unlock(&fb_state.lock);

        if (n == 0)
            break;
        for (int i = 0; i < n; i++)
            (void)fb_bo_destroy(handles[i]);
    }
}

void fb_gpu_destroy_render_owner(uint64 owner_id)
{
    if (owner_id == 0)
        return;

    for (;;) {
        struct fb_gpu_bo_entry *bos[FB_GPU_DESTROY_BATCH];
        int borrowed[FB_GPU_DESTROY_BATCH];
        int n = 0;

        spin_lock(&fb_state.lock);
        struct fb_gpu_render_owner *owner =
            fb_gpu_render_owner_lookup_locked(owner_id, 0);
        if (owner != NULL) {
            for (int i = 0; i < FB_GPU_MAX_BOS && n < FB_GPU_DESTROY_BATCH; i++) {
                if (!owner->bo_handles[i].in_use)
                    continue;
                bos[n] = owner->bo_handles[i].bo;
                borrowed[n] = owner->bo_handles[i].borrowed;
                n++;
                memset(&owner->bo_handles[i], 0,
                       sizeof(owner->bo_handles[i]));
            }
        } else {
            for (int i = 0; i < FB_GPU_MAX_BOS && n < FB_GPU_DESTROY_BATCH; i++) {
                struct fb_gpu_bo_entry *bo = &fb_state.bos[i];

                if (!bo->in_use || bo->owner_id != owner_id)
                    continue;
                bos[n] = bo;
                borrowed[n] = 0;
                n++;
            }
        }
        spin_unlock(&fb_state.lock);

        if (n == 0)
            break;
        for (int i = 0; i < n; i++) {
            if (bos[i] == NULL)
                continue;
            if (borrowed[i]) {
                fb_bo_put(bos[i]);
                continue;
            }
            spin_lock(&fb_state.lock);
            if (bos[i]->in_use) {
                bos[i]->in_use = 0;
                bos[i]->dead = 1;
                fb_ttm_resv_release_owner_if_last_locked(bos[i]);
                if (bos[i]->stats_accounted)
                    fb_ttm_account_locked(bos[i]->ttm_mem_type,
                                          bos[i]->size, 0);
                if (bos[i]->stats_accounted && bos[i]->ttm_pin_count != 0) {
                    if (fb_state.stats.ttm_pinned_bytes >= bos[i]->size)
                        fb_state.stats.ttm_pinned_bytes -= bos[i]->size;
                    else
                        fb_state.stats.ttm_pinned_bytes = 0;
                    bos[i]->ttm_pin_count = 0;
                }
                if (fb_state.stats.bo_handles > 0)
                    fb_state.stats.bo_handles--;
                if (bos[i]->stats_accounted &&
                    fb_state.stats.bo_live_bytes >= bos[i]->size)
                    fb_state.stats.bo_live_bytes -= bos[i]->size;
                else if (bos[i]->stats_accounted)
                    fb_state.stats.bo_live_bytes = 0;
                if (bos[i]->dmabuf_attachment_accounted &&
                    fb_state.stats.dmabuf_live_attachments > 0)
                    fb_state.stats.dmabuf_live_attachments--;
            }
            spin_unlock(&fb_state.lock);
            fb_bo_put(bos[i]);
        }
    }
}

static uint64 fb_gpu_count_render_owner_bos(uint64 owner_id)
{
    uint64 count = 0;

    if (owner_id == 0)
        return 0;

    spin_lock(&fb_state.lock);
    struct fb_gpu_render_owner *owner =
        fb_gpu_render_owner_lookup_locked(owner_id, 0);
    if (owner != NULL) {
        for (int i = 0; i < FB_GPU_MAX_BOS; i++) {
            if (owner->bo_handles[i].in_use)
                count++;
        }
    } else {
        for (int i = 0; i < FB_GPU_MAX_BOS; i++) {
            struct fb_gpu_bo_entry *bo = &fb_state.bos[i];

            if (bo->in_use && bo->owner_id == owner_id)
                count++;
        }
    }
    spin_unlock(&fb_state.lock);
    return count;
}

static int fb_bo_destroy(uint32 handle)
{
    struct fb_gpu_bo_entry *bo;

    spin_lock(&fb_state.lock);
    bo = fb_bo_lookup_locked(handle);
    if (bo == NULL) {
        spin_unlock(&fb_state.lock);
        return -ENOENT;
    }
    bo->in_use = 0;
    bo->dead = 1;
    fb_ttm_resv_release_owner_if_last_locked(bo);
    if (bo->stats_accounted)
        fb_ttm_account_locked(bo->ttm_mem_type, bo->size, 0);
    if (bo->stats_accounted && bo->ttm_pin_count != 0) {
        if (fb_state.stats.ttm_pinned_bytes >= bo->size)
            fb_state.stats.ttm_pinned_bytes -= bo->size;
        else
            fb_state.stats.ttm_pinned_bytes = 0;
        bo->ttm_pin_count = 0;
    }
    if (fb_state.stats.bo_handles > 0)
        fb_state.stats.bo_handles--;
    if (bo->stats_accounted && fb_state.stats.bo_live_bytes >= bo->size)
        fb_state.stats.bo_live_bytes -= bo->size;
    else if (bo->stats_accounted)
        fb_state.stats.bo_live_bytes = 0;
    if (bo->dmabuf_attachment_accounted &&
        fb_state.stats.dmabuf_live_attachments > 0)
        fb_state.stats.dmabuf_live_attachments--;
    spin_unlock(&fb_state.lock);

    fb_bo_put(bo);
    return 0;
}

static int fb_bo_destroy_owned(uint32 handle, uint64 owner_id,
                               pid_t owner_tgid)
{
    struct fb_gpu_bo_entry *bo;
    struct fb_gpu_render_owner *owner;
    int borrowed = 0;
    int removed;

    spin_lock(&fb_state.lock);
    owner = fb_gpu_render_owner_lookup_locked(owner_id, owner_tgid);
    removed = fb_bo_owner_remove_handle_locked(owner, handle, &bo, &borrowed);
    if (removed == 0) {
        if (borrowed) {
            spin_unlock(&fb_state.lock);
            fb_bo_put(bo);
            return 0;
        }
        if (bo != NULL && bo->in_use) {
            bo->in_use = 0;
            bo->dead = 1;
            fb_ttm_resv_release_owner_if_last_locked(bo);
            if (bo->stats_accounted)
                fb_ttm_account_locked(bo->ttm_mem_type, bo->size, 0);
            if (bo->stats_accounted && bo->ttm_pin_count != 0) {
                if (fb_state.stats.ttm_pinned_bytes >= bo->size)
                    fb_state.stats.ttm_pinned_bytes -= bo->size;
                else
                    fb_state.stats.ttm_pinned_bytes = 0;
                bo->ttm_pin_count = 0;
            }
            if (fb_state.stats.bo_handles > 0)
                fb_state.stats.bo_handles--;
            if (bo->stats_accounted &&
                fb_state.stats.bo_live_bytes >= bo->size)
                fb_state.stats.bo_live_bytes -= bo->size;
            else if (bo->stats_accounted)
                fb_state.stats.bo_live_bytes = 0;
            if (bo->dmabuf_attachment_accounted &&
                fb_state.stats.dmabuf_live_attachments > 0)
                fb_state.stats.dmabuf_live_attachments--;
        }
        spin_unlock(&fb_state.lock);
        fb_bo_put(bo);
        return 0;
    }
    if (owner != NULL) {
        spin_unlock(&fb_state.lock);
        return -ENOENT;
    }
    bo = fb_bo_lookup_locked(handle);
    if (bo == NULL) {
        spin_unlock(&fb_state.lock);
        return -ENOENT;
    }
    if (!fb_bo_owner_matches(bo, owner_id, owner_tgid)) {
        spin_unlock(&fb_state.lock);
        return -EPERM;
    }
    spin_unlock(&fb_state.lock);
    return fb_bo_destroy(handle);
}

/* ── BGA mode setting ─────────────────────────────────────────────── */

static void bga_set_mode(uint32 width, uint32 height, uint32 bpp)
{
    bga_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bga_write_reg(VBE_DISPI_INDEX_XRES, (uint16)width);
    bga_write_reg(VBE_DISPI_INDEX_YRES, (uint16)height);
    bga_write_reg(VBE_DISPI_INDEX_BPP,  (uint16)bpp);
    bga_write_reg(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16)width);
    bga_write_reg(VBE_DISPI_INDEX_VIRT_HEIGHT, (uint16)height);
    bga_write_reg(VBE_DISPI_INDEX_X_OFFSET, 0);
    bga_write_reg(VBE_DISPI_INDEX_Y_OFFSET, 0);
    bga_write_reg(VBE_DISPI_INDEX_ENABLE,
                  VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    /*
     * Ensure VGA Attribute Controller has PAS (Palette Address Source)
     * bit set.  QEMU's vga_draw_graphic() checks ar_index & 0x20 and
     * blanks the display if PAS is clear.  BGA mode setting does NOT
     * touch the legacy VGA registers, so PAS may be left clear after
     * SeaBIOS → kernel handoff.
     */
    fb_inb(0x3DA);          /* Reset AR flip-flop */
    fb_outb(0x3C0, 0x20);   /* Set PAS bit, index 0 */

    /* Misc Output Register: enable RAM access, color I/O base (0x3D?) */
    fb_outb(0x3C2, 0x63);

    /* Sequencer register 01h, bit 5 = "Screen Off" — ensure it's clear */
    fb_outb(0x3C4, 0x01);
    uint8 seq1 = fb_inb(0x3C5);
    fb_outb(0x3C5, seq1 & ~0x20);

    fb_state.xres  = width;
    fb_state.yres  = height;
    fb_state.bpp   = bpp;
    fb_state.pitch = width * (bpp / 8);
    fb_state.fb_size = fb_state.pitch * height;
}

/* ── PCI discovery (called from pci_init) ─────────────────────────── */

void fb_pci_init(uint8 bus, uint8 dev, uint8 func)
{
    /* Guard against double-init on multi-core PCI scan */
    spin_lock(&fb_state.lock);
    if (fb_state.detected) {
        spin_unlock(&fb_state.lock);
        return;
    }
    spin_unlock(&fb_state.lock);

    printf("PCI: Bochs VGA detected at %d:%d:%d\n", bus, dev, func);

    /* Enable memory access */
    uint16 cmd = pci_config_read16(bus, dev, func, 0x04);
    cmd |= PCIE_CSCMD_MAE;
    pci_config_write16(bus, dev, func, 0x04, cmd);

    /* Read BAR0 — linear framebuffer base (memory BAR, mask low 4 bits) */
    uint32 bar0 = pci_config_read32(bus, dev, func, 0x10) & ~0xFU;

    fb_state.fb_phys = (uint64)bar0;
    fb_state.fb_virt = (volatile uint8 *)PA2VA(fb_state.fb_phys);
    fb_state.firmware_backed = 0;
    fb_state.scanout_mappable = fb_state.fb_phys != 0;

    printf("FB: BAR0 = 0x%lx (VA = 0x%lx)\n",
           fb_state.fb_phys, (uint64)fb_state.fb_virt);

    /* Verify BGA is present by reading the ID register */
    uint16 id = bga_read_reg(VBE_DISPI_INDEX_ID);
    printf("FB: BGA ID = 0x%x\n", id);

    /* Set video mode — check cmdline "video=WxH" first, else defaults */
    uint32 width = FB_DEFAULT_WIDTH;
    uint32 height = FB_DEFAULT_HEIGHT;
    {
        char vbuf[32];
        if (cmdline_get_param("video", vbuf, sizeof(vbuf)) == 0) {
            /* Parse "WIDTHxHEIGHT" */
            uint32 w = 0, h = 0;
            const char *p = vbuf;
            while (*p >= '0' && *p <= '9')
                w = w * 10 + (*p++ - '0');
            if (*p == 'x' || *p == 'X') {
                p++;
                while (*p >= '0' && *p <= '9')
                    h = h * 10 + (*p++ - '0');
            }
            if (w >= 640 && w <= 2560 && h >= 480 && h <= 1600) {
                width = w;
                height = h;
                printf("FB: using cmdline resolution %dx%d\n", w, h);
            } else {
                printf("FB: ignoring invalid video=%s, using default\n", vbuf);
            }
        }
    }
    bga_set_mode(width, height, FB_DEFAULT_BPP);

    /* Read back BGA registers to verify mode was actually set */
    {
        uint16 en  = bga_read_reg(VBE_DISPI_INDEX_ENABLE);
        uint16 rxr = bga_read_reg(VBE_DISPI_INDEX_XRES);
        uint16 ryr = bga_read_reg(VBE_DISPI_INDEX_YRES);
        uint16 rbp = bga_read_reg(VBE_DISPI_INDEX_BPP);
        printf("FB: BGA readback: enable=0x%x xres=%d yres=%d bpp=%d\n",
               en, rxr, ryr, rbp);
    }

    printf("FB: mode set to %dx%dx%d (pitch=%d, size=%d)\n",
           fb_state.xres, fb_state.yres, fb_state.bpp,
           fb_state.pitch, fb_state.fb_size);

    if (fb_cmdline_enabled("fbtest")) {
        /* Write a bright test pattern to verify LFB writes reach the VGA.
         * Top 16 scanlines: bright blue. Rest: black. */
        volatile uint32 *pixels = (volatile uint32 *)fb_state.fb_virt;
        uint32 npx = fb_state.xres * fb_state.yres;
        /* Clear to black first */
        for (uint32 i = 0; i < npx; i++)
            pixels[i] = 0x00000000;
        /* Bright blue bar at top */
        uint32 bar_rows = 16;
        for (uint32 y = 0; y < bar_rows && y < fb_state.yres; y++) {
            for (uint32 x = 0; x < fb_state.xres; x++) {
                pixels[y * fb_state.xres + x] = 0x000080FF; /* bright blue (BGRX) */
            }
        }
        /* Also write a bright white cross at center for visibility */
        uint32 cx = fb_state.xres / 2;
        uint32 cy = fb_state.yres / 2;
        for (uint32 i = 0; i < 40; i++) {
            if (cx - 20 + i < fb_state.xres)
                pixels[cy * fb_state.xres + cx - 20 + i] = 0x00FFFFFF;
            if (cy - 20 + i < fb_state.yres)
                pixels[(cy - 20 + i) * fb_state.xres + cx] = 0x00FFFFFF;
        }
        /* Read back a pixel to verify the write landed */
        uint32 readback = pixels[0];
        printf("FB: LFB test pattern written, readback pixel[0]=0x%x (expect 0x000080FF)\n",
               readback);
    } else {
        fb_paint_boot_logo();
    }

    /* Publish as detected — must be last so readers see consistent state */
    __sync_synchronize();
    spin_lock(&fb_state.lock);
    fb_state.detected = 1;
    spin_unlock(&fb_state.lock);
}

int fb_detected(void)
{
    return fb_state.detected;
}

void fb_get_resolution(uint32 *xres, uint32 *yres)
{
    spin_lock(&fb_state.lock);
    if (xres)
        *xres = fb_state.xres;
    if (yres)
        *yres = fb_state.yres;
    spin_unlock(&fb_state.lock);
}

/* ── GPU acceleration primitives ──────────────────────────────────── */

/*
 * gpu_fill_rect — fill a rectangle in the LFB with a solid color.
 * Writes directly to MMIO framebuffer memory.  Caller must hold fb_state.lock.
 */
static void gpu_fill_rect_locked(uint32 x, uint32 y, uint32 w, uint32 h,
                                 uint32 color)
{
    volatile uint32 *fb = (volatile uint32 *)fb_state.fb_virt;
    uint32 stride = fb_state.xres;  /* pixels per row */

    for (uint32 row = y; row < y + h; row++) {
        volatile uint32 *dst = fb + row * stride + x;
        for (uint32 col = 0; col < w; col++)
            dst[col] = color;
    }
}

/*
 * gpu_copy_rect — screen-to-screen rectangle copy.
 * Handles overlapping regions with correct copy direction.
 * Caller must hold fb_state.lock.
 */
static void gpu_copy_rect_locked(uint32 sx, uint32 sy,
                                 uint32 dx, uint32 dy,
                                 uint32 w, uint32 h)
{
    volatile uint32 *fb = (volatile uint32 *)fb_state.fb_virt;
    uint32 stride = fb_state.xres;

    if (dy < sy || (dy == sy && dx < sx)) {
        /* Copy top-to-bottom, left-to-right */
        for (uint32 row = 0; row < h; row++) {
            volatile uint32 *src = fb + (sy + row) * stride + sx;
            volatile uint32 *dst = fb + (dy + row) * stride + dx;
            for (uint32 col = 0; col < w; col++)
                dst[col] = src[col];
        }
    } else {
        /* Copy bottom-to-top, right-to-left (overlapping case) */
        for (uint32 row = h; row > 0; row--) {
            volatile uint32 *src = fb + (sy + row - 1) * stride + sx;
            volatile uint32 *dst = fb + (dy + row - 1) * stride + dx;
            for (uint32 col = w; col > 0; col--)
                dst[col - 1] = src[col - 1];
        }
    }
}

/* ── Character device operations ──────────────────────────────────── */

static int fb_open(cdev_t *cdev) { return 0; }
static int fb_release(cdev_t *cdev)
{
    (void)cdev;
    return 0;
}

static void gpu_drm_lifecycle_open(enum drm_core_node_type type)
{
    spin_lock(&fb_state.lock);
    switch (type) {
    case DRM_CORE_NODE_PRIMARY:
        fb_state.stats.drm_file_primary_opens++;
        fb_state.stats.drm_file_primary_live++;
        break;
    case DRM_CORE_NODE_RENDER:
        fb_state.stats.drm_file_render_opens++;
        fb_state.stats.drm_file_render_live++;
        break;
    default:
        fb_state.stats.drm_file_legacy_opens++;
        fb_state.stats.drm_file_legacy_live++;
        break;
    }
    spin_unlock(&fb_state.lock);
}

static void gpu_drm_lifecycle_close(enum drm_core_node_type type,
                                    uint64 stale_gem_handles,
                                    uint64 stale_kms_fbs,
                                    uint64 stale_syncobjs,
                                    uint64 stale_events)
{
    spin_lock(&fb_state.lock);
    switch (type) {
    case DRM_CORE_NODE_PRIMARY:
        fb_state.stats.drm_file_primary_closes++;
        break;
    case DRM_CORE_NODE_RENDER:
        fb_state.stats.drm_file_render_closes++;
        break;
    default:
        fb_state.stats.drm_file_legacy_closes++;
        break;
    }
    fb_state.stats.drm_file_close_generation++;
    fb_state.stats.drm_file_stale_gem_handles += stale_gem_handles;
    fb_state.stats.drm_file_stale_kms_fbs += stale_kms_fbs;
    fb_state.stats.drm_file_stale_syncobjs += stale_syncobjs;
    fb_state.stats.drm_file_stale_events += stale_events;
    spin_unlock(&fb_state.lock);
}

static void gpu_drm_lifecycle_live_open(struct fb_gpu_render_owner *owner)
{
    if (owner == NULL || owner->lifecycle_live_accounted)
        return;

    spin_lock(&fb_state.lock);
    if (!owner->lifecycle_live_accounted) {
        switch (owner->drm.node_type) {
        case DRM_CORE_NODE_PRIMARY:
            fb_state.stats.drm_file_primary_live++;
            break;
        case DRM_CORE_NODE_RENDER:
            fb_state.stats.drm_file_render_live++;
            break;
        default:
            fb_state.stats.drm_file_legacy_live++;
            break;
        }
        owner->lifecycle_live_accounted = 1;
    }
    spin_unlock(&fb_state.lock);
}

static void gpu_drm_lifecycle_live_close(struct fb_gpu_render_owner *owner)
{
    if (owner == NULL || !owner->lifecycle_live_accounted)
        return;

    spin_lock(&fb_state.lock);
    if (owner->lifecycle_live_accounted) {
        switch (owner->drm.node_type) {
        case DRM_CORE_NODE_PRIMARY:
            if (fb_state.stats.drm_file_primary_live > 0)
                fb_state.stats.drm_file_primary_live--;
            break;
        case DRM_CORE_NODE_RENDER:
            if (fb_state.stats.drm_file_render_live > 0)
                fb_state.stats.drm_file_render_live--;
            break;
        default:
            if (fb_state.stats.drm_file_legacy_live > 0)
                fb_state.stats.drm_file_legacy_live--;
            break;
        }
        owner->lifecycle_live_accounted = 0;
    }
    spin_unlock(&fb_state.lock);
}

static int gpu_open(cdev_t *cdev)
{
    enum drm_core_node_type type = gpu_drm_node_from_cdev(cdev);

    spin_lock(&fb_state.lock);
    fb_state.stats.gpu_opens++;
    fb_state.stats.gpu_live_opens++;
    if (type == DRM_CORE_NODE_PRIMARY) {
        fb_state.stats.drm_primary_opens++;
        fb_state.stats.drm_primary_live++;
    } else if (type == DRM_CORE_NODE_RENDER) {
        fb_state.stats.drm_render_opens++;
        fb_state.stats.drm_render_live++;
    }
    spin_unlock(&fb_state.lock);
    return 0;
}

static int gpu_release_node(enum drm_core_node_type type)
{
    spin_lock(&fb_state.lock);
    if (fb_state.stats.gpu_live_opens > 0)
        fb_state.stats.gpu_live_opens--;
    if (type == DRM_CORE_NODE_PRIMARY &&
        fb_state.stats.drm_primary_live > 0) {
        fb_state.stats.drm_primary_live--;
    } else if (type == DRM_CORE_NODE_RENDER &&
               fb_state.stats.drm_render_live > 0) {
        fb_state.stats.drm_render_live--;
    }
    spin_unlock(&fb_state.lock);
    return 0;
}

static int gpu_release(cdev_t *cdev)
{
    return gpu_release_node(gpu_drm_node_from_cdev(cdev));
}

static int fb_write(cdev_t *cdev, bool user, const void *buf, size_t count)
{
    (void)cdev;

    if (!fb_state.detected)
        return -ENODEV;
    if (buf == NULL)
        return -EINVAL;
    if (count == 0)
        return 0;

    /* Clamp to framebuffer size (write always starts at offset 0) */
    if (count > fb_state.fb_size)
        count = fb_state.fb_size;

    if (user) {
        /*
         * Copy from userspace in chunks.  copyin can sleep so we must
         * NOT hold the spinlock across it.  Instead: copyin → lock →
         * memcpy to MMIO → unlock.
         */
        uint8 kbuf[4096];
        size_t done = 0;
        while (done < count) {
            size_t chunk = count - done;
            if (chunk > sizeof(kbuf))
                chunk = sizeof(kbuf);
            if (either_copyin(kbuf, 1, (uint64)buf + done, chunk) < 0)
                return done ? (int)done : -EFAULT;
            spin_lock(&fb_state.lock);
            memcpy((void *)(fb_state.fb_virt + done), kbuf, chunk);
            spin_unlock(&fb_state.lock);
            done += chunk;
        }
        virtio_gpu_present_fb_rect(fb_state.fb_virt, fb_state.pitch,
                                   0, 0, fb_state.xres, fb_state.yres);
        hyperv_video_dirty(0, 0, fb_state.xres, fb_state.yres);
        return done;
    } else {
        spin_lock(&fb_state.lock);
        memcpy((void *)fb_state.fb_virt, buf, count);
        spin_unlock(&fb_state.lock);
        virtio_gpu_present_fb_rect(fb_state.fb_virt, fb_state.pitch,
                                   0, 0, fb_state.xres, fb_state.yres);
        hyperv_video_dirty(0, 0, fb_state.xres, fb_state.yres);
        return count;
    }
}

static int fb_read(cdev_t *cdev, bool user, void *buf, size_t count)
{
    (void)cdev;

    if (!fb_state.detected)
        return -ENODEV;
    if (buf == NULL)
        return -EINVAL;
    if (count == 0)
        return 0;

    if (count > fb_state.fb_size)
        count = fb_state.fb_size;

    if (user) {
        /*
         * Lock → memcpy from MMIO → unlock → copyout.
         * copyout may sleep, so it must be outside the lock.
         */
        uint8 kbuf[4096];
        size_t done = 0;
        while (done < count) {
            size_t chunk = count - done;
            if (chunk > sizeof(kbuf))
                chunk = sizeof(kbuf);
            spin_lock(&fb_state.lock);
            memcpy(kbuf, (void *)(fb_state.fb_virt + done), chunk);
            spin_unlock(&fb_state.lock);
            if (either_copyout(1, (uint64)buf + done, kbuf, chunk) < 0)
                return done ? (int)done : -EFAULT;
            done += chunk;
        }
        return done;
    } else {
        spin_lock(&fb_state.lock);
        memcpy(buf, (void *)fb_state.fb_virt, count);
        spin_unlock(&fb_state.lock);
        return count;
    }
}

static int fb_read_current_framebuffer(uint32 x, uint32 y, uint32 w, uint32 h,
                                       void *dst, uint32 dst_pitch,
                                       uint32 *screen_width,
                                       uint32 *screen_height,
                                       uint32 *screen_pitch)
{
    uint32 xres;
    uint32 yres;
    uint32 pitch;
    volatile uint8 *src;
    uint8 *out = (uint8 *)dst;

    if (dst == NULL || w == 0 || h == 0 || dst_pitch < w * sizeof(uint32))
        return -EINVAL;

    spin_lock(&fb_state.lock);
    if (!fb_state.detected || fb_state.fb_virt == NULL ||
        fb_state.bpp != 32) {
        spin_unlock(&fb_state.lock);
        return -ENODEV;
    }
    xres = fb_state.xres;
    yres = fb_state.yres;
    pitch = fb_state.pitch;
    if (pitch < xres * sizeof(uint32) || x > xres || w > xres - x ||
        y > yres || h > yres - y) {
        spin_unlock(&fb_state.lock);
        return -EINVAL;
    }
    src = fb_state.fb_virt + (uint64)y * pitch + (uint64)x * sizeof(uint32);
    for (uint32 row = 0; row < h; row++)
        memcpy(out + (uint64)row * dst_pitch,
               (const void *)(src + (uint64)row * pitch),
               (uint64)w * sizeof(uint32));
    spin_unlock(&fb_state.lock);

    if (screen_width != NULL)
        *screen_width = xres;
    if (screen_height != NULL)
        *screen_height = yres;
    if (screen_pitch != NULL)
        *screen_pitch = pitch;
    return 0;
}

static int fb_ioctl_for_owner(cdev_t *cdev, uint64 cmd, void *arg,
                              uint64 owner_id, pid_t owner_tgid)
{
    (void)cdev;
    if (owner_tgid == 0 && current != NULL)
        owner_tgid = current->tgid;

    if (!fb_state.detected)
        return -ENODEV;

    switch (cmd) {
    case FBIOGET_VSCREENINFO: {
        struct fb_var_screeninfo info;
        memset(&info, 0, sizeof(info));
        spin_lock(&fb_state.lock);
        info.xres = fb_state.xres;
        info.yres = fb_state.yres;
        info.xres_virtual = fb_state.xres;
        info.yres_virtual = fb_state.yres;
        info.bits_per_pixel = fb_state.bpp;
        if (fb_state.firmware_backed && platform.has_framebuffer) {
            info.red.offset = platform.framebuffer_red_pos;
            info.green.offset = platform.framebuffer_green_pos;
            info.blue.offset = platform.framebuffer_blue_pos;
        } else {
            info.red.offset = 16;
            info.green.offset = 8;
            info.blue.offset = 0;
        }
        info.red.length = 8;
        info.green.length = 8;
        info.blue.length = 8;
        if (fb_state.bpp == 32) {
            info.transp.offset = 24;
            info.transp.length = 8;
        }
        info.activate = FB_ACTIVATE_NOW;
        info.vmode = FB_VMODE_NONINTERLACED;
        spin_unlock(&fb_state.lock);
        if (either_copyout(1, (uint64)arg, (char *)&info, sizeof(info)) < 0)
            return -EFAULT;
        return 0;
    }
    case FBIOGET_FSCREENINFO: {
        struct fb_fix_screeninfo info;
        memset(&info, 0, sizeof(info));
        spin_lock(&fb_state.lock);
        strncpy(info.id, fb_state.virtio_backed ? "VirtioGPU" :
                (fb_state.firmware_backed ? "FirmwareFB" : "BochsVGA"),
                sizeof(info.id) - 1);
        info.smem_start = fb_state.fb_phys;
        info.smem_len = fb_state.fb_size;
        info.type = FB_TYPE_PACKED_PIXELS;
        info.visual = FB_VISUAL_TRUECOLOR;
        info.line_length = fb_state.pitch;
        info.accel = FB_ACCEL_NONE;
        spin_unlock(&fb_state.lock);
        if (either_copyout(1, (uint64)arg, (char *)&info, sizeof(info)) < 0)
            return -EFAULT;
        return 0;
    }

    case FB_GPU_FILL_RECT: {
        struct fb_gpu_fill cmd;
        int clipped = 0;
        if (either_copyin((char *)&cmd, 1, (uint64)arg, sizeof(cmd)) < 0)
            return -EFAULT;
        if (cmd.w == 0 || cmd.h == 0)
            return 0;

        spin_lock(&fb_state.lock);
        /* Clip to screen bounds */
        if (cmd.x >= fb_state.xres || cmd.y >= fb_state.yres) {
            spin_unlock(&fb_state.lock);
            return 0;
        }
        if ((uint64)cmd.x + cmd.w > fb_state.xres) {
            cmd.w = fb_state.xres - cmd.x;
            clipped = 1;
        }
        if ((uint64)cmd.y + cmd.h > fb_state.yres) {
            cmd.h = fb_state.yres - cmd.y;
            clipped = 1;
        }

        gpu_fill_rect_locked(cmd.x, cmd.y, cmd.w, cmd.h, cmd.color);
        fb_state.stats.fill_rects++;
        if (clipped)
            fb_state.stats.clipped_blits++;
        spin_unlock(&fb_state.lock);
        virtio_gpu_present_fb_rect(fb_state.fb_virt, fb_state.pitch,
                                   cmd.x, cmd.y, cmd.w, cmd.h);
        hyperv_video_dirty(cmd.x, cmd.y, cmd.w, cmd.h);
        return 0;
    }

    case FB_GPU_BLIT: {
        struct fb_gpu_blit cmd;

        if (either_copyin((char *)&cmd, 1, (uint64)arg, sizeof(cmd)) < 0)
            return -EFAULT;
        return fb_blit_from_user(cmd, 0);
    }

    case FB_GPU_BO_CREATE: {
        struct fb_gpu_bo_create req;
        uint64 size;
        uint64 addr;
        uint32 handle;
        uint32 npages;
        page_t **pages;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if ((req.flags & ~FB_GPU_BO_F_EXPORTABLE) != 0 ||
            req.width == 0 || req.height == 0 ||
            req.width > 8192 || req.height > 8192 ||
            req.width > 0xffffffffU / 4)
            return -EINVAL;

        req.pitch = req.width * 4;
        if ((uint64)req.pitch > ((uint64)-1) / req.height)
            return -EINVAL;
        size = (uint64)req.pitch * req.height;
        if (size == 0 || size > 64ULL * 1024 * 1024)
            return -EINVAL;

        req.size = FB_GPU_ALIGN_UP(size, FB_GPU_D3D12_HEAP_ALIGN);
        npages = req.size / PGSIZE;
        ret = fb_shmem_alloc_pages(npages, &pages);
        if (ret != 0)
            return ret;

        ret = fb_bo_register(owner_id, owner_tgid, req.width, req.height,
                             req.pitch, req.size, pages, npages, &handle);
        if (ret != 0) {
            fb_shmem_release_pages(pages, npages);
            return ret;
        }

        struct fb_gpu_bo_entry *bo =
            fb_bo_get_owned(handle, owner_id, owner_tgid);
        if (bo == NULL) {
            (void)fb_bo_destroy_owned(handle, owner_id, owner_tgid);
            return -ENOENT;
        }
        ret = fb_bo_map_current(bo, &addr);
        fb_bo_put(bo);
        if (ret != 0) {
            (void)fb_bo_destroy_owned(handle, owner_id, owner_tgid);
            return ret;
        }

        req.addr = addr;
        req.handle = handle;
        req.reserved = 0;
        spin_lock(&fb_state.lock);
        fb_state.stats.bo_allocs++;
        fb_state.stats.bo_bytes += req.size;
        spin_unlock(&fb_state.lock);

        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            (void)fb_bo_destroy_owned(handle, owner_id, owner_tgid);
            (void)vm_munmap(current->vm, addr, (size_t)req.size);
            return -EFAULT;
        }
        return 0;
    }

    case FB_GPU_BO_PRESENT: {
        struct fb_gpu_bo_present req;
        struct fb_gpu_blit blit;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if ((req.flags & ~(FB_GPU_BO_PRESENT_F_VIRGL_COPY |
                           FB_GPU_BO_PRESENT_F_VIRGL_SCANOUT |
                           FB_GPU_BO_PRESENT_F_READBACK_FALLBACK)) != 0)
            return -EINVAL;

        if (req.handle != 0) {
            struct fb_gpu_bo_entry *bo =
                fb_bo_get_owned(req.handle, owner_id, owner_tgid);
            int ret;

            if (bo == NULL) {
                spin_lock(&fb_state.lock);
                fb_state.stats.rejected_blits++;
                spin_unlock(&fb_state.lock);
                return -ENOENT;
            }
            ret = fb_blit_from_bo(bo, req, &req.fence, &req.flags);
            fb_bo_put(bo);
            if (ret == 0 &&
                either_copyout(1, (uint64)arg, (char *)&req,
                               sizeof(req)) < 0)
                return -EFAULT;
            return ret;
        }

        if (req.flags != 0)
            return -EINVAL;
        req.fence = 0;
        blit.x = req.x;
        blit.y = req.y;
        blit.w = req.w;
        blit.h = req.h;
        blit.src_pitch = req.src_pitch;
        blit.pixels = req.pixels;
        int ret = fb_blit_from_user(blit, 1);
        if (ret == 0 &&
            either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        return ret;
    }

    case FB_GPU_PAGE_FLIP: {
        struct fb_gpu_page_flip req;
        struct fb_gpu_bo_entry *bo;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.handle == 0 || req.flags != 0)
            return -EINVAL;

        bo = fb_bo_get_owned(req.handle, owner_id, owner_tgid);
        if (bo == NULL)
            return -ENOENT;
        ret = fb_page_flip_bo(bo, &req);
        fb_bo_put(bo);
        if (ret == 0 &&
            either_copyout(1, (uint64)arg, (char *)&req,
                           sizeof(req)) < 0)
            return -EFAULT;
        return ret;
    }

    case FB_GPU_SET_CURSOR: {
        struct fb_gpu_cursor_image req;
        uint64 npix;
        uint64 size;
        void *pixels;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.reserved != 0 ||
            req.width == 0 || req.height == 0 ||
            req.width > FB_GPU_CURSOR_MAX_DIM ||
            req.height > FB_GPU_CURSOR_MAX_DIM ||
            req.hot_x >= req.width || req.hot_y >= req.height ||
            req.pixels == 0)
            return -EINVAL;

        npix = (uint64)req.width * req.height;
        size = npix * sizeof(uint32);
        pixels = kvmalloc((size_t)size);
        if (pixels == NULL)
            return -ENOMEM;
        if (either_copyin((char *)pixels, 1, req.pixels, size) < 0) {
            kvfree(pixels);
            return -EFAULT;
        }
        ret = virtio_gpu_user_set_cursor(pixels, req.width, req.height,
                                         req.hot_x, req.hot_y);
        kvfree(pixels);
        return ret;
    }

    case FB_GPU_MOVE_CURSOR: {
        struct fb_gpu_cursor_move req;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.reserved != 0)
            return -EINVAL;
        return virtio_gpu_user_move_cursor(
            req.x, req.y, (req.flags & FB_GPU_CURSOR_F_VISIBLE) ? 1 : 0);
    }

    case FB_GPU_BO_COPY: {
        struct fb_gpu_bo_copy req;
        struct fb_gpu_bo_entry *src_bo;
        struct fb_gpu_bo_entry *dst_bo;
        uint64 fence = 0;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.reserved != 0 ||
            req.src_handle == 0 || req.dst_handle == 0 ||
            req.w == 0 || req.h == 0)
            return -EINVAL;

        src_bo = fb_bo_get_owned(req.src_handle, owner_id, owner_tgid);
        if (src_bo == NULL)
            return -ENOENT;
        dst_bo = fb_bo_get_owned(req.dst_handle, owner_id, owner_tgid);
        if (dst_bo == NULL) {
            fb_bo_put(src_bo);
            return -ENOENT;
        }
        if (src_bo->virtio_resource_id == 0 ||
            dst_bo->virtio_resource_id == 0) {
            ret = -EOPNOTSUPP;
            goto bo_copy_out;
        }
        ret = virtio_gpu_copy_resource_to_resource(
            src_bo->virtio_resource_id, dst_bo->virtio_resource_id,
            req.src_x, req.src_y, req.dst_x, req.dst_y, req.w, req.h);
        if (ret == 0) {
            spin_lock(&fb_state.lock);
            fence = fb_bo_signal_present_locked(dst_bo);
            spin_unlock(&fb_state.lock);
            req.fence = fence;
        }
bo_copy_out:
        fb_bo_put(dst_bo);
        fb_bo_put(src_bo);
        if (ret == 0 &&
            either_copyout(1, (uint64)arg, (char *)&req,
                           sizeof(req)) < 0)
            return -EFAULT;
        return ret;
    }

    case FB_GPU_SCANOUT_MAP: {
        struct fb_gpu_scanout_map req;
        int ret;

        memset(&req, 0, sizeof(req));
        ret = fb_map_scanout_current(&req);
        if (ret != 0)
            return ret;
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            (void)vm_munmap(current->vm, req.addr, (size_t)req.size);
            return -EFAULT;
        }
        return 0;
    }

    case FB_GPU_SCANOUT_FLUSH: {
        struct fb_gpu_scanout_flush req;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        return fb_flush_scanout_rect(req);
    }

    case FB_GPU_SCANOUT_READ: {
        struct fb_gpu_scanout_read req;
        void *pixels;
        uint64 size;
        int virtio_backed;
        int ret;
        int diag;
        int skip_kms;
        int guard_kms_black;
        uint32 seq = 0;
        uint64 start_ticks = 0;
        uint64 phase_ticks = 0;
        static uint32 read_seq;
        static int read_logs;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        diag = fb_cmdline_enabled("virtio_gpu_scanout_read_diag") ||
               fb_cmdline_enabled("fb_scanout_read_ioctl_diag");
        skip_kms = fb_cmdline_enabled("fb_scanout_read_skip_kms") ||
                   fb_cmdline_enabled("virtio_gpu_scanout_read_skip_kms");
        guard_kms_black =
            !fb_cmdline_enabled("fb_scanout_read_no_kms_black_guard") &&
            !fb_cmdline_enabled("virtio_gpu_scanout_read_no_kms_black_guard");
        if (diag) {
            seq = __atomic_add_fetch(&read_seq, 1, __ATOMIC_RELAXED);
            start_ticks = r_time();
            if (read_logs < 96) {
                printf("FB: scanout-read-ioctl[%u] begin rect=%u,%u %ux%u pitch=%u pixels=0x%lx flags=0x%x\n",
                       seq, req.x, req.y, req.w, req.h, req.pitch,
                       req.pixels, req.flags);
                read_logs++;
            }
        }
        if (req.flags != 0 || req.pixels == 0 || req.w == 0 || req.h == 0 ||
            req.pitch < req.w * sizeof(uint32)) {
            if (diag && read_logs < 96) {
                printf("FB: scanout-read-ioctl[%u] end ret=%d elapsed_us=%lu reason=bad-args\n",
                       seq, -EINVAL, fb_ticks_to_us(r_time() - start_ticks));
                read_logs++;
            }
            return -EINVAL;
        }
        size = (uint64)req.pitch * req.h;
        if (size == 0 || size > 64ULL * 1024 * 1024) {
            if (diag && read_logs < 96) {
                printf("FB: scanout-read-ioctl[%u] end ret=%d elapsed_us=%lu reason=bad-size size=%lu\n",
                       seq, -EINVAL, fb_ticks_to_us(r_time() - start_ticks),
                       size);
                read_logs++;
            }
            return -EINVAL;
        }

        pixels = kvmalloc((size_t)size);
        if (pixels == NULL) {
            if (diag && read_logs < 96) {
                printf("FB: scanout-read-ioctl[%u] end ret=%d elapsed_us=%lu reason=nomem size=%lu\n",
                       seq, -ENOMEM, fb_ticks_to_us(r_time() - start_ticks),
                       size);
                read_logs++;
            }
            return -ENOMEM;
        }
        memset(pixels, 0, (size_t)size);
        spin_lock(&fb_state.lock);
        virtio_backed = fb_state.virtio_backed;
        spin_unlock(&fb_state.lock);
        if (diag && read_logs < 96) {
            printf("FB: scanout-read-ioctl[%u] buffer-ready size=%lu virtio_backed=%d elapsed_us=%lu\n",
                   seq, size, virtio_backed,
                   fb_ticks_to_us(r_time() - start_ticks));
            read_logs++;
        }
        if (virtio_backed) {
            phase_ticks = r_time();
            if (diag && read_logs < 96) {
                printf("FB: scanout-read-ioctl[%u] virtio-begin elapsed_us=%lu\n",
                       seq, fb_ticks_to_us(phase_ticks - start_ticks));
                read_logs++;
            }
            ret = virtio_gpu_read_current_scanout(
                req.x, req.y, req.w, req.h, pixels, req.pitch,
                &req.screen_width, &req.screen_height, &req.screen_pitch);
            if (diag && read_logs < 96) {
                printf("FB: scanout-read-ioctl[%u] virtio-end ret=%d stage_us=%lu elapsed_us=%lu screen=%ux%u pitch=%u\n",
                       seq, ret, fb_ticks_to_us(r_time() - phase_ticks),
                       fb_ticks_to_us(r_time() - start_ticks),
                       req.screen_width, req.screen_height, req.screen_pitch);
                read_logs++;
            }
            if (ret == 0 && skip_kms) {
                if (diag && read_logs < 96) {
                    printf("FB: scanout-read-ioctl[%u] kms-skip elapsed_us=%lu\n",
                           seq, fb_ticks_to_us(r_time() - start_ticks));
                    read_logs++;
                }
            } else if (ret == 0) {
                void *kms_pixels = NULL;
                void *kms_dst = pixels;
                uint32 virtio_nonblack = 0;
                uint32 virtio_center = 0;
                uint64 virtio_hash = 0;
                uint32 kms_nonblack = 0;
                uint32 kms_center = 0;
                uint64 kms_hash = 0;
                uint32 kms_screen_width = req.screen_width;
                uint32 kms_screen_height = req.screen_height;
                uint32 kms_screen_pitch = req.screen_pitch;
                int kms_ret;

                if (guard_kms_black) {
                    virtio_hash =
                        fb_scanout_diag_sample_hash(pixels, req.pitch,
                                                    req.w, req.h,
                                                    &virtio_nonblack,
                                                    &virtio_center);
                    kms_pixels = kvmalloc((size_t)size);
                    if (kms_pixels != NULL) {
                        memset(kms_pixels, 0, (size_t)size);
                        kms_dst = kms_pixels;
                    }
                    if (diag && read_logs < 96) {
                        printf("FB: scanout-read-ioctl[%u] virtio-sample hash=0x%lx nonblack=%u center=0x%x kms_guard=%d kms_temp=%d elapsed_us=%lu\n",
                               seq, virtio_hash, virtio_nonblack,
                               virtio_center, guard_kms_black,
                               kms_pixels != NULL,
                               fb_ticks_to_us(r_time() - start_ticks));
                        read_logs++;
                    }
                }
                phase_ticks = r_time();
                if (diag && read_logs < 96) {
                    printf("FB: scanout-read-ioctl[%u] kms-begin elapsed_us=%lu\n",
                           seq, fb_ticks_to_us(phase_ticks - start_ticks));
                    read_logs++;
                }
                kms_ret = fb_read_current_kms_framebuffer(
                    req.x, req.y, req.w, req.h, kms_dst, req.pitch,
                    &kms_screen_width, &kms_screen_height,
                    &kms_screen_pitch);
                if (guard_kms_black && kms_ret == 0) {
                    kms_hash =
                        fb_scanout_diag_sample_hash(kms_dst, req.pitch,
                                                    req.w, req.h,
                                                    &kms_nonblack,
                                                    &kms_center);
                }
                if (diag && read_logs < 96) {
                    printf("FB: scanout-read-ioctl[%u] kms-end ret=%d stage_us=%lu elapsed_us=%lu screen=%ux%u pitch=%u\n",
                           seq, kms_ret,
                           fb_ticks_to_us(r_time() - phase_ticks),
                           fb_ticks_to_us(r_time() - start_ticks),
                           kms_screen_width, kms_screen_height,
                           kms_screen_pitch);
                    read_logs++;
                }
                if (guard_kms_black && kms_ret == 0 &&
                    kms_pixels != NULL) {
                    int preserve_virtio =
                        virtio_nonblack > 0 && kms_nonblack == 0;

                    if (!preserve_virtio) {
                        memcpy(pixels, kms_pixels, (size_t)size);
                        req.screen_width = kms_screen_width;
                        req.screen_height = kms_screen_height;
                        req.screen_pitch = kms_screen_pitch;
                    }
                    if (diag && read_logs < 96) {
                        printf("FB: scanout-read-ioctl[%u] kms-sample hash=0x%lx nonblack=%u center=0x%x preserve_virtio=%d virtio_hash=0x%lx virtio_nonblack=%u virtio_center=0x%x elapsed_us=%lu\n",
                               seq, kms_hash, kms_nonblack, kms_center,
                               preserve_virtio, virtio_hash,
                               virtio_nonblack, virtio_center,
                               fb_ticks_to_us(r_time() - start_ticks));
                        read_logs++;
                    }
                } else if (kms_ret == 0) {
                    req.screen_width = kms_screen_width;
                    req.screen_height = kms_screen_height;
                    req.screen_pitch = kms_screen_pitch;
                }
                if (kms_pixels != NULL)
                    kvfree(kms_pixels);
                if (kms_ret == 0)
                    ret = 0;
            }
        } else {
            ret = fb_read_current_framebuffer(
                req.x, req.y, req.w, req.h, pixels, req.pitch,
                &req.screen_width, &req.screen_height, &req.screen_pitch);
        }
        if (ret == 0) {
            phase_ticks = r_time();
            if (diag && read_logs < 96) {
                printf("FB: scanout-read-ioctl[%u] pixel-copyout-begin bytes=%lu elapsed_us=%lu\n",
                       seq, (uint64)req.pitch * req.h,
                       fb_ticks_to_us(phase_ticks - start_ticks));
                read_logs++;
            }
            if (either_copyout(1, req.pixels, pixels,
                               (uint64)req.pitch * req.h) < 0)
                ret = -EFAULT;
            if (diag && read_logs < 96) {
                printf("FB: scanout-read-ioctl[%u] pixel-copyout-end ret=%d stage_us=%lu elapsed_us=%lu\n",
                       seq, ret, fb_ticks_to_us(r_time() - phase_ticks),
                       fb_ticks_to_us(r_time() - start_ticks));
                read_logs++;
            }
        }
        kvfree(pixels);
        if (ret != 0) {
            if (diag && read_logs < 96) {
                printf("FB: scanout-read-ioctl[%u] end ret=%d elapsed_us=%lu reason=read-or-copyout\n",
                       seq, ret, fb_ticks_to_us(r_time() - start_ticks));
                read_logs++;
            }
            return ret;
        }
        phase_ticks = r_time();
        if (diag && read_logs < 96) {
            printf("FB: scanout-read-ioctl[%u] req-copyout-begin elapsed_us=%lu\n",
                   seq, fb_ticks_to_us(phase_ticks - start_ticks));
            read_logs++;
        }
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            if (diag && read_logs < 96) {
                printf("FB: scanout-read-ioctl[%u] end ret=%d stage_us=%lu elapsed_us=%lu reason=req-copyout\n",
                       seq, -EFAULT, fb_ticks_to_us(r_time() - phase_ticks),
                       fb_ticks_to_us(r_time() - start_ticks));
                read_logs++;
            }
            return -EFAULT;
        }
        if (diag && read_logs < 96) {
            printf("FB: scanout-read-ioctl[%u] end ret=0 stage_us=%lu elapsed_us=%lu\n",
                   seq, fb_ticks_to_us(r_time() - phase_ticks),
                   fb_ticks_to_us(r_time() - start_ticks));
            read_logs++;
        }
        return 0;
    }

    case FB_GPU_DISPLAY_PROBE: {
        struct fb_gpu_display_probe req;
        uint32 host_w = 0, host_h = 0;
        uint32 edid_w = 0, edid_h = 0, edid_refresh = 0;

        memset(&req, 0, sizeof(req));
        spin_lock(&fb_state.lock);
        req.current_width = fb_state.xres;
        req.current_height = fb_state.yres;
        req.current_pitch = fb_state.pitch;
        spin_unlock(&fb_state.lock);
        req.current_refresh_millihz = 60000;
        req.host_refresh_millihz = 60000;
        if (virtio_gpu_probe_scanout(&host_w, &host_h) == 0 &&
            host_w != 0 && host_h != 0) {
            req.host_width = host_w;
            req.host_height = host_h;
            req.flags |= FB_GPU_DISPLAY_F_HOST_SCANOUT;
            if (host_w > 4096 || host_h > 4096 ||
                (req.current_width != 0 && host_w >= req.current_width * 2) ||
                (req.current_height != 0 && host_h >= req.current_height * 2))
                req.flags |= FB_GPU_DISPLAY_F_HOST_SCALED;
        }
        if (virtio_gpu_probe_edid_mode(&edid_w, &edid_h, &edid_refresh) == 0 &&
            edid_w != 0 && edid_h != 0) {
            req.preferred_width = edid_w;
            req.preferred_height = edid_h;
            req.preferred_refresh_millihz = edid_refresh;
            req.flags |= FB_GPU_DISPLAY_F_EDID;
            if (edid_w == req.current_width && edid_h == req.current_height &&
                edid_refresh != 0)
                req.current_refresh_millihz = edid_refresh;
        }
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }

    case FB_GPU_DISPLAY_WAIT: {
        struct fb_gpu_display_wait req;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if ((req.flags & ~FB_GPU_DISPLAY_WAIT_F_WAIT) != 0)
            return -EINVAL;

        spin_lock(&fb_state.lock);
        req.presented = fb_state.stats.display_last_present;
        req.completed = fb_state.stats.display_last_complete;
        spin_unlock(&fb_state.lock);
        req.refresh_millihz = 60000;

        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        if ((req.flags & FB_GPU_DISPLAY_WAIT_F_WAIT) &&
            req.wait_for != 0 && req.completed < req.wait_for)
            return -EAGAIN;
        return 0;
    }

    case FB_GPU_BO_DESTROY: {
        struct fb_gpu_bo_destroy req;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.handle == 0)
            return -EINVAL;
        return fb_bo_destroy_owned(req.handle, owner_id, owner_tgid);
    }

    case FB_GPU_BO_IMPORT: {
        struct fb_gpu_bo_import req;
        struct fb_gpu_bo_entry *bo;
        uint64 addr;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.handle == 0)
            return -EINVAL;

        bo = fb_bo_get_owned(req.handle, owner_id, owner_tgid);
        if (bo == NULL) {
            spin_lock(&fb_state.lock);
            fb_state.stats.rejected_blits++;
            spin_unlock(&fb_state.lock);
            return -ENOENT;
        }

        ret = fb_bo_map_current(bo, &addr);
        if (ret != 0) {
            fb_bo_put(bo);
            return ret;
        }

        req.width = bo->width;
        req.height = bo->height;
        req.pitch = bo->pitch;
        req.reserved = 0;
        req.size = bo->size;
        req.addr = addr;
        fb_bo_put(bo);

        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            (void)vm_munmap(current->vm, addr, (size_t)req.size);
            return -EFAULT;
        }

        spin_lock(&fb_state.lock);
        fb_state.stats.bo_imports++;
        spin_unlock(&fb_state.lock);
        return 0;
    }

    case FB_GPU_BO_EXPORT_FD: {
        struct fb_gpu_bo_export_fd req;
        struct fb_gpu_bo_entry *bo;
        struct dma_buf *dbuf;
        int fd;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.handle == 0)
            return -EINVAL;

        bo = fb_bo_get_owned(req.handle, owner_id, owner_tgid);
        if (bo == NULL) {
            spin_lock(&fb_state.lock);
            fb_state.stats.rejected_blits++;
            spin_unlock(&fb_state.lock);
            return -ENOENT;
        }

        struct fb_gpu_dmabuf_object *dmabuf;

        dmabuf = fb_dmabuf_create_from_bo(bo, FB_GPU_DMABUF_TAG_FB_BO);
        fb_bo_put(bo);
        if (dmabuf == NULL)
            return -ENOENT;
        dbuf = dmabuf->dbuf;
        fd = vfs_custom_fd_alloc(&fb_dmabuf_file_ops, dbuf, 0);
        if (fd < 0) {
            fb_dmabuf_put(dbuf);
            return fd;
        }

        req.fd = fd;
        req.reserved = 0;
        fb_dmabuf_note_export(dmabuf);
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            fb_gpu_close_exported_fd(fd);
            return -EFAULT;
        }
        return 0;
    }

    case FB_GPU_TEST_DMABUF_EXPORT_FD: {
        struct fb_gpu_bo_export_fd req;
        struct fb_gpu_bo_entry *bo;
        struct fb_gpu_dmabuf_object *dmabuf;
        struct dma_buf *dbuf;
        int fd;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.handle == 0)
            return -EINVAL;

        bo = fb_bo_get_owned(req.handle, owner_id, owner_tgid);
        if (bo == NULL)
            return -ENOENT;

        dmabuf = fb_test_dmabuf_create_from_bo(bo, FB_GPU_DMABUF_TAG_FB_BO);
        fb_bo_put(bo);
        if (dmabuf == NULL)
            return -ENOENT;
        dbuf = dmabuf->dbuf;
        fd = vfs_custom_fd_alloc(&fb_dmabuf_file_ops, dbuf, 0);
        if (fd < 0) {
            fb_dmabuf_put(dbuf);
            return fd;
        }

        req.fd = fd;
        req.reserved = 0;
        fb_dmabuf_note_export(dmabuf);
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            fb_gpu_close_exported_fd(fd);
            return -EFAULT;
        }
        return 0;
    }

    case FB_GPU_BO_IMPORT_FD: {
        struct fb_gpu_bo_import_fd req;
        struct vfs_file *file;
        struct fb_gpu_bo_entry *bo;
        struct fb_gpu_dmabuf_object *dmabuf;
        struct fb_gpu_gem_object *gem = NULL;
        struct dma_buf *dbuf;
        uint32 handle;
        uint64 addr;
        int ret;
        int handle_created = 0;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0)
            return -EINVAL;
        if (req.fd < 0) {
            fb_dmabuf_note_bad_fd_reject();
            return -EINVAL;
        }

        file = vfs_fdtable_get_file(current->fdtable, req.fd);
        if (file == NULL) {
            fb_dmabuf_note_bad_fd_reject();
            return -EBADF;
        }
        dbuf = fb_dma_buf_from_file(file);
        if (dbuf == NULL) {
            fb_dmabuf_note_foreign_fd_reject();
            vfs_fput(file);
            return -EINVAL;
        }
        dmabuf = fb_dmabuf_from_dma_buf(dbuf);

        ret = fb_dma_buf_get_gem(dbuf, &gem);
        if (ret != 0) {
            vfs_fput(file);
            return ret;
        }
        ret = fb_bo_register_gem(owner_id, owner_tgid, gem, &handle,
                                 &handle_created);
        fb_gem_put(gem);
        if (ret != 0) {
            vfs_fput(file);
            return ret;
        }

        bo = fb_bo_get_owned(handle, owner_id, owner_tgid);
        if (bo == NULL) {
            if (handle_created)
                (void)fb_bo_destroy_owned(handle, owner_id, owner_tgid);
            vfs_fput(file);
            return -ENOENT;
        }
        spin_lock(&fb_state.lock);
        ret = fb_bo_apply_dmabuf_metadata_locked(bo, req.format,
                                                 req.modifier,
                                                 req.plane_count,
                                                 req.offsets, req.strides);
        spin_unlock(&fb_state.lock);
        if (ret != 0) {
            fb_bo_put(bo);
            if (handle_created)
                (void)fb_bo_destroy_owned(handle, owner_id, owner_tgid);
            vfs_fput(file);
            return ret;
        }
        spin_lock(&fb_state.lock);
        fb_dmabuf_note_import_locked(dmabuf, bo, FB_GPU_DMABUF_TAG_FB_BO);
        spin_unlock(&fb_state.lock);
        ret = fb_bo_map_current(bo, &addr);
        if (ret != 0) {
            fb_bo_put(bo);
            if (handle_created)
                (void)fb_bo_destroy_owned(handle, owner_id, owner_tgid);
            vfs_fput(file);
            return ret;
        }

        req.width = bo->width;
        req.height = bo->height;
        req.pitch = bo->pitch;
        req.handle = handle;
        req.size = bo->size;
        req.addr = addr;
        if (bo->gem != NULL) {
            uint32 resource_id;
            uint64 resource_owner_id;
            pid_t resource_owner_tgid;
            uint64 resource_fence = 0;

            req.format = bo->gem->metadata.format;
            req.plane_count = bo->gem->metadata.plane_count;
            req.modifier = bo->gem->metadata.modifier;
            memmove(req.offsets, bo->gem->metadata.offsets,
                    sizeof(req.offsets));
            memmove(req.strides, bo->gem->metadata.strides,
                    sizeof(req.strides));
            req.implicit_fence = bo->gem->metadata.implicit_fence;
            req.explicit_fence = bo->gem->metadata.explicit_fence;
            resource_id = bo->virtio_resource_id != 0 ?
                bo->virtio_resource_id : bo->gem->virtio_resource_id;
            resource_owner_id = bo->virtio_resource_owner_id != 0 ?
                bo->virtio_resource_owner_id :
                bo->gem->virtio_resource_owner_id;
            resource_owner_tgid = bo->virtio_resource_owner_tgid != 0 ?
                bo->virtio_resource_owner_tgid :
                bo->gem->virtio_resource_owner_tgid;
            if (resource_id != 0 &&
                virtio_gpu_user_resource_last_submit_fence(
                    resource_owner_id, resource_owner_tgid, resource_id,
                    &resource_fence) == 0 &&
                resource_fence > req.implicit_fence)
                req.implicit_fence = resource_fence;
        }
        fb_bo_put(bo);
        vfs_fput(file);

        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            (void)vm_munmap(current->vm, addr, (size_t)req.size);
            if (handle_created)
                (void)fb_bo_destroy_owned(handle, owner_id, owner_tgid);
            return -EFAULT;
        }

        return 0;
    }

    case FB_GPU_BO_INFO: {
        struct fb_gpu_bo_info req;
        struct fb_gpu_bo_entry *bo;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.handle == 0)
            return -EINVAL;

        bo = fb_bo_get_owned(req.handle, owner_id, owner_tgid);
        if (bo == NULL)
            return -ENOENT;

        req.width = bo->width;
        req.height = bo->height;
        req.pitch = bo->pitch;
        req.format = bo->gem != NULL && bo->gem->metadata.format != 0 ?
            bo->gem->metadata.format : FB_GPU_BO_FORMAT_XRGB8888;
        req.modifier = bo->gem != NULL ?
            bo->gem->metadata.modifier : FB_GPU_BO_MOD_LINEAR;
        req.size = bo->size;
        req.addr_align = FB_GPU_D3D12_HEAP_ALIGN;
        req.size_align = FB_GPU_D3D12_HEAP_ALIGN;
        req.page_size = PGSIZE;
        req.reserved = 0;
        req.mmap_offset = GPU_DRM_MMAP_OFFSET(req.handle);
        req.virtio_resource_id = bo->virtio_resource_id;
        req.reserved1 = 0;
        req.virtio_resource_owner_id = bo->virtio_resource_owner_id;
        req.virtio_resource_owner_tgid = bo->virtio_resource_owner_tgid;
        req.reserved2 = 0;
        if (bo->gem != NULL) {
            uint64 resource_fence = 0;

            req.plane_count = bo->gem->metadata.plane_count;
            req.metadata_flags = 0;
            memmove(req.offsets, bo->gem->metadata.offsets,
                    sizeof(req.offsets));
            memmove(req.strides, bo->gem->metadata.strides,
                    sizeof(req.strides));
            req.implicit_fence = bo->gem->metadata.implicit_fence;
            req.explicit_fence = bo->gem->metadata.explicit_fence;
            if (req.virtio_resource_id == 0) {
                req.virtio_resource_id = bo->gem->virtio_resource_id;
                req.virtio_resource_owner_id =
                    bo->gem->virtio_resource_owner_id;
                req.virtio_resource_owner_tgid =
                    bo->gem->virtio_resource_owner_tgid;
            }
            if (req.virtio_resource_id != 0 &&
                virtio_gpu_user_resource_last_submit_fence(
                    req.virtio_resource_owner_id,
                    req.virtio_resource_owner_tgid,
                    req.virtio_resource_id, &resource_fence) == 0 &&
                resource_fence > req.implicit_fence)
                req.implicit_fence = resource_fence;
        }
        fb_bo_put(bo);

        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }

    case FB_GPU_TTM_VALIDATE: {
        struct fb_gpu_ttm_validate req;
        struct fb_gpu_bo_entry *bo;
        uint32 allowed = FB_GPU_TTM_F_SET_PLACEMENT |
                         FB_GPU_TTM_F_PIN |
                         FB_GPU_TTM_F_UNPIN |
                         FB_GPU_TTM_F_FORCE_EVICT |
                         FB_GPU_TTM_F_RESERVE |
                         FB_GPU_TTM_F_UNRESERVE |
                         FB_GPU_TTM_F_WW_VALIDATE;
        int ret = 0;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if ((req.flags & ~allowed) != 0 || req.handle == 0)
            return -EINVAL;
        if ((req.flags & FB_GPU_TTM_F_WW_VALIDATE) != 0 &&
            ((req.flags & ~FB_GPU_TTM_F_WW_VALIDATE) != 0 ||
             req.peer_handle == 0))
            return -EINVAL;
        if ((req.flags & FB_GPU_TTM_F_WW_VALIDATE) == 0 &&
            req.peer_handle != 0)
            return -EINVAL;

        spin_lock(&fb_state.lock);
        bo = fb_bo_lookup_owned_locked(req.handle, owner_id, owner_tgid);
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
        bo->refs++;
        if ((req.flags & FB_GPU_TTM_F_WW_VALIDATE) != 0) {
            struct fb_gpu_bo_entry *peer =
                fb_bo_lookup_owned_locked(req.peer_handle, owner_id,
                                          owner_tgid);

            if (peer == NULL || !fb_bo_owner_matches(peer, owner_id,
                                                     owner_tgid)) {
                fb_state.stats.ttm_resv_ww_validate_failures++;
                fb_state.stats.ttm_validate_failures++;
                ret = peer == NULL ? -ENOENT : -EPERM;
            } else {
                peer->refs++;
                ret = fb_ttm_resv_ww_validate_pair_locked(
                    bo, peer, owner_id, owner_tgid);
                peer->refs--;
            }
        }
        if ((req.flags & FB_GPU_TTM_F_RESERVE) != 0)
            ret = fb_ttm_reserve_locked(bo, owner_id, owner_tgid);
        if (ret == 0 && (req.flags & FB_GPU_TTM_F_FORCE_EVICT) != 0) {
            uint32 evict_mem = req.placement != 0 ?
                fb_ttm_mem_type_for_placement(req.placement) :
                bo->ttm_mem_type;

            if (req.placement != 0 && !fb_ttm_valid_placement(req.placement)) {
                fb_state.stats.ttm_validate_failures++;
                ret = -EINVAL;
            } else {
                ret = fb_ttm_evict_one_locked(evict_mem, owner_id,
                                              owner_tgid);
            }
        }
        if (ret == 0 && (req.flags & FB_GPU_TTM_F_SET_PLACEMENT) != 0)
            ret = fb_ttm_validate_placement_locked(bo, req.placement,
                                                   owner_id, owner_tgid);
        if (ret == 0 && (req.flags & FB_GPU_TTM_F_PIN) != 0)
            ret = fb_ttm_pin_locked(bo, owner_id, owner_tgid);
        if (ret == 0 && (req.flags & FB_GPU_TTM_F_UNPIN) != 0)
            ret = fb_ttm_unpin_locked(bo, owner_id, owner_tgid);
        if (ret == 0 && (req.flags & FB_GPU_TTM_F_UNRESERVE) != 0)
            ret = fb_ttm_unreserve_locked(bo, owner_id, owner_tgid);
        if (ret == 0) {
            if (!bo->in_use || bo->dead)
                ret = -ENOENT;
            else
                fb_ttm_fill_validate_locked(&req, bo);
        }
        spin_unlock(&fb_state.lock);
        fb_bo_put(bo);
        if (ret != 0)
            return ret;

        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }

    case FB_GPU_DXG_PRESENT_SOURCE_REGISTER: {
        struct fb_gpu_dxg_present_source_register req;
        int ret;

        spin_lock(&fb_state.lock);
        fb_state.stats.dxg_present_register_ioctl_entries++;
        spin_unlock(&fb_state.lock);
        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0) {
            spin_lock(&fb_state.lock);
            fb_state.stats.dxg_present_register_copyin_failures++;
            fb_state.stats.dxg_present_last_ret = EFAULT;
            spin_unlock(&fb_state.lock);
            return -EFAULT;
        }
        ret = fb_dxg_present_register(owner_id, owner_tgid, &req);
        if (ret != 0)
            return ret;
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            fb_dxg_present_source_unregister(req.present_source);
            return -EFAULT;
        }
        return 0;
    }

    case FB_GPU_DXG_PRESENT_SOURCE_COMMIT: {
        struct fb_gpu_dxg_present_source_commit req;
        int ret;

        spin_lock(&fb_state.lock);
        fb_state.stats.dxg_present_commit_ioctl_entries++;
        spin_unlock(&fb_state.lock);
        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0) {
            spin_lock(&fb_state.lock);
            fb_state.stats.dxg_present_commit_copyin_failures++;
            fb_state.stats.dxg_present_last_ret = EFAULT;
            spin_unlock(&fb_state.lock);
            return -EFAULT;
        }
        ret = fb_dxg_present_commit(owner_id, owner_tgid, &req);
        if (ret != 0)
            return ret;
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            spin_lock(&fb_state.lock);
            fb_state.stats.dxg_present_commit_copyout_failures++;
            fb_state.stats.dxg_present_last_ret = EFAULT;
            spin_unlock(&fb_state.lock);
            return -EFAULT;
        }
        return 0;
    }

    case FB_GPU_DXG_PRESENT_SOURCE_QUERY: {
        struct fb_gpu_dxg_present_source_query req;
        int ret;

        spin_lock(&fb_state.lock);
        fb_state.stats.dxg_present_query_ioctl_entries++;
        spin_unlock(&fb_state.lock);
        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0) {
            spin_lock(&fb_state.lock);
            fb_state.stats.dxg_present_query_copyin_failures++;
            fb_state.stats.dxg_present_last_ret = EFAULT;
            spin_unlock(&fb_state.lock);
            return -EFAULT;
        }
        ret = fb_dxg_present_query(owner_id, owner_tgid, &req);
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            spin_lock(&fb_state.lock);
            fb_state.stats.dxg_present_query_copyout_failures++;
            fb_state.stats.dxg_present_last_ret = EFAULT;
            spin_unlock(&fb_state.lock);
            return -EFAULT;
        }
        return ret;
    }

    case FB_GPU_DXG_PRESENT_BIND_CONTRACT_QUERY: {
        struct fb_gpu_dxg_present_host_bind_contract req;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0) {
            spin_lock(&fb_state.lock);
            fb_state.stats.dxg_present_query_copyin_failures++;
            fb_state.stats.dxg_present_last_ret = EFAULT;
            spin_unlock(&fb_state.lock);
            return -EFAULT;
        }
        ret = fb_dxg_present_bind_contract_query(owner_id, owner_tgid, &req);
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            spin_lock(&fb_state.lock);
            fb_state.stats.dxg_present_query_copyout_failures++;
            fb_state.stats.dxg_present_last_ret = EFAULT;
            spin_unlock(&fb_state.lock);
            return -EFAULT;
        }
        return ret;
    }

    case FB_GPU_BO_FENCE: {
        struct fb_gpu_bo_fence req;
        struct fb_gpu_bo_entry *bo;
        uint64 target;
        int ret = 0;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if ((req.flags & ~FB_GPU_BO_FENCE_WAIT) != 0 || req.handle == 0)
            return -EINVAL;

        bo = fb_bo_get_owned(req.handle, owner_id, owner_tgid);
        if (bo == NULL) {
            spin_lock(&fb_state.lock);
            fb_state.stats.rejected_blits++;
            spin_unlock(&fb_state.lock);
            return -ENOENT;
        }

        spin_lock(&fb_state.lock);
        target = req.wait_for ? req.wait_for :
            (bo->gem != NULL ? bo->gem->last_fence : bo->last_fence);
        req.last_present = bo->gem != NULL ?
            bo->gem->last_fence : bo->last_fence;
        req.signaled = bo->gem != NULL ?
            bo->gem->signaled_fence : bo->signaled_fence;
        fb_state.stats.bo_fence_waits++;
        if ((req.flags & FB_GPU_BO_FENCE_WAIT) != 0 && bo->gem != NULL) {
            ret = fb_ttm_resv_wait_fence_locked(bo->gem, target);
            req.last_present = bo->gem->last_fence;
            req.signaled = bo->gem->signaled_fence;
        }
        spin_unlock(&fb_state.lock);

        fb_bo_put(bo);
        if ((req.flags & FB_GPU_BO_FENCE_WAIT) && target > req.signaled)
            ret = ret != 0 ? ret : -EAGAIN;
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        return ret;
    }

    case FB_GPU_FENCE_EXPORT_FD: {
        struct fb_gpu_fence_export_fd req;
        struct fb_gpu_bo_entry *bo;
        struct fb_gpu_fence_file *fence_file;
        struct fb_gpu_fence *fence_obj;
        uint64 context;
        uint64 target;
        uint64 signaled;
        int fd;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.handle == 0)
            return -EINVAL;

        bo = fb_bo_get_owned(req.handle, owner_id, owner_tgid);
        if (bo == NULL)
            return -ENOENT;

        spin_lock(&fb_state.lock);
        target = req.fence ? req.fence :
            (bo->gem != NULL ? bo->gem->last_fence : bo->last_fence);
        req.fence = target;
        context = fb_gpu_fence_file_context_locked(bo);
        signaled = bo->gem != NULL ?
            bo->gem->signaled_fence : bo->signaled_fence;
        req.signaled = signaled;
        spin_unlock(&fb_state.lock);
        if (target == 0) {
            fb_bo_put(bo);
            return -EINVAL;
        }

        fence_obj = fb_gpu_fence_create(context, target,
                                        signaled >= target, 0);
        if (fence_obj == NULL) {
            fb_bo_put(bo);
            return -ENOMEM;
        }

        fence_file = kvmalloc(sizeof(*fence_file));
        if (fence_file == NULL) {
            fb_gpu_fence_put(fence_obj);
            fb_bo_put(bo);
            return -ENOMEM;
        }
        memset(fence_file, 0, sizeof(*fence_file));
        fence_file->bo = bo;
        fence_file->fence = target;
        fence_file->fence_obj = fence_obj;

        fd = vfs_custom_fd_alloc(&fb_fence_file_ops, fence_file, 0);
        if (fd < 0) {
            fb_bo_put(bo);
            fb_gpu_fence_put(fence_obj);
            kvfree(fence_file);
            return fd;
        }

        req.fd = fd;
        req.reserved = 0;
        spin_lock(&fb_state.lock);
        fb_state.stats.fence_fd_exports++;
        spin_unlock(&fb_state.lock);
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            fb_gpu_close_exported_fd(fd);
            return -EFAULT;
        }
        return 0;
    }

    case FB_GPU_FENCE_QUERY: {
        struct fb_gpu_fence_query req;
        struct fb_gpu_fence_file *fence_file;
        struct vfs_file *file;
        int signaled;
        int fence_error;
        int ret = 0;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if ((req.flags & ~FB_GPU_FENCE_WAIT) != 0 || req.fd < 0)
            return -EINVAL;

        file = vfs_fdtable_get_file(current->fdtable, req.fd);
        if (file == NULL)
            return -EBADF;
        if (file->ops != &fb_fence_file_ops || file->private_data == NULL) {
            vfs_fput(file);
            return -EINVAL;
        }

        fence_file = (struct fb_gpu_fence_file *)file->private_data;
        spin_lock(&fb_state.lock);
        (void)fb_gpu_fence_file_refresh_locked(fence_file);
        req.fence = fb_gpu_fence_query_locked(fence_file->fence_obj,
                                              &signaled, &fence_error);
        req.signaled = fb_gpu_fence_file_signaled_locked(fence_file);
        if (signaled && req.signaled < req.fence)
            req.signaled = req.fence;
        fb_state.stats.fence_fd_queries++;
        if ((req.flags & FB_GPU_FENCE_WAIT) != 0)
            ret = fb_gpu_fence_file_wait_locked(fence_file);
        if (ret == 0 && fence_error != 0)
            ret = fence_error < 0 ? fence_error : -fence_error;
        req.signaled = fb_gpu_fence_file_signaled_locked(fence_file);
        if (fence_file->fence_obj != NULL &&
            fence_file->fence_obj->signaled && req.signaled < req.fence)
            req.signaled = req.fence;
        spin_unlock(&fb_state.lock);
        vfs_fput(file);

        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        if (ret != 0)
            return ret;
        if ((req.flags & FB_GPU_FENCE_WAIT) && req.signaled < req.fence)
            return -EAGAIN;
        return 0;
    }

    case FB_GPU_VIRGL_CTX_CREATE: {
        struct fb_gpu_virgl_ctx req;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0)
            return -EINVAL;
        req.debug_name[sizeof(req.debug_name) - 1] = 0;

        ret = virtio_gpu_user_context_create(owner_id, owner_tgid,
                                             0, 0, req.debug_name,
                                             &req.ctx_id, NULL);
        if (ret != 0)
            return ret;
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            (void)virtio_gpu_user_context_destroy(owner_id, owner_tgid,
                                                  req.ctx_id);
            return -EFAULT;
        }
        return 0;
    }

    case FB_GPU_VIRGL_CTX_DESTROY: {
        struct fb_gpu_virgl_ctx req;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.ctx_id == 0)
            return -EINVAL;
        return virtio_gpu_user_context_destroy(owner_id, owner_tgid,
                                               req.ctx_id);
    }

    case FB_GPU_VIRGL_SUBMIT: {
        struct fb_gpu_virgl_submit req;
        uint32 *cmds;
        uint32 *resources = NULL;
        uint32 alloc_len = PGSIZE;
        int order = 0;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if ((req.flags & ~(FB_GPU_VIRGL_SUBMIT_ASYNC |
                           FB_GPU_VIRGL_SUBMIT_FORCE_FAIL)) != 0 ||
            req.ctx_id == 0 || req.cmd == 0 ||
            req.cmd_size == 0 || req.cmd_size > PGSIZE * 64 ||
            (req.cmd_size & (sizeof(uint32) - 1)) != 0 ||
            req.resource_count > 4096 ||
            (req.resource_count != 0 && req.resources == 0))
            return -EINVAL;

        while (alloc_len < req.cmd_size) {
            if (order >= PAGE_BUDDY_MAX_ORDER)
                return -ENOMEM;
            order++;
            alloc_len <<= 1;
        }

        cmds = page_alloc(order, PAGE_TYPE_ANON);
        if (cmds == NULL)
            return -ENOMEM;
        if (either_copyin((char *)cmds, 1, req.cmd, req.cmd_size) < 0) {
            page_free(cmds, order);
            return -EFAULT;
        }
        if (req.resource_count != 0) {
            uint64 resources_size =
                (uint64)req.resource_count * sizeof(uint32);

            resources = kvmalloc((size_t)resources_size);
            if (resources == NULL) {
                page_free(cmds, order);
                return -ENOMEM;
            }
            if (either_copyin((char *)resources, 1, req.resources,
                              resources_size) < 0) {
                kvfree(resources);
                page_free(cmds, order);
                return -EFAULT;
            }
        }

        ret = virtio_gpu_user_submit(owner_id, owner_tgid, req.ctx_id,
                                     req.flags, cmds,
                                     req.cmd_size / sizeof(uint32),
                                     resources, req.resource_count,
                                     &req.fence, &req.signaled, NULL, NULL);
        if (resources != NULL)
            kvfree(resources);
        page_free(cmds, order);
        if (ret != 0)
            return ret;
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }

    case FB_GPU_VIRGL_FENCE: {
        struct fb_gpu_virgl_fence req;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if ((req.flags & ~FB_GPU_VIRGL_FENCE_WAIT) != 0)
            return -EINVAL;

        ret = virtio_gpu_user_fence(req.wait_for,
                                    (req.flags & FB_GPU_VIRGL_FENCE_WAIT) != 0,
                                    &req.signaled);
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        return ret;
    }

    case FB_GPU_VIRGL_FENCE_EXPORT_FD: {
        struct fb_gpu_virgl_fence_export_fd req;
        struct fb_gpu_virgl_fence_file *fence_file;
        uint64 signaled = 0;
        uint64 target;
        int fd;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0)
            return -EINVAL;
        if (virtio_gpu_user_fence(0, 0, &signaled) != 0)
            return -ENODEV;
        target = req.fence ? req.fence : signaled;
        if (target == 0)
            return -EINVAL;

        fence_file = kvmalloc(sizeof(*fence_file));
        if (fence_file == NULL)
            return -ENOMEM;
        fence_file->fence = target;
        fence_file->live_accounted = 0;
        fd = vfs_custom_fd_alloc(&fb_virgl_fence_file_ops, fence_file, 0);
        if (fd < 0) {
            kvfree(fence_file);
            return fd;
        }

        req.fd = fd;
        req.fence = target;
        req.signaled = signaled;
        spin_lock(&fb_state.lock);
        fb_state.stats.fence_fd_exports++;
        spin_unlock(&fb_state.lock);
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            fb_gpu_close_exported_fd(fd);
            return -EFAULT;
        }
        return 0;
    }

    case FB_GPU_VIRGL_FENCE_QUERY_FD: {
        struct fb_gpu_virgl_fence_query_fd req;
        struct fb_gpu_virgl_fence_file *fence_file;
        struct vfs_file *file;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if ((req.flags & ~FB_GPU_VIRGL_FENCE_WAIT) != 0 || req.fd < 0)
            return -EINVAL;

        file = vfs_fdtable_get_file(current->fdtable, req.fd);
        if (file == NULL)
            return -EBADF;
        if (file->ops != &fb_virgl_fence_file_ops ||
            file->private_data == NULL) {
            vfs_fput(file);
            return -EINVAL;
        }
        fence_file = (struct fb_gpu_virgl_fence_file *)file->private_data;
        req.fence = fence_file->fence;
        vfs_fput(file);

        ret = virtio_gpu_user_fence(req.fence,
                                    (req.flags & FB_GPU_VIRGL_FENCE_WAIT) != 0,
                                    &req.signaled);
        if (ret != 0) {
            if (either_copyout(1, (uint64)arg, (char *)&req,
                               sizeof(req)) < 0)
                return -EFAULT;
            return ret;
        }

        spin_lock(&fb_state.lock);
        fb_state.stats.fence_fd_queries++;
        spin_unlock(&fb_state.lock);
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }

    case FB_GPU_VIRGL_GET_CAPS: {
        struct fb_gpu_virgl_caps req;
        void *caps = NULL;
        uint32 capset_size = 0;
        uint32 requested_size;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0)
            return -EINVAL;
        requested_size = req.size;
        if (req.data != 0) {
            if (requested_size == 0 || requested_size > PGSIZE)
                return -EINVAL;
            caps = kalloc();
            if (caps == NULL)
                return -ENOMEM;
        }

        ret = virtio_gpu_user_get_caps(caps, req.data ? req.size : 0,
                                       &req.capset_id,
                                       &req.capset_version,
                                       &capset_size);
        req.size = capset_size;
        if (ret == 0 && req.data != 0) {
            uint32 copy_size = capset_size;
            if (copy_size > requested_size)
                copy_size = requested_size;
            if (copy_size != 0 &&
                either_copyout(1, req.data, (char *)caps, copy_size) < 0)
                ret = -EFAULT;
        }
        if (caps != NULL)
            kfree(caps);
        if (ret != 0)
            return ret;
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }

    case FB_GPU_VIRGL_RESOURCE_CREATE: {
        struct fb_gpu_virgl_resource_create req;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        req.resource_id = 0;
        req.addr = 0;
        ret = virtio_gpu_user_resource_create(owner_id, owner_tgid, &req);
        if (ret != 0)
            return ret;
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            (void)virtio_gpu_user_resource_destroy(owner_id, owner_tgid,
                                                   req.resource_id);
            if (req.addr != 0 && req.size != 0)
                (void)vm_munmap(current->vm, req.addr, (size_t)req.size);
            return -EFAULT;
        }
        return 0;
    }

    case FB_GPU_VIRGL_RESOURCE_DESTROY: {
        struct fb_gpu_virgl_resource_destroy req;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.resource_id == 0)
            return -EINVAL;
        return virtio_gpu_user_resource_destroy(owner_id, owner_tgid,
                                                req.resource_id);
    }

    case FB_GPU_VIRGL_RESOURCE_ATTACH: {
        struct fb_gpu_virgl_resource_attach req;
        struct fb_gpu_bo_entry *bo;
        int allow_imported = 0;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.ctx_id == 0 || req.resource_id == 0)
            return -EINVAL;

        if (req.handle != 0) {
            bo = fb_bo_get_owned(req.handle, owner_id, owner_tgid);
            if (bo == NULL)
                return -ENOENT;
            if (bo->virtio_resource_id == req.resource_id ||
                (bo->gem != NULL &&
                 bo->gem->virtio_resource_id == req.resource_id))
                allow_imported = 1;
            fb_bo_put(bo);
            if (!allow_imported)
                return -EPERM;
        }

        return virtio_gpu_user_resource_attach(owner_id, owner_tgid,
                                               req.ctx_id,
                                               req.resource_id,
                                               allow_imported);
    }

    case FB_GPU_VIRGL_RESOURCE_EXPORT_FD: {
        struct fb_gpu_virgl_resource_export_fd req;
        struct fb_gpu_bo_entry *bo;
        struct fb_gpu_dmabuf_object *dmabuf;
        struct dma_buf *dbuf;
        page_t **pages = NULL;
        uint32 width = 0;
        uint32 height = 0;
        uint32 pitch = 0;
        uint32 npages = 0;
        uint32 handle = 0;
        uint64 size = 0;
        int fd;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.resource_id == 0)
            return -EINVAL;

        ret = virtio_gpu_user_resource_export_pages(owner_id, owner_tgid,
                                                    req.resource_id, &width,
                                                    &height, &pitch, &size,
                                                    &pages, &npages);
        if (ret != 0)
            return ret;

        ret = fb_bo_register(owner_id, owner_tgid, width, height, pitch, size,
                             pages, npages, &handle);
        if (ret != 0) {
            fb_shmem_release_pages(pages, npages);
            return ret;
        }
        ret = fb_bo_set_virtio_resource(handle, owner_id, owner_tgid,
                                        req.resource_id);
        if (ret != 0) {
            (void)fb_bo_destroy_owned(handle, owner_id, owner_tgid);
            return ret;
        }

        bo = fb_bo_get_owned(handle, owner_id, owner_tgid);
        if (bo == NULL) {
            (void)fb_bo_destroy_owned(handle, owner_id, owner_tgid);
            return -ENOENT;
        }
        dmabuf = fb_dmabuf_create_from_bo(bo, FB_GPU_DMABUF_TAG_FB_BO);
        fb_bo_put(bo);
        if (dmabuf == NULL) {
            (void)fb_bo_destroy_owned(handle, owner_id, owner_tgid);
            return -ENOENT;
        }
        dbuf = dmabuf->dbuf;
        fd = vfs_custom_fd_alloc(&fb_dmabuf_file_ops, dbuf, 0);
        if (fd < 0) {
            fb_dmabuf_put(dbuf);
            (void)fb_bo_destroy_owned(handle, owner_id, owner_tgid);
            return fd;
        }

        req.fd = fd;
        req.handle = handle;
        req.width = width;
        req.height = height;
        req.pitch = pitch;
        req.reserved = 0;
        req.size = size;
        fb_dmabuf_note_export(dmabuf);

        /*
         * The fd owns the exported BO capability. Drop the transient handle
         * immediately so closing the fd releases the shared page references.
         */
        (void)fb_bo_destroy_owned(handle, owner_id, owner_tgid);

        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            fb_gpu_close_exported_fd(fd);
            return -EFAULT;
        }
        return 0;
    }

    case FB_GPU_VIRGL_TRANSFER_TO_HOST:
    case FB_GPU_VIRGL_TRANSFER_FROM_HOST: {
        struct fb_gpu_virgl_transfer req;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        return virtio_gpu_user_transfer(owner_id, owner_tgid, &req,
            cmd == FB_GPU_VIRGL_TRANSFER_FROM_HOST);
    }

    case FB_GPU_COPY_RECT: {
        struct fb_gpu_copy cmd;
        int clipped = 0;
        if (either_copyin((char *)&cmd, 1, (uint64)arg, sizeof(cmd)) < 0)
            return -EFAULT;
        if (cmd.w == 0 || cmd.h == 0)
            return 0;

        spin_lock(&fb_state.lock);
        /* Clip source and destination to screen bounds */
        if (cmd.src_x >= fb_state.xres || cmd.src_y >= fb_state.yres ||
            cmd.dst_x >= fb_state.xres || cmd.dst_y >= fb_state.yres) {
            spin_unlock(&fb_state.lock);
            return 0;
        }
        uint32 w = cmd.w, h = cmd.h;
        if ((uint64)cmd.src_x + w > fb_state.xres) {
            w = fb_state.xres - cmd.src_x;
            clipped = 1;
        }
        if ((uint64)cmd.src_y + h > fb_state.yres) {
            h = fb_state.yres - cmd.src_y;
            clipped = 1;
        }
        if ((uint64)cmd.dst_x + w > fb_state.xres) {
            w = fb_state.xres - cmd.dst_x;
            clipped = 1;
        }
        if ((uint64)cmd.dst_y + h > fb_state.yres) {
            h = fb_state.yres - cmd.dst_y;
            clipped = 1;
        }

        gpu_copy_rect_locked(cmd.src_x, cmd.src_y,
                             cmd.dst_x, cmd.dst_y, w, h);
        fb_state.stats.copy_rects++;
        if (clipped)
            fb_state.stats.clipped_blits++;
        spin_unlock(&fb_state.lock);
        virtio_gpu_present_fb_rect(fb_state.fb_virt, fb_state.pitch,
                                   cmd.dst_x, cmd.dst_y, w, h);
        return 0;
    }

    case FB_GPU_GET_STATS: {
        struct fb_gpu_stats stats;
        struct fb_gpu_backend_info backend;

        spin_lock(&fb_state.lock);
        fb_dxg_present_note_host_lanes_locked();
        stats = fb_state.stats;
        spin_unlock(&fb_state.lock);
        stats.ttm_resv_shared_slots = FB_GPU_RESV_SHARED_SLOTS;
        stats.dmabuf_poll_semantics = 1;
        stats.dmabuf_shared_fence_semantics = FB_GPU_RESV_SHARED_SLOTS;
        stats.dmabuf_wait_queue_semantics = 1;
        stats.drm_minor_model_version = 1;
        stats.drm_minor_primary_index = 0;
        stats.drm_minor_render_index = 128;
        stats.drm_minor_control_index = 64;
        stats.drm_minor_control_registered = 0;
        stats.drm_minor_static_nodes =
            stats.drm_minor_primary_registered +
            stats.drm_minor_render_registered +
            stats.drm_minor_control_registered;
        stats.drm_minor_dynamic_nodes = 0;
        virtio_gpu_get_fb_stats(&stats);
        gpu_backend_fill(&backend);
        stats.gpu_backend = backend.backend;
        stats.gpu_backend_flags = backend.flags;
        stats.dxg_global_open = backend.dxg_global_open;
        stats.dxg_vgpu_open = backend.dxg_vgpu_open;
        stats.dxg_d3dkmt = backend.dxg_d3dkmt;
        stats.dxg_global_rx = backend.dxg_global_rx;
        stats.dxg_vgpu_rx = backend.dxg_vgpu_rx;
        if (either_copyout(1, (uint64)arg, (char *)&stats, sizeof(stats)) < 0)
            return -EFAULT;
        return 0;
    }

    case FB_GPU_BACKEND_QUERY:
        return gpu_backend_query((uint64)arg);

    case FBIOPUT_VSCREENINFO: {
        struct fb_var_screeninfo req;
        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;

        /* Validate requested resolution */
        if (req.xres < 640 || req.xres > 2560 ||
            req.yres < 480 || req.yres > 1600)
            return -EINVAL;

        if (fb_state.virtio_backed) {
            int ret = virtio_gpu_resize_scanout(req.xres, req.yres);
            if (ret == 0)
                printf("FB: virtio resolution changed to %dx%d\n",
                       req.xres, req.yres);
            return ret;
        }

        spin_lock(&fb_state.lock);
        bga_set_mode(req.xres, req.yres, FB_DEFAULT_BPP);

        /* Clear framebuffer to black after mode change */
        volatile uint32 *pixels = (volatile uint32 *)fb_state.fb_virt;
        uint32 npx = fb_state.xres * fb_state.yres;
        for (uint32 i = 0; i < npx; i++)
            pixels[i] = 0x00000000;
        spin_unlock(&fb_state.lock);
        virtio_gpu_present_fb_rect(fb_state.fb_virt, fb_state.pitch,
                                   0, 0, fb_state.xres, fb_state.yres);

        printf("FB: resolution changed to %dx%d\n", fb_state.xres, fb_state.yres);
        return 0;
    }

    default:
        return -EINVAL;
    }
}

static int fb_ioctl(cdev_t *cdev, uint64 cmd, void *arg)
{
    return fb_ioctl_for_owner(cdev, cmd, arg, 0,
                              current ? current->tgid : 0);
}

static int gpu_ioctl(cdev_t *cdev, uint64 cmd, void *arg)
{
    int trace = fb_gpu_trace_enabled() && fb_gpu_trace_process();
    int ret;

    if (trace)
        printf("fb-gpu-trace: enter pid=%d name=%s dev=cdev cmd=0x%lx(%s)\n",
               current ? current->pid : -1,
               current ? current->name : "?", cmd,
               fb_gpu_ioctl_name(cmd));

    switch (cmd) {
    case FB_GPU_GET_STATS:
    case FB_GPU_BO_CREATE:
    case FB_GPU_BO_PRESENT:
    case FB_GPU_PAGE_FLIP:
    case FB_GPU_BO_COPY:
    case FB_GPU_SET_CURSOR:
    case FB_GPU_MOVE_CURSOR:
    case FB_GPU_BO_DESTROY:
    case FB_GPU_BO_IMPORT:
    case FB_GPU_BO_EXPORT_FD:
    case FB_GPU_TEST_DMABUF_EXPORT_FD:
    case FB_GPU_BO_IMPORT_FD:
    case FB_GPU_BO_INFO:
    case FB_GPU_TTM_VALIDATE:
    case FB_GPU_DXG_PRESENT_SOURCE_REGISTER:
    case FB_GPU_DXG_PRESENT_SOURCE_COMMIT:
    case FB_GPU_DXG_PRESENT_SOURCE_QUERY:
    case FB_GPU_DXG_PRESENT_BIND_CONTRACT_QUERY:
    case FB_GPU_BO_FENCE:
    case FB_GPU_FENCE_EXPORT_FD:
    case FB_GPU_FENCE_QUERY:
    case FB_GPU_VIRGL_CTX_CREATE:
    case FB_GPU_VIRGL_CTX_DESTROY:
    case FB_GPU_VIRGL_SUBMIT:
    case FB_GPU_VIRGL_FENCE:
    case FB_GPU_VIRGL_FENCE_EXPORT_FD:
    case FB_GPU_VIRGL_FENCE_QUERY_FD:
    case FB_GPU_VIRGL_GET_CAPS:
    case FB_GPU_VIRGL_RESOURCE_CREATE:
    case FB_GPU_VIRGL_RESOURCE_DESTROY:
    case FB_GPU_VIRGL_RESOURCE_ATTACH:
    case FB_GPU_VIRGL_RESOURCE_EXPORT_FD:
    case FB_GPU_VIRGL_TRANSFER_TO_HOST:
    case FB_GPU_VIRGL_TRANSFER_FROM_HOST:
    case FB_GPU_DISPLAY_PROBE:
    case FB_GPU_BACKEND_QUERY:
        break;
    default:
        return -EINVAL;
    }

    spin_lock(&fb_state.lock);
    fb_state.stats.gpu_ioctls++;
    spin_unlock(&fb_state.lock);
    ret = fb_ioctl(cdev, cmd, arg);
    if (trace)
        printf("fb-gpu-trace: exit pid=%d name=%s dev=cdev cmd=0x%lx(%s) ret=%d\n",
               current ? current->pid : -1,
               current ? current->name : "?", cmd,
               fb_gpu_ioctl_name(cmd), ret);
    return ret;
}

static uint64 gpu_alloc_render_owner_id(void)
{
    uint64 id;

    spin_lock(&fb_state.lock);
    id = fb_state.next_render_owner_id++;
    if (fb_state.next_render_owner_id == 0)
        fb_state.next_render_owner_id = 1;
    spin_unlock(&fb_state.lock);
    return id;
}

static enum drm_core_node_type gpu_drm_node_from_cdev(cdev_t *cdev)
{
    if (cdev == NULL)
        return DRM_CORE_NODE_LEGACY;
    if (cdev->dev.devname != NULL) {
        if (strcmp(cdev->dev.devname, "dri/card0") == 0)
            return DRM_CORE_NODE_PRIMARY;
        if (strcmp(cdev->dev.devname, "dri/renderD128") == 0)
            return DRM_CORE_NODE_RENDER;
    }
    if (cdev->dev.major == DRM_PRIMARY_MAJOR &&
        cdev->dev.minor == DRM_PRIMARY_MINOR)
        return DRM_CORE_NODE_PRIMARY;
    if (cdev->dev.major == DRM_RENDER_MAJOR &&
        cdev->dev.minor == DRM_RENDER_MINOR)
        return DRM_CORE_NODE_RENDER;
    return DRM_CORE_NODE_LEGACY;
}
