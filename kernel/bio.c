// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple threads.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
//   NOTE: bread() returns NULL on memory allocation failure (OOM).
//   Callers must handle this gracefully (return -EIO or similar).
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one thread at a time can use a buffer,
//     so do not keep them longer than necessary.
//
// Locking order:
// 1. bcache.lock (spinlock) - protects LRU list and hash table
// 2. buf->lock (mutex) - protects individual buffer contents
// 3. disk I/O completion (via bio_await)
//
// bread() acquires buf->lock and may block waiting for disk I/O.
// Callers should not hold other sleeping locks while holding buffer locks
// if those locks might be needed by the disk interrupt handler path.

#include "types.h"
#include "param.h"
#include "errno.h"
#include "lock/spinlock.h"
#include "lock/mutex_types.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "vfs/xv6fs/ondisk.h" // for BSIZE
#include "dev/buf.h"
#include "dev/bio.h"
#include <mm/page.h>
#include <mm/folio.h>
#include "dev/blkdev.h"
#include "dev/iosched.h"
#include "list.h"
#include "hlist.h"
#include "proc/thread.h"
#include "accounting.h"
#include "kstats.h"

/* Global block I/O counters (read by sys_kstats) */
uint64 g_bio_reads;
uint64 g_bio_writes;
uint64 g_bio_read_bytes;
uint64 g_bio_write_bytes;

struct {
    spinlock_t lock;
    struct buf buf[NBUF];

    // Free list of unused buffers (refcnt == 0), sorted by LRU order.
    // Push at head (most recently used), pop from tail (oldest/least recently
    // used).
    list_node_t free_list;

    // Dirty list of buffers that need writeback.
    // Buffers are added when marked dirty, removed after writeback.
    list_node_t dirty_list;
    uint dirty_count; // number of dirty buffers

    hlist_t cached; // Hash list of buffers, sorted by (dev, blockno).
    hlist_bucket_t
        buckets[BIO_HASH_BUCKETS]; // stores the hash buckets of the hash list
} bcache;

static ht_hash_t __bcache_hash_func(void *node) {
    struct buf *bnode = node;
    ht_hash_t h = hlist_hash_uint64(bnode->blockno) + bnode->dev;
    return hlist_hash_uint64(h);
}

static void *__bcache_hlist_get_node(hlist_entry_t *entry) {
    return container_of(entry, struct buf, hlist_entry);
}

static hlist_entry_t *__bcache_hlist_get_entry(void *node) {
    struct buf *bnode = node;
    return &bnode->hlist_entry;
}

static int __bcache_hlist_cmp(hlist_t *hlist, void *node1, void *node2) {
    struct buf *bnode1 = node1;
    struct buf *bnode2 = node2;
    if (bnode1->dev > bnode2->dev) {
        return 1;
    } else if (bnode1->dev < bnode2->dev) {
        return -1;
    }

    if (bnode1->blockno > bnode2->blockno) {
        return 1;
    } else if (bnode1->blockno < bnode2->blockno) {
        return -1;
    }

    return 0;
}

static inline struct buf *__bcache_hlist_get(uint dev, uint blockno) {
    // Create a dummy node to search for
    struct buf dummy = {0};
    dummy.dev = dev;
    dummy.blockno = blockno;

    return hlist_get(&bcache.cached, &dummy);
}

static inline struct buf *__bcache_hlist_pop(uint dev, uint blockno) {
    // Create a dummy node to search for
    struct buf dummy = {0};
    dummy.dev = dev;
    dummy.blockno = blockno;

    struct buf *buf = hlist_pop(&bcache.cached, &dummy);
    return buf;
}

static inline int __bcache_hlist_push(struct buf *buf) {
    struct buf *entry = hlist_put(&bcache.cached, buf, false);
    if (entry == NULL) {
        return 0; // succeeded
    } else if (entry != buf) {
        return -1; // failed to insert
    } else {
        return -1; // the entry is already in the hash list
    }
}

// preallocate memory for buffer cache
static void __buf_cache_prealloc(void) {
    int page_blocks = PGSIZE / BSIZE;
    int pages_needed = (NBUF + page_blocks - 1) / page_blocks;
    for (int i = 0; i < pages_needed; i++) {
        void *pa = page_alloc(0, PAGE_TYPE_ANON);
        assert(pa != NULL, "__buf_cache_prealloc: page_alloc failed");
        for (int j = 0; j < page_blocks; j++) {
            int buf_idx = i * page_blocks + j;
            if (buf_idx >= NBUF) {
                break;
            }
            bcache.buf[buf_idx].data = (uchar *)pa + j * BSIZE;
        }
    }
}

void binit(void) {
    struct buf *b;

    spin_init(&bcache.lock, "bcache");

    // Create linked list of buffers
    list_entry_init(&bcache.free_list);
    list_entry_init(&bcache.dirty_list);
    bcache.dirty_count = 0;

    hlist_func_t hlist_func = {.hash = __bcache_hash_func,
                               .get_node = __bcache_hlist_get_node,
                               .get_entry = __bcache_hlist_get_entry,
                               .cmp_node = __bcache_hlist_cmp};
    hlist_init(&bcache.cached, BIO_HASH_BUCKETS, &hlist_func);
    for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
        list_entry_init(&b->free_entry);
        list_entry_init(&b->dirty_entry);
        b->dirty = 0;
        mutex_init(&b->lock, "buffer");
        // Add to free list
        list_entry_push(&bcache.free_list, &b->free_entry);
    }
    __buf_cache_prealloc();
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
STATIC struct buf *bget(uint dev, uint blockno) {
    struct buf *b, *b1;

    spin_lock(&bcache.lock);

    // Is the block already cached?
    b = __bcache_hlist_get(dev, blockno);
    if (b != NULL) {
        // Found it.
        // Remove from free list if it's there (refcnt was 0)
        if (!LIST_NODE_IS_DETACHED(b, free_entry)) {
            list_node_detach(b, free_entry);
        }
        b->refcnt++;
        spin_unlock(&bcache.lock);
        mutex_lock(&b->lock);
        return b;
    }

    // Not cached.
    // Get a free buffer from the free list (O(1) instead of O(n) scan)
    if (LIST_IS_EMPTY(&bcache.free_list)) {
        panic("bget: no buffers");
    }

    // Pop from free list (get oldest free buffer for LRU behavior)
    b = list_node_pop_back(&bcache.free_list, struct buf, free_entry);

    // Remove from hash table if it was caching a different block
    b1 = __bcache_hlist_pop(b->dev, b->blockno);
    if (b1 && b1 != b) {
        if (b->blockno != 0 || b->dev != 0) {
            // Only unused buffers could clash, otherwise it is a bug.
            printf("bget: found a buffer with blockno %d, dev %d, but "
                   "it is not the same as the one we are recycling\n",
                   b1->blockno, b1->dev);
            panic("bget: found a buffer that is not the same as the "
                  "one we are recycling");
        }
        // the buffer b is unused, so we can put back b1 and safely use b
        if (__bcache_hlist_push(b1) != 0) {
            panic("bget: failed to push cached buffer into hash list");
        }
    }

    __atomic_thread_fence(
        __ATOMIC_SEQ_CST); // Ensure the buffer is detached before using it

    b->dev = dev;
    b->blockno = blockno;
    b->valid = 0;
    b->refcnt = 1;
    if (__bcache_hlist_push(b) != 0) {
        printf("dev: %d, blockno: %d\n", dev, blockno);
        panic("bget: failed to push recycled buffer into hash list");
    }
    spin_unlock(&bcache.lock);
    mutex_lock(&b->lock);
    return b;
}

static struct bio *__buf_alloc_bio(struct buf *b, blkdev_t *blkdev,
                                   bool write) {
    struct bio *bio = bio_alloc(blkdev, 1, write, NULL, NULL);
    if (IS_ERR_OR_NULL(bio)) {
        return NULL;
    }
    bio->blkno = b->blockno * (BSIZE >> 9);
    page_t *page = __pa_to_page((uint64)b->data & ~PAGE_MASK);
    size_t page_offset = (uint64)b->data & PAGE_MASK;
    int ret = bio_add_folio(bio, page_folio(page), BSIZE, page_offset);
    if (ret != 0) {
        bio_release(bio);
        return NULL;
    }
    return bio;
}

static void __buf_bio_cleanup(struct bio *bio) {
    if (bio) {
        bio_release(bio);
    }
}

// Return a locked buf with the contents of the indicated block.
// Returns NULL if memory allocation fails (e.g., during OOM conditions)
// or if the calling thread was interrupted by a signal.
struct buf *bread(uint dev, uint blockno) {
    struct buf *b;

    b = bget(dev, blockno);
    if (!b->valid) {
        blkdev_t *blkdev = blkdev_get(major(b->dev), minor(b->dev));
        assert(!IS_ERR(blkdev), "bread: blkdev_get failed");
        struct bio *bio = __buf_alloc_bio(b, blkdev, false);
        if (IS_ERR_OR_NULL(bio)) {
            // OOM during bio allocation - release buffer and return NULL
            // Callers should handle this gracefully
            int ret = blkdev_put(blkdev);
            assert(ret == 0, "bread: blkdev_put failed: %d", ret);
            brelse(b);
            return NULL;
        }
        int err = blkdev_submit_bio(blkdev, bio);
        if (err == 0)
            err = bio_await(bio);
        __buf_bio_cleanup(bio);
        int ret = blkdev_put(blkdev);
        assert(ret == 0, "bread: blkdev_put failed: %d", ret);
        if (err) {
            // I/O error or interrupted — don't mark valid, release buffer
            brelse(b);
            return NULL;
        }
        b->valid = 1;
    }
    if (current && current->thread_group)
        ACCT_INC(current->thread_group, bio_reads);
    g_bio_reads += 1;
    g_bio_read_bytes += BSIZE;
    return b;
}

// Write b's contents to disk.  Must be locked.
void bwrite(struct buf *b) {
    if (!holding_mutex(&b->lock))
        panic("bwrite");
    if (current && current->thread_group)
        ACCT_INC(current->thread_group, bio_writes);
    g_bio_writes += 1;
    g_bio_write_bytes += BSIZE;
    blkdev_t *blkdev = blkdev_get(major(b->dev), minor(b->dev));
    assert(!IS_ERR(blkdev), "bwrite: blkdev_get failed");
    struct bio *bio = __buf_alloc_bio(b, blkdev, true);
    assert(!IS_ERR_OR_NULL(bio), "bwrite: bio_alloc failed");
    blkdev_submit_bio(blkdev, bio);
    bio_await(bio);
    __buf_bio_cleanup(bio);

    // Clear dirty flag after successful write
    spin_lock(&bcache.lock);
    if (b->dirty) {
        b->dirty = 0;
        if (!LIST_NODE_IS_DETACHED(b, dirty_entry)) {
            list_node_detach(b, dirty_entry);
            bcache.dirty_count--;
        }
    }
    spin_unlock(&bcache.lock);

    int ret = blkdev_put(blkdev);
    assert(ret == 0, "bwrite: blkdev_put failed: %d", ret);
}

// Mark buffer as dirty for later writeback. Must be locked.
// This is much faster than bwrite() as it doesn't block on disk I/O.
void bwrite_async(struct buf *b) {
    if (!holding_mutex(&b->lock))
        panic("bwrite_async");
    if (current && current->thread_group)
        ACCT_INC(current->thread_group, bio_writes);
    g_bio_writes += 1;
    g_bio_write_bytes += BSIZE;

    spin_lock(&bcache.lock);
    if (!b->dirty) {
        b->dirty = 1;
        // Add to dirty list (at tail for FIFO writeback order)
        list_node_push_back(&bcache.dirty_list, b, dirty_entry);
        bcache.dirty_count++;
    }
    spin_unlock(&bcache.lock);
}

// Flush all dirty buffers to disk.
// Called periodically or on sync().
//
// Buffers are collected in batches, all BIOs are submitted, then all are
// awaited — turning N sequential I/O round-trips into one parallel batch.
//
// The IO scheduler sorts bios within each batch using the C-SCAN elevator
// algorithm so the device sees requests in ascending sector order (fewer
// seeks on rotational media, and friendly to sequential prefetch on SSDs).
#define BSYNC_BATCH_MAX 64

void bsync(void) {
    struct {
        struct buf *buf;
        struct bio *bio;
        blkdev_t *blkdev;
    } batch[BSYNC_BATCH_MAX];
    struct bio *dispatched[BSYNC_BATCH_MAX];
    int flushed = 0;

    while (1) {
        int n = 0;

        /* Phase 1: Collect up to BSYNC_BATCH_MAX dirty buffers */
        spin_lock(&bcache.lock);
        while (!LIST_IS_EMPTY(&bcache.dirty_list) && n < BSYNC_BATCH_MAX) {
            struct buf *b =
                list_node_pop_back(&bcache.dirty_list, struct buf, dirty_entry);
            b->dirty = 0;
            bcache.dirty_count--;
            if (b->refcnt == 0 && !LIST_NODE_IS_DETACHED(b, free_entry)) {
                list_node_detach(b, free_entry);
            }
            b->refcnt++;
            batch[n].buf = b;
            batch[n].bio = NULL;
            batch[n].blkdev = NULL;
            n++;
        }
        spin_unlock(&bcache.lock);

        if (n == 0)
            break;

        /* Phase 2: Lock each buffer, create bio, enqueue in IO scheduler.
         * Bios are NOT submitted to the driver yet — the scheduler
         * accumulates them so it can dispatch in elevator order. */
        blkdev_t *sched_bdev = NULL; /* scheduler device for this batch */
        for (int i = 0; i < n; i++) {
            struct buf *b = batch[i].buf;
            mutex_lock(&b->lock);

            if (b->valid) {
                blkdev_t *blkdev = blkdev_get(major(b->dev), minor(b->dev));
                if (!IS_ERR(blkdev)) {
                    struct bio *bio = __buf_alloc_bio(b, blkdev, true);
                    if (!IS_ERR_OR_NULL(bio)) {
                        batch[i].bio = bio;
                        batch[i].blkdev = blkdev;
                        sched_bdev = blkdev;
                        iosched_enqueue(&blkdev->iosched, bio);
                    } else {
                        blkdev_put(blkdev);
                    }
                }
            }
        }

        /* Phase 2b: Dispatch all enqueued bios in elevator order.
         * iosched_dispatch_batch() picks bios in C-SCAN order and
         * submits each to the driver via blkdev->ops.submit_bio(). */
        int ndispatched = 0;
        if (sched_bdev != NULL) {
            ndispatched = iosched_dispatch_batch(
                &sched_bdev->iosched, dispatched, n);
        }

        /* Phase 3: Await all in-flight bios in parallel */
        for (int i = 0; i < ndispatched; i++) {
            bio_await(dispatched[i]);
        }
        /* Clean up bio resources and unlock buffers */
        for (int i = 0; i < n; i++) {
            if (batch[i].bio != NULL) {
                __buf_bio_cleanup(batch[i].bio);
                blkdev_put(batch[i].blkdev);
                flushed++;
            }
            mutex_unlock(&batch[i].buf->lock);
        }

        /* Phase 4: Release buffer references in one lock acquisition */
        spin_lock(&bcache.lock);
        for (int i = 0; i < n; i++) {
            struct buf *b = batch[i].buf;
            b->refcnt--;
            if (b->refcnt == 0) {
                list_node_push(&bcache.free_list, b, free_entry);
            }
        }
        spin_unlock(&bcache.lock);
    }

    if (flushed > 0) {
        // Could add debug output here if needed
    }
}

// Get count of dirty buffers (for debugging/stats)
uint bdirty_count(void) {
    spin_lock(&bcache.lock);
    uint count = bcache.dirty_count;
    spin_unlock(&bcache.lock);
    return count;
}

// release a locked buffer.
// Move to the head of the most-recently-used list.
void brelse(struct buf *b) {
    if (!holding_mutex(&b->lock))
        panic("brelse");

    mutex_unlock(&b->lock);

    spin_lock(&bcache.lock);
    b->refcnt--;
    if (b->refcnt == 0) {
        // no one is waiting for it.
        // Add to free list (most recently used at head, oldest at tail)
        list_node_push(&bcache.free_list, b, free_entry);
    }

    spin_unlock(&bcache.lock);
}

void bpin(struct buf *b) {
    spin_lock(&bcache.lock);
    // If refcnt was 0, remove from free list
    if (b->refcnt == 0 && !LIST_NODE_IS_DETACHED(b, free_entry)) {
        list_node_detach(b, free_entry);
    }
    b->refcnt++;
    spin_unlock(&bcache.lock);
}

void bunpin(struct buf *b) {
    spin_lock(&bcache.lock);
    b->refcnt--;
    // If refcnt becomes 0, add to free list
    if (b->refcnt == 0) {
        list_node_push(&bcache.free_list, b, free_entry);
    }
    spin_unlock(&bcache.lock);
}
