/**
 * @file buffer.c
 * @brief buffer_head implementation backed by the page cache.
 *
 * Replaces the legacy xv6 buffer cache (kernel/bio.c → bcache) with a
 * design that unifies metadata block I/O into the pcache/folio
 * infrastructure.
 *
 * Each xv6fs superblock owns a "metadata pcache" that caches disk blocks
 * in page-sized granularity.  A buffer_head is a lightweight, slab-allocated
 * descriptor that points into a specific BSIZE-byte region of a pcache
 * folio.  The pcache provides LRU eviction, dirty tracking, and
 * asynchronous writeback — the buffer_head only adds block-level
 * addressing on top.
 *
 * Block number mapping:
 *   xv6fs blockno  →  512-byte sector  =  blockno * (BSIZE / BLK_SIZE)
 *   pcache blkno (512-byte key)        =  blockno * (BSIZE / BLK_SIZE)
 *   offset within page                 =  (blockno % BLOCKS_PER_PAGE) * BSIZE
 *
 * where BLOCKS_PER_PAGE = PGSIZE / BSIZE = 4.
 */

#include <types.h>
#include <param.h>
#include <defs.h>
#include <string.h>
#include <printf.h>
#include <errno.h>
#include <riscv.h>
#include <lock/spinlock.h>
#include <lock/mutex_types.h>
#include <mm/page.h>
#include <mm/folio.h>
#include <mm/pcache.h>
#include <mm/slab.h>
#include <mm/buffer_head.h>
#include <dev/bio.h>
#include <dev/blkdev.h>
#include <vfs/xv6fs/ondisk.h>
#include <accounting.h>
#include <kstats.h>
#include <proc/thread.h>

/* Forward-declare the private xv6fs superblock type.
 * We only need its blkdev, disk_sb, and meta_pcache fields. */
struct xv6fs_superblock;

/* ── Include the xv6fs private header for the xv6fs_superblock layout ──── */
#include "kernel/vfs/xv6fs/xv6fs_private.h"

/* ── Constants ───────────────────────────────────────────────────────────── */

#define META_BLOCKS_PER_PAGE    (PGSIZE / BSIZE)        /* 4 */
#define META_BLK512_PER_BSIZE   (BSIZE / BLK_SIZE)      /* 2 */
#define META_BLKS512_PER_PAGE   (PGSIZE / BLK_SIZE)     /* 8 */

/* ── Global block I/O counters (shared with the old buffer cache) ──────── */

extern _Atomic uint64 g_bio_reads;
extern _Atomic uint64 g_bio_writes;
extern _Atomic uint64 g_bio_read_bytes;
extern _Atomic uint64 g_bio_write_bytes;

/* ── Slab cache for buffer_head_t ────────────────────────────────────────── */

static slab_cache_t __bh_slab = {0};

/* ======================================================================
 *  Metadata pcache ops — direct 1:1 block I/O (no bmap translation)
 * ====================================================================== */

/**
 * meta_pcache_read_page - read a page worth of metadata blocks from disk.
 *
 * pcnode->blkno is the page-aligned 512-byte sector number.  For metadata
 * the mapping is 1:1: sector N on the pcache equals sector N on disk.
 */
static int meta_pcache_read_page(struct pcache *pcache, page_t *page) {
    struct xv6fs_superblock *xv6_sb =
        (struct xv6fs_superblock *)pcache->private_data;
    if (xv6_sb == NULL || xv6_sb->blkdev == NULL)
        return -EIO;

    struct pcache_node *pcnode = page->pcache.pcache_node;
    uint64 sector = pcnode->blkno;        /* 512-byte sector */

    struct bio *bio = bio_alloc(xv6_sb->blkdev, 1, false, NULL, NULL);
    if (IS_ERR_OR_NULL(bio))
        return -ENOMEM;

    bio->blkno = sector;
    int ret = bio_add_folio(bio, page_folio(page), PGSIZE, 0);
    if (ret != 0) {
        bio_release(bio);
        return ret;
    }

    ret = blkdev_submit_bio(xv6_sb->blkdev, bio);
    if (ret == 0)
        ret = bio_await(bio);
    bio_release(bio);
    return ret;
}

/**
 * meta_pcache_write_page - write a page worth of metadata blocks to disk.
 */
static int meta_pcache_write_page(struct pcache *pcache, page_t *page) {
    struct xv6fs_superblock *xv6_sb =
        (struct xv6fs_superblock *)pcache->private_data;
    if (xv6_sb == NULL || xv6_sb->blkdev == NULL)
        return -EIO;

    struct pcache_node *pcnode = page->pcache.pcache_node;
    uint64 sector = pcnode->blkno;

    struct bio *bio = bio_alloc(xv6_sb->blkdev, 1, true, NULL, NULL);
    if (IS_ERR_OR_NULL(bio))
        return -ENOMEM;

    bio->blkno = sector;
    int ret = bio_add_folio(bio, page_folio(page), PGSIZE, 0);
    if (ret != 0) {
        bio_release(bio);
        return ret;
    }

    ret = blkdev_submit_bio(xv6_sb->blkdev, bio);
    if (ret == 0)
        ret = bio_await(bio);
    bio_release(bio);
    return ret;
}

static int meta_pcache_read_folio(struct pcache *pcache, folio_t *folio) {
    struct xv6fs_superblock *xv6_sb =
        (struct xv6fs_superblock *)pcache->private_data;
    if (xv6_sb == NULL || xv6_sb->blkdev == NULL)
        return -EIO;
    page_t *page = &folio->page;
    struct pcache_node *pcnode = page->pcache.pcache_node;

    struct bio *bio = bio_alloc(xv6_sb->blkdev, 1, false, NULL, NULL);
    if (IS_ERR_OR_NULL(bio))
        return -ENOMEM;
    bio->blkno = pcnode->blkno;
    int ret = bio_add_folio(bio, folio, pcnode->size, 0);
    if (ret != 0) { bio_release(bio); return ret; }
    ret = blkdev_submit_bio(xv6_sb->blkdev, bio);
    if (ret == 0) ret = bio_await(bio);
    bio_release(bio);
    return ret;
}

static int meta_pcache_write_folio(struct pcache *pcache, folio_t *folio) {
    struct xv6fs_superblock *xv6_sb =
        (struct xv6fs_superblock *)pcache->private_data;
    if (xv6_sb == NULL || xv6_sb->blkdev == NULL)
        return -EIO;
    page_t *page = &folio->page;
    struct pcache_node *pcnode = page->pcache.pcache_node;

    struct bio *bio = bio_alloc(xv6_sb->blkdev, 1, true, NULL, NULL);
    if (IS_ERR_OR_NULL(bio))
        return -ENOMEM;
    bio->blkno = pcnode->blkno;
    int ret = bio_add_folio(bio, folio, pcnode->size, 0);
    if (ret != 0) { bio_release(bio); return ret; }
    ret = blkdev_submit_bio(xv6_sb->blkdev, bio);
    if (ret == 0) ret = bio_await(bio);
    bio_release(bio);
    return ret;
}

/*
 * Non-blocking variant for batched flushing: create bio, submit with
 * batch flag, return bio for caller to await later.
 */
static struct bio *meta_pcache_submit_write_folio(struct pcache *pcache,
                                                   folio_t *folio) {
    struct xv6fs_superblock *xv6_sb =
        (struct xv6fs_superblock *)pcache->private_data;
    if (xv6_sb == NULL || xv6_sb->blkdev == NULL)
        return NULL;
    page_t *page = &folio->page;
    struct pcache_node *pcnode = page->pcache.pcache_node;

    struct bio *bio = bio_alloc(xv6_sb->blkdev, 1, true, NULL, NULL);
    if (IS_ERR_OR_NULL(bio))
        return NULL;
    bio->blkno = pcnode->blkno;
    bio->batch = 1;
    int ret = bio_add_folio(bio, folio, pcnode->size, 0);
    if (ret != 0) { bio_release(bio); return NULL; }
    ret = blkdev_submit_bio(xv6_sb->blkdev, bio);
    if (ret != 0) { bio_release(bio); return NULL; }
    return bio;
}

static struct pcache_ops meta_pcache_ops = {
    .read_page  = meta_pcache_read_page,
    .write_page = meta_pcache_write_page,
    .read_folio = meta_pcache_read_folio,
    .write_folio = meta_pcache_write_folio,
    .submit_write_folio = meta_pcache_submit_write_folio,
};

/* ======================================================================
 *  buffer_head slab helpers
 * ====================================================================== */

static buffer_head_t *__bh_alloc(void) {
    buffer_head_t *bh = slab_alloc(&__bh_slab);
    if (bh == NULL)
        return NULL;
    memset(bh, 0, sizeof(*bh));
    mutex_init(&bh->b_lock, "bh");
    return bh;
}

static void __bh_free(buffer_head_t *bh) {
    slab_free(bh);
}

/* ======================================================================
 *  Core API
 * ====================================================================== */

buffer_head_t *sb_bread(struct xv6fs_superblock *xv6_sb, uint blockno) {
    struct pcache *pc = xv6_sb->meta_pcache;

    /* Convert xv6fs block number to 512-byte sector for pcache key */
    uint64 blkno_512 = (uint64)blockno * META_BLK512_PER_BSIZE;

    /* Get (or allocate) the pcache page covering this block */
    folio_t *folio = pcache_get_folio(pc, blkno_512);
    if (folio == NULL) {
        printf("sb_bread: pcache_get_folio failed for block %u (blkno_512=%lu, "
               "blk_count=%lu, active=%d, private_data=%p)\n",
               blockno, blkno_512, pc->blk_count, pc->active, pc->private_data);
        return NULL;
    }

    /* Ensure data is read from disk */
    int ret = pcache_read_folio(pc, folio);
    if (ret != 0) {
        printf("sb_bread: pcache_read_folio failed for block %u (ret=%d)\n",
               blockno, ret);
        pcache_put_folio(pc, folio);
        return NULL;
    }

    /* Allocate a buffer_head */
    buffer_head_t *bh = __bh_alloc();
    if (bh == NULL) {
        pcache_put_folio(pc, folio);
        return NULL;
    }

    /* Compute byte offset within the folio.
     * The folio's pcn->blkno is the base 512-byte sector.
     * Our block starts at sector blkno_512, so the byte offset is:
     *   (blkno_512 - pcn->blkno) * BLK_SIZE
     */
    page_t *pcpage = &folio->page;
    struct pcache_node *pcn = pcpage->pcache.pcache_node;
    uint byte_offset = (uint)(blkno_512 - pcn->blkno) * BLK_SIZE;

    bh->b_blocknr = blockno;
    bh->b_size = BSIZE;
    bh->b_bdev = xv6fs_sb_dev(xv6_sb);
    bh->b_data = (uchar *)pcn->data + byte_offset;
    bh->b_folio = folio;
    bh->b_pcache = pc;

    /* Lock the buffer_head (caller-visible lock, matches bread semantics) */
    mutex_lock(&bh->b_lock);

    if (current && current->thread_group)
        ACCT_INC(current->thread_group, bio_reads);
    __atomic_fetch_add(&g_bio_reads, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_bio_read_bytes, BSIZE, __ATOMIC_RELAXED);

    return bh;
}

void bh_write(buffer_head_t *bh) {
    if (bh == NULL)
        return;

    if (current && current->thread_group)
        ACCT_INC(current->thread_group, bio_writes);
    __atomic_fetch_add(&g_bio_writes, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_bio_write_bytes, BSIZE, __ATOMIC_RELAXED);

    /* Mark the page dirty, then force a synchronous writeback of the
     * entire page.  This matches the old bwrite() semantics: the
     * caller's BSIZE block is guaranteed to be on disk when we return.
     *
     * pcache_mark_folio_dirty + pcache_flush achieves this.
     * However, pcache_flush writes ALL dirty pages, which may be too
     * broad.  Instead, we do a direct BIO write of just the page. */
    page_t *page = &bh->b_folio->page;
    struct pcache_node *pcn = page->pcache.pcache_node;

    struct xv6fs_superblock *xv6_sb =
        (struct xv6fs_superblock *)bh->b_pcache->private_data;
    blkdev_t *blkdev = xv6_sb->blkdev;

    struct bio *bio = bio_alloc(blkdev, 1, true, NULL, NULL);
    if (IS_ERR_OR_NULL(bio)) {
        /* Best effort — should not happen for metadata writes */
        return;
    }
    bio->blkno = pcn->blkno;   /* 512-byte sector of the page */
    int ret = bio_add_folio(bio, page_folio(page), PGSIZE, 0);
    if (ret != 0) {
        bio_release(bio);
        return;
    }
    blkdev_submit_bio(blkdev, bio);
    bio_await(bio);
    bio_release(bio);

    /* Clear dirty state on the pcache page if it was marked dirty */
    /* (pcache_mark_page_dirty may have added it to the dirty list) */
}

void bh_write_async(buffer_head_t *bh) {
    if (bh == NULL)
        return;

    if (current && current->thread_group)
        ACCT_INC(current->thread_group, bio_writes);
    __atomic_fetch_add(&g_bio_writes, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_bio_write_bytes, BSIZE, __ATOMIC_RELAXED);

    /* Mark the backing folio dirty for deferred writeback */
    pcache_mark_folio_dirty(bh->b_pcache, bh->b_folio);
}

void bh_release(buffer_head_t *bh) {
    if (bh == NULL)
        return;

    mutex_unlock(&bh->b_lock);

    /* Drop the pcache folio reference obtained by sb_bread */
    pcache_put_folio(bh->b_pcache, bh->b_folio);

    bh->b_data = NULL;
    bh->b_folio = NULL;
    bh->b_pcache = NULL;

    __bh_free(bh);
}

void bh_sync(struct xv6fs_superblock *xv6_sb) {
    pcache_flush(xv6_sb->meta_pcache);
}

void bh_pin(buffer_head_t *bh) {
    if (bh == NULL)
        return;
    /* Increment the pcache folio reference to prevent LRU eviction.
     * This matches the old bpin() semantics. */
    folio_get(bh->b_folio);
}

void bh_unpin(buffer_head_t *bh) {
    if (bh == NULL)
        return;
    /* Drop the pin reference.  If the ref drops to the pcache-only
     * level, the page becomes eligible for LRU reclaim again. */
    pcache_put_folio(bh->b_pcache, bh->b_folio);
}

uint bh_dirty_count(struct xv6fs_superblock *xv6_sb) {
    return xv6_sb->meta_pcache ? (uint)xv6_sb->meta_pcache->dirty_count : 0;
}

/* ======================================================================
 *  Metadata pcache lifecycle
 * ====================================================================== */

void meta_pcache_init(struct xv6fs_superblock *xv6_sb) {
    struct pcache *pc = kalloc();
    if (pc == NULL)
        panic("meta_pcache_init: kalloc failed");
    memset(pc, 0, sizeof(*pc));
    xv6_sb->meta_pcache = pc;

    pc->ops = &meta_pcache_ops;
    /* blk_count: total 512-byte sectors on the device */
    pc->blk_count = ((uint64)xv6_sb->disk_sb.size * META_BLK512_PER_BSIZE +
                     META_BLKS512_PER_PAGE - 1) &
                    ~(uint64)(META_BLKS512_PER_PAGE - 1);
    pc->gfp_flags = 0;

    int ret = pcache_init(pc);
    if (ret != 0)
        panic("meta_pcache_init: pcache_init failed");

    /* pcache_init resets private_data, so set it after init */
    pc->private_data = xv6_sb;
}

void meta_pcache_destroy(struct xv6fs_superblock *xv6_sb) {
    if (xv6_sb->meta_pcache == NULL)
        return;
    /* Flush any outstanding dirty pages before teardown */
    pcache_flush(xv6_sb->meta_pcache);
    /* Free the dynamically allocated pcache struct. */
    kfree(xv6_sb->meta_pcache);
    xv6_sb->meta_pcache = NULL;
}

/* ======================================================================
 *  Global init (replaces binit)
 * ====================================================================== */

void bh_global_init(void) {
    int ret = slab_cache_init(&__bh_slab, "buffer_head",
                              sizeof(buffer_head_t), 0);
    if (ret)
        panic("bh_global_init: slab_cache_init failed");
}
