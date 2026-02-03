/*
 * xv6fs block cache - buddy-like bitmap cache for free block tracking
 *
 * Provides O(log n) free block allocation with wear leveling and
 * locality-aware allocation for better file contiguity.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "lock/spinlock.h"
#include "dev/buf.h"
#include "vfs/fs.h"
#include "../vfs_private.h"
#include "block_cache.h"
#include "xv6fs_private.h"

/* Bitmap block calculation */
#define BBLOCK_PTR(b, sbp) ((b)/BPB + (sbp)->bmapstart)

/******************************************************************************
 * Bitmap helpers - multi-page aware
 ******************************************************************************/

/*
 * Get pointer to the byte containing a bit in a multi-page bitmap
 */
static inline uint8 *
bcache_get_byte_ptr(struct xv6fs_block_cache *bc, int level, uint32 bit_index)
{
    uint32 byte_index = bit_index / 8;
    uint32 page_index = byte_index / PGSIZE;
    uint32 page_offset = byte_index % PGSIZE;
    
    if (page_index >= bc->level_npages[level]) {
        return 0;
    }
    
    return bc->level_pages[level][page_index] + page_offset;
}

static inline int
bcache_get_bit_mp(struct xv6fs_block_cache *bc, int level, uint32 bit_index)
{
    uint8 *byte_ptr = bcache_get_byte_ptr(bc, level, bit_index);
    if (!byte_ptr) return 0;
    return (*byte_ptr & (1u << (bit_index & 7))) != 0;
}

static inline void
bcache_set_bit_mp(struct xv6fs_block_cache *bc, int level, uint32 bit_index)
{
    uint8 *byte_ptr = bcache_get_byte_ptr(bc, level, bit_index);
    if (byte_ptr) {
        *byte_ptr |= (uint8)(1u << (bit_index & 7));
    }
}

static inline void
bcache_clear_bit_mp(struct xv6fs_block_cache *bc, int level, uint32 bit_index)
{
    uint8 *byte_ptr = bcache_get_byte_ptr(bc, level, bit_index);
    if (byte_ptr) {
        *byte_ptr &= (uint8)~(1u << (bit_index & 7));
    }
}

/*
 * Check if a 64-bit group has any set bits (free blocks) - multi-page aware
 */
static int
bcache_group_has_free_mp(struct xv6fs_block_cache *bc, int level, uint32 group_start, uint32 max_bits)
{
    uint32 end = group_start + BCACHE_BITS_PER_LEVEL;
    if (end > max_bits) end = max_bits;
    
    for (uint32 i = group_start; i < end; i++) {
        if (bcache_get_bit_mp(bc, level, i)) {
            return 1;
        }
    }
    return 0;
}

/*
 * Update a parent level bit based on children
 */
static void
bcache_update_parent(struct xv6fs_block_cache *bc, int level, uint32 parent_index)
{
    if (level >= BCACHE_MAX_LEVELS - 1 || !bc->levels[level + 1]) {
        return;
    }
    
    uint32 child_start = parent_index * BCACHE_BITS_PER_LEVEL;
    int has_free = bcache_group_has_free_mp(bc, level, child_start, bc->level_bits[level]);
    
    if (has_free) {
        bcache_set_bit_mp(bc, level + 1, parent_index);
    } else {
        bcache_clear_bit_mp(bc, level + 1, parent_index);
    }
    
    /* Recursively update parent levels */
    bcache_update_parent(bc, level + 1, parent_index / BCACHE_BITS_PER_LEVEL);
}

/******************************************************************************
 * Block cache operations
 ******************************************************************************/

/*
 * Validate block number and convert to data index.
 * Returns -1 if invalid, otherwise returns the data index.
 */
static inline int
bcache_validate_block(struct xv6fs_block_cache *bc, uint32 blockno, uint32 *data_index_out)
{
    if (!bc->initialized || blockno < bc->data_start) {
        return -1;
    }
    
    uint32 data_index = blockno - bc->data_start;
    if (data_index >= bc->nblocks) {
        return -1;
    }
    
    *data_index_out = data_index;
    return 0;
}

/*
 * Mark a block as free in the cache
 */
void
xv6fs_bcache_mark_free(struct xv6fs_superblock *xv6_sb, uint32 blockno)
{
    struct xv6fs_block_cache *bc = &xv6_sb->block_cache;
    uint32 data_index;
    
    if (bcache_validate_block(bc, blockno, &data_index) < 0) {
        return;
    }
    
    spin_lock(&bc->lock);
    
    if (!bcache_get_bit_mp(bc, 0, data_index)) {
        bcache_set_bit_mp(bc, 0, data_index);
        bc->free_count++;
        bcache_update_parent(bc, 0, data_index / BCACHE_BITS_PER_LEVEL);
    }
    
    spin_unlock(&bc->lock);
}

/*
 * Mark a block as used in the cache
 */
void
xv6fs_bcache_mark_used(struct xv6fs_superblock *xv6_sb, uint32 blockno)
{
    struct xv6fs_block_cache *bc = &xv6_sb->block_cache;
    uint32 data_index;
    
    if (bcache_validate_block(bc, blockno, &data_index) < 0) {
        return;
    }
    
    spin_lock(&bc->lock);
    
    if (bcache_get_bit_mp(bc, 0, data_index)) {
        bcache_clear_bit_mp(bc, 0, data_index);
        if (bc->free_count > 0) bc->free_count--;
        bcache_update_parent(bc, 0, data_index / BCACHE_BITS_PER_LEVEL);
    }
    
    spin_unlock(&bc->lock);
}

/*
 * Find a free block using true hierarchical top-down search with wear leveling.
 * This achieves O(log n) search by starting from the highest level and drilling down.
 */
int
xv6fs_bcache_find_free_block(struct xv6fs_superblock *xv6_sb, uint32 *blockno_out)
{
    struct xv6fs_block_cache *bc = &xv6_sb->block_cache;
    
    if (!bc->initialized) {
        return -EINVAL;
    }
    
    spin_lock(&bc->lock);
    
    if (bc->free_count == 0) {
        spin_unlock(&bc->lock);
        return -ENOSPC;
    }
    
    /* Find the highest level with any bits set */
    int top_level = 0;
    for (int level = BCACHE_MAX_LEVELS - 1; level >= 0; level--) {
        if (bc->levels[level] && bc->level_bits[level] > 0) {
            top_level = level;
            break;
        }
    }
    
    /* Start searching from the alloc_cursor region for wear leveling */
    uint32 cursor_group = bc->alloc_cursor;
    
    /* Scale cursor to the top level */
    for (int level = 0; level < top_level; level++) {
        cursor_group = cursor_group / BCACHE_BITS_PER_LEVEL;
    }
    
    uint32 top_bits = bc->level_bits[top_level];
    
    /* Search at top level starting from cursor, wrapping around */
    for (uint32 i = 0; i < top_bits; i++) {
        uint32 top_idx = (cursor_group + i) % top_bits;
        
        if (!bcache_get_bit_mp(bc, top_level, top_idx)) {
            continue;
        }
        
        /* Found a group with free blocks - drill down to level 0 */
        uint32 block_idx = top_idx;
        
        for (int level = top_level - 1; level >= 0; level--) {
            /* Convert parent index to child range start */
            uint32 child_start = block_idx * BCACHE_BITS_PER_LEVEL;
            uint32 child_end = child_start + BCACHE_BITS_PER_LEVEL;
            if (child_end > bc->level_bits[level]) {
                child_end = bc->level_bits[level];
            }
            
            /* Find first free child */
            int found = 0;
            for (uint32 c = child_start; c < child_end; c++) {
                if (bcache_get_bit_mp(bc, level, c)) {
                    block_idx = c;
                    found = 1;
                    break;
                }
            }
            
            if (!found) {
                /* This shouldn't happen if parent bit was set correctly */
                break;
            }
        }
        
        /* block_idx is now a level-0 index (actual data block offset) */
        if (bcache_get_bit_mp(bc, 0, block_idx)) {
            *blockno_out = bc->data_start + block_idx;
            /* Advance cursor for wear leveling */
            bc->alloc_cursor = (block_idx + 1) % bc->nblocks;
            spin_unlock(&bc->lock);
            return 0;
        }
    }
    
    spin_unlock(&bc->lock);
    return -ENOSPC;
}

/*
 * Find a free block near a hint block for better locality
 */
int
xv6fs_bcache_find_free_block_near(struct xv6fs_superblock *xv6_sb, 
                                   uint32 hint, uint32 *blockno_out)
{
    struct xv6fs_block_cache *bc = &xv6_sb->block_cache;
    
    if (!bc->initialized) {
        return -EINVAL;
    }
    
    /* If hint is invalid, use regular search */
    if (hint < bc->data_start) {
        return xv6fs_bcache_find_free_block(xv6_sb, blockno_out);
    }
    
    uint32 hint_idx = hint - bc->data_start;
    if (hint_idx >= bc->nblocks) {
        return xv6fs_bcache_find_free_block(xv6_sb, blockno_out);
    }
    
    spin_lock(&bc->lock);
    
    if (bc->free_count == 0) {
        spin_unlock(&bc->lock);
        return -ENOSPC;
    }
    
    /* Search in a window around the hint for locality */
    #define LOCALITY_WINDOW 64
    uint32 n = bc->nblocks;
    
    /* First try blocks immediately after hint */
    for (uint32 i = 0; i < LOCALITY_WINDOW && hint_idx + i < n; i++) {
        uint32 idx = hint_idx + i;
        if (bcache_get_bit_mp(bc, 0, idx)) {
            *blockno_out = bc->data_start + idx;
            spin_unlock(&bc->lock);
            return 0;
        }
    }
    
    /* Try blocks before hint */
    for (uint32 i = 1; i < LOCALITY_WINDOW && i <= hint_idx; i++) {
        uint32 idx = hint_idx - i;
        if (bcache_get_bit_mp(bc, 0, idx)) {
            *blockno_out = bc->data_start + idx;
            spin_unlock(&bc->lock);
            return 0;
        }
    }
    
    #undef LOCALITY_WINDOW
    
    spin_unlock(&bc->lock);
    
    /* Fall back to regular wear-leveling search */
    return xv6fs_bcache_find_free_block(xv6_sb, blockno_out);
}

/*
 * Get the number of free blocks
 */
uint32
xv6fs_bcache_free_count(struct xv6fs_superblock *xv6_sb)
{
    struct xv6fs_block_cache *bc = &xv6_sb->block_cache;
    
    if (!bc->initialized) {
        return 0;
    }
    
    spin_lock(&bc->lock);
    uint32 count = bc->free_count;
    spin_unlock(&bc->lock);
    
    return count;
}

/*
 * Initialize the block cache from on-disk bitmap
 */
int
xv6fs_bcache_init(struct xv6fs_superblock *xv6_sb)
{
    struct xv6fs_block_cache *bc = &xv6_sb->block_cache;
    struct superblock *disk_sb = &xv6_sb->disk_sb;
    uint dev = xv6fs_sb_dev(xv6_sb);
    
    if (bc->initialized) {
        return 0;
    }
    
    /* Calculate data blocks: from data start to end of disk */
    uint32 data_start = disk_sb->bmapstart + (disk_sb->size + BPB - 1) / BPB;
    if (data_start > disk_sb->size) {
        /* Invalid superblock */
        return -EINVAL;
    }
    uint32 nblocks = disk_sb->size - data_start;
    
    spin_init(&bc->lock, "bcache");
    bc->nblocks = nblocks;
    bc->data_start = data_start;
    bc->alloc_cursor = 0;
    bc->free_count = 0;
    
    /* Initialize all levels to NULL first */
    for (int level = 0; level < BCACHE_MAX_LEVELS; level++) {
        bc->levels[level] = 0;
        bc->level_bits[level] = 0;
        bc->level_bytes[level] = 0;
        bc->level_npages[level] = 0;
        for (int p = 0; p < BCACHE_MAX_PAGES; p++) {
            bc->level_pages[level][p] = 0;
        }
    }
    
    /* Calculate sizes for each level and allocate */
    uint32 bits = nblocks;
    for (int level = 0; level < BCACHE_MAX_LEVELS; level++) {
        bc->level_bits[level] = bits;
        bc->level_bytes[level] = (bits + 7) / 8;
        
        uint32 npages = (bc->level_bytes[level] + PGSIZE - 1) / PGSIZE;
        
        /* Limit to max pages we can track */
        if (npages > BCACHE_MAX_PAGES) {
            npages = BCACHE_MAX_PAGES;
            bc->level_bytes[level] = npages * PGSIZE;
            bc->level_bits[level] = bc->level_bytes[level] * 8;
            bits = bc->level_bits[level];
        }
        
        bc->level_npages[level] = npages;
        
        if (npages > 0) {
            /* Allocate pages and create a contiguous virtual mapping would be ideal,
             * but for simplicity we use the first page as the main pointer and
             * handle multi-page access in the bit access functions.
             * For level 0, use kalloc for each page needed. */
            for (uint32 p = 0; p < npages; p++) {
                bc->level_pages[level][p] = kalloc();
                if (!bc->level_pages[level][p]) {
                    /* Cleanup on failure */
                    for (uint32 q = 0; q < p; q++) {
                        kfree(bc->level_pages[level][q]);
                        bc->level_pages[level][q] = 0;
                    }
                    for (int j = 0; j < level; j++) {
                        for (uint32 q = 0; q < bc->level_npages[j]; q++) {
                            if (bc->level_pages[j][q]) {
                                kfree(bc->level_pages[j][q]);
                            }
                        }
                    }
                    return -ENOMEM;
                }
                memset(bc->level_pages[level][p], 0, PGSIZE);
            }
            bc->levels[level] = bc->level_pages[level][0];
        } else {
            bc->levels[level] = 0;
            break;
        }
        
        bits = (bits + BCACHE_BITS_PER_LEVEL - 1) / BCACHE_BITS_PER_LEVEL;
        if (bits <= 1) {
            /* No need for more levels */
            break;
        }
    }
    
    /* Clamp nblocks to what we can actually track in level 0 */
    uint32 max_trackable = bc->level_bits[0];
    if (nblocks > max_trackable) {
        printf("xv6fs: block cache: clamping from %d to %d blocks\n", nblocks, max_trackable);
        nblocks = max_trackable;
        bc->nblocks = nblocks;
    }
    
    /* Populate level 0 from on-disk bitmap - read each bitmap block only once */
    uint32 last_bitmap_block = (uint32)-1;
    struct buf *bp = 0;
    
    for (uint32 b = 0; b < nblocks; b++) {
        uint32 blockno = data_start + b;
        uint32 bitmap_block = BBLOCK_PTR(blockno, disk_sb);
        
        /* Only read new bitmap block if different from last one */
        if (bitmap_block != last_bitmap_block) {
            if (bp) {
                brelse(bp);
            }
            bp = bread(dev, bitmap_block);
            if (!bp) {
                last_bitmap_block = (uint32)-1;
                continue;
            }
            last_bitmap_block = bitmap_block;
        }
        
        int bi = blockno % BPB;
        int m = 1 << (bi % 8);
        int used = (bp->data[bi/8] & m) != 0;
        
        if (!used) {
            bcache_set_bit_mp(bc, 0, b);
            bc->free_count++;
        }
    }
    
    /* Release last bitmap block */
    if (bp) {
        brelse(bp);
    }
    
    /* Build higher levels */
    for (int level = 0; level < BCACHE_MAX_LEVELS - 1; level++) {
        if (!bc->levels[level + 1]) break;
        
        uint32 parent_count = bc->level_bits[level + 1];
        for (uint32 p = 0; p < parent_count; p++) {
            uint32 child_start = p * BCACHE_BITS_PER_LEVEL;
            if (bcache_group_has_free_mp(bc, level, child_start, bc->level_bits[level])) {
                bcache_set_bit_mp(bc, level + 1, p);
            }
        }
    }
    
    bc->initialized = 1;
    printf("xv6fs: block cache initialized: %d data blocks (%d pages), %d free\n",
           nblocks, bc->level_npages[0], bc->free_count);
    
    return 0;
}

/*
 * Destroy the block cache and free memory
 */
void
xv6fs_bcache_destroy(struct xv6fs_superblock *xv6_sb)
{
    struct xv6fs_block_cache *bc = &xv6_sb->block_cache;
    
    if (!bc->initialized) {
        return;
    }
    
    spin_lock(&bc->lock);
    
    for (int level = 0; level < BCACHE_MAX_LEVELS; level++) {
        for (uint32 p = 0; p < bc->level_npages[level]; p++) {
            if (bc->level_pages[level][p]) {
                kfree(bc->level_pages[level][p]);
                bc->level_pages[level][p] = 0;
            }
        }
        bc->levels[level] = 0;
        bc->level_npages[level] = 0;
    }
    bc->initialized = 0;
    bc->free_count = 0;
    
    spin_unlock(&bc->lock);
}
