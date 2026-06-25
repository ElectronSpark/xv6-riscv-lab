}

static int virtio_gpu_create_context(struct virtio_gpu *g, uint32 ctx_id,
                                     uint32 capset_id, uint32 context_init,
                                     const char *name)
{
    struct virtio_gpu_ctx_create *cmd =
        (struct virtio_gpu_ctx_create *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    uint32 name_len = 0;

    (void)capset_id;
    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    cmd->hdr.ctx_id = ctx_id;
    if (g->driver_features0 & (1u << VIRTIO_GPU_F_CONTEXT_INIT))
        cmd->context_init = context_init;
    while (name[name_len] && name_len < sizeof(cmd->debug_name))
        name_len++;
    memcpy(cmd->debug_name, name, name_len);
    cmd->nlen = name_len;

    return virtio_gpu_submit(g, cmd, sizeof(*cmd), NULL, 0, false, resp,
                             sizeof(*resp), VIRTIO_GPU_RESP_OK_NODATA);
}

static int virtio_gpu_destroy_context(struct virtio_gpu *g, uint32 ctx_id)
{
    struct virtio_gpu_ctx_destroy *cmd =
        (struct virtio_gpu_ctx_destroy *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;

    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    cmd->hdr.ctx_id = ctx_id;

    return virtio_gpu_submit(g, cmd, sizeof(*cmd), NULL, 0, false, resp,
                             sizeof(*resp), VIRTIO_GPU_RESP_OK_NODATA);
}

static int virtio_gpu_context_resource_submit(struct virtio_gpu *g,
                                              uint32 type, uint32 ctx_id,
                                              uint32 resource_id,
                                              int mixed_async_wait)
{
    struct virtio_gpu_ctx_resource *cmd =
        (struct virtio_gpu_ctx_resource *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;

    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = type;
    cmd->hdr.ctx_id = ctx_id;
    cmd->resource_id = resource_id;

    if (mixed_async_wait && virtio_gpu_async_pending(g))
        return virtio_gpu_submit_mixed_async(g, cmd, sizeof(*cmd), NULL, 0,
                                             false, resp, sizeof(*resp),
                                             VIRTIO_GPU_RESP_OK_NODATA, NULL);
    return virtio_gpu_submit(g, cmd, sizeof(*cmd), NULL, 0, false, resp,
                             sizeof(*resp), VIRTIO_GPU_RESP_OK_NODATA);
}

static int virtio_gpu_context_resource(struct virtio_gpu *g, uint32 type,
                                       uint32 ctx_id, uint32 resource_id)
{
    return virtio_gpu_context_resource_submit(g, type, ctx_id, resource_id, 0);
}

static int virtio_gpu_context_resource_mixed_async(
    struct virtio_gpu *g, uint32 type, uint32 ctx_id, uint32 resource_id)
{
    return virtio_gpu_context_resource_submit(g, type, ctx_id, resource_id, 1);
}

static int virtio_gpu_submit_3d(struct virtio_gpu *g, uint32 ctx_id,
                                const uint32 *cmds, uint32 nr_dwords,
                                uint64 fence_id)
{
    struct virtio_gpu_cmd_submit *cmd =
        (struct virtio_gpu_cmd_submit *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    uint32 bytes = nr_dwords * sizeof(uint32);

    if (nr_dwords == 0 || bytes > PGSIZE * 64) {
        virtio_gpu_count_failure(g);
        return -1;
    }

    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    cmd->hdr.ctx_id = ctx_id;
    if (fence_id != 0) {
        cmd->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
        cmd->hdr.fence_id = fence_id;
    }
    cmd->size = bytes;

    if (virtio_gpu_submit(g, cmd, sizeof(*cmd), (void *)cmds, bytes, false,
                          resp, sizeof(*resp),
                          VIRTIO_GPU_RESP_OK_NODATA) != 0)
        return -1;

    if (fence_id != 0) {
        if (resp->fence_id != fence_id) {
            virtio_gpu_count_failure(g);
            printf("virtio_gpu: fence mismatch got=%lu expected=%lu\n",
                   resp->fence_id, fence_id);
            return -1;
        }
        spin_lock(&g->lock);
        g->stats.fences++;
        g->stats.last_fence = fence_id;
        spin_unlock(&g->lock);
    }
    return 0;
}

static int virtio_gpu_submit_3d_async_prepare(
    struct virtio_gpu *g, uint32 ctx_id, const uint32 *cmds,
    uint32 nr_dwords, uint64 fence_id, struct virtio_gpu_async_submit *prep)
{
    struct virtio_gpu_cmd_submit *cmd;
    struct virtio_gpu_ctrl_hdr *resp;
    uint32 bytes = nr_dwords * sizeof(uint32);
    uint32 order;
    void *data;

    memset(prep, 0, sizeof(*prep));
    if (nr_dwords == 0 || bytes > PGSIZE * 64) {
        virtio_gpu_count_failure(g);
        return -1;
    }
    {
        uint32 alloc_len;
        int backing_order = virtio_gpu_backing_order(bytes, &alloc_len);
        if (backing_order < 0)
            return -1;
        order = (uint32)backing_order;
    }

    cmd = kalloc();
    resp = kalloc();
    if (cmd == NULL || resp == NULL) {
        if (cmd != NULL)
            kfree(cmd);
        if (resp != NULL)
            kfree(resp);
        return -ENOMEM;
    }
    data = page_alloc(order, PAGE_TYPE_ANON);
    if (data == NULL) {
        kfree(cmd);
        kfree(resp);
        return -ENOMEM;
    }

    memset(cmd, 0, sizeof(*cmd));
    memset(resp, 0, sizeof(*resp));
    memcpy(data, cmds, bytes);
    cmd->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    cmd->hdr.ctx_id = ctx_id;
    if (fence_id != 0) {
        cmd->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
        cmd->hdr.fence_id = fence_id;
    }
    cmd->size = bytes;

    prep->ctx_id = ctx_id;
    prep->fence_id = fence_id;
    prep->type = VIRTIO_GPU_CMD_SUBMIT_3D;
    prep->expected = VIRTIO_GPU_RESP_OK_NODATA;
    prep->cmd = cmd;
    prep->cmd_len = sizeof(*cmd);
    prep->data = data;
    prep->data_len = bytes;
    prep->data_order = order;
    prep->resp = resp;
    prep->resp_len = sizeof(*resp);
    return 0;
}

static int virtio_gpu_submit_3d_async_post_prepared(
    struct virtio_gpu *g, struct virtio_gpu_async_submit *prep)
{
    return virtio_gpu_async_post_prepared(
        g, prep, VIRTIO_GPU_ASYNC_REASON_SUBMIT_3D);
}

static void virtio_gpu_smoke_context(struct virtio_gpu *g)
{
    uint32 ctx_id = 1;
    uint32 nop = VIRGL_CMD0(VIRGL_CCMD_NOP, 0, 0);
    uint64 fence_id;
    struct virtio_gpu_resource *res = NULL;
    int created = 0;
    int attached_to_ctx = 0;

    if (!g->virgl_capset_id)
        return;
    if (!(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL))) {
        printf("virtio_gpu: virgl capset present but feature not negotiated\n");
        return;
    }

    if (virtio_gpu_create_context(g, ctx_id, g->virgl_capset_id,
                                  g->virgl_capset_id,
                                  "xv6-virgl-smoke") != 0) {
        printf("virtio_gpu: 3D context create failed\n");
        return;
    }
    created = 1;
    if (virtio_gpu_resource_create_2d(g, VIRTIO_GPU_SMOKE_WIDTH,
                                      VIRTIO_GPU_SMOKE_HEIGHT,
                                      VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                                      &res) != 0) {
        printf("virtio_gpu: 3D context resource create failed\n");
        goto out;
    }
    if (virtio_gpu_resource_attach_backing(g, res) != 0) {
        printf("virtio_gpu: 3D context resource backing failed\n");
        goto out;
    }
    if (virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
                                    ctx_id, res->id) != 0) {
        printf("virtio_gpu: 3D context attach resource failed\n");
        goto out;
    }
    attached_to_ctx = 1;

    spin_lock(&g->lock);
    fence_id = ++g->next_fence_id;
    spin_unlock(&g->lock);
    if (virtio_gpu_submit_3d(g, ctx_id, &nop, 1, fence_id) != 0) {
        printf("virtio_gpu: 3D NOP submit failed\n");
        goto out;
    }

    if (virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE,
                                    ctx_id, res->id) != 0) {
        printf("virtio_gpu: 3D context detach resource failed\n");
        goto out;
    }
    attached_to_ctx = 0;
    if (virtio_gpu_destroy_context(g, ctx_id) != 0) {
        printf("virtio_gpu: 3D context destroy failed\n");
        goto out;
    }
    created = 0;

out:
    if (attached_to_ctx && res)
        virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE,
                                    ctx_id, res->id);
    if (res)
        virtio_gpu_resource_unref(g, res);
    if (created)
        virtio_gpu_destroy_context(g, ctx_id);
}

/*
 * One-shot, init-time probe that actually exercises the host-visible blob
 * map path against the host.  Without it the HOST_VISIBLE capability gate is
 * circular: GETPARAM(HOST_VISIBLE) only reports true after a RESOURCE_MAP_BLOB
 * succeeds, but nothing attempts that map until the capability is advertised.
 * This sends exactly one RESOURCE_CREATE_BLOB and, if creation succeeds, one
 * RESOURCE_MAP_BLOB so the host itself decides whether mappable host-visible
 * blobs are supported.  The advertised capability reflects a real, verified
 * mapping instead of a guess.
 * It only runs when the host has negotiated a host-visible blob aperture, so
 * backends without one keep failing closed with no extra traffic.
 */
static void virtio_gpu_smoke_host_visible_map(struct virtio_gpu *g)
{
    struct fb_gpu_virgl_blob_create req;
    struct virtio_gpu_resource *res = NULL;
    uint32 ctx_id = 2;
    uint32 map_info = 0;
    int created_ctx = 0;
    int created_blob = 0;
    int create_ret;
    int map_ret = -EOPNOTSUPP;

    if (!virtio_gpu_host_visible_operational(g) || g->host_visible_mapped_once)
        return;

    if (virtio_gpu_create_context(g, ctx_id, g->virgl_capset_id,
                                  g->virgl_capset_id,
                                  "xv6-hostvis-probe") != 0) {
        printf("virtio_gpu: host-visible map probe: context create failed\n");
        return;
    }
    created_ctx = 1;

    memset(&req, 0, sizeof(req));
    req.ctx_id = ctx_id;
    req.blob_mem = VIRTIO_GPU_BLOB_MEM_HOST3D;
    req.blob_flags = VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE;
    req.blob_id = 1; /* host3d mappable blobs require a nonzero blob id */
    req.size = PGSIZE;

    create_ret = virtio_gpu_resource_create_blob(g, 0, 0, &req, &res, 1);
    if (create_ret != 0 || res == NULL) {
        printf("virtio_gpu: host-visible map probe: create=%d (host declined mappable host3d blob)\n",
               create_ret);
        goto out;
    }
    created_blob = 1;

    /*
     * map_blob sets g->host_visible_blob_ok on success, which is exactly what
     * GETPARAM(HOST_VISIBLE) reports; leave that flag set (unmap keeps it
     * sticky) so the capability mirrors a mapping the host actually honoured.
     */
    map_ret = virtio_gpu_resource_map_blob(g, res, 0, &map_info);
    printf("virtio_gpu: host-visible map probe: create=%d map=%d map_info=0x%x advertised=%d\n",
           create_ret, map_ret, map_info,
           virtio_gpu_host_visible_ready(g) ? 1 : 0);

    if (map_ret == 0)
        virtio_gpu_resource_unmap_blob(g, res);

out:
    if (created_blob && res != NULL)
        virtio_gpu_resource_unref(g, res);
    if (created_ctx)
        virtio_gpu_destroy_context(g, ctx_id);
}
