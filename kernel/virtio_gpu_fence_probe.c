/*
 * Default-off, one-shot ordering regression for two named contexts on one
 * open GPU file. No device fault is injected. A waits before op_lock and
 * queue publication; B submits and completes a normal synchronous NOP.
 * All state is protected by g->lock; no GPU lock is held across a wait.
 */
static struct {
    int initialized;
    uint64 owner_id;
    pid_t owner_tgid;
    int a_seen;
    int b_seen;
    int failed;
    uint64 b_fence;
    completion_t a_prepared;
    completion_t b_completed;
} virtio_gpu_fence_probe;

static int virtio_gpu_fence_probe_enabled(void)
{
    static int enabled = -1;
    int value = __atomic_load_n(&enabled, __ATOMIC_ACQUIRE);
    char param[16];

    if (value < 0) {
        value = cmdline_get_param("virtio_gpu_fence_order_probe", param,
                                  sizeof(param)) == 0 &&
            cmdline_value_is_true(param);
        __atomic_store_n(&enabled, value, __ATOMIC_RELEASE);
    }
    return value;
}

/* Returns zero for ordinary clients, a positive role, or a bounded error. */
static int virtio_gpu_fence_probe_before(struct virtio_gpu *g,
                                          uint64 owner_id, pid_t owner_tgid,
                                          uint32 ctx_id, int async_submit,
                                          uint64 reserved_fence)
{
    struct virtio_gpu_context *ctx;
    int role = 0;
    int failed;
    uint64 done;
    uint64 b_fence;

    if (!virtio_gpu_fence_probe_enabled())
        return 0;

    spin_lock(&g->lock);
    ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
    if (ctx != NULL && ctx->owner_id == owner_id &&
        ctx->owner_tgid == owner_tgid) {
        if (async_submit && strcmp(ctx->debug_name, "virgl-fence-A") == 0)
            role = 1;
        else if (!async_submit &&
                 strcmp(ctx->debug_name, "virgl-fence-B") == 0)
            role = 2;
    }
    if (role == 0) {
        spin_unlock(&g->lock);
        return 0;
    }
    if (!virtio_gpu_fence_probe.initialized) {
        completion_init(&virtio_gpu_fence_probe.a_prepared);
        completion_init(&virtio_gpu_fence_probe.b_completed);
        virtio_gpu_fence_probe.owner_id = owner_id;
        virtio_gpu_fence_probe.owner_tgid = owner_tgid;
        virtio_gpu_fence_probe.initialized = 1;
    }
    if (virtio_gpu_fence_probe.owner_id != owner_id ||
        virtio_gpu_fence_probe.owner_tgid != owner_tgid ||
        (role == 1 && virtio_gpu_fence_probe.a_seen) ||
        (role == 2 && virtio_gpu_fence_probe.b_seen)) {
        spin_unlock(&g->lock);
        return -EBUSY;
    }
    if (role == 1)
        virtio_gpu_fence_probe.a_seen = 1;
    else
        virtio_gpu_fence_probe.b_seen = 1;
    spin_unlock(&g->lock);

    if (role == 1)
        complete_all(&virtio_gpu_fence_probe.a_prepared);
    if (wait_for_completion_timeout(role == 1 ?
            &virtio_gpu_fence_probe.b_completed :
            &virtio_gpu_fence_probe.a_prepared, 5000) == 0) {
        spin_lock(&g->lock);
        virtio_gpu_fence_probe.failed = 1;
        spin_unlock(&g->lock);
        complete_all(&virtio_gpu_fence_probe.a_prepared);
        complete_all(&virtio_gpu_fence_probe.b_completed);
        printf("virtio-gpu-fence-probe: role=%d status=FAIL wait-timeout\n",
               role);
        return -ETIMEDOUT;
    }

    spin_lock(&g->lock);
    failed = virtio_gpu_fence_probe.failed;
    done = g->stats.last_fence;
    b_fence = virtio_gpu_fence_probe.b_fence;
    spin_unlock(&g->lock);
    if (role == 1)
        printf("virtio-gpu-fence-probe: A-unpublished reserved=%lu completed=%lu B=%lu prematurely-complete=%d status=%s\n",
               reserved_fence, done, b_fence,
               reserved_fence != 0 && done >= reserved_fence,
               failed ? "FAIL" : "READY");
    return failed ? -EIO : role;
}

static void virtio_gpu_fence_probe_after(struct virtio_gpu *g, int role,
                                         int ret, uint64 fence)
{
    uint64 done;
    uint64 b_fence;
    int failed;

    if (role <= 0)
        return;
    spin_lock(&g->lock);
    done = g->stats.last_fence;
    if (ret != 0 || fence == 0 || (role == 2 && done < fence))
        virtio_gpu_fence_probe.failed = 1;
    if (role == 2)
        virtio_gpu_fence_probe.b_fence = fence;
    b_fence = virtio_gpu_fence_probe.b_fence;
    failed = virtio_gpu_fence_probe.failed;
    spin_unlock(&g->lock);
    printf("virtio-gpu-fence-probe: role=%d published=%lu completed=%lu B=%lu ret=%d status=%s\n",
           role, fence, done, b_fence, ret,
           failed || (role == 1 && fence <= b_fence) ? "FAIL" : "PASS");
    if (role == 2)
        complete_all(&virtio_gpu_fence_probe.b_completed);
}
