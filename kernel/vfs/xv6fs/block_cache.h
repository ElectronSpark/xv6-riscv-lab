/*
 * xv6fs block cache - buddy-like bitmap cache for free block tracking
 *
 * This implements a hierarchical bitmap cache for fast free block allocation
 * with the following features:
 * - Multi-level bitmap for O(log n) free block search
 * - Wear leveling via rotating allocation cursor
 * - Locality-aware allocation for consecutive block placement
 *
 * The hierarchy works as follows:
 * - Level 0: 1 bit per block (1 = free, 0 = used)
 * - Level 1: 1 bit per 64 blocks (1 = has free, 0 = all used)
 * - Level 2: 1 bit per 4096 blocks
 * - Level 3: 1 bit per 262144 blocks
 */

#ifndef KERNEL_VFS_XV6FS_BLOCK_CACHE_H
#define KERNEL_VFS_XV6FS_BLOCK_CACHE_H

#include "types.h"
#include "lock/spinlock.h"

#define BCACHE_BITS_PER_LEVEL 64
#define BCACHE_MAX_LEVELS 4
#define BCACHE_MAX_PAGES 8    /* Max pages per level (32KB = 256K blocks) */

/*
 * Block allocation cache structure
 * Embedded in xv6fs_superblock for per-mount caching
 */
struct xv6fs_block_cache {
    uint8 *levels[BCACHE_MAX_LEVELS];      /* Bitmap for each level */
    uint8 *level_pages[BCACHE_MAX_LEVELS][BCACHE_MAX_PAGES]; /* Page pointers for freeing */
    uint32 level_npages[BCACHE_MAX_LEVELS]; /* Number of pages per level */
    uint32 level_bits[BCACHE_MAX_LEVELS];  /* Number of bits in each level */
    uint32 level_bytes[BCACHE_MAX_LEVELS]; /* Number of bytes in each level */
    uint32 nblocks;                        /* Total data blocks */
    uint32 data_start;                     /* First data block number */
    uint32 alloc_cursor;                   /* Rotating allocation pointer for wear leveling */
    uint32 free_count;                     /* Total number of free blocks */
    struct spinlock lock;                  /* Protect cache operations */
    int initialized;                       /* Cache is ready for use */
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
 * Mark a block as used in the cache
 * Called when a block is allocated
 */
void xv6fs_bcache_mark_used(struct xv6fs_superblock *xv6_sb, uint32 blockno);

/*
 * Find a free block using hierarchical search with wear leveling
 * Returns 0 on success with block number in blockno_out
 * Returns -ENOSPC if no free blocks available
 */
int xv6fs_bcache_find_free_block(struct xv6fs_superblock *xv6_sb, uint32 *blockno_out);

/*
 * Find a free block near a hint block for better locality
 * Searches in a window around the hint first, then falls back
 * to the regular wear-leveling search
 * Returns 0 on success, -ENOSPC if no free blocks
 */
int xv6fs_bcache_find_free_block_near(struct xv6fs_superblock *xv6_sb, 
                                       uint32 hint, uint32 *blockno_out);

/*
 * Get the number of free blocks
 */
uint32 xv6fs_bcache_free_count(struct xv6fs_superblock *xv6_sb);

#endif /* KERNEL_VFS_XV6FS_BLOCK_CACHE_H */
