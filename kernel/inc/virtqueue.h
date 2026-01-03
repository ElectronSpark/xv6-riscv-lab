//
// Generic virtqueue management for virtio devices.
// Implements the split virtqueue layout from virtio spec 1.1.
//

#ifndef __KERNEL_VIRTQUEUE_H
#define __KERNEL_VIRTQUEUE_H

#include "types.h"
#include "virtio.h"
#include "freelist.h"
#include "spinlock.h"

// Generic virtqueue structure that can be used by any virtio device
struct virtqueue {
  // Virtio queue structures (from spec)
  struct virtq_desc *desc;
  struct virtq_avail *avail;
  struct virtq_used *used;
  
  // Resource management
  char free[NUM];           // is descriptor free?
  uint16 free_list[NUM];    // free descriptor indices
  struct freelist desc_freelist;  // Freelist manager
  
  // Queue state
  uint16 used_idx;          // last processed index in used ring
  uint16 num;               // number of descriptors in queue
  
  // Synchronization
  struct spinlock lock;     // protects this virtqueue
};

// Initialize a virtqueue with allocated memory
// desc, avail, used must be page-aligned and zero-initialized
static inline void
virtqueue_init(struct virtqueue *vq, struct virtq_desc *desc,
               struct virtq_avail *avail, struct virtq_used *used,
               uint16 num, const char *lock_name)
{
  vq->desc = desc;
  vq->avail = avail;
  vq->used = used;
  vq->num = num;
  vq->used_idx = 0;
  
  spin_init(&vq->lock, lock_name);
  freelist_init(&vq->desc_freelist, vq->free, vq->free_list, num);
}

// Allocate a single descriptor
static inline int
virtqueue_alloc_desc(struct virtqueue *vq)
{
  return freelist_alloc(&vq->desc_freelist);
}

// Free a single descriptor
static inline void
virtqueue_free_desc(struct virtqueue *vq, int i)
{
  if(freelist_free(&vq->desc_freelist, i) != 0)
    panic("virtqueue_free_desc: invalid free");
  
  // Clear descriptor
  vq->desc[i].addr = 0;
  vq->desc[i].len = 0;
  vq->desc[i].flags = 0;
  vq->desc[i].next = 0;
}

// Free a chain of descriptors
static inline void
virtqueue_free_chain(struct virtqueue *vq, int i)
{
  while(1) {
    int flag = vq->desc[i].flags;
    int nxt = vq->desc[i].next;
    virtqueue_free_desc(vq, i);
    if(flag & VRING_DESC_F_NEXT)
      i = nxt;
    else
      break;
  }
}

// Check how many descriptors are available
static inline int
virtqueue_available_desc(struct virtqueue *vq)
{
  return freelist_available(&vq->desc_freelist);
}

// Add a buffer to the available ring
// head is the first descriptor in the chain
static inline void
virtqueue_add_buf(struct virtqueue *vq, uint16 head)
{
  vq->avail->ring[vq->avail->idx % vq->num] = head;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  vq->avail->idx += 1;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

// Check if there are completed buffers in the used ring
static inline int
virtqueue_has_used_buf(struct virtqueue *vq)
{
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  return vq->used_idx != vq->used->idx;
}

// Get next completed buffer from used ring
// Returns descriptor id, or -1 if none available
static inline int
virtqueue_get_used_buf(struct virtqueue *vq, uint32 *len)
{
  if(!virtqueue_has_used_buf(vq))
    return -1;
  
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  int id = vq->used->ring[vq->used_idx % vq->num].id;
  if(len)
    *len = vq->used->ring[vq->used_idx % vq->num].len;
  
  vq->used_idx += 1;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  
  return id;
}

#endif // __KERNEL_VIRTQUEUE_H
