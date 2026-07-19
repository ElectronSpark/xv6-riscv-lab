/*
 * N1/M7 (2026-07-05): initial DEFAULT-OFF rework.  The first default-on
 * battery hit `PANIC thread_queue.c:213 tq_remove: queue is empty` in
 * virtio_gpu paths (2/2 GUI runs): releasing op_lock across the
 * make-room stall allows MULTIPLE concurrent waiters on the used-ring
 * wait queue, which the virtio_gpu_wait_for_used sleep/wake path
 * implicitly assumed had at most one (it only ever ran under op_lock).
 * The same-binary opt-out control (unlocked_wait=0, depth 60 + poll
 * defaults on) ran a clean full video window — bisect is conclusive.
 * Re-enable only after making the used-ring wait multi-waiter-safe.
 *
 * 2026-07-05 redesign (B2/B3/progress, still default-OFF pending A/B):
 * the reaper is now TOTAL (sync completions route through the queue's
 * sync record instead of being stolen/discarded), aborts quarantine
 * POSTED slots as ABANDONED instead of freeing device-owned memory
 * (claim/fill/post/reap state machine under q->lock), and progress
 * detection snapshot-compares used->idx / retire_seq MOVEMENT instead
 * of occupancy.  These changes run unconditionally and are safe in
 * default mode; this flag still only controls whether ctx_submit
 * releases op_lock across make-room stalls.
 *
 * 2026-07-19 validation: the multi-waiter-safe queue, reason-specific submit
 * accounting, posted-age watchdog, and abort generation pass the SDL hover
 * workload.  The reproducible SDL/virgl launcher now enables
 * virtio_gpu_submit_unlocked_wait=1; an explicit zero remains the same-binary
 * diagnostic control.
 */
static int virtio_gpu_submit_unlocked_wait_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("virtio_gpu_submit_unlocked_wait",
                                    value, sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

/*
 * Diagnostic only.  Virgl's submit resource list is a reachability list, not
 * a read/write access mask.  Treating every listed resource as a write hazard
 * serialized ordinary Plasma/Chromium frames behind nearly every scanout
 * flush and reduced 720p presentation from the 50--60 fps range to 37.8 fps.
 * Keep the strict policy available for an A/B together with
 * virtio_gpu_fenced_scanout_flush=1, but leave normal presentation pipelined
 * like Linux: SET_SCANOUT + RESOURCE_FLUSH stay ordered on the control queue
 * and a synchronous RESOURCE_UNREF drains outstanding async work before
 * freeing a resource.
 */
static int virtio_gpu_present_reuse_wait_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("virtio_gpu_present_reuse_wait",
                                    value, sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

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

static int virtio_gpu_user_capset_creatable(uint32 capset_id)
{
    return virtio_gpu_user_capset_policy_for(capset_id) ==
        VIRTIO_GPU_USER_CAPSET_HOST_CREATABLE;
}

static uint32 virtio_gpu_user_capset_payload_limit(void)
{
    return PGSIZE - sizeof(struct virtio_gpu_resp_capset);
}

int virtio_gpu_user_capset_query_only(uint32 capset_id)
{
    return virtio_gpu_user_capset_policy_for(capset_id) ==
        VIRTIO_GPU_USER_CAPSET_QUERY_ONLY;
}

static int virtio_gpu_resource_accessible_by_owner_locked(
    struct virtio_gpu *g, const struct virtio_gpu_resource *res,
    uint64 owner_id, pid_t owner_tgid)
{
    if (g == NULL || res == NULL || !res->in_use)
        return 0;
    if (virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                 owner_id, owner_tgid))
        return 1;

    for (uint32 i = 0; i < res->ctx_attach_count; i++) {
        struct virtio_gpu_context *ctx =
            virtio_gpu_lookup_context_locked(g, res->ctx_attached[i]);

        if (ctx != NULL && ctx->in_use && !ctx->failed &&
            virtio_gpu_owner_matches(ctx->owner_id, ctx->owner_tgid,
                                     owner_id, owner_tgid))
            return 1;
    }
    return 0;
}

static int virtio_gpu_user_blob_class_admissible(struct virtio_gpu *g,
                                                 uint32 blob_mem,
                                                 uint32 blob_flags)
{
    uint32 allowed_flags = VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE |
        VIRTIO_GPU_BLOB_FLAG_USE_SHAREABLE;

    if (g == NULL || !g->initialized)
        return -ENODEV;
    if ((g->driver_features0 & (1u << VIRTIO_GPU_F_RESOURCE_BLOB)) == 0)
        return -EOPNOTSUPP;
    if (blob_mem != VIRTIO_GPU_BLOB_MEM_GUEST &&
        blob_mem != VIRTIO_GPU_BLOB_MEM_HOST3D &&
        blob_mem != VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST)
        return -EOPNOTSUPP;
    if ((blob_flags & ~allowed_flags) != 0)
        return -EOPNOTSUPP;
    if (blob_flags != 0 && !virtio_gpu_host_visible_operational(g))
        return -EOPNOTSUPP;
    if (blob_mem == VIRTIO_GPU_BLOB_MEM_HOST3D &&
        (blob_flags & VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE) == 0)
        return -EINVAL;
    return 0;
}

static int virtio_gpu_user_resource_blob_contract_supported(struct virtio_gpu *g)
{
    return virtio_gpu_user_blob_class_admissible(g,
                                                VIRTIO_GPU_BLOB_MEM_GUEST,
                                                0) == 0;
}

int virtio_gpu_user_resource_blob_supported(void)
{
    return virtio_gpu_user_resource_blob_contract_supported(&gpu);
}

int virtio_gpu_user_context_create(uint64 owner_id, pid_t owner_tgid,
                                   uint32 capset_id, uint32 context_init,
                                   const char *name, uint32 *ctx_id,
                                   uint32 *actual_capset_id)
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
    capset = virtio_gpu_lookup_capset_locked(g, capset_id);
    if (capset == NULL || !capset->creatable ||
        !virtio_gpu_user_capset_creatable(capset_id)) {
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
    ctx->context_init = context_init;
    ctx->owner_id = owner_id;
    ctx->owner_tgid = owner_tgid;
    if (name != NULL) {
        memmove(ctx->debug_name, name,
                strlen(name) < sizeof(ctx->debug_name) ?
                strlen(name) + 1 : sizeof(ctx->debug_name));
    } else {
        memcpy(ctx->debug_name, "xv6-virgl", sizeof("xv6-virgl"));
    }
    ctx->debug_name[sizeof(ctx->debug_name) - 1] = 0;
    spin_unlock(&g->lock);

    ret = virtio_gpu_create_context(g, id, capset_id, context_init,
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
    if (actual_capset_id != NULL)
        *actual_capset_id = capset_id;
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
                           uint64 *signaled, uint32 *first_submit,
                           const struct virtio_gpu_submit_trace_shape *shape)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_context *ctx;
    struct virtio_gpu_async_submit prep;
    uint64 fence_id;
    uint64 present_wait_fence;
    int trace = virtio_gpu_submit_trace_collect_enabled();
    uint64 trace_start = trace ? r_time() : 0;
    uint64 trace_lock_start = 0;
    uint64 trace_lock_wait_ticks = 0;
    uint64 trace_attach_count = 0;
    uint64 trace_attach_ticks = 0;
    uint64 trace_prepare_start = 0;
    uint64 trace_prepare_ticks = 0;
    uint64 trace_post_start = 0;
    uint64 trace_post_ticks = 0;
    int ret;
    int async_submit;
    int first_submit_reserved = 0;

    if (first_submit)
        *first_submit = 0;
    if ((flags & ~(FB_GPU_VIRGL_SUBMIT_ASYNC |
                   FB_GPU_VIRGL_SUBMIT_ALLOW_IMPORTED_RESOURCES |
                   FB_GPU_VIRGL_SUBMIT_FORCE_FAIL)) != 0 ||
        ctx_id == 0 || cmds == NULL || nr_dwords == 0 ||
        nr_dwords > (PGSIZE * 64) / sizeof(uint32)) {
        if (trace && g->initialized)
            virtio_gpu_submit_trace_record_submit(
                g, owner_id, owner_tgid, ctx_id, r_time() - trace_start,
                0, 0, 0, 0, 0, 0, 1, -EINVAL, shape);
        return -EINVAL;
    }
    if (!g->initialized)
        return -ENODEV;

    spin_lock(&g->lock);
    ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
    if (ctx == NULL) {
        spin_unlock(&g->lock);
        if (trace)
            virtio_gpu_submit_trace_record_submit(
                g, owner_id, owner_tgid, ctx_id, r_time() - trace_start,
                0, 0, 0, 0, 0, 0, 1, -ENOENT, shape);
        return -ENOENT;
    }
    if (!virtio_gpu_owner_matches(ctx->owner_id, ctx->owner_tgid,
                                  owner_id, owner_tgid)) {
        spin_unlock(&g->lock);
        if (trace)
            virtio_gpu_submit_trace_record_submit(
                g, owner_id, owner_tgid, ctx_id, r_time() - trace_start,
                0, 0, 0, 0, 0, 0, 1, -EPERM, shape);
        return -EPERM;
    }
    if (ctx->failed) {
        spin_unlock(&g->lock);
        if (trace)
            virtio_gpu_submit_trace_record_submit(
                g, owner_id, owner_tgid, ctx_id, r_time() - trace_start,
                0, 0, 0, 0, 0, 0, 1, -EIO, shape);
        return -EIO;
    }
    if (flags & FB_GPU_VIRGL_SUBMIT_FORCE_FAIL) {
        virtio_gpu_mark_context_failed_locked(g, ctx);
        spin_unlock(&g->lock);
        if (trace)
            virtio_gpu_submit_trace_record_submit(
                g, owner_id, owner_tgid, ctx_id, r_time() - trace_start,
                0, 0, 0, 0, 0, 0, 1, -EIO, shape);
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
        if (trace)
            trace_prepare_start = r_time();
        ret = virtio_gpu_submit_3d_async_prepare(
            g, ctx_id, cmds, nr_dwords, fence_id, &prep);
        if (trace)
            trace_prepare_ticks = r_time() - trace_prepare_start;
        if (ret != 0) {
            if (trace)
                virtio_gpu_submit_trace_record_submit(
                    g, owner_id, owner_tgid, ctx_id,
                    r_time() - trace_start, 0, 0, 0,
                    trace_prepare_ticks, 0, 0, 1, ret, shape);
            return ret;
        }
    }

    if (trace)
        trace_lock_start = r_time();
    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_SUBMIT_3D);
    if (trace)
        trace_lock_wait_ticks = r_time() - trace_lock_start;
    if (trace)
        virtio_gpu_submit_trace_set_current(g, owner_id, owner_tgid, ctx_id,
                                            shape);
    /* Strict hazard experiment only; see the policy comment above. */
    while (virtio_gpu_present_reuse_wait_enabled()) {
        uint64 completed_fence;

        present_wait_fence = 0;
        spin_lock(&g->lock);
        completed_fence = g->stats.last_fence;
        if (resources != NULL) {
            for (uint32 i = 0; i < resource_count; i++) {
                struct virtio_gpu_resource *res;

                if (resources[i] == 0)
                    continue;
                res = virtio_gpu_lookup_resource_locked(g, resources[i]);
                if (res != NULL &&
                    (allow_imported_resources ||
                     virtio_gpu_owner_matches(res->owner_id,
                                              res->owner_tgid,
                                              owner_id, owner_tgid) ||
                     virtio_gpu_resource_context_attached_locked(res,
                                                                 ctx_id)) &&
                    res->last_present_fence > completed_fence &&
                    res->last_present_fence > present_wait_fence)
                    present_wait_fence = res->last_present_fence;
            }
        }
        spin_unlock(&g->lock);
        if (present_wait_fence == 0)
            break;

        if (trace)
            virtio_gpu_submit_trace_clear_current(g);
        virtio_gpu_op_unlock(g);
        ret = virtio_gpu_drain_async_until_fence(g, present_wait_fence);
        virtio_gpu_op_lock(g, VIRTIO_GPU_OP_SUBMIT_3D);
        if (trace)
            virtio_gpu_submit_trace_set_current(g, owner_id, owner_tgid,
                                                ctx_id, shape);
        if (ret != 0)
            goto out_unlock_submit;
    }
    if (resources != NULL) {
        for (uint32 i = 0; i < resource_count; i++) {
            struct virtio_gpu_resource *res;
            int already_attached = 0;
            int recorded = 0;
            uint64 attach_start = 0;

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

            if (trace) {
                trace_attach_count++;
                attach_start = r_time();
            }
            if (async_submit)
                ret = virtio_gpu_context_resource_mixed_async(
                    g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, ctx_id,
                    resources[i]);
            else
                ret = virtio_gpu_context_resource(
                    g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, ctx_id,
                    resources[i]);
            if (trace)
                trace_attach_ticks += r_time() - attach_start;
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

    if (trace)
        trace_post_start = r_time();
    if (async_submit && virtio_gpu_submit_unlocked_wait_enabled()) {
        /*
         * N1/M7 fix: never hold the op lock across a make-room stall.
         * A full async ring means we are waiting on HOST retirement of
         * earlier submits; holding op_lock through that wait serialized
         * KWin's page-flip present behind it (measured 8.6ms average
         * present vs 1.45ms unblocked) and paced the desktop to ~45Hz.
         * Post without waiting; on -EAGAIN drop the lock, wait for room,
         * re-take it and retry.  Resource attaches done above are
         * idempotent context state, and cross-context submit order was
         * never defined by op_lock arrival order anyway.
         */
        for (;;) {
            ret = virtio_gpu_submit_3d_async_post_prepared(g, &prep, 1);
            if (ret != -EAGAIN)
                break;
            if (trace)
                virtio_gpu_submit_trace_clear_current(g);
            virtio_gpu_op_unlock(g);
            int room = virtio_gpu_async_make_room(
                g, VIRTIO_GPU_ASYNC_REASON_SUBMIT_3D);
            virtio_gpu_op_lock(g, VIRTIO_GPU_OP_SUBMIT_3D);
            if (trace)
                virtio_gpu_submit_trace_set_current(g, owner_id,
                                                    owner_tgid, ctx_id,
                                                    shape);
            if (room != 0) {
                ret = -EIO;
                break;
            }
        }
    } else if (async_submit)
        ret = virtio_gpu_submit_3d_async_post_prepared(g, &prep, 0);
    else
        ret = virtio_gpu_submit_3d(g, ctx_id, cmds, nr_dwords, fence_id);
    if (trace)
        trace_post_ticks = r_time() - trace_post_start;
out_unlock_submit:
    if (ret == 0) {
        spin_lock(&g->lock);
        ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
        if (ctx != NULL && !ctx->first_submit_seen) {
            ctx->first_submit_seen = 1;
            first_submit_reserved = 1;
        }
        spin_unlock(&g->lock);
    }
    if (trace)
        virtio_gpu_submit_trace_clear_current(g);
    virtio_gpu_op_unlock(g);
    if (ret != 0)
        virtio_gpu_async_submit_free(&prep);
    if (ret != 0) {
        spin_lock(&g->lock);
        ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
        virtio_gpu_mark_context_failed_locked(g, ctx);
        spin_unlock(&g->lock);
        if (trace)
            virtio_gpu_submit_trace_record_submit(
                g, owner_id, owner_tgid, ctx_id,
                r_time() - trace_start, trace_lock_wait_ticks,
                trace_attach_count, trace_attach_ticks, trace_prepare_ticks,
                trace_post_ticks, 0, 1, -EIO, shape);
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
    if (first_submit)
        *first_submit = first_submit_reserved;
    if (trace)
        virtio_gpu_submit_trace_record_submit(
            g, owner_id, owner_tgid, ctx_id,
            r_time() - trace_start, trace_lock_wait_ticks,
            trace_attach_count, trace_attach_ticks, trace_prepare_ticks,
            trace_post_ticks, first_submit_reserved, 0, 0, shape);
    return 0;
}

int virtio_gpu_user_fence(uint64 wait_for, int wait, uint64 *signaled)
{
    struct virtio_gpu *g = &gpu;
    uint64 done;
    int trace = virtio_gpu_submit_trace_enabled();
    uint64 trace_start = trace ? r_time() : 0;
    uint64 trace_drain_start = 0;
    uint64 trace_drain_ticks = 0;
    uint64 trace_drain_calls = 0;
    int ret = 0;

    if (!g->initialized)
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_FENCE);
    spin_lock(&g->lock);
    done = g->stats.last_fence;
    spin_unlock(&g->lock);
    if (virtio_gpu_async_pending(g) && wait && wait_for != 0) {
        if (trace) {
            trace_drain_calls++;
            trace_drain_start = r_time();
        }
        ret = virtio_gpu_drain_async_until_fence(g, wait_for);
        if (trace)
            trace_drain_ticks += r_time() - trace_drain_start;
        if (ret != 0) {
            virtio_gpu_op_unlock(g);
            if (trace)
                virtio_gpu_submit_trace_record_fence(
                    g, r_time() - trace_start, trace_drain_calls,
                    trace_drain_ticks, 1);
            return -EIO;
        }
    } else if (virtio_gpu_async_pending(g) &&
               (wait || wait_for == 0 ||
                wait_for <= virtio_gpu_async_newest_fence(g))) {
        if (trace) {
            trace_drain_calls++;
            trace_drain_start = r_time();
        }
        ret = virtio_gpu_drain_async_submit(g, wait);
        if (trace)
            trace_drain_ticks += r_time() - trace_drain_start;
        if (ret != 0) {
            virtio_gpu_op_unlock(g);
            if (trace)
                virtio_gpu_submit_trace_record_fence(
                    g, r_time() - trace_start, trace_drain_calls,
                    trace_drain_ticks, 1);
            return -EIO;
        }
    }
    spin_lock(&g->lock);
    done = g->stats.last_fence;
    spin_unlock(&g->lock);
    virtio_gpu_op_unlock(g);

    if (signaled)
        *signaled = done;
    if (wait && wait_for != 0 && wait_for > done) {
        if (trace)
            virtio_gpu_submit_trace_record_fence(
                g, r_time() - trace_start, trace_drain_calls,
                trace_drain_ticks, 1);
        return -EAGAIN;
    }
    if (trace)
        virtio_gpu_submit_trace_record_fence(
            g, r_time() - trace_start, trace_drain_calls,
            trace_drain_ticks, 0);
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
        return 0;
    transfer_size = size;
    if (transfer_size > buf_size)
        transfer_size = buf_size;
    if (transfer_size == 0)
        return -EINVAL;
    if (transfer_size > virtio_gpu_user_capset_payload_limit())
        return -EOVERFLOW;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_CAPSET);
    ret = virtio_gpu_submit_capset(g, id, version, transfer_size, buf);
    virtio_gpu_op_unlock(g);
    if (ret != 0)
        return -EIO;

    return 0;
}

int virtio_gpu_user_get_caps(void *buf, uint32 buf_size, uint32 *capset_id,
                             uint32 *capset_version, uint32 *capset_size)
{
    return virtio_gpu_user_get_caps_for(0, 0, buf, buf_size, capset_id,
                                        capset_version, capset_size);
}

static int virtio_gpu_user_collect_capset_ids(uint64 *ids, int creatable_only)
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
            (!creatable_only || g->capsets[i].creatable) &&
            g->capsets[i].id < 64)
            value |= 1ULL << g->capsets[i].id;
    }
    spin_unlock(&g->lock);
    *ids = value;
    return 0;
}

int virtio_gpu_user_capset_ids(uint64 *ids)
{
    return virtio_gpu_user_collect_capset_ids(ids, 0);
}

int virtio_gpu_user_creatable_capset_ids(uint64 *ids)
{
    return virtio_gpu_user_collect_capset_ids(ids, 1);
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
    ret = virtio_gpu_user_blob_class_admissible(g, req->blob_mem,
                                                req->blob_flags);
    if (ret != 0)
        return ret;

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
    int bound_scanout = 0;
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

    spin_lock(&g->lock);
    bound_scanout = g->bound_scanout_resource_id == resource_id ||
        g->scanout_resource == res ||
        g->present_scanout_resource_id == resource_id;
    spin_unlock(&g->lock);
    if (bound_scanout &&
        virtio_gpu_set_scanout(g, 0, NULL, 0, 0, 0, 0) != 0)
        return -EIO;

    spin_lock(&g->lock);
    if (g->bound_scanout_resource_id == resource_id)
        g->bound_scanout_resource_id = 0;
    if (g->scanout_resource == res)
        g->scanout_resource = NULL;
    if (g->present_scanout_resource_id == resource_id) {
        g->present_scanout_ctx_id = 0;
        g->present_scanout_resource_id = 0;
    }
    spin_unlock(&g->lock);
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
    if (virtio_gpu_resource_context_attached_locked(res, ctx_id)) {
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return 0;
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

int virtio_gpu_user_resource_export_pin(uint64 owner_id, pid_t owner_tgid,
                                        uint32 resource_id, uint32 *width,
                                        uint32 *height, uint32 *format,
                                        uint64 *size, uint32 *blob_mem)
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
    if (res->width == 0 || res->height == 0 || res->alloc_len == 0) {
        spin_unlock(&g->lock);
        return -EINVAL;
    }
    if (width != NULL)
        *width = res->width;
    if (height != NULL)
        *height = res->height;
    if (format != NULL)
        *format = res->format;
    if (size != NULL)
        *size = res->alloc_len;
    if (blob_mem != NULL)
        *blob_mem = res->is_blob ? res->blob_mem : 0;
    res->export_refs++;
    spin_unlock(&g->lock);
    return 0;
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
                !virtio_gpu_resource_accessible_by_owner_locked(
                    g, res, owner_id, owner_tgid))
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
                !virtio_gpu_resource_accessible_by_owner_locked(
                    g, res, owner_id, owner_tgid))
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
    virtio_gpu_submit_trace_emit(g);
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
    virtio_gpu_submit_trace_emit(g);
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
    int submit_trace = virtio_gpu_submit_trace_collect_enabled();
    int submit_trace_current_set = 0;
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
    if (submit_trace) {
        virtio_gpu_submit_trace_set_current(g, owner_id, owner_tgid,
                                            ctx_id, NULL);
        submit_trace_current_set = 1;
    }
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
    if (submit_trace_current_set)
        virtio_gpu_submit_trace_clear_current(g);
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
