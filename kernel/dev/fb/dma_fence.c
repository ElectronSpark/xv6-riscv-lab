#include <types.h>
#include <defs.h>
#include <errno.h>
#include <string.h>
#include <list.h>
#include <dev/dma_fence.h>
#include <mm/vm.h>
#include <proc/sched.h>
#include <timer/timer.h>

#define DMA_FENCE_NSEC_PER_MSEC 1000000ULL

static spinlock_t dma_fence_context_lock =
    SPINLOCK_INITIALIZED("dma_fence_context");
static uint64 dma_fence_next_context = 1;
static int dma_fence_selftest_done;

static uint64 dma_fence_timeout_ms(int64 timeout_ns)
{
    if (timeout_ns <= 0)
        return 0;
    return ((uint64)timeout_ns + DMA_FENCE_NSEC_PER_MSEC - 1) /
        DMA_FENCE_NSEC_PER_MSEC;
}

static void dma_fence_timeout_wakeup(void *data)
{
    struct dma_fence *fence = (struct dma_fence *)data;

    if (fence == NULL)
        return;
    wakeup_on_chan(&fence->wakeup_seq);
    dma_fence_put(fence);
}

uint64 dma_fence_context_alloc(void)
{
    uint64 context;

    spin_lock(&dma_fence_context_lock);
    context = dma_fence_next_context++;
    if (dma_fence_next_context == 0)
        dma_fence_next_context = 1;
    if (context == 0)
        context = dma_fence_next_context++;
    spin_unlock(&dma_fence_context_lock);
    return context;
}

void dma_fence_init(struct dma_fence *fence, uint64 context, uint64 seqno)
{
    if (fence == NULL)
        return;
    memset(fence, 0, sizeof(*fence));
    fence->context = context != 0 ? context : dma_fence_context_alloc();
    fence->seqno = seqno;
    fence->refs = 1;
    list_entry_init(&fence->callbacks);
    spin_init(&fence->lock, "dma_fence");
}

struct dma_fence *dma_fence_get(struct dma_fence *fence)
{
    if (fence == NULL)
        return NULL;
    spin_lock(&fence->lock);
    if (fence->refs != 0)
        fence->refs++;
    spin_unlock(&fence->lock);
    return fence;
}

void dma_fence_put(struct dma_fence *fence)
{
    int do_free = 0;

    if (fence == NULL)
        return;
    spin_lock(&fence->lock);
    if (fence->refs != 0) {
        fence->refs--;
        if (fence->refs == 0)
            do_free = 1;
    }
    spin_unlock(&fence->lock);
    if (do_free)
        kvfree(fence);
}

void dma_fence_set_virtio_fence(struct dma_fence *fence, uint64 virtio_fence)
{
    if (fence == NULL)
        return;
    spin_lock(&fence->lock);
    fence->virtio_fence = virtio_fence;
    spin_unlock(&fence->lock);
}

uint64 dma_fence_get_virtio_fence(struct dma_fence *fence)
{
    uint64 virtio_fence;

    if (fence == NULL)
        return 0;
    spin_lock(&fence->lock);
    virtio_fence = fence->virtio_fence;
    spin_unlock(&fence->lock);
    return virtio_fence;
}

int dma_fence_signal(struct dma_fence *fence, int error)
{
    list_node_t callbacks;
    struct dma_fence_cb *cb;
    struct dma_fence_cb *tmp;
    int run_callbacks = 0;

    if (fence == NULL)
        return -EINVAL;

    list_entry_init(&callbacks);
    spin_lock(&fence->lock);
    if (fence->signaled) {
        if (fence->error == 0 && error != 0)
            fence->error = error;
        spin_unlock(&fence->lock);
        return 0;
    }

    fence->signaled = 1;
    fence->error = error;
    fence->wakeup_seq++;
    list_entry_insert_bulk(&callbacks, &fence->callbacks);
    run_callbacks = !LIST_IS_EMPTY(&callbacks);
    wakeup_on_chan(&fence->wakeup_seq);
    if (run_callbacks)
        fence->refs++;
    spin_unlock(&fence->lock);

    list_foreach_node_safe(&callbacks, cb, tmp, node) {
        list_node_detach(cb, node);
        if (cb->fn != NULL)
            cb->fn(fence, cb->arg);
    }
    if (run_callbacks)
        dma_fence_put(fence);
    return 0;
}

int dma_fence_add_callback(struct dma_fence *fence, struct dma_fence_cb *cb,
                           void (*fn)(struct dma_fence *fence, void *arg),
                           void *arg)
{
    if (fence == NULL || cb == NULL || fn == NULL)
        return -EINVAL;

    spin_lock(&fence->lock);
    if (fence->signaled || fence->error != 0) {
        spin_unlock(&fence->lock);
        return -ENOENT;
    }
    list_entry_init(&cb->node);
    cb->fn = fn;
    cb->arg = arg;
    list_node_push(&fence->callbacks, cb, node);
    spin_unlock(&fence->lock);
    return 0;
}

int dma_fence_remove_callback(struct dma_fence *fence,
                              struct dma_fence_cb *cb)
{
    int ret = -ENOENT;

    if (fence == NULL || cb == NULL)
        return -EINVAL;

    spin_lock(&fence->lock);
    if (!LIST_ENTRY_IS_DETACHED(&cb->node)) {
        list_node_detach(cb, node);
        ret = 0;
    }
    spin_unlock(&fence->lock);
    return ret;
}

int dma_fence_is_signaled(struct dma_fence *fence)
{
    int signaled;

    if (fence == NULL)
        return 0;
    spin_lock(&fence->lock);
    signaled = fence->signaled || fence->error != 0;
    spin_unlock(&fence->lock);
    return signaled;
}

int dma_fence_wait(struct dma_fence *fence, int64 timeout_ns)
{
    uint64 timeout_ms = 0;
    uint64 deadline_ms = 0;
    int finite = timeout_ns >= 0;
    int ret = 0;

    if (fence == NULL)
        return -EINVAL;

    if (finite) {
        timeout_ms = dma_fence_timeout_ms(timeout_ns);
        deadline_ms = sched_timer_now_ms() + timeout_ms;
        if (timeout_ms != 0) {
            if (dma_fence_get(fence) == NULL)
                return -EINVAL;
            if (sched_timer_add(dma_fence_timeout_wakeup, fence,
                                timeout_ms) != 0) {
                dma_fence_put(fence);
                timeout_ms = 0;
            }
        }
    }

    spin_lock(&fence->lock);
    while (!fence->signaled && fence->error == 0) {
        if (finite &&
            (timeout_ms == 0 || sched_timer_now_ms() >= deadline_ms)) {
            ret = -ETIME;
            break;
        }
        ret = sleep_on_chan_interruptible(&fence->wakeup_seq, &fence->lock);
        if (ret != 0)
            break;
    }
    if (ret == 0 && fence->error != 0)
        ret = fence->error < 0 ? fence->error : -fence->error;
    spin_unlock(&fence->lock);
    return ret;
}

int dma_fence_wait_uninterruptible(struct dma_fence *fence)
{
    int ret = 0;

    if (fence == NULL)
        return -EINVAL;

    spin_lock(&fence->lock);
    while (!fence->signaled && fence->error == 0)
        sleep_on_chan(&fence->wakeup_seq, &fence->lock);
    if (fence->error != 0)
        ret = fence->error < 0 ? fence->error : -fence->error;
    spin_unlock(&fence->lock);
    return ret;
}

struct dma_fence_selftest_state {
    int callbacks;
    uint64 context;
    uint64 seqno;
    int signaled;
};

static void dma_fence_selftest_cb(struct dma_fence *fence, void *arg)
{
    struct dma_fence_selftest_state *state =
        (struct dma_fence_selftest_state *)arg;

    state->callbacks++;
    state->context = fence->context;
    state->seqno = fence->seqno;
    state->signaled = dma_fence_is_signaled(fence);
}

int dma_fence_selftest(void)
{
    struct dma_fence *fence;
    struct dma_fence_cb cb;
    struct dma_fence_selftest_state state;
    int ret;

    if (dma_fence_selftest_done)
        return 0;

    fence = kvmalloc(sizeof(*fence));
    if (fence == NULL)
        return -ENOMEM;
    memset(&cb, 0, sizeof(cb));
    memset(&state, 0, sizeof(state));
    dma_fence_init(fence, 0, 7);

    ret = dma_fence_add_callback(fence, &cb, dma_fence_selftest_cb, &state);
    if (ret != 0)
        goto out;
    ret = dma_fence_signal(fence, 0);
    if (ret != 0)
        goto out;
    ret = dma_fence_wait(fence, 0);
    if (ret != 0)
        goto out;
    if (state.callbacks != 1 || state.context != fence->context ||
        state.seqno != fence->seqno || !state.signaled) {
        ret = -EINVAL;
        goto out;
    }

    dma_fence_selftest_done = 1;
out:
    dma_fence_put(fence);
    return ret;
}
