// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
//   NOTE: bread() returns NULL on memory allocation failure (OOM).
//   Callers must handle this gracefully (return -EIO or similar).
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.
//
// Locking order:
// 1. bcache.lock (spinlock) - protects LRU list and hash table
// 2. buf->lock (mutex) - protects individual buffer contents
// 3. disk I/O completion (via wait_for_completion)
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
#include <mm/page.h>
#include "dev/blkdev.h"
#include "list.h"
#include "hlist.h"

struct {
    struct spinlock lock;
    struct buf buf[NBUF];

    // Linked list of all buffers, through prev/next.
    // Sorted by how recently the buffer was used.
    // head.next is most recent, head.prev is least.
    list_node_t lru_entry; // For debugging, not used in the code.
    hlist_t cached;        // Hash list of buffers, sorted by (dev, blockno).
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
    list_entry_init(&bcache.lru_entry);
    hlist_func_t hlist_func = {.hash = __bcache_hash_func,
                               .get_node = __bcache_hlist_get_node,
                               .get_entry = __bcache_hlist_get_entry,
                               .cmp_node = __bcache_hlist_cmp};
    hlist_init(&bcache.cached, BIO_HASH_BUCKETS, &hlist_func);
    for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
        list_entry_init(&b->lru_entry);
        mutex_init(&b->lock, "buffer");
        list_entry_push(&bcache.lru_entry, &b->lru_entry);
    }
    __buf_cache_prealloc();
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
STATIC struct buf *bget(uint dev, uint blockno) {
    struct buf *b, *b1, *tmp;

    spin_lock(&bcache.lock);

    // Is the block already cached?
    b = __bcache_hlist_get(dev, blockno);
    if (b != NULL) {
        // Found it.
        if (!LIST_NODE_IS_DETACHED(b, lru_entry)) {
            list_node_detach(b, lru_entry);
        }
        b->refcnt++;
        spin_unlock(&bcache.lock);
        assert(mutex_lock(&b->lock) == 0, "bget: failed to lock buffer");
        return b;
    }

    // Not cached.
    // Recycle the least recently used (LRU) unused buffer.
    list_foreach_node_safe(&bcache.lru_entry, b, tmp, lru_entry) {
        if (b->refcnt == 0) {
            // Found an unused buffer.
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
                // the buffer b is unused, so we can put back b1 and safely use
                // b
                if (__bcache_hlist_push(b1) != 0) {
                    panic("bget: failed to push cached buffer into hash list");
                }
            }
            list_node_detach(b, lru_entry);
            __atomic_thread_fence(__ATOMIC_SEQ_CST); // Ensure the buffer is
                                                     // detached before using it
            // list_node_push(&bcache.lru_entry, b, lru_entry);
            b->dev = dev;
            b->blockno = blockno;
            b->valid = 0;
            b->refcnt = 1;
            if (__bcache_hlist_push(b) != 0) {
                printf("dev: %d, blockno: %d\n", dev, blockno);
                panic("bget: failed to push recycled buffer into hash list");
            }
            spin_unlock(&bcache.lock);
            assert(mutex_lock(&b->lock) == 0, "bget: failed to lock buffer");
            return b;
        }
    }
    panic("bget: no buffers");
}

static struct bio *__buf_alloc_bio(struct buf *b, blkdev_t *blkdev,
                                   bool write) {
    struct bio *bio = bio_alloc(blkdev, 1, write, NULL, NULL);
    if (IS_ERR_OR_NULL(bio)) {
        return NULL;
    }
    bio->blkno = b->blockno * (BSIZE / 512);
    page_t *page = __pa_to_page((uint64)b->data & ~PAGE_MASK);
    size_t page_offset = (uint64)b->data & PAGE_MASK;
    int ret = bio_add_seg(bio, page, 0, BSIZE, page_offset);
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
// Returns NULL if memory allocation fails (e.g., during OOM conditions).
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
        blkdev_submit_bio(blkdev, bio);
        b->valid = 1;
        __buf_bio_cleanup(bio);
        int ret = blkdev_put(blkdev);
        assert(ret == 0, "bread: blkdev_put failed: %d", ret);
    }
    return b;
}

// Write b's contents to disk.  Must be locked.
void bwrite(struct buf *b) {
    if (!holding_mutex(&b->lock))
        panic("bwrite");
    blkdev_t *blkdev = blkdev_get(major(b->dev), minor(b->dev));
    assert(!IS_ERR(blkdev), "bwrite: blkdev_get failed");
    struct bio *bio = __buf_alloc_bio(b, blkdev, true);
    assert(!IS_ERR_OR_NULL(bio), "bwrite: bio_alloc failed");
    blkdev_submit_bio(blkdev, bio);
    __buf_bio_cleanup(bio);
    int ret = blkdev_put(blkdev);
    assert(ret == 0, "bwrite: blkdev_put failed: %d", ret);
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
        list_node_push(&bcache.lru_entry, b, lru_entry);
    }

    spin_unlock(&bcache.lock);
}

void bpin(struct buf *b) {
    spin_lock(&bcache.lock);
    b->refcnt++;
    spin_unlock(&bcache.lock);
}

void bunpin(struct buf *b) {
    spin_lock(&bcache.lock);
    b->refcnt--;
    spin_unlock(&bcache.lock);
}
