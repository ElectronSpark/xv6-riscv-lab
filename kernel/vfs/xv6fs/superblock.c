/*
 * xv6fs superblock operations
 *
 * Handles mounting, syncing, and managing the xv6 filesystem superblock.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "cmdline.h"
#include "lock/spinlock.h"
#include "lock/mutex_types.h"
#include <mm/vm.h>
#include "dev/buf.h"
#include "dev/blkdev.h"
#include "vfs/fs.h"
#include "../vfs_private.h"
#include <mm/slab.h>
#include <mm/pcache.h>
#include <mm/folio.h>
#include <mm/buffer_head.h>
#include "dev/bio.h"
#include "xv6fs_private.h"
#include "block_cache.h"
#include "xv6fs_smoketest.h"

// Slab caches for xv6fs structures
slab_cache_t __xv6fs_inode_cache;
static slab_cache_t __xv6fs_sb_cache;

/******************************************************************************
 * Per-inode page cache operations for file data
 *
 * The pcache is keyed by logical file offset in 512-byte units.
 * One pcache page (4KB) covers BSIZE_PER_PAGE (4) xv6fs blocks.
 * read_page translates logical block → physical via bmap, then reads via bio.
 * write_page translates logical block → physical via bmap, then writes via bio
 * (data=writeback: data blocks bypass the xv6fs log).
 ******************************************************************************/

#define BSIZE_PER_PAGE (PGSIZE / BSIZE) /* 4 */
#define BLK512_PER_BSIZE (BSIZE / 512)  /* 2 */

static int xv6fs_pcache_read_page(struct pcache *pcache, page_t *page) {
    struct vfs_inode *inode = (struct vfs_inode *)pcache->private_data;
    struct xv6fs_inode *ip = container_of(inode, struct xv6fs_inode, vfs_inode);
    struct xv6fs_superblock *xv6_sb =
        container_of(inode->sb, struct xv6fs_superblock, vfs_sb);
    struct pcache_node *pcnode = page->pcache.pcache_node;

    /* pcnode->blkno is the page-aligned logical offset in 512-byte units */
    uint base_bn = pcnode->blkno / BLK512_PER_BSIZE; /* first xv6fs block */

    /*
     * Resolve all block addresses first, then merge contiguous runs into
     * single bios.  For a non-fragmented file the entire 4 KB page becomes
     * one DMA transfer instead of four.
     */
    uint addrs[BSIZE_PER_PAGE];
    for (int i = 0; i < BSIZE_PER_PAGE; i++)
        addrs[i] = xv6fs_bmap_read(ip, base_bn + i);

    struct bio *bios[BSIZE_PER_PAGE] = {NULL};
    int submitted = 0;
    int ret = 0;

    for (int i = 0; i < BSIZE_PER_PAGE; ) {
        if (addrs[i] == 0) {
            /* Sparse / beyond file — zero-fill this 1 KB slot */
            memset((char *)pcnode->data + i * BSIZE, 0, BSIZE);
            i++;
            continue;
        }

        /* Find the longest contiguous run starting at slot i */
        int run = 1;
        while (i + run < BSIZE_PER_PAGE &&
               addrs[i + run] == addrs[i] + run)
            run++;

        struct bio *bio =
            bio_alloc(xv6_sb->blkdev, 1, false, NULL, NULL);
        if (IS_ERR_OR_NULL(bio)) {
            ret = -ENOMEM;
            goto await_submitted;
        }

        bio->blkno = (uint64)addrs[i] * BLK512_PER_BSIZE;
        ret = bio_add_folio(bio, page_folio(page), run * BSIZE, i * BSIZE);
        if (ret != 0) {
            bio_release(bio);
            goto await_submitted;
        }

        ret = blkdev_submit_bio(xv6_sb->blkdev, bio);
        if (ret != 0) {
            bio_release(bio);
            goto await_submitted;
        }

        bios[submitted++] = bio;
        i += run;
    }

    ret = 0;

await_submitted:
    /* Wait for all in-flight bios — must complete even on early error */
    for (int i = 0; i < submitted; i++) {
        int err = bio_await(bios[i]);
        if (err != 0 && ret == 0)
            ret = err;
        bio_release(bios[i]);
    }

    return ret;
}

static int xv6fs_pcache_write_page(struct pcache *pcache, page_t *page) {
    struct vfs_inode *inode = (struct vfs_inode *)pcache->private_data;

    /*
     * The flush worker may run concurrently with inode teardown.  If the
     * inode has already been detached from its superblock (inode->sb == NULL),
     * skip the writeback — the data is about to be truncated anyway.
     */
    if (inode == NULL || inode->sb == NULL)
        return 0;

    struct xv6fs_inode *ip = container_of(inode, struct xv6fs_inode, vfs_inode);
    struct xv6fs_superblock *xv6_sb =
        container_of(inode->sb, struct xv6fs_superblock, vfs_sb);
    struct pcache_node *pcnode = page->pcache.pcache_node;

    /* pcnode->blkno is the page-aligned logical offset in 512-byte units */
    uint base_bn = pcnode->blkno / BLK512_PER_BSIZE;

    /*
     * Resolve all block addresses first, then merge contiguous runs into
     * single bios.  For a non-fragmented file the entire 4 KB page becomes
     * one DMA transfer instead of four.
     */
    uint addrs[BSIZE_PER_PAGE];
    for (int i = 0; i < BSIZE_PER_PAGE; i++)
        addrs[i] = xv6fs_bmap_read(ip, base_bn + i);

    struct bio *bios[BSIZE_PER_PAGE] = {NULL};
    int submitted = 0;
    int ret = 0;

    for (int i = 0; i < BSIZE_PER_PAGE; ) {
        if (addrs[i] == 0) {
            i++; /* Sparse / beyond file — nothing to write back */
            continue;
        }

        /* Find the longest contiguous run starting at slot i */
        int run = 1;
        while (i + run < BSIZE_PER_PAGE &&
               addrs[i + run] == addrs[i] + run)
            run++;

        struct bio *bio =
            bio_alloc(xv6_sb->blkdev, 1, true, NULL, NULL);
        if (IS_ERR_OR_NULL(bio)) {
            ret = -ENOMEM;
            goto await_submitted;
        }

        bio->blkno = (uint64)addrs[i] * BLK512_PER_BSIZE;
        ret = bio_add_folio(bio, page_folio(page), run * BSIZE, i * BSIZE);
        if (ret != 0) {
            bio_release(bio);
            goto await_submitted;
        }

        ret = blkdev_submit_bio(xv6_sb->blkdev, bio);
        if (ret != 0) {
            bio_release(bio);
            goto await_submitted;
        }

        bios[submitted++] = bio;
        i += run;
    }

    ret = 0;

await_submitted:
    /* Wait for all in-flight bios — must complete even on early error */
    for (int i = 0; i < submitted; i++) {
        int err = bio_await(bios[i]);
        if (err != 0 && ret == 0)
            ret = err;
        bio_release(bios[i]);
    }

    return ret;
}

static int xv6fs_pcache_read_folio(struct pcache *pcache, folio_t *folio) {
    page_t *page = &folio->page;
    struct vfs_inode *inode = (struct vfs_inode *)pcache->private_data;
    struct xv6fs_inode *ip = container_of(inode, struct xv6fs_inode, vfs_inode);
    struct xv6fs_superblock *xv6_sb =
        container_of(inode->sb, struct xv6fs_superblock, vfs_sb);
    struct pcache_node *pcnode = page->pcache.pcache_node;

    /* Number of xv6fs-sized blocks (BSIZE) in the entire folio. */
    int folio_nblk = pcnode->size / BSIZE;
    uint base_bn = pcnode->blkno / BLK512_PER_BSIZE;

    /* Resolve all block addresses, then merge contiguous runs. */
    uint addrs[FOLIO_MAX_ORDER_NR_PAGES * BSIZE_PER_PAGE];
    for (int i = 0; i < folio_nblk; i++)
        addrs[i] = xv6fs_bmap_read(ip, base_bn + i);

    struct bio *bios[FOLIO_MAX_ORDER_NR_PAGES * BSIZE_PER_PAGE] = {NULL};
    int submitted = 0;
    int ret = 0;

    for (int i = 0; i < folio_nblk; ) {
        if (addrs[i] == 0) {
            memset((char *)pcnode->data + i * BSIZE, 0, BSIZE);
            i++;
            continue;
        }
        int run = 1;
        while (i + run < folio_nblk && addrs[i + run] == addrs[i] + run)
            run++;

        struct bio *bio = bio_alloc(xv6_sb->blkdev, 1, false, NULL, NULL);
        if (IS_ERR_OR_NULL(bio)) { ret = -ENOMEM; goto await_submitted; }

        bio->blkno = (uint64)addrs[i] * BLK512_PER_BSIZE;
        ret = bio_add_folio(bio, folio, run * BSIZE, i * BSIZE);
        if (ret != 0) { bio_release(bio); goto await_submitted; }

        ret = blkdev_submit_bio(xv6_sb->blkdev, bio);
        if (ret != 0) { bio_release(bio); goto await_submitted; }
        bios[submitted++] = bio;
        i += run;
    }
    ret = 0;

await_submitted:
    for (int i = 0; i < submitted; i++) {
        int err = bio_await(bios[i]);
        if (err != 0 && ret == 0) ret = err;
        bio_release(bios[i]);
    }
    return ret;
}

static int xv6fs_pcache_write_folio(struct pcache *pcache, folio_t *folio) {
    page_t *page = &folio->page;
    struct vfs_inode *inode = (struct vfs_inode *)pcache->private_data;
    if (inode == NULL || inode->sb == NULL)
        return 0;

    struct xv6fs_inode *ip = container_of(inode, struct xv6fs_inode, vfs_inode);
    struct xv6fs_superblock *xv6_sb =
        container_of(inode->sb, struct xv6fs_superblock, vfs_sb);
    struct pcache_node *pcnode = page->pcache.pcache_node;

    int folio_nblk = pcnode->size / BSIZE;
    uint base_bn = pcnode->blkno / BLK512_PER_BSIZE;

    uint addrs[FOLIO_MAX_ORDER_NR_PAGES * BSIZE_PER_PAGE];
    for (int i = 0; i < folio_nblk; i++)
        addrs[i] = xv6fs_bmap_read(ip, base_bn + i);

    struct bio *bios[FOLIO_MAX_ORDER_NR_PAGES * BSIZE_PER_PAGE] = {NULL};
    int submitted = 0;
    int ret = 0;

    for (int i = 0; i < folio_nblk; ) {
        if (addrs[i] == 0) { i++; continue; }
        int run = 1;
        while (i + run < folio_nblk && addrs[i + run] == addrs[i] + run)
            run++;

        struct bio *bio = bio_alloc(xv6_sb->blkdev, 1, true, NULL, NULL);
        if (IS_ERR_OR_NULL(bio)) { ret = -ENOMEM; goto await_submitted; }

        bio->blkno = (uint64)addrs[i] * BLK512_PER_BSIZE;
        ret = bio_add_folio(bio, folio, run * BSIZE, i * BSIZE);
        if (ret != 0) { bio_release(bio); goto await_submitted; }

        ret = blkdev_submit_bio(xv6_sb->blkdev, bio);
        if (ret != 0) { bio_release(bio); goto await_submitted; }
        bios[submitted++] = bio;
        i += run;
    }
    ret = 0;

await_submitted:
    for (int i = 0; i < submitted; i++) {
        int err = bio_await(bios[i]);
        if (err != 0 && ret == 0) ret = err;
        bio_release(bios[i]);
    }
    return ret;
}

/******************************************************************************
 * Batch I/O helper: submit folio reads without awaiting
 *
 * Resolves bmap for all blocks in the folio, creates merged bios for
 * contiguous disk runs, and submits them.  Bios are returned to the
 * caller (in bios[]) so they can be batch-awaited later.  This enables
 * multiple folios' I/O to be in-flight simultaneously.
 ******************************************************************************/

int xv6fs_submit_folio_reads(struct xv6fs_inode *ip,
                             struct xv6fs_superblock *xv6_sb,
                             folio_t *folio, struct bio **bios, int max_bios,
                             int *n_submitted) {
    page_t *page = &folio->page;
    struct pcache_node *pcnode = page->pcache.pcache_node;
    int folio_nblk = pcnode->size / BSIZE;
    uint base_bn = pcnode->blkno / BLK512_PER_BSIZE;
    int ret = 0;

    /* Resolve all block addresses */
    uint addrs[FOLIO_MAX_ORDER_NR_PAGES * BSIZE_PER_PAGE];
    for (int i = 0; i < folio_nblk; i++)
        addrs[i] = xv6fs_bmap_read(ip, base_bn + i);

    /* Merge contiguous runs into bios and submit without awaiting */
    for (int i = 0; i < folio_nblk; ) {
        if (addrs[i] == 0) {
            memset((char *)pcnode->data + i * BSIZE, 0, BSIZE);
            i++;
            continue;
        }
        int run = 1;
        while (i + run < folio_nblk && addrs[i + run] == addrs[i] + run)
            run++;

        if (*n_submitted >= max_bios) { ret = -ENOMEM; break; }

        struct bio *bio = bio_alloc(xv6_sb->blkdev, 1, false, NULL, NULL);
        if (IS_ERR_OR_NULL(bio)) { ret = -ENOMEM; break; }

        bio->blkno = (uint64)addrs[i] * BLK512_PER_BSIZE;
        bio->batch = 1; /* caller will kick the device after all bios */
        int r = bio_add_folio(bio, folio, run * BSIZE, i * BSIZE);
        if (r != 0) { bio_release(bio); ret = r; break; }

        r = blkdev_submit_bio(xv6_sb->blkdev, bio);
        if (r != 0) { bio_release(bio); ret = r; break; }

        bios[(*n_submitted)++] = bio;
        i += run;
    }
    return ret;
}

/******************************************************************************
 * Cross-folio merged I/O: submit reads for multiple folios as few bios.
 *
 * For a batch of non-uptodate folios, this function:
 *   1. Resolves the first and last physical disk blocks of each folio
 *      (2 bmap calls per folio) to detect internal + cross-folio contiguity.
 *   2. Groups consecutive contiguous folios into runs.
 *   3. Creates ONE bio per run with multiple bio_vecs (one per folio),
 *      submitted as a single virtio scatter-gather request for drastically
 *      reduced per-batch overhead.
 *   4. Falls back to per-folio xv6fs_submit_folio_reads for fragmented folios.
 ******************************************************************************/

int xv6fs_submit_merged_folio_reads(struct xv6fs_inode *ip,
                                    struct xv6fs_superblock *xv6_sb,
                                    folio_t **folios, int n_folios,
                                    struct bio **bios, int max_bios,
                                    int *n_submitted) {
    if (n_folios == 0)
        return 0;

    /* Per-folio disk block info (stack: ~12 bytes × 32 = 384 bytes). */
    struct {
        uint first_addr; /* first physical disk block (BSIZE units) */
        uint last_addr;  /* last  physical disk block (BSIZE units) */
        int  nblk;       /* blocks in this folio (BSIZE units) */
        bool contig;     /* all blocks within folio are contiguous */
    } fi[n_folios]; /* VLA */

    /* Step 1: resolve boundary blocks for each folio. */
    for (int i = 0; i < n_folios; i++) {
        struct pcache_node *pcn = folios[i]->page.pcache.pcache_node;
        fi[i].nblk = pcn->size / BSIZE;
        uint base = pcn->blkno / BLK512_PER_BSIZE;

        fi[i].first_addr = xv6fs_bmap_read(ip, base);
        if (fi[i].first_addr == 0) {
            fi[i].contig = false;
            fi[i].last_addr = 0;
            continue;
        }
        if (fi[i].nblk <= 1) {
            fi[i].last_addr = fi[i].first_addr;
            fi[i].contig = true;
        } else {
            fi[i].last_addr = xv6fs_bmap_read(ip, base + fi[i].nblk - 1);
            fi[i].contig = (fi[i].last_addr ==
                            fi[i].first_addr + (uint)(fi[i].nblk - 1));
        }
    }

    /* Step 2: group contiguous folios into runs and create merged bios. */
    int ret = 0;
    int i = 0;
    while (i < n_folios) {
        /* Non-contiguous or hole: fall back to per-folio path. */
        if (!fi[i].contig) {
            ret = xv6fs_submit_folio_reads(ip, xv6_sb, folios[i],
                                           bios, max_bios, n_submitted);
            if (ret != 0) return ret;
            i++;
            continue;
        }

        /* Find the longest contiguous run starting at i. */
        int run_start = i;
        i++;
        while (i < n_folios && fi[i].contig &&
               fi[i].first_addr == fi[i - 1].last_addr + 1)
            i++;
        int run_len = i - run_start;

        /* Single contiguous folio: per-folio path is just as efficient. */
        if (run_len == 1) {
            ret = xv6fs_submit_folio_reads(ip, xv6_sb, folios[run_start],
                                           bios, max_bios, n_submitted);
            if (ret != 0) return ret;
            continue;
        }

        /* Merged bio for run_len contiguous folios. */
        if (*n_submitted >= max_bios)
            return -ENOMEM;

        struct bio *bio = bio_alloc(xv6_sb->blkdev, run_len, false, NULL, NULL);
        if (IS_ERR_OR_NULL(bio))
            return -ENOMEM;

        bio->blkno = (uint64)fi[run_start].first_addr * BLK512_PER_BSIZE;
        bio->batch = 1; /* caller kicks the device */

        for (int j = 0; j < run_len; j++) {
            struct pcache_node *pcn =
                folios[run_start + j]->page.pcache.pcache_node;
            int r = bio_add_folio(bio, folios[run_start + j], pcn->size, 0);
            if (r != 0) {
                bio_release(bio);
                return r;
            }
        }

        int r = blkdev_submit_bio(xv6_sb->blkdev, bio);
        if (r != 0) {
            bio_release(bio);
            return r;
        }

        bios[(*n_submitted)++] = bio;
    }

    return ret;
}

/******************************************************************************
 * Readahead callback for pcache sliding-window prefetch.
 *
 * Called by __pcache_readahead() with up to PCACHE_RA_WINDOW folios whose
 * io_in_progress is already set.  We resolve bmap, merge contiguous folios
 * into large scatter-gather BIOs, kick the device once, and hand back one
 * bio reference per page (bio_dup'd for pages sharing a merged bio).
 ******************************************************************************/

static int xv6fs_submit_readahead(struct pcache *pcache,
                                  page_t **pages, int nr_pages,
                                  struct bio **bios, int max_bios)
{
    struct vfs_inode *inode = (struct vfs_inode *)pcache->private_data;
    if (inode == NULL || inode->sb == NULL)
        return 0;

    struct xv6fs_inode *ip = container_of(inode, struct xv6fs_inode, vfs_inode);
    struct xv6fs_superblock *xv6_sb =
        container_of(inode->sb, struct xv6fs_superblock, vfs_sb);

    for (int i = 0; i < nr_pages; i++)
        bios[i] = NULL;

    int submitted = 0;
    int i = 0;
    while (i < nr_pages) {
        struct pcache_node *pcn = pages[i]->pcache.pcache_node;
        int nblk = pcn->size / BSIZE;
        uint base = pcn->blkno / BLK512_PER_BSIZE;

        /* Boundary check: first and last physical block of this folio */
        uint first_addr = xv6fs_bmap_read(ip, base);
        if (first_addr == 0) { i++; continue; }

        bool contig;
        if (nblk <= 1) {
            contig = true;
        } else {
            uint last_addr = xv6fs_bmap_read(ip, base + nblk - 1);
            contig = (last_addr == first_addr + (uint)(nblk - 1));
        }
        if (!contig) { i++; continue; }

        /* Build contiguous run starting at page i */
        int run_len = 1;
        uint run_bytes = pcn->size;
        uint expected_next = first_addr + (uint)nblk;

        while (i + run_len < nr_pages && run_len < BIO_MAX_VECS) {
            struct pcache_node *pn = pages[i + run_len]->pcache.pcache_node;
            int pn_nblk = pn->size / BSIZE;
            uint pn_base = pn->blkno / BLK512_PER_BSIZE;

            if (run_bytes + pn->size > BIO_MAX_SIZE)
                break;

            uint pn_first = xv6fs_bmap_read(ip, pn_base);
            if (pn_first != expected_next)
                break;

            if (pn_nblk > 1) {
                uint pn_last = xv6fs_bmap_read(ip, pn_base + pn_nblk - 1);
                if (pn_last != pn_first + (uint)(pn_nblk - 1))
                    break;
            }

            expected_next = pn_first + (uint)pn_nblk;
            run_bytes += pn->size;
            run_len++;
        }

        /* Create one merged BIO for the entire contiguous run */
        struct bio *bio = bio_alloc(xv6_sb->blkdev, run_len, false, NULL, NULL);
        if (IS_ERR_OR_NULL(bio)) { i += run_len; continue; }

        bio->blkno = (uint64)first_addr * BLK512_PER_BSIZE;
        bio->batch = 1; /* caller kicks after all bios */

        bool ok = true;
        for (int j = 0; j < run_len; j++) {
            struct pcache_node *pj = pages[i + j]->pcache.pcache_node;
            int r = bio_add_folio(bio, pj->folio, pj->size, 0);
            if (r != 0) { ok = false; break; }
        }

        if (!ok || blkdev_submit_bio(xv6_sb->blkdev, bio) != 0) {
            bio_release(bio);
            i += run_len;
            continue;
        }

        /* Distribute references: first page gets original,
         * rest get bio_dup'd so each can independently await+release. */
        bios[i] = bio;
        for (int j = 1; j < run_len; j++) {
            bio_dup(bio);
            bios[i + j] = bio;
        }

        submitted += run_len;
        i += run_len;
    }

    /* Single kick for all batched bios */
    if (submitted > 0)
        blkdev_kick(xv6_sb->blkdev);

    return submitted;
}

static struct pcache_ops xv6fs_pcache_ops = {
    .read_page = xv6fs_pcache_read_page,
    .write_page = xv6fs_pcache_write_page,
    .read_folio = xv6fs_pcache_read_folio,
    .write_folio = xv6fs_pcache_write_folio,
    .submit_readahead = xv6fs_submit_readahead,
};

/*
 * Initialise the embedded per-inode pcache (i_data).
 * Call once for every regular-file inode after its mode is known.
 */
void xv6fs_inode_pcache_init(struct vfs_inode *inode) {
    if (!S_ISREG(inode->mode))
        return;

    struct pcache *pc = &inode->i_data;
    memset(pc, 0, sizeof(*pc));
    pc->ops = &xv6fs_pcache_ops;
    /* blk_count in 512-byte units, rounded up to page boundary (8 blocks) */
    pc->blk_count = ((uint64)XV6FS_MAXFILE * BLK512_PER_BSIZE + 7) & ~(uint64)7;

    int ret = pcache_init(pc);
    if (ret != 0)
        return; /* proceed without pcache */

    /* pcache_init resets private_data, so set it after init */
    pc->private_data = inode;
}

/******************************************************************************
 * Slab cache initialization
 ******************************************************************************/

static int __xv6fs_init_cache(void) {
    int ret = 0;
    ret = slab_cache_init(&__xv6fs_inode_cache, "xv6fs_inode",
                          sizeof(struct xv6fs_inode),
                          SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
    if (ret != 0) {
        return ret;
    }
    ret = slab_cache_init(&__xv6fs_sb_cache, "xv6fs_sb",
                          sizeof(struct xv6fs_superblock),
                          SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
    if (ret != 0) {
        return ret;
    }
    return 0;
}

// Shrink xv6fs slab caches to release unused pages
void xv6fs_shrink_caches(void) {
    slab_cache_shrink(&__xv6fs_inode_cache, 0x7fffffff);
    slab_cache_shrink(&__xv6fs_sb_cache, 0x7fffffff);
}

/******************************************************************************
 * Superblock read/write helpers
 ******************************************************************************/

// Read the superblock from disk
static int __xv6fs_read_superblock(uint dev, struct superblock *disk_sb) {
    struct buf *bp = bread(dev, 1);
    if (bp == NULL) {
        return -EIO;
    }
    memmove(disk_sb, bp->data, sizeof(*disk_sb));
    brelse(bp);

    if (disk_sb->magic != FSMAGIC) {
        return -EINVAL;
    }
    return 0;
}

// Write the superblock to disk (must be called after meta_pcache is initialized)
static int __xv6fs_write_superblock(struct xv6fs_superblock *xv6_sb) {
    buffer_head_t *bh = sb_bread(xv6_sb, 1);
    if (bh == NULL) {
        return -EIO;
    }
    memmove(bh->b_data, &xv6_sb->disk_sb, sizeof(xv6_sb->disk_sb));
    bh_write(bh);
    bh_release(bh);
    return 0;
}

/******************************************************************************
 * Inode allocation
 ******************************************************************************/

static struct xv6fs_inode *__xv6fs_alloc_inode_structure(void) {
    struct xv6fs_inode *xv6fs_inode = slab_alloc(&__xv6fs_inode_cache);
    if (xv6fs_inode == NULL) {
        return NULL;
    }
    memset(xv6fs_inode, 0, sizeof(*xv6fs_inode));
    xv6fs_inode->vfs_inode.ops = &xv6fs_inode_ops;
    return xv6fs_inode;
}

struct vfs_inode *xv6fs_alloc_inode(struct vfs_superblock *sb) {
    if (sb == NULL) {
        return ERR_PTR(-EINVAL);
    }

    struct xv6fs_superblock *xv6_sb =
        container_of(sb, struct xv6fs_superblock, vfs_sb);
    struct superblock *disk_sb = &xv6_sb->disk_sb;
    uint dev = xv6fs_sb_dev(xv6_sb);

    // Find a free inode on disk
    buffer_head_t *bh;
    struct dinode *dip;

    for (uint inum = 1; inum < disk_sb->ninodes; inum++) {
        bh = sb_bread(xv6_sb, XV6FS_IBLOCK(inum, disk_sb));
        if (bh == NULL) {
            return ERR_PTR(-EIO);
        }
        dip = (struct dinode *)bh->b_data + inum % IPB;
        if (dip->type == 0) {
            // Found a free inode
            memset(dip, 0, sizeof(*dip));
            // Mark as allocated but type will be set by caller
            xv6fs_log_write(xv6_sb, bh);
            bh_release(bh);

            // Allocate in-memory structure
            struct xv6fs_inode *xv6fs_inode = __xv6fs_alloc_inode_structure();
            if (xv6fs_inode == NULL) {
                return ERR_PTR(-ENOMEM);
            }

            xv6fs_inode->dev = dev;
            xv6fs_inode->vfs_inode.ino = inum;
            // Note: Do NOT set vfs_inode.sb here - VFS will set it in
            // vfs_add_inode
            xv6fs_inode->vfs_inode.ref_count = 1;

            return &xv6fs_inode->vfs_inode;
        }
        bh_release(bh);
    }

    return ERR_PTR(-ENOSPC);
}

/******************************************************************************
 * Get inode from disk
 ******************************************************************************/

struct vfs_inode *xv6fs_get_inode(struct vfs_superblock *sb, uint64 ino) {
    if (sb == NULL || ino == 0) {
        return ERR_PTR(-EINVAL);
    }

    struct xv6fs_superblock *xv6_sb =
        container_of(sb, struct xv6fs_superblock, vfs_sb);
    struct superblock *disk_sb = &xv6_sb->disk_sb;
    uint dev = xv6fs_sb_dev(xv6_sb);

    if (ino >= disk_sb->ninodes) {
        printf("xv6fs_get_inode: ino=%lu >= ninodes=%u\n", ino, disk_sb->ninodes);
        return ERR_PTR(-ENOENT);
    }

    // Read inode from disk
    uint iblock = XV6FS_IBLOCK(ino, disk_sb);
    buffer_head_t *bh = sb_bread(xv6_sb, iblock);
    if (bh == NULL) {
        printf("xv6fs_get_inode: sb_bread failed for iblock=%u (ino=%lu)\n", iblock, ino);
        return ERR_PTR(-EIO);
    }

    struct dinode *dip = (struct dinode *)bh->b_data + ino % IPB;
    if (dip->type == 0) {
        printf("xv6fs_get_inode: ino=%lu type=0 (iblock=%u, offset=%lu)\n", ino, iblock, ino % IPB);
        bh_release(bh);
        return ERR_PTR(-ENOENT);
    }

    // Allocate in-memory inode
    struct xv6fs_inode *xv6fs_inode = __xv6fs_alloc_inode_structure();
    if (xv6fs_inode == NULL) {
        bh_release(bh);
        return ERR_PTR(-ENOMEM);
    }

    // Fill in VFS inode fields
    xv6fs_inode->dev = dev;
    xv6fs_inode->vfs_inode.ino = ino;
    // Note: Do NOT set vfs_inode.sb here - VFS will set it when adding to hash
    xv6fs_inode->vfs_inode.ref_count = 1;
    xv6fs_inode->vfs_inode.mode = xv6fs_type_to_mode(dip->type);
    xv6fs_inode->vfs_inode.n_links = dip->nlink;
    xv6fs_inode->vfs_inode.size = dip->size;

    // Fill in xv6fs-specific fields
    xv6fs_inode->major = dip->major;
    xv6fs_inode->minor = dip->minor;
    memmove(xv6fs_inode->addrs, dip->addrs, sizeof(dip->addrs));

    // For device inodes, set the appropriate device number field
    if (dip->type == XV6FS_T_BLKDEVICE) {
        dev_t devno = mkdev(xv6fs_inode->major, xv6fs_inode->minor);
        xv6fs_inode->vfs_inode.bdev = devno;
    } else if (dip->type == XV6FS_T_CDEVICE) {
        dev_t devno = mkdev(xv6fs_inode->major, xv6fs_inode->minor);
        xv6fs_inode->vfs_inode.cdev = devno;
    }

    bh_release(bh);
    return &xv6fs_inode->vfs_inode;
}

/******************************************************************************
 * Sync operations
 ******************************************************************************/

int xv6fs_sync_fs(struct vfs_superblock *sb, int wait) {
    if (sb == NULL) {
        return -EINVAL;
    }

    struct xv6fs_superblock *xv6_sb =
        container_of(sb, struct xv6fs_superblock, vfs_sb);

    // Write superblock to disk if dirty
    if (xv6_sb->dirty) {
        int ret = __xv6fs_write_superblock(xv6_sb);
        if (ret != 0) {
            return ret;
        }
        xv6_sb->dirty = 0;
    }

    sb->dirty = 0;
    return 0;
}

void xv6fs_unmount_begin(struct vfs_superblock *sb) {
    // Sync any pending changes before unmount
    xv6fs_sync_fs(sb, 1);
}

/******************************************************************************
 * Mount/Free operations
 ******************************************************************************/

void xv6fs_free(struct vfs_superblock *sb) {
    struct xv6fs_superblock *xv6_sb =
        container_of(sb, struct xv6fs_superblock, vfs_sb);

    // Destroy block cache
    xv6fs_bcache_destroy(xv6_sb);

    if (xv6_sb->blkdev != NULL) {
        blkdev_put(xv6_sb->blkdev);
    }
    slab_free(xv6_sb);
}

int xv6fs_mount(struct vfs_inode *mountpoint, struct vfs_inode *device,
                int flags, const char *data, struct vfs_superblock **ret_sb) {
    if (mountpoint == NULL || ret_sb == NULL) {
        return -EINVAL;
    }

    /*
     * Get the block device from the device inode.
     * The device inode's bdev field contains the device number (major:minor).
     * If no device inode is provided, fall back to ROOTDEV for compatibility.
     */
    dev_t dev_num;
    if (device != NULL && S_ISBLK(device->mode)) {
        dev_num = device->bdev;
    } else {
        return -EINVAL; // xv6fs does not support block device inode
    }

    // Get blkdev reference
    blkdev_t *blkdev = blkdev_get(major(dev_num), minor(dev_num));
    if (IS_ERR(blkdev)) {
        return PTR_ERR(blkdev);
    }

    // Allocate superblock
    struct xv6fs_superblock *xv6_sb = slab_alloc(&__xv6fs_sb_cache);
    if (xv6_sb == NULL) {
        blkdev_put(blkdev);
        return -ENOMEM;
    }
    memset(xv6_sb, 0, sizeof(*xv6_sb));

    // Store blkdev reference
    xv6_sb->blkdev = blkdev;

    // Read on-disk superblock
    int ret = __xv6fs_read_superblock(xv6fs_sb_dev(xv6_sb), &xv6_sb->disk_sb);
    if (ret != 0) {
        blkdev_put(blkdev);
        slab_free(xv6_sb);
        return ret;
    }

    printf("xv6fs_mount: sb magic=%x size=%u nblocks=%u ninodes=%u nlog=%u logstart=%u inodestart=%u bmapstart=%u\n",
           xv6_sb->disk_sb.magic, xv6_sb->disk_sb.size,
           xv6_sb->disk_sb.nblocks, xv6_sb->disk_sb.ninodes,
           xv6_sb->disk_sb.nlog, xv6_sb->disk_sb.logstart,
           xv6_sb->disk_sb.inodestart, xv6_sb->disk_sb.bmapstart);

    xv6_sb->dirty = 0;

    // Initialize metadata page cache (backing store for buffer_head)
    meta_pcache_init(xv6_sb);

    // Initialize logging layer
    xv6fs_initlog(xv6_sb);

    // Initialize block allocation cache
    ret = xv6fs_bcache_init(xv6_sb);
    if (ret != 0) {
        printf("xv6fs: warning: block cache init failed (%d), using fallback\n",
               ret);
        // Don't fail mount - the fallback linear scan will still work
    }

    // Initialize VFS superblock
    xv6_sb->vfs_sb.block_size = XV6FS_BSIZE;
    xv6_sb->vfs_sb.total_blocks = xv6_sb->disk_sb.size;
    // xv6fs is a backend filesystem - inodes can be evicted from cache
    // when refcount reaches 0 since they can be re-read from disk.
    // Root inodes and mountpoint inodes are protected in vfs_iput.
    xv6_sb->vfs_sb.backendless = 0;
    xv6_sb->vfs_sb.ops = &xv6fs_superblock_ops;
    xv6_sb->vfs_sb.fs_data = xv6_sb;

    // Load root inode (inode 1 in xv6)
    struct vfs_inode *root_inode = xv6fs_get_inode(&xv6_sb->vfs_sb, ROOTINO);
    if (IS_ERR_OR_NULL(root_inode)) {
        blkdev_put(blkdev);
        slab_free(xv6_sb);
        return root_inode == NULL ? -ENOMEM : PTR_ERR(root_inode);
    }

    xv6_sb->vfs_sb.root_inode = root_inode;

    *ret_sb = &xv6_sb->vfs_sb;
    return 0;
}

/******************************************************************************
 * Orphan inode operations
 *
 * These operations are used by the VFS layer to track orphan inodes (inodes
 * with n_links=0 but ref_count>0) for crash recovery. When an inode becomes
 * an orphan, add_orphan is called to record it persistently. When the last
 * reference drops and the inode is destroyed, remove_orphan is called.
 * On mount, recover_orphans is called to clean up orphans from a previous
 * crash.
 *
 * TODO: Implement persistent orphan journal. For now, these are stubs that
 * allow the VFS unmount path to work correctly. If the system crashes with
 * orphan inodes, those inodes will leak until fsck is run.
 ******************************************************************************/

// Add an inode to the orphan list (called when n_links drops to 0)
static int xv6fs_add_orphan(struct vfs_superblock *sb,
                            struct vfs_inode *inode) {
    // TODO: Implement persistent orphan journal
    // For now, just return success - the VFS layer maintains an in-memory list
    (void)sb;
    (void)inode;
    return 0;
}

// Remove an inode from the orphan list (called after destroy_inode)
static int xv6fs_remove_orphan(struct vfs_superblock *sb,
                               struct vfs_inode *inode) {
    // TODO: Implement persistent orphan journal
    (void)sb;
    (void)inode;
    return 0;
}

// Recover orphan inodes from a previous crash (called during mount)
static int xv6fs_recover_orphans(struct vfs_superblock *sb) {
    // TODO: Implement persistent orphan recovery
    // Walk the orphan journal and destroy/free each orphan inode
    (void)sb;
    return 0;
}

/*
 * Transaction Callbacks for VFS-managed operations
 *
 * DESIGN: VFS Transaction Management vs FS-Internal Management
 * ============================================================
 *
 * Filesystems have two choices for transaction management:
 *
 * 1. REGISTER CALLBACKS (begin_transaction/end_transaction):
 *    - VFS manages transactions for METADATA operations
 *    - VFS calls begin_transaction BEFORE acquiring any locks
 *    - VFS calls end_transaction AFTER releasing all locks
 *    - FS inode operations (create, mkdir, unlink, link, rename, etc.)
 *      must NOT call begin_op/end_op internally
 *    - This ensures correct lock ordering: transaction → locks
 *    - Use this for filesystems with simple transaction requirements
 *
 * 2. DO NOT REGISTER CALLBACKS (set to NULL):
 *    - FS manages ALL transactions internally
 *    - FS is responsible for correct lock ordering
 *    - FS inode operations must call begin_op/end_op themselves
 *    - Use this for filesystems that need complex transaction control
 *      (e.g., batching, nested transactions, custom commit logic)
 *    - WARNING: Must be careful about lock ordering to avoid deadlock
 *
 * xv6fs HYBRID APPROACH:
 * ----------------------
 * xv6fs registers callbacks for metadata operations (create, unlink, etc.)
 * because these are single-transaction operations that benefit from VFS
 * lock ordering management.
 *
 * However, FILE OPERATIONS (write, truncate) manage transactions INTERNALLY:
 * - File write needs multiple transactions (batching for large writes)
 * - Truncate needs batched transactions for large files
 * - VFS holds inode lock before calling file ops, so VFS can't wrap them
 * - These ops call xv6fs_begin_op/end_op directly
 *
 * This hybrid approach works because:
 * - Metadata ops use directory inodes + superblock lock
 * - File ops use file inodes only (no superblock lock)
 * - Different lock sets means no direct circular dependency
 *
 * Lock ordering summary:
 * - Metadata ops (VFS-managed): transaction → superblock_wlock → inode_mutex
 * - File ops (FS-managed): inode_mutex → transaction (reversed but safe)
 */

static int xv6fs_begin_transaction_op(struct vfs_superblock *sb) {
    struct xv6fs_superblock *xv6_sb =
        container_of(sb, struct xv6fs_superblock, vfs_sb);
    return xv6fs_begin_op(xv6_sb);
}

static int xv6fs_end_transaction_op(struct vfs_superblock *sb) {
    struct xv6fs_superblock *xv6_sb =
        container_of(sb, struct xv6fs_superblock, vfs_sb);
    xv6fs_end_op(xv6_sb);
    return 0;
}

/******************************************************************************
 * VFS operations structures
 ******************************************************************************/

struct vfs_superblock_ops xv6fs_superblock_ops = {
    .alloc_inode = xv6fs_alloc_inode,
    .get_inode = xv6fs_get_inode,
    .sync_fs = xv6fs_sync_fs,
    .unmount_begin = xv6fs_unmount_begin,
    .add_orphan = xv6fs_add_orphan,
    .remove_orphan = xv6fs_remove_orphan,
    .recover_orphans = xv6fs_recover_orphans,
    .begin_transaction = xv6fs_begin_transaction_op,
    .end_transaction = xv6fs_end_transaction_op,
};

struct vfs_fs_type_ops xv6fs_fs_type_ops = {
    .mount = xv6fs_mount,
    .free = xv6fs_free,
};

/******************************************************************************
 * Filesystem type initialization
 ******************************************************************************/

/**
 * Initialize xv6fs caches and register the filesystem type.
 * Does NOT mount the filesystem - call xv6fs_mount_root() for that.
 */
void xv6fs_init(void) {
    // Initialize caches
    int ret = __xv6fs_init_cache();
    assert(ret == 0, "xv6fs_init: __xv6fs_init_cache failed, errno=%d", ret);

    // Allocate and register filesystem type
    struct vfs_fs_type *fs_type = vfs_fs_type_allocate();
    assert(fs_type != NULL, "xv6fs_init: vfs_fs_type_allocate failed");

    fs_type->name = "xv6fs";
    fs_type->ops = &xv6fs_fs_type_ops;

    vfs_mount_lock();
    ret = vfs_register_fs_type(fs_type);
    assert(ret == 0, "xv6fs_init: vfs_register_fs_type failed, errno=%d", ret);
    vfs_mount_unlock();

    printf("xv6fs: filesystem type registered\n");
}

/**
 * Mount xv6fs at /root and chroot into it.
 * Requires: tmpfs already mounted as initial root (vfs_root_inode.mnt_rooti
 * set).
 *
 * Root device selection order:
 *   1. boot command line "root=" parameter (from bootloader / QEMU -append)
 *   2. ramdisk (major 3) if available
 *   3. fallback to ROOTDEV (virtio disk, major 2)
 */
void xv6fs_mount_root(void) {
    struct vfs_inode *tmpfs_root = vfs_root_inode.mnt_rooti;
    if (tmpfs_root == NULL) {
        printf("xv6fs: no root filesystem to mount onto\n");
        return;
    }

    // Create /root directory in tmpfs root (vfs_mkdir handles its own locking)
    struct vfs_inode *root_dir = vfs_mkdir(tmpfs_root, 0755, "root", 4);

    if (IS_ERR_OR_NULL(root_dir)) {
        printf("xv6fs: failed to create /root directory\n");
        return;
    }

    // Select root device:
    // 1. Try boot command line root= parameter
    // 2. Prefer ramdisk if available
    // 3. Fall back to compiled-in ROOTDEV
    dev_t root_dev = cmdline_get_root_dev();
    if (root_dev != 0) {
        blkdev_t *bdev = blkdev_get(major(root_dev), minor(root_dev));
        if (bdev == NULL || IS_ERR(bdev)) {
            printf("xv6fs: cmdline root device (%d,%d) not found, "
                   "falling back\n", major(root_dev), minor(root_dev));
            root_dev = 0;
        } else {
            blkdev_put(bdev);
        }
    }
    if (root_dev == 0) {
        blkdev_t *ramdisk = blkdev_get(major(RAMDISK_DEV), minor(RAMDISK_DEV));
        if (ramdisk != NULL && !IS_ERR(ramdisk)) {
            root_dev = RAMDISK_DEV;
            blkdev_put(ramdisk);
        } else {
            root_dev = ROOTDEV;
        }
    }

    // Create a block device inode for root device
    struct vfs_inode *dev_inode =
        vfs_mknod(tmpfs_root, S_IFBLK | 0600, root_dev, "rootdev", 7);
    if (IS_ERR_OR_NULL(dev_inode)) {
        printf("xv6fs: failed to create device inode, errno=%ld\n",
               dev_inode ? PTR_ERR(dev_inode) : -ENOMEM);
        vfs_iput(root_dir);
        return;
    }

    // Mount xv6fs at /root
    // vfs_mount requires: mount mutex, superblock write lock, and inode lock
    // On success, caller must release locks. On failure, vfs_mount releases
    // them.
    vfs_mount_lock();
    vfs_superblock_wlock(root_dir->sb);
    vfs_ilock(root_dir);
    int ret = vfs_mount("xv6fs", root_dir, dev_inode, 0, NULL);
    if (ret == 0) {
        // Success: caller releases locks
        vfs_iunlock(root_dir);
        vfs_superblock_unlock(root_dir->sb);
    }
    // On failure, vfs_mount already released locks
    vfs_mount_unlock();

    // Release device inode reference (mount holds its own if needed)
    vfs_iput(dev_inode);

    if (ret == 0) {
        printf("xv6fs: mounted at /root\n");

        // Now chroot into the xv6fs root
        struct vfs_inode *xv6fs_root = root_dir->mnt_rooti;
        if (xv6fs_root != NULL) {
            ret = vfs_chroot(xv6fs_root);
            if (ret == 0) {
                printf("xv6fs: chroot to /root successful\n");
            } else {
                printf("xv6fs: chroot to /root failed, errno=%d\n", ret);
            }
        }
    } else {
        printf("xv6fs: failed to mount at /root, errno=%d\n", ret);
    }
    vfs_iput(root_dir);

    // xv6fs_run_all_smoketests();
}
