enum {
    VIRTIO_GPU_DRM_CAPSET_VERSION = 1,
    VIRTIO_GPU_DRM_CAPSET_SIZE = 160,
};

enum virtio_gpu_user_capset_policy {
    VIRTIO_GPU_USER_CAPSET_UNSUPPORTED,
    VIRTIO_GPU_USER_CAPSET_HOST_CREATABLE,
    VIRTIO_GPU_USER_CAPSET_QUERY_ONLY,
};

static enum virtio_gpu_user_capset_policy
virtio_gpu_user_capset_policy_for(uint32 capset_id)
{
    switch (capset_id) {
    case VIRTIO_GPU_CAPSET_VIRGL:
    case VIRTIO_GPU_CAPSET_VIRGL2:
        return VIRTIO_GPU_USER_CAPSET_HOST_CREATABLE;
    case VIRTIO_GPU_CAPSET_DRM:
        return VIRTIO_GPU_USER_CAPSET_QUERY_ONLY;
    default:
        return VIRTIO_GPU_USER_CAPSET_UNSUPPORTED;
    }
}

int virtio_gpu_user_capset_query_only(uint32 capset_id)
{
    return virtio_gpu_user_capset_policy_for(capset_id) ==
        VIRTIO_GPU_USER_CAPSET_QUERY_ONLY;
}

static int virtio_gpu_user_fill_probe_capset(uint32 requested_capset_id,
                                             uint32 requested_capset_version,
                                             void *buf, uint32 buf_size,
                                             uint32 *capset_id,
                                             uint32 *capset_version,
                                             uint32 *capset_size)
{
    uint32 transfer_size;

    if (!virtio_gpu_user_capset_query_only(requested_capset_id))
        return -EINVAL;
    if (requested_capset_version > VIRTIO_GPU_DRM_CAPSET_VERSION)
        return -EINVAL;
    if (capset_id)
        *capset_id = VIRTIO_GPU_CAPSET_DRM;
    if (capset_version)
        *capset_version = VIRTIO_GPU_DRM_CAPSET_VERSION;
    if (capset_size)
        *capset_size = VIRTIO_GPU_DRM_CAPSET_SIZE;
    if (buf != NULL && buf_size != 0) {
        transfer_size = VIRTIO_GPU_DRM_CAPSET_SIZE;
        if (transfer_size > buf_size)
            transfer_size = buf_size;
        memset(buf, 0, transfer_size);
    }
    return 0;
}

int virtio_gpu_user_context_create(uint64 owner_id, pid_t owner_tgid,
                                   uint32 capset_id, const char *name,
                                   uint32 *ctx_id)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_context *ctx;
    struct virtio_gpu_capset *capset;
    uint32 id;
    int ret;

    if (ctx_id == NULL)
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_CONTEXT);
    spin_lock(&g->lock);
    if (capset_id == 0)
        capset_id = g->virgl_capset_id;
    if (virtio_gpu_user_capset_policy_for(capset_id) !=
        VIRTIO_GPU_USER_CAPSET_HOST_CREATABLE) {
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -EINVAL;
    }
    capset = virtio_gpu_lookup_capset_locked(g, capset_id);
    if (capset == NULL) {
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -EINVAL;
    }
    ctx = virtio_gpu_alloc_context_slot_locked(g);
    if (ctx == NULL) {
        g->stats.failures++;
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -ENOSPC;
    }
    id = g->next_context_id++;
    if (g->next_context_id == 0)
        g->next_context_id = 2;
    memset(ctx, 0, sizeof(*ctx));
    ctx->in_use = 1;
    ctx->id = id;
    ctx->capset_id = capset_id;
    ctx->owner_id = owner_id;
    ctx->owner_tgid = owner_tgid;
    spin_unlock(&g->lock);

    ret = virtio_gpu_create_context(g, id, capset_id,
                                    name ? name : "xv6-virgl");
    if (ret != 0) {
        spin_lock(&g->lock);
        ctx = virtio_gpu_lookup_context_locked(g, id);
        if (ctx != NULL)
            memset(ctx, 0, sizeof(*ctx));
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -EIO;
    }
    virtio_gpu_op_unlock(g);

    *ctx_id = id;
    return 0;
}

static void virtio_gpu_forget_context_resources_locked(struct virtio_gpu *g,
                                                       uint32 ctx_id)
{
    if (ctx_id == 0)
        return;

    for (int i = 0; i < VIRTIO_GPU_MAX_RESOURCES; i++) {
        struct virtio_gpu_resource *res = &g->resources[i];

        if (res->in_use)
            virtio_gpu_resource_forget_context_locked(res, ctx_id);
    }
}

int virtio_gpu_user_context_destroy(uint64 owner_id, pid_t owner_tgid,
                                    uint32 ctx_id)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_context *ctx;
    int ret;

    if (ctx_id == 0)
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_CONTEXT);
    spin_lock(&g->lock);
    ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
    if (ctx == NULL) {
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -ENOENT;
    }
    if (!virtio_gpu_owner_matches(ctx->owner_id, ctx->owner_tgid,
                                  owner_id, owner_tgid)) {
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -EPERM;
    }
    spin_unlock(&g->lock);

    ret = virtio_gpu_destroy_context(g, ctx_id);
    if (ret == 0) {
        spin_lock(&g->lock);
        if (g->present_scanout_ctx_id == ctx_id) {
            g->present_scanout_ctx_id = 0;
            g->present_scanout_resource_id = 0;
        }
        virtio_gpu_forget_context_resources_locked(g, ctx_id);
        ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
        if (ctx != NULL) {
            if (ctx->failed && g->stats.context_failed > 0)
                g->stats.context_failed--;
            memset(ctx, 0, sizeof(*ctx));
        }
        spin_unlock(&g->lock);
    }
    virtio_gpu_op_unlock(g);
    return ret == 0 ? 0 : -EIO;
}

int virtio_gpu_user_submit(uint64 owner_id, pid_t owner_tgid, uint32 ctx_id,
                           uint32 flags, const uint32 *cmds,
                           uint32 nr_dwords, const uint32 *resources,
                           uint32 resource_count, uint64 *fence,
                           uint64 *signaled)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_context *ctx;
    struct virtio_gpu_async_submit prep;
    uint64 fence_id;
    int ret;
    int async_submit;

    if ((flags & ~(FB_GPU_VIRGL_SUBMIT_ASYNC |
                   FB_GPU_VIRGL_SUBMIT_ALLOW_IMPORTED_RESOURCES |
                   FB_GPU_VIRGL_SUBMIT_FORCE_FAIL)) != 0 ||
        ctx_id == 0 || cmds == NULL || nr_dwords == 0 ||
        nr_dwords > (PGSIZE * 64) / sizeof(uint32))
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    spin_lock(&g->lock);
    ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
    if (ctx == NULL) {
        spin_unlock(&g->lock);
        return -ENOENT;
    }
    if (!virtio_gpu_owner_matches(ctx->owner_id, ctx->owner_tgid,
                                  owner_id, owner_tgid)) {
        spin_unlock(&g->lock);
        return -EPERM;
    }
    if (ctx->failed) {
        spin_unlock(&g->lock);
        return -EIO;
    }
    if (flags & FB_GPU_VIRGL_SUBMIT_FORCE_FAIL) {
        virtio_gpu_mark_context_failed_locked(g, ctx);
        spin_unlock(&g->lock);
        return -EIO;
    }
    spin_unlock(&g->lock);

    spin_lock(&g->lock);
    fence_id = ++g->next_fence_id;
    spin_unlock(&g->lock);

    async_submit = (flags & FB_GPU_VIRGL_SUBMIT_ASYNC) != 0;
    int allow_imported_resources =
        (flags & FB_GPU_VIRGL_SUBMIT_ALLOW_IMPORTED_RESOURCES) != 0;
    memset(&prep, 0, sizeof(prep));
    if (async_submit) {
        ret = virtio_gpu_submit_3d_async_prepare(
            g, ctx_id, cmds, nr_dwords, fence_id, &prep);
        if (ret != 0)
            return ret;
    }

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_SUBMIT_3D);
    if (resources != NULL) {
        for (uint32 i = 0; i < resource_count; i++) {
            struct virtio_gpu_resource *res;
            int already_attached = 0;
            int recorded = 0;

            if (resources[i] == 0)
                continue;

            spin_lock(&g->lock);
            res = virtio_gpu_lookup_resource_locked(g, resources[i]);
            if (res == NULL ||
                (!virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                           owner_id, owner_tgid) &&
                 !virtio_gpu_resource_context_attached_locked(res, ctx_id) &&
                 !allow_imported_resources)) {
                spin_unlock(&g->lock);
                ret = -ENOENT;
                goto out_unlock_submit;
            }
            already_attached =
                virtio_gpu_resource_context_attached_locked(res, ctx_id);
            spin_unlock(&g->lock);

            if (already_attached)
                continue;

            if (async_submit)
                ret = virtio_gpu_context_resource_mixed_async(
                    g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, ctx_id,
                    resources[i]);
            else
                ret = virtio_gpu_context_resource(
                    g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, ctx_id,
                    resources[i]);
            if (ret != 0) {
                ret = -EIO;
                goto out_unlock_submit;
            }

            spin_lock(&g->lock);
            res = virtio_gpu_lookup_resource_locked(g, resources[i]);
            if (res != NULL &&
                (allow_imported_resources ||
                 virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                          owner_id, owner_tgid)))
                recorded = virtio_gpu_resource_record_context_locked(res,
                                                                     ctx_id);
            spin_unlock(&g->lock);
            if (recorded != 0) {
                virtio_gpu_context_resource(
                    g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, ctx_id,
                    resources[i]);
                ret = recorded;
                goto out_unlock_submit;
            }
        }
    }

    if (async_submit)
        ret = virtio_gpu_submit_3d_async_post_prepared(g, &prep);
    else
        ret = virtio_gpu_submit_3d(g, ctx_id, cmds, nr_dwords, fence_id);
out_unlock_submit:
    virtio_gpu_op_unlock(g);
    if (ret != 0)
        virtio_gpu_async_submit_free(&prep);
    if (ret != 0) {
        spin_lock(&g->lock);
        ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
        virtio_gpu_mark_context_failed_locked(g, ctx);
        spin_unlock(&g->lock);
        return -EIO;
    }

    spin_lock(&g->lock);
    if (signaled)
        *signaled = g->stats.last_fence;
    if (resources != NULL) {
        for (uint32 i = 0; i < resource_count; i++) {
            struct virtio_gpu_resource *res;

            if (resources[i] == 0)
                continue;
            res = virtio_gpu_lookup_resource_locked(g, resources[i]);
            if (res == NULL)
                continue;
            if (!allow_imported_resources &&
                !virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                          owner_id, owner_tgid))
                continue;
            res->last_submit_fence = fence_id;
        }
    }
    spin_unlock(&g->lock);
    if (fence)
        *fence = fence_id;
    return 0;
}

int virtio_gpu_user_fence(uint64 wait_for, int wait, uint64 *signaled)
{
    struct virtio_gpu *g = &gpu;
    uint64 done;
    int ret = 0;

    if (!g->initialized)
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_FENCE);
    spin_lock(&g->lock);
    done = g->stats.last_fence;
    spin_unlock(&g->lock);
    if (virtio_gpu_async_pending(g) && wait && wait_for != 0) {
        ret = virtio_gpu_drain_async_until_fence(g, wait_for);
        if (ret != 0) {
            virtio_gpu_op_unlock(g);
            return -EIO;
        }
    } else if (virtio_gpu_async_pending(g) &&
               (wait || wait_for == 0 ||
                wait_for <= virtio_gpu_async_newest_fence(g))) {
        ret = virtio_gpu_drain_async_submit(g, wait);
        if (ret != 0) {
            virtio_gpu_op_unlock(g);
            return -EIO;
        }
    }
    spin_lock(&g->lock);
    done = g->stats.last_fence;
    spin_unlock(&g->lock);
    virtio_gpu_op_unlock(g);

    if (signaled)
        *signaled = done;
    if (wait && wait_for != 0 && wait_for > done)
        return -EAGAIN;
    return 0;
}

uint64 virtio_gpu_user_last_fence(void)
{
    struct virtio_gpu *g = &gpu;
    uint64 done;

    if (!g->initialized)
        return 0;

    spin_lock(&g->lock);
    done = g->stats.last_fence;
    spin_unlock(&g->lock);
    return done;
}

int virtio_gpu_user_get_caps_for(uint32 requested_capset_id,
                                 uint32 requested_capset_version,
                                 void *buf, uint32 buf_size,
                                 uint32 *capset_id, uint32 *capset_version,
                                 uint32 *capset_size)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_capset *capset;
    uint32 id;
    uint32 version;
    uint32 size;
    uint32 transfer_size;
    int ret = 0;

    if (!g->initialized)
        return -ENODEV;

    spin_lock(&g->lock);
    if (requested_capset_id == 0)
        requested_capset_id = g->virgl_capset_id;
    if (virtio_gpu_user_capset_policy_for(requested_capset_id) ==
        VIRTIO_GPU_USER_CAPSET_QUERY_ONLY) {
        spin_unlock(&g->lock);
        return virtio_gpu_user_fill_probe_capset(requested_capset_id,
                                                 requested_capset_version,
                                                 buf, buf_size, capset_id,
                                                 capset_version,
                                                 capset_size);
    }
    if (virtio_gpu_user_capset_policy_for(requested_capset_id) !=
        VIRTIO_GPU_USER_CAPSET_HOST_CREATABLE) {
        spin_unlock(&g->lock);
        return -EINVAL;
    }
    capset = virtio_gpu_lookup_capset_locked(g, requested_capset_id);
    if (capset == NULL) {
        spin_unlock(&g->lock);
        return -EINVAL;
    }
    id = capset->id;
    version = capset->version;
    size = capset->size;
    spin_unlock(&g->lock);

    if (requested_capset_version != 0) {
        if (requested_capset_version > version)
            return -EINVAL;
        version = requested_capset_version;
    }

    if (capset_id)
        *capset_id = id;
    if (capset_version)
        *capset_version = version;
    if (capset_size)
        *capset_size = size;
    if (buf == NULL || buf_size == 0)
        return 0;
    if (size == 0)
        return -EINVAL;
    transfer_size = size;
    if (transfer_size > buf_size)
        transfer_size = buf_size;
    if (transfer_size > PGSIZE - sizeof(struct virtio_gpu_resp_capset))
        transfer_size = PGSIZE - sizeof(struct virtio_gpu_resp_capset);

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_CAPSET);
    ret = virtio_gpu_submit_capset(g, id, version, transfer_size, buf);
    virtio_gpu_op_unlock(g);
    return ret == 0 ? 0 : -EIO;
}

int virtio_gpu_user_get_caps(void *buf, uint32 buf_size, uint32 *capset_id,
                             uint32 *capset_version, uint32 *capset_size)
{
    return virtio_gpu_user_get_caps_for(0, 0, buf, buf_size, capset_id,
                                        capset_version, capset_size);
}

int virtio_gpu_user_capset_ids(uint64 *ids)
{
    struct virtio_gpu *g = &gpu;
    uint64 value = 0;

    if (ids == NULL)
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    spin_lock(&g->lock);
    for (int i = 0; i < VIRTIO_GPU_MAX_CAPSETS; i++) {
        if (g->capsets[i].valid &&
            virtio_gpu_user_capset_policy_for(g->capsets[i].id) ==
            VIRTIO_GPU_USER_CAPSET_HOST_CREATABLE &&
            g->capsets[i].id < 64)
            value |= 1ULL << g->capsets[i].id;
    }
    spin_unlock(&g->lock);
    *ids = value;
    return 0;
}

int virtio_gpu_user_creatable_capset_ids(uint64 *ids)
{
    struct virtio_gpu *g = &gpu;
    uint64 value = 0;

    if (ids == NULL)
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    spin_lock(&g->lock);
    for (int i = 0; i < VIRTIO_GPU_MAX_CAPSETS; i++) {
        if (g->capsets[i].valid &&
            virtio_gpu_user_capset_policy_for(g->capsets[i].id) ==
            VIRTIO_GPU_USER_CAPSET_HOST_CREATABLE &&
            g->capsets[i].id < 64)
            value |= 1ULL << g->capsets[i].id;
    }
    spin_unlock(&g->lock);
    *ids = value;
    return 0;
}

int virtio_gpu_user_resource_create(uint64 owner_id, pid_t owner_tgid,
                                    struct fb_gpu_virgl_resource_create *req)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_context *ctx;
    struct virtio_gpu_resource *res = NULL;
    uint64 addr;
    int ret;
    int attached_to_ctx = 0;

    if (req == NULL)
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    if (req->ctx_id != 0) {
        spin_lock(&g->lock);
        ctx = virtio_gpu_lookup_context_locked(g, req->ctx_id);
        if (ctx == NULL) {
            spin_unlock(&g->lock);
            return -ENOENT;
        }
        if (!virtio_gpu_owner_matches(ctx->owner_id, ctx->owner_tgid,
                                      owner_id, owner_tgid)) {
            spin_unlock(&g->lock);
            return -EPERM;
        }
        if (ctx->failed) {
            spin_unlock(&g->lock);
            return -EIO;
        }
        spin_unlock(&g->lock);
    }

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_RESOURCE);
    ret = virtio_gpu_resource_create_3d(g, owner_id, owner_tgid, req, &res);
    if (ret != 0)
        goto out;
    if (virtio_gpu_resource_attach_pages(g, res) != 0) {
        ret = -EIO;
        goto out_unref;
    }
    if (req->ctx_id != 0) {
        if (virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
                                        req->ctx_id, res->id) != 0) {
            ret = -EIO;
            goto out_unref;
        }
        spin_lock(&g->lock);
        ret = virtio_gpu_resource_record_context_locked(res, req->ctx_id);
        spin_unlock(&g->lock);
        if (ret != 0)
            goto out_detach;
        attached_to_ctx = 1;
    }

    ret = virtio_gpu_map_pages_current(res->pages, res->npages,
                                       res->alloc_len, &addr);
    if (ret != 0)
        goto out_detach;

    req->resource_id = res->id;
    req->size = res->alloc_len;
    req->addr = addr;
    virtio_gpu_op_unlock(g);
    return 0;

out_detach:
    if (attached_to_ctx)
        virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE,
                                    req->ctx_id, res->id);
out_unref:
    if (res != NULL)
        virtio_gpu_resource_unref(g, res);
out:
    virtio_gpu_op_unlock(g);
    return ret;
}

int virtio_gpu_user_resource_create_blob(uint64 owner_id, pid_t owner_tgid,
                                         struct fb_gpu_virgl_blob_create *req)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_context *ctx;
    struct virtio_gpu_resource *res = NULL;
    uint64 addr;
    int ret;

    if (req == NULL)
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    if (req->ctx_id != 0) {
        spin_lock(&g->lock);
        ctx = virtio_gpu_lookup_context_locked(g, req->ctx_id);
        if (ctx == NULL) {
            spin_unlock(&g->lock);
            return -ENOENT;
        }
        if (!virtio_gpu_owner_matches(ctx->owner_id, ctx->owner_tgid,
                                      owner_id, owner_tgid)) {
            spin_unlock(&g->lock);
            return -EPERM;
        }
        if (ctx->failed) {
            spin_unlock(&g->lock);
            return -EIO;
        }
        spin_unlock(&g->lock);
    }

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_RESOURCE);
    ret = virtio_gpu_resource_create_blob(g, owner_id, owner_tgid, req, &res,
                                          0);
    if (ret != 0) {
        printf("virtio_gpu: user blob create failed ret=%d blob_mem=%u size=%lu\n",
               ret, req->blob_mem, req->size);
        goto out;
    }

    if (req->blob_mem == VIRTIO_GPU_BLOB_MEM_HOST3D) {
        addr = 0;
    } else {
        ret = virtio_gpu_map_pages_current(res->pages, res->npages,
                                           res->alloc_len, &addr);
        if (ret != 0) {
            printf("virtio_gpu: user blob map failed resource=%u ret=%d pages=%u size=%u\n",
                   res->id, ret, res->npages, res->alloc_len);
            goto out_unref;
        }
    }

    req->resource_id = res->id;
    req->size = res->alloc_len;
    req->addr = addr;
    virtio_gpu_op_unlock(g);
    return 0;

out_unref:
    if (res != NULL)
        virtio_gpu_resource_unref(g, res);
out:
    virtio_gpu_op_unlock(g);
    return ret;
}

static int virtio_gpu_destroy_resource_locked(struct virtio_gpu *g,
                                              uint32 resource_id,
                                              uint64 owner_id,
                                              pid_t owner_tgid)
{
    struct virtio_gpu_resource *res;
    uint32 ctx_ids[VIRTIO_GPU_RESOURCE_MAX_CONTEXT_ATTACHMENTS];
    uint32 ctx_count = 0;
    int ret;

    spin_lock(&g->lock);
    res = virtio_gpu_lookup_resource_locked(g, resource_id);
    ctx_count = virtio_gpu_resource_copy_contexts_locked(res, ctx_ids,
                                                        NELEM(ctx_ids));
    if (res != NULL &&
        !virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                  owner_id, owner_tgid))
        res = NULL;
    spin_unlock(&g->lock);
    if (res == NULL)
        return -ENOENT;

    spin_lock(&g->lock);
    if (res->export_refs != 0) {
        res->destroy_pending = 1;
        spin_unlock(&g->lock);
        return 0;
    }
    spin_unlock(&g->lock);

    for (uint32 i = 0; i < ctx_count; i++)
        virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE,
                                    ctx_ids[i], resource_id);
    virtio_gpu_page_flip_scanout_set_remove(g, resource_id);
    ret = virtio_gpu_resource_unref(g, res);
    return ret == 0 ? 0 : -EIO;
}

int virtio_gpu_user_resource_destroy(uint64 owner_id, pid_t owner_tgid,
                                     uint32 resource_id)
{
    struct virtio_gpu *g = &gpu;
    int ret;

    if (resource_id == 0)
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_RESOURCE);
    ret = virtio_gpu_destroy_resource_locked(g, resource_id, owner_id,
                                             owner_tgid);
    virtio_gpu_op_unlock(g);
    return ret;
}

int virtio_gpu_user_resource_attach(uint64 owner_id, pid_t owner_tgid,
                                    uint32 ctx_id, uint32 resource_id,
                                    int allow_imported)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_context *ctx;
    struct virtio_gpu_resource *res;
    int ret;

    if (ctx_id == 0 || resource_id == 0)
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_RESOURCE);
    spin_lock(&g->lock);
    ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
    if (ctx == NULL) {
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -ENOENT;
    }
    if (!virtio_gpu_owner_matches(ctx->owner_id, ctx->owner_tgid,
                                  owner_id, owner_tgid)) {
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -EPERM;
    }
    if (ctx->failed) {
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -EIO;
    }
    res = virtio_gpu_lookup_resource_locked(g, resource_id);
    if (res == NULL) {
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -ENOENT;
    }
    if (!virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                  owner_id, owner_tgid) &&
        !allow_imported) {
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -EPERM;
    }
    spin_unlock(&g->lock);

    ret = virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
                                      ctx_id, resource_id);
    if (ret == 0) {
        int recorded = 0;

        spin_lock(&g->lock);
        res = virtio_gpu_lookup_resource_locked(g, resource_id);
        if (res != NULL)
            recorded = virtio_gpu_resource_record_context_locked(res, ctx_id);
        spin_unlock(&g->lock);
        if (recorded != 0) {
            virtio_gpu_context_resource(g,
                                        VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE,
                                        ctx_id, resource_id);
            ret = recorded;
        }
    }
    virtio_gpu_op_unlock(g);
    return ret == 0 ? 0 : -EIO;
}

int virtio_gpu_user_resource_export_pages(uint64 owner_id, pid_t owner_tgid,
                                          uint32 resource_id, uint32 *width,
                                          uint32 *height, uint32 *pitch,
                                          uint64 *size, page_t ***pages_out,
                                          uint32 *npages_out)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;
    page_t **pages;
    uint32 npages;
    uint32 i;

    if (resource_id == 0 || width == NULL || height == NULL ||
        pitch == NULL || size == NULL || pages_out == NULL ||
        npages_out == NULL)
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    spin_lock(&g->lock);
    res = virtio_gpu_lookup_resource_locked(g, resource_id);
    if (res == NULL ||
        !virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                  owner_id, owner_tgid)) {
        spin_unlock(&g->lock);
        return -ENOENT;
    }
    if (res->pages == NULL || res->npages == 0 ||
        res->width == 0 || res->height == 0 ||
        res->width > 0xffffffffU / 4) {
        spin_unlock(&g->lock);
        return -EINVAL;
    }

    npages = res->npages;
    pages = kvmalloc((size_t)npages * sizeof(*pages));
    if (pages == NULL) {
        spin_unlock(&g->lock);
        return -ENOMEM;
    }
    memset(pages, 0, (size_t)npages * sizeof(*pages));

    for (i = 0; i < npages; i++) {
        uint64 pa;

        if (res->pages[i] == NULL)
            goto fail_locked;
        pa = __page_to_pa(res->pages[i]);
        if (page_ref_inc((void *)pa) <= 0)
            goto fail_locked;
        pages[i] = res->pages[i];
    }

    *width = res->width;
    *height = res->height;
    *pitch = res->width * 4;
    *size = res->alloc_len;
    *pages_out = pages;
    *npages_out = npages;
    res->export_refs++;
    spin_unlock(&g->lock);
    return 0;

fail_locked:
    while (i > 0) {
        i--;
        if (pages[i] != NULL)
            page_ref_dec((void *)__page_to_pa(pages[i]));
    }
    kvfree(pages);
    spin_unlock(&g->lock);
    return -ENOMEM;
}

void virtio_gpu_user_resource_export_put(uint32 resource_id)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;
    uint32 ctx_ids[VIRTIO_GPU_RESOURCE_MAX_CONTEXT_ATTACHMENTS];
    uint32 ctx_count = 0;
    int destroy_now = 0;

    if (resource_id == 0 || !g->initialized)
        return;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_RESOURCE);
    spin_lock(&g->lock);
    res = virtio_gpu_lookup_resource_locked(g, resource_id);
    if (res != NULL) {
        if (res->export_refs > 0)
            res->export_refs--;
        if (res->export_refs == 0 && res->destroy_pending) {
            ctx_count = virtio_gpu_resource_copy_contexts_locked(
                res, ctx_ids, NELEM(ctx_ids));
            destroy_now = 1;
        }
    }
    spin_unlock(&g->lock);

    if (destroy_now && res != NULL) {
        for (uint32 i = 0; i < ctx_count; i++)
            virtio_gpu_context_resource(g,
                                        VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE,
                                        ctx_ids[i], resource_id);
        (void)virtio_gpu_resource_unref(g, res);
    }
    virtio_gpu_op_unlock(g);
}

int virtio_gpu_user_resource_info(uint64 owner_id, pid_t owner_tgid,
                                  uint32 resource_id, uint32 *width,
                                  uint32 *height, uint32 *format,
                                  uint64 *size)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;

    if (resource_id == 0)
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    spin_lock(&g->lock);
    res = virtio_gpu_lookup_resource_locked(g, resource_id);
    if (res == NULL ||
        !virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                  owner_id, owner_tgid)) {
        spin_unlock(&g->lock);
        return -ENOENT;
    }
    if (width)
        *width = res->width;
    if (height)
        *height = res->height;
    if (format)
        *format = res->format;
    if (size)
        *size = res->alloc_len;
    spin_unlock(&g->lock);
    return 0;
}

int virtio_gpu_user_resource_blob_mem(uint64 owner_id, pid_t owner_tgid,
                                      uint32 resource_id, uint32 *blob_mem)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;

    if (blob_mem != NULL)
        *blob_mem = 0;
    if (resource_id == 0)
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    spin_lock(&g->lock);
    res = virtio_gpu_lookup_resource_locked(g, resource_id);
    if (res == NULL ||
        !virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                  owner_id, owner_tgid)) {
        spin_unlock(&g->lock);
        return -ENOENT;
    }
    if (blob_mem != NULL)
        *blob_mem = res->is_blob ? res->blob_mem : 0;
    spin_unlock(&g->lock);
    return 0;
}

int virtio_gpu_user_resource_map_offset(uint64 owner_id, pid_t owner_tgid,
                                        uint32 resource_id, uint64 *offset)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;
    uint64 map_offset;
    int ret;

    if (offset != NULL)
        *offset = 0;
    if (resource_id == 0 || offset == NULL)
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_RESOURCE);
    spin_lock(&g->lock);
    res = virtio_gpu_lookup_resource_locked(g, resource_id);
    if (res == NULL ||
        !virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                  owner_id, owner_tgid)) {
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -ENOENT;
    }
    if (!res->is_blob ||
        (res->blob_flags & VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE) == 0) {
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -EOPNOTSUPP;
    }
    if (res->host_visible_mapped) {
        *offset = res->host_visible_offset;
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return 0;
    }
    map_offset = virtio_gpu_align_up(g->host_visible_next_offset, PGSIZE);
    if (!virtio_gpu_host_visible_operational(g) ||
        map_offset + PGROUNDUP(res->alloc_len) < map_offset ||
        map_offset + PGROUNDUP(res->alloc_len) > g->host_visible_length) {
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -EOPNOTSUPP;
    }
    g->host_visible_next_offset = map_offset + PGROUNDUP(res->alloc_len);
    spin_unlock(&g->lock);

    ret = virtio_gpu_resource_map_blob(g, res, map_offset, NULL);
    if (ret == 0)
        *offset = map_offset;
    virtio_gpu_op_unlock(g);
    return ret;
}

int virtio_gpu_user_host_visible_mmap(uint64 owner_id, pid_t owner_tgid,
                                      uint64 offset, uint64 size)
{
    struct virtio_gpu *g = &gpu;
    uint64 end;
    int ok = 0;

    if (!g->initialized || size == 0)
        return -ENODEV;
    end = offset + size;
    if (end <= offset)
        return -EINVAL;

    spin_lock(&g->lock);
    if (virtio_gpu_host_visible_operational(g) &&
        end <= g->host_visible_length) {
        for (int i = 0; i < VIRTIO_GPU_MAX_RESOURCES; i++) {
            struct virtio_gpu_resource *res = &g->resources[i];
            uint64 res_end;

            if (!res->in_use || !res->host_visible_mapped ||
                !virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                          owner_id, owner_tgid))
                continue;
            res_end = res->host_visible_offset + res->host_visible_size;
            if (offset >= res->host_visible_offset && end <= res_end) {
                ok = 1;
                break;
            }
        }
    }
    spin_unlock(&g->lock);
    return ok ? 0 : -ENOENT;
}

void *virtio_gpu_user_host_visible_page(uint64 owner_id, pid_t owner_tgid,
                                        uint64 offset)
{
    struct virtio_gpu *g = &gpu;
    uint64 pa = 0;

    if (!g->initialized)
        return NULL;
    spin_lock(&g->lock);
    if (virtio_gpu_host_visible_operational(g) &&
        offset < g->host_visible_length) {
        for (int i = 0; i < VIRTIO_GPU_MAX_RESOURCES; i++) {
            struct virtio_gpu_resource *res = &g->resources[i];
            uint64 res_end;

            if (!res->in_use || !res->host_visible_mapped ||
                !virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                          owner_id, owner_tgid))
                continue;
            res_end = res->host_visible_offset + res->host_visible_size;
            if (offset >= res->host_visible_offset && offset < res_end) {
                pa = g->host_visible_pa + offset;
                break;
            }
        }
    }
    spin_unlock(&g->lock);
    return pa != 0 ? (void *)pa : NULL;
}

int virtio_gpu_user_resource_last_submit_fence(uint64 owner_id,
                                               pid_t owner_tgid,
                                               uint32 resource_id,
                                               uint64 *fence)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;

    if (fence)
        *fence = 0;
    if (resource_id == 0)
        return -EINVAL;
    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return -ENODEV;

    spin_lock(&g->lock);
    res = virtio_gpu_lookup_resource_locked(g, resource_id);
    if (res == NULL ||
        !virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                  owner_id, owner_tgid)) {
        spin_unlock(&g->lock);
        return -ENOENT;
    }
    if (fence)
        *fence = res->last_submit_fence;
    spin_unlock(&g->lock);
    return 0;
}

void *virtio_gpu_user_resource_page(uint64 owner_id, pid_t owner_tgid,
                                    uint32 resource_id, uint64 page_index)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;
    void *pa = NULL;

    if (resource_id == 0 || !g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return NULL;

    spin_lock(&g->lock);
    res = virtio_gpu_lookup_resource_locked(g, resource_id);
    if (res != NULL &&
        virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                 owner_id, owner_tgid) &&
        res->pages != NULL && page_index < res->npages &&
        res->pages[page_index] != NULL) {
        pa = (void *)__page_to_pa(res->pages[page_index]);
        if (page_ref_inc(pa) <= 0)
            pa = NULL;
    }
    spin_unlock(&g->lock);
    return pa;
}

void virtio_gpu_user_destroy_owner(pid_t owner_tgid)
{
    struct virtio_gpu *g = &gpu;
    uint32 *resource_ids;
    uint32 *context_ids;
    uint32 nresources = 0;
    uint32 ncontexts = 0;

    if (owner_tgid == 0)
        return;
    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return;

    resource_ids = kvmalloc((size_t)VIRTIO_GPU_MAX_RESOURCES *
                            sizeof(*resource_ids));
    context_ids = kvmalloc((size_t)VIRTIO_GPU_MAX_CONTEXTS *
                           sizeof(*context_ids));
    if (resource_ids == NULL || context_ids == NULL) {
        kvfree(resource_ids);
        kvfree(context_ids);
        return;
    }

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_RESOURCE);
    spin_lock(&g->lock);
    for (int i = 0; i < VIRTIO_GPU_MAX_RESOURCES; i++) {
        if (g->resources[i].in_use &&
            g->resources[i].owner_tgid == owner_tgid &&
            nresources < VIRTIO_GPU_MAX_RESOURCES)
            resource_ids[nresources++] = g->resources[i].id;
    }
    for (int i = 0; i < VIRTIO_GPU_MAX_CONTEXTS; i++) {
        if (g->contexts[i].in_use &&
            g->contexts[i].owner_tgid == owner_tgid &&
            ncontexts < VIRTIO_GPU_MAX_CONTEXTS)
            context_ids[ncontexts++] = g->contexts[i].id;
    }
    spin_unlock(&g->lock);

    for (uint32 i = 0; i < nresources; i++)
        (void)virtio_gpu_destroy_resource_locked(g, resource_ids[i], 0,
                                                 owner_tgid);

    for (uint32 i = 0; i < ncontexts; i++) {
        struct virtio_gpu_context *ctx;

        if (virtio_gpu_destroy_context(g, context_ids[i]) != 0)
            continue;
        spin_lock(&g->lock);
        if (g->present_scanout_ctx_id == context_ids[i]) {
            g->present_scanout_ctx_id = 0;
            g->present_scanout_resource_id = 0;
        }
        virtio_gpu_forget_context_resources_locked(g, context_ids[i]);
        ctx = virtio_gpu_lookup_context_locked(g, context_ids[i]);
        if (ctx != NULL && ctx->owner_tgid == owner_tgid) {
            if (ctx->failed && g->stats.context_failed > 0)
                g->stats.context_failed--;
            memset(ctx, 0, sizeof(*ctx));
        }
        spin_unlock(&g->lock);
    }
    virtio_gpu_op_unlock(g);
    kvfree(resource_ids);
    kvfree(context_ids);
}

void virtio_gpu_user_destroy_render_owner(uint64 owner_id)
{
    struct virtio_gpu *g = &gpu;
    uint32 *resource_ids;
    uint32 *context_ids;
    uint32 nresources = 0;
    uint32 ncontexts = 0;

    if (owner_id == 0)
        return;
    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return;

    resource_ids = kvmalloc((size_t)VIRTIO_GPU_MAX_RESOURCES *
                            sizeof(*resource_ids));
    context_ids = kvmalloc((size_t)VIRTIO_GPU_MAX_CONTEXTS *
                           sizeof(*context_ids));
    if (resource_ids == NULL || context_ids == NULL) {
        kvfree(resource_ids);
        kvfree(context_ids);
        return;
    }

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_RESOURCE);
    spin_lock(&g->lock);
    for (int i = 0; i < VIRTIO_GPU_MAX_RESOURCES; i++) {
        if (g->resources[i].in_use &&
            g->resources[i].owner_id == owner_id &&
            nresources < VIRTIO_GPU_MAX_RESOURCES)
            resource_ids[nresources++] = g->resources[i].id;
    }
    for (int i = 0; i < VIRTIO_GPU_MAX_CONTEXTS; i++) {
        if (g->contexts[i].in_use &&
            g->contexts[i].owner_id == owner_id &&
            ncontexts < VIRTIO_GPU_MAX_CONTEXTS)
            context_ids[ncontexts++] = g->contexts[i].id;
    }
    spin_unlock(&g->lock);

    for (uint32 i = 0; i < nresources; i++)
        (void)virtio_gpu_destroy_resource_locked(g, resource_ids[i],
                                                 owner_id, 0);

    for (uint32 i = 0; i < ncontexts; i++) {
        struct virtio_gpu_context *ctx;

        if (virtio_gpu_destroy_context(g, context_ids[i]) != 0)
            continue;
        spin_lock(&g->lock);
        if (g->present_scanout_ctx_id == context_ids[i]) {
            g->present_scanout_ctx_id = 0;
            g->present_scanout_resource_id = 0;
        }
        virtio_gpu_forget_context_resources_locked(g, context_ids[i]);
        ctx = virtio_gpu_lookup_context_locked(g, context_ids[i]);
        if (ctx != NULL && ctx->owner_id == owner_id) {
            if (ctx->failed && g->stats.context_failed > 0)
                g->stats.context_failed--;
            memset(ctx, 0, sizeof(*ctx));
        }
        spin_unlock(&g->lock);
    }
    virtio_gpu_op_unlock(g);
    kvfree(resource_ids);
    kvfree(context_ids);
}

int virtio_gpu_user_transfer(uint64 owner_id, pid_t owner_tgid,
                             struct fb_gpu_virgl_transfer *req,
                             int from_host)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;
    struct virtio_gpu_transfer_host_3d *cmd =
        (struct virtio_gpu_transfer_host_3d *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    uint32 cmd_type = from_host ? VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D :
                                  VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    uint32 ctx_id = 0;
    int ret = -EIO;
    int transfer_perf;
    int async_path = 0;
    int mixed_path = 0;
    int linear_transfer_path = 0;
    int bound_scanout_path = 0;
    int wait_holder = VIRTIO_GPU_OP_NONE;
    uint64 total_start = 0;
    uint64 lock_acquired = 0;
    uint64 submit_start;
    uint64 submit_ticks = 0;
    uint64 drain_ticks = 0;
    uint64 command_ticks = 0;
    struct virtio_gpu_async_drain_sample drain_sample;
    uint32 log_x = 0;
    uint32 log_y = 0;
    uint32 log_w = 0;
    uint32 log_h = 0;
    uint32 log_resource_id = 0;
    uint32 log_ctx_id = 0;
    uint32 log_res_width = 0;
    uint32 log_res_height = 0;
    uint32 log_target = 0;
    uint32 log_bind = 0;
    uint64 log_offset = 0;
    uint32 log_stride = 0;

    if (req == NULL || req->resource_id == 0 || req->flags != 0 ||
        req->w == 0 || req->h == 0 || req->d == 0)
        return -EINVAL;
    memset(&drain_sample, 0, sizeof(drain_sample));
    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return -ENODEV;
    log_x = req->x;
    log_y = req->y;
    log_w = req->w;
    log_h = req->h;
    log_resource_id = req->resource_id;
    log_offset = req->offset;
    log_stride = req->stride;

    transfer_perf = virtio_gpu_cmdline_enabled("virtio_gpu_transfer_perf");
    if (transfer_perf)
        total_start = r_time();
    if (transfer_perf && mutex_trylock(&g->op_lock)) {
        wait_holder = VIRTIO_GPU_OP_NONE;
    } else {
        if (transfer_perf)
            wait_holder = __atomic_load_n(&g->op_lock_holder,
                                          __ATOMIC_RELAXED);
        mutex_lock(&g->op_lock);
    }
    __atomic_store_n(&g->op_lock_holder, VIRTIO_GPU_OP_TRANSFER,
                     __ATOMIC_RELAXED);
    if (transfer_perf)
        lock_acquired = r_time();
    spin_lock(&g->lock);
    res = virtio_gpu_lookup_resource_locked(g, req->resource_id);
    if (res != NULL) {
        struct virtio_gpu_context *ctx = NULL;
        uint32 primary_ctx_id =
            virtio_gpu_resource_primary_context_locked(res);
        if (primary_ctx_id != 0)
            ctx = virtio_gpu_lookup_context_locked(g, primary_ctx_id);
        if (!virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                      owner_id, owner_tgid) ||
            (ctx != NULL && ctx->failed) ||
            req->x > res->width || req->w > res->width - req->x ||
            req->y > res->height || req->h > res->height - req->y ||
            req->z > res->depth || req->d > res->depth - req->z ||
            req->level > res->last_level)
            res = NULL;
        else
            ctx_id = primary_ctx_id;
    }
    spin_unlock(&g->lock);
    if (res == NULL) {
        ret = -EINVAL;
        goto out;
    }
    bound_scanout_path = req->resource_id == g->bound_scanout_resource_id;
    log_ctx_id = ctx_id;
    log_res_width = res->width;
    log_res_height = res->height;
    log_target = res->target;
    log_bind = res->bind;
    linear_transfer_path = !from_host && res->height == 1 && req->h == 1 &&
        res->target == 0 && (res->bind & 0x20) != 0;

    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = cmd_type;
    cmd->hdr.ctx_id = ctx_id;
    cmd->box.x = req->x;
    cmd->box.y = req->y;
    cmd->box.z = req->z;
    cmd->box.w = req->w;
    cmd->box.h = req->h;
    cmd->box.d = req->d;
    cmd->offset = req->offset;
    cmd->resource_id = req->resource_id;
    cmd->level = req->level;
    cmd->stride = req->stride;
    cmd->layer_stride = req->layer_stride;

    /*
     * The compositor's per-frame chrome upload to the bound scanout resource is
     * small and on the present hot path.  The synchronous submit below first
     * drains the whole async ring, so the upload ends up blocking on the prior
     * frame's RESOURCE_FLUSH completing (a ~vsync wait on the single in-order
     * control queue).  Posting it asynchronously instead lets the guest keep
     * encoding the gl-compose stream while the host drains the queue.  Only
     * TO_HOST transfers of the bound scanout resource are eligible; FROM_HOST
     * readbacks and other resources keep the synchronous semantics callers
     * rely on.  Opt-in via the "virtio_gpu_async_fb_transfer" cmdline flag.
     *
     * Mesa's linear transfer resources are a separate diagnostic path: they use
     * a 1D resource layout and must preserve the caller's offset/stride exactly.
     * Keep that behind "virtio_gpu_async_linear_transfer" until validation proves
     * it is safe and useful.
     */
    if (!from_host &&
        ((req->resource_id == g->bound_scanout_resource_id &&
          virtio_gpu_cmdline_enabled("virtio_gpu_async_fb_transfer")) ||
         (res->height == 1 && req->h == 1 && res->target == 0 &&
          (res->bind & 0x20) != 0 &&
          virtio_gpu_cmdline_enabled("virtio_gpu_async_linear_transfer")))) {
        async_path = 1;
        submit_start = transfer_perf ? r_time() : 0;
        ret = virtio_gpu_transfer_to_host_3d_async(g, res, ctx_id, req->x,
                                                   req->y, req->w, req->h,
                                                   req->z, req->d,
                                                   req->level, req->offset,
                                                   cmd->stride,
                                                   cmd->layer_stride);
        if (transfer_perf)
            submit_ticks = r_time() - submit_start;
        ret = ret == 0 ? 0 : -EIO;
        goto out;
    }

    submit_start = transfer_perf ? r_time() : 0;
    if (linear_transfer_path &&
        virtio_gpu_cmdline_enabled("virtio_gpu_linear_transfer_mixed_wait")) {
        mixed_path = 1;
        ret = virtio_gpu_submit_mixed_async(
            g, cmd, sizeof(*cmd), NULL, 0, false, resp, sizeof(*resp),
            VIRTIO_GPU_RESP_OK_NODATA,
            transfer_perf ? &drain_sample : NULL);
        if (transfer_perf)
            command_ticks = r_time() - submit_start;
    } else {
        ret = virtio_gpu_submit_internal(g, cmd, sizeof(*cmd), NULL, 0, false,
                                         resp, sizeof(*resp),
                                         VIRTIO_GPU_RESP_OK_NODATA,
                                         0,
                                         transfer_perf ? &drain_ticks : NULL,
                                         transfer_perf ? &command_ticks : NULL,
                                         transfer_perf ? &drain_sample : NULL);
    }
    if (transfer_perf)
        submit_ticks = r_time() - submit_start;
    ret = ret == 0 ? 0 : -EIO;
out:
    if (transfer_perf)
        virtio_gpu_transfer_perf_log(
            ret, from_host, async_path, mixed_path, bound_scanout_path,
            wait_holder,
            r_time() - total_start, lock_acquired - total_start, submit_ticks,
            drain_ticks, command_ticks, drain_sample.submit_3d,
            drain_sample.flush, drain_sample.transfer, drain_sample.other,
            log_x, log_y, log_w, log_h, log_resource_id, log_ctx_id,
            log_res_width, log_res_height, log_target, log_bind, log_offset,
            log_stride, owner_tgid);
    virtio_gpu_op_unlock(g);
    return ret;
}
