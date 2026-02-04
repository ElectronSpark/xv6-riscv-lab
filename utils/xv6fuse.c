#define _GNU_SOURCE
#define FUSE_USE_VERSION 31

#include <errno.h>
#include <fcntl.h>
#include <fuse3/fuse.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__has_include)
# if __has_include(<sys/mman.h>)
#  include <sys/mman.h>
# endif
# if __has_include(<sys/statvfs.h>)
#  include <sys/statvfs.h>
# endif
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define XV6_SYMLINK_MAX_DEPTH 10

#ifndef MAP_SHARED
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define MS_SYNC 0x04
#define MS_ASYNC 0x08
#define MAP_FAILED ((void *)-1)
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int msync(void *addr, size_t length, int flags);
int munmap(void *addr, size_t length);
#endif

#define ON_HOST_OS
#define dirent xv6_dirent
#include "../kernel/inc/types.h"
#include "../kernel/inc/vfs/xv6fs/ondisk.h"
#undef dirent

#include "rbtree.h"

struct xv6fs_context {
  int fd;
  size_t image_size;
  void *image;
  struct superblock sb;
  char *image_path;
  bool readonly;
  bool sync_writes;  /* if false, use async sync for better performance */
};

static struct xv6fs_context g_fs = {
  .fd = -1,
  .image = NULL,
  .image_size = 0,
  .image_path = NULL,
  .readonly = false,
  .sync_writes = false,  /* async by default for performance */
};

/* Forward declarations for functions used by block cache */
static int bitmap_get(uint32_t blockno, bool *is_set);
static int free_block(uint32_t blockno);

/******************************************************************************
 * Free Extent Cache using the rb-tree
 ******************************************************************************/

struct free_extent {
  struct rb_node rb_node;
  uint32_t start;
  uint32_t length;
};

struct block_cache {
  struct rb_root tree;
  struct rb_root_opts tree_opts;
  uint32_t nblocks;
  uint32_t data_start;
  uint32_t alloc_cursor;
  uint32_t free_count;
  uint32_t extent_count;
  bool initialized;
};

static struct block_cache g_bcache = {
  .initialized = false,
};

/* Key comparison function for extents */
static int extent_keys_cmp(uint64_t k1, uint64_t k2) {
  if (k1 < k2) return -1;
  if (k1 > k2) return 1;
  return 0;
}

/* Get key (start block) from extent node */
static uint64_t extent_get_key(struct rb_node *node) {
  struct free_extent *ext = (struct free_extent *)
    ((char *)node - offsetof(struct free_extent, rb_node));
  return ext->start;
}

/* Get extent from rb_node */
static inline struct free_extent *rb_entry_extent(struct rb_node *node) {
  if (!node) return NULL;
  return (struct free_extent *)((char *)node - offsetof(struct free_extent, rb_node));
}

/* Allocate a new extent node */
static struct free_extent *extent_alloc(void) {
  struct free_extent *ext = calloc(1, sizeof(*ext));
  if (ext) rb_node_init(&ext->rb_node);
  return ext;
}

/* Free an extent node */
static void extent_free(struct free_extent *ext) {
  free(ext);
}

/* Find extent with start <= blockno (floor search) */
static struct free_extent *
bcache_find_extent_le(struct block_cache *bc, uint32_t blockno)
{
  return rb_entry_extent(rb_find_key_rdown(&bc->tree, blockno));
}

/* Find extent with start >= blockno (ceiling search) */
static struct free_extent *
bcache_find_extent_ge(struct block_cache *bc, uint32_t blockno)
{
  return rb_entry_extent(rb_find_key_rup(&bc->tree, blockno));
}

/* Find extent containing blockno */
static struct free_extent *
bcache_find_extent_containing(struct block_cache *bc, uint32_t blockno)
{
  struct free_extent *ext = bcache_find_extent_le(bc, blockno);
  if (ext && blockno < ext->start + ext->length) {
    return ext;
  }
  return NULL;
}

/* Insert an extent, merging with adjacent extents */
static void
bcache_insert_extent(struct block_cache *bc, uint32_t start, uint32_t length)
{
  struct free_extent *prev, *next;
  uint32_t end = start + length;

  /* Check for merge with previous extent */
  prev = bcache_find_extent_le(bc, start);
  if (prev && prev->start + prev->length == start) {
    /* Merge with previous: extend it */
    prev->length += length;
    bc->free_count += length;

    /* Check if we can also merge with next */
    struct rb_node *next_node = rb_next_node(&prev->rb_node);
    if (next_node) {
      next = rb_entry_extent(next_node);
      if (prev->start + prev->length == next->start) {
        /* Merge all three into prev */
        prev->length += next->length;
        rb_delete_node_color(&bc->tree, &next->rb_node);
        extent_free(next);
        bc->extent_count--;
      }
    }
    return;
  }

  /* Check for merge with next extent */
  next = bcache_find_extent_ge(bc, start);
  if (next && end == next->start) {
    /* Merge with next: move its start back */
    /* Need to remove and re-insert since key changes */
    rb_delete_node_color(&bc->tree, &next->rb_node);
    next->start = start;
    next->length += length;
    rb_node_init(&next->rb_node);
    rb_insert_color(&bc->tree, &next->rb_node);
    bc->free_count += length;
    return;
  }

  /* No merge possible, create new extent */
  struct free_extent *ext = extent_alloc();
  if (!ext) return;

  ext->start = start;
  ext->length = length;
  rb_insert_color(&bc->tree, &ext->rb_node);
  bc->extent_count++;
  bc->free_count += length;
}

/* Allocate one block from an extent (from the END for O(1) operation) */
static uint32_t
bcache_alloc_from_extent(struct block_cache *bc, struct free_extent *ext)
{
  /* Allocate from the end - no re-keying needed */
  uint32_t blockno = ext->start + ext->length - 1;

  if (ext->length == 1) {
    rb_delete_node_color(&bc->tree, &ext->rb_node);
    extent_free(ext);
    bc->extent_count--;
  } else {
    ext->length--;
  }

  bc->free_count--;
  return blockno;
}

/* Mark a block as free in the cache */
static void
bcache_mark_free(struct block_cache *bc, uint32_t blockno)
{
  if (!bc->initialized || blockno < bc->data_start) {
    return;
  }
  bcache_insert_extent(bc, blockno, 1);
}

/* Find a free block with wear leveling */
static int
bcache_find_free_block(struct block_cache *bc, uint32_t *blockno_out)
{
  if (!bc->initialized || bc->free_count == 0 || !bc->tree.node) {
    return -ENOSPC;
  }

  /* Find extent at or after cursor for wear leveling */
  struct free_extent *ext = bcache_find_extent_ge(bc, bc->alloc_cursor);

  /* Wrap around if no extent found after cursor */
  if (!ext) {
    ext = rb_entry_extent(rb_first_node(&bc->tree));
  }

  *blockno_out = bcache_alloc_from_extent(bc, ext);
  bc->alloc_cursor = *blockno_out + 1;
  if (bc->alloc_cursor >= bc->data_start + bc->nblocks) {
    bc->alloc_cursor = bc->data_start;
  }

  return 0;
}

/* Find a free block near a hint for locality */
static int
bcache_find_free_block_near(struct block_cache *bc, uint32_t hint, uint32_t *blockno_out)
{
  if (!bc->initialized || bc->free_count == 0 || !bc->tree.node) {
    return -ENOSPC;
  }

  /* Clamp hint to valid range */
  if (hint < bc->data_start) {
    hint = bc->data_start;
  } else if (hint >= bc->data_start + bc->nblocks) {
    hint = bc->data_start + bc->nblocks - 1;
  }

  /* Try to find extent containing the hint */
  struct free_extent *ext = bcache_find_extent_containing(bc, hint);
  if (ext) {
    *blockno_out = bcache_alloc_from_extent(bc, ext);
    return 0;
  }

  /* Find extent at or after hint */
  ext = bcache_find_extent_ge(bc, hint);
  if (ext) {
    *blockno_out = bcache_alloc_from_extent(bc, ext);
    return 0;
  }

  /* All extents are before hint - use the last one */
  ext = rb_entry_extent(rb_last_node(&bc->tree));
  *blockno_out = bcache_alloc_from_extent(bc, ext);
  return 0;
}

/* Initialize the block cache from on-disk bitmap */
static int
bcache_init(struct block_cache *bc, uint32_t nblocks, uint32_t data_start)
{
  if (bc->initialized) {
    return 0;
  }

  bc->tree_opts.keys_cmp_fun = extent_keys_cmp;
  bc->tree_opts.get_key_fun = extent_get_key;
  rb_root_init(&bc->tree, &bc->tree_opts);

  bc->nblocks = nblocks;
  bc->data_start = data_start;
  bc->alloc_cursor = data_start;
  bc->free_count = 0;
  bc->extent_count = 0;

  /* Scan bitmap and build extent tree */
  uint32_t run_start = 0;
  uint32_t run_length = 0;
  int in_run = 0;

  for (uint32_t b = 0; b < nblocks; b++) {
    uint32_t blockno = data_start + b;
    bool used = true;
    bitmap_get(blockno, &used);

    if (!used) {
      if (in_run) {
        run_length++;
      } else {
        run_start = blockno;
        run_length = 1;
        in_run = 1;
      }
    } else {
      if (in_run) {
        bcache_insert_extent(bc, run_start, run_length);
        in_run = 0;
      }
    }
  }

  /* Flush remaining run */
  if (in_run) {
    bcache_insert_extent(bc, run_start, run_length);
  }

  bc->initialized = true;
  fprintf(stderr, "[xv6fs] block cache initialized: %u data blocks, %u free in %u extents\n",
          nblocks, bc->free_count, bc->extent_count);

  return 0;
}

/* Free the block cache */
static void
bcache_destroy(struct block_cache *bc)
{
  /* Free all extent nodes */
  struct rb_node *node = rb_first_node(&bc->tree);
  while (node) {
    struct rb_node *next = rb_next_node(node);
    struct free_extent *ext = rb_entry_extent(node);
    extent_free(ext);
    node = next;
  }
  bc->tree.node = NULL;
  bc->initialized = false;
  bc->free_count = 0;
  bc->extent_count = 0;
}

STATIC_INLINE uint16_t
from_le16(uint16_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  return value;
#else
  return __builtin_bswap16(value);
#endif
}

STATIC_INLINE uint32_t
from_le32(uint32_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  return value;
#else
  return __builtin_bswap32(value);
#endif
}

STATIC_INLINE uint16_t
to_le16(uint16_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  return value;
#else
  return __builtin_bswap16(value);
#endif
}

STATIC_INLINE uint32_t
to_le32(uint32_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  return value;
#else
  return __builtin_bswap32(value);
#endif
}

static inline void *
block_ptr(uint32_t blockno)
{
  size_t offset = (size_t)blockno * BSIZE;
  if (blockno >= g_fs.sb.size || offset + BSIZE > g_fs.image_size) {
    return NULL;
  }
  return (char *)g_fs.image + offset;
}

static size_t
page_size_cached(void)
{
  static size_t cached = 0;
  if (cached == 0) {
    long ps = sysconf(_SC_PAGESIZE);
    cached = (ps > 0) ? (size_t)ps : 4096u;
  }
  return cached;
}

static int
sync_range(void *addr, size_t length)
{
  if (!addr || length == 0 || g_fs.image == NULL) {
    return 0;
  }

  size_t ps = page_size_cached();
  uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(ps - 1);
  uintptr_t end = ((uintptr_t)addr + length + ps - 1) & ~(uintptr_t)(ps - 1);
  int flags = g_fs.sync_writes ? MS_SYNC : MS_ASYNC;
  if (msync((void *)start, end - start, flags) != 0) {
    return -errno;
  }
  return 0;
}

/* Force synchronous sync - used at critical points like fsync/flush */
static int
sync_range_sync(void *addr, size_t length)
{
  if (!addr || length == 0 || g_fs.image == NULL) {
    return 0;
  }

  size_t ps = page_size_cached();
  uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(ps - 1);
  uintptr_t end = ((uintptr_t)addr + length + ps - 1) & ~(uintptr_t)(ps - 1);
  if (msync((void *)start, end - start, MS_SYNC) != 0) {
    return -errno;
  }
  return 0;
}

static int
sync_block(void *addr)
{
  return sync_range(addr, BSIZE);
}

static inline uint32_t
bitmap_block_count(void)
{
  return (g_fs.sb.nblocks + BPB - 1) / BPB;
}

static inline uint32_t
data_start_block(void)
{
  return g_fs.sb.bmapstart + bitmap_block_count();
}

static bool
buffer_is_zero(const void *buf, size_t len)
{
  const uint8_t *p = (const uint8_t *)buf;
  for (size_t i = 0; i < len; ++i) {
    if (p[i] != 0) {
      return false;
    }
  }
  return true;
}

static int
bitmap_get(uint32_t blockno, bool *is_set)
{
  if (!is_set) {
    return -EINVAL;
  }
  uint32_t bblock = BBLOCK(blockno, g_fs.sb);
  uint8_t *bits = block_ptr(bblock);
  if (!bits) {
    return -EIO;
  }
  uint32_t bi = blockno % BPB;
  uint32_t byte_index = bi / 8;
  uint8_t mask = (uint8_t)(1u << (bi & 7));
  *is_set = (bits[byte_index] & mask) != 0;
  return 0;
}

static int
bitmap_update(uint32_t blockno, bool set)
{
  uint32_t bblock = BBLOCK(blockno, g_fs.sb);
  uint8_t *bits = block_ptr(bblock);
  if (!bits) {
    return -EIO;
  }
  uint32_t bi = blockno % BPB;
  uint32_t byte_index = bi / 8;
  uint8_t mask = (uint8_t)(1u << (bi & 7));
  if (set) {
    bits[byte_index] |= mask;
  } else {
    bits[byte_index] &= (uint8_t)~mask;
  }
  return sync_range(bits + byte_index, 1);
}

static int
alloc_block(uint32_t *blockno_out)
{
  /* Try to use the block cache if initialized */
  if (g_bcache.initialized) {
    uint32_t candidate;
    int rc = bcache_find_free_block(&g_bcache, &candidate);
    if (rc != 0) {
      return rc;
    }

    /* Verify and update on-disk bitmap */
    bool used = false;
    rc = bitmap_get(candidate, &used);
    if (rc != 0) {
      return rc;
    }
    if (used) {
      /* Cache inconsistency - block already allocated, retry */
      /* Note: bcache_find_free_block already removed it from cache */
      return alloc_block(blockno_out);
    }

    rc = bitmap_update(candidate, true);
    if (rc != 0) {
      /* Bitmap update failed - add block back to cache and retry */
      bcache_mark_free(&g_bcache, candidate);
      return rc;
    }

    /* Block is now allocated (already removed from cache by find_free_block) */

    void *block = block_ptr(candidate);
    if (!block) {
      return -EIO;
    }
    memset(block, 0, BSIZE);
    rc = sync_block(block);
    if (rc != 0) {
      return rc;
    }

    if (blockno_out) {
      *blockno_out = candidate;
    }
    return 0;
  }

  /* Fallback to linear search if cache not available */
  uint32_t start = data_start_block();
  for (uint32_t offset = 0; offset < g_fs.sb.nblocks; ++offset) {
    uint32_t candidate = start + offset;
    bool used = false;
    int rc = bitmap_get(candidate, &used);
    if (rc != 0) {
      return rc;
    }
    if (used) {
      continue;
    }

    rc = bitmap_update(candidate, true);
    if (rc != 0) {
      return rc;
    }

    void *block = block_ptr(candidate);
    if (!block) {
      return -EIO;
    }
    memset(block, 0, BSIZE);
    rc = sync_block(block);
    if (rc != 0) {
      return rc;
    }

    if (blockno_out) {
      *blockno_out = candidate;
    }
    return 0;
  }

  return -ENOSPC;
}

/*
 * Allocate a block near a hint for better file locality.
 * If hint is 0 or invalid, falls back to regular wear-leveling allocation.
 */
static int
alloc_block_near(uint32_t hint, uint32_t *blockno_out)
{
  if (hint == 0) {
    return alloc_block(blockno_out);
  }

  /* Try to use the block cache with locality hint */
  if (g_bcache.initialized) {
    uint32_t candidate;
    int rc = bcache_find_free_block_near(&g_bcache, hint, &candidate);
    if (rc != 0) {
      return rc;
    }

    /* Verify and update on-disk bitmap */
    bool used = false;
    rc = bitmap_get(candidate, &used);
    if (rc != 0) {
      return rc;
    }
    if (used) {
      /* Cache inconsistency - block already allocated, retry */
      /* Note: bcache_find_free_block_near already removed it from cache */
      return alloc_block_near(hint, blockno_out);
    }

    rc = bitmap_update(candidate, true);
    if (rc != 0) {
      /* Bitmap update failed - add block back to cache and retry */
      bcache_mark_free(&g_bcache, candidate);
      return rc;
    }

    /* Block is now allocated (already removed from cache by find_free_block_near) */

    void *block = block_ptr(candidate);
    if (!block) {
      return -EIO;
    }
    memset(block, 0, BSIZE);
    rc = sync_block(block);
    if (rc != 0) {
      return rc;
    }

    if (blockno_out) {
      *blockno_out = candidate;
    }
    return 0;
  }

  /* Fall back to regular allocation */
  return alloc_block(blockno_out);
}

/*
 * Allocate multiple blocks with locality hints for better performance.
 * Returns the first block number in start_out.
 * Blocks may not be strictly consecutive but will be allocated with locality hints.
 *
 * Note: This function is available for future use when batch allocation
 * is beneficial. Currently marked unused to suppress warning.
 */
static int __attribute__((unused))
alloc_consecutive_blocks(uint32_t count, uint32_t *start_out)
{
  if (count == 0) {
    return -EINVAL;
  }
  if (count == 1) {
    return alloc_block(start_out);
  }

  /* Allocate first block normally */
  uint32_t first;
  int rc = alloc_block(&first);
  if (rc != 0) return rc;
  if (start_out) *start_out = first;

  /* Allocate remaining blocks with locality hint */
  for (uint32_t i = 1; i < count; i++) {
    uint32_t next;
    rc = alloc_block_near(first + i, &next);
    if (rc != 0) {
      /* Rollback allocated blocks */
      free_block(first);
      for (uint32_t j = 1; j < i; j++) {
        /* Note: blocks may not be consecutive, but we allocated them */
        /* This is a simplification - ideally track all allocated blocks */
      }
      return rc;
    }
  }
  return 0;
}

static int
free_block(uint32_t blockno)
{
  bool used = false;
  int rc = bitmap_get(blockno, &used);
  if (rc != 0) {
    return rc;
  }
  if (!used) {
    return 0;
  }

  rc = bitmap_update(blockno, false);
  if (rc != 0) {
    return rc;
  }

  /* Update the cache */
  if (g_bcache.initialized) {
    bcache_mark_free(&g_bcache, blockno);
  }

  void *block = block_ptr(blockno);
  if (block) {
    memset(block, 0, BSIZE);
    rc = sync_block(block);
    if (rc != 0) {
      return rc;
    }
  }

  return 0;
}

static inline struct dinode *
get_inode(uint32_t inum)
{
  if (inum >= g_fs.sb.ninodes) {
    return NULL;
  }

  uint32_t blockno = IBLOCK(inum, g_fs.sb);
  char *block = block_ptr(blockno);
  if (!block) {
    return NULL;
  }

  return (struct dinode *)(block + (inum % IPB) * sizeof(struct dinode));
}

static inline uint16_t
inode_type(const struct dinode *ip)
{
  return from_le16(ip->type);
}

static inline uint16_t
inode_nlink(const struct dinode *ip)
{
  return from_le16(ip->nlink);
}

static inline uint32_t
inode_size(const struct dinode *ip)
{
  return from_le32(ip->size);
}

static int msync_inode_block(uint32_t inum);

static int
inode_block_address(const struct dinode *ip, uint32_t block_index, uint32_t *addr_out)
{
  if (block_index < NDIRECT) {
    uint32_t addr = from_le32(ip->addrs[block_index]);
    if (addr == 0) {
      return -ENOENT;
    }
    *addr_out = addr;
    return 0;
  }

  block_index -= NDIRECT;

  if (block_index < NINDIRECT) {
    uint32_t indirect_block = from_le32(ip->addrs[NDIRECT]);
    if (indirect_block == 0) {
      return -ENOENT;
    }
    uint32_t *entries = block_ptr(indirect_block);
    if (!entries) {
      return -EIO;
    }
    uint32_t addr = from_le32(entries[block_index]);
    if (addr == 0) {
      return -ENOENT;
    }
    *addr_out = addr;
    return 0;
  }

  block_index -= NINDIRECT;

  if (block_index < NDINDIRECT) {
    uint32_t doubly = from_le32(ip->addrs[NDIRECT + 1]);
    if (doubly == 0) {
      return -ENOENT;
    }
    uint32_t *l1 = block_ptr(doubly);
    if (!l1) {
      return -EIO;
    }
    uint32_t idx1 = block_index / NINDIRECT;
    uint32_t idx2 = block_index % NINDIRECT;
    uint32_t indirect = from_le32(l1[idx1]);
    if (indirect == 0) {
      return -ENOENT;
    }
    uint32_t *l2 = block_ptr(indirect);
    if (!l2) {
      return -EIO;
    }
    uint32_t addr = from_le32(l2[idx2]);
    if (addr == 0) {
      return -ENOENT;
    }
    *addr_out = addr;
    return 0;
  }

  return -EFBIG;
}

/*
 * Get a hint for block allocation based on adjacent blocks in the inode.
 * Returns the previous block's address if available, or 0 otherwise.
 */
static uint32_t
get_allocation_hint(const struct dinode *ip, uint32_t block_index)
{
  /* Try to get the previous block as a hint for locality */
  if (block_index > 0 && block_index <= NDIRECT) {
    uint32_t prev_addr = from_le32(ip->addrs[block_index - 1]);
    if (prev_addr != 0) {
      return prev_addr;
    }
  } else if (block_index == 0 && NDIRECT > 1) {
    /* For the first block, try to use an existing block as hint */
    for (uint32_t i = 1; i < NDIRECT; i++) {
      uint32_t addr = from_le32(ip->addrs[i]);
      if (addr != 0) {
        return addr;
      }
    }
  }
  return 0;
}

static int
inode_ensure_block(uint32_t inum, struct dinode *ip, uint32_t block_index, uint32_t *addr_out)
{
  if (block_index < NDIRECT) {
    uint32_t addr = from_le32(ip->addrs[block_index]);
    if (addr == 0) {
      /* Use hint-based allocation for better locality */
      uint32_t hint = get_allocation_hint(ip, block_index);
      int rc = alloc_block_near(hint, &addr);
      if (rc != 0) {
        return rc;
      }
      ip->addrs[block_index] = to_le32(addr);
      rc = msync_inode_block(inum);
      if (rc != 0) {
        return rc;
      }
    }
    if (addr_out) {
      *addr_out = from_le32(ip->addrs[block_index]);
    }
    return 0;
  }

  block_index -= NDIRECT;

  if (block_index < NINDIRECT) {
    uint32_t indirect = from_le32(ip->addrs[NDIRECT]);
    if (indirect == 0) {
      /* Use last direct block as hint for indirect block */
      uint32_t hint = from_le32(ip->addrs[NDIRECT - 1]);
      int rc = alloc_block_near(hint, &indirect);
      if (rc != 0) {
        return rc;
      }
      ip->addrs[NDIRECT] = to_le32(indirect);
      rc = msync_inode_block(inum);
      if (rc != 0) {
        return rc;
      }
    }

    uint32_t *entries = block_ptr(indirect);
    if (!entries) {
      return -EIO;
    }

    uint32_t addr = from_le32(entries[block_index]);
    if (addr == 0) {
      /* Use previous entry or indirect block as hint */
      uint32_t hint = (block_index > 0) ? from_le32(entries[block_index - 1]) : indirect;
      int rc = alloc_block_near(hint, &addr);
      if (rc != 0) {
        return rc;
      }
      entries[block_index] = to_le32(addr);
      rc = sync_block(entries);
      if (rc != 0) {
        return rc;
      }
    }

    if (addr_out) {
      *addr_out = addr;
    }
    return 0;
  }

  block_index -= NINDIRECT;

  if (block_index < NDINDIRECT) {
    uint32_t doubly = from_le32(ip->addrs[NDIRECT + 1]);
    if (doubly == 0) {
      /* Use single indirect block as hint */
      uint32_t hint = from_le32(ip->addrs[NDIRECT]);
      int rc = alloc_block_near(hint, &doubly);
      if (rc != 0) {
        return rc;
      }
      ip->addrs[NDIRECT + 1] = to_le32(doubly);
      rc = msync_inode_block(inum);
      if (rc != 0) {
        return rc;
      }
    }

    uint32_t *l1 = block_ptr(doubly);
    if (!l1) {
      return -EIO;
    }

    uint32_t idx1 = block_index / NINDIRECT;
    uint32_t idx2 = block_index % NINDIRECT;

    uint32_t indirect = from_le32(l1[idx1]);
    if (indirect == 0) {
      /* Use previous L1 entry or doubly block as hint */
      uint32_t hint = (idx1 > 0) ? from_le32(l1[idx1 - 1]) : doubly;
      int rc = alloc_block_near(hint, &indirect);
      if (rc != 0) {
        return rc;
      }
      l1[idx1] = to_le32(indirect);
      rc = sync_block(l1);
      if (rc != 0) {
        return rc;
      }
    }

    uint32_t *l2 = block_ptr(indirect);
    if (!l2) {
      return -EIO;
    }

    uint32_t addr = from_le32(l2[idx2]);
    if (addr == 0) {
      /* Use previous L2 entry or indirect block as hint */
      uint32_t hint = (idx2 > 0) ? from_le32(l2[idx2 - 1]) : indirect;
      int rc = alloc_block_near(hint, &addr);
      if (rc != 0) {
        return rc;
      }
      l2[idx2] = to_le32(addr);
      rc = sync_block(l2);
      if (rc != 0) {
        return rc;
      }
    }

    if (addr_out) {
      *addr_out = addr;
    }
    return 0;
  }

  return -EFBIG;
}

static int
inode_clear_block(uint32_t inum, struct dinode *ip, uint32_t block_index)
{
  if (block_index < NDIRECT) {
    uint32_t addr = from_le32(ip->addrs[block_index]);
    if (addr == 0) {
      return 0;
    }
    ip->addrs[block_index] = to_le32(0);
    int rc = msync_inode_block(inum);
    if (rc != 0) {
      return rc;
    }
    return free_block(addr);
  }

  block_index -= NDIRECT;

  if (block_index < NINDIRECT) {
    uint32_t indirect_block = from_le32(ip->addrs[NDIRECT]);
    if (indirect_block == 0) {
      return 0;
    }

    uint32_t *entries = block_ptr(indirect_block);
    if (!entries) {
      return -EIO;
    }

    uint32_t addr = from_le32(entries[block_index]);
    if (addr != 0) {
      entries[block_index] = to_le32(0);
      int rc = sync_block(entries);
      if (rc != 0) {
        return rc;
      }
      rc = free_block(addr);
      if (rc != 0) {
        return rc;
      }
    }

    if (buffer_is_zero(entries, BSIZE)) {
      ip->addrs[NDIRECT] = to_le32(0);
      int rc = msync_inode_block(inum);
      if (rc != 0) {
        return rc;
      }
      return free_block(indirect_block);
    }
    return 0;
  }

  block_index -= NINDIRECT;

  if (block_index < NDINDIRECT) {
    uint32_t doubly = from_le32(ip->addrs[NDIRECT + 1]);
    if (doubly == 0) {
      return 0;
    }

    uint32_t *l1 = block_ptr(doubly);
    if (!l1) {
      return -EIO;
    }

    uint32_t idx1 = block_index / NINDIRECT;
    uint32_t idx2 = block_index % NINDIRECT;

    uint32_t indirect = from_le32(l1[idx1]);
    if (indirect == 0) {
      return 0;
    }

    uint32_t *l2 = block_ptr(indirect);
    if (!l2) {
      return -EIO;
    }

    uint32_t addr = from_le32(l2[idx2]);
    if (addr != 0) {
      l2[idx2] = to_le32(0);
      int rc = sync_block(l2);
      if (rc != 0) {
        return rc;
      }
      rc = free_block(addr);
      if (rc != 0) {
        return rc;
      }
    }

    if (buffer_is_zero(l2, BSIZE)) {
      int rc = free_block(indirect);
      if (rc != 0) {
        return rc;
      }
      l1[idx1] = to_le32(0);
      rc = sync_block(l1);
      if (rc != 0) {
        return rc;
      }
      if (buffer_is_zero(l1, BSIZE)) {
        ip->addrs[NDIRECT + 1] = to_le32(0);
        rc = msync_inode_block(inum);
        if (rc != 0) {
          return rc;
        }
        return free_block(doubly);
      }
    }
    return 0;
  }

  return -EFBIG;
}

static ssize_t
inode_read(const struct dinode *ip, void *dst, off_t offset, size_t length)
{
  if (offset < 0) {
    return -EINVAL;
  }
  uint32_t size = inode_size(ip);
  if ((uint64_t)offset >= size) {
    return 0;
  }

  if ((uint64_t)offset + length > size) {
    uint64_t max_len = size - (uint64_t)offset;
    length = (size_t)max_len;
  }

  size_t copied = 0;
  while (copied < length) {
    uint64_t absolute = (uint64_t)offset + copied;
    uint32_t block_index = absolute / BSIZE;
    size_t block_offset = absolute % BSIZE;
    size_t to_copy = BSIZE - block_offset;
    if (to_copy > length - copied) {
      to_copy = length - copied;
    }

    uint32_t data_block;
    int rc = inode_block_address(ip, block_index, &data_block);
    if (rc != 0) {
      return rc;
    }

    char *block = block_ptr(data_block);
    if (!block) {
      return -EIO;
    }

    memcpy((char *)dst + copied, block + block_offset, to_copy);
    copied += to_copy;
  }

  return (ssize_t)copied;
}

static int
msync_inode_block(uint32_t inum)
{
  uint32_t blockno = IBLOCK(inum, g_fs.sb);
  void *addr = block_ptr(blockno);
  if (!addr) {
    return -EIO;
  }
  return sync_block(addr);
}

static ssize_t
inode_write(uint32_t inum, struct dinode *ip, const char *src, off_t offset, size_t length)
{
  if (length == 0) {
    return 0;
  }
  if (offset < 0) {
    return -EINVAL;
  }

  size_t copied = 0;
  while (copied < length) {
    uint64_t absolute = (uint64_t)offset + copied;
    uint32_t block_index = absolute / BSIZE;
    size_t block_offset = absolute % BSIZE;
    size_t to_copy = BSIZE - block_offset;
    if (to_copy > length - copied) {
      to_copy = length - copied;
    }

    uint32_t data_block;
    int rc = inode_ensure_block(inum, ip, block_index, &data_block);
    if (rc != 0) {
      if (copied == 0) {
        return rc;
      }
      break;
    }

    char *block = block_ptr(data_block);
    if (!block) {
      return -EIO;
    }

    memcpy(block + block_offset, src + copied, to_copy);
    copied += to_copy;
  }

  uint32_t old_size = inode_size(ip);
  uint64_t new_end = (uint64_t)offset + copied;
  if (copied > 0 && new_end > old_size) {
    if (new_end > UINT32_MAX) {
      return -EFBIG;
    }
    ip->size = to_le32((uint32_t)new_end);
  }

  /* Sync all written data in one call */
  if (copied > 0) {
    /* Just mark the whole write range dirty - sync will happen on flush */
    uint32_t start_block_idx = (uint32_t)(offset / BSIZE);
    uint32_t end_block_idx = (uint32_t)((offset + copied - 1) / BSIZE);
    for (uint32_t bi = start_block_idx; bi <= end_block_idx; bi++) {
      uint32_t data_block;
      if (inode_block_address(ip, bi, &data_block) == 0) {
        sync_range(block_ptr(data_block), BSIZE);
      }
    }
    msync_inode_block(inum);
  }

  return (ssize_t)copied;
}

static int
inode_truncate(uint32_t inum, struct dinode *ip, off_t length)
{
  if (length < 0) {
    return -EINVAL;
  }

  if ((uint64_t)length > UINT32_MAX) {
    return -EFBIG;
  }

  uint32_t current = inode_size(ip);
  if ((uint64_t)length > current) {
    return -EOPNOTSUPP;
  }

  uint32_t new_size = (uint32_t)length;
  if (new_size == current) {
    return 0;
  }

  uint32_t old_blocks = (current + BSIZE - 1) / BSIZE;
  uint32_t new_blocks = (new_size + BSIZE - 1) / BSIZE;

  for (uint32_t b = old_blocks; b > new_blocks; ) {
    --b;
    int rc = inode_clear_block(inum, ip, b);
    if (rc != 0) {
      return rc;
    }
  }

  if (new_blocks > 0 && (new_size % BSIZE) != 0) {
    uint32_t blockno;
    int rc = inode_block_address(ip, new_blocks - 1, &blockno);
    if (rc == 0) {
      char *block = block_ptr(blockno);
      if (!block) {
        return -EIO;
      }
      size_t start = new_size % BSIZE;
      memset(block + start, 0, BSIZE - start);
      rc = sync_range(block + start, BSIZE - start);
      if (rc != 0) {
        return rc;
      }
    }
  }

  ip->size = to_le32(new_size);
  return msync_inode_block(inum);
}

static int
reset_inode(uint32_t inum, struct dinode *ip)
{
  int rc = inode_truncate(inum, ip, 0);
  if (rc != 0) {
    return rc;
  }
  memset(ip, 0, sizeof(*ip));
  return msync_inode_block(inum);
}

static int
read_symlink_target(uint32_t inum, char *buf, size_t bufsize, size_t *len_out)
{
  const struct dinode *dip = get_inode(inum);
  if (!dip) {
    return -ENOENT;
  }
  if (inode_type(dip) != XV6_T_SYMLINK) {
    return -EINVAL;
  }

  size_t stored_len = 0;
  ssize_t rc = inode_read(dip, &stored_len, 0, sizeof(size_t));
  if (rc < 0) {
    return (int)rc;
  }
  if ((size_t)rc != sizeof(size_t)) {
    return -EIO;
  }

  uint32_t inode_sz = inode_size(dip);
  if (stored_len + sizeof(size_t) > inode_sz) {
    return -EIO;
  }

  if (buf) {
    if (bufsize == 0 || stored_len + 1 > bufsize) {
      return -ENAMETOOLONG;
    }
    rc = inode_read(dip, buf, sizeof(size_t), stored_len);
    if (rc < 0) {
      return (int)rc;
    }
    if ((size_t)rc != stored_len) {
      return -EIO;
    }
    buf[stored_len] = '\0';
  }

  if (len_out) {
    *len_out = stored_len;
  }

  return 0;
}

static int
lookup_dir_entry(uint32_t dir_inum, const char *name, uint32_t *result_inum)
{
  const struct dinode *dip = get_inode(dir_inum);
  if (!dip) {
    return -ENOENT;
  }
  if (inode_type(dip) != XV6_T_DIR) {
    return -ENOTDIR;
  }

  uint32_t dir_size = inode_size(dip);
  struct xv6_dirent entry;

  for (uint32_t off = 0; off + sizeof(entry) <= dir_size; off += sizeof(entry)) {
    ssize_t rc = inode_read(dip, &entry, off, sizeof(entry));
    if (rc < 0) {
      return (int)rc;
    }
    if (rc != sizeof(entry)) {
      return -EIO;
    }

    uint16_t inum = from_le16(entry.inum);
    if (inum == 0) {
      continue;
    }

    size_t len = strnlen(entry.name, DIRSIZ);
    if (len == strlen(name) && strncmp(entry.name, name, DIRSIZ) == 0) {
      *result_inum = inum;
      return 0;
    }
  }

  return -ENOENT;
}

static int
resolve_path_internal(const char *path, uint32_t *inum_out, bool follow_final)
{
  if (!path || path[0] == '\0') {
    return -ENOENT;
  }

  size_t path_len = strnlen(path, PATH_MAX);
  if (path_len >= PATH_MAX) {
    return -ENAMETOOLONG;
  }

  char pending[PATH_MAX];
  memcpy(pending, path, path_len + 1);

  uint32_t current = ROOTINO;
  char current_path[PATH_MAX];
  strcpy(current_path, "/");

  char *cursor = pending;
  int remaining_symlinks = XV6_SYMLINK_MAX_DEPTH;

  while (1) {
    while (*cursor == '/') {
      cursor++;
    }

    if (*cursor == '\0') {
      *inum_out = current;
      return 0;
    }

    char *component_start = cursor;
    size_t comp_len = 0;
    while (component_start[comp_len] != '\0' && component_start[comp_len] != '/') {
      comp_len++;
    }

    if (comp_len == 0) {
      if (component_start[comp_len] == '\0') {
        *inum_out = current;
        return 0;
      }
      cursor++;
      continue;
    }

    if (comp_len > DIRSIZ) {
      return -ENAMETOOLONG;
    }

    char component[DIRSIZ + 1];
    memcpy(component, component_start, comp_len);
    component[comp_len] = '\0';

    char *rest_start = component_start + comp_len;
    char rest[PATH_MAX];
    size_t rest_len = strnlen(rest_start, PATH_MAX);
    if (rest_len >= PATH_MAX) {
      return -ENAMETOOLONG;
    }
    memcpy(rest, rest_start, rest_len + 1);

    cursor = rest_start;
    while (*cursor == '/') {
      cursor++;
    }
    bool last_component = (*cursor == '\0');

    uint32_t child_inum;
    int rc = lookup_dir_entry(current, component, &child_inum);
    if (rc != 0) {
      return rc;
    }

    struct dinode *child = get_inode(child_inum);
    if (!child) {
      return -ENOENT;
    }

    uint16_t type = inode_type(child);

    if (type == XV6_T_SYMLINK && (!last_component || follow_final)) {
      if (remaining_symlinks == 0) {
        return -ELOOP;
      }
      remaining_symlinks--;

      char target[PATH_MAX];
      size_t target_len = 0;
      rc = read_symlink_target(child_inum, target, sizeof(target), &target_len);
      if (rc != 0) {
        return rc;
      }

      char new_path[PATH_MAX];
      if (target[0] == '/') {
        rc = snprintf(new_path, sizeof(new_path), "%s%s", target, rest);
      } else if (strcmp(current_path, "/") == 0) {
        rc = snprintf(new_path, sizeof(new_path), "/%s%s", target, rest);
      } else {
        rc = snprintf(new_path, sizeof(new_path), "%s/%s%s", current_path, target, rest);
      }

      if (rc < 0 || (size_t)rc >= sizeof(new_path)) {
        return -ENAMETOOLONG;
      }

      size_t new_len = strnlen(new_path, sizeof(new_path));
      memcpy(pending, new_path, new_len + 1);
      cursor = pending;
      current = ROOTINO;
      strcpy(current_path, "/");
      continue;
    }

    if (!last_component && type != XV6_T_DIR) {
      return -ENOTDIR;
    }

    current = child_inum;
    if (strcmp(component, ".") == 0) {
      /* no-op */
    } else if (strcmp(component, "..") == 0) {
      if (strcmp(current_path, "/") != 0) {
        char *slash = strrchr(current_path, '/');
        if (slash) {
          if (slash == current_path) {
            *(slash + 1) = '\0';
          } else {
            *slash = '\0';
          }
        } else {
          strcpy(current_path, "/");
        }
      }
    } else {
      if (strcmp(current_path, "/") == 0) {
        int wrote = snprintf(current_path, sizeof(current_path), "/%s", component);
        if (wrote < 0 || (size_t)wrote >= sizeof(current_path)) {
          return -ENAMETOOLONG;
        }
      } else {
        size_t cur_len = strnlen(current_path, sizeof(current_path));
        if (cur_len + 1 + comp_len >= sizeof(current_path)) {
          return -ENAMETOOLONG;
        }
        current_path[cur_len] = '/';
        memcpy(current_path + cur_len + 1, component, comp_len + 1);
      }
    }

    if (last_component) {
      *inum_out = current;
      return 0;
    }
  }
}

static int
resolve_path_follow(const char *path, uint32_t *inum_out)
{
  return resolve_path_internal(path, inum_out, true);
}

static int
resolve_path_nofollow(const char *path, uint32_t *inum_out)
{
  return resolve_path_internal(path, inum_out, false);
}

static int
resolve_parent(const char *path, uint32_t *parent_out, char name_out[DIRSIZ + 1])
{
  if (!path || path[0] != '/') {
    return -EINVAL;
  }

  size_t path_len = strnlen(path, PATH_MAX);
  if (path_len >= PATH_MAX) {
    return -ENAMETOOLONG;
  }

  char temp[PATH_MAX];
  memcpy(temp, path, path_len + 1);

  while (path_len > 1 && temp[path_len - 1] == '/') {
    temp[--path_len] = '\0';
  }

  char *last = strrchr(temp, '/');
  if (!last) {
    return -EINVAL;
  }

  char *name = last + 1;
  if (*name == '\0') {
    return -EINVAL;
  }
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
    return -EINVAL;
  }
  size_t name_len = strlen(name);
  if (name_len > DIRSIZ) {
    return -ENAMETOOLONG;
  }
  memcpy(name_out, name, name_len + 1);

  char parent_path[PATH_MAX];
  if (last == temp) {
    parent_path[0] = '/';
    parent_path[1] = '\0';
  } else {
    *last = '\0';
    size_t parent_len = strlen(temp);
    memcpy(parent_path, temp, parent_len + 1);
  }

  return resolve_path_follow(parent_path, parent_out);
}

static void
dirent_copy_name(const char *src, char *dst)
{
  memset(dst, 0, DIRSIZ);
  size_t len = strnlen(src, DIRSIZ);
  memcpy(dst, src, len);
}

static void
dirent_to_name(const struct xv6_dirent *entry, char buf[DIRSIZ + 1])
{
  size_t len = strnlen(entry->name, DIRSIZ);
  memcpy(buf, entry->name, len);
  buf[len] = '\0';
}

static int
dir_add_entry(uint32_t dir_inum, struct dinode *dir_ip, const char *name, uint32_t child_inum)
{
  struct xv6_dirent entry;
  uint32_t dir_size = inode_size(dir_ip);

  for (uint32_t off = 0; off + sizeof(entry) <= dir_size; off += sizeof(entry)) {
    ssize_t rc = inode_read(dir_ip, &entry, off, sizeof(entry));
    if (rc < 0) {
      return (int)rc;
    }
    if ((size_t)rc != sizeof(entry)) {
      return -EIO;
    }

    if (from_le16(entry.inum) == 0) {
      entry.inum = to_le16(child_inum);
      dirent_copy_name(name, entry.name);
      rc = inode_write(dir_inum, dir_ip, (const char *)&entry, off, sizeof(entry));
      if (rc < 0) {
        return (int)rc;
      }
      if ((size_t)rc != sizeof(entry)) {
        return -EIO;
      }
      return 0;
    }
  }

  memset(&entry, 0, sizeof(entry));
  entry.inum = to_le16(child_inum);
  dirent_copy_name(name, entry.name);
  ssize_t wrote = inode_write(dir_inum, dir_ip, (const char *)&entry, dir_size, sizeof(entry));
  if (wrote < 0) {
    return (int)wrote;
  }
  if ((size_t)wrote != sizeof(entry)) {
    return -EIO;
  }
  return 0;
}

static int
dir_remove_entry(uint32_t dir_inum, struct dinode *dir_ip, const char *name)
{
  uint32_t dir_size = inode_size(dir_ip);
  struct xv6_dirent entry;

  for (uint32_t off = 0; off + sizeof(entry) <= dir_size; off += sizeof(entry)) {
    ssize_t rc = inode_read(dir_ip, &entry, off, sizeof(entry));
    if (rc < 0) {
      return (int)rc;
    }
    if ((size_t)rc != sizeof(entry)) {
      return -EIO;
    }

    if (from_le16(entry.inum) == 0) {
      continue;
    }

    char entry_name[DIRSIZ + 1];
    dirent_to_name(&entry, entry_name);
    if (strcmp(entry_name, name) != 0) {
      continue;
    }

    memset(&entry, 0, sizeof(entry));
    rc = inode_write(dir_inum, dir_ip, (const char *)&entry, off, sizeof(entry));
    if (rc < 0) {
      return (int)rc;
    }
    if ((size_t)rc != sizeof(entry)) {
      return -EIO;
    }
    return 0;
  }

  return -ENOENT;
}

static int
allocate_inode(uint16_t type, uint32_t *inum_out)
{
  for (uint32_t inum = 1; inum < g_fs.sb.ninodes; ++inum) {
    struct dinode *dip = get_inode(inum);
    if (!dip) {
      return -EIO;
    }
    if (inode_type(dip) == 0) {
      memset(dip, 0, sizeof(*dip));
      dip->type = to_le16(type);
      dip->nlink = to_le16(1);
      dip->size = to_le32(0);
      int rc = msync_inode_block(inum);
      if (rc != 0) {
        return rc;
      }
      if (inum_out) {
        *inum_out = inum;
      }
      return 0;
    }
  }

  return -ENOSPC;
}

static mode_t
mode_from_type(uint16_t type)
{
  switch (type) {
  case XV6_T_DIR:
    return S_IFDIR | 0755;
  case XV6_T_FILE:
    return S_IFREG | 0644;
  case XV6_T_DEVICE:
    return S_IFCHR | 0600;
  case XV6_T_SYMLINK:
    return S_IFLNK | 0777;
  default:
    return 0;
  }
}

static int
xv6_create_node(const char *path, mode_t mode, dev_t rdev, uint32_t *inum_out)
{
  (void)rdev;
  if (g_fs.readonly) {
    return -EROFS;
  }

  uint32_t parent_inum;
  char name[DIRSIZ + 1];
  int rc = resolve_parent(path, &parent_inum, name);
  if (rc != 0) {
    return rc;
  }

  struct dinode *parent = get_inode(parent_inum);
  if (!parent) {
    return -ENOENT;
  }
  if (inode_type(parent) != XV6_T_DIR) {
    return -ENOTDIR;
  }

  uint32_t existing;
  rc = lookup_dir_entry(parent_inum, name, &existing);
  if (rc == 0) {
    return -EEXIST;
  }
  if (rc != -ENOENT) {
    return rc;
  }

  uint16_t type;
  if (S_ISREG(mode)) {
    type = XV6_T_FILE;
  } else if (S_ISDIR(mode)) {
    type = XV6_T_DIR;
  } else if (S_ISLNK(mode)) {
    type = XV6_T_SYMLINK;
  } else {
    return -EOPNOTSUPP;
  }

  uint32_t inum;
  rc = allocate_inode(type, &inum);
  if (rc != 0) {
    return rc;
  }

  struct dinode *child = get_inode(inum);
  if (!child) {
    return -EIO;
  }

  bool parent_linked = false;
  bool parent_nlink_inc = false;

  if (type == XV6_T_DIR) {
    rc = dir_add_entry(inum, child, ".", inum);
    if (rc != 0) {
      goto fail;
    }
    rc = dir_add_entry(inum, child, "..", parent_inum);
    if (rc != 0) {
      goto fail;
    }
  }

  rc = dir_add_entry(parent_inum, parent, name, inum);
  if (rc != 0) {
    goto fail;
  }
  parent_linked = true;

  if (type == XV6_T_DIR) {
    uint16_t parent_links = inode_nlink(parent);
    if (parent_links == UINT16_MAX) {
      rc = -EMLINK;
      goto fail;
    }
    parent->nlink = to_le16(parent_links + 1);
    parent_nlink_inc = true;
    rc = msync_inode_block(parent_inum);
    if (rc != 0) {
      goto fail;
    }
  }

  if (inum_out) {
    *inum_out = inum;
  }

  return 0;

fail:
  if (parent_nlink_inc) {
    uint16_t parent_links = inode_nlink(parent);
    if (parent_links > 0) {
      parent->nlink = to_le16(parent_links - 1);
      (void)msync_inode_block(parent_inum);
    }
  }
  if (parent_linked) {
    (void)dir_remove_entry(parent_inum, parent, name);
  }
  (void)reset_inode(inum, child);
  return rc;
}

static int
xv6_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
  uint32_t inum;
  int rc = xv6_create_node(path, mode, 0, &inum);
  if (rc != 0) {
    return rc;
  }

  fi->fh = inum;
  return 0;
}

static int
xv6_mkdir(const char *path, mode_t mode)
{
  return xv6_create_node(path, mode | S_IFDIR, 0, NULL);
}

static int
xv6_mknod(const char *path, mode_t mode, dev_t rdev)
{
  return xv6_create_node(path, mode, rdev, NULL);
}

static int xv6_unlink(const char *path);

static int
xv6_symlink(const char *target, const char *linkpath)
{
  if (g_fs.readonly) {
    return -EROFS;
  }
  if (!target || !linkpath) {
    return -EINVAL;
  }

  size_t len = strnlen(target, PATH_MAX);
  if (len == 0) {
    return -EINVAL;
  }
  if (len >= PATH_MAX) {
    return -ENAMETOOLONG;
  }

  uint32_t inum;
  int rc = xv6_create_node(linkpath, S_IFLNK, 0, &inum);
  if (rc != 0) {
    return rc;
  }

  struct dinode *dip = get_inode(inum);
  if (!dip) {
    (void)xv6_unlink(linkpath);
    return -ENOENT;
  }

  ssize_t wrote = inode_write(inum, dip, (const char *)&len, 0, sizeof(size_t));
  if (wrote < 0 || (size_t)wrote != sizeof(size_t)) {
    rc = (wrote < 0) ? (int)wrote : -EIO;
    goto fail;
  }

  wrote = inode_write(inum, dip, target, sizeof(size_t), len);
  if (wrote < 0 || (size_t)wrote != len) {
    rc = (wrote < 0) ? (int)wrote : -EIO;
    goto fail;
  }

  rc = msync_inode_block(inum);
  if (rc != 0) {
    goto fail;
  }

  return 0;

fail:
  (void)xv6_unlink(linkpath);
  return rc;
}

static int
xv6_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi)
{
  (void)tv;
  (void)fi;

  if (g_fs.readonly) {
    return -EROFS;
  }

  uint32_t inum;
  int rc = resolve_path_follow(path, &inum);
  if (rc != 0) {
    return rc;
  }

  if (!get_inode(inum)) {
    return -ENOENT;
  }

  /* xv6 does not store timestamps, so we acknowledge the request without changes. */
  return 0;
}

static int
xv6_unlink(const char *path)
{
  if (g_fs.readonly) {
    return -EROFS;
  }

  uint32_t parent_inum;
  char name[DIRSIZ + 1];
  int rc = resolve_parent(path, &parent_inum, name);
  if (rc != 0) {
    return rc;
  }

  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
    return -EINVAL;
  }

  struct dinode *parent = get_inode(parent_inum);
  if (!parent) {
    return -ENOENT;
  }
  if (inode_type(parent) != XV6_T_DIR) {
    return -ENOTDIR;
  }

  uint32_t dir_size = inode_size(parent);
  struct xv6_dirent entry;
  uint32_t entry_offset = 0;
  bool found = false;

  for (uint32_t off = 0; off + sizeof(entry) <= dir_size; off += sizeof(entry)) {
    ssize_t read_rc = inode_read(parent, &entry, off, sizeof(entry));
    if (read_rc < 0) {
      return (int)read_rc;
    }
    if ((size_t)read_rc != sizeof(entry)) {
      return -EIO;
    }

    uint16_t child_inum = from_le16(entry.inum);
    if (child_inum == 0) {
      continue;
    }

    char entry_name[DIRSIZ + 1];
    dirent_to_name(&entry, entry_name);
    if (strcmp(entry_name, name) == 0) {
      entry_offset = off;
      found = true;
      break;
    }
  }

  if (!found) {
    return -ENOENT;
  }

  uint16_t child_inum = from_le16(entry.inum);
  struct dinode *child = get_inode(child_inum);
  if (!child) {
    return -ENOENT;
  }

  if (inode_type(child) == XV6_T_DIR) {
    return -EISDIR;
  }

  uint16_t nlink = inode_nlink(child);
  if (nlink == 0) {
    return -ENOENT;
  }

  if (nlink == 1) {
    rc = inode_truncate(child_inum, child, 0);
    if (rc != 0) {
      return rc;
    }
    memset(child, 0, sizeof(*child));
  } else {
    child->nlink = to_le16(nlink - 1);
  }

  rc = msync_inode_block(child_inum);
  if (rc != 0) {
    return rc;
  }

  memset(&entry, 0, sizeof(entry));
  ssize_t write_rc = inode_write(parent_inum, parent, (const char *)&entry, entry_offset, sizeof(entry));
  if (write_rc < 0) {
    return (int)write_rc;
  }
  if ((size_t)write_rc != sizeof(entry)) {
    return -EIO;
  }

  return 0;
}

static int
xv6_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
  (void)fi;
  memset(stbuf, 0, sizeof(*stbuf));

  uint32_t inum;
  int rc = resolve_path_nofollow(path, &inum);
  if (rc != 0) {
    return rc;
  }

  const struct dinode *dip = get_inode(inum);
  if (!dip) {
    return -ENOENT;
  }

  uint16_t type = inode_type(dip);
  mode_t mode = mode_from_type(type);
  if (mode == 0) {
    return -ENOENT;
  }

  stbuf->st_mode = mode;
  stbuf->st_nlink = inode_nlink(dip);
  if (type == XV6_T_SYMLINK) {
    size_t link_len = 0;
    int link_rc = read_symlink_target(inum, NULL, 0, &link_len);
    stbuf->st_size = (link_rc == 0) ? (off_t)link_len : (off_t)inode_size(dip);
  } else {
    stbuf->st_size = inode_size(dip);
  }
  stbuf->st_uid = getuid();
  stbuf->st_gid = getgid();
  stbuf->st_blksize = BSIZE;
  stbuf->st_blocks = (stbuf->st_size + 511) / 512;
  stbuf->st_ino = inum;
  return 0;
}

static int
xv6_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
            struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
  (void)offset;
  (void)flags;
  (void)fi;

  uint32_t inum;
  int rc = resolve_path_follow(path, &inum);
  if (rc != 0) {
    return rc;
  }

  const struct dinode *dip = get_inode(inum);
  if (!dip) {
    return -ENOENT;
  }

  if (inode_type(dip) != XV6_T_DIR) {
    return -ENOTDIR;
  }

  filler(buf, ".", NULL, 0, 0);
  filler(buf, "..", NULL, 0, 0);

  uint32_t dir_size = inode_size(dip);
  struct xv6_dirent entry;

  for (uint32_t off = 0; off + sizeof(entry) <= dir_size; off += sizeof(entry)) {
    ssize_t got = inode_read(dip, &entry, off, sizeof(entry));
    if (got < 0) {
      return (int)got;
    }
    if (got != sizeof(entry)) {
      continue;
    }

    uint16_t child_inum = from_le16(entry.inum);
    if (child_inum == 0) {
      continue;
    }

    char name[DIRSIZ + 1];
    size_t name_len = strnlen(entry.name, DIRSIZ);
    memcpy(name, entry.name, name_len);
    name[name_len] = '\0';

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }

    filler(buf, name, NULL, 0, 0);
  }

  return 0;
}

static int
xv6_open(const char *path, struct fuse_file_info *fi)
{
  uint32_t inum;
  int rc = resolve_path_follow(path, &inum);
  if (rc != 0) {
    return rc;
  }

  struct dinode *dip = get_inode(inum);
  if (!dip) {
    return -ENOENT;
  }

  int accmode = fi->flags & O_ACCMODE;
  if ((accmode == O_WRONLY || accmode == O_RDWR) && g_fs.readonly) {
    return -EROFS;
  }

  if (inode_type(dip) == XV6_T_DIR) {
    return -EISDIR;
  }

  if ((fi->flags & O_TRUNC) && !g_fs.readonly) {
    rc = inode_truncate(inum, dip, 0);
    if (rc != 0) {
      return rc;
    }
  } else if ((fi->flags & O_TRUNC) && g_fs.readonly) {
    return -EROFS;
  }

  fi->fh = inum;
  return 0;
}

static int
xv6_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
  (void)path;

  uint32_t inum = (fi && fi->fh) ? (uint32_t)fi->fh : 0;
  if (inum == 0) {
    int rc = resolve_path_follow(path, &inum);
    if (rc != 0) {
      return rc;
    }
  }

  const struct dinode *dip = get_inode(inum);
  if (!dip) {
    return -ENOENT;
  }

  ssize_t rc = inode_read(dip, buf, offset, size);
  if (rc < 0) {
    return (int)rc;
  }
  return (int)rc;
}

static int
xv6_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
  if (size == 0) {
    return 0;
  }
  if (g_fs.readonly) {
    return -EROFS;
  }

  uint32_t inum = (fi && fi->fh) ? (uint32_t)fi->fh : 0;
  if (inum == 0) {
    int rc = resolve_path_follow(path, &inum);
    if (rc != 0) {
      return rc;
    }
  }

  struct dinode *dip = get_inode(inum);
  if (!dip) {
    return -ENOENT;
  }
  if (inode_type(dip) == XV6_T_DIR) {
    return -EISDIR;
  }

  ssize_t rc = inode_write(inum, dip, buf, offset, size);
  if (rc < 0) {
    return (int)rc;
  }
  return (int)rc;
}

static int
xv6_readlink(const char *path, char *buf, size_t size)
{
  uint32_t inum;
  int rc = resolve_path_nofollow(path, &inum);
  if (rc != 0) {
    return rc;
  }

  const struct dinode *dip = get_inode(inum);
  if (!dip) {
    return -ENOENT;
  }

  if (inode_type(dip) != XV6_T_SYMLINK) {
    return -EINVAL;
  }

  if (size == 0) {
    return -EINVAL;
  }

  rc = read_symlink_target(inum, buf, size, NULL);
  if (rc != 0) {
    return rc;
  }

  return 0;
}

static int
xv6_truncate_cb(const char *path, off_t length, struct fuse_file_info *fi)
{
  if (g_fs.readonly) {
    return -EROFS;
  }
  if (length < 0) {
    return -EINVAL;
  }

  uint32_t inum = (fi && fi->fh) ? (uint32_t)fi->fh : 0;
  if (inum == 0) {
    int rc = resolve_path_follow(path, &inum);
    if (rc != 0) {
      return rc;
    }
  }

  struct dinode *dip = get_inode(inum);
  if (!dip) {
    return -ENOENT;
  }
  if (inode_type(dip) == XV6_T_DIR) {
    return -EISDIR;
  }

  return inode_truncate(inum, dip, length);
}

static int
xv6_opendir(const char *path, struct fuse_file_info *fi)
{
  uint32_t inum;
  int rc = resolve_path_follow(path, &inum);
  if (rc != 0) {
    return rc;
  }

  const struct dinode *dip = get_inode(inum);
  if (!dip) {
    return -ENOENT;
  }

  if (inode_type(dip) != XV6_T_DIR) {
    return -ENOTDIR;
  }

  fi->fh = inum;
  return 0;
}

static void *
xv6_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
  /* Enable writeback caching for much better write performance.
   * This allows FUSE to batch writes and reduces syscall overhead. */
  if (conn->capable & FUSE_CAP_WRITEBACK_CACHE) {
    conn->want |= FUSE_CAP_WRITEBACK_CACHE;
  }

  cfg->kernel_cache = 1;

  /* Enable direct_io = 0 to use page cache */
  cfg->direct_io = 0;

  return &g_fs;
}

/*
 * Flush file data to disk.
 * Called when a file is closed.
 */
static int
xv6_flush(const char *path, struct fuse_file_info *fi)
{
  (void)path;
  (void)fi;

  /* Sync the entire image to disk */
  if (g_fs.image && !g_fs.readonly) {
    msync(g_fs.image, g_fs.image_size, MS_ASYNC);
  }
  return 0;
}

/*
 * Synchronize file contents.
 * Called by fsync() and fdatasync().
 */
static int
xv6_fsync(const char *path, int isdatasync, struct fuse_file_info *fi)
{
  (void)path;
  (void)isdatasync;

  if (g_fs.readonly) {
    return 0;
  }

  uint32_t inum = (fi && fi->fh) ? (uint32_t)fi->fh : 0;
  if (inum != 0) {
    /* Sync the inode and its data blocks */
    struct dinode *dip = get_inode(inum);
    if (dip) {
      uint32_t size = inode_size(dip);
      uint32_t nblocks = (size + BSIZE - 1) / BSIZE;

      /* Sync all data blocks */
      for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t data_block;
        if (inode_block_address(dip, bi, &data_block) == 0) {
          void *block = block_ptr(data_block);
          if (block) {
            sync_range_sync(block, BSIZE);
          }
        }
      }

      /* Sync the inode block */
      sync_range_sync(dip, sizeof(*dip));
    }
  }

  return 0;
}

static void
xv6_destroy(void *userdata)
{
  (void)userdata;

  /* Destroy the block cache */
  bcache_destroy(&g_bcache);

  if (g_fs.image && g_fs.image_size) {
    munmap(g_fs.image, g_fs.image_size);
  }
  g_fs.image = NULL;
  g_fs.image_size = 0;
  if (g_fs.fd >= 0) {
    close(g_fs.fd);
    g_fs.fd = -1;
  }
  free(g_fs.image_path);
  g_fs.image_path = NULL;
  g_fs.readonly = false;
}

/*
 * Return filesystem statistics
 */
static int
xv6_statfs(const char *path, struct statvfs *stbuf)
{
  (void)path;

  memset(stbuf, 0, sizeof(*stbuf));

  stbuf->f_bsize = BSIZE;
  stbuf->f_frsize = BSIZE;
  stbuf->f_blocks = g_fs.sb.nblocks;

  /* Use cache for accurate free block count */
  if (g_bcache.initialized) {
    stbuf->f_bfree = g_bcache.free_count;
    stbuf->f_bavail = g_bcache.free_count;
  } else {
    /* Fall back to counting from bitmap */
    uint32_t free_count = 0;
    uint32_t start = data_start_block();
    for (uint32_t i = 0; i < g_fs.sb.nblocks; i++) {
      bool used = false;
      if (bitmap_get(start + i, &used) == 0 && !used) {
        free_count++;
      }
    }
    stbuf->f_bfree = free_count;
    stbuf->f_bavail = free_count;
  }

  stbuf->f_files = g_fs.sb.ninodes;

  /* Count free inodes */
  uint32_t free_inodes = 0;
  for (uint32_t i = 1; i < g_fs.sb.ninodes; i++) {
    const struct dinode *dip = get_inode(i);
    if (dip && inode_type(dip) == 0) {
      free_inodes++;
    }
  }
  stbuf->f_ffree = free_inodes;
  stbuf->f_favail = free_inodes;

  stbuf->f_namemax = DIRSIZ;

  return 0;
}

static struct fuse_operations xv6_ops = {
  .init = xv6_init,
  .destroy = xv6_destroy,
  .getattr = xv6_getattr,
  .readdir = xv6_readdir,
  .mknod = xv6_mknod,
  .create = xv6_create,
  .unlink = xv6_unlink,
  .open = xv6_open,
  .read = xv6_read,
  .write = xv6_write,
  .flush = xv6_flush,
  .fsync = xv6_fsync,
  .opendir = xv6_opendir,
  .readlink = xv6_readlink,
  .symlink = xv6_symlink,
  .mkdir = xv6_mkdir,
  .utimens = xv6_utimens,
  .truncate = xv6_truncate_cb,
  .statfs = xv6_statfs,
};

struct xv6_options {
  const char *image_path;
  int show_help;
  int readonly;
  int sync_writes;
};

#define OPTION(t, p) { t, offsetof(struct xv6_options, p), 1 }

static const struct fuse_opt option_spec[] = {
  OPTION("--image=%s", image_path),
  OPTION("-i %s", image_path),
  OPTION("--help", show_help),
  OPTION("--readonly", readonly),
  OPTION("--read-only", readonly),
  OPTION("-r", readonly),
  OPTION("--sync", sync_writes),
  OPTION("-s", sync_writes),
  FUSE_OPT_END
};

static void
print_usage(const char *prog)
{
  fprintf(stderr,
          "usage: %s --image=PATH <mountpoint> [FUSE options...]\n\n"
          "Mount an xv6 fs.img via FUSE (read-write when permitted).\n\n"
          "Options:\n"
          "  --image=PATH, -i PATH  Path to the xv6 fs.img file\n"
          "  --readonly, -r         Mount read-only\n"
          "  --sync, -s             Use synchronous writes (slower but safer)\n"
          "  --help                 Show this help message\n",
          prog);
}

static int
load_superblock(void)
{
  if (!g_fs.image) {
    return -EINVAL;
  }

  if (g_fs.image_size < BSIZE + sizeof(struct superblock)) {
    fprintf(stderr, "[xv6fs] image too small to contain superblock\n");
    return -EINVAL;
  }

  struct superblock raw;
  memcpy(&raw, (char *)g_fs.image + BSIZE, sizeof(raw));

  g_fs.sb.magic = from_le32(raw.magic);
  g_fs.sb.size = from_le32(raw.size);
  g_fs.sb.nblocks = from_le32(raw.nblocks);
  g_fs.sb.ninodes = from_le32(raw.ninodes);
  g_fs.sb.nlog = from_le32(raw.nlog);
  g_fs.sb.logstart = from_le32(raw.logstart);
  g_fs.sb.inodestart = from_le32(raw.inodestart);
  g_fs.sb.bmapstart = from_le32(raw.bmapstart);

  if (g_fs.sb.magic != FSMAGIC) {
    fprintf(stderr, "[xv6fs] invalid superblock magic (0x%x)\n", g_fs.sb.magic);
    return -EINVAL;
  }

  if (g_fs.image_size < (size_t)g_fs.sb.size * BSIZE) {
    fprintf(stderr, "[xv6fs] warning: image smaller than advertised (%zu < %u blocks)\n",
            g_fs.image_size, g_fs.sb.size);
  }

  /* Initialize the block cache for free block tracking */
  uint32_t data_start = data_start_block();
  int rc = bcache_init(&g_bcache, g_fs.sb.nblocks, data_start);
  if (rc != 0) {
    fprintf(stderr, "[xv6fs] warning: failed to initialize block cache: %s\n",
            strerror(-rc));
    /* Non-fatal - we can fall back to linear search */
  }

  return 0;
}

static int
map_image(const char *path)
{
  struct stat st;
  int open_flags = g_fs.readonly ? O_RDONLY : O_RDWR;
  g_fs.fd = open(path, open_flags);
  if (g_fs.fd < 0 && !g_fs.readonly) {
    int saved = errno;
    fprintf(stderr, "[xv6fs] warning: %s opening '%s' read-write, retrying read-only\n",
            strerror(saved), path);
    g_fs.readonly = true;
    g_fs.fd = open(path, O_RDONLY);
  }

  if (g_fs.fd < 0) {
    int saved = errno;
    perror("open");
    return -saved;
  }

  if (fstat(g_fs.fd, &st) < 0) {
    int saved = errno;
    perror("fstat");
    close(g_fs.fd);
    g_fs.fd = -1;
    return -saved;
  }

  g_fs.image_size = (size_t)st.st_size;
  int prot = PROT_READ | (g_fs.readonly ? 0 : PROT_WRITE);
  int map_flags = MAP_SHARED;
  g_fs.image = mmap(NULL, g_fs.image_size, prot, map_flags, g_fs.fd, 0);
  if (g_fs.image == MAP_FAILED) {
    int saved = errno;
    perror("mmap");
    close(g_fs.fd);
    g_fs.fd = -1;
    g_fs.image = NULL;
    return -saved;
  }

  g_fs.image_path = strdup(path);
  if (!g_fs.image_path) {
    int saved = errno;
    munmap(g_fs.image, g_fs.image_size);
    g_fs.image = NULL;
    g_fs.image_size = 0;
    close(g_fs.fd);
    g_fs.fd = -1;
    return -saved;
  }

  int rc = load_superblock();
  if (rc != 0) {
    xv6_destroy(&g_fs);
    return rc;
  }

  return 0;
}

int
main(int argc, char *argv[])
{
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  struct xv6_options options = {
    .image_path = NULL,
    .show_help = 0,
    .readonly = 0,
  };

  if (fuse_opt_parse(&args, &options, option_spec, NULL) == -1) {
    return EXIT_FAILURE;
  }

  if (options.show_help) {
    print_usage(argv[0]);
    fuse_opt_add_arg(&args, "--help");
    fuse_main(args.argc, args.argv, &xv6_ops, &g_fs);
    fuse_opt_free_args(&args);
    return EXIT_SUCCESS;
  }

  if (!options.image_path) {
    print_usage(argv[0]);
    fuse_opt_free_args(&args);
    return EXIT_FAILURE;
  }

  g_fs.readonly = options.readonly != 0;
  g_fs.sync_writes = options.sync_writes != 0;

  int rc = map_image(options.image_path);
  if (rc != 0) {
    fprintf(stderr, "[xv6fs] failed to load image '%s': %s\n",
            options.image_path, strerror(-rc));
    fuse_opt_free_args(&args);
    return EXIT_FAILURE;
  }

  rc = fuse_main(args.argc, args.argv, &xv6_ops, &g_fs);

  fuse_opt_free_args(&args);
  xv6_destroy(&g_fs);

  return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
