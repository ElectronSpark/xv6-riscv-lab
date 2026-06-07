static int virtio_gpu_ensure_present_context(struct virtio_gpu *g,
                                             uint32 *ctx_id)
{
    struct virtio_gpu_context *ctx;
    struct virtio_gpu_capset *capset;
    uint32 id;
    int ret;

    spin_lock(&g->lock);
    if (g->present_ctx_id != 0) {
        ctx = virtio_gpu_lookup_context_locked(g, g->present_ctx_id);
        if (ctx != NULL && !ctx->failed) {
            *ctx_id = g->present_ctx_id;
            spin_unlock(&g->lock);
            return 0;
        }
        g->present_ctx_id = 0;
    }
    capset = virtio_gpu_lookup_capset_locked(g, g->virgl_capset_id);
    if (capset == NULL) {
        spin_unlock(&g->lock);
        return -ENODEV;
    }
    ctx = virtio_gpu_alloc_context_slot_locked(g);
    if (ctx == NULL) {
        g->stats.failures++;
        spin_unlock(&g->lock);
        return -ENOSPC;
    }
    id = g->next_context_id++;
    if (g->next_context_id == 0)
        g->next_context_id = 2;
    memset(ctx, 0, sizeof(*ctx));
    ctx->in_use = 1;
    ctx->id = id;
    ctx->capset_id = capset->id;
    g->present_ctx_id = id;
    spin_unlock(&g->lock);

    ret = virtio_gpu_create_context(g, id, capset->id, "xv6-present-copy");
    if (ret != 0) {
        spin_lock(&g->lock);
        ctx = virtio_gpu_lookup_context_locked(g, id);
        if (ctx != NULL)
            memset(ctx, 0, sizeof(*ctx));
        if (g->present_ctx_id == id)
            g->present_ctx_id = 0;
        spin_unlock(&g->lock);
        return -EIO;
    }
    *ctx_id = id;
    return 0;
}

static void virtio_gpu_note_diagnostic_present_locked(
    struct virtio_gpu *g, uint32 ctx_id, uint32 resource_id,
    uint32 src_x, uint32 src_y, uint32 dst_x, uint32 dst_y,
    uint32 w, uint32 h)
{
    g->diagnostic_present_ctx_id = ctx_id;
    g->diagnostic_present_resource_id = resource_id;
    g->diagnostic_present_src_x = src_x;
    g->diagnostic_present_src_y = src_y;
    g->diagnostic_present_dst_x = dst_x;
    g->diagnostic_present_dst_y = dst_y;
    g->diagnostic_present_w = w;
    g->diagnostic_present_h = h;
}

static void virtio_gpu_drop_present_context(struct virtio_gpu *g, uint32 ctx_id)
{
    struct virtio_gpu_context *ctx;

    if (ctx_id == 0)
        return;
    (void)virtio_gpu_destroy_context(g, ctx_id);
    spin_lock(&g->lock);
    if (g->present_scanout_ctx_id == ctx_id) {
        g->present_scanout_ctx_id = 0;
        g->present_scanout_resource_id = 0;
    }
    ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
    if (ctx != NULL && ctx->owner_id == 0 && ctx->owner_tgid == 0) {
        if (ctx->failed && g->stats.context_failed > 0)
            g->stats.context_failed--;
        memset(ctx, 0, sizeof(*ctx));
    }
    if (g->present_ctx_id == ctx_id)
        g->present_ctx_id = 0;
    spin_unlock(&g->lock);
}

static int virtio_gpu_ensure_scanout_attached(struct virtio_gpu *g,
                                              uint32 ctx_id,
                                              uint32 resource_id)
{
    struct virtio_gpu_context *ctx;
    uint32 old_ctx = 0;
    uint32 old_res = 0;
    int cached;

    spin_lock(&g->lock);
    ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
    cached = ctx != NULL && !ctx->failed &&
             g->present_scanout_ctx_id == ctx_id &&
             g->present_scanout_resource_id == resource_id;
    if (cached) {
        spin_unlock(&g->lock);
        return 0;
    }
    if (g->present_scanout_ctx_id != 0) {
        old_ctx = g->present_scanout_ctx_id;
        old_res = g->present_scanout_resource_id;
        g->present_scanout_ctx_id = 0;
        g->present_scanout_resource_id = 0;
    }
    spin_unlock(&g->lock);

    if (old_ctx != 0 && old_res != 0)
        (void)virtio_gpu_context_resource(
            g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, old_ctx, old_res);

    if (virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
                                    ctx_id, resource_id) != 0)
        return -EIO;

    spin_lock(&g->lock);
    ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
    if (ctx != NULL && !ctx->failed) {
        g->present_scanout_ctx_id = ctx_id;
        g->present_scanout_resource_id = resource_id;
    }
    spin_unlock(&g->lock);
    return 0;
}

static void virtio_gpu_drop_present_flip_resources(struct virtio_gpu *g)
{
    for (int i = 0; i < 2; i++) {
        struct virtio_gpu_resource *res = g->present_flip_resource[i];

        g->present_flip_valid[i] = 0;
        g->present_flip_rect_x[i] = 0;
        g->present_flip_rect_y[i] = 0;
        g->present_flip_rect_w[i] = 0;
        g->present_flip_rect_h[i] = 0;
        if (res == NULL)
            continue;
        g->present_flip_resource[i] = NULL;
        if (g->bound_scanout_resource_id == res->id) {
            (void)virtio_gpu_set_scanout(g, 0, NULL, 0, 0, 0, 0);
            g->bound_scanout_resource_id = 0;
        }
        (void)virtio_gpu_resource_unref(g, res);
    }
    g->present_flip_index = 0;
}

static int virtio_gpu_present_flip_slot_for_id(struct virtio_gpu *g,
                                               uint32 resource_id,
                                               uint32 *slot_out)
{
    if (resource_id == 0)
        return 0;
    for (uint32 i = 0; i < 2; i++) {
        struct virtio_gpu_resource *res = g->present_flip_resource[i];

        if (res != NULL && res->id == resource_id) {
            if (slot_out != NULL)
                *slot_out = i;
            return 1;
        }
    }
    return 0;
}

static void virtio_gpu_invalidate_present_flip_slot(struct virtio_gpu *g,
                                                    uint32 slot)
{
    if (slot >= 2)
        return;
    g->present_flip_valid[slot] = 0;
    g->present_flip_base_generation[slot] = 0;
    g->present_flip_rect_x[slot] = 0;
    g->present_flip_rect_y[slot] = 0;
    g->present_flip_rect_w[slot] = 0;
    g->present_flip_rect_h[slot] = 0;
}

static int virtio_gpu_ensure_present_flip_resource(
    struct virtio_gpu *g, uint32 width, uint32 height,
    struct virtio_gpu_resource **out, uint32 *slot_out)
{
    struct virtio_gpu_resource *res;
    uint32 first;

    if (out == NULL || width == 0 || height == 0 ||
        width > 4096 || height > 4096)
        return -EINVAL;

    first = (g->present_flip_index + 1) & 1u;
    for (int pass = 0; pass < 2; pass++) {
        uint32 slot = (first + (uint32)pass) & 1u;

        res = g->present_flip_resource[slot];
        if (res != NULL && res->width == width && res->height == height &&
            res->attached) {
            if (g->bound_scanout_resource_id == res->id)
                continue;
            *out = res;
            if (slot_out)
                *slot_out = slot;
            return 0;
        }
    }

    for (int pass = 0; pass < 2; pass++) {
        uint32 slot = (first + (uint32)pass) & 1u;

        res = g->present_flip_resource[slot];
        if (res != NULL) {
            if (g->bound_scanout_resource_id == res->id)
                continue;
            g->present_flip_resource[slot] = NULL;
            virtio_gpu_invalidate_present_flip_slot(g, slot);
            (void)virtio_gpu_resource_unref(g, res);
        }

        if (virtio_gpu_resource_create_3d_backing(
                g, width, height, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                VIRTIO_GPU_PIPE_BIND_RENDER_TARGET |
                VIRTIO_GPU_PIPE_BIND_SAMPLER_VIEW |
                VIRTIO_GPU_PIPE_BIND_DISPLAY_TARGET |
                VIRTIO_GPU_PIPE_BIND_SCANOUT |
                VIRTIO_GPU_PIPE_BIND_SHARED |
                VIRTIO_GPU_PIPE_BIND_LINEAR,
                &res) != 0)
            return -EIO;
        memset(res->backing, 0, res->backing_len);
        if (virtio_gpu_resource_attach_backing(g, res) != 0) {
            (void)virtio_gpu_resource_unref(g, res);
            return -EIO;
        }
        /*
         * Match the persistent scanout resource setup: make QEMU/virgl create
         * host storage from the newly attached guest backing before the
         * resource is used as a pageflip target.  Without this, copies into
         * the flip resource can complete but TRANSFER_FROM_HOST_3D validates
         * black on WSL/D3D12, causing the same blank/blinking path the Linux
         * KMS import sequence avoids.
         */
        if (virtio_gpu_resource_transfer_scanout(g, res, 0, 0, width,
                                                 height) != 0) {
            (void)virtio_gpu_resource_unref(g, res);
            return -EIO;
        }
        g->present_flip_resource[slot] = res;
        virtio_gpu_invalidate_present_flip_slot(g, slot);
        *out = res;
        if (slot_out)
            *slot_out = slot;
        return 0;
    }
    return -EAGAIN;
}

static uint32 virtio_gpu_resource_sample_pixel(struct virtio_gpu_resource *res,
                                               uint32 x, uint32 y)
{
    uint64 off;
    uint32 page_idx;
    uint32 page_off;
    uint8 *p;

    if (res == NULL || res->width == 0 ||
        res->height == 0 || x >= res->width || y >= res->height)
        return 0;
    off = ((uint64)y * res->width + x) * sizeof(uint32);
    if (res->backing != NULL && off + sizeof(uint32) <= res->backing_len)
        return *(uint32 *)((uint8 *)res->backing + off);
    if (res->pages == NULL)
        return 0;
    page_idx = off / PGSIZE;
    page_off = off & (PGSIZE - 1);
    if (page_idx >= res->npages || page_off + sizeof(uint32) > PGSIZE ||
        res->pages[page_idx] == NULL)
        return 0;
    p = (uint8 *)PA2VA(__page_to_pa(res->pages[page_idx])) + page_off;
    return *(uint32 *)p;
}

struct virtio_gpu_present_samples {
    uint32 p[VIRTIO_GPU_PRESENT_SAMPLE_COUNT];
    uint32 nonblack;
    uint32 unique_nonblack;
};

static void virtio_gpu_count_present_samples(
    struct virtio_gpu_present_samples *samples)
{
    samples->nonblack = 0;
    samples->unique_nonblack = 0;
    for (int i = 0; i < VIRTIO_GPU_PRESENT_SAMPLE_COUNT; i++) {
        uint32 rgb = samples->p[i] & 0x00ffffffu;
        int seen = 0;

        if (rgb == 0)
            continue;
        samples->nonblack++;
        for (int j = 0; j < i; j++) {
            if ((samples->p[j] & 0x00ffffffu) == rgb) {
                seen = 1;
                break;
            }
        }
        if (!seen)
            samples->unique_nonblack++;
    }
}

static int virtio_gpu_resource_host_samples(
    struct virtio_gpu *g,
    struct virtio_gpu_resource *res,
    uint32 ctx_id,
    uint32 x, uint32 y,
    uint32 w, uint32 h,
    struct virtio_gpu_present_samples *samples)
{
    struct virtio_gpu_transfer_host_3d *transfer =
        (struct virtio_gpu_transfer_host_3d *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;

    if (res == NULL || res->width == 0 || res->height == 0 ||
        w == 0 || h == 0 ||
        x > res->width || w > res->width - x ||
        y > res->height || h > res->height - y ||
        (res->backing == NULL && (res->pages == NULL || res->npages == 0)) ||
        samples == NULL)
        return -EINVAL;

    memset(transfer, 0, sizeof(*transfer));
    transfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    transfer->hdr.ctx_id = ctx_id;
    transfer->box.x = x;
    transfer->box.y = y;
    transfer->box.z = 0;
    transfer->box.w = w;
    transfer->box.h = h;
    transfer->box.d = 1;
    transfer->offset = ((uint64)y * res->width + x) * sizeof(uint32);
    transfer->resource_id = res->id;
    transfer->level = 0;
    transfer->stride = res->width * sizeof(uint32);
    transfer->layer_stride = (uint64)transfer->stride * res->height;
    if (virtio_gpu_submit(g, transfer, sizeof(*transfer), NULL, 0, false,
                          resp, sizeof(*resp),
                          VIRTIO_GPU_RESP_OK_NODATA) != 0)
        return -EIO;

    memset(samples, 0, sizeof(*samples));
    samples->p[0] = virtio_gpu_resource_sample_pixel(res, x + w / 2,
                                                     y + h / 2);
    samples->p[1] = virtio_gpu_resource_sample_pixel(res, x + w / 4,
                                                     y + h / 4);
    samples->p[2] = virtio_gpu_resource_sample_pixel(res, x + (w * 3) / 4,
                                                     y + h / 4);
    samples->p[3] = virtio_gpu_resource_sample_pixel(res, x + w / 4,
                                                     y + (h * 3) / 4);
    samples->p[4] = virtio_gpu_resource_sample_pixel(res,
                                                     x + (w * 3) / 4,
                                                     y + (h * 3) / 4);
    virtio_gpu_count_present_samples(samples);
    return 0;
}

static int virtio_gpu_resource_host_nonblack(struct virtio_gpu *g,
                                             struct virtio_gpu_resource *res,
                                             uint32 ctx_id,
                                             uint32 x, uint32 y,
                                             uint32 w, uint32 h)
{
    struct virtio_gpu_present_samples samples;
    int ret = virtio_gpu_resource_host_samples(g, res, ctx_id, x, y, w, h,
                                               &samples);

    return ret == 0 ? (int)samples.nonblack : ret;
}

static int virtio_gpu_present_sample_matches(
    const struct virtio_gpu_present_samples *src,
    const struct virtio_gpu_present_samples *dst)
{
    int matches = 0;

    if (src == NULL || dst == NULL)
        return 0;
    for (int i = 0; i < VIRTIO_GPU_PRESENT_SAMPLE_COUNT; i++) {
        uint32 rgb = src->p[i] & 0x00ffffffu;

        if (rgb != 0 && rgb == (dst->p[i] & 0x00ffffffu))
            matches++;
    }
    return matches;
}

static int virtio_gpu_clear_texture_rect(struct virtio_gpu *g,
                                         uint32 ctx_id,
                                         uint32 resource_id,
                                         uint32 x, uint32 y,
                                         uint32 w, uint32 h,
                                         uint32 color)
{
    uint32 cmds[VIRGL_CLEAR_TEXTURE_SIZE + 1];
    uint64 fence_id;

    if (resource_id == 0 || w == 0 || h == 0)
        return -EINVAL;

    spin_lock(&g->lock);
    fence_id = ++g->next_fence_id;
    spin_unlock(&g->lock);

    memset(cmds, 0, sizeof(cmds));
    cmds[0] = VIRGL_CMD0(VIRGL_CCMD_CLEAR_TEXTURE, 0,
                         VIRGL_CLEAR_TEXTURE_SIZE);
    cmds[1] = resource_id;
    cmds[2] = 0;
    cmds[3] = x;
    cmds[4] = y;
    cmds[5] = 0;
    cmds[6] = w;
    cmds[7] = h;
    cmds[8] = 1;
    cmds[9] = color;
    cmds[10] = 0;
    cmds[11] = 0;
    cmds[12] = 0;

    return virtio_gpu_submit_3d(g, ctx_id, cmds,
                                sizeof(cmds) / sizeof(cmds[0]),
                                fence_id) == 0 ? 0 : -EIO;
}

static int virtio_gpu_submit_blit_rect(struct virtio_gpu *g,
                                       uint32 ctx_id,
                                       uint32 dst_resource_id,
                                       uint32 dst_format,
                                       uint32 dst_x, uint32 dst_y,
                                       uint32 w, uint32 h,
                                       uint32 src_resource_id,
                                       uint32 src_format,
                                       uint32 src_x, uint32 src_y,
                                       uint64 fence_id)
{
    uint32 cmds[VIRGL_CMD_BLIT_SIZE + 1];

    memset(cmds, 0, sizeof(cmds));
    cmds[0] = VIRGL_CMD0(VIRGL_CCMD_BLIT, 0, VIRGL_CMD_BLIT_SIZE);
    cmds[1] = VIRGL_CMD_BLIT_S0_MASK(VIRGL_PIPE_MASK_RGBA) |
              VIRGL_CMD_BLIT_S0_FILTER(VIRGL_PIPE_TEX_FILTER_NEAREST);
    cmds[2] = 0;
    cmds[3] = 0;
    cmds[4] = dst_resource_id;
    cmds[5] = 0;
    cmds[6] = dst_format ? dst_format : VIRGL_FORMAT_B8G8R8A8_UNORM;
    cmds[7] = dst_x;
    cmds[8] = dst_y;
    cmds[9] = 0;
    cmds[10] = w;
    cmds[11] = h;
    cmds[12] = 1;
    cmds[13] = src_resource_id;
    cmds[14] = 0;
    cmds[15] = src_format ? src_format : VIRGL_FORMAT_B8G8R8A8_UNORM;
    cmds[16] = src_x;
    cmds[17] = src_y;
    cmds[18] = 0;
    cmds[19] = w;
    cmds[20] = h;
    cmds[21] = 1;
    return virtio_gpu_submit_3d(g, ctx_id, cmds,
                                sizeof(cmds) / sizeof(cmds[0]),
                                fence_id) == 0 ? 0 : -EIO;
}

static int virtio_gpu_submit_copy_rect(struct virtio_gpu *g,
                                       uint32 ctx_id,
                                       uint32 dst_resource_id,
                                       uint32 dst_x, uint32 dst_y,
                                       uint32 w, uint32 h,
                                       uint32 src_resource_id,
                                       uint32 src_x, uint32 src_y,
                                       uint64 fence_id)
{
    uint32 cmds[VIRGL_CMD_RESOURCE_COPY_REGION_SIZE + 1];

    memset(cmds, 0, sizeof(cmds));
    cmds[0] = VIRGL_CMD0(VIRGL_CCMD_RESOURCE_COPY_REGION, 0,
                         VIRGL_CMD_RESOURCE_COPY_REGION_SIZE);
    cmds[1] = dst_resource_id;
    cmds[2] = 0;
    cmds[3] = dst_x;
    cmds[4] = dst_y;
    cmds[5] = 0;
    cmds[6] = src_resource_id;
    cmds[7] = 0;
    cmds[8] = src_x;
    cmds[9] = src_y;
    cmds[10] = 0;
    cmds[11] = w;
    cmds[12] = h;
    cmds[13] = 1;
    return virtio_gpu_submit_3d(g, ctx_id, cmds,
                                sizeof(cmds) / sizeof(cmds[0]),
                                fence_id) == 0 ? 0 : -EIO;
}

static int virtio_gpu_submit_copy_transfer_rect(struct virtio_gpu *g,
                                                uint32 ctx_id,
                                                struct virtio_gpu_resource *dst,
                                                uint32 dst_x, uint32 dst_y,
                                                uint32 w, uint32 h,
                                                struct virtio_gpu_resource *src,
                                                uint32 src_x, uint32 src_y,
                                                uint64 fence_id)
{
    uint32 cmds[VIRGL_COPY_TRANSFER3D_SIZE + 1];
    uint64 src_offset;

    if (dst == NULL || src == NULL || dst->width == 0 || src->width == 0)
        return -EINVAL;
    src_offset = ((uint64)src_y * src->width + src_x) * sizeof(uint32);
    if (src_offset > 0xffffffffu)
        return -EINVAL;

    memset(cmds, 0, sizeof(cmds));
    cmds[0] = VIRGL_CMD0(VIRGL_CCMD_COPY_TRANSFER3D, 0,
                         VIRGL_COPY_TRANSFER3D_SIZE);
    cmds[1] = dst->id;
    cmds[2] = 0;
    cmds[3] = 0;
    cmds[4] = dst->width * sizeof(uint32);
    cmds[5] = dst->width * dst->height * sizeof(uint32);
    cmds[6] = dst_x;
    cmds[7] = dst_y;
    cmds[8] = 0;
    cmds[9] = w;
    cmds[10] = h;
    cmds[11] = 1;
    cmds[12] = src->id;
    cmds[13] = (uint32)src_offset;
    cmds[14] = VIRGL_COPY_TRANSFER3D_FLAGS_SYNCHRONIZED |
               VIRGL_COPY_TRANSFER3D_FLAGS_READ_FROM_HOST;
    return virtio_gpu_submit_3d(g, ctx_id, cmds,
                                sizeof(cmds) / sizeof(cmds[0]),
                                fence_id) == 0 ? 0 : -EIO;
}

static int virtio_gpu_submit_present_copy_method(
    struct virtio_gpu *g, uint32 ctx_id,
    struct virtio_gpu_resource *dst, uint32 dst_x, uint32 dst_y,
    uint32 w, uint32 h,
    struct virtio_gpu_resource *src, uint32 src_x, uint32 src_y,
    uint64 fence_id, int use_copy_transfer, int use_blit)
{
    if (dst == NULL || src == NULL)
        return -EINVAL;
    if (use_copy_transfer) {
        return virtio_gpu_submit_copy_transfer_rect(g, ctx_id, dst, dst_x,
                                                    dst_y, w, h, src, src_x,
                                                    src_y, fence_id);
    }
    if (use_blit) {
        return virtio_gpu_submit_blit_rect(
            g, ctx_id, dst->id, dst->format, dst_x, dst_y, w, h,
            src->id, src->format, src_x, src_y, fence_id);
    }
    return virtio_gpu_submit_copy_rect(g, ctx_id, dst->id, dst_x, dst_y,
                                       w, h, src->id, src_x, src_y,
                                       fence_id);
}

static int virtio_gpu_try_pageflip_copy(
    struct virtio_gpu *g, struct virtio_gpu_resource *src,
    struct virtio_gpu_resource *scanout, uint32 ctx_id,
    uint32 src_x, uint32 src_y, uint32 dst_x, uint32 dst_y,
    uint32 w, uint32 h, int use_copy_transfer, int use_blit)
{
    struct virtio_gpu_resource *base;
    struct virtio_gpu_resource *flip = NULL;
    uint32 base_resource_id;
    uint32 flip_slot = 0;
    uint64 fence_id;
    int ret;
    int base_attached = 0;
    int flip_attached = 0;
    int need_base_copy = 1;
    uint32 base_slot = 0;
    int base_is_valid_flip = 0;
    int same_rect_as_flip = 0;
    int flip_has_current_base = 0;
    uint32 base_generation = g->present_base_generation;
    uint32 flush_x = 0;
    uint32 flush_y = 0;
    uint32 flush_w = 0;
    uint32 flush_h = 0;
    int validate_copy = virtio_gpu_pageflip_validate_enabled();
    static int validation_state;
    static int fail_logs;
    static int success_logs;

    if (!virtio_gpu_pageflip_copy_enabled() ||
        src == NULL || scanout == NULL || !scanout->is_3d ||
        scanout->width == 0 || scanout->height == 0)
        return -EOPNOTSUPP;
    if (dst_x > scanout->width || w > scanout->width - dst_x ||
        dst_y > scanout->height || h > scanout->height - dst_y)
        return -EINVAL;
    flush_w = scanout->width;
    flush_h = scanout->height;
    if (validate_copy && validation_state < 0 &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_present_force_copy"))
        return -EOPNOTSUPP;

    ret = virtio_gpu_ensure_present_flip_resource(
        g, scanout->width, scanout->height, &flip, &flip_slot);
    if (ret != 0) {
        if (fail_logs < 8) {
            printf("virtio_gpu: pageflip-copy setup failed ret=%d screen=%ux%u\n",
                   ret, scanout->width, scanout->height);
            fail_logs++;
        }
        return ret;
    }

    if (virtio_gpu_cmdline_enabled("virtio_gpu_pageflip_base_scanout")) {
        base = scanout;
        base_resource_id = base->id;
    } else {
        spin_lock(&g->lock);
        base = virtio_gpu_lookup_resource_locked(g,
                                                 g->bound_scanout_resource_id);
        if (base == NULL || !base->attached ||
            base->width != scanout->width || base->height != scanout->height)
            base = scanout;
        base_resource_id = base->id;
        spin_unlock(&g->lock);
    }

    base_is_valid_flip =
        virtio_gpu_present_flip_slot_for_id(g, base_resource_id, &base_slot) &&
        g->present_flip_valid[base_slot];
    same_rect_as_flip =
        g->present_flip_valid[flip_slot] &&
        g->present_flip_rect_x[flip_slot] == dst_x &&
        g->present_flip_rect_y[flip_slot] == dst_y &&
        g->present_flip_rect_w[flip_slot] == w &&
        g->present_flip_rect_h[flip_slot] == h;
    flip_has_current_base =
        g->present_flip_valid[flip_slot] &&
        g->present_flip_base_generation[flip_slot] == base_generation;
    if (base_is_valid_flip && same_rect_as_flip &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_pageflip_full_copy"))
        need_base_copy = 0;
    if (same_rect_as_flip && flip_has_current_base &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_pageflip_full_copy"))
        need_base_copy = 0;

    if (need_base_copy &&
        base_resource_id != scanout->id && base_resource_id != src->id) {
        ret = virtio_gpu_context_resource(
            g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, ctx_id, base_resource_id);
        if (ret != 0) {
            if (fail_logs < 8) {
                printf("virtio_gpu: pageflip-copy attach-base failed ret=%d ctx=%u base=%u\n",
                       ret, ctx_id, base_resource_id);
                fail_logs++;
            }
            return -EIO;
        }
        base_attached = 1;
    }
    ret = virtio_gpu_context_resource(
        g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, ctx_id, flip->id);
    if (ret != 0) {
        if (fail_logs < 8) {
            printf("virtio_gpu: pageflip-copy attach-flip failed ret=%d ctx=%u flip=%u\n",
                   ret, ctx_id, flip->id);
            fail_logs++;
        }
        ret = -EIO;
        goto out;
    }
    flip_attached = 1;

    spin_lock(&g->lock);
    fence_id = ++g->next_fence_id;
    spin_unlock(&g->lock);
    if (need_base_copy) {
        ret = virtio_gpu_submit_present_copy_method(
            g, ctx_id, flip, 0, 0, scanout->width, scanout->height,
            base, 0, 0, fence_id, 0, use_blit);
        if (ret != 0) {
            if (fail_logs < 8) {
                printf("virtio_gpu: pageflip-copy base-copy failed ret=%d ctx=%u base=%u flip=%u size=%ux%u mode=%s\n",
                       ret, ctx_id, base_resource_id, flip->id,
                       scanout->width, scanout->height,
                       use_blit ? "blit" : "copy");
                fail_logs++;
            }
            virtio_gpu_invalidate_present_flip_slot(g, flip_slot);
            goto out;
        }
        g->present_flip_base_generation[flip_slot] = base_generation;
    } else {
        flush_x = dst_x;
        flush_y = dst_y;
        flush_w = w;
        flush_h = h;
    }

    spin_lock(&g->lock);
    fence_id = ++g->next_fence_id;
    spin_unlock(&g->lock);
    ret = virtio_gpu_submit_present_copy_method(
        g, ctx_id, flip, dst_x, dst_y, w, h, src, src_x, src_y,
        fence_id, use_copy_transfer, use_blit);
    if (ret != 0) {
        if (fail_logs < 8) {
            printf("virtio_gpu: pageflip-copy overlay-copy failed ret=%d ctx=%u src=%u flip=%u rect=%u,%u %ux%u mode=%s\n",
                   ret, ctx_id, src->id, flip->id, dst_x, dst_y, w, h,
                   use_copy_transfer ? "copy-transfer" :
                                       (use_blit ? "blit" : "copy"));
            fail_logs++;
        }
        virtio_gpu_invalidate_present_flip_slot(g, flip_slot);
        goto out;
    }

    ret = virtio_gpu_set_scanout(g, 0, flip, 0, 0, scanout->width,
                                 scanout->height) == 0 ? 0 : -EIO;
    if (ret != 0) {
        if (fail_logs < 8) {
            printf("virtio_gpu: pageflip-copy set-scanout failed ret=%d flip=%u size=%ux%u\n",
                   ret, flip->id, scanout->width, scanout->height);
            fail_logs++;
        }
        goto out;
    }
    g->bound_scanout_resource_id = flip->id;
    g->present_flip_index = flip_slot;
    g->present_flip_valid[flip_slot] = 1;
    g->present_flip_rect_x[flip_slot] = dst_x;
    g->present_flip_rect_y[flip_slot] = dst_y;
    g->present_flip_rect_w[flip_slot] = w;
    g->present_flip_rect_h[flip_slot] = h;
    ret = virtio_gpu_resource_flush(g, flip, flush_x, flush_y, flush_w,
                                    flush_h) == 0 ? 0 : -EIO;
    if (ret != 0 && fail_logs < 8) {
        printf("virtio_gpu: pageflip-copy flush failed ret=%d flip=%u rect=%u,%u %ux%u\n",
               ret, flip->id, flush_x, flush_y, flush_w, flush_h);
        fail_logs++;
    }
    if (ret == 0 && validate_copy && validation_state == 0 &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_present_force_copy")) {
        struct virtio_gpu_present_samples src_samples;
        struct virtio_gpu_present_samples flip_samples;
        int src_ret = virtio_gpu_resource_host_samples(
            g, src, ctx_id, src_x, src_y, w, h, &src_samples);
        int flip_ret = virtio_gpu_resource_host_samples(
            g, flip, ctx_id, dst_x, dst_y, w, h, &flip_samples);
        int matches = (src_ret == 0 && flip_ret == 0) ?
            virtio_gpu_present_sample_matches(&src_samples,
                                              &flip_samples) : 0;
        int required_matches =
            (src_ret == 0 && src_samples.unique_nonblack >= 2 &&
             src_samples.nonblack >= 3) ? 2 : 1;

        if (src_ret != 0 || flip_ret != 0 ||
            (src_samples.nonblack > 0 && matches < required_matches)) {
            validation_state = -1;
            if (fail_logs < 4) {
                printf("virtio_gpu: pageflip-copy validation failed after-scanout src_ret=%d flip_ret=%d src_nonblack=%u flip_nonblack=%u matches=%d required=%d src=%08x,%08x,%08x,%08x,%08x flip=%08x,%08x,%08x,%08x,%08x; disabling pageflip-copy for this boot\n",
                       src_ret, flip_ret,
                       src_ret == 0 ? src_samples.nonblack : 0,
                       flip_ret == 0 ? flip_samples.nonblack : 0,
                       matches, required_matches,
                       src_ret == 0 ? src_samples.p[0] : 0,
                       src_ret == 0 ? src_samples.p[1] : 0,
                       src_ret == 0 ? src_samples.p[2] : 0,
                       src_ret == 0 ? src_samples.p[3] : 0,
                       src_ret == 0 ? src_samples.p[4] : 0,
                       flip_ret == 0 ? flip_samples.p[0] : 0,
                       flip_ret == 0 ? flip_samples.p[1] : 0,
                       flip_ret == 0 ? flip_samples.p[2] : 0,
                       flip_ret == 0 ? flip_samples.p[3] : 0,
                       flip_ret == 0 ? flip_samples.p[4] : 0);
                fail_logs++;
            }
            (void)virtio_gpu_set_scanout(g, 0, scanout, 0, 0,
                                         scanout->width, scanout->height);
            g->bound_scanout_resource_id = scanout->id;
            virtio_gpu_invalidate_present_flip_slot(g, flip_slot);
            ret = -EOPNOTSUPP;
            goto out;
        }
        if (matches >= required_matches)
            validation_state = 1;
    }
    if (ret == 0 && success_logs < 8) {
        printf("virtio_gpu: pageflip-copy present src=%u base=%u flip=%u slot=%u rect=%u,%u %ux%u screen=%ux%u base_copy=%d base_gen=%u flush=%u,%u %ux%u\n",
               src->id, base_resource_id, flip->id, flip_slot,
               dst_x, dst_y, w, h, scanout->width, scanout->height,
               need_base_copy, base_generation, flush_x, flush_y, flush_w,
               flush_h);
        success_logs++;
    }
    if (ret != 0)
        virtio_gpu_invalidate_present_flip_slot(g, flip_slot);

out:
    if (flip_attached)
        (void)virtio_gpu_context_resource(
            g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, ctx_id, flip->id);
    if (base_attached)
        (void)virtio_gpu_context_resource(
            g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, ctx_id, base_resource_id);
    return ret;
}

static void virtio_gpu_probe_present_source(struct virtio_gpu *g,
                                            struct virtio_gpu_resource *src,
                                            uint32 ctx_id)
{
    struct virtio_gpu_transfer_host_3d *transfer =
        (struct virtio_gpu_transfer_host_3d *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    uint32 p0, p1, p2, p3, p4;
    uint32 nonblack = 0;
    int ret;
    static int probe_logs;

    if (!virtio_gpu_cmdline_enabled("virtio_gpu_present_probe") ||
        probe_logs >= 12 || src == NULL || src->width == 0 ||
        src->height == 0 || src->pages == NULL || src->npages == 0)
        return;

    memset(transfer, 0, sizeof(*transfer));
    transfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    transfer->hdr.ctx_id = ctx_id;
    transfer->box.x = 0;
    transfer->box.y = 0;
    transfer->box.z = 0;
    transfer->box.w = src->width;
    transfer->box.h = src->height;
    transfer->box.d = 1;
    transfer->offset = 0;
    transfer->resource_id = src->id;
    transfer->level = 0;
    transfer->stride = src->width * sizeof(uint32);
    transfer->layer_stride = (uint64)transfer->stride * src->height;
    ret = virtio_gpu_submit(g, transfer, sizeof(*transfer), NULL, 0, false,
                            resp, sizeof(*resp),
                            VIRTIO_GPU_RESP_OK_NODATA);
    p0 = virtio_gpu_resource_sample_pixel(src, src->width / 2,
                                          src->height / 2);
    p1 = virtio_gpu_resource_sample_pixel(src, src->width / 4,
                                          src->height / 4);
    p2 = virtio_gpu_resource_sample_pixel(src, (src->width * 3) / 4,
                                          src->height / 4);
    p3 = virtio_gpu_resource_sample_pixel(src, src->width / 4,
                                          (src->height * 3) / 4);
    p4 = virtio_gpu_resource_sample_pixel(src, (src->width * 3) / 4,
                                          (src->height * 3) / 4);
    if ((p0 & 0x00ffffffu) != 0) nonblack++;
    if ((p1 & 0x00ffffffu) != 0) nonblack++;
    if ((p2 & 0x00ffffffu) != 0) nonblack++;
    if ((p3 & 0x00ffffffu) != 0) nonblack++;
    if ((p4 & 0x00ffffffu) != 0) nonblack++;
    printf("virtio_gpu: present source probe resource=%u ctx=%u ret=%d nonblack=%u samples=%08x,%08x,%08x,%08x,%08x size=%ux%u\n",
           src->id, ctx_id, ret == 0 ? 0 : -EIO, nonblack,
           p0, p1, p2, p3, p4, src->width, src->height);
    probe_logs++;
}

static void virtio_gpu_probe_present_dst(struct virtio_gpu *g,
                                         struct virtio_gpu_resource *dst,
                                         uint32 ctx_id,
                                         uint32 x, uint32 y,
                                         uint32 w, uint32 h)
{
    struct virtio_gpu_transfer_host_3d *transfer =
        (struct virtio_gpu_transfer_host_3d *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    uint32 p0, p1, p2, p3, p4;
    uint32 nonblack = 0;
    int ret;
    static int probe_logs;

    if (!virtio_gpu_cmdline_enabled("virtio_gpu_present_probe") ||
        probe_logs >= 12 || dst == NULL || dst->width == 0 ||
        dst->height == 0 ||
        (dst->backing == NULL && (dst->pages == NULL || dst->npages == 0)))
        return;

    memset(transfer, 0, sizeof(*transfer));
    transfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    transfer->hdr.ctx_id = ctx_id;
    transfer->box.x = x;
    transfer->box.y = y;
    transfer->box.z = 0;
    transfer->box.w = w;
    transfer->box.h = h;
    transfer->box.d = 1;
    transfer->offset = ((uint64)y * dst->width + x) * sizeof(uint32);
    transfer->resource_id = dst->id;
    transfer->level = 0;
    transfer->stride = dst->width * sizeof(uint32);
    transfer->layer_stride = (uint64)transfer->stride * dst->height;
    ret = virtio_gpu_submit(g, transfer, sizeof(*transfer), NULL, 0, false,
                            resp, sizeof(*resp),
                            VIRTIO_GPU_RESP_OK_NODATA);
    p0 = virtio_gpu_resource_sample_pixel(dst, x + w / 2, y + h / 2);
    p1 = virtio_gpu_resource_sample_pixel(dst, x + w / 4, y + h / 4);
    p2 = virtio_gpu_resource_sample_pixel(dst, x + (w * 3) / 4, y + h / 4);
    p3 = virtio_gpu_resource_sample_pixel(dst, x + w / 4, y + (h * 3) / 4);
    p4 = virtio_gpu_resource_sample_pixel(dst, x + (w * 3) / 4,
                                          y + (h * 3) / 4);
    if ((p0 & 0x00ffffffu) != 0) nonblack++;
    if ((p1 & 0x00ffffffu) != 0) nonblack++;
    if ((p2 & 0x00ffffffu) != 0) nonblack++;
    if ((p3 & 0x00ffffffu) != 0) nonblack++;
    if ((p4 & 0x00ffffffu) != 0) nonblack++;
    printf("virtio_gpu: present dst probe resource=%u ctx=%u ret=%d nonblack=%u samples=%08x,%08x,%08x,%08x,%08x rect=%u,%u %ux%u\n",
           dst->id, ctx_id, ret == 0 ? 0 : -EIO, nonblack,
           p0, p1, p2, p3, p4, x, y, w, h);
    probe_logs++;
}

int virtio_gpu_copy_resource_to_scanout(uint32 src_resource_id,
                                        uint32 src_x, uint32 src_y,
                                        uint32 dst_x, uint32 dst_y,
                                        uint32 w, uint32 h)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *src;
    struct virtio_gpu_resource *dst;
    struct virtio_gpu_context *ctx;
    uint32 ctx_id = 0;
    uint32 dst_resource_id;
    uint32 src_width = 0;
    uint32 src_height = 0;
    uint32 dst_width = 0;
    uint32 dst_height = 0;
    uint64 fence_id;
    uint64 src_submit_fence = 0;
    uint64 completed_fence = 0;
    int should_wait_src_fence = 0;
    int src_present = 0;
    int src_is_attached = 0;
    int dst_present = 0;
    int dst_is_attached = 0;
    int src_attached = 0;
    int used_present_ctx = 0;
    int ret = -EIO;
    static int invalid_logs;
    static int debug_logs;
    static int copy_validation_state;
    static int copy_validation_logs;
    int validate_copy =
        virtio_gpu_cmdline_enabled("virtio_gpu_present_validate_copy");
    int use_copy_transfer =
        virtio_gpu_cmdline_enabled("virtio_gpu_present_copy_transfer");
    int use_blit =
        virtio_gpu_cmdline_enabled("virtio_gpu_present_blit") &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_present_copy_region");
    int unbind_scanout_copy =
        virtio_gpu_cmdline_enabled("virtio_gpu_unbind_scanout_copy");
    int rearm_scanout_copy =
        virtio_gpu_cmdline_enabled("virtio_gpu_rearm_scanout_copy") &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_no_rearm_scanout_copy");

    if (src_resource_id == 0 || w == 0 || h == 0)
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_PRESENT_COPY);
    spin_lock(&g->lock);
    src = virtio_gpu_lookup_resource_locked(g, src_resource_id);
    dst = g->scanout_resource;
    src_present = src != NULL;
    dst_present = dst != NULL;
    if (src != NULL) {
        src_width = src->width;
        src_height = src->height;
        src_is_attached = src->attached;
        src_submit_fence = src->last_submit_fence;
    }
    if (dst != NULL) {
        dst_width = dst->width;
        dst_height = dst->height;
        dst_is_attached = dst->attached;
    }
    if (src == NULL || dst == NULL || !src->attached || !dst->attached ||
        src_x > src->width || w > src->width - src_x ||
        src_y > src->height || h > src->height - src_y ||
        dst_x > dst->width || w > dst->width - dst_x ||
        dst_y > dst->height || h > dst->height - dst_y) {
        spin_unlock(&g->lock);
        if (invalid_logs < 4) {
            printf("virtio_gpu: resource-copy reject src=%u present=%d attached=%d size=%ux%u src_rect=%u,%u %ux%u dst_present=%d dst_attached=%d dst_size=%ux%u dst_rect=%u,%u\n",
                   src_resource_id, src_present, src_is_attached,
                   src_width, src_height, src_x, src_y, w, h,
                   dst_present, dst_is_attached, dst_width, dst_height,
                   dst_x, dst_y);
            invalid_logs++;
        }
        ret = -EINVAL;
        goto out;
    }
    dst_resource_id = dst->id;
    if (src->ctx_id != 0 &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_present_separate_ctx")) {
        ctx = virtio_gpu_lookup_context_locked(g, src->ctx_id);
        if (ctx != NULL && !ctx->failed)
            ctx_id = src->ctx_id;
    }
    if (debug_logs < 8) {
        printf("virtio_gpu: present copy src=%u ctx=%u kind=%s fmt=%u bind=0x%x target=%u %ux%u -> dst=%u kind=%s fmt=%u bind=0x%x target=%u %ux%u rect src=%u,%u dst=%u,%u %ux%u mode=%s\n",
               src_resource_id, ctx_id, src->is_3d ? "3d" : "2d",
               src->format, src->bind, src->target, src->width, src->height,
               dst->id, dst->is_3d ? "3d" : "2d", dst->format, dst->bind,
               dst->target, dst->width, dst->height, src_x, src_y, dst_x,
               dst_y, w, h,
               use_copy_transfer ? "copy-transfer" :
                                    (use_blit ? "blit" : "copy"));
        debug_logs++;
    }
    completed_fence = g->stats.last_fence;
    should_wait_src_fence = src_submit_fence != 0 &&
        src_submit_fence > completed_fence &&
        virtio_gpu_async_pending(g) &&
        virtio_gpu_async_newest_fence(g) <= src_submit_fence;
    spin_unlock(&g->lock);

    if (virtio_gpu_cmdline_enabled("virtio_gpu_present_readback_copy")) {
        ret = -EINVAL;
        goto out;
    }

    /*
     * Mesa submits virgl command streams asynchronously.  Linux/Alpine gets an
     * implicit dma-buf fence before scanout import; xv6's native Wayland path
     * currently hands the compositor only the resource fd.  Drain the one
     * pending virgl submit before copying that resource into the scanout, so we
     * present the rendered frame instead of the previous/empty contents.
     *
     * By default this drains the WHOLE async ring with wait=1, which also waits
     * for the previous frame's async RESOURCE_FLUSH to retire on the host (its
     * ~vsync).  That re-serialises the pipeline at every frame boundary and is
     * the dominant reason async flush/transfer buy nothing: compose(N) blocks on
     * flush(N-1).  should_wait_src_fence already precisely captures "the source
     * buffer's render is still in flight in the ring" (src_submit_fence newer
     * than the last completed fence and still pending).  In minimal-drain mode
     * we drain only when that is true, so a compose whose source is already
     * complete proceeds immediately and overlaps the prior flush's host vsync.
     * Opt-in via "virtio_gpu_present_minimal_drain"; the blanket drain remains
     * the default and "virtio_gpu_present_no_drain" still force-skips it.
     */
    {
        int minimal_drain =
            virtio_gpu_cmdline_enabled("virtio_gpu_present_minimal_drain");
        int want_drain = should_wait_src_fence ||
            (!minimal_drain &&
             !virtio_gpu_cmdline_enabled("virtio_gpu_present_no_drain"));

        if (want_drain) {
            ret = virtio_gpu_drain_async_submit(g, 1);
            if (ret != 0) {
                ret = -EIO;
                goto out;
            }
        }
    }

    if (ctx_id == 0) {
        ret = virtio_gpu_ensure_present_context(g, &ctx_id);
        if (ret != 0)
            goto out;
        used_present_ctx = 1;
        if (virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
                                        ctx_id, src_resource_id) != 0) {
            ret = -EIO;
            goto out_drop_context;
        }
        src_attached = 1;
    }

    ret = virtio_gpu_ensure_scanout_attached(g, ctx_id, dst_resource_id);
    if (ret != 0)
        goto out_detach;

    virtio_gpu_probe_present_source(g, src, ctx_id);

    if (virtio_gpu_cmdline_enabled("virtio_gpu_present_clear_probe")) {
        uint32 pw = w < 64 ? w : 64;
        uint32 ph = h < 64 ? h : 64;
        int clear_ret = virtio_gpu_clear_texture_rect(
            g, ctx_id, dst_resource_id, dst_x, dst_y, pw, ph, 0xffffffffu);
        int dst_nonblack = clear_ret == 0 ?
            virtio_gpu_resource_host_nonblack(g, dst, ctx_id,
                                              dst_x, dst_y, pw, ph) :
            clear_ret;

        printf("virtio_gpu: present clear-probe resource=%u ctx=%u ret=%d nonblack=%d rect=%u,%u %ux%u\n",
               dst_resource_id, ctx_id, clear_ret, dst_nonblack,
               dst_x, dst_y, pw, ph);
        ret = -EOPNOTSUPP;
        goto out_detach;
    }

    if (virtio_gpu_cmdline_enabled("virtio_gpu_present_temp_blit_probe")) {
        struct virtio_gpu_resource *tmp = NULL;
        struct virtio_gpu_present_samples src_samples;
        struct virtio_gpu_present_samples tmp_samples;
        uint64 tmp_fence;
        int tmp_ret;
        int tmp_attached = 0;
        int tmp_ctx_attached = 0;
        int src_ret;
        int sample_ret;
        int matches = 0;

        tmp_ret = virtio_gpu_resource_create_3d_backing(
            g, w, h, src->format ? src->format : VIRGL_FORMAT_B8G8R8A8_UNORM,
            VIRTIO_GPU_PIPE_BIND_RENDER_TARGET |
            VIRTIO_GPU_PIPE_BIND_SAMPLER_VIEW |
            VIRTIO_GPU_PIPE_BIND_SHARED |
            VIRTIO_GPU_PIPE_BIND_LINEAR, &tmp);
        if (tmp_ret == 0) {
            tmp_ret = virtio_gpu_resource_attach_backing(g, tmp);
            tmp_attached = tmp_ret == 0;
        }
        if (tmp_ret == 0) {
            memset(tmp->backing, 0, tmp->backing_len);
            tmp_ret = virtio_gpu_resource_transfer_3d(g, tmp, 0, 0, w, h);
        }
        if (tmp_ret == 0) {
            tmp_ret = virtio_gpu_context_resource(
                g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, ctx_id, tmp->id);
            tmp_ctx_attached = tmp_ret == 0;
        }
        if (tmp_ret == 0) {
            spin_lock(&g->lock);
            tmp_fence = ++g->next_fence_id;
            spin_unlock(&g->lock);
            if (use_copy_transfer) {
                tmp_ret = virtio_gpu_submit_copy_transfer_rect(
                    g, ctx_id, tmp, 0, 0, w, h, src, src_x, src_y,
                    tmp_fence);
            } else if (use_blit) {
                tmp_ret = virtio_gpu_submit_blit_rect(
                    g, ctx_id, tmp->id, tmp->format, 0, 0, w, h,
                    src_resource_id, src->format, src_x, src_y, tmp_fence);
            } else {
                tmp_ret = virtio_gpu_submit_copy_rect(
                    g, ctx_id, tmp->id, 0, 0, w, h,
                    src_resource_id, src_x, src_y, tmp_fence);
            }
        }
        src_ret = virtio_gpu_resource_host_samples(g, src, ctx_id,
                                                   src_x, src_y, w, h,
                                                   &src_samples);
        sample_ret = tmp_ret == 0 ?
            virtio_gpu_resource_host_samples(g, tmp, ctx_id, 0, 0, w, h,
                                             &tmp_samples) : tmp_ret;
        if (src_ret == 0 && sample_ret == 0)
            matches = virtio_gpu_present_sample_matches(&src_samples,
                                                        &tmp_samples);
        printf("virtio_gpu: present temp-%s-probe create=%d attached=%d ctx_attached=%d sample=%d matches=%d src_nonblack=%u tmp_nonblack=%u src=%08x,%08x,%08x,%08x,%08x tmp=%08x,%08x,%08x,%08x,%08x\n",
               use_copy_transfer ? "copy-transfer" :
                                   (use_blit ? "blit" : "copy"),
               tmp_ret, tmp_attached,
               tmp_ctx_attached, sample_ret, matches,
               src_ret == 0 ? src_samples.nonblack : 0,
               sample_ret == 0 ? tmp_samples.nonblack : 0,
               src_ret == 0 ? src_samples.p[0] : 0,
               src_ret == 0 ? src_samples.p[1] : 0,
               src_ret == 0 ? src_samples.p[2] : 0,
               src_ret == 0 ? src_samples.p[3] : 0,
               src_ret == 0 ? src_samples.p[4] : 0,
               sample_ret == 0 ? tmp_samples.p[0] : 0,
               sample_ret == 0 ? tmp_samples.p[1] : 0,
               sample_ret == 0 ? tmp_samples.p[2] : 0,
               sample_ret == 0 ? tmp_samples.p[3] : 0,
               sample_ret == 0 ? tmp_samples.p[4] : 0);
        if (tmp_ctx_attached)
            (void)virtio_gpu_context_resource(
                g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, ctx_id, tmp->id);
        if (tmp != NULL)
            (void)virtio_gpu_resource_unref(g, tmp);
        ret = -EOPNOTSUPP;
        goto out_detach;
    }

    ret = virtio_gpu_try_pageflip_copy(g, src, dst, ctx_id, src_x, src_y,
                                       dst_x, dst_y, w, h,
                                       use_copy_transfer, use_blit);
    if (ret == 0) {
        spin_lock(&g->lock);
        virtio_gpu_note_diagnostic_present_locked(
            g, ctx_id, src_resource_id, src_x, src_y, dst_x, dst_y, w, h);
        spin_unlock(&g->lock);
        goto out_detach;
    }

    if (g->bound_scanout_resource_id != dst_resource_id) {
        if (virtio_gpu_set_scanout(g, 0, dst, 0, 0, dst_width,
                                   dst_height) != 0) {
            ret = -EIO;
            goto out_detach;
        }
        g->bound_scanout_resource_id = dst_resource_id;
    }

    spin_lock(&g->lock);
    fence_id = ++g->next_fence_id;
    spin_unlock(&g->lock);
    if (unbind_scanout_copy &&
        g->bound_scanout_resource_id == dst_resource_id) {
        if (virtio_gpu_set_scanout(g, 0, NULL, 0, 0, 0, 0) != 0) {
            ret = -EIO;
            goto out_detach;
        }
        g->bound_scanout_resource_id = 0;
    }
    if (use_copy_transfer) {
        ret = virtio_gpu_submit_copy_transfer_rect(g, ctx_id, dst, dst_x,
                                                   dst_y, w, h, src, src_x,
                                                   src_y, fence_id);
    } else if (use_blit) {
        ret = virtio_gpu_submit_blit_rect(g, ctx_id, dst_resource_id,
                                          dst->format, dst_x, dst_y, w, h,
                                          src_resource_id, src->format,
                                          src_x, src_y, fence_id);
    } else {
        ret = virtio_gpu_submit_copy_rect(g, ctx_id, dst_resource_id,
                                          dst_x, dst_y, w, h,
                                          src_resource_id, src_x, src_y,
                                          fence_id);
    }
    if (validate_copy && ret == 0 &&
        copy_validation_state < 0 &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_present_force_copy")) {
        ret = -EOPNOTSUPP;
        goto out_detach;
    }
    if (validate_copy && ret == 0 && copy_validation_state == 0 &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_present_force_copy")) {
        struct virtio_gpu_present_samples src_samples;
        struct virtio_gpu_present_samples dst_samples;
        int src_ret = virtio_gpu_resource_host_samples(
            g, src, ctx_id, src_x, src_y, w, h, &src_samples);
        int dst_ret = virtio_gpu_resource_host_samples(
            g, dst, ctx_id, dst_x, dst_y, w, h, &dst_samples);
        int matches = (src_ret == 0 && dst_ret == 0) ?
            virtio_gpu_present_sample_matches(&src_samples, &dst_samples) : 0;
        int required_matches =
            (src_ret == 0 && src_samples.unique_nonblack >= 2 &&
             src_samples.nonblack >= 3) ? 2 : 1;

        if (src_ret != 0 || dst_ret != 0 ||
            (src_samples.nonblack > 0 && matches < required_matches)) {
            copy_validation_state = -1;
            if (copy_validation_logs < 4) {
                printf("virtio_gpu: virgl resource-copy validation failed src_ret=%d dst_ret=%d src_nonblack=%u dst_nonblack=%u matches=%d required=%d src=%08x,%08x,%08x,%08x,%08x dst=%08x,%08x,%08x,%08x,%08x; falling back to readback present\n",
                       src_ret, dst_ret,
                       src_ret == 0 ? src_samples.nonblack : 0,
                       dst_ret == 0 ? dst_samples.nonblack : 0,
                       matches, required_matches,
                       src_ret == 0 ? src_samples.p[0] : 0,
                       src_ret == 0 ? src_samples.p[1] : 0,
                       src_ret == 0 ? src_samples.p[2] : 0,
                       src_ret == 0 ? src_samples.p[3] : 0,
                       src_ret == 0 ? src_samples.p[4] : 0,
                       dst_ret == 0 ? dst_samples.p[0] : 0,
                       dst_ret == 0 ? dst_samples.p[1] : 0,
                       dst_ret == 0 ? dst_samples.p[2] : 0,
                       dst_ret == 0 ? dst_samples.p[3] : 0,
                       dst_ret == 0 ? dst_samples.p[4] : 0);
                copy_validation_logs++;
            }
            ret = -EOPNOTSUPP;
            goto out_detach;
        }
        if (matches >= required_matches)
            copy_validation_state = 1;
    }
    if (ret == 0 &&
        (unbind_scanout_copy ||
         rearm_scanout_copy)) {
        /*
         * Some QEMU/virgl hosts need a no-mode-change SET_SCANOUT refresh hint
         * after a 3D copy into the already-bound scanout.  It is expensive on
         * WSL/NVIDIA and the resource flush is sufficient there, so keep it
         * available as virtio_gpu_rearm_scanout_copy=1 rather than defaulting
         * to it every frame.
         */
        ret = virtio_gpu_set_scanout(g, 0, dst, 0, 0, dst_width,
                                     dst_height) == 0 ? 0 : -EIO;
        if (ret == 0)
            g->bound_scanout_resource_id = dst_resource_id;
    }
    if (ret == 0)
        virtio_gpu_probe_present_dst(g, dst, ctx_id, dst_x, dst_y, w, h);
    if (ret == 0)
        ret = virtio_gpu_resource_flush(g, dst, dst_x, dst_y, w, h) == 0 ?
            0 : -EIO;
    if (ret == 0) {
        spin_lock(&g->lock);
        virtio_gpu_note_diagnostic_present_locked(
            g, ctx_id, src_resource_id, src_x, src_y, dst_x, dst_y, w, h);
        spin_unlock(&g->lock);
    }

out_detach:
    if (src_attached)
        (void)virtio_gpu_context_resource(
            g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, ctx_id, src_resource_id);
out_drop_context:
    if (ret != 0 && used_present_ctx)
        virtio_gpu_drop_present_context(g, ctx_id);
out:
    virtio_gpu_op_unlock(g);
    return ret;
}

int virtio_gpu_copy_resource_to_resource(uint32 src_resource_id,
                                         uint32 dst_resource_id,
                                         uint32 src_x, uint32 src_y,
                                         uint32 dst_x, uint32 dst_y,
                                         uint32 w, uint32 h)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *src;
    struct virtio_gpu_resource *dst;
    struct virtio_gpu_context *ctx;
    uint32 ctx_id = 0;
    uint64 src_submit_fence = 0;
    uint64 completed_fence = 0;
    uint64 fence_id;
    int should_wait_src_fence = 0;
    int src_attached = 0;
    int dst_attached = 0;
    int used_present_ctx = 0;
    int ret = -EIO;
    static int copy_logs;
    int use_copy_transfer =
        virtio_gpu_cmdline_enabled("virtio_gpu_present_copy_transfer");
    int use_blit = virtio_gpu_cmdline_enabled("virtio_gpu_present_blit") &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_present_copy_region");

    if (src_resource_id == 0 || dst_resource_id == 0 ||
        src_resource_id == dst_resource_id || w == 0 || h == 0)
        return -EINVAL;
    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_PRESENT_COPY);
    spin_lock(&g->lock);
    src = virtio_gpu_lookup_resource_locked(g, src_resource_id);
    dst = virtio_gpu_lookup_resource_locked(g, dst_resource_id);
    if (src == NULL || dst == NULL || !src->attached || !dst->attached ||
        src_x > src->width || w > src->width - src_x ||
        src_y > src->height || h > src->height - src_y ||
        dst_x > dst->width || w > dst->width - dst_x ||
        dst_y > dst->height || h > dst->height - dst_y) {
        spin_unlock(&g->lock);
        ret = -EINVAL;
        goto out;
    }
    if (src->ctx_id != 0) {
        ctx = virtio_gpu_lookup_context_locked(g, src->ctx_id);
        if (ctx != NULL && !ctx->failed)
            ctx_id = src->ctx_id;
    }
    src_submit_fence = src->last_submit_fence;
    completed_fence = g->stats.last_fence;
    should_wait_src_fence = src_submit_fence != 0 &&
        src_submit_fence > completed_fence &&
        virtio_gpu_async_pending(g) &&
        virtio_gpu_async_newest_fence(g) <= src_submit_fence;
    if (copy_logs < 8) {
        printf("virtio_gpu: resource-copy-to-resource src=%u dst=%u ctx=%u rect src=%u,%u dst=%u,%u %ux%u\n",
               src_resource_id, dst_resource_id, ctx_id, src_x, src_y,
               dst_x, dst_y, w, h);
        copy_logs++;
    }
    spin_unlock(&g->lock);

    if (should_wait_src_fence ||
        !virtio_gpu_cmdline_enabled("virtio_gpu_present_no_drain")) {
        ret = virtio_gpu_drain_async_submit(g, 1);
        if (ret != 0) {
            ret = -EIO;
            goto out;
        }
    }

    if (ctx_id == 0) {
        ret = virtio_gpu_ensure_present_context(g, &ctx_id);
        if (ret != 0)
            goto out;
        used_present_ctx = 1;
        if (virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
                                        ctx_id, src_resource_id) != 0) {
            ret = -EIO;
            goto out_drop_context;
        }
        src_attached = 1;
    }
    if (virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
                                    ctx_id, dst_resource_id) != 0) {
        ret = -EIO;
        goto out_detach_src;
    }
    dst_attached = 1;

    spin_lock(&g->lock);
    src = virtio_gpu_lookup_resource_locked(g, src_resource_id);
    dst = virtio_gpu_lookup_resource_locked(g, dst_resource_id);
    fence_id = ++g->next_fence_id;
    spin_unlock(&g->lock);
    if (src == NULL || dst == NULL) {
        ret = -EINVAL;
        goto out_detach_dst;
    }
    ret = virtio_gpu_submit_present_copy_method(
        g, ctx_id, dst, dst_x, dst_y, w, h, src, src_x, src_y, fence_id,
        use_copy_transfer, use_blit);

out_detach_dst:
    if (dst_attached)
        (void)virtio_gpu_context_resource(
            g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, ctx_id, dst_resource_id);
out_detach_src:
    if (src_attached)
        (void)virtio_gpu_context_resource(
            g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, ctx_id, src_resource_id);
out_drop_context:
    if (ret != 0 && used_present_ctx)
        virtio_gpu_drop_present_context(g, ctx_id);
out:
    virtio_gpu_op_unlock(g);
    return ret;
}

static void virtio_gpu_copy_fb_rect_to_resource(
    struct virtio_gpu_resource *res, volatile void *fb, uint32 src_pitch,
    uint32 x, uint32 y, uint32 w, uint32 h)
{
    uint8 *dst_base;
    volatile uint8 *src_base;

    if (res == NULL || res->backing == NULL || fb == NULL)
        return;

    dst_base = (uint8 *)res->backing;
    src_base = (volatile uint8 *)fb;
    for (uint32 row = 0; row < h; row++) {
        volatile uint32 *src =
            (volatile uint32 *)(src_base + (uint64)(y + row) * src_pitch +
                                (uint64)x * sizeof(uint32));
        uint32 *dst =
            (uint32 *)(dst_base + (uint64)(y + row) * res->width *
                       sizeof(uint32) + (uint64)x * sizeof(uint32));
        for (uint32 col = 0; col < w; col++)
            dst[col] = src[col];
    }
}

void virtio_gpu_present_fb_rect(volatile void *fb, uint32 src_pitch,
                                uint32 x, uint32 y, uint32 w, uint32 h)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;
    struct virtio_gpu_resource *target;
    uint32 target_slot = 0;
    int target_is_flip = 0;

    if (!g->initialized || fb == NULL || w == 0 || h == 0)
        return;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_FB_PRESENT);
    res = g->scanout_resource;
    if (res == NULL || !res->attached || res->backing == NULL)
        goto out;
    if (x >= res->width || y >= res->height)
        goto out;
    if ((uint64)x + w > res->width)
        w = res->width - x;
    if ((uint64)y + h > res->height)
        h = res->height - y;
    if (w == 0 || h == 0)
        goto out;

    target = res;
    if (g->bound_scanout_resource_id != res->id) {
        struct virtio_gpu_resource *bound = NULL;
        uint32 bound_id = g->bound_scanout_resource_id;

        spin_lock(&g->lock);
        bound = virtio_gpu_lookup_resource_locked(g, bound_id);
        if (bound == NULL || !bound->attached || bound->backing == NULL ||
            bound->width != res->width || bound->height != res->height ||
            !virtio_gpu_present_flip_slot_for_id(g, bound->id,
                                                 &target_slot)) {
            bound = NULL;
        }
        spin_unlock(&g->lock);

        if (bound != NULL) {
            target = bound;
            target_is_flip = 1;
        } else {
            if (virtio_gpu_set_scanout(g, 0, res, 0, 0, res->width,
                                       res->height) != 0)
                goto out;
            g->bound_scanout_resource_id = res->id;
            virtio_gpu_invalidate_present_flip_slot(g, 0);
            virtio_gpu_invalidate_present_flip_slot(g, 1);
        }
    }

    if ((void *)fb != res->backing ||
        src_pitch != res->width * sizeof(uint32))
        virtio_gpu_copy_fb_rect_to_resource(res, fb, src_pitch, x, y, w, h);
    g->present_base_generation++;
    if (g->present_base_generation == 0)
        g->present_base_generation = 1;

    if (target_is_flip) {
        for (uint32 i = 0; i < 2; i++) {
            struct virtio_gpu_resource *flip = g->present_flip_resource[i];

            if (flip == NULL || flip == target || !g->present_flip_valid[i] ||
                flip->width != res->width || flip->height != res->height)
                continue;
            virtio_gpu_copy_fb_rect_to_resource(flip, fb, src_pitch,
                                                x, y, w, h);
            if (virtio_gpu_resource_transfer_scanout(g, flip, x, y, w, h) != 0) {
                virtio_gpu_invalidate_present_flip_slot(g, i);
            } else {
                g->present_flip_base_generation[i] =
                    g->present_base_generation;
            }
        }
        virtio_gpu_copy_fb_rect_to_resource(target, fb, src_pitch, x, y, w, h);
        g->present_flip_base_generation[target_slot] =
            g->present_base_generation;
    }

    if (virtio_gpu_resource_transfer_scanout(g, target, x, y, w, h) != 0)
        goto out;
    virtio_gpu_resource_flush(g, target, x, y, w, h);

out:
    virtio_gpu_op_unlock(g);
}

int virtio_gpu_read_current_scanout(uint32 x, uint32 y, uint32 w, uint32 h,
                                    void *dst, uint32 dst_pitch,
                                    uint32 *screen_width,
                                    uint32 *screen_height,
                                    uint32 *screen_pitch)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res = NULL;
    struct virtio_gpu_resource *present = NULL;
    uint32 src_pitch;
    uint8 *src_base;
    uint8 *dst_base = (uint8 *)dst;
    uint32 present_resource_id = 0;
    uint32 present_ctx_id = 0;
    uint32 present_src_x = 0;
    uint32 present_src_y = 0;
    uint32 present_dst_x = 0;
    uint32 present_dst_y = 0;
    uint32 present_w = 0;
    uint32 present_h = 0;
    uint32 read_ctx_id = 0;
    int ret = 0;

    if (dst == NULL || w == 0 || h == 0 || dst_pitch < w * sizeof(uint32))
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_PROBE);
    spin_lock(&g->lock);
    res = g->scanout_resource;
    present_resource_id = g->diagnostic_present_resource_id;
    present_ctx_id = g->diagnostic_present_ctx_id;
    present_src_x = g->diagnostic_present_src_x;
    present_src_y = g->diagnostic_present_src_y;
    present_dst_x = g->diagnostic_present_dst_x;
    present_dst_y = g->diagnostic_present_dst_y;
    present_w = g->diagnostic_present_w;
    present_h = g->diagnostic_present_h;
    spin_unlock(&g->lock);

    if (res == NULL || !res->attached ||
        (res->backing == NULL &&
         (res->pages == NULL || res->npages == 0)) ||
        res->width == 0 || res->height == 0 ||
        x > res->width || w > res->width - x ||
        y > res->height || h > res->height - y) {
        ret = -EINVAL;
        goto out;
    }

    src_pitch = res->width * sizeof(uint32);
    if (res->backing != NULL &&
        (uint64)src_pitch * res->height > res->backing_len) {
        ret = -EINVAL;
        goto out;
    }

    if (virtio_gpu_async_pending(g) &&
        virtio_gpu_drain_async_submit(g, 1) != 0) {
        ret = -EIO;
        goto out;
    }

    if (res->is_3d) {
        struct virtio_gpu_transfer_host_3d *transfer =
            (struct virtio_gpu_transfer_host_3d *)g->cmd_page;
        struct virtio_gpu_ctrl_hdr *resp =
            (struct virtio_gpu_ctrl_hdr *)g->resp_page;

        read_ctx_id = res->ctx_id;
        if (read_ctx_id == 0) {
            ret = virtio_gpu_ensure_present_context(g, &read_ctx_id);
            if (ret != 0)
                goto out;
            ret = virtio_gpu_context_resource(
                g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, read_ctx_id, res->id);
            if (ret != 0) {
                ret = -EIO;
                goto out;
            }
        }
        memset(transfer, 0, sizeof(*transfer));
        transfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
        transfer->hdr.ctx_id = read_ctx_id;
        transfer->box.x = x;
        transfer->box.y = y;
        transfer->box.z = 0;
        transfer->box.w = w;
        transfer->box.h = h;
        transfer->box.d = 1;
        transfer->offset = (uint64)y * src_pitch +
                           (uint64)x * sizeof(uint32);
        transfer->resource_id = res->id;
        transfer->level = 0;
        transfer->stride = src_pitch;
        transfer->layer_stride = (uint64)src_pitch * res->height;
        if (virtio_gpu_submit(g, transfer, sizeof(*transfer), NULL, 0,
                              false, resp, sizeof(*resp),
                              VIRTIO_GPU_RESP_OK_NODATA) != 0) {
            ret = -EIO;
            goto out;
        }
    }

    if (res->backing != NULL) {
        src_base = (uint8 *)res->backing;
        for (uint32 row = 0; row < h; row++) {
            memcpy(dst_base + (uint64)row * dst_pitch,
                   src_base + (uint64)(y + row) * src_pitch +
                       (uint64)x * sizeof(uint32),
                   (size_t)w * sizeof(uint32));
        }
    } else {
        for (uint32 row = 0; row < h; row++) {
            uint32 *dst_row =
                (uint32 *)(dst_base + (uint64)row * dst_pitch);

            for (uint32 col = 0; col < w; col++)
                dst_row[col] =
                    virtio_gpu_resource_sample_pixel(res, x + col, y + row);
        }
    }

    spin_lock(&g->lock);
    present = virtio_gpu_lookup_resource_locked(g, present_resource_id);
    spin_unlock(&g->lock);
    if (present != NULL && present->attached &&
        (present->backing != NULL ||
         (present->pages != NULL && present->npages != 0)) &&
        present_w != 0 && present_h != 0 &&
        present_src_x <= present->width &&
        present_w <= present->width - present_src_x &&
        present_src_y <= present->height &&
        present_h <= present->height - present_src_y &&
        present_dst_x <= res->width &&
        present_w <= res->width - present_dst_x &&
        present_dst_y <= res->height &&
        present_h <= res->height - present_dst_y) {
        uint32 ix0 = x > present_dst_x ? x : present_dst_x;
        uint32 iy0 = y > present_dst_y ? y : present_dst_y;
        uint32 req_x1 = x + w;
        uint32 req_y1 = y + h;
        uint32 pres_x1 = present_dst_x + present_w;
        uint32 pres_y1 = present_dst_y + present_h;
        uint32 ix1 = req_x1 < pres_x1 ? req_x1 : pres_x1;
        uint32 iy1 = req_y1 < pres_y1 ? req_y1 : pres_y1;

        if (ix1 > ix0 && iy1 > iy0) {
            struct virtio_gpu_transfer_host_3d *transfer =
                (struct virtio_gpu_transfer_host_3d *)g->cmd_page;
            struct virtio_gpu_ctrl_hdr *resp =
                (struct virtio_gpu_ctrl_hdr *)g->resp_page;
            uint32 present_pitch = present->width * sizeof(uint32);
            uint32 src_start_x;
            uint32 src_start_y;

            if (present_ctx_id == 0) {
                ret = virtio_gpu_ensure_present_context(g, &present_ctx_id);
                if (ret != 0)
                    goto out;
                ret = virtio_gpu_context_resource(
                    g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
                    present_ctx_id, present->id);
                if (ret != 0) {
                    ret = -EIO;
                    goto out;
                }
            }
            memset(transfer, 0, sizeof(*transfer));
            transfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
            transfer->hdr.ctx_id = present_ctx_id;
            src_start_x = present_src_x + (ix0 - present_dst_x);
            src_start_y = present_src_y + (iy0 - present_dst_y);
            transfer->box.x = src_start_x;
            transfer->box.y = src_start_y;
            transfer->box.z = 0;
            transfer->box.w = ix1 - ix0;
            transfer->box.h = iy1 - iy0;
            transfer->box.d = 1;
            transfer->offset = (uint64)transfer->box.y * present_pitch +
                               (uint64)transfer->box.x * sizeof(uint32);
            transfer->resource_id = present->id;
            transfer->level = 0;
            transfer->stride = present_pitch;
            transfer->layer_stride = (uint64)present_pitch * present->height;
            if (virtio_gpu_submit(g, transfer, sizeof(*transfer), NULL, 0,
                                  false, resp, sizeof(*resp),
                                  VIRTIO_GPU_RESP_OK_NODATA) != 0) {
                ret = -EIO;
                goto out;
            }
            for (uint32 row = 0; row < iy1 - iy0; row++) {
                uint32 *dst_row =
                    (uint32 *)(dst_base +
                               (uint64)(iy0 - y + row) * dst_pitch +
                               (uint64)(ix0 - x) * sizeof(uint32));

                for (uint32 col = 0; col < ix1 - ix0; col++)
                    dst_row[col] = virtio_gpu_resource_sample_pixel(
                        present, src_start_x + col, src_start_y + row);
            }
        }
    }

    if (screen_width != NULL)
        *screen_width = res->width;
    if (screen_height != NULL)
        *screen_height = res->height;
    if (screen_pitch != NULL)
        *screen_pitch = src_pitch;

out:
    virtio_gpu_op_unlock(g);
    return ret;
}

