#ifndef __KERNEL_DEV_DMA_FENCE_H
#define __KERNEL_DEV_DMA_FENCE_H

#include <types.h>
#include <list_type.h>
#include <lock/spinlock.h>

struct dma_fence;

struct dma_fence_cb {
    list_node_t node;
    void (*fn)(struct dma_fence *fence, void *arg);
    void *arg;
};

struct dma_fence {
    uint64 context;
    uint64 seqno;
    int signaled;
    int error;
    uint32 refs;
    uint64 wakeup_seq;
    list_node_t callbacks;
    spinlock_t lock;
};

uint64 dma_fence_context_alloc(void);
void dma_fence_init(struct dma_fence *fence, uint64 context, uint64 seqno);
struct dma_fence *dma_fence_get(struct dma_fence *fence);
void dma_fence_put(struct dma_fence *fence);
int dma_fence_signal(struct dma_fence *fence, int error);
int dma_fence_add_callback(struct dma_fence *fence, struct dma_fence_cb *cb,
                           void (*fn)(struct dma_fence *fence, void *arg),
                           void *arg);
int dma_fence_remove_callback(struct dma_fence *fence,
                              struct dma_fence_cb *cb);
int dma_fence_is_signaled(struct dma_fence *fence);
int dma_fence_wait(struct dma_fence *fence, int64 timeout_ns);

int dma_fence_selftest(void);

#endif /* __KERNEL_DEV_DMA_FENCE_H */
