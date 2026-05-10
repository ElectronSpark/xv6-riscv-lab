/*
 * ext4fs file operations
 *
 * File read/write implemented using lwext4's block-level APIs:
 *   ext4_fs_get_inode_dblk_idx  — map logical block to physical
 *   ext4_fs_init_inode_dblk_idx — allocate + map a logical block
 *   ext4_fs_append_inode_dblk   — append a new data block
 *   ext4_block_get / ext4_block_set — read / release cached blocks
 *
 * LOCKING: VFS file operations do NOT hold the inode lock on entry.
 * The driver acquires it as needed (same model as xv6fs).
 */

#include <stdbool.h>   /* must come before types.h so bool = _Bool everywhere */
#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "proc/thread.h"
#include "lock/mutex_types.h"
#include <mm/vm.h>
#include <mm/page.h>
#include <mm/pcache.h>
#include <mm/folio.h>
#include <smp/atomic.h>
#include "proc/tq.h"
#include "timer/timer.h"
#include "vfs/fs.h"
#include "vfs/stat.h"
#include "vfs/fcntl.h"
#include "ext4fs_private.h"
#include "vfs/uio.h"
#include "dev/blkdev.h"

#include <ext4_errno.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_super.h>
#include <ext4_blockdev.h>
#include <ext4_bcache.h>
#include <ext4_balloc.h>

#include "timer/goldfish_rtc.h"
#include "kstats.h"

static ssize_t ext4fs_file_readv(struct vfs_file *file, struct iov_iter *iter,
                                 bool user);

#define EXT4FS_BLKS_PER_PAGE ((uint64)(PGSIZE / 512))
#define EXT4FS_FAULT_READAHEAD_PAGES 2
#define EXT4FS_MAP_CACHE_BLOCKS 32

static inline uint64 ext4fs_pcache_blk_count(loff_t size)
{
    uint64 pages = ((uint64)size + PGSIZE - 1) / PGSIZE;
    if (pages == 0)
        pages = 1;
    return pages * EXT4FS_BLKS_PER_PAGE;
}

static void ext4fs_pcache_readahead(struct pcache *pc, uint64 start_blkno_512,
                                    uint64 limit_blkno_512, int nr_pages)
{
    if (pc == NULL || !pc->active || nr_pages <= 0)
        return;

    for (int i = 0; i < nr_pages; i++) {
        uint64 blkno_512 = start_blkno_512 +
                           (uint64)i * EXT4FS_BLKS_PER_PAGE;
        if (blkno_512 >= limit_blkno_512)
            break;

        page_t *page = pcache_get_page(pc, blkno_512);
        if (page == NULL)
            break;

        (void)pcache_read_page(pc, page);
        pcache_put_page(pc, page);
    }
}

/*
 * ext4fs_folio_read_direct - Fill a multi-page folio via a single BIO,
 * bypassing the lwext4 block cache.  Only used when all data blocks are
 * physically contiguous on disk and block_size == PGSIZE.
 *
 * Returns 0 on success; negative errno on failure (caller falls back to
 * per-page ext4fs_fill_page_from_ref).
 */
static int ext4fs_folio_read_direct(struct ext4_fs *fs,
                                    struct ext4fs_superblock *esb,
                                    struct ext4_inode_ref *ref,
                                    struct pcache_node *pcn,
                                    uint64 base_file_off,
                                    uint64 inode_size)
{
    uint32_t block_size = ext4_sb_get_block_size(&fs->sb);
    unsigned int nr_pages = pcn->page_count;

    /* Only handle 4K-block filesystems with multi-page folios. */
    if (block_size != PGSIZE || nr_pages < 2 || nr_pages > 16)
        return -EINVAL;

    /* Determine how many pages fall within the file. */
    unsigned int fill_pages = nr_pages;
    for (unsigned int i = 0; i < nr_pages; i++) {
        if (base_file_off + (uint64)i * PGSIZE >= inode_size) {
            fill_pages = i;
            break;
        }
    }
    if (fill_pages < 2)
        return -EINVAL;

    /* Resolve physical blocks and check contiguity.
     * Instead of checking every block (O(N)), verify first and last only.
     * If last_fblock == first_fblock + (N-1), the range is contiguous
     * (ext4 extents guarantee blocks within one extent are contiguous,
     * and two adjacent extents with matching physical offsets are too). */
    ext4_lblk_t first_iblock = (ext4_lblk_t)(base_file_off / block_size);
    ext4_fsblk_t first_fblock;
    int r = ext4_fs_get_inode_dblk_idx(ref, first_iblock, &first_fblock, true);
    if (r != EOK || first_fblock == 0)
        return -EIO;

    ext4_fsblk_t last_fblock;
    r = ext4_fs_get_inode_dblk_idx(ref, first_iblock + fill_pages - 1,
                                   &last_fblock, true);
    if (r != EOK || last_fblock == 0 ||
        last_fblock != first_fblock + fill_pages - 1)
        return -EINVAL; /* not contiguous — fall back */

    /* Convert ext4 physical block to 512-byte sector for BIO. */
    blkdev_t *blk = esb->xv6_blkdev;
    uint64 pba = ((uint64)first_fblock * block_size +
                  esb->bdev.part_offset) / esb->bdev_iface.ph_bsize;

    struct bio *bio = bio_alloc(blk, 1, false, NULL, NULL);
    if (IS_ERR_OR_NULL(bio))
        return -ENOMEM;

    bio->blkno = pba;

    /* Add the pcache folio directly — compound pages are physically
     * contiguous, so the device DMA's straight into pcache memory. */
    r = bio_add_folio(bio, pcn->folio, fill_pages * PGSIZE, 0);
    if (r != 0) {
        bio_release(bio);
        return r;
    }

    r = blkdev_submit_bio(blk, bio);
    if (r != 0) {
        bio_release(bio);
        return r;
    }

    r = bio_await(bio);
    bio_release(bio);
    if (r != 0)
        return r;

    /* Zero-fill any partial tail within the last in-file page. */
    uint64 file_tail = inode_size - base_file_off;
    if (file_tail < (uint64)fill_pages * PGSIZE)
        memset((char *)pcn->data + file_tail, 0,
               (uint64)fill_pages * PGSIZE - file_tail);

    /* Zero-fill pages beyond the file end (remaining folio sub-pages). */
    for (unsigned int i = fill_pages; i < nr_pages; i++)
        memset((char *)pcn->data + (uint64)i * PGSIZE, 0, PGSIZE);

    return 0;
}

/*
 * ext4fs_folio_submit_direct - Like ext4fs_folio_read_direct, but returns
 * the submitted BIO instead of awaiting it.  The caller is responsible
 * for calling bio_await() + bio_release() and for zero-filling tails.
 *
 * Sets bio->batch = 1 so the device is not notified per-BIO;
 * the caller must call blkdev_kick() after submitting all batch BIOs.
 *
 * Returns the submitted BIO on success, or NULL on failure (caller should
 * fall back to the synchronous path).
 */
static struct bio *ext4fs_folio_submit_direct(struct ext4_fs *fs,
                                              struct ext4fs_superblock *esb,
                                              struct ext4_inode_ref *ref,
                                              struct pcache_node *pcn,
                                              uint64 base_file_off,
                                              uint64 inode_size)
{
    uint32_t block_size = ext4_sb_get_block_size(&fs->sb);
    unsigned int nr_pages = pcn->page_count;

    if (block_size != PGSIZE || nr_pages < 2 || nr_pages > 16)
        return NULL;

    unsigned int fill_pages = nr_pages;
    for (unsigned int i = 0; i < nr_pages; i++) {
        if (base_file_off + (uint64)i * PGSIZE >= inode_size) {
            fill_pages = i;
            break;
        }
    }
    if (fill_pages < 2)
        return NULL;

    ext4_lblk_t first_iblock = (ext4_lblk_t)(base_file_off / block_size);
    ext4_fsblk_t first_fblock;
    int r = ext4_fs_get_inode_dblk_idx(ref, first_iblock, &first_fblock, true);
    if (r != EOK || first_fblock == 0)
        return NULL;

    ext4_fsblk_t last_fblock;
    r = ext4_fs_get_inode_dblk_idx(ref, first_iblock + fill_pages - 1,
                                   &last_fblock, true);
    if (r != EOK || last_fblock == 0 ||
        last_fblock != first_fblock + fill_pages - 1)
        return NULL;

    blkdev_t *blk = esb->xv6_blkdev;
    uint64 pba = ((uint64)first_fblock * block_size +
                  esb->bdev.part_offset) / esb->bdev_iface.ph_bsize;

    struct bio *bio = bio_alloc(blk, 1, false, NULL, NULL);
    if (IS_ERR_OR_NULL(bio))
        return NULL;

    bio->blkno = pba;
    bio->batch = 1;

    r = bio_add_folio(bio, pcn->folio, fill_pages * PGSIZE, 0);
    if (r != 0) {
        bio_release(bio);
        return NULL;
    }

    r = blkdev_submit_bio(blk, bio);
    if (r != 0) {
        bio_release(bio);
        return NULL;
    }

    return bio;
}

static int ext4fs_fill_page_from_ref(struct ext4_fs *fs,
                                     struct ext4fs_superblock *esb,
                                     struct vfs_inode *inode,
                                     struct ext4_inode_ref *ref,
                                     void *dst,
                                     uint64 file_off,
                                     uint64 inode_size)
{
    uint32_t block_size = ext4_sb_get_block_size(&fs->sb);
    uint64 done = 0;

    if (file_off >= inode_size) {
        memset(dst, 0, PGSIZE);
        return 0;
    }

    uint64 bytes_to_read = PGSIZE;
    if (file_off + PGSIZE > inode_size)
        bytes_to_read = inode_size - file_off;

    while (done < bytes_to_read) {
        ext4_lblk_t iblock = (ext4_lblk_t)((file_off + done) / block_size);
        uint off = (uint)((file_off + done) % block_size);
        uint n = block_size - off;
        if (n > bytes_to_read - done)
            n = (uint)(bytes_to_read - done);

        ext4_fsblk_t fblock;
        int r = ext4_fs_get_inode_dblk_idx(ref, iblock, &fblock, true);
        if (r != EOK)
            return -r;

        if (fblock == 0) {
            memset((char *)dst + done, 0, n);
        } else {
            struct ext4_block blk;
            r = ext4_block_get(&esb->bdev, &blk, fblock);
            if (r != EOK)
                return -EIO;
            memcpy((char *)dst + done, (char *)blk.data + off, n);
            ext4_block_set(&esb->bdev, &blk);
        }

        done += n;
    }

    if (bytes_to_read < PGSIZE)
        memset((char *)dst + bytes_to_read, 0, PGSIZE - bytes_to_read);

    return 0;
}

static int ext4fs_prefault_begin_page(struct pcache *pc, page_t *page,
                                      struct pcache_node **out_node)
{
    int ret = 0;

retry:
    spin_lock(&pc->spinlock);
    if (!spin_trylock(&page->lock)) {
        spin_unlock(&pc->spinlock);
        cpu_relax();
        goto retry;
    }

    if (page->pcache.pcache != pc || page->pcache.pcache_node == NULL) {
        ret = -EINVAL;
        goto out;
    }

    struct pcache_node *pcn = page->pcache.pcache_node;
    if (pcn->uptodate) {
        ret = 1;
        goto out;
    }
    if (pcn->io_in_progress) {
        ret = 2;
        goto out;
    }

    pcn->io_in_progress = 1;
    pcn->last_request = get_jiffs();
    *out_node = pcn;

out:
    page_lock_release(page);
    spin_unlock(&pc->spinlock);
    return ret;
}

static void ext4fs_prefault_end_page(struct pcache *pc, page_t *page,
                                     int uptodate)
{
retry:
    spin_lock(&pc->spinlock);
    if (!spin_trylock(&page->lock)) {
        spin_unlock(&pc->spinlock);
        cpu_relax();
        goto retry;
    }

    if (page->pcache.pcache == pc && page->pcache.pcache_node != NULL) {
        struct pcache_node *pcn = page->pcache.pcache_node;
        if (uptodate) {
            pcn->dirty = 0;
            pcn->uptodate = 1;
        }
        pcn->io_in_progress = 0;
        pcn->last_flushed = get_jiffs();
        tq_wakeup_all(&pcn->io_waiters, 0, 0);
    }

    page_lock_release(page);
    spin_unlock(&pc->spinlock);
}

static int ext4fs_pcache_read_page(struct pcache *pcache, page_t *page)
{
    uint64 read_start = r_time();
    g_ext4_pcache_read_page_calls += 1;

    struct vfs_inode *inode = (struct vfs_inode *)pcache->private_data;
    struct pcache_node *pcnode = page->pcache.pcache_node;
    if (inode == NULL || inode->sb == NULL || pcnode == NULL)
        return -EINVAL;

    struct ext4fs_superblock *esb = ext4fs_get_esb(inode->sb);
    struct ext4_fs *fs = &esb->ext4fs;
    uint64 file_off = pcnode->blkno * 512ULL;
    uint64 inode_size = (uint64)inode->size;

    ext4fs_lock(esb);
    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)inode->ino, &ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return -r;
    }

    r = ext4fs_fill_page_from_ref(fs, esb, inode, &ref, pcnode->data,
                                  file_off, inode_size);

    ext4_fs_put_inode_ref(&ref);
    ext4fs_unlock(esb);

    if (r != 0)
        return r;

    g_ext4_pcache_pages_filled += 1;
    g_ext4_pcache_read_page_ticks += r_time() - read_start;

    return 0;
}

static int ext4fs_file_prefault(struct vfs_file *file, struct vma *vma,
                                uint64 start_va, uint64 end_va)
{
    if (file == NULL || vma == NULL || start_va >= end_va)
        return -EINVAL;

    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (inode == NULL)
        return -EINVAL;

    struct pcache *pc = &inode->i_data;
    if (!pc->active)
        ext4fs_inode_pcache_init(inode);
    if (!pc->active)
        return 0;

    struct ext4fs_superblock *esb = ext4fs_get_esb(inode->sb);
    struct ext4_fs *fs = &esb->ext4fs;
    uint64 inode_size = (uint64)READ_ONCE(inode->size);
    int ret = 0;

    /*
     * Phase 1: Collect folios that need I/O and submit BIOs in batch mode.
     * Up to 32 folios for a 2MB hugepage (64KB each).
     */
#define PREFAULT_MAX_BATCH 32
    struct {
        page_t *pcpage;
        struct pcache_node *pcn;
        struct bio *bio;
    } batch[PREFAULT_MAX_BATCH];
    int batch_count = 0;

    ext4fs_lock(esb);
    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)inode->ino, &ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return -r;
    }

    for (uint64 va = start_va; va < end_va; ) {
        uint64 file_off = vma->pgoff + (va - vma->start);
        if (file_off >= inode_size)
            break;

        uint64 blkno_512 = file_off / 512ULL;
        page_t *pcpage = pcache_get_page(pc, blkno_512);
        if (pcpage == NULL) {
            ret = -ENOMEM;
            break;
        }

        struct pcache_node *pcn = NULL;
        int state = ext4fs_prefault_begin_page(pc, pcpage, &pcn);
        unsigned long skip_pages = 1;

        if (state == 0) {
            /* Folio needs I/O — try to submit a batch BIO. */
            unsigned long nr_pages = pcn->page_count;
            uint64 base_file_off = (uint64)pcn->blkno * 512ULL;
            struct bio *bio = NULL;

            if (batch_count < PREFAULT_MAX_BATCH)
                bio = ext4fs_folio_submit_direct(fs, esb, &ref, pcn,
                                                 base_file_off, inode_size);

            if (bio != NULL) {
                /* BIO submitted in batch mode — defer await. */
                batch[batch_count].pcpage = pcpage;
                batch[batch_count].pcn = pcn;
                batch[batch_count].bio = bio;
                batch_count++;
                /* Don't pcache_put_page yet — held until await completes. */
            } else {
                /* Direct BIO failed — fall back to synchronous read. */
                uint64 read_start = r_time();
                g_ext4_pcache_read_page_calls += 1;
                r = ext4fs_folio_read_direct(fs, esb, &ref, pcn,
                                             base_file_off, inode_size);
                if (r != 0) {
                    r = 0;
                    for (unsigned long p = 0; p < nr_pages; p++) {
                        uint64 pg_off = base_file_off + p * PGSIZE;
                        void *dst = (char *)pcn->data + p * PGSIZE;
                        r = ext4fs_fill_page_from_ref(fs, esb, inode, &ref,
                                                      dst, pg_off, inode_size);
                        if (r != 0)
                            break;
                    }
                }
                if (r == 0)
                    g_ext4_pcache_pages_filled += nr_pages;
                g_ext4_pcache_read_page_ticks += r_time() - read_start;
                ext4fs_prefault_end_page(pc, pcpage, r == 0);
                if (r != 0 && ret == 0)
                    ret = r;
                pcache_put_page(pc, pcpage);
            }

            uint64 folio_end_off = base_file_off + nr_pages * PGSIZE;
            if (folio_end_off > file_off)
                skip_pages = (folio_end_off - file_off) / PGSIZE;
        } else {
            /* Folio already uptodate or in-progress — skip. */
            struct pcache_node *node = pcpage->pcache.pcache_node;
            if (node != NULL && node->page_count > 1) {
                uint64 folio_end_off = (uint64)node->blkno * 512ULL +
                                       (uint64)node->page_count * PGSIZE;
                if (folio_end_off > file_off)
                    skip_pages = (folio_end_off - file_off) / PGSIZE;
            }
            pcache_put_page(pc, pcpage);
        }

        if (skip_pages < 1)
            skip_pages = 1;
        va += skip_pages * PGSIZE;
    }

    ext4_fs_put_inode_ref(&ref);
    ext4fs_unlock(esb);

    /*
     * Phase 2: Kick device once, then await all batch BIOs.
     * Measure total batch I/O as a single interval (kick → all complete).
     */
    if (batch_count > 0) {
        uint64 batch_start = r_time();
        blkdev_kick(esb->xv6_blkdev);

        for (int i = 0; i < batch_count; i++) {
            struct pcache_node *pcn = batch[i].pcn;
            unsigned long nr_pages = pcn->page_count;

            r = bio_await(batch[i].bio);
            bio_release(batch[i].bio);

            if (r == 0) {
                /* Zero-fill partial tail / beyond-EOF pages. */
                uint64 base_file_off = (uint64)pcn->blkno * 512ULL;
                uint64 file_tail = inode_size - base_file_off;
                if (file_tail < (uint64)nr_pages * PGSIZE)
                    memset((char *)pcn->data + file_tail, 0,
                           (uint64)nr_pages * PGSIZE - file_tail);
                g_ext4_pcache_pages_filled += nr_pages;
            }

            ext4fs_prefault_end_page(pc, batch[i].pcpage, r == 0);
            if (r != 0 && ret == 0)
                ret = r;
            pcache_put_page(pc, batch[i].pcpage);
        }

        g_ext4_pcache_read_page_calls += batch_count;
        g_ext4_pcache_read_page_ticks += r_time() - batch_start;
    }
#undef PREFAULT_MAX_BATCH

    return ret;
}

static int ext4fs_pcache_write_page(struct pcache *pcache, page_t *page)
{
    struct vfs_inode *inode = (struct vfs_inode *)pcache->private_data;
    struct pcache_node *pcnode = page->pcache.pcache_node;
    if (inode == NULL || inode->sb == NULL || pcnode == NULL)
        return -EINVAL;

    uint64 file_off = pcnode->blkno * 512ULL;

    vfs_ilock(inode);
    uint64 inode_size = (uint64)inode->size;
    vfs_iunlock(inode);

    if (file_off >= inode_size)
        return 0;

    size_t len = PGSIZE;
    if (file_off + PGSIZE > inode_size)
        len = (size_t)(inode_size - file_off);

    struct ext4fs_superblock *esb = ext4fs_get_esb(inode->sb);
    struct ext4_fs *fs = &esb->ext4fs;
    uint32_t block_size = ext4_sb_get_block_size(&fs->sb);

    ext4fs_lock(esb);

    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)inode->ino, &ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return -r;
    }

    ext4_block_cache_write_back(&esb->bdev, 1);

    uint64_t orig_ondisk_size = ext4_inode_get_size(&fs->sb, ref.inode);
    uint32_t ifile_blocks =
        (uint32_t)((orig_ondisk_size + block_size - 1) / block_size);

    size_t done = 0;
    int err = 0;
    while (done < len) {
        ext4_lblk_t iblock =
            (ext4_lblk_t)((file_off + done) / block_size);
        uint off = (uint)((file_off + done) % block_size);
        uint n = block_size - off;
        if (n > len - done)
            n = (uint)(len - done);

        ext4_fsblk_t fblock;
        if (iblock < ifile_blocks) {
            r = ext4_fs_init_inode_dblk_idx(&ref, iblock, &fblock);
        } else {
            ext4_lblk_t appended_iblock;
            r = ext4_fs_append_inode_dblk(&ref, &fblock, &appended_iblock);
            if (r == EOK)
                ifile_blocks++;
        }
        if (r != EOK) {
            err = -r;
            break;
        }

        struct ext4_block blk;
        if (off == 0 && n == block_size)
            r = ext4_block_get_noread(&esb->bdev, &blk, fblock);
        else
            r = ext4_block_get(&esb->bdev, &blk, fblock);
        if (r != EOK) {
            err = -EIO;
            break;
        }

        memcpy((char *)blk.data + off, (const char *)pcnode->data + done, n);
        ext4_bcache_set_dirty(blk.buf);
        ext4_block_set(&esb->bdev, &blk);
        done += n;
    }

    ext4_fs_put_inode_ref(&ref);
    ext4_block_cache_write_back(&esb->bdev, 0);
    ext4fs_unlock(esb);
    return err;
}

static int ext4fs_pcache_read_folio(struct pcache *pcache, folio_t *folio) {
    page_t *head = &folio->page;
    struct pcache_node *pcn = head->pcache.pcache_node;
    unsigned long nr = pcn->page_count;

    /* Try direct multi-page BIO for multi-page folios. */
    if (nr >= 2) {
        struct vfs_inode *inode = (struct vfs_inode *)pcache->private_data;
        if (inode != NULL && inode->sb != NULL) {
            struct ext4fs_superblock *esb = ext4fs_get_esb(inode->sb);
            struct ext4_fs *fs = &esb->ext4fs;
            uint64 inode_size = (uint64)inode->size;
            uint64 base_file_off = (uint64)pcn->blkno * 512ULL;

            ext4fs_lock(esb);
            struct ext4_inode_ref ref;
            int r = ext4_fs_get_inode_ref(fs, (uint32_t)inode->ino, &ref);
            if (r == EOK) {
                r = ext4fs_folio_read_direct(fs, esb, &ref, pcn,
                                             base_file_off, inode_size);
                ext4_fs_put_inode_ref(&ref);
            }
            ext4fs_unlock(esb);
            if (r == 0)
                return 0;
        }
    }

    /* Fall back to per-page read through lwext4 bcache. */
    for (unsigned long i = 0; i < nr; i++) {
        uint64 saved_blkno = pcn->blkno;
        void  *saved_data  = pcn->data;
        size_t saved_size  = pcn->size;

        pcn->blkno = saved_blkno + i * (PGSIZE / 512);
        pcn->data  = (char *)saved_data + i * PGSIZE;
        pcn->size  = PGSIZE;

        int ret = ext4fs_pcache_read_page(pcache, head);

        pcn->blkno = saved_blkno;
        pcn->data  = saved_data;
        pcn->size  = saved_size;

        if (ret != 0)
            return ret;
    }
    return 0;
}

/*
 * Locking rationale for pcache write_page / write_folio / fault / read
 * (legacy paths):
 *
 * These callbacks can run concurrently with read() on the same inode.
 * Previously, write_folio and the legacy fault/read paths held
 * vfs_ilock(inode) across the entire block I/O loop.  With 256 folios
 * flushing at ~10 ms each, every concurrent read() — which also needs
 * vfs_ilock briefly to snapshot f_pos and inode->size — was blocked for
 * the full flush duration, dropping ext4 read throughput from ~200+ MB/s
 * to ~6 MB/s.
 *
 * Fix: hold vfs_ilock only long enough to snapshot the metadata we need
 * (inode->size, file->f_pos).  All ext4 block I/O is serialised by
 * ext4fs_lock(esb), which does not conflict with the brief vfs_ilock in
 * the read path.  This is safe because:
 *   - inode->ino is immutable after creation
 *   - inode->size is captured once under brief vfs_ilock
 *   - ext4 on-disk metadata ops are serialised by ext4fs_lock(esb)
 *   - the on-disk inode (ext4_inode_ref) is a separate structure from
 *     the VFS inode and is protected by ext4fs_lock, not vfs_ilock
 */
static int ext4fs_pcache_write_folio(struct pcache *pcache, folio_t *folio) {
    page_t *head = &folio->page;
    struct pcache_node *pcn = head->pcache.pcache_node;
    unsigned long nr = pcn->page_count;

    struct vfs_inode *inode = (struct vfs_inode *)pcache->private_data;
    if (inode == NULL || inode->sb == NULL)
        return -EINVAL;

    uint64 base_file_off = (uint64)pcn->blkno * 512ULL;

    vfs_ilock(inode);
    uint64 inode_size = (uint64)inode->size;
    vfs_iunlock(inode);

    if (base_file_off >= inode_size)
        return 0;

    struct ext4fs_superblock *esb = ext4fs_get_esb(inode->sb);
    struct ext4_fs *fs = &esb->ext4fs;
    uint32_t block_size = ext4_sb_get_block_size(&fs->sb);

    ext4fs_lock(esb);

    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)inode->ino, &ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return -r;
    }

    /* Enable write-back for the entire folio to batch block cache flushes */
    ext4_block_cache_write_back(&esb->bdev, 1);

    /* For each block, look up the physical block via init_inode_dblk_idx.
     * If the block is unmapped (fblock=0), allocate at that specific
     * position using ext4_balloc_alloc_block + set_inode_data_block_index. */

    ext4_lblk_t max_written_iblock = 0;
    bool any_written = false;

    int err = 0;
    for (unsigned long pg = 0; pg < nr && err == 0; pg++) {
        uint64 file_off = base_file_off + pg * PGSIZE;
        if (file_off >= inode_size)
            break;

        size_t len = PGSIZE;
        if (file_off + PGSIZE > inode_size)
            len = (size_t)(inode_size - file_off);

        const char *src = (const char *)pcn->data + pg * PGSIZE;
        size_t done = 0;
        while (done < len) {
            ext4_lblk_t iblock =
                (ext4_lblk_t)((file_off + done) / block_size);
            uint off = (uint)((file_off + done) % block_size);
            uint n = block_size - off;
            if (n > len - done)
                n = (uint)(len - done);

            ext4_fsblk_t fblock;
            r = ext4_fs_init_inode_dblk_idx(&ref, iblock, &fblock);
            if (r != EOK) {
                err = -r;
                break;
            }

            /* Block not mapped — allocate at this specific position.
             * For indirect-block inodes, init_inode_dblk_idx is
             * lookup-only and returns fblock=0 for unmapped blocks.
             * Allocate a physical block and set the mapping directly
             * so that we handle any folio flush order correctly. */
            if (fblock == 0) {
                ext4_fsblk_t goal;
                r = ext4_fs_indirect_find_goal(&ref, &goal);
                if (r != EOK) { err = -r; break; }
                ext4_fsblk_t phys;
                r = ext4_balloc_alloc_block(&ref, goal, &phys);
                if (r != EOK) { err = -r; break; }
                r = ext4_fs_set_inode_data_block_index(&ref, iblock,
                                                       phys);
                if (r != EOK) {
                    ext4_balloc_free_block(&ref, phys);
                    err = -r;
                    break;
                }
                ref.dirty = true;
                fblock = phys;
            }

            struct ext4_block blk;
            if (off == 0 && n == block_size)
                r = ext4_block_get_noread(&esb->bdev, &blk, fblock);
            else
                r = ext4_block_get(&esb->bdev, &blk, fblock);
            if (r != EOK) {
                err = -EIO;
                break;
            }

            memcpy((char *)blk.data + off, src + done, n);
            ext4_bcache_set_dirty(blk.buf);
            ext4_block_set(&esb->bdev, &blk);
            done += n;

            if (iblock >= max_written_iblock) {
                max_written_iblock = iblock;
                any_written = true;
            }
        }
    }

    /* Update on-disk inode size to match VFS inode size.
     * ext4_fs_append_inode_dblk inflates on-disk size by one block per
     * append, so we may need to trim it back to the exact VFS size. */
    if (any_written) {
        uint64_t cur_size = ext4_inode_get_size(&fs->sb, ref.inode);
        if (cur_size != inode_size) {
            ext4_inode_set_size(ref.inode, inode_size);
            ref.dirty = true;
        }
    }

    ext4_fs_put_inode_ref(&ref);
    /* Single flush for the entire folio instead of per-page */
    ext4_block_cache_write_back(&esb->bdev, 0);
    ext4fs_unlock(esb);
    return err;
}

/*
 * Batch readahead: submit non-blocking read BIOs for multiple folios.
 * Acquires ext4fs_lock once, resolves block mappings for all folios,
 * and merges contiguous folios into single multi-segment BIOs to reduce
 * virtio round-trips.  Each merged BIO is bio_dup'd so every page in
 * the run holds its own reference (compatible with per-page bio_await
 * in __pcache_readahead).  Each BIO notifies the device on its last
 * segment to avoid descriptor exhaustion deadlocks.
 */
static int ext4fs_submit_readahead(struct pcache *pcache,
                                   page_t **pages, int nr_pages,
                                   struct bio **bios, int max_bios)
{
    struct vfs_inode *inode = (struct vfs_inode *)pcache->private_data;
    if (inode == NULL || inode->sb == NULL)
        return 0;

    struct ext4fs_superblock *esb = ext4fs_get_esb(inode->sb);
    struct ext4_fs *fs = &esb->ext4fs;

    vfs_ilock(inode);
    uint64 inode_size = (uint64)inode->size;
    vfs_iunlock(inode);

    ext4fs_lock(esb);

    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)inode->ino, &ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return 0;
    }

    uint32_t block_size = ext4_sb_get_block_size(&fs->sb);
    blkdev_t *blk = esb->xv6_blkdev;

    for (int i = 0; i < nr_pages; i++)
        bios[i] = NULL;

    int submitted = 0;
    int i = 0;
    while (i < nr_pages) {
        struct pcache_node *pcn = pages[i]->pcache.pcache_node;
        uint64 base_off = (uint64)pcn->blkno * 512ULL;

        /* How many pages of this folio have valid file data? */
        unsigned int fill = pcn->page_count;
        for (unsigned int p = 0; p < pcn->page_count; p++) {
            if (base_off + (uint64)p * PGSIZE >= inode_size) {
                fill = p;
                break;
            }
        }
        if (block_size != PGSIZE || fill < 2) {
            i++;
            continue;
        }

        /* Lookup first and last physical block of this folio */
        ext4_lblk_t first_iblock = (ext4_lblk_t)(base_off / block_size);
        ext4_fsblk_t first_fblock;
        r = ext4_fs_get_inode_dblk_idx(&ref, first_iblock,
                                       &first_fblock, true);
        if (r != EOK || first_fblock == 0) { i++; continue; }

        ext4_fsblk_t last_fblock;
        r = ext4_fs_get_inode_dblk_idx(&ref, first_iblock + fill - 1,
                                       &last_fblock, true);
        if (r != EOK || last_fblock != first_fblock + fill - 1) {
            i++;
            continue;
        }

        /* Start a contiguous run from page i */
        int run_len = 1;
        ext4_fsblk_t expected_next = first_fblock + fill;
        uint32 run_bytes = fill * PGSIZE;

        /* Precompute candidate run: gather folio fill counts and total
         * byte/block span without any extent lookups.  Then do a single
         * speculative extent lookup on the very last logical block —
         * if it maps to exactly the right physical block, the entire
         * range is contiguous and we skip all intermediate lookups. */
        unsigned int cand_fills[BIO_MAX_VECS];
        cand_fills[0] = fill;
        int cand_len = 1;
        uint32 cand_bytes = run_bytes;
        ext4_lblk_t cand_last_lblock = first_iblock + fill - 1;

        while (i + cand_len < nr_pages && cand_len < BIO_MAX_VECS) {
            struct pcache_node *pn = pages[i + cand_len]->pcache.pcache_node;
            uint64 n_off = (uint64)pn->blkno * 512ULL;
            unsigned int n_fill = pn->page_count;
            for (unsigned int p = 0; p < pn->page_count; p++) {
                if (n_off + (uint64)p * PGSIZE >= inode_size) {
                    n_fill = p;
                    break;
                }
            }
            if (n_fill < 2)
                break;
            if (cand_bytes + n_fill * PGSIZE > BIO_MAX_SIZE)
                break;
            cand_fills[cand_len] = n_fill;
            cand_bytes += n_fill * PGSIZE;
            cand_last_lblock += n_fill;
            cand_len++;
        }

        /* Speculative check: is the entire candidate range contiguous? */
        if (cand_len > 1) {
            ext4_fsblk_t spec_fblock;
            r = ext4_fs_get_inode_dblk_idx(&ref, cand_last_lblock,
                                           &spec_fblock, true);
            if (r == EOK &&
                spec_fblock == first_fblock + (cand_last_lblock - first_iblock)) {
                /* Entire range confirmed contiguous — accept all */
                run_len = cand_len;
                run_bytes = cand_bytes;
            } else {
                /* Fall back to per-folio contiguity check */
                while (i + run_len < nr_pages && run_len < cand_len) {
                    struct pcache_node *pn =
                        pages[i + run_len]->pcache.pcache_node;
                    uint64 n_off = (uint64)pn->blkno * 512ULL;
                    unsigned int n_fill = cand_fills[run_len];

                    ext4_lblk_t n_iblk = (ext4_lblk_t)(n_off / block_size);
                    ext4_fsblk_t n_fblk;
                    r = ext4_fs_get_inode_dblk_idx(&ref, n_iblk,
                                                   &n_fblk, true);
                    if (r != EOK || n_fblk != expected_next)
                        break;

                    ext4_fsblk_t n_last;
                    r = ext4_fs_get_inode_dblk_idx(&ref, n_iblk + n_fill - 1,
                                                   &n_last, true);
                    if (r != EOK || n_last != n_fblk + n_fill - 1)
                        break;

                    expected_next = n_fblk + n_fill;
                    run_bytes += n_fill * PGSIZE;
                    run_len++;
                }
            }
        }

        /* Create one merged BIO for the entire contiguous run */
        uint64 pba = ((uint64)first_fblock * block_size +
                      esb->bdev.part_offset) / esb->bdev_iface.ph_bsize;

        struct bio *bio = bio_alloc(blk, run_len, false, NULL, NULL);
        if (IS_ERR_OR_NULL(bio)) {
            i += run_len;
            continue;
        }

        bio->blkno = pba;

        bool ok = true;
        for (int j = 0; j < run_len; j++) {
            struct pcache_node *pj = pages[i + j]->pcache.pcache_node;
            uint64 oj = (uint64)pj->blkno * 512ULL;
            unsigned int fj = pj->page_count;
            for (unsigned int p = 0; p < pj->page_count; p++) {
                if (oj + (uint64)p * PGSIZE >= inode_size) {
                    fj = p;
                    break;
                }
            }
            r = bio_add_folio(bio, pj->folio, fj * PGSIZE, 0);
            if (r != 0) {
                ok = false;
                break;
            }
        }

        if (!ok || blkdev_submit_bio(blk, bio) != 0) {
            bio_release(bio);
            i += run_len;
            continue;
        }

        /* Distribute BIO references: first page gets the original,
         * remaining pages get bio_dup'd references.  This way each
         * page can independently bio_await + bio_release. */
        bios[i] = bio;
        for (int j = 1; j < run_len; j++) {
            bio_dup(bio);
            bios[i + j] = bio;
        }

        submitted += run_len;
        i += run_len;
    }

    ext4_fs_put_inode_ref(&ref);
    ext4fs_unlock(esb);

    return submitted;
}

static struct pcache_ops ext4fs_pcache_ops = {
    .read_page = ext4fs_pcache_read_page,
    .write_page = ext4fs_pcache_write_page,
    .read_folio = ext4fs_pcache_read_folio,
    .write_folio = ext4fs_pcache_write_folio,
    .submit_readahead = ext4fs_submit_readahead,
};

void ext4fs_inode_pcache_init(struct vfs_inode *inode)
{
    if (inode == NULL || !S_ISREG(inode->mode) || inode->i_data.active)
        return;

    struct pcache *pc = &inode->i_data;
    memset(pc, 0, sizeof(*pc));
    pc->ops = &ext4fs_pcache_ops;
    pc->blk_count = ext4fs_pcache_blk_count(inode->size);

    if (pcache_init(pc) != 0)
        return;

    pc->private_data = inode;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* File read                                                                   */
/* ──────────────────────────────────────────────────────────────────────────── */

ssize_t ext4fs_file_read(struct vfs_file *file, char *buf, size_t count,
                         bool user)
{
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (!S_ISREG(inode->mode))
        return -EINVAL;

    if (inode->i_data.active) {
        /*
         * Delegate to readv for pcache-backed reads.  readv sets
         * file->f_pos itself, but our caller (vfs_fileread) will ALSO
         * advance f_pos by the return value.  Save/restore f_pos so the
         * caller's "f_pos += ret" is the only advance.
         */
        loff_t saved_pos = file->f_pos;
        struct kernel_iovec iov = {
            .iov_base = (uint64)buf,
            .iov_len = count,
        };
        struct iov_iter iter;
        iov_iter_init(&iter, &iov, 1, count);
        ssize_t ret = ext4fs_file_readv(file, &iter, user);
        file->f_pos = saved_pos;
        return ret;
    }

    struct ext4fs_superblock *esb = ext4fs_get_esb(inode->sb);
    struct ext4_fs *fs = &esb->ext4fs;
    uint32_t block_size = ext4_sb_get_block_size(&fs->sb);

    /* Snapshot f_pos and size — see locking rationale above write_folio */
    vfs_ilock(inode);
    loff_t pos = file->f_pos;
    loff_t isize = inode->size;
    vfs_iunlock(inode);

    if (pos >= isize)
        return 0; /* EOF */
    if ((uint64_t)(pos + count) > (uint64_t)isize)
        count = (size_t)(isize - pos);

    ext4fs_lock(esb);

    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)inode->ino, &ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return -r;
    }

    size_t bytes_read = 0;
    while (bytes_read < count) {
        ext4_lblk_t iblock = (ext4_lblk_t)((pos + bytes_read) / block_size);
        uint off = (uint)((pos + bytes_read) % block_size);
        uint n = block_size - off;
        if (n > count - bytes_read)
            n = (uint)(count - bytes_read);

        ext4_fsblk_t fblock;
        r = ext4_fs_get_inode_dblk_idx(&ref, iblock, &fblock, true);
        if (r != EOK) {
            if (bytes_read == 0)
                bytes_read = (size_t)(-r);
            break;
        }

        if (fblock == 0) {
            /* Sparse block — zero-fill */
            if (user) {
                char zeros[64];
                memset(zeros, 0, sizeof(zeros));
                uint rem = n;
                while (rem > 0) {
                    uint chunk = rem > sizeof(zeros) ? sizeof(zeros) : rem;
                    if (vm_copyout(current->vm,
                                   (uint64)(buf + bytes_read + (n - rem)),
                                   zeros, chunk) < 0) {
                        if (bytes_read == 0)
                            bytes_read = (size_t)(-EFAULT);
                        goto out;
                    }
                    rem -= chunk;
                }
            } else {
                memset(buf + bytes_read, 0, n);
            }
            bytes_read += n;
            continue;
        }

        struct ext4_block blk;
        r = ext4_block_get(&esb->bdev, &blk, fblock);
        if (r != EOK) {
            if (bytes_read == 0)
                bytes_read = (size_t)(-EIO);
            break;
        }

        if (user) {
            if (vm_copyout(current->vm, (uint64)(buf + bytes_read),
                           (char *)blk.data + off, n) < 0) {
                ext4_block_set(&esb->bdev, &blk);
                if (bytes_read == 0)
                    bytes_read = (size_t)(-EFAULT);
                break;
            }
        } else {
            memcpy(buf + bytes_read, (char *)blk.data + off, n);
        }

        ext4_block_set(&esb->bdev, &blk);
        bytes_read += n;
    }

out:
    ext4_fs_put_inode_ref(&ref);
    ext4fs_unlock(esb);

    /* Update atime if we read anything */
    if ((ssize_t)bytes_read > 0)
        inode->atime = goldfish_rtc_read_sec();

    return (ssize_t)bytes_read;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* File write                                                                  */
/* ──────────────────────────────────────────────────────────────────────────── */

ssize_t ext4fs_file_write(struct vfs_file *file, const char *buf, size_t count,
                          bool user)
{
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (!S_ISREG(inode->mode))
        return -EINVAL;

    /* Initialize pcache if needed — writes go through pcache now */
    if (!inode->i_data.active)
        ext4fs_inode_pcache_init(inode);

    struct pcache *pc = &inode->i_data;
    if (!pc->active) {
        /* Fallback: pcache init failed, use legacy direct write */
        goto legacy_write;
    }

    {
        /* pcache-based write: data goes into pcache folios, dirty pages
         * are flushed asynchronously by pcache_write_folio callback. */
        vfs_ilock(inode);
        loff_t pos = file->f_pos;
        loff_t end_pos = pos + count;

        /* Grow blk_count if the write extends beyond current pcache range */
        uint64 needed_blk_count = ext4fs_pcache_blk_count(end_pos);
        if (needed_blk_count > pc->blk_count)
            pc->blk_count = needed_blk_count;

        vfs_iunlock(inode);

        size_t bytes_written = 0;
        while (bytes_written < count) {
            uint64 blkno_512 = (pos / PGSIZE) * EXT4FS_BLKS_PER_PAGE;
            page_t *page = pcache_get_page(pc, blkno_512);
            if (page == NULL) {
                if (bytes_written == 0)
                    return -ENOMEM;
                break;
            }

            struct pcache_node *pcn = page->pcache.pcache_node;
            uint64 folio_start = (uint64)pcn->blkno * 512;
            uint64 folio_off = (uint64)pos - folio_start;
            size_t chunk = pcn->size - (size_t)folio_off;
            if (chunk > count - bytes_written)
                chunk = count - bytes_written;

            /* Skip the disk read if overwriting the entire folio */
            int ret;
            bool full_folio = (folio_off == 0 && chunk >= pcn->size);
            bool need_commit = false;
            if (full_folio) {
                ret = pcache_begin_full_page_write(pc, page);
                if (ret < 0) {
                    pcache_put_page(pc, page);
                    if (bytes_written == 0)
                        return ret;
                    break;
                }
                need_commit = (ret == 1);
            } else {
                ret = pcache_read_page(pc, page);
                if (ret != 0) {
                    pcache_put_page(pc, page);
                    if (bytes_written == 0)
                        return ret;
                    break;
                }
            }

            char *data = (char *)pcn->data + folio_off;
            bool copy_ok = true;
            if (user) {
                if (vm_copyin(current->vm, data,
                              (uint64)(buf + bytes_written), chunk) < 0) {
                    copy_ok = false;
                }
            } else {
                memmove(data, buf + bytes_written, chunk);
            }

            if (need_commit)
                pcache_end_full_page_write(pc, page, copy_ok);

            if (!copy_ok) {
                pcache_put_page(pc, page);
                if (bytes_written == 0)
                    return -EFAULT;
                break;
            }

            pcache_mark_page_dirty(pc, page);
            pcache_put_page(pc, page);

            bytes_written += chunk;
            pos += chunk;
        }

        /* Update file size if extended, and mark inode dirty so
         * vfs_iput syncs metadata (size, timestamps) to disk. */
        if (bytes_written > 0) {
            vfs_ilock(inode);
            if (pos > inode->size)
                inode->size = pos;
            uint64 now = goldfish_rtc_read_sec();
            inode->mtime = now;
            inode->ctime = now;
            inode->dirty = 1;
            vfs_iunlock(inode);
        }

        return (ssize_t)bytes_written;
    }

legacy_write:
    ext4fs_inode_map_cache_invalidate(inode);

    struct ext4fs_superblock *esb = ext4fs_get_esb(inode->sb);
    struct ext4_fs *fs = &esb->ext4fs;
    uint32_t block_size = ext4_sb_get_block_size(&fs->sb);

    vfs_ilock(inode);
    ext4fs_lock(esb);

    loff_t pos = file->f_pos;

    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)inode->ino, &ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        vfs_iunlock(inode);
        return -r;
    }

    /* Enable write-back caching for the duration of the write */
    ext4_block_cache_write_back(&esb->bdev, 1);

    /* Compute the number of existing file blocks (for allocation decision).
     * Save the original on-disk size BEFORE the write loop because
     * ext4_fs_append_inode_dblk inflates the on-disk inode size to
     * block-aligned values.  We need the original size to decide whether
     * the write extended the file. */
    uint64_t orig_ondisk_size = ext4_inode_get_size(&fs->sb, ref.inode);
    uint32_t ifile_blocks =
        (uint32_t)((orig_ondisk_size + block_size - 1) /
                   block_size);

    size_t bytes_written = 0;
    while (bytes_written < count) {
        ext4_lblk_t iblock = (ext4_lblk_t)((pos + bytes_written) / block_size);
        uint off = (uint)((pos + bytes_written) % block_size);
        uint n = block_size - off;
        if (n > count - bytes_written)
            n = (uint)(count - bytes_written);

        /*
         * Block allocation: follow the same pattern as lwext4's ext4_fwrite.
         * - For blocks within the current file size, use init_inode_dblk_idx
         *   (lookup only — works for both extent and indirect-block).
         * - For blocks beyond the current file size, use append_inode_dblk
         *   which allocates a physical block, sets the mapping, AND updates
         *   the on-disk inode size.
         */
        ext4_fsblk_t fblock;
        if (iblock < ifile_blocks) {
            r = ext4_fs_init_inode_dblk_idx(&ref, iblock, &fblock);
        } else {
            ext4_lblk_t appended_iblock;
            r = ext4_fs_append_inode_dblk(&ref, &fblock, &appended_iblock);
            if (r == EOK)
                ifile_blocks++;  /* track newly allocated block */
        }
        if (r != EOK) {
            if (bytes_written == 0)
                bytes_written = (size_t)(-r);
            break;
        }

        struct ext4_block blk;
        /* If we're writing the entire block, no need to read it first */
        if (off == 0 && n == block_size)
            r = ext4_block_get_noread(&esb->bdev, &blk, fblock);
        else
            r = ext4_block_get(&esb->bdev, &blk, fblock);

        if (r != EOK) {
            if (bytes_written == 0)
                bytes_written = (size_t)(-EIO);
            break;
        }

        if (user) {
            if (vm_copyin(current->vm, (char *)blk.data + off,
                          (uint64)(buf + bytes_written), n) < 0) {
                ext4_block_set(&esb->bdev, &blk);
                if (bytes_written == 0)
                    bytes_written = (size_t)(-EFAULT);
                break;
            }
        } else {
            memcpy((char *)blk.data + off, buf + bytes_written, n);
        }

        /* Mark block dirty and up-to-date.
         * ext4_bcache_set_dirty sets both BC_UPTODATE and BC_DIRTY.
         * BC_UPTODATE is required so ext4_bcache_free will flush
         * (or insert into the dirty list) rather than drop the buffer. */
        ext4_bcache_set_dirty(blk.buf);
        ext4_block_set(&esb->bdev, &blk);
        bytes_written += n;
    }

    /* Update file size if we extended it.
     * ext4_fs_append_inode_dblk inflates the on-disk inode size by whole
     * blocks.  Compare against orig_ondisk_size (saved before the loop)
     * to detect extension, then set the exact byte count so the file
     * doesn't appear larger than what was actually written. */
    uint64_t new_pos = (uint64_t)pos + bytes_written;
    if (new_pos > orig_ondisk_size) {
        ext4_inode_set_size(ref.inode, new_pos);
        ref.dirty = true;
    }
    if ((loff_t)new_pos > inode->size)
        inode->size = (loff_t)new_pos;

    /* Update n_blocks from on-disk inode */
    inode->n_blocks = ext4_inode_get_blocks_count(&fs->sb, ref.inode);

    ext4_fs_put_inode_ref(&ref);

    /* Flush write-back cache */
    ext4_block_cache_write_back(&esb->bdev, 0);

    /* Update mtime/ctime if we wrote anything */
    if ((ssize_t)bytes_written > 0) {
        uint64 now = goldfish_rtc_read_sec();
        inode->mtime = now;
        inode->ctime = now;
    }

    ext4fs_unlock(esb);
    vfs_iunlock(inode);
    return (ssize_t)bytes_written;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* File seek                                                                   */
/* ──────────────────────────────────────────────────────────────────────────── */

loff_t ext4fs_file_llseek(struct vfs_file *file, loff_t offset, int whence)
{
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    loff_t new_pos;

    switch (whence) {
    case SEEK_SET:
        new_pos = offset;
        break;
    case SEEK_CUR:
        new_pos = file->f_pos + offset;
        break;
    case SEEK_END:
        vfs_ilock(inode);
        new_pos = inode->size + offset;
        vfs_iunlock(inode);
        break;
    default:
        return -EINVAL;
    }

    if (new_pos < 0)
        return -EINVAL;

    return new_pos;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* File fsync / fflush                                                         */
/* ──────────────────────────────────────────────────────────────────────────── */

static int ext4fs_file_fsync(struct vfs_file *file, loff_t start, loff_t len)
{
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    (void)start;
    (void)len;

    if (inode == NULL || inode->sb == NULL)
        return 0;

    struct ext4fs_superblock *esb = ext4fs_get_esb(inode->sb);

    /* Enable ext4 bcache write-back batching BEFORE pcache_flush.
     * Each write_folio call does its own write_back(1)/write_back(0)
     * pair, which would flush the ext4 block cache to disk per-folio.
     * By holding an outer write_back(1), the counter never reaches 0
     * during individual write_folio calls, so dirty blocks accumulate
     * in the ext4 bcache and get flushed ONCE at the end.  This turns
     * 256 separate disk flush operations into 1. */
    struct pcache *pc = &inode->i_data;
    if (pc->active && pc->dirty_count > 0) {
        ext4fs_lock(esb);
        ext4_block_cache_write_back(&esb->bdev, 1);
        ext4fs_unlock(esb);

        pcache_flush(pc);

        /* Drop the outer write-back hold.  If counter reaches 0 this
         * flushes all accumulated dirty blocks to disk in one batch. */
        ext4fs_lock(esb);
        ext4_block_cache_write_back(&esb->bdev, 0);
        ext4fs_unlock(esb);
    }

    if (inode->dirty)
        vfs_sync_inode(inode);

    ext4fs_lock(esb);
    int r = ext4_block_cache_flush(&esb->bdev) == EOK ? 0 : -EIO;
    ext4fs_unlock(esb);
    if (r != 0)
        return r;

    return blkdev_flush(esb->xv6_blkdev);
}

static int ext4fs_file_fflush(struct vfs_file *file)
{
    return ext4fs_file_fsync(file, 0, 0);
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* mmap writeback: write a dirty page at an explicit file offset               */
/* ──────────────────────────────────────────────────────────────────────────── */

/**
 * ext4fs_file_writepage - Write back a single dirty page.
 *
 * Called from the VM during munmap / MADV_DONTNEED / msync for MAP_SHARED
 * file mappings whose fault handler returns anonymous pages.  @offset is
 * the byte position within the file, @data points to a kernel page, and
 * @len is the number of valid bytes (<= PGSIZE).
 *
 * This is a positional write: it does NOT read or modify file->f_pos.
 */
static int ext4fs_file_writepage(struct vfs_file *file, loff_t offset,
                                 const void *data, size_t len)
{
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (inode == NULL || !S_ISREG(inode->mode))
        return -EINVAL;

    ext4fs_inode_map_cache_invalidate(inode);

    struct pcache *pc = &inode->i_data;
    if (pc->active) {
        page_t *pg = __pa_to_page((uint64)data);
        page_t *pc_head = page_pcache_head(pg);
        if (pc_head != NULL) {
            return pcache_mark_page_dirty(pc, pc_head);
        }
        uint64 blkno_512 = offset / 512ULL;
        page_t *pcpage = pcache_get_page(pc, blkno_512);
        if (pcpage == NULL)
            return -ENOMEM;

        g_ext4_pcache_readahead_pages += 1;

        struct pcache_node *pcn = pcpage->pcache.pcache_node;
        /* Compute the sub-page offset for multi-page folios. */
        uint64 folio_byte_off = blkno_512 * 512ULL - (uint64)pcn->blkno * 512ULL;

        /*
         * Only use the prepare (zero-fill) fast-path when the write
         * covers the ENTIRE folio.  For partial writes (e.g. a single
         * PGSIZE page within a multi-page folio), read the existing
         * folio content first so the other sub-pages retain valid data.
         */
        int ret;
        bool full_folio = (folio_byte_off == 0 && len >= pcn->size);
        if (full_folio)
            ret = pcache_prepare_write_page(pc, pcpage);
        else
            ret = pcache_read_page(pc, pcpage);
        if (ret != 0) {
            pcache_put_page(pc, pcpage);
            return ret;
        }

        memcpy((char *)pcn->data + folio_byte_off, data, len);
        if (len < PGSIZE)
            memset((char *)pcn->data + folio_byte_off + len, 0, PGSIZE - len);

        ret = pcache_mark_page_dirty(pc, pcpage);
        pcache_put_page(pc, pcpage);
        return ret;
    }

    struct ext4fs_superblock *esb = ext4fs_get_esb(inode->sb);
    struct ext4_fs *fs = &esb->ext4fs;
    uint32_t block_size = ext4_sb_get_block_size(&fs->sb);

    vfs_ilock(inode);
    ext4fs_lock(esb);

    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)inode->ino, &ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        vfs_iunlock(inode);
        return -r;
    }

    ext4_block_cache_write_back(&esb->bdev, 1);

    uint64_t orig_ondisk_size = ext4_inode_get_size(&fs->sb, ref.inode);
    uint32_t ifile_blocks =
        (uint32_t)((orig_ondisk_size + block_size - 1) / block_size);

    size_t done = 0;
    int err = 0;
    while (done < len) {
        ext4_lblk_t iblock =
            (ext4_lblk_t)(((uint64_t)offset + done) / block_size);
        uint off = (uint)(((uint64_t)offset + done) % block_size);
        uint n = block_size - off;
        if (n > len - done)
            n = (uint)(len - done);

        ext4_fsblk_t fblock;
        if (iblock < ifile_blocks) {
            r = ext4_fs_init_inode_dblk_idx(&ref, iblock, &fblock);
        } else {
            ext4_lblk_t appended_iblock;
            r = ext4_fs_append_inode_dblk(&ref, &fblock, &appended_iblock);
            if (r == EOK)
                ifile_blocks++;
        }
        if (r != EOK) {
            err = -r;
            break;
        }

        struct ext4_block blk;
        if (off == 0 && n == block_size)
            r = ext4_block_get_noread(&esb->bdev, &blk, fblock);
        else
            r = ext4_block_get(&esb->bdev, &blk, fblock);
        if (r != EOK) {
            err = -EIO;
            break;
        }

        memcpy((char *)blk.data + off, (const char *)data + done, n);
        ext4_bcache_set_dirty(blk.buf);
        ext4_block_set(&esb->bdev, &blk);
        done += n;
    }

    ext4_fs_put_inode_ref(&ref);
    ext4_block_cache_write_back(&esb->bdev, 0);

    ext4fs_unlock(esb);
    vfs_iunlock(inode);
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* File-backed mmap fault handler                                              */
/* ──────────────────────────────────────────────────────────────────────────── */

/**
 * ext4fs_file_fault - demand-page a single page for a file-backed mapping
 *
 * Called from the page-fault path (vm rlock held, pgtable spinlock NOT held).
 * Uses the per-inode page cache when available so full-page faults can map the
 * cached page directly; falls back to an anonymous copy for the trailing
 * partial page or when the page cache is inactive. Returns NULL on failure.
 */
static void *ext4fs_file_fault(struct vfs_file *file, struct vma *vma,
                               uint64 va)
{
    uint64 fault_start = r_time();
    g_ext4_fault_calls += 1;

    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (inode == NULL)
        return NULL;

    struct pcache *pc = &inode->i_data;

    if (!pc->active)
        ext4fs_inode_pcache_init(inode);

    if (pc->active) {
        uint64 file_off = vma->pgoff + (va - vma->start);
        uint64 inode_size;
        uint64 bytes_to_read;

        inode_size = (uint64)READ_ONCE(inode->size);
        if (file_off >= inode_size) {
            void *pa = page_alloc(0, PAGE_TYPE_ANON);
            if (pa == NULL)
                return NULL;
            memset(pa, 0, PGSIZE);
            return pa;
        }

        bytes_to_read = PGSIZE;
        if (file_off + PGSIZE > inode_size)
            bytes_to_read = inode_size - file_off;

        uint64 blkno_512 = file_off / 512ULL;
        page_t *pcpage = pcache_get_page(pc, blkno_512);
        if (pcpage == NULL)
            return NULL;

        int ret = pcache_read_page(pc, pcpage);
        if (ret != 0) {
            pcache_put_page(pc, pcpage);
            return NULL;
        }

        struct pcache_node *pcn = pcpage->pcache.pcache_node;
        /* Compute sub-page offset within multi-page folio. */
        uint64 folio_byte_off = blkno_512 * 512ULL -
                                (uint64)pcn->blkno * 512ULL;
        if (bytes_to_read == PGSIZE) {
            g_ext4_fault_zero_copy += 1;
            g_ext4_fault_ticks += r_time() - fault_start;
            return (char *)pcn->data + folio_byte_off;
        }

        g_ext4_fault_partial_copy += 1;

        void *pa = page_alloc(0, PAGE_TYPE_ANON);
        if (pa == NULL) {
            pcache_put_page(pc, pcpage);
            return NULL;
        }

        memcpy(pa, (char *)pcn->data + folio_byte_off, bytes_to_read);
        memset((char *)pa + bytes_to_read, 0, PGSIZE - bytes_to_read);

        pcache_put_page(pc, pcpage);
        g_ext4_fault_ticks += r_time() - fault_start;
        return pa;
    }

    struct ext4fs_superblock *esb = ext4fs_get_esb(inode->sb);
    struct ext4_fs *fs = &esb->ext4fs;
    uint32_t block_size = ext4_sb_get_block_size(&fs->sb);

    /* file_off is page-aligned (both pgoff and va are page-aligned) */
    uint64 file_off = vma->pgoff + (va - vma->start);

    void *pa = page_alloc(0, PAGE_TYPE_ANON);
    if (pa == NULL)
        return NULL;

    /* Snapshot inode size — see locking rationale above write_folio */
    vfs_ilock(inode);
    uint64 inode_size = (uint64)inode->size;
    vfs_iunlock(inode);

    /* Entirely beyond EOF — return a zero page */
    if (file_off >= inode_size) {
        memset(pa, 0, PGSIZE);
        return pa;
    }

    uint64 bytes_to_read = PGSIZE;
    if (file_off + PGSIZE > inode_size)
        bytes_to_read = inode_size - file_off;

    ext4fs_lock(esb);

    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)inode->ino, &ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        page_free(pa, 0);
        return NULL;
    }

    uint64 done = 0;
    while (done < bytes_to_read) {
        ext4_lblk_t iblock =
            (ext4_lblk_t)((file_off + done) / block_size);
        uint off = (uint)((file_off + done) % block_size);
        uint n = block_size - off;
        if (n > bytes_to_read - done)
            n = (uint)(bytes_to_read - done);

        ext4_fsblk_t fblock;
        r = ext4_fs_get_inode_dblk_idx(&ref, iblock, &fblock, true);
        if (r != EOK) {
            /* I/O error — give up */
            ext4_fs_put_inode_ref(&ref);
            ext4fs_unlock(esb);
            page_free(pa, 0);
            return NULL;
        }

        if (fblock == 0) {
            /* Sparse block — zero-fill this chunk */
            memset((char *)pa + done, 0, n);
        } else {
            struct ext4_block blk;
            r = ext4_block_get(&esb->bdev, &blk, fblock);
            if (r != EOK) {
                ext4_fs_put_inode_ref(&ref);
                ext4fs_unlock(esb);
                page_free(pa, 0);
                return NULL;
            }
            memcpy((char *)pa + done, (char *)blk.data + off, n);
            ext4_block_set(&esb->bdev, &blk);
        }

        done += n;
    }

    ext4_fs_put_inode_ref(&ref);
    ext4fs_unlock(esb);

    /* Zero-fill remainder if partial page */
    if (bytes_to_read < PGSIZE)
        memset((char *)pa + bytes_to_read, 0, PGSIZE - bytes_to_read);

    g_ext4_fault_ticks += r_time() - fault_start;

    return pa;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* VFS file operations table                                                   */
/* ──────────────────────────────────────────────────────────────────────────── */

/* ──────────────────────────────────────────────────────────────────────────── */
/* Vectored read (readv)                                                       */
/*                                                                             */
/* Takes inode lock + ext4fs lock once, iterates all iov_iter segments         */
/* within a single inode_ref open/close, reducing per-segment overhead.        */
/* ──────────────────────────────────────────────────────────────────────────── */

static ssize_t ext4fs_file_readv(struct vfs_file *file, struct iov_iter *iter,
                                 bool user)
{
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (!S_ISREG(inode->mode))
        return -EINVAL;

    if (inode->i_data.active) {
        struct pcache *pc = &inode->i_data;
        loff_t pos;
        loff_t isize;

        vfs_ilock(inode);
        pos = file->f_pos;
        isize = inode->size;
        vfs_iunlock(inode);

        ssize_t ret = pcache_readv(pc, iter, &pos, isize, user);
        if (ret > 0)
            inode->atime = goldfish_rtc_read_sec();
        file->f_pos = pos;
        return ret;
    }

    struct ext4fs_superblock *esb = ext4fs_get_esb(inode->sb);
    struct ext4_fs *fs = &esb->ext4fs;
    uint32_t block_size = ext4_sb_get_block_size(&fs->sb);

    vfs_ilock(inode);
    ext4fs_lock(esb);

    loff_t pos = file->f_pos;
    if (pos >= inode->size) {
        ext4fs_unlock(esb);
        vfs_iunlock(inode);
        return 0;
    }

    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)inode->ino, &ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        vfs_iunlock(inode);
        return -r;
    }

    ssize_t total = 0;

    while (iter->nr_segs > 0 && iter->count > 0 && pos < inode->size) {
        size_t seg_len = iter->iov->iov_len - iter->iov_off;
        if (seg_len == 0) { iov_iter_advance(iter, 0); continue; }
        uint64 base = iter->iov->iov_base + iter->iov_off;

        if (pos + (loff_t)seg_len > inode->size)
            seg_len = (size_t)(inode->size - pos);

        size_t seg_read = 0;
        while (seg_read < seg_len) {
            ext4_lblk_t iblock = (ext4_lblk_t)(pos / block_size);
            uint off = (uint)(pos % block_size);
            uint n = block_size - off;
            if (n > seg_len - seg_read)
                n = (uint)(seg_len - seg_read);

            ext4_fsblk_t fblock;
            r = ext4_fs_get_inode_dblk_idx(&ref, iblock, &fblock, true);
            if (r != EOK) {
                if (total == 0) total = -r;
                goto out;
            }

            if (fblock == 0) {
                /* Sparse block — zero-fill */
                if (user) {
                    char zeros[64];
                    memset(zeros, 0, sizeof(zeros));
                    uint rem = n;
                    while (rem > 0) {
                        uint chunk = rem > sizeof(zeros) ? sizeof(zeros) : rem;
                        if (vm_copyout(current->vm,
                                       (uint64)(base + seg_read + (n - rem)),
                                       zeros, chunk) < 0) {
                            if (total == 0) total = -EFAULT;
                            goto out;
                        }
                        rem -= chunk;
                    }
                } else {
                    memset((char *)(base + seg_read), 0, n);
                }
            } else {
                struct ext4_block blk;
                r = ext4_block_get(&esb->bdev, &blk, fblock);
                if (r != EOK) {
                    if (total == 0) total = -EIO;
                    goto out;
                }
                if (user) {
                    if (vm_copyout(current->vm, (uint64)(base + seg_read),
                                   (char *)blk.data + off, n) < 0) {
                        ext4_block_set(&esb->bdev, &blk);
                        if (total == 0) total = -EFAULT;
                        goto out;
                    }
                } else {
                    memcpy((char *)(base + seg_read), (char *)blk.data + off, n);
                }
                ext4_block_set(&esb->bdev, &blk);
            }
            seg_read += n;
            pos += n;
        }
        total += seg_read;
        iov_iter_advance(iter, seg_read);
        if (seg_read < seg_len) break;
    }

out:
    ext4_fs_put_inode_ref(&ref);
    ext4fs_unlock(esb);

    if (total > 0)
        inode->atime = goldfish_rtc_read_sec();

    vfs_iunlock(inode);
    file->f_pos = pos;
    return total;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Vectored write (writev)                                                     */
/* ──────────────────────────────────────────────────────────────────────────── */

static ssize_t ext4fs_file_writev(struct vfs_file *file, struct iov_iter *iter,
                                  bool user)
{
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (!S_ISREG(inode->mode))
        return -EINVAL;

    /* Initialize pcache if needed */
    if (!inode->i_data.active)
        ext4fs_inode_pcache_init(inode);

    struct pcache *pc = &inode->i_data;
    if (pc->active) {
        /* pcache-based writev: use pcache_writev for efficient batched writes */
        vfs_ilock(inode);
        loff_t pos = file->f_pos;

        /* Estimate final position for blk_count growth */
        loff_t end_pos = pos + (loff_t)iter->count;
        uint64 needed_blk_count = ext4fs_pcache_blk_count(end_pos);
        if (needed_blk_count > pc->blk_count)
            pc->blk_count = needed_blk_count;

        vfs_iunlock(inode);

        ssize_t ret = pcache_writev(pc, iter, &pos, user);

        if (ret > 0 && pos > inode->size) {
            vfs_ilock(inode);
            if (pos > inode->size)
                inode->size = pos;
            vfs_iunlock(inode);
        }

        /* Mark inode dirty so vfs_iput syncs metadata (size) to disk */
        if (ret > 0) {
            vfs_ilock(inode);
            uint64 now = goldfish_rtc_read_sec();
            inode->mtime = now;
            inode->ctime = now;
            inode->dirty = 1;
            vfs_iunlock(inode);
        }

        file->f_pos = pos;
        return ret;
    }

    ext4fs_inode_map_cache_invalidate(inode);

    struct ext4fs_superblock *esb = ext4fs_get_esb(inode->sb);
    struct ext4_fs *fs = &esb->ext4fs;
    uint32_t block_size = ext4_sb_get_block_size(&fs->sb);

    vfs_ilock(inode);
    ext4fs_lock(esb);

    loff_t pos = file->f_pos;

    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)inode->ino, &ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        vfs_iunlock(inode);
        return -r;
    }

    ext4_block_cache_write_back(&esb->bdev, 1);

    uint64_t orig_ondisk_size = ext4_inode_get_size(&fs->sb, ref.inode);
    uint32_t ifile_blocks =
        (uint32_t)((orig_ondisk_size + block_size - 1) / block_size);

    ssize_t total = 0;

    while (iter->nr_segs > 0 && iter->count > 0) {
        size_t seg_len = iter->iov->iov_len - iter->iov_off;
        if (seg_len == 0) { iov_iter_advance(iter, 0); continue; }
        uint64 base = iter->iov->iov_base + iter->iov_off;

        size_t seg_written = 0;
        while (seg_written < seg_len) {
            ext4_lblk_t iblock = (ext4_lblk_t)(pos / block_size);
            uint off = (uint)(pos % block_size);
            uint n = block_size - off;
            if (n > seg_len - seg_written)
                n = (uint)(seg_len - seg_written);

            ext4_fsblk_t fblock;
            if (iblock < ifile_blocks) {
                r = ext4_fs_init_inode_dblk_idx(&ref, iblock, &fblock);
            } else {
                ext4_lblk_t appended_iblock;
                r = ext4_fs_append_inode_dblk(&ref, &fblock, &appended_iblock);
                if (r == EOK)
                    ifile_blocks++;
            }
            if (r != EOK) {
                if (total == 0) total = -r;
                goto out_w;
            }

            struct ext4_block blk;
            if (off == 0 && n == block_size)
                r = ext4_block_get_noread(&esb->bdev, &blk, fblock);
            else
                r = ext4_block_get(&esb->bdev, &blk, fblock);
            if (r != EOK) {
                if (total == 0) total = -EIO;
                goto out_w;
            }

            if (user) {
                if (vm_copyin(current->vm, (char *)blk.data + off,
                              (uint64)(base + seg_written), n) < 0) {
                    ext4_block_set(&esb->bdev, &blk);
                    if (total == 0) total = -EFAULT;
                    goto out_w;
                }
            } else {
                memcpy((char *)blk.data + off, (const char *)(base + seg_written), n);
            }
            ext4_bcache_set_dirty(blk.buf);
            ext4_block_set(&esb->bdev, &blk);

            seg_written += n;
            pos += n;
        }
        total += seg_written;
        iov_iter_advance(iter, seg_written);
        if (seg_written < seg_len) break;
    }

out_w:
    {
        uint64_t new_pos = (uint64_t)pos;
        if (new_pos > orig_ondisk_size) {
            ext4_inode_set_size(ref.inode, new_pos);
            ref.dirty = true;
        }
        if ((loff_t)new_pos > inode->size)
            inode->size = (loff_t)new_pos;
        inode->n_blocks = ext4_inode_get_blocks_count(&fs->sb, ref.inode);
    }

    ext4_fs_put_inode_ref(&ref);
    ext4_block_cache_write_back(&esb->bdev, 0);

    if (total > 0) {
        uint64 now = goldfish_rtc_read_sec();
        inode->mtime = now;
        inode->ctime = now;
    }

    ext4fs_unlock(esb);
    vfs_iunlock(inode);
    file->f_pos = pos;
    return total;
}

struct vfs_file_ops ext4fs_file_ops = {
    .read      = ext4fs_file_read,
    .write     = ext4fs_file_write,
    .llseek    = ext4fs_file_llseek,
    .release   = NULL,
    .fsync     = ext4fs_file_fsync,
    .fflush    = ext4fs_file_fflush,
    .fault     = ext4fs_file_fault,
    .prefault  = ext4fs_file_prefault,
    .writepage = ext4fs_file_writepage,
    .readv     = ext4fs_file_readv,
    .writev    = ext4fs_file_writev,
};
