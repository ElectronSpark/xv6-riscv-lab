/*
 * ext4fs block device adapter
 *
 * Bridges lwext4's ext4_blockdev_iface to xv6's BIO/blkdev layer.
 * Physical block size is 512 bytes.  Reads and writes allocate BIOs
 * and submit them directly to the underlying blkdev_t, bypassing the
 * legacy buffer cache entirely.
 */

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "string.h"
#include "param.h"
#include "lock/mutex_types.h"
#include "dev/bio.h"
#include "dev/blkdev.h"
#include "ext4fs_private.h"
#include <mm/page.h>
#include <mm/folio.h>
#include <errno.h>

#include <ext4_errno.h>

/* Physical block size used by lwext4 */
#define PH_BSIZE 512

/* Number of 512-byte sectors that fit in one page */
#define SECTORS_PER_PAGE (PGSIZE / PH_BSIZE)
#define EXT4FS_BIO_MAX_PAGES 128  /* max pages for bread/bwrite stack arrays */

/*
 * Obtain the xv6 blkdev_t pointer from the ext4_blockdev container.
 */
static inline blkdev_t *ext4fs_bdev_blkdev(struct ext4_blockdev *bdev)
{
    struct ext4fs_superblock *esb =
        container_of(bdev, struct ext4fs_superblock, bdev);
    return esb->xv6_blkdev;
}

/******************************************************************************
 * ext4_blockdev_iface callbacks
 ******************************************************************************/

static int ext4fs_blockdev_open(struct ext4_blockdev *bdev)
{
    /* The xv6 block device is already opened during mount. */
    (void)bdev;
    return EOK;
}

static int ext4fs_blockdev_close(struct ext4_blockdev *bdev)
{
    /* Cleanup is done in ext4fs_free. */
    (void)bdev;
    return EOK;
}

/*
 * Read blk_cnt physical blocks (512 bytes each) starting at blk_id
 * into buf.  Uses BIO submitted directly to the blkdev.
 *
 * For efficiency we batch up to one page worth of contiguous sectors
 * into a single BIO.
 */
static int ext4fs_blockdev_bread(struct ext4_blockdev *bdev, void *buf,
                                 uint64_t blk_id, uint32_t blk_cnt)
{
    blkdev_t *blk = ext4fs_bdev_blkdev(bdev);
    char *dst = (char *)buf;
    page_t *pages[EXT4FS_BIO_MAX_PAGES];
    uint16 seg_bytes[EXT4FS_BIO_MAX_PAGES];
    int16 nr_pages;

    uint32_t done = 0;
    while (done < blk_cnt) {
        memset(pages, 0, sizeof(pages));
        memset(seg_bytes, 0, sizeof(seg_bytes));
        nr_pages = 0;
        uint32_t batch = blk_cnt - done;
        uint32_t max_batch = EXT4FS_BIO_MAX_PAGES * SECTORS_PER_PAGE;
        uint32_t packed = 0;
        if (batch > max_batch)
            batch = max_batch;

        while ((uint32_t)nr_pages * SECTORS_PER_PAGE < batch) {
            uint32_t sectors_left = batch - (uint32_t)nr_pages * SECTORS_PER_PAGE;
            uint16 bytes = (uint16)((sectors_left > SECTORS_PER_PAGE ?
                                     SECTORS_PER_PAGE : sectors_left) * PH_BSIZE);
            page_t *page = __page_alloc(0, PAGE_TYPE_ANON);
            if (page == NULL)
                goto read_fail;
            pages[nr_pages] = page;
            seg_bytes[nr_pages] = bytes;
            nr_pages++;
        }

        struct bio *bio = bio_alloc(blk, nr_pages, false, NULL, NULL);
        if (IS_ERR_OR_NULL(bio)) {
            goto read_fail;
        }

        bio->blkno = blk_id + done;
        int ret = 0;
        for (int16 i = 0; i < nr_pages; i++) {
            ret = bio_add_folio(bio, page_folio(pages[i]), seg_bytes[i], 0);
            if (ret != 0) {
                bio_release(bio);
                goto read_fail;
            }
        }

        ret = blkdev_submit_bio(blk, bio);
        if (ret != 0) {
            bio_release(bio);
            goto read_fail;
        }

        ret = bio_await(bio);
        bio_release(bio);

        if (ret != 0)
            goto read_fail;

        uint32_t copied = 0;
        for (int16 i = 0; i < nr_pages; i++) {
            void *pa = (void *)__page_to_pa(pages[i]);
            memcpy(dst + (uint64_t)done * PH_BSIZE + copied, pa, seg_bytes[i]);
            copied += seg_bytes[i];
            __page_free(pages[i], 0);
        }

        done += batch;
    }

    return EOK;

read_fail:
    for (int16 i = 0; i < EXT4FS_BIO_MAX_PAGES; i++) {
        if (pages[i] != NULL)
            __page_free(pages[i], 0);
    }
    return EIO;
}

/*
 * Write blk_cnt physical blocks (512 bytes each) starting at blk_id
 * from buf.  Uses BIO submitted directly to the blkdev.
 */
static int ext4fs_blockdev_bwrite(struct ext4_blockdev *bdev, const void *buf,
                                  uint64_t blk_id, uint32_t blk_cnt)
{
    blkdev_t *blk = ext4fs_bdev_blkdev(bdev);
    const char *src = (const char *)buf;
    page_t *pages[EXT4FS_BIO_MAX_PAGES];
    uint16 seg_bytes[EXT4FS_BIO_MAX_PAGES];
    int16 nr_pages;

    uint32_t done = 0;
    while (done < blk_cnt) {
        memset(pages, 0, sizeof(pages));
        memset(seg_bytes, 0, sizeof(seg_bytes));
        nr_pages = 0;
        uint32_t batch = blk_cnt - done;
        uint32_t max_batch = EXT4FS_BIO_MAX_PAGES * SECTORS_PER_PAGE;
        uint32_t packed = 0;
        if (batch > max_batch)
            batch = max_batch;

        while ((uint32_t)nr_pages * SECTORS_PER_PAGE < batch) {
            uint32_t sectors_left = batch - (uint32_t)nr_pages * SECTORS_PER_PAGE;
            uint16 bytes = (uint16)((sectors_left > SECTORS_PER_PAGE ?
                                     SECTORS_PER_PAGE : sectors_left) * PH_BSIZE);
            page_t *page = __page_alloc(0, PAGE_TYPE_ANON);
            if (page == NULL)
                goto write_fail;
            void *pa = (void *)__page_to_pa(page);
            memcpy(pa, src + (uint64_t)done * PH_BSIZE + packed, bytes);
            pages[nr_pages] = page;
            seg_bytes[nr_pages] = bytes;
            packed += bytes;
            nr_pages++;
        }

        struct bio *bio = bio_alloc(blk, nr_pages, true, NULL, NULL);
        if (IS_ERR_OR_NULL(bio)) {
            goto write_fail;
        }

        bio->blkno = blk_id + done;
        int ret = 0;
        for (int16 i = 0; i < nr_pages; i++) {
            ret = bio_add_folio(bio, page_folio(pages[i]), seg_bytes[i], 0);
            if (ret != 0) {
                bio_release(bio);
                goto write_fail;
            }
        }

        ret = blkdev_submit_bio(blk, bio);
        if (ret != 0) {
            bio_release(bio);
            goto write_fail;
        }

        ret = bio_await(bio);
        bio_release(bio);
        for (int16 i = 0; i < nr_pages; i++)
            __page_free(pages[i], 0);

        if (ret != 0)
            return EIO;

        done += batch;
    }

    return EOK;

write_fail:
    for (int16 i = 0; i < EXT4FS_BIO_MAX_PAGES; i++) {
        if (pages[i] != NULL)
            __page_free(pages[i], 0);
    }
    return EIO;
}

/*
 * Lock/unlock callbacks: disabled.  Serialisation is now done at the VFS
 * operation level via ext4fs_lock()/ext4fs_unlock(), which brackets entire
 * operation sequences rather than individual bread/bwrite calls.
 * The bdif-level lock only bracketed bread/bwrite but did NOT protect the
 * bcache RB-tree, so concurrent callers could corrupt it.
 */

/******************************************************************************
 * Initialise the ext4_blockdev / ext4_blockdev_iface for a mount
 ******************************************************************************/

/*
 * Default physical block count when the actual device size is unknown.
 * 1 << 24 = 16M sectors × 512 = 8 GB — a safe upper bound for the
 * ramdisk and virtio disk.  ext4_fs_init() will validate the real
 * filesystem size from the superblock.
 */
#define EXT4FS_DEFAULT_PH_BCNT (1ULL << 24)

void ext4fs_blockdev_setup(struct ext4fs_superblock *esb)
{
    struct ext4_blockdev_iface *iface = &esb->bdev_iface;

    iface->open   = ext4fs_blockdev_open;
    iface->bread  = ext4fs_blockdev_bread;
    iface->bwrite = ext4fs_blockdev_bwrite;
    iface->close  = ext4fs_blockdev_close;
    iface->lock   = NULL;
    iface->unlock = NULL;

    iface->ph_bsize  = PH_BSIZE;
    iface->ph_bcnt   = EXT4FS_DEFAULT_PH_BCNT;
    iface->ph_bbuf   = esb->ph_bbuf;
    iface->ph_refctr  = 0;
    iface->bread_ctr  = 0;
    iface->bwrite_ctr = 0;
    iface->p_user    = NULL;

    struct ext4_blockdev *bd = &esb->bdev;
    memset(bd, 0, sizeof(*bd));
    bd->bdif         = iface;
    bd->part_offset  = 0;
    bd->part_size    = (uint64_t)iface->ph_bcnt * iface->ph_bsize;
    bd->bc           = NULL;
    bd->lg_bsize     = 0;
    bd->lg_bcnt      = 0;
    bd->cache_write_back = 0;
    bd->fs           = NULL;
    bd->journal      = NULL;
}
