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
#include "mutex_types.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"
#include "pcache.h"
#include "blkdev.h"
#include "bio.h"
#include "page.h"
#include "errno.h"

#if (BSIZE % BLK_SIZE) != 0
#error "BSIZE must be a multiple of BLK_SIZE"
#endif

#define BIO_BLKS_PER_BUF (BSIZE / BLK_SIZE)

#if BIO_BLKS_PER_BUF == 0
#error "BIO_BLKS_PER_BUF must be greater than zero"
#endif

#if (BIO_BLKS_PER_PAGE % BIO_BLKS_PER_BUF) != 0
#error "Page cache page must contain an integer number of filesystem blocks"
#endif

#define BIO_BUFS_PER_PAGE (BIO_BLKS_PER_PAGE / BIO_BLKS_PER_BUF)

struct bcache_backend {
  struct pcache cache;
  blkdev_t *blkdev;
};

static struct bcache_backend bcache;

static inline uint64
__buf_to_cache_blk(uint blockno)
{
  return (uint64)blockno * BIO_BLKS_PER_BUF;
}

static struct buf *
__buf_alloc(uint dev, uint blockno)
{
  struct buf *b = kmm_alloc(sizeof(*b));
  assert(b != NULL, "bread: allocation failed");
  memset(b, 0, sizeof(*b));
  mutex_init(&b->lock, "buffer");
  b->dev = dev;
  b->blockno = blockno;
  b->refcnt = 1;
  return b;
}

static void
__buf_free(struct buf *b)
{
  if (b == NULL)
    return;
  kmm_free(b);
}

static inline void
__buf_set_page(struct buf *b, page_t *page)
{
  b->page = page;
}

static inline page_t *
__buf_get_page(struct buf *b)
{
  return b->page;
}

static void
__buf_page_pin(page_t *page)
{
  if (page == NULL)
    panic("bpin: buffer without page");

  page_lock_acquire(page);
  int ref = page_ref_inc_unlocked(page);
  page_lock_release(page);

  if (ref < 0)
    panic("bpin: page ref++ failed");
}

static void
__buf_page_unpin(page_t *page)
{
  if (page == NULL)
    return;

  page_lock_acquire(page);
  int ref = page_ref_dec_unlocked(page);
  page_lock_release(page);

  if (ref < 0)
    panic("bunpin: page ref-- underflow");
}

static page_t *
__buf_ref_put(struct buf *b, bool *should_free)
{
  int old = __atomic_fetch_sub(&b->refcnt, 1, __ATOMIC_ACQ_REL);
  if (old <= 0)
    panic("brelse: refcnt underflow");
  if (old == 1) {
    if (should_free)
      *should_free = true;
    page_t *page = b->page;
    b->page = NULL;
    b->data = NULL;
    b->valid = 0;
    return page;
  }
  if (should_free)
    *should_free = false;
  return NULL;
}

static int
__bio_submit_page(struct pcache *pcache, page_t *page, bool write)
{
  struct bcache_backend *backend = pcache->private_data;
  if (backend == NULL || backend->blkdev == NULL)
    return -ENODEV;

  if (page->pcache.size == 0)
    return -EINVAL;

  uint64 base_blk = page->pcache.blkno;
  if (base_blk >= pcache->blk_count)
    return -EINVAL;

  size_t max_bytes = page->pcache.size;
  uint64 remaining_units = pcache->blk_count - base_blk;
  uint64 remaining_bytes = remaining_units << BLK_SIZE_SHIFT;
  if (remaining_bytes < max_bytes)
    max_bytes = (size_t)remaining_bytes;

  if (max_bytes < BSIZE)
    return -EINVAL;

  size_t seg_count = max_bytes / BSIZE;
  if (seg_count > BIO_BUFS_PER_PAGE)
    seg_count = BIO_BUFS_PER_PAGE;
  max_bytes = seg_count * BSIZE;

  if (seg_count == 0)
    return -EINVAL;

  struct bio *bio = NULL;
  int ret = bio_alloc(backend->blkdev, (int16)seg_count, write, NULL, NULL, &bio);
  if (ret != 0)
    return ret;

  bio->blkno = base_blk;

  for (size_t i = 0; i < seg_count; i++) {
    size_t offset = i * BSIZE;
    size_t len = BSIZE;
    if (offset + len > max_bytes)
      len = max_bytes - offset;
    ret = bio_add_seg(bio, page, (int16)i, (uint16)len, (uint16)offset);
    if (ret != 0)
      goto out_release;
  }

  ret = blkdev_submit_bio(backend->blkdev, bio);

out_release:
  bio_release(bio);
  return ret;
}

static int
__bio_read_page(struct pcache *pcache, page_t *page)
{
  return __bio_submit_page(pcache, page, false);
}

static int
__bio_write_page(struct pcache *pcache, page_t *page)
{
  return __bio_submit_page(pcache, page, true);
}

static int
__bio_write_begin(struct pcache *pcache)
{
  (void)pcache;
  return 0;
}

static int
__bio_write_end(struct pcache *pcache)
{
  (void)pcache;
  return 0;
}

static void
__bio_invalidate_page(struct pcache *pcache, page_t *page)
{
  (void)pcache;
  (void)page;
}

static void
__bio_mark_dirty(struct pcache *pcache, page_t *page)
{
  (void)pcache;
  (void)page;
}

static void
__bio_abort_io(struct pcache *pcache, page_t *page)
{
  (void)pcache;
  (void)page;
}

static struct pcache_ops __bio_pcache_ops = {
  .read_page       = __bio_read_page,
  .write_page      = __bio_write_page,
  .write_begin     = __bio_write_begin,
  .write_end       = __bio_write_end,
  .invalidate_page = __bio_invalidate_page,
  .mark_dirty      = __bio_mark_dirty,
  .abort_io        = __bio_abort_io,
};

static void
__buf_prepare(struct buf *b, page_t *page, uint64 cache_blk)
{
  uint64 base_blk = page->pcache.blkno;
  if (cache_blk < base_blk)
    panic("buffer cache_blk < base_blk");

  uint64 delta_units = cache_blk - base_blk;
  if (delta_units + BIO_BLKS_PER_BUF > BIO_BLKS_PER_PAGE)
    panic("buffer offset out of range");

  uchar *base = (uchar *)(uint64)__page_to_pa(page);
  b->data = base + delta_units * BLK_SIZE;
  b->valid = (page->flags & PAGE_FLAG_UPTODATE) != 0;
  b->disk = 0;
  __buf_set_page(b, page);
}

void
binit(void)
{
  memset(&bcache, 0, sizeof(bcache));

  int ret = 0;

  ret = blkdev_get(major(ROOTDEV), minor(ROOTDEV), &bcache.blkdev);
  assert(ret == 0, "binit: blkdev_get failed: %d", ret);

  bcache.cache.ops = &__bio_pcache_ops;
  bcache.cache.blk_count = (uint64)FSSIZE * BIO_BLKS_PER_BUF;
  bcache.cache.max_pages = NBUF;
  ret = pcache_init(&bcache.cache);
  assert(ret == 0, "binit: pcache_init failed: %d", ret);
  bcache.cache.private_data = &bcache;
}

struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b = __buf_alloc(dev, blockno);
  assert(mutex_lock(&b->lock) == 0, "bread: failed to lock buffer");

  uint64 cache_blk = __buf_to_cache_blk(blockno);
  page_t *page = pcache_get_page(&bcache.cache, cache_blk);
  assert(page != NULL, "bread: pcache_get_page failed");
  __buf_prepare(b, page, cache_blk);

  if (!b->valid) {
    int ret = pcache_read_page(&bcache.cache, page);
    if (ret != 0) {
      mutex_unlock(&b->lock);
      pcache_put_page(&bcache.cache, page);
      panic("bread: read failed: %d", ret);
    }
    b->valid = 1;
  }

  return b;
}

void
bwrite(struct buf *b)
{
  if (!holding_mutex(&b->lock))
    panic("bwrite");

  page_t *page = __buf_get_page(b);
  if (page == NULL)
    panic("bwrite: buffer without pcache page");

  int ret = pcache_mark_page_dirty(&bcache.cache, page);
  if (ret != 0)
    panic("bwrite: mark dirty failed: %d", ret);

  ret = pcache_flush(&bcache.cache);
  if (ret != 0)
    panic("bwrite: flush failed: %d", ret);
}

void
brelse(struct buf *b)
{
  if (!holding_mutex(&b->lock))
    panic("brelse");

  mutex_unlock(&b->lock);
  bool should_free = false;
  page_t *page = __buf_ref_put(b, &should_free);
  if (page != NULL)
    pcache_put_page(&bcache.cache, page);
  if (should_free)
    __buf_free(b);
}

void
bpin(struct buf *b)
{
  page_t *page = __buf_get_page(b);
  __buf_page_pin(page);
}

void
bunpin(struct buf *b)
{
  page_t *page = __buf_get_page(b);
  __buf_page_unpin(page);
}


