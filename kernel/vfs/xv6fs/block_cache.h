/*
 * xv6fs block cache - Red-Black Tree based free extent tracking
 *
 * This implements a red-black tree cache for fast free block allocation
 * with the following features:
 * - O(log n) free extent search using rb-tree keyed by block number
 * - Extent merging for contiguous free regions
 * - Locality-aware allocation for consecutive block placement
 * - Wear leveling via rotating allocation cursor
 *
 * Free blocks are stored as extents (start, length) in an rb-tree,
 * allowing efficient allocation of contiguous blocks for large files.
 */

#ifndef KERNEL_VFS_XV6FS_BLOCK_CACHE_H
#define KERNEL_VFS_XV6FS_BLOCK_CACHE_H

#include "types.h"
#include "lock/spinlock.h"
#include "bintree_type.h"
#include "mm/slab.h"

/*
 * Free extent node - represents a contiguous range of free blocks
 */
struct free_extent {
    struct rb_node rb_node;     /* Red-black tree node (keyed by start block) */
    uint32 start;               /* First block number in this extent */
    uint32 length;              /* Number of contiguous free blocks */
};

/*
 * Block allocation cache structure
 * Embedded in xv6fs_superblock for per-mount caching
 */
struct xv6fs_block_cache {
    struct rb_root extent_tree;         /* RB-tree of free extents */
    struct rb_root_opts tree_opts;      /* Tree comparison functions */
    slab_cache_t extent_cache;          /* Slab cache for extent nodes */
    uint32 nblocks;                     /* Total data blocks */
    uint32 data_start;                  /* First data block number */
    uint32 alloc_cursor;                /* Rotating allocation pointer for wear leveling */
    uint32 free_count;                  /* Total number of free blocks */
    uint32 extent_count;                /* Number of extents in tree */
    spinlock_t lock;               /* Protect cache operations */
    int initialized;                    /* Cache is ready for use */
};

/* Forward declaration */
struct xv6fs_superblock;

/*
 * Initialize the block cache from on-disk bitmap
 * Called during filesystem mount
 * Returns 0 on success, negative error code on failure
 */
int xv6fs_bcache_init(struct xv6fs_superblock *xv6_sb);

/*
 * Destroy the block cache and free memory
 * Called during filesystem unmount
 */
void xv6fs_bcache_destroy(struct xv6fs_superblock *xv6_sb);

/*
 * Mark a block as free in the cache
 * Called when a block is freed
 */
void xv6fs_bcache_mark_free(struct xv6fs_superblock *xv6_sb, uint32 blockno);

/*
 * Find a free block using rb-tree search with wear leveling
 * Returns 0 on success with block number in blockno_out
 * Returns -ENOSPC if no free blocks available
 */
int xv6fs_bcache_find_free_block(struct xv6fs_superblock *xv6_sb, uint32 *blockno_out);

/*
 * Find a free block near a hint block for better locality
 * Searches for extent containing or near the hint first
 * Returns 0 on success, -ENOSPC if no free blocks
 */
int xv6fs_bcache_find_free_block_near(struct xv6fs_superblock *xv6_sb, 
                                       uint32 hint, uint32 *blockno_out);

/*
 * Get the number of free blocks
 */
uint32 xv6fs_bcache_free_count(struct xv6fs_superblock *xv6_sb);

#endif /* KERNEL_VFS_XV6FS_BLOCK_CACHE_H */
