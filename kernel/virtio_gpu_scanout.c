static int virtio_gpu_smoke_resource(struct virtio_gpu *g)
{
    struct virtio_gpu_resource *res;
    int scanout_bound = 0;

    if (virtio_gpu_resource_create_2d(g, VIRTIO_GPU_SMOKE_WIDTH,
                                      VIRTIO_GPU_SMOKE_HEIGHT,
                                      VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                                      &res) != 0)
        return -1;

    uint32 *pixels = (uint32 *)res->backing;

    for (uint32 y = 0; y < VIRTIO_GPU_SMOKE_HEIGHT; y++) {
        for (uint32 x = 0; x < VIRTIO_GPU_SMOKE_WIDTH; x++) {
            uint8 r = (x * 255) / (VIRTIO_GPU_SMOKE_WIDTH - 1);
            uint8 gch = (y * 255) / (VIRTIO_GPU_SMOKE_HEIGHT - 1);
            pixels[y * VIRTIO_GPU_SMOKE_WIDTH + x] =
                0xff000000u | ((uint32)r << 16) | ((uint32)gch << 8) | 0x40;
        }
    }

    if (virtio_gpu_resource_attach_backing(g, res) != 0)
        goto fail;
    if (virtio_gpu_set_scanout(g, 0, res, 0, 0, VIRTIO_GPU_SMOKE_WIDTH,
                               VIRTIO_GPU_SMOKE_HEIGHT) != 0)
        goto fail;
    scanout_bound = 1;
    if (virtio_gpu_resource_transfer_2d(g, res, 0, 0,
                                        VIRTIO_GPU_SMOKE_WIDTH,
                                        VIRTIO_GPU_SMOKE_HEIGHT) != 0)
        goto fail;
    if (virtio_gpu_resource_flush(g, res, 0, 0, VIRTIO_GPU_SMOKE_WIDTH,
                                  VIRTIO_GPU_SMOKE_HEIGHT) != 0)
        goto fail;
    if (virtio_gpu_set_scanout(g, 0, NULL, 0, 0, 0, 0) != 0)
        goto fail;
    scanout_bound = 0;

    printf("virtio_gpu: resource smoke ok resource=%u size=%ux%u bytes=%u\n",
           res->id, VIRTIO_GPU_SMOKE_WIDTH, VIRTIO_GPU_SMOKE_HEIGHT,
           res->backing_len);
    return virtio_gpu_resource_unref(g, res);

fail:
    if (scanout_bound)
        virtio_gpu_set_scanout(g, 0, NULL, 0, 0, 0, 0);
    virtio_gpu_resource_unref(g, res);
    return -1;
}

static void virtio_gpu_fill_scanout_pattern(struct virtio_gpu_resource *res)
{
    uint32 *pixels = (uint32 *)res->backing;

    for (uint32 y = 0; y < res->height; y++) {
        for (uint32 x = 0; x < res->width; x++) {
            uint8 r = (x * 160) / (res->width ? res->width : 1);
            uint8 g = (y * 160) / (res->height ? res->height : 1);
            uint8 b = ((x ^ y) & 0x3f) + 0x30;
            pixels[y * res->width + x] =
                0xff000000u | ((uint32)r << 16) | ((uint32)g << 8) | b;
        }
    }
}

static int virtio_gpu_cmdline_video(uint32 *width, uint32 *height)
{
    char vbuf[32];
    uint32 w = 0;
    uint32 h = 0;
    const char *p;

    if (cmdline_get_param("video", vbuf, sizeof(vbuf)) != 0)
        return -1;

    p = vbuf;
    while (*p >= '0' && *p <= '9')
        w = w * 10 + (uint32)(*p++ - '0');
    if (*p != 'x' && *p != 'X')
        return -1;
    p++;
    while (*p >= '0' && *p <= '9')
        h = h * 10 + (uint32)(*p++ - '0');

    if (w < 640 || w > 2560 || h < 400 || h > 1600)
        return -1;
    *width = w;
    *height = h;
    return 0;
}

static int virtio_gpu_scanout_takeover_allowed(
    const struct virtio_pci_discovery *vd)
{
    uint8 base_class;
    uint8 subclass;

    if (virtio_gpu_cmdline_enabled("virtio_gpu_force_scanout"))
        return 1;
    if (virtio_gpu_cmdline_enabled("virtio_gpu_render_only"))
        return 0;
    if (vd == NULL)
        return 1;

    base_class = (uint8)((vd->class_code >> 16) & 0xff);
    subclass = (uint8)((vd->class_code >> 8) & 0xff);

    /*
     * QEMU's two-adapter fallback exposes Bochs as the visible VGA console and
     * virtio-gpu as a secondary display controller (PCI class 0x038000).  Keep
     * that virtio device render-only so /dev/fb0 continues to point at the
     * host-visible Bochs scanout.  A virtio-vga primary device reports
     * PCI class 0x030000 and may take over scanout.
     */
    return base_class == 0x03 && subclass == 0x00;
}

static int virtio_gpu_init_persistent_scanout(struct virtio_gpu *g)
{
    struct virtio_gpu_resource *res;
    uint32 width = g->scanout_width ? g->scanout_width : 640;
    uint32 height = g->scanout_height ? g->scanout_height : 480;
    uint32 reported_width = g->scanout_width;
    uint32 reported_height = g->scanout_height;
    uint32 fb_w = 0, fb_h = 0;
    uint32 video_w = 0, video_h = 0;
    int scanout_bound = 0;

    /*
     * Prefer an explicit kernel video= mode, then the firmware mode requested
     * by QEMU/OVMF, then the current virtio scanout size.  The reported
     * scanout can start as QEMU's default window geometry under GTK/GL, which
     * makes the desktop look tiny even though the boot framebuffer already
     * describes the intended guest resolution.
     */
    if (virtio_gpu_cmdline_video(&video_w, &video_h) == 0) {
        width = video_w;
        height = video_h;
        g->scanout_width = width;
        g->scanout_height = height;
        if (reported_width != width || reported_height != height) {
            printf("virtio_gpu: using cmdline video mode %ux%u for scanout (device reported %ux%u)\n",
                   width, height, reported_width, reported_height);
        }
    } else {
        fb_get_resolution(&fb_w, &fb_h);
        if (fb_w >= 640 && fb_h >= 400) {
            width = fb_w;
            height = fb_h;
            g->scanout_width = width;
            g->scanout_height = height;
            if (reported_width != width || reported_height != height) {
                printf("virtio_gpu: using fb0 mode %ux%u for scanout (device reported %ux%u)\n",
                       width, height, reported_width, reported_height);
            }
        } else if (reported_width >= 640 && reported_height >= 400) {
            width = reported_width;
            height = reported_height;
        }
    }

    if (g->scanout_width == 0 || g->scanout_height == 0) {
        printf("virtio_gpu: using default mode %ux%u for scanout fallback\n",
               width, height);
    }

    if (virtio_gpu_use_3d_scanout(g) &&
        virtio_gpu_resource_create_3d_backing(
            g, width, height, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
            VIRTIO_GPU_PIPE_BIND_RENDER_TARGET |
            VIRTIO_GPU_PIPE_BIND_SAMPLER_VIEW |
            VIRTIO_GPU_PIPE_BIND_DISPLAY_TARGET |
            VIRTIO_GPU_PIPE_BIND_SCANOUT |
            VIRTIO_GPU_PIPE_BIND_SHARED |
            VIRTIO_GPU_PIPE_BIND_LINEAR,
            &res) == 0) {
        printf("virtio_gpu: using Alpine-style virgl 3D scanout resource\n");
    } else if (virtio_gpu_resource_create_2d(
                   g, width, height, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                   &res) != 0) {
        return -1;
    }

    virtio_gpu_fill_scanout_pattern(res);

    if (virtio_gpu_resource_attach_backing(g, res) != 0)
        goto fail;
    if (virtio_gpu_set_scanout(g, 0, res, 0, 0, width, height) != 0)
        goto fail;
    scanout_bound = 1;
    g->bound_scanout_resource_id = res->id;
    if (virtio_gpu_resource_transfer_scanout(g, res, 0, 0, width, height) != 0)
        goto fail;
    if (virtio_gpu_resource_flush(g, res, 0, 0, width, height) != 0)
        goto fail;

    g->scanout_resource = res;
    printf("virtio_gpu: persistent scanout resource=%u kind=%s size=%ux%u bytes=%u alloc=%u\n",
           res->id, res->is_3d ? "3d" : "2d", width, height,
           res->backing_len, res->alloc_len);
    if (fb_init_virtio_gpu_scanout_backing(width, height, res->backing,
                                           res->backing_len,
                                           res->width * sizeof(uint32)) != 0)
        printf("virtio_gpu: warning: failed to expose scanout as /dev/fb0\n");
    return 0;

fail:
    if (scanout_bound)
        virtio_gpu_set_scanout(g, 0, NULL, 0, 0, 0, 0);
    virtio_gpu_resource_unref(g, res);
    return -1;
}

/*
 * Hardware cursor plane.
 *
 * virtio-gpu exposes a dedicated cursor virtqueue (queue index 1) for
 * UPDATE_CURSOR / MOVE_CURSOR.  These commands never touch the control queue
 * that carries scanout/flush/3D work, so moving the pointer does not
 * recomposite or serialise behind the compositor's present.  This mirrors the
 * Weston DRM-backend hardware cursor plane on the same host.
 *
 * Cursor-queue commands take no device response: a single device-readable
 * descriptor is posted and the device returns it to the used ring.  We keep a
 * small ring of command buffers and reclaim consumed descriptors lazily.
 */
static void virtio_gpu_notify_cursor(struct virtio_gpu *g)
{
    volatile uint16 *notify_addr = (volatile uint16 *)
        ((uint8 *)g->pci.notify_base +
         g->cursorq.notify_off * g->pci.notify_off_multiplier);
    *notify_addr = 1;
}

static void virtio_gpu_cursor_post(struct virtio_gpu *g,
                                   struct virtio_gpu_update_cursor *cmd)
{
    struct virtio_gpu_queue *q = &g->cursorq;
    struct virtio_gpu_update_cursor *slotcmd;
    static int drop_logs;
    uint16 outstanding;
    uint16 slot;
    uint16 next_used;
    int found = 0;

    if (!g->cursor_ready)
        return;

    int intena = spin_lock_irqsave(&q->lock);
    /* Reclaim descriptors the device has already consumed. */
    next_used = q->used->idx;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    while (q->used_idx != next_used) {
        uint16 used_slot = q->used->ring[q->used_idx % q->size].id;

        if (used_slot < g->cursor_ring)
            g->cursor_cmd_inflight[used_slot] = 0;
        q->used_idx++;
    }
    outstanding = (uint16)(q->avail->idx - q->used_idx);
    if (outstanding >= q->size) {
        /*
         * Cursor motion is naturally coalesced by position.  Dropping a late
         * command is much safer than reusing a command slot that QEMU has not
         * consumed yet, which can corrupt an in-flight UPDATE_CURSOR image.
         */
        if (drop_logs < 4) {
            printf("virtio_gpu: cursor queue full, dropping cmd type=0x%x outstanding=%u size=%u\n",
                   cmd->hdr.type, outstanding, q->size);
            drop_logs++;
        }
        spin_unlock_irqrestore(&q->lock, intena);
        return;
    }
    slot = g->cursor_cmd_idx % g->cursor_ring;
    for (uint16 i = 0; i < g->cursor_ring; i++) {
        uint16 candidate = (slot + i) % g->cursor_ring;

        if (!g->cursor_cmd_inflight[candidate]) {
            slot = candidate;
            found = 1;
            break;
        }
    }
    if (!found) {
        if (drop_logs < 4) {
            printf("virtio_gpu: cursor command slots busy, dropping cmd type=0x%x outstanding=%u size=%u\n",
                   cmd->hdr.type, outstanding, q->size);
            drop_logs++;
        }
        spin_unlock_irqrestore(&q->lock, intena);
        return;
    }
    slotcmd = &((struct virtio_gpu_update_cursor *)g->cursor_cmd_page)[slot];
    *slotcmd = *cmd;

    q->desc[slot].addr = (uint64)slotcmd;
    q->desc[slot].len = sizeof(*slotcmd);
    q->desc[slot].flags = 0; /* device-read-only, no response */
    q->desc[slot].next = 0;

    g->cursor_cmd_inflight[slot] = 1;
    q->avail->ring[q->avail->idx % q->size] = slot;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->avail->idx++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g->cursor_cmd_idx = slot + 1;
    virtio_gpu_notify_cursor(g);
    spin_unlock_irqrestore(&q->lock, intena);
}

static int virtio_gpu_cursor_queue_init(struct virtio_gpu *g)
{
    volatile struct virtio_pci_common_cfg *cfg = g->pci.common_cfg;
    struct virtio_gpu_queue *q = &g->cursorq;
    uint16 max;
    uint16 qsize;

    if (cfg->num_queues < 2) {
        printf("virtio_gpu: no cursor queue (num_queues=%u)\n",
               cfg->num_queues);
        return -1;
    }

    cfg->queue_select = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    max = cfg->queue_size;
    if (max == 0) {
        printf("virtio_gpu: cursor queue missing\n");
        return -1;
    }
    qsize = NUM;
    if (max < NUM)
        qsize = max;
    if (qsize < 2) {
        printf("virtio_gpu: cursor queue too small (%u)\n", qsize);
        return -1;
    }
    /* The command ring fits in one page; cap it so the cmd_page never
     * overflows even if the device advertised a large queue. */
    if (qsize > VIRTIO_GPU_CURSOR_RING)
        qsize = VIRTIO_GPU_CURSOR_RING;

    q->desc = kalloc();
    q->avail = kalloc();
    q->used = kalloc();
    g->cursor_cmd_page = kalloc();
    if (!q->desc || !q->avail || !q->used || !g->cursor_cmd_page) {
        if (q->desc) kfree(q->desc);
        if (q->avail) kfree(q->avail);
        if (q->used) kfree(q->used);
        if (g->cursor_cmd_page) kfree(g->cursor_cmd_page);
        q->desc = NULL;
        q->avail = NULL;
        q->used = NULL;
        g->cursor_cmd_page = NULL;
        printf("virtio_gpu: cursor queue kalloc failed\n");
        return -1;
    }
    memset(q->desc, 0, PGSIZE);
    memset(q->avail, 0, PGSIZE);
    memset(q->used, 0, PGSIZE);
    memset(g->cursor_cmd_page, 0, PGSIZE);

    cfg->queue_size = qsize;
    cfg->queue_desc = (uint64)q->desc;
    cfg->queue_driver = (uint64)q->avail;
    cfg->queue_device = (uint64)q->used;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    cfg->queue_enable = 1;
    q->size = qsize;
    q->used_idx = 0;
    q->notify_off = cfg->queue_notify_off;
    spin_init(&q->lock, "virtio_gpucur");
    g->cursor_ring = qsize;
    g->cursor_visible = 0;
    g->cursor_ready = 1;
    return 0;
}

static struct virtio_gpu_resource *
virtio_gpu_cursor_next_resource(struct virtio_gpu *g)
{
    struct virtio_gpu_resource *res;
    uint32 slot = g->cursor_resource_next % VIRTIO_GPU_CURSOR_IMAGE_SLOTS;

    res = g->cursor_resources[slot];
    if (res == NULL) {
        if (virtio_gpu_resource_create_2d(g, VIRTIO_GPU_CURSOR_DIM,
                                          VIRTIO_GPU_CURSOR_DIM,
                                          VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                                          &res) != 0 || res == NULL)
            return NULL;
        if (virtio_gpu_resource_attach_backing(g, res) != 0) {
            virtio_gpu_resource_unref(g, res);
            return NULL;
        }
        g->cursor_resources[slot] = res;
    }

    g->cursor_resource_next =
        (slot + 1) % VIRTIO_GPU_CURSOR_IMAGE_SLOTS;
    return res;
}

static uint8
virtio_gpu_cursor_unpremul(uint8 c, uint8 a)
{
    uint32 v;

    if (a == 0 || a == 255)
        return c;
    v = ((uint32)c * 255u + (uint32)a / 2u) / (uint32)a;
    return v > 255u ? 255u : (uint8)v;
}

/*
 * QEMU's GTK cursor path has historically interpreted virtio-gpu cursor
 * images as unpremultiplied RGBA even though the resource format is BGRA.
 * Keep the normal guest cursor BO format unchanged and only translate the
 * uploaded cursor resource when the launch path asks for this compatibility
 * mode.
 */
static uint32
virtio_gpu_cursor_qemu_gtk_pixel(uint32 pixel)
{
    uint8 a = (uint8)(pixel >> 24);
    uint8 r = (uint8)(pixel >> 16);
    uint8 g = (uint8)(pixel >> 8);
    uint8 b = (uint8)pixel;

    r = virtio_gpu_cursor_unpremul(r, a);
    g = virtio_gpu_cursor_unpremul(g, a);
    b = virtio_gpu_cursor_unpremul(b, a);

    return ((uint32)a << 24) | ((uint32)b << 16) |
           ((uint32)g << 8) | (uint32)r;
}

static uint64
virtio_gpu_cursor_alpha_nonzero_count(const uint32 *pixels, uint32 width,
                                      uint32 height)
{
    uint64 count = 0;

    for (uint32 row = 0; row < height; row++) {
        const uint32 *src = pixels + (uint64)row * width;

        for (uint32 col = 0; col < width; col++) {
            if ((src[col] >> 24) != 0)
                count++;
        }
    }
    return count;
}

static uint32
virtio_gpu_scale_cursor_abs(uint16 value, uint32 extent)
{
    if (extent <= 1)
        return 0;
    return (uint32)((((uint64)value * (uint64)(extent - 1)) + 32767u) /
                    65535u);
}

static void
virtio_gpu_fill_default_injected_cursor(uint32 *pixels)
{
    static const char *shape[VIRTIO_GPU_INJECTED_CURSOR_H] = {
        "#...............",
        "##..............",
        "#o#.............",
        "#oo#............",
        "#ooo#...........",
        "#oooo#..........",
        "#ooooo#.........",
        "#oooooo#........",
        "#ooooooo#.......",
        "#oooooooo#......",
        "#ooooo#####.....",
        "#oo#oo#.........",
        "#o#.#oo#........",
        "##..#oo#........",
        "#....#oo#.......",
        ".....#oo#.......",
        "......##........",
        "................",
        "................",
        "................",
        "................",
        "................",
        "................",
        "................",
    };

    memset(pixels, 0, VIRTIO_GPU_INJECTED_CURSOR_W *
           VIRTIO_GPU_INJECTED_CURSOR_H * sizeof(uint32));
    for (uint32 y = 0; y < VIRTIO_GPU_INJECTED_CURSOR_H; y++) {
        for (uint32 x = 0; x < VIRTIO_GPU_INJECTED_CURSOR_W; x++) {
            char c = shape[y][x];

            if (c == '#')
                pixels[y * VIRTIO_GPU_INJECTED_CURSOR_W + x] = 0xff000000u;
            else if (c == 'o')
                pixels[y * VIRTIO_GPU_INJECTED_CURSOR_W + x] = 0xffffffffu;
        }
    }
}

/*
 * Upload (or replace) the hardware cursor image.  pixels points to a kernel
 * buffer of width*height BGRA pixels (0xAARRGGBB) with width,height <= 64.
 * Rotate among a few resources so QEMU never sees us rewrite the image
 * resource currently bound to the hardware cursor plane.
 */
int virtio_gpu_user_set_cursor(const void *pixels, uint32 width,
                               uint32 height, uint32 hot_x, uint32 hot_y)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_update_cursor cmd;
    struct virtio_gpu_resource *res;
    uint32 *dst;
    uint64 fence_id;
    uint64 alpha_nonzero;
    int qemu_gtk_cursor_compat;
    int ret;

    if (!g->initialized || !g->cursor_ready || pixels == NULL)
        return -ENODEV;
    if (width == 0 || height == 0 ||
        width > VIRTIO_GPU_CURSOR_DIM || height > VIRTIO_GPU_CURSOR_DIM)
        return -EINVAL;

    if (virtio_gpu_cmdline_enabled("virtio_gpu_host_cursor_only")) {
        g->cursor_hot_x = hot_x;
        g->cursor_hot_y = hot_y;
        g->cursor_resource = NULL;
        g->cursor_resource_id = 0;
        g->cursor_visible = 0;
        if (virtio_gpu_cmdline_enabled("virtio_gpu_cursor_trace")) {
            printf("virtio_gpu: host-cursor-only suppress cursor upload "
                   "size=%ux%u hot=%u,%u\n",
                   width, height, hot_x, hot_y);
        }
        return 0;
    }

    alpha_nonzero = virtio_gpu_cursor_alpha_nonzero_count(
        (const uint32 *)pixels, width, height);
    if (alpha_nonzero == 0) {
        g->cursor_resource = NULL;
        g->cursor_resource_id = 0;
        g->cursor_hot_x = hot_x;
        g->cursor_hot_y = hot_y;
        ret = virtio_gpu_user_move_cursor(g->cursor_x, g->cursor_y, 0);
        if (virtio_gpu_cmdline_enabled("virtio_gpu_cursor_trace")) {
            printf("virtio_gpu: cursor upload hidden all-transparent "
                   "size=%ux%u hot=%u,%u ret=%d\n",
                   width, height, hot_x, hot_y, ret);
        }
        return ret;
    }

    mutex_lock(&g->op_lock);
    res = virtio_gpu_cursor_next_resource(g);
    if (res == NULL) {
        mutex_unlock(&g->op_lock);
        return -EIO;
    }

    /* Compose the image into the 64x64 backing (rest transparent). */
    dst = (uint32 *)res->backing;
    qemu_gtk_cursor_compat =
        virtio_gpu_cmdline_enabled("virtio_gpu_cursor_rgba_compat");
    memset(dst, 0,
           VIRTIO_GPU_CURSOR_DIM * VIRTIO_GPU_CURSOR_DIM * sizeof(uint32));
    for (uint32 row = 0; row < height; row++) {
        const uint32 *src = (const uint32 *)pixels + (uint64)row * width;
        uint32 *row_dst = &dst[row * VIRTIO_GPU_CURSOR_DIM];

        if (!qemu_gtk_cursor_compat) {
            memmove(row_dst, src, (uint64)width * sizeof(uint32));
            continue;
        }
        for (uint32 col = 0; col < width; col++)
            row_dst[col] = virtio_gpu_cursor_qemu_gtk_pixel(src[col]);
    }

    spin_lock(&g->lock);
    fence_id = ++g->next_fence_id;
    spin_unlock(&g->lock);
    if (virtio_gpu_resource_transfer_2d_fenced(g, res, 0, 0,
                                               VIRTIO_GPU_CURSOR_DIM,
                                               VIRTIO_GPU_CURSOR_DIM,
                                               fence_id) != 0) {
        mutex_unlock(&g->op_lock);
        return -EIO;
    }

    g->cursor_resource = res;
    g->cursor_resource_id = res->id;
    g->cursor_hot_x = hot_x;
    g->cursor_hot_y = hot_y;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_UPDATE_CURSOR;
    cmd.pos.scanout_id = 0;
    cmd.pos.x = (uint32)(g->cursor_x < 0 ? 0 : g->cursor_x);
    cmd.pos.y = (uint32)(g->cursor_y < 0 ? 0 : g->cursor_y);
    cmd.resource_id = res->id;
    cmd.hot_x = hot_x;
    cmd.hot_y = hot_y;
    g->cursor_visible = 1;
    virtio_gpu_cursor_post(g, &cmd);
    if (virtio_gpu_cmdline_enabled("virtio_gpu_cursor_trace")) {
        printf("virtio_gpu: cursor upload visible resource=%u size=%ux%u "
               "hot=%u,%u alpha_nonzero=%lu compat=%d\n",
               res->id, width, height, hot_x, hot_y, alpha_nonzero,
               qemu_gtk_cursor_compat);
    }
    mutex_unlock(&g->op_lock);
    return 0;
}

/*
 * Move (or show/hide) the hardware cursor.  Posts only to the cursor queue and
 * deliberately does NOT take the control-queue op_lock, so pointer motion is
 * never serialised behind a compositor present.
 */
int virtio_gpu_user_move_cursor(int32 x, int32 y, int visible)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_update_cursor cmd;

    if (!g->initialized || !g->cursor_ready)
        return -ENODEV;

    if (virtio_gpu_cmdline_enabled("virtio_gpu_host_cursor_only")) {
        g->cursor_x = x;
        g->cursor_y = y;
        g->cursor_visible = 0;
        if (virtio_gpu_cmdline_enabled("virtio_gpu_cursor_trace")) {
            printf("virtio_gpu: host-cursor-only suppress cursor move "
                   "x=%d y=%d visible=%d\n",
                   x, y, visible);
        }
        return 0;
    }

    if (visible && g->cursor_resource_id == 0) {
        g->cursor_x = x;
        g->cursor_y = y;
        g->cursor_visible = 0;
        return 0;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.pos.scanout_id = 0;
    cmd.pos.x = (uint32)(x < 0 ? 0 : x);
    cmd.pos.y = (uint32)(y < 0 ? 0 : y);
    if (!visible) {
        /* resource_id 0 hides the cursor. */
        cmd.hdr.type = VIRTIO_GPU_CMD_UPDATE_CURSOR;
        cmd.resource_id = 0;
    } else if (!g->cursor_visible && g->cursor_resource_id) {
        /* Re-show after a hide: UPDATE_CURSOR rebinds the image resource. */
        cmd.hdr.type = VIRTIO_GPU_CMD_UPDATE_CURSOR;
        cmd.resource_id = g->cursor_resource_id;
        cmd.hot_x = g->cursor_hot_x;
        cmd.hot_y = g->cursor_hot_y;
    } else {
        cmd.hdr.type = VIRTIO_GPU_CMD_MOVE_CURSOR;
    }
    g->cursor_x = x;
    g->cursor_y = y;
    g->cursor_visible = visible ? 1 : 0;
    virtio_gpu_cursor_post(g, &cmd);
    return 0;
}

/*
 * Mirror synthetic absolute /dev/mouse injection into the active hardware
 * cursor.  This preserves the single-cursor contract:
 *  - guest hardware cursor mode moves the existing cursor image;
 *  - before a compositor uploads one, a small built-in arrow is installed;
 *  - host-cursor-only mode stays a no-op because QEMU's frontend cursor is
 *    the only visible cursor and harnesses must move it from the host side.
 */
int virtio_gpu_user_move_injected_cursor(uint16 x_abs, uint16 y_abs,
                                         int visible)
{
    struct virtio_gpu *g = &gpu;
    uint32 pixels[VIRTIO_GPU_INJECTED_CURSOR_W *
                  VIRTIO_GPU_INJECTED_CURSOR_H];
    uint32 scanout_w;
    uint32 scanout_h;
    char value[8];
    uint32 x;
    uint32 y;
    int installed = 0;
    int ret;

    if (cmdline_get_param("virtio_gpu_injected_cursor", value,
                          sizeof(value)) == 0 && value[0] == '0')
        return 0;
    if (!g->initialized || !g->cursor_ready)
        return -ENODEV;
    if (virtio_gpu_cmdline_enabled("virtio_gpu_host_cursor_only"))
        return 0;

    scanout_w = g->scanout_width ? g->scanout_width : 640;
    scanout_h = g->scanout_height ? g->scanout_height : 480;
    x = virtio_gpu_scale_cursor_abs(x_abs, scanout_w);
    y = virtio_gpu_scale_cursor_abs(y_abs, scanout_h);

    if (visible && g->cursor_resource_id == 0) {
        virtio_gpu_fill_default_injected_cursor(pixels);
        ret = virtio_gpu_user_set_cursor(pixels,
                                         VIRTIO_GPU_INJECTED_CURSOR_W,
                                         VIRTIO_GPU_INJECTED_CURSOR_H,
                                         0, 0);
        if (ret != 0)
            return ret;
        installed = 1;
    }

    ret = virtio_gpu_user_move_cursor((int32)x, (int32)y, visible);
    if (virtio_gpu_cmdline_enabled("virtio_gpu_injected_cursor_trace")) {
        printf("virtio_gpu: injected cursor x_abs=%u y_abs=%u x=%u y=%u "
               "visible=%d installed_default=%d ret=%d\n",
               x_abs, y_abs, x, y, visible, installed, ret);
    }
    return ret;
}

static int virtio_gpu_queue_init(struct virtio_gpu *g)
{
    volatile struct virtio_pci_common_cfg *cfg = g->pci.common_cfg;
    struct virtio_gpu_queue *q = &g->ctrlq;

    printf("virtio_gpu: queue init begin\n");
    cfg->queue_select = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    uint16 max = cfg->queue_size;
    if (max == 0) {
        printf("virtio_gpu: control queue missing\n");
        return -1;
    }
    uint16 qsize = NUM;
    if (max < NUM) {
        qsize = max;
        printf("virtio_gpu: control queue max=%d, using %d descriptor slots\n",
               max, max);
    }

    q->desc = kalloc();
    q->avail = kalloc();
    q->used = kalloc();
    g->cmd_page = kalloc();
    g->resp_page = kalloc();
    g->data_page = kalloc();
    if (!q->desc || !q->avail || !q->used || !g->cmd_page || !g->resp_page ||
        !g->data_page)
        panic("virtio_gpu: kalloc");

    memset(q->desc, 0, PGSIZE);
    memset(q->avail, 0, PGSIZE);
    memset(q->used, 0, PGSIZE);
    memset(g->cmd_page, 0, PGSIZE);
    memset(g->resp_page, 0, PGSIZE);
    memset(g->data_page, 0, PGSIZE);

    cfg->queue_size = qsize;
    cfg->queue_desc = (uint64)q->desc;
    cfg->queue_driver = (uint64)q->avail;
    cfg->queue_device = (uint64)q->used;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    cfg->queue_enable = 1;
    q->size = qsize;
    q->notify_off = cfg->queue_notify_off;
    spin_init(&q->lock, "virtio_gpuq");
    /*
     * Async slot budget (review finding #7): slot i owns descriptors
     * [8 + i*4, 8 + i*4 + 4), so the negotiated table must cover
     * 8 + capacity*4 entries.  Clamp the usable slot count to what the
     * device actually gave us; capacity 0 disables the async ring
     * (async posts fail fast and callers use the sync path/error
     * handling).  No async post may ever reference an out-of-range
     * descriptor.
     */
    if (qsize > VIRTIO_GPU_ASYNC_DESC_BASE)
        g->async_capacity =
            (int)((qsize - VIRTIO_GPU_ASYNC_DESC_BASE) /
                  VIRTIO_GPU_ASYNC_DESC_PER_SLOT);
    else
        g->async_capacity = 0;
    if (g->async_capacity > VIRTIO_GPU_ASYNC_MAX_DEPTH)
        g->async_capacity = VIRTIO_GPU_ASYNC_MAX_DEPTH;
    if (g->async_capacity < VIRTIO_GPU_ASYNC_MAX_DEPTH)
        printf("virtio_gpu: async ring clamped to %d slots (queue size %u < %u)\n",
               g->async_capacity, qsize,
               VIRTIO_GPU_ASYNC_DESC_BASE +
                   VIRTIO_GPU_ASYNC_MAX_DEPTH *
                       VIRTIO_GPU_ASYNC_DESC_PER_SLOT);
    printf("virtio_gpu: queue init done size=%u notify_off=%u\n",
           q->size, q->notify_off);
    return 0;
}

void virtio_gpu_init(void)
{
    struct virtio_pci_discovery *vd = pci_get_virtio_gpu(0);
    struct virtio_gpu *g = &gpu;

    if (!vd || !vd->found)
        return;
    if (g->initialized)
        return;

    printf("virtio_gpu: init begin\n");
    if (!vd->common_cfg_cap || !vd->notify_cfg_cap || !vd->isr_cfg_cap ||
        !vd->device_cfg_cap) {
        printf("virtio_gpu: missing PCI capability, skipping driver init\n");
        return;
    }

    uint8 ccap = vd->common_cfg_cap;
    uint8 ncap = vd->notify_cfg_cap;
    uint8 icap = vd->isr_cfg_cap;
    uint8 dcap = vd->device_cfg_cap;

    uint8 cc_bar = pci_config_read8(vd->bus, vd->dev, vd->func, ccap + 4);
    uint32 cc_off = pci_config_read32(vd->bus, vd->dev, vd->func, ccap + 8);
    uint32 cc_len = pci_config_read32(vd->bus, vd->dev, vd->func, ccap + 12);
    uint8 n_bar = pci_config_read8(vd->bus, vd->dev, vd->func, ncap + 4);
    uint32 n_off = pci_config_read32(vd->bus, vd->dev, vd->func, ncap + 8);
    uint32 n_len = pci_config_read32(vd->bus, vd->dev, vd->func, ncap + 12);
    uint32 n_mult = pci_config_read32(vd->bus, vd->dev, vd->func, ncap + 16);
    uint8 i_bar = pci_config_read8(vd->bus, vd->dev, vd->func, icap + 4);
    uint32 i_off = pci_config_read32(vd->bus, vd->dev, vd->func, icap + 8);
    uint32 i_len = pci_config_read32(vd->bus, vd->dev, vd->func, icap + 12);
    uint8 d_bar = pci_config_read8(vd->bus, vd->dev, vd->func, dcap + 4);
    uint32 d_off = pci_config_read32(vd->bus, vd->dev, vd->func, dcap + 8);
    uint32 d_len = pci_config_read32(vd->bus, vd->dev, vd->func, dcap + 12);

    uint64 cfg_va = virtio_gpu_map_mmio_window(
        virtio_gpu_bar_base(vd, cc_bar), cc_off, cc_len);
    uint64 notify_va = virtio_gpu_map_mmio_window(
        virtio_gpu_bar_base(vd, n_bar), n_off, n_len);
    uint64 isr_va = virtio_gpu_map_mmio_window(
        virtio_gpu_bar_base(vd, i_bar), i_off, i_len);
    uint64 dev_cfg_va = virtio_gpu_map_mmio_window(
        virtio_gpu_bar_base(vd, d_bar), d_off, d_len);

    memset(g, 0, sizeof(*g));
    spin_init(&g->lock, "virtio_gpu");
    mutex_init(&g->op_lock, "virtio_gpuop");
    mutex_init(&g->async_wait_serialize, "virtio_gpuaw");
    mutex_init(&g->async_reap_serialize, "virtio_gpurp");
    /*
     * One-time init of the shared progress completion (this kernel's
     * completion_t embeds a spinlock and a tq that REQUIRE init).
     * virtio_gpu_async_wait_progress only completion_reinit()s it from
     * here on — a full re-init under a sleeping waiter corrupts the tq.
     */
    completion_init(&g->async_wait);
    g->next_resource_id = 1;
    g->next_context_id = 2;
    g->pci.use_pci = 1;
    g->pci.common_cfg = (volatile struct virtio_pci_common_cfg *)cfg_va;
    g->pci.notify_base = (volatile uint16 *)notify_va;
    g->pci.notify_off_multiplier = n_mult;
    g->pci.isr = (volatile uint8 *)isr_va;
    g->config = (volatile struct virtio_gpu_config *)dev_cfg_va;
    g->next_fence_id = 1;
    if (vd->shared_memory_cfg_cap != 0 &&
        vd->shared_memory_id == VIRTIO_GPU_SHM_ID_HOST_VISIBLE &&
        vd->shared_memory_bar < 6 &&
        vd->shared_memory_bar_base != 0 &&
        vd->shared_memory_length != 0 &&
        vd->shared_memory_length <= UINT32_MAX &&
        vd->shared_memory_offset + vd->shared_memory_length >=
            vd->shared_memory_offset) {
        g->host_visible_cap_present = 1;
        g->host_visible_pa =
            vd->shared_memory_bar_base + vd->shared_memory_offset;
        g->host_visible_length = vd->shared_memory_length;
        g->host_visible_va = virtio_gpu_map_mmio_window(
            vd->shared_memory_bar_base, (uint32)vd->shared_memory_offset,
            (uint32)vd->shared_memory_length);
        g->host_visible_next_offset = 0;
        printf("virtio_gpu: host visible shm id=%u bar=%u pa=0x%lx va=0x%lx len=0x%lx\n",
               vd->shared_memory_id, vd->shared_memory_bar,
               g->host_visible_pa, g->host_visible_va,
               g->host_visible_length);
    } else if (vd->shared_memory_cfg_cap != 0) {
        printf("virtio_gpu: host visible shm unusable id=%u bar=%u base=0x%lx off=0x%lx len=0x%lx\n",
               vd->shared_memory_id, vd->shared_memory_bar,
               vd->shared_memory_bar_base, vd->shared_memory_offset,
               vd->shared_memory_length);
    }

    volatile struct virtio_pci_common_cfg *cfg = g->pci.common_cfg;

    printf("virtio_gpu: reset device\n");
    cfg->device_status = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (int i = 0; cfg->device_status != 0 && i < 1000000; i++)
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (cfg->device_status != 0) {
        printf("virtio_gpu: reset timed out status=0x%x\n",
               cfg->device_status);
        return;
    }

    printf("virtio_gpu: negotiate features\n");
    uint8 status = VIRTIO_CONFIG_S_ACKNOWLEDGE;
    cfg->device_status = status;
    status |= VIRTIO_CONFIG_S_DRIVER;
    cfg->device_status = status;

    cfg->device_feature_select = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32 features0 = cfg->device_feature;
    cfg->device_feature_select = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32 features1 = cfg->device_feature;
    uint32 driver_features0 = 0;
    uint32 driver_features1 = 0;
    if (features0 & (1u << VIRTIO_GPU_F_VIRGL))
        driver_features0 |= (1u << VIRTIO_GPU_F_VIRGL);
    if (features0 & (1u << VIRTIO_GPU_F_EDID))
        driver_features0 |= (1u << VIRTIO_GPU_F_EDID);
    if (features0 & (1u << VIRTIO_GPU_F_RESOURCE_BLOB))
        driver_features0 |= (1u << VIRTIO_GPU_F_RESOURCE_BLOB);
    if (features0 & (1u << VIRTIO_GPU_F_CONTEXT_INIT))
        driver_features0 |= (1u << VIRTIO_GPU_F_CONTEXT_INIT);
    if (features1 & (1u << (VIRTIO_F_VERSION_1 - 32)))
        driver_features1 |= (1u << (VIRTIO_F_VERSION_1 - 32));
    g->features0 = features0;
    g->features1 = features1;
    g->driver_features0 = driver_features0;
    g->driver_features1 = driver_features1;

    cfg->driver_feature_select = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    cfg->driver_feature = driver_features0;
    cfg->driver_feature_select = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    cfg->driver_feature = driver_features1;

    status |= VIRTIO_CONFIG_S_FEATURES_OK;
    cfg->device_status = status;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!(cfg->device_status & VIRTIO_CONFIG_S_FEATURES_OK)) {
        printf("virtio_gpu: device rejected feature set\n");
        cfg->device_status = 0;
        return;
    }

    printf("virtio_gpu: setup queue\n");
    if (virtio_gpu_queue_init(g) != 0) {
        cfg->device_status = 0;
        return;
    }
    /* Optional dedicated cursor queue; failure leaves cursor_ready=0 and the
     * compositor falls back to the software cursor. */
    (void)virtio_gpu_cursor_queue_init(g);

    printf("virtio_gpu: driver ok\n");
    status |= VIRTIO_CONFIG_S_DRIVER_OK;
    cfg->device_status = status;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g->initialized = 1;

    /* IRQ-driven retire: must exist before the first interrupt fires. */
    virtio_gpu_reap_wq_init(g);

    struct irq_desc virtio_gpu_irq_desc = {
        .handler = virtio_gpu_intr,
        .data = g,
        .dev = NULL,
    };
    int irq_ret = register_irq_handler(PLIC_IRQ(vd->irq_line),
                                       &virtio_gpu_irq_desc);
    if (irq_ret == 0) {
        extern void plic_enable_irq_level(int irq);
        plic_enable_irq_level(vd->irq_line);
    } else {
        printf("virtio_gpu: WARNING: IRQ %d registration failed (%d), polling fallback only\n",
               vd->irq_line, irq_ret);
    }

    printf("virtio_gpu: initialized queues=%u features0=0x%x features1=0x%x driver_features0=0x%x driver_features1=0x%x scanouts=%u capsets=%u irq=%d\n",
           cfg->num_queues, features0, features1, driver_features0,
           driver_features1,
           g->config->num_scanouts, g->config->num_capsets, vd->irq_line);

    virtio_gpu_query_capsets(g);
    virtio_gpu_smoke_context(g);
    virtio_gpu_smoke_host_visible_map(g);
    virtio_gpu_submit_display_info(g);
    virtio_gpu_get_edid_mode(g, NULL, NULL, NULL, 1);
    if (virtio_gpu_scanout_takeover_allowed(vd)) {
        virtio_gpu_smoke_resource(g);
        virtio_gpu_init_persistent_scanout(g);
    } else {
        printf("virtio_gpu: keeping secondary display controller render-only class=0x%lx\n",
               (uint64)vd->class_code);
    }
    if (virtio_gpu_has_virgl())
        fb_gpu_register_render_node();
}

void virtio_gpu_get_fb_stats(struct fb_gpu_stats *stats)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_stats vg_stats;
    uint64 async_pending;
    uint64 async_depth;
    uint64 hot_shape_owners = 0;
    uint64 hot_shape_submit_calls = 0;
    uint64 hot_shape_make_room_calls = 0;
    uint64 hot_shape_make_room_stalls = 0;
    uint64 hot_shape_make_room_wait_ticks = 0;
    uint64 hot_shape_make_room_max_wait_ticks = 0;
    uint64 hot_shape_make_room_depth_max = 0;
    uint64 hot_shape_make_room_count_max = 0;
    uint64 hot_shape_make_room_wait_count_max = 0;
    uint64 hot_shape_posted = 0;
    uint64 hot_shape_post_count_max = 0;
    uint64 hot_shape_retired = 0;
    uint64 hot_shape_retire_ticks = 0;
    uint64 hot_shape_retire_max_ticks = 0;
    uint64 hot_shape_failures = 0;
    uint64 hot_shape_mixed = 0;

    if (!g->initialized)
        return;

    spin_lock(&g->lock);
    vg_stats = g->stats;
    /* async_count is q->lock-protected; lock-free stats snapshot. */
    async_pending = __atomic_load_n(&g->async_count, __ATOMIC_ACQUIRE);
    async_depth = g->async_depth ? (uint64)g->async_depth : 2;
    for (int i = 0; i < VIRTIO_GPU_SUBMIT_TRACE_OWNER_SLOTS; i++) {
        const struct virtio_gpu_submit_trace_owner *o =
            &g->submit_trace_owners[i];
        const struct virtio_gpu_submit_trace_owner_counts *c = &o->total;

        if (!o->in_use || !c->shape.valid)
            continue;
        hot_shape_owners++;
        hot_shape_submit_calls += c->shape_submit_calls;
        hot_shape_make_room_calls += c->shape_make_room_calls;
        hot_shape_make_room_stalls += c->shape_make_room_stalls;
        hot_shape_make_room_wait_ticks += c->shape_make_room_wait_ticks;
        if (c->shape_make_room_max_wait_ticks >
            hot_shape_make_room_max_wait_ticks)
            hot_shape_make_room_max_wait_ticks =
                c->shape_make_room_max_wait_ticks;
        if (c->shape_make_room_depth_max > hot_shape_make_room_depth_max)
            hot_shape_make_room_depth_max = c->shape_make_room_depth_max;
        if (c->shape_make_room_count_max > hot_shape_make_room_count_max)
            hot_shape_make_room_count_max = c->shape_make_room_count_max;
        if (c->shape_make_room_wait_count_max >
            hot_shape_make_room_wait_count_max)
            hot_shape_make_room_wait_count_max =
                c->shape_make_room_wait_count_max;
        hot_shape_posted += c->shape_posted;
        if (c->shape_post_count_max > hot_shape_post_count_max)
            hot_shape_post_count_max = c->shape_post_count_max;
        hot_shape_retired += c->shape_retired;
        hot_shape_retire_ticks += c->shape_retire_ticks;
        if (c->shape_retire_max_ticks > hot_shape_retire_max_ticks)
            hot_shape_retire_max_ticks = c->shape_retire_max_ticks;
        hot_shape_failures += c->shape_failures;
        hot_shape_mixed += c->shape_mixed;
    }
    spin_unlock(&g->lock);

    stats->virtio_commands = vg_stats.commands;
    stats->virtio_failures = vg_stats.failures;
    stats->virtio_timeouts = vg_stats.timeouts;
    stats->virtio_resources = vg_stats.resources;
    stats->virtio_resource_bytes = vg_stats.resource_bytes;
    stats->virtio_transfers = vg_stats.transfers;
    stats->virtio_flushes = vg_stats.flushes;
    stats->virtio_scanouts = vg_stats.scanouts;
    stats->virtio_capsets = vg_stats.capsets;
    stats->virtio_virgl = vg_stats.virgl;
    stats->virtio_virgl_version = vg_stats.virgl_version;
    stats->virtio_virgl_size = vg_stats.virgl_size;
    stats->virtio_contexts = vg_stats.contexts;
    stats->virtio_context_failed = vg_stats.context_failed;
    stats->virtio_context_failures = vg_stats.context_failures;
    stats->virtio_submits = vg_stats.submits;
    stats->virtio_fences = vg_stats.fences;
    stats->virtio_last_fence = vg_stats.last_fence;
    stats->virtio_irq_completions = vg_stats.irq_completions;
    stats->virtio_poll_fallbacks = vg_stats.poll_fallbacks;
    stats->virtio_async_posted = vg_stats.async_posted;
    stats->virtio_async_posted_submit_3d = vg_stats.async_posted_submit_3d;
    stats->virtio_async_posted_flush = vg_stats.async_posted_flush;
    stats->virtio_async_posted_transfer = vg_stats.async_posted_transfer;
    stats->virtio_async_retired = vg_stats.async_retired;
    stats->virtio_async_pending = async_pending;
    stats->virtio_async_depth = async_depth;
    stats->virtio_async_make_room_calls = vg_stats.async_make_room_calls;
    stats->virtio_async_make_room_submit_3d_calls =
        vg_stats.async_make_room_submit_3d_calls;
    stats->virtio_async_make_room_flush_calls =
        vg_stats.async_make_room_flush_calls;
    stats->virtio_async_make_room_transfer_calls =
        vg_stats.async_make_room_transfer_calls;
    stats->virtio_async_make_room_stalls = vg_stats.async_make_room_stalls;
    stats->virtio_async_make_room_submit_3d_stalls =
        vg_stats.async_make_room_submit_3d_stalls;
    stats->virtio_async_make_room_flush_stalls =
        vg_stats.async_make_room_flush_stalls;
    stats->virtio_async_make_room_transfer_stalls =
        vg_stats.async_make_room_transfer_stalls;
    stats->virtio_async_wait_progress_calls =
        vg_stats.async_wait_progress_calls;
    stats->virtio_async_make_room_wait_ticks =
        vg_stats.async_make_room_wait_ticks;
    stats->virtio_async_make_room_last_wait_us =
        vg_stats.async_make_room_last_wait_us;
    stats->virtio_async_make_room_max_wait_us =
        vg_stats.async_make_room_max_wait_us;
    stats->virtio_present_copy_calls = vg_stats.present_copy_calls;
    stats->virtio_present_copy_drain_calls =
        vg_stats.present_copy_drain_calls;
    stats->virtio_present_copy_src_fence_drains =
        vg_stats.present_copy_src_fence_drains;
    stats->virtio_present_copy_blanket_drains =
        vg_stats.present_copy_blanket_drains;
    stats->virtio_present_copy_src_fence_only_drains =
        vg_stats.present_copy_src_fence_only_drains;
    stats->virtio_present_copy_blanket_only_drains =
        vg_stats.present_copy_blanket_only_drains;
    stats->virtio_present_copy_src_fence_blanket_drains =
        vg_stats.present_copy_src_fence_blanket_drains;
    stats->virtio_present_copy_no_drain_skips =
        vg_stats.present_copy_no_drain_skips;
    stats->virtio_present_copy_minimal_skips =
        vg_stats.present_copy_minimal_skips;
    stats->virtio_present_copy_drain_failures =
        vg_stats.present_copy_drain_failures;
    stats->virtio_present_copy_drain_ticks =
        vg_stats.present_copy_drain_ticks;
    stats->virtio_present_copy_drain_last_us =
        vg_stats.present_copy_drain_last_us;
    stats->virtio_present_copy_drain_max_us =
        vg_stats.present_copy_drain_max_us;
    stats->virtio_hot_shape_owners = hot_shape_owners;
    stats->virtio_hot_shape_owner_drops =
        vg_stats.submit_trace_owner_drops;
    stats->virtio_hot_shape_submit_calls = hot_shape_submit_calls;
    stats->virtio_hot_shape_make_room_calls = hot_shape_make_room_calls;
    stats->virtio_hot_shape_make_room_stalls = hot_shape_make_room_stalls;
    stats->virtio_hot_shape_make_room_wait_us =
        virtio_gpu_ticks_to_us(hot_shape_make_room_wait_ticks);
    stats->virtio_hot_shape_make_room_max_wait_us =
        virtio_gpu_ticks_to_us(hot_shape_make_room_max_wait_ticks);
    stats->virtio_hot_shape_make_room_depth_max =
        hot_shape_make_room_depth_max;
    stats->virtio_hot_shape_make_room_count_max =
        hot_shape_make_room_count_max;
    stats->virtio_hot_shape_make_room_wait_count_max =
        hot_shape_make_room_wait_count_max;
    stats->virtio_hot_shape_posted = hot_shape_posted;
    stats->virtio_hot_shape_post_count_max = hot_shape_post_count_max;
    stats->virtio_hot_shape_retired = hot_shape_retired;
    stats->virtio_hot_shape_retire_us =
        virtio_gpu_ticks_to_us(hot_shape_retire_ticks);
    stats->virtio_hot_shape_retire_max_us =
        virtio_gpu_ticks_to_us(hot_shape_retire_max_ticks);
    stats->virtio_hot_shape_failures = hot_shape_failures;
    stats->virtio_hot_shape_mixed = hot_shape_mixed;
}

int virtio_gpu_has_virgl(void)
{
    struct virtio_gpu *g = &gpu;

    return g->initialized && g->virgl_capset_id &&
        (g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL));
}

int virtio_gpu_has_context_init(void)
{
    struct virtio_gpu *g = &gpu;

    return virtio_gpu_has_virgl() &&
        (g->driver_features0 & (1u << VIRTIO_GPU_F_CONTEXT_INIT));
}

int virtio_gpu_has_resource_blob(void)
{
    struct virtio_gpu *g = &gpu;

    return g->initialized &&
        (g->driver_features0 & (1u << VIRTIO_GPU_F_RESOURCE_BLOB));
}

int virtio_gpu_has_host_visible(void)
{
    struct virtio_gpu *g = &gpu;

    return g->initialized && virtio_gpu_host_visible_ready(g);
}

int virtio_gpu_probe_scanout(uint32 *width, uint32 *height)
{
    struct virtio_gpu *g = &gpu;
    int ret;

    if (width != NULL)
        *width = 0;
    if (height != NULL)
        *height = 0;
    if (!g->initialized)
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_PROBE);
    ret = virtio_gpu_get_display_info(g, width, height, 0);
    virtio_gpu_op_unlock(g);
    return ret == 0 ? 0 : -EIO;
}

int virtio_gpu_probe_edid_mode(uint32 *width, uint32 *height,
                               uint32 *refresh_millihz)
{
    struct virtio_gpu *g = &gpu;
    int ret;

    if (width != NULL)
        *width = 0;
    if (height != NULL)
        *height = 0;
    if (refresh_millihz != NULL)
        *refresh_millihz = 0;
    if (!g->initialized)
        return -ENODEV;
    if (g->edid_width != 0 && g->edid_height != 0) {
        if (width != NULL)
            *width = g->edid_width;
        if (height != NULL)
            *height = g->edid_height;
        if (refresh_millihz != NULL)
            *refresh_millihz = g->edid_refresh_millihz;
        return 0;
    }

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_PROBE);
    ret = virtio_gpu_get_edid_mode(g, width, height, refresh_millihz, 0);
    if (ret != 0 && g->edid_width != 0 && g->edid_height != 0) {
        if (width != NULL)
            *width = g->edid_width;
        if (height != NULL)
            *height = g->edid_height;
        if (refresh_millihz != NULL)
            *refresh_millihz = g->edid_refresh_millihz;
        ret = 0;
    }
    virtio_gpu_op_unlock(g);
    return ret == 0 ? 0 : -EIO;
}

int virtio_gpu_resize_scanout(uint32 width, uint32 height)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;
    struct virtio_gpu_resource *old;
    int ret = -EIO;
    int bound = 0;

    if (width < 640 || width > 2560 || height < 400 || height > 1600)
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_RESIZE);
    old = g->scanout_resource;
    if (old != NULL && old->width == width && old->height == height) {
        ret = 0;
        goto out;
    }

    if (virtio_gpu_use_3d_scanout(g) &&
        virtio_gpu_resource_create_3d_backing(
            g, width, height, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
            VIRTIO_GPU_PIPE_BIND_RENDER_TARGET |
            VIRTIO_GPU_PIPE_BIND_SAMPLER_VIEW |
            VIRTIO_GPU_PIPE_BIND_DISPLAY_TARGET |
            VIRTIO_GPU_PIPE_BIND_SCANOUT |
            VIRTIO_GPU_PIPE_BIND_SHARED |
            VIRTIO_GPU_PIPE_BIND_LINEAR,
            &res) == 0) {
        printf("virtio_gpu: using Alpine-style virgl 3D scanout resource for mode set\n");
    } else if (virtio_gpu_resource_create_2d(
                   g, width, height, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                   &res) != 0) {
        goto out;
    }
    memset(res->backing, 0, res->backing_len);

    if (virtio_gpu_resource_attach_backing(g, res) != 0)
        goto fail_new;
    if (virtio_gpu_set_scanout(g, 0, res, 0, 0, width, height) != 0)
        goto fail_new;
    bound = 1;
    g->bound_scanout_resource_id = res->id;
    if (virtio_gpu_resource_transfer_scanout(g, res, 0, 0, width, height) != 0)
        goto fail_new;
    if (virtio_gpu_resource_flush(g, res, 0, 0, width, height) != 0)
        goto fail_new;

    g->scanout_resource = res;
    g->scanout_width = width;
    g->scanout_height = height;
    virtio_gpu_drop_present_flip_resources(g);
    virtio_gpu_page_flip_scanout_set_reset(g);
    g->present_scanout_ctx_id = 0;
    g->present_scanout_resource_id = 0;
    g->bound_scanout_resource_id = res->id;
    ret = fb_replace_virtio_gpu_scanout_backing(width, height, res->backing,
                                                res->backing_len,
                                                res->width * sizeof(uint32));
    if (ret != 0) {
        g->scanout_resource = old;
        goto fail_new;
    }

    /*
     * Existing userspace may still have the old direct scanout mmap.  Keep the
     * old resource alive instead of freeing pages that might still be mapped;
     * the compositor remaps the current scanout after FBIOPUT succeeds.
     */
    ret = 0;
    goto out;

fail_new:
    if (bound) {
        if (old != NULL) {
            virtio_gpu_set_scanout(g, 0, old, 0, 0, old->width, old->height);
            g->bound_scanout_resource_id = old->id;
        } else {
            virtio_gpu_set_scanout(g, 0, NULL, 0, 0, 0, 0);
            g->bound_scanout_resource_id = 0;
        }
    }
    virtio_gpu_resource_unref(g, res);
out:
    virtio_gpu_op_unlock(g);
    return ret;
}

int virtio_gpu_bind_resource_scanout(uint32 resource_id, uint32 x, uint32 y,
                                     uint32 w, uint32 h)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;
    uint32 scanout_w;
    uint32 scanout_h;
    int already_bound = 0;
    int full_size_resource;
    int ret = -EIO;
    int scanout_perf;
    int scanout_path = 0;
    int prepared_async_flush = 0;
    int wait_holder = VIRTIO_GPU_OP_NONE;
    struct virtio_gpu_async_submit scanout_flush_prep;
    uint64 total_start = 0;
    uint64 lock_acquired = 0;
    uint64 set_scanout_start;
    uint64 set_scanout_ticks = 0;
    uint64 flush_start;
    uint64 flush_ticks = 0;

    if (resource_id == 0 || w == 0 || h == 0)
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    memset(&scanout_flush_prep, 0, sizeof(scanout_flush_prep));
    if (virtio_gpu_async_scanout_flush_enabled() &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_no_scanout_flush") &&
        __atomic_load_n(&g->bound_scanout_resource_id,
                        __ATOMIC_RELAXED) == resource_id &&
        virtio_gpu_resource_flush_async_prepare(resource_id, x, y, w, h,
                                                &scanout_flush_prep) == 0)
        prepared_async_flush = 1;

    scanout_perf = virtio_gpu_cmdline_enabled("virtio_gpu_scanout_perf");
    if (scanout_perf)
        total_start = r_time();
    if (scanout_perf && mutex_trylock(&g->op_lock)) {
        wait_holder = VIRTIO_GPU_OP_NONE;
    } else {
        if (scanout_perf)
            wait_holder = __atomic_load_n(&g->op_lock_holder,
                                          __ATOMIC_RELAXED);
        mutex_lock(&g->op_lock);
    }
    __atomic_store_n(&g->op_lock_holder, VIRTIO_GPU_OP_SCANOUT,
                     __ATOMIC_RELAXED);
    if (scanout_perf)
        lock_acquired = r_time();
    spin_lock(&g->lock);
    res = virtio_gpu_lookup_resource_locked(g, resource_id);
    if (res == NULL || !res->attached ||
        x > res->width || w > res->width - x ||
        y > res->height || h > res->height - y) {
        spin_unlock(&g->lock);
        ret = -EINVAL;
        goto out;
    }
    spin_unlock(&g->lock);

    /*
     * SET_SCANOUT changes the host-visible mode to the supplied rectangle.
     * Linux's fast path is safe when KMS page-flips a full-size framebuffer;
     * binding a window-sized virgl resource here makes QEMU resize between the
     * desktop and the client window.  Keep partial scanout behind an explicit
     * diagnostic flag so normal windowed compositing cannot trigger that jump.
     */
    scanout_w = g->scanout_width;
    scanout_h = g->scanout_height;
    already_bound = g->bound_scanout_resource_id == res->id;
    full_size_resource = res->width == scanout_w && res->height == scanout_h;
    if (already_bound &&
        ((uint64)x + w > scanout_w || (uint64)y + h > scanout_h)) {
        ret = -EINVAL;
        goto out;
    }
    if (!virtio_gpu_cmdline_enabled("virtio_gpu_allow_partial_scanout") &&
        !already_bound && !full_size_resource &&
        (w != scanout_w || h != scanout_h)) {
        ret = -EOPNOTSUPP;
        goto out;
    }

    if (!already_bound) {
        uint32 bind_x = full_size_resource ? 0 : x;
        uint32 bind_y = full_size_resource ? 0 : y;
        uint32 bind_w = full_size_resource ? scanout_w : w;
        uint32 bind_h = full_size_resource ? scanout_h : h;

        set_scanout_start = scanout_perf ? r_time() : 0;
        if (virtio_gpu_set_scanout(g, 0, res, bind_x, bind_y,
                                   bind_w, bind_h) != 0)
            goto out;
        if (scanout_perf)
            set_scanout_ticks += r_time() - set_scanout_start;
        g->bound_scanout_resource_id = res->id;
        if (!virtio_gpu_present_flip_slot_for_id(g, res->id, NULL)) {
            virtio_gpu_invalidate_present_flip_slot(g, 0);
            virtio_gpu_invalidate_present_flip_slot(g, 1);
        }
        virtio_gpu_page_flip_scanout_set_reset(g);
    }
    /*
     * The compositor's virgl target is updated by GL, not by the CPU backing.
     * A TRANSFER_TO_HOST here would overwrite that rendered target, but QEMU
     * still needs RESOURCE_FLUSH as the display update signal for the bound
     * scanout resource.  Leave a diagnostic escape hatch for comparing hosts
     * that repaint a 3D scanout without this notification.
     */
    flush_start = scanout_perf ? r_time() : 0;
    if (virtio_gpu_cmdline_enabled("virtio_gpu_no_scanout_flush")) {
        scanout_path = 1;
        ret = 0;
    } else if (already_bound && virtio_gpu_async_scanout_flush_enabled()) {
        scanout_path = 2;
        if (prepared_async_flush)
            ret = virtio_gpu_async_post_prepared(
                g, &scanout_flush_prep,
                VIRTIO_GPU_ASYNC_REASON_FLUSH, 0) == 0 ? 0 : -EIO;
        else
            ret = virtio_gpu_resource_flush_async(g, res, x, y, w, h) == 0 ?
                  0 : -EIO;
    } else {
        scanout_path = 3;
        ret = virtio_gpu_resource_flush(g, res, x, y, w, h) == 0 ? 0 : -EIO;
    }
    if (scanout_perf)
        flush_ticks = r_time() - flush_start;
out:
    if (scanout_perf)
        virtio_gpu_scanout_perf_log(
            ret, scanout_path, already_bound, wait_holder,
            r_time() - total_start, lock_acquired - total_start,
            set_scanout_ticks, flush_ticks);
    virtio_gpu_op_unlock(g);
    if (prepared_async_flush)
        virtio_gpu_async_submit_free(&scanout_flush_prep);
    return ret;
}

/*
 * P2 default flip (2026-07-04): the ordered page-flip path is default-ON
 * after the direct-KMS A/B (100 -> 118-125 FPS), two clean KDE passes,
 * and removal of the R5 blocker that failed the first promotion attempt.
 * Set virtio_gpu_ordered_page_flip=0 to restore the drain-to-fence path.
 */
static int virtio_gpu_ordered_page_flip_enabled(void)
{
    char buf[16];

    if (cmdline_get_param("virtio_gpu_ordered_page_flip", buf,
                          sizeof(buf)) != 0)
        return 1;
    return !(strcmp(buf, "0") == 0 ||
             strcmp(buf, "no") == 0 ||
             strcmp(buf, "false") == 0 ||
             strcmp(buf, "off") == 0);
}

int virtio_gpu_page_flip_resource(uint32 resource_id, uint32 w, uint32 h,
                                  uint32 *flags_out)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;
    uint32 scanout_w;
    uint32 scanout_h;
    int already_bound;
    int registered;
    int rebind = 0;
    int async_scanout = 0;
    uint64 src_submit_fence = 0;
    uint64 completed_fence = 0;
    int should_wait_src_fence = 0;
    int ordered_async_src_fence = 0;
    int ret;
    struct virtio_gpu_async_submit scanout_prep;
    static int flip_logs;
    static int wait_logs;

    if (flags_out != NULL)
        *flags_out = 0;
    if (resource_id == 0 || w == 0 || h == 0)
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    memset(&scanout_prep, 0, sizeof(scanout_prep));
    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_PAGE_FLIP);
    spin_lock(&g->lock);
    res = virtio_gpu_lookup_resource_locked(g, resource_id);
    scanout_w = g->scanout_width;
    scanout_h = g->scanout_height;
    if (res == NULL || !res->attached ||
        res->width != scanout_w || res->height != scanout_h ||
        w != scanout_w || h != scanout_h) {
        spin_unlock(&g->lock);
        ret = -EINVAL;
        goto out;
    }
    src_submit_fence = res->last_submit_fence;
    completed_fence = g->stats.last_fence;
    should_wait_src_fence = src_submit_fence != 0 &&
        src_submit_fence > completed_fence &&
        virtio_gpu_async_pending(g) &&
        virtio_gpu_async_newest_fence(g) >= src_submit_fence;
    ordered_async_src_fence = should_wait_src_fence &&
        virtio_gpu_async_scanout_flush_enabled() &&
        virtio_gpu_ordered_page_flip_enabled() &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_page_flip_drain_src_fence");
    spin_unlock(&g->lock);

    /*
     * Direct page-flip scanout must preserve the same producer-before-present
     * ordering as the resource-copy path.  Mesa can submit rendering work
     * asynchronously, then KMS may immediately flip/flush the target resource.
     *
     * With ordered page flip (default-on since 2026-07-04; opt out with
     * virtio_gpu_ordered_page_flip=0), the SET_SCANOUT/RESOURCE_FLUSH
     * below is posted after the pending producer submit on the same virtio
     * control queue, so queue ordering preserves producer-before-present
     * without stalling the compositor here.
     */
    if (should_wait_src_fence && !ordered_async_src_fence) {
        ret = virtio_gpu_drain_async_until_fence(g, src_submit_fence);
        if (ret != 0) {
            ret = -EIO;
            goto out;
        }
        if (virtio_gpu_cmdline_enabled("virtio_gpu_page_flip_wait_diag") &&
            wait_logs < 8) {
            printf("virtio_gpu: page-flip waited source fence=%lu completed=%lu resource=%u\n",
                   src_submit_fence, completed_fence, resource_id);
            wait_logs++;
        }
    } else if (ordered_async_src_fence &&
               virtio_gpu_cmdline_enabled("virtio_gpu_page_flip_wait_diag") &&
               wait_logs < 8) {
        printf("virtio_gpu: page-flip ordered source fence=%lu completed=%lu resource=%u\n",
               src_submit_fence, completed_fence, resource_id);
        wait_logs++;
    }

    if (g->page_flip_scanout_width != scanout_w ||
        g->page_flip_scanout_height != scanout_h)
        virtio_gpu_page_flip_scanout_set_reset(g);
    already_bound = g->bound_scanout_resource_id == res->id;
    registered = virtio_gpu_page_flip_scanout_set_contains(g, res->id);
    if (!already_bound && !registered) {
        if ((virtio_gpu_cmdline_enabled(
                 "virtio_gpu_async_page_flip_scanout") ||
             virtio_gpu_cmdline_enabled("vgpu_async_pf")) &&
            virtio_gpu_async_scanout_flush_enabled() &&
            virtio_gpu_set_scanout_async_prepare(
                0, res->id, 0, 0, scanout_w, scanout_h,
                &scanout_prep) == 0) {
            if (virtio_gpu_async_post_prepared(
                    g, &scanout_prep,
                    VIRTIO_GPU_ASYNC_REASON_OTHER, 0) != 0) {
                virtio_gpu_async_submit_free(&scanout_prep);
                if (virtio_gpu_set_scanout(g, 0, res, 0, 0,
                                           scanout_w, scanout_h) != 0) {
                    ret = -EIO;
                    goto out;
                }
            } else {
                async_scanout = 1;
            }
        } else {
            if (virtio_gpu_set_scanout(g, 0, res, 0, 0,
                                       scanout_w, scanout_h) != 0) {
                ret = -EIO;
                goto out;
            }
        }
        g->bound_scanout_resource_id = res->id;
        g->scanout_resource = res;
        virtio_gpu_page_flip_scanout_set_note(g, res->id, scanout_w,
                                              scanout_h);
        rebind = 1;
        if (!virtio_gpu_present_flip_slot_for_id(g, res->id, NULL)) {
            virtio_gpu_invalidate_present_flip_slot(g, 0);
            virtio_gpu_invalidate_present_flip_slot(g, 1);
        }
    }
    if (virtio_gpu_async_scanout_flush_enabled())
        ret = virtio_gpu_resource_flush_async(g, res, 0, 0,
                                              scanout_w, scanout_h) == 0 ?
            0 : -EIO;
    else
        ret = virtio_gpu_resource_flush(g, res, 0, 0, scanout_w,
                                        scanout_h) == 0 ? 0 : -EIO;
    if (ret == 0 && flip_logs < 8) {
        printf("virtio_gpu: page-flip present resource=%u size=%ux%u already_bound=%d registered=%d rebind=%d set_count=%u async_scanout=%d\n",
               res->id, scanout_w, scanout_h, already_bound, registered,
               rebind, g->page_flip_scanout_set_count, async_scanout);
        flip_logs++;
    }
    if (ret == 0 && flags_out != NULL) {
        if (rebind)
            *flags_out |= FB_GPU_PAGE_FLIP_F_SCANOUT_REBIND;
        if (registered || rebind)
            *flags_out |= FB_GPU_PAGE_FLIP_F_SCANOUT_CACHED;
    }
out:
    if (scanout_prep.cmd != NULL || scanout_prep.resp != NULL ||
        scanout_prep.data != NULL)
        virtio_gpu_async_submit_free(&scanout_prep);
    virtio_gpu_op_unlock(g);
    return ret;
}
