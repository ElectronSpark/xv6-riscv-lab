/*
 * xv6fs block cache - Red-Black Tree based free extent tracking
 *
 * Provides O(log n) free block allocation using an rb-tree of free extents.
 * Extents are merged when adjacent blocks are freed, reducing fragmentation
 * and allowing efficient allocation of contiguous blocks for large files.
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
#include "bintree.h"
#include "rbtree.h"

/* Bitmap block calculation for reading on-disk bitmap */
#define BBLOCK_PTR(b, sbp) ((b) / BPB + (sbp)->bmapstart)

/******************************************************************************
 * Red-Black Tree callbacks for insertion/deletion
 ******************************************************************************/

/*
 * Compare two extent keys by start block number.
 */
static int extent_keys_cmp(uint64 key1, uint64 key2) {
    struct free_extent *ext1 = (struct free_extent *)key1;
    struct free_extent *ext2 = (struct free_extent *)key2;

    if (ext1->start < ext2->start)
        return -1;
    if (ext1->start > ext2->start)
        return 1;
    /* Equal starts - use address to distinguish */
    if (key1 < key2)
        return -1;
    if (key1 > key2)
        return 1;
    return 0;
}

/*
 * Get key from rb_node - returns extent pointer as key
 */
static uint64 extent_get_key(struct rb_node *node) {
    struct free_extent *ext = container_of(node, struct free_extent, rb_node);
    return (uint64)ext;
}

/******************************************************************************
 * Extent allocation helpers
 ******************************************************************************/

static struct free_extent *bcache_alloc_extent(struct xv6fs_block_cache *bc) {
    struct free_extent *ext = slab_alloc(&bc->extent_cache);
    if (ext) {
        memset(ext, 0, sizeof(*ext));
    }
    return ext;
}

static void bcache_free_extent(struct xv6fs_block_cache *bc,
                               struct free_extent *ext) {
    slab_free(ext);
}

/******************************************************************************
 * Internal tree operations (caller must hold lock)
 *
 * Simple manual traversal for O(log n) searches.
 ******************************************************************************/

/*
 * Find the extent with start <= blockno (floor search).
 * Returns NULL if no such extent exists.
 */
static struct free_extent *bcache_find_extent_le(struct xv6fs_block_cache *bc,
                                                 uint32 blockno) {
    struct rb_node *node = bc->extent_tree.node;
    struct free_extent *best = 0;

    while (node) {
        struct free_extent *ext =
            container_of(node, struct free_extent, rb_node);
        if (ext->start <= blockno) {
            best = ext;
            node = node->right;
        } else {
            node = node->left;
        }
    }
    return best;
}

/*
 * Find the extent with start >= blockno (ceiling search).
 * Returns NULL if no such extent exists.
 */
static struct free_extent *bcache_find_extent_ge(struct xv6fs_block_cache *bc,
                                                 uint32 blockno) {
    struct rb_node *node = bc->extent_tree.node;
    struct free_extent *best = 0;

    while (node) {
        struct free_extent *ext =
            container_of(node, struct free_extent, rb_node);
        if (ext->start >= blockno) {
            best = ext;
            node = node->left;
        } else {
            node = node->right;
        }
    }
    return best;
}

/*
 * Find extent containing a specific block (block is within extent range)
 */
static struct free_extent *
bcache_find_extent_containing(struct xv6fs_block_cache *bc, uint32 blockno) {
    struct free_extent *ext = bcache_find_extent_le(bc, blockno);
    /* ext->start <= blockno is guaranteed by bcache_find_extent_le */
    if (ext && blockno < ext->start + ext->length) {
        return ext;
    }
    return 0;
}

/*
 * Insert a new extent into the tree.
 * Attempts to merge with adjacent extents.
 */
static void bcache_insert_extent(struct xv6fs_block_cache *bc, uint32 start,
                                 uint32 length) {
    struct free_extent *prev, *next;
    uint32 end = start + length;

    /* Check for merge with previous extent */
    prev = bcache_find_extent_le(bc, start);
    if (prev && prev->start + prev->length == start) {
        /* Merge with previous: extend it */
        prev->length += length;
        bc->free_count += length;

        /* Check if we can also merge with next */
        struct rb_node *next_node = rb_next_node(&prev->rb_node);
        if (next_node) {
            next = container_of(next_node, struct free_extent, rb_node);
            if (prev->start + prev->length == next->start) {
                /* Merge all three into prev */
                prev->length += next->length;
                rb_delete_node_color(&bc->extent_tree, &next->rb_node);
                bcache_free_extent(bc, next);
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
        rb_delete_node_color(&bc->extent_tree, &next->rb_node);
        next->start = start;
        next->length += length;
        rb_insert_color(&bc->extent_tree, &next->rb_node);
        bc->free_count += length;
        return;
    }

    /* No merge possible, create new extent */
    struct free_extent *ext = bcache_alloc_extent(bc);
    if (!ext) {
        /* Out of memory - silently fail (cache is optimization only) */
        return;
    }

    ext->start = start;
    ext->length = length;
    rb_insert_color(&bc->extent_tree, &ext->rb_node);
    bc->extent_count++;
    bc->free_count += length;
}

/*
 * Allocate one block from an extent, preferring the END for efficiency.
 * Allocating from the end only requires decrementing length (O(1)),
 * whereas allocating from the start requires re-keying the tree (O(log n)).
 */
static uint32 bcache_alloc_from_extent(struct xv6fs_block_cache *bc,
                                       struct free_extent *ext) {
    /* Allocate from the end of the extent */
    uint32 blockno = ext->start + ext->length - 1;

    if (ext->length == 1) {
        /* Remove entire extent */
        rb_delete_node_color(&bc->extent_tree, &ext->rb_node);
        bcache_free_extent(bc, ext);
        bc->extent_count--;
    } else {
        /* Simply shrink from the end - no key change, O(1) */
        ext->length--;
    }

    bc->free_count--;
    return blockno;
}

/******************************************************************************
 * Public API
 ******************************************************************************/

/*
 * Mark a block as free in the cache
 */
void xv6fs_bcache_mark_free(struct xv6fs_superblock *xv6_sb, uint32 blockno) {
    struct xv6fs_block_cache *bc = &xv6_sb->block_cache;

    if (!bc->initialized || blockno < bc->data_start) {
        return;
    }

    spin_lock(&bc->lock);
    bcache_insert_extent(bc, blockno, 1);
    spin_unlock(&bc->lock);
}

/*
 * Find a free block using rb-tree search with wear leveling.
 * O(log n) search starting from allocation cursor.
 */
int xv6fs_bcache_find_free_block(struct xv6fs_superblock *xv6_sb,
                                 uint32 *blockno_out) {
    struct xv6fs_block_cache *bc = &xv6_sb->block_cache;

    if (!bc->initialized) {
        return -EINVAL;
    }

    spin_lock(&bc->lock);

    if (bc->free_count == 0 || rb_root_is_empty(&bc->extent_tree)) {
        spin_unlock(&bc->lock);
        return -ENOSPC;
    }

    /* Find extent at or after cursor for wear leveling */
    struct free_extent *ext = bcache_find_extent_ge(bc, bc->alloc_cursor);

    /* Wrap around if no extent found after cursor */
    if (!ext) {
        ext = container_of(rb_first_node(&bc->extent_tree), struct free_extent,
                           rb_node);
    }
    /* ext is guaranteed non-NULL since we checked tree is not empty above */

    *blockno_out = bcache_alloc_from_extent(bc, ext);
    bc->alloc_cursor = *blockno_out + 1;
    if (bc->alloc_cursor >= bc->data_start + bc->nblocks) {
        bc->alloc_cursor = bc->data_start;
    }

    spin_unlock(&bc->lock);
    return 0;
}

/*
 * Find a free block near a hint block for better locality.
 * Uses O(log n) rb-tree search - no linear fallback paths.
 */
int xv6fs_bcache_find_free_block_near(struct xv6fs_superblock *xv6_sb,
                                      uint32 hint, uint32 *blockno_out) {
    struct xv6fs_block_cache *bc = &xv6_sb->block_cache;
    struct free_extent *ext;

    if (!bc->initialized) {
        return -EINVAL;
    }

    spin_lock(&bc->lock);

    if (bc->free_count == 0 || rb_root_is_empty(&bc->extent_tree)) {
        spin_unlock(&bc->lock);
        return -ENOSPC;
    }

    /* Clamp hint to valid range */
    if (hint < bc->data_start) {
        hint = bc->data_start;
    } else if (hint >= bc->data_start + bc->nblocks) {
        hint = bc->data_start + bc->nblocks - 1;
    }

    /* Try to find extent containing the hint - O(log n) */
    ext = bcache_find_extent_containing(bc, hint);
    if (ext) {
        *blockno_out = bcache_alloc_from_extent(bc, ext);
        spin_unlock(&bc->lock);
        return 0;
    }

    /* Find extent at or after hint - O(log n) */
    ext = bcache_find_extent_ge(bc, hint);
    if (ext) {
        *blockno_out = bcache_alloc_from_extent(bc, ext);
        spin_unlock(&bc->lock);
        return 0;
    }

    /* No extent at/after hint means all extents are before hint.
     * The last (largest key) extent is closest to hint - O(log n) */
    ext = container_of(rb_last_node(&bc->extent_tree), struct free_extent,
                       rb_node);
    *blockno_out = bcache_alloc_from_extent(bc, ext);
    spin_unlock(&bc->lock);
    return 0;
}

/*
 * Get the number of free blocks
 */
uint32 xv6fs_bcache_free_count(struct xv6fs_superblock *xv6_sb) {
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
int xv6fs_bcache_init(struct xv6fs_superblock *xv6_sb) {
    struct xv6fs_block_cache *bc = &xv6_sb->block_cache;
    struct superblock *disk_sb = &xv6_sb->disk_sb;
    uint dev = xv6fs_sb_dev(xv6_sb);

    if (bc->initialized) {
        return 0;
    }

    /* Calculate data blocks */
    uint32 data_start = disk_sb->bmapstart + (disk_sb->size + BPB - 1) / BPB;
    if (data_start > disk_sb->size) {
        return -EINVAL;
    }
    uint32 nblocks = disk_sb->size - data_start;

    /* Initialize basic fields */
    spin_init(&bc->lock, "bcache");
    bc->nblocks = nblocks;
    bc->data_start = data_start;
    bc->alloc_cursor = data_start;
    bc->free_count = 0;
    bc->extent_count = 0;

    /* Initialize slab cache for extents */
    slab_cache_init(&bc->extent_cache, "bcache_extent",
                    sizeof(struct free_extent), 0);

    /* Initialize rb-tree */
    bc->tree_opts.keys_cmp_fun = extent_keys_cmp;
    bc->tree_opts.get_key_fun = extent_get_key;
    rb_root_init(&bc->extent_tree, &bc->tree_opts);

    /* Scan on-disk bitmap and build extent tree */
    uint32 last_bitmap_block = (uint32)-1;
    struct buf *bp = 0;

    uint32 run_start = 0;
    uint32 run_length = 0;
    int in_run = 0;

    for (uint32 b = 0; b < nblocks; b++) {
        uint32 blockno = data_start + b;
        uint32 bitmap_block = BBLOCK_PTR(blockno, disk_sb);

        if (bitmap_block != last_bitmap_block) {
            if (bp) {
                brelse(bp);
            }
            bp = bread(dev, bitmap_block);
            if (!bp) {
                last_bitmap_block = (uint32)-1;
                /* Treat read errors as used blocks */
                if (in_run) {
                    bcache_insert_extent(bc, run_start, run_length);
                    in_run = 0;
                }
                continue;
            }
            last_bitmap_block = bitmap_block;
        }

        int bi = blockno % BPB;
        int m = 1 << (bi % 8);
        int used = (bp->data[bi / 8] & m) != 0;

        if (!used) {
            /* Block is free */
            if (in_run) {
                run_length++;
            } else {
                run_start = blockno;
                run_length = 1;
                in_run = 1;
            }
        } else {
            /* Block is used */
            if (in_run) {
                bcache_insert_extent(bc, run_start, run_length);
                in_run = 0;
            }
        }
    }

    /* Flush any remaining run */
    if (in_run) {
        bcache_insert_extent(bc, run_start, run_length);
    }

    if (bp) {
        brelse(bp);
    }

    bc->initialized = 1;
    printf("xv6fs: block cache initialized: %d data blocks, %d free in %d "
           "extents\n",
           nblocks, bc->free_count, bc->extent_count);

    return 0;
}

/*
 * Destroy the block cache and free memory
 */
void xv6fs_bcache_destroy(struct xv6fs_superblock *xv6_sb) {
    struct xv6fs_block_cache *bc = &xv6_sb->block_cache;

    if (!bc->initialized) {
        return;
    }

    spin_lock(&bc->lock);

    /* Free all extents */
    struct rb_node *node = rb_first_node(&bc->extent_tree);
    while (node) {
        struct free_extent *ext =
            container_of(node, struct free_extent, rb_node);
        node = rb_next_node(node);
        rb_delete_node_color(&bc->extent_tree, &ext->rb_node);
        bcache_free_extent(bc, ext);
    }

    bc->initialized = 0;
    bc->free_count = 0;
    bc->extent_count = 0;

    spin_unlock(&bc->lock);

    /* Note: slab cache memory will be reclaimed automatically */
}
