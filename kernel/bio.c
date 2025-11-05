#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"
#include "page.h"
#include "pcache.h"
#include "blkdev.h"
#include "bio.h"
#include "errno.h"
#include "timer.h"

#define BLKS_PER_BSIZE      (BSIZE / BLK_SIZE)
#define BLKS_PER_PAGE       (PGSIZE / BLK_SIZE)
#define FS_BLOCKS_PER_PAGE  (PGSIZE / BSIZE)

struct block_cache {
	struct pcache pcache;
	blkdev_t *blkdev;
};

struct block_cache_entry {
	uint dev;
	struct block_cache cache;
	int initialized;
};

static int block_cache_init_entry(struct block_cache_entry *entry, uint dev);
static int block_cache_read_page(struct pcache *pcache, page_t *page);
static int block_cache_write_page(struct pcache *pcache, page_t *page);
static void block_cache_mark_dirty(struct pcache *pcache, page_t *page);

static struct pcache_ops block_cache_ops = {
	.read_page = block_cache_read_page,
	.write_page = block_cache_write_page,
	.write_begin = NULL,
	.write_end = NULL,
	.mark_dirty = block_cache_mark_dirty,
};

static struct block_cache_entry root_entry = {
	.dev = (uint)-1,
	.initialized = 0,
};

static inline uint64
fs_block_to_blkno(uint blockno)
{
	return (uint64)blockno * BLKS_PER_BSIZE;
}

static inline size_t
block_offset_bytes(page_t *page, uint blockno)
{
	if (page == NULL) {
		return 0;
	}
	struct pcache_node *pcnode = page->pcache.pcache_node;
	assert(pcnode != NULL, "block_offset_bytes: page missing pcache node");
	uint64 base_blk = pcnode->blkno;
	uint64 target_blk = fs_block_to_blkno(blockno);
	assert(target_blk >= base_blk, "block_offset_bytes: block outside page (before)");
	assert(target_blk < base_blk + BLKS_PER_PAGE, "block_offset_bytes: block outside page (after)");
	return (size_t)((target_blk - base_blk) << BLK_SIZE_SHIFT);
}

static inline void *
page_block_ptr(page_t *page, uint blockno)
{
	size_t offset = block_offset_bytes(page, blockno);
	uint64 pa = __page_to_pa(page);
	return (void *)(pa + offset);
}

static struct block_cache_entry *
block_cache_for_dev(uint dev)
{
	if (root_entry.initialized) {
		if (root_entry.dev == dev) {
			return &root_entry;
		}
		panic("block_cache_for_dev: unsupported device %u", dev);
	}

	if (block_cache_init_entry(&root_entry, dev) != 0) {
		panic("block_cache_for_dev: failed to initialise cache for dev %u", dev);
	}
	return &root_entry;
}

static int
block_cache_init_entry(struct block_cache_entry *entry, uint dev)
{
	if (entry->initialized) {
		return 0;
	}

	memset(entry, 0, sizeof(*entry));
	entry->dev = dev;

	struct block_cache *cache = &entry->cache;
	memset(cache, 0, sizeof(*cache));

	cache->pcache.ops = &block_cache_ops;
	cache->pcache.blk_count = (uint64)FSSIZE * BLKS_PER_BSIZE;
	// cache->pcache.max_pages = NBUF; // keep residency comparable to legacy buffer cache
  cache->pcache.max_pages = 0; // Use default max pages
	cache->pcache.private_data = entry;

	blkdev_t *blkdev = NULL;
	int ret = blkdev_get(major(dev), minor(dev), &blkdev);
	if (ret != 0) {
		return ret;
	}

	cache->blkdev = blkdev;
	ret = pcache_init(&cache->pcache);
	if (ret != 0) {
		blkdev_put(blkdev);
		cache->blkdev = NULL;
		return ret;
	}
	cache->pcache.private_data = entry;

	entry->initialized = 1;
	return 0;
}

static int
block_cache_submit_block(blkdev_t *blkdev, page_t *page, uint64 blkno, size_t offset, bool write)
{
	struct bio *bio = NULL;
	int ret = bio_alloc(blkdev, 1, write, NULL, NULL, &bio);
	if (bio == NULL) {
		return ret == 0 ? -ENOMEM : ret;
	}

	bio->blkno = blkno;
	ret = bio_add_seg(bio, page, 0, BSIZE, offset);
	if (ret != 0) {
		bio_release(bio);
		return ret;
	}

	ret = blkdev_submit_bio(blkdev, bio);
	bio_release(bio);
	return ret;
}

static int
block_cache_read_page(struct pcache *pcache, page_t *page)
{
	struct block_cache_entry *entry = (struct block_cache_entry *)pcache->private_data;
	assert(entry != NULL && entry->initialized, "block_cache_read_page: invalid cache entry");

	blkdev_t *blkdev = entry->cache.blkdev;
	assert(blkdev != NULL, "block_cache_read_page: blkdev not available");

	struct pcache_node *pcnode = page->pcache.pcache_node;
	assert(pcnode != NULL, "block_cache_read_page: page missing pcache node");

	uint64 base_blk = pcnode->blkno;
	for (size_t i = 0; i < FS_BLOCKS_PER_PAGE; i++) {
		uint64 blkno = base_blk + (i * BLKS_PER_BSIZE);
		size_t offset = i * BSIZE;
		int ret = block_cache_submit_block(blkdev, page, blkno, offset, false);
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

static int
block_cache_write_page(struct pcache *pcache, page_t *page)
{
	struct block_cache_entry *entry = (struct block_cache_entry *)pcache->private_data;
	assert(entry != NULL && entry->initialized, "block_cache_write_page: invalid cache entry");

	blkdev_t *blkdev = entry->cache.blkdev;
	assert(blkdev != NULL, "block_cache_write_page: blkdev not available");

	struct pcache_node *pcnode = page->pcache.pcache_node;
	assert(pcnode != NULL, "block_cache_write_page: page missing pcache node");

	uint64 base_blk = pcnode->blkno;
	for (size_t i = 0; i < FS_BLOCKS_PER_PAGE; i++) {
		uint64 blkno = base_blk + (i * BLKS_PER_BSIZE);
		size_t offset = i * BSIZE;
		int ret = block_cache_submit_block(blkdev, page, blkno, offset, true);
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

static void
block_cache_mark_dirty(struct pcache *pcache, page_t *page)
{
	struct block_cache_entry *entry = (struct block_cache_entry *)pcache->private_data;
	if (entry == NULL || !entry->initialized || page == NULL) {
		return;
	}
	entry->cache.pcache.last_request = get_jiffs();
}

void
binit(void)
{
	block_cache_init_entry(&root_entry, ROOTDEV);
}

page_t *
bread(uint dev, uint blockno, void **data_out)
{
	struct block_cache_entry *entry = block_cache_for_dev(dev);
	assert(entry != NULL && entry->initialized, "bread: cache not initialised");

	struct pcache *pcache = &entry->cache.pcache;
	uint64 blkno = fs_block_to_blkno(blockno);

	page_t *page = pcache_get_page(pcache, blkno);
	assert(page != NULL, "bread: pcache_get_page failed");

	int ret = pcache_read_page(pcache, page);
	assert(ret == 0, "bread: pcache_read_page failed: %d", ret);

	if (data_out != NULL) {
		*data_out = page_block_ptr(page, blockno);
	}

	return page;
}

int
bwrite(uint dev, uint blockno, page_t *page)
{
	if (page == NULL) {
		return -EINVAL;
	}

	struct block_cache_entry *entry = block_cache_for_dev(dev);
	assert(entry != NULL && entry->initialized, "bwrite: cache not initialised");

	uint64 blkno = fs_block_to_blkno(blockno);
	size_t offset = block_offset_bytes(page, blockno);
	return block_cache_submit_block(entry->cache.blkdev, page, blkno, offset, true);
}

void
brelse(page_t *page)
{
	if (page == NULL) {
		return;
	}
	struct pcache *pcache = page->pcache.pcache;
	assert(pcache != NULL, "brelse: page without pcache");
	pcache_put_page(pcache, page);
}

void
bpin(page_t *page)
{
	if (page == NULL) {
		return;
	}
  page_lock_acquire(page);
	int ret = page_ref_inc_unlocked(page);
	assert(ret >= 0, "bpin: failed to increment refcount");
  page_lock_release(page);
}

void
bunpin(page_t *page)
{
	if (page == NULL) {
		return;
	}
  page_lock_acquire(page);
	int ret = page_ref_dec_unlocked(page);
	assert(ret >= 1, "bunpin: refcount underflow");
  page_lock_release(page);
}

void
bmark_dirty(page_t *page)
{
	if (page == NULL) {
		return;
	}
	struct pcache *pcache = page->pcache.pcache;
	assert(pcache != NULL, "bmark_dirty: page without pcache");
	int ret = pcache_mark_page_dirty(pcache, page);
	assert(ret == 0 || ret == -EBUSY, "bmark_dirty: failed to mark page dirty: %d", ret);
}

void *
block_data(page_t *page, uint blockno)
{
	if (page == NULL) {
		return NULL;
	}
	return page_block_ptr(page, blockno);
}

