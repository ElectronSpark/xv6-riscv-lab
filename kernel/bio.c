// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "mutex_types.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"
#include "page.h"
#include "blkdev.h"
#include "list.h"
#include "hlist.h"

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  list_node_t lru_entry; // For debugging, not used in the code.
  hlist_t cached; // Hash list of buffers, sorted by (dev, blockno).
  hlist_bucket_t buckets[BIO_HASH_BUCKETS]; // stores the hash buckets of the hash list
} bcache;

static ht_hash_t __bcache_hash_func(void *node)  {
  struct buf *bnode = node;
  return hlist_hash_uint64(bnode->blockno + (bnode->dev << 16));
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
  int value1 = (int)(bnode1->blockno + (bnode1->dev << 16));
  int value2 = (int)(bnode2->blockno + (bnode2->dev << 16));

  return value1 - value2;
}

static inline struct buf*
__bcache_hlist_get(uint dev, uint blockno) {
  // Create a dummy node to search for
  struct buf dummy = { 0 };
  dummy.dev = dev;
  dummy.blockno = blockno;

  return hlist_get(&bcache.cached, &dummy);
}

static inline struct buf*
__bcache_hlist_pop(uint dev, uint blockno) {
  // Create a dummy node to search for
  struct buf dummy = { 0 };
  dummy.dev = dev;
  dummy.blockno = blockno;

  struct buf *buf = hlist_pop(&bcache.cached, &dummy);
  return buf;
}

static inline int
__bcache_hlist_push(struct buf *buf) {
  struct buf *entry = hlist_put(&bcache.cached, buf);
  if (entry == NULL) {
    return 0; // succeeded
  } else if (entry != buf) {
    return -1; // failed to insert
  } else {
    return -1; // the entry is already in the hash list
  }
}

void
binit(void)
{
  struct buf *b;

  spin_init(&bcache.lock, "bcache");

  // Create linked list of buffers
  list_entry_init(&bcache.lru_entry);
  hlist_func_t hlist_func = {
    .hash = __bcache_hash_func,
    .get_node = __bcache_hlist_get_node,
    .get_entry = __bcache_hlist_get_entry,
    .cmp_node = __bcache_hlist_cmp
  };
  hlist_init(&bcache.cached, BIO_HASH_BUCKETS, &hlist_func);
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    list_entry_init(&b->lru_entry);
    mutex_init(&b->lock, "buffer");
    list_entry_push(&bcache.lru_entry, &b->lru_entry);
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
STATIC struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b, *b1, *tmp;

  spin_acquire(&bcache.lock);

  // Is the block already cached?
  b = __bcache_hlist_get(dev, blockno);
  if(b != NULL) {
    // Found it.
    if (!LIST_NODE_IS_DETACHED(b, lru_entry)) {
      list_node_detach(b, lru_entry);
    }
    b->refcnt++;
    spin_release(&bcache.lock);
    assert(mutex_lock(&b->lock) == 0, "bget: failed to lock buffer");
    return b;
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  list_foreach_node_safe(&bcache.lru_entry, b, tmp, lru_entry) {
    if(b->refcnt == 0) {
      // Found an unused buffer.
      b1 = __bcache_hlist_pop(b->dev, b->blockno);
      if (b1 && b1 != b) {
        if (b->blockno != 0 || b->dev != 0) {
          // Only unused buffers could clash, otherwise it is a bug.
          printf("bget: found a buffer with blockno %d, dev %d, but it is not the same as the one we are recycling\n", b1->blockno, b1->dev);
          panic("bget: found a buffer that is not the same as the one we are recycling");
        }
        // the buffer b is unused, so we can put back b1 and safely use b
        if (__bcache_hlist_push(b1) != 0) {
          panic("bget: failed to push cached buffer into hash list");
        }
      }
      list_node_detach(b, lru_entry);
      __atomic_thread_fence(__ATOMIC_SEQ_CST); // Ensure the buffer is detached before using it
      // list_node_push(&bcache.lru_entry, b, lru_entry);
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      if (__bcache_hlist_push(b) != 0) {
        printf("dev: %d, blockno: %d\n", dev, blockno);
        panic("bget: failed to push recycled buffer into hash list");
      }
      spin_release(&bcache.lock);
      assert(mutex_lock(&b->lock) == 0, "bget: failed to lock buffer");
      return b;
    }
  }
  panic("bget: no buffers");
}

static struct bio *__buf_alloc_bio(struct buf *b, blkdev_t *blkdev, bool write) {
  struct bio *bio = NULL;
  int ret = bio_alloc(blkdev, 1, write, NULL, NULL, &bio);
  if (bio == NULL) {
    return NULL;
  }
  bio->blkno = b->blockno * (BSIZE / 512);
  page_t *page = __pa_to_page((uint64)b->data & ~PAGE_MASK);
  size_t page_offset = (uint64)b->data & PAGE_MASK;
  ret = bio_add_seg(bio, page, 0, BSIZE, page_offset);
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
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    blkdev_t *blkdev = NULL;
    int ret = blkdev_get(major(b->dev), minor(b->dev), &blkdev);
    assert(ret == 0, "bwrite: blkdev_get failed: %d", ret);
    struct bio *bio = __buf_alloc_bio(b, blkdev, false);
    assert(bio != NULL, "bread: bio_alloc failed");
    blkdev_submit_bio(blkdev, bio);
    b->valid = 1;
    __buf_bio_cleanup(bio);
    ret = blkdev_put(blkdev);
    assert(ret == 0, "bread: blkdev_put failed: %d", ret);
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holding_mutex(&b->lock))
    panic("bwrite");
  blkdev_t *blkdev = NULL;
  int ret = blkdev_get(major(b->dev), minor(b->dev), &blkdev);
  assert(ret == 0, "bwrite: blkdev_get failed: %d", ret);
  struct bio *bio = __buf_alloc_bio(b, blkdev, true);
  assert(bio != NULL, "bwrite: bio_alloc failed");
  blkdev_submit_bio(blkdev, bio);
  __buf_bio_cleanup(bio);
  ret = blkdev_put(blkdev);
  assert(ret == 0, "bwrite: blkdev_put failed: %d", ret);
}

// release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holding_mutex(&b->lock))
    panic("brelse");

  mutex_unlock(&b->lock);

  spin_acquire(&bcache.lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    list_node_push(&bcache.lru_entry, b, lru_entry);
  }
  
  spin_release(&bcache.lock);
}

void
bpin(struct buf *b) {
  spin_acquire(&bcache.lock);
  b->refcnt++;
  spin_release(&bcache.lock);
}

void
bunpin(struct buf *b) {
  spin_acquire(&bcache.lock);
  b->refcnt--;
  spin_release(&bcache.lock);
}


