/*
 * ext4fs block device adapter
 *
 * Bridges lwext4's ext4_blockdev_iface to xv6's buffer cache (bread/bwrite).
 * Physical block size is 512 bytes; xv6's BSIZE is 1024.
 */

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "string.h"
#include "param.h"
#include "vfs/xv6fs/ondisk.h"
#include "lock/mutex_types.h"
#include "dev/buf.h"
#include "dev/blkdev.h"
#include "ext4fs_private.h"

#include <ext4_errno.h>

/* Number of 512-byte physical blocks per xv6 BSIZE block */
#define PH_BSIZE       512
#define PH_PER_BSIZE   (BSIZE / PH_BSIZE)

/*
 * Obtain the xv6 device number from the ext4_blockdev container.
 */
static inline uint ext4fs_bdev_dev(struct ext4_blockdev *bdev)
{
    struct ext4fs_superblock *esb =
        container_of(bdev, struct ext4fs_superblock, bdev);
    return ext4fs_sb_dev(esb);
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
 * into buf.  Translates to xv6 bread() calls on BSIZE blocks.
 */
static int ext4fs_blockdev_bread(struct ext4_blockdev *bdev, void *buf,
                                 uint64_t blk_id, uint32_t blk_cnt)
{
    uint dev = ext4fs_bdev_dev(bdev);
    char *dst = (char *)buf;

    for (uint32_t i = 0; i < blk_cnt; i++) {
        uint64_t ph = blk_id + i;
        uint xv6_bn  = (uint)(ph / PH_PER_BSIZE);
        uint off      = (uint)((ph % PH_PER_BSIZE) * PH_BSIZE);

        struct buf *bp = bread(dev, xv6_bn);
        if (bp == NULL)
            return EIO;

        memcpy(dst + (uint64_t)i * PH_BSIZE, bp->data + off, PH_BSIZE);
        brelse(bp);
    }

    return EOK;
}

/*
 * Write blk_cnt physical blocks (512 bytes each) starting at blk_id
 * from buf.  Uses read-modify-write through xv6's buffer cache.
 */
static int ext4fs_blockdev_bwrite(struct ext4_blockdev *bdev, const void *buf,
                                  uint64_t blk_id, uint32_t blk_cnt)
{
    uint dev = ext4fs_bdev_dev(bdev);
    const char *src = (const char *)buf;

    for (uint32_t i = 0; i < blk_cnt; i++) {
        uint64_t ph = blk_id + i;
        uint xv6_bn  = (uint)(ph / PH_PER_BSIZE);
        uint off      = (uint)((ph % PH_PER_BSIZE) * PH_BSIZE);

        struct buf *bp = bread(dev, xv6_bn);
        if (bp == NULL)
            return EIO;

        memcpy(bp->data + off, src + (uint64_t)i * PH_BSIZE, PH_BSIZE);
        bwrite(bp);
        brelse(bp);
    }

    return EOK;
}

/*
 * Lock/unlock callbacks protect the ext4 block cache (RB-tree, LRU list)
 * which has no internal synchronisation.  Must be a sleeping lock because
 * bread() acquires buffer mutexes internally.
 */
static int ext4fs_blockdev_lock(struct ext4_blockdev *bdev)
{
    struct ext4fs_superblock *esb =
        container_of(bdev, struct ext4fs_superblock, bdev);
    mutex_lock(&esb->lock);
    return EOK;
}

static int ext4fs_blockdev_unlock(struct ext4_blockdev *bdev)
{
    struct ext4fs_superblock *esb =
        container_of(bdev, struct ext4fs_superblock, bdev);
    mutex_unlock(&esb->lock);
    return EOK;
}

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
    iface->lock   = ext4fs_blockdev_lock;
    iface->unlock = ext4fs_blockdev_unlock;

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
