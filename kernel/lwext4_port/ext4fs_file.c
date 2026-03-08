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
#include <smp/atomic.h>
#include "proc/tq.h"
#include "timer/timer.h"
#include "vfs/fs.h"
#include "vfs/stat.h"
#include "vfs/fcntl.h"
#include "ext4fs_private.h"
#include "vfs/uio.h"

#include <ext4_errno.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_super.h>
#include <ext4_blockdev.h>
#include <ext4_bcache.h>

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
        int r;
        struct ext4fs_inode *ei = ext4fs_inode_from_vfs(inode);

        if (ei != NULL) {
            int hit = 0;

            spin_lock(&ei->map_cache.lock);
            if (ei->map_cache.valid && iblock >= ei->map_cache.lblk_start &&
                iblock < ei->map_cache.lblk_start + ei->map_cache.len) {
                if (ei->map_cache.hole)
                    fblock = 0;
                else
                    fblock = (ext4_fsblk_t)(ei->map_cache.pblk_start +
                                            (iblock - ei->map_cache.lblk_start));
                hit = 1;
            }
            spin_unlock(&ei->map_cache.lock);

            if (!hit) {
                ext4_fsblk_t first_fblock;
                uint32 run_len = 1;

                r = ext4_fs_get_inode_dblk_idx(ref, iblock, &first_fblock, true);
                if (r != EOK)
                    return -r;

                for (ext4_lblk_t next = iblock + 1;
                     next < iblock + EXT4FS_MAP_CACHE_BLOCKS; next++) {
                    ext4_fsblk_t next_fblock;
                    r = ext4_fs_get_inode_dblk_idx(ref, next, &next_fblock, true);
                    if (r != EOK)
                        break;
                    if (first_fblock == 0) {
                        if (next_fblock != 0)
                            break;
                    } else if (next_fblock != first_fblock + (next - iblock)) {
                        break;
                    }
                    run_len++;
                }

                spin_lock(&ei->map_cache.lock);
                ei->map_cache.lblk_start = iblock;
                ei->map_cache.pblk_start = first_fblock;
                ei->map_cache.len = run_len;
                ei->map_cache.valid = 1;
                ei->map_cache.hole = (first_fblock == 0);
                spin_unlock(&ei->map_cache.lock);

                fblock = first_fblock;
            }
        } else {
            r = ext4_fs_get_inode_dblk_idx(ref, iblock, &fblock, true);
            if (r != EOK)
                return -r;
        }

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

    spin_lock(&pc->spinlock);
    page_lock_acquire(page);

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
    spin_lock(&pc->spinlock);
    page_lock_acquire(page);

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
    __atomic_fetch_add(&g_ext4_pcache_read_page_calls, 1, __ATOMIC_RELAXED);

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

    __atomic_fetch_add(&g_ext4_pcache_pages_filled, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_ext4_pcache_read_page_ticks, r_time() - read_start,
                       __ATOMIC_RELAXED);

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

    ext4fs_lock(esb);
    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)inode->ino, &ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return -r;
    }

    for (uint64 va = start_va; va < end_va; va += PGSIZE) {
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
        if (state == 0) {
            uint64 read_start = r_time();
            __atomic_fetch_add(&g_ext4_pcache_read_page_calls, 1,
                               __ATOMIC_RELAXED);

            r = ext4fs_fill_page_from_ref(fs, esb, inode, &ref, pcn->data,
                                          file_off, inode_size);

            if (r == 0)
                __atomic_fetch_add(&g_ext4_pcache_pages_filled, 1,
                                   __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_ext4_pcache_read_page_ticks,
                               r_time() - read_start, __ATOMIC_RELAXED);

            ext4fs_prefault_end_page(pc, pcpage, r == 0);
            if (r != 0 && ret == 0)
                ret = r;
        }

        pcache_put_page(pc, pcpage);
    }

    ext4_fs_put_inode_ref(&ref);
    ext4fs_unlock(esb);
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
    vfs_iunlock(inode);
    return err;
}

static struct pcache_ops ext4fs_pcache_ops = {
    .read_page = ext4fs_pcache_read_page,
    .write_page = ext4fs_pcache_write_page,
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
        struct kernel_iovec iov = {
            .iov_base = (uint64)buf,
            .iov_len = count,
        };
        struct iov_iter iter;
        iov_iter_init(&iter, &iov, 1, count);
        return ext4fs_file_readv(file, &iter, user);
    }

    struct ext4fs_superblock *esb = ext4fs_get_esb(inode->sb);
    struct ext4_fs *fs = &esb->ext4fs;
    uint32_t block_size = ext4_sb_get_block_size(&fs->sb);

    /* Lock inode to read size safely and prevent truncation during read */
    vfs_ilock(inode);
    ext4fs_lock(esb);

    loff_t pos = file->f_pos;
    if (pos >= inode->size) {
        ext4fs_unlock(esb);
        vfs_iunlock(inode);
        return 0; /* EOF */
    }
    if ((uint64_t)(pos + count) > (uint64_t)inode->size)
        count = (size_t)(inode->size - pos);

    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)inode->ino, &ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        vfs_iunlock(inode);
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

    vfs_iunlock(inode);
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

    ext4fs_inode_map_cache_invalidate(inode);

    if (inode->i_data.active)
        pcache_teardown(&inode->i_data);

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
    ext4fs_lock(esb);
    int r = ext4_block_cache_flush(&esb->bdev) == EOK ? 0 : -EIO;
    ext4fs_unlock(esb);
    return r;
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
        if (pg != NULL && PAGE_IS_TYPE(pg, PAGE_TYPE_PCACHE)) {
            return pcache_mark_page_dirty(pc, pg);
        }

        uint64 blkno_512 = offset / 512ULL;
        page_t *pcpage = pcache_get_page(pc, blkno_512);
        if (pcpage == NULL)
            return -ENOMEM;

        __atomic_fetch_add(&g_ext4_pcache_readahead_pages, 1,
                           __ATOMIC_RELAXED);
        int ret = pcache_prepare_write_page(pc, pcpage);
        if (ret != 0) {
            pcache_put_page(pc, pcpage);
            return ret;
        }

        struct pcache_node *pcn = pcpage->pcache.pcache_node;
        memcpy(pcn->data, data, len);
        if (len < PGSIZE)
            memset((char *)pcn->data + len, 0, PGSIZE - len);

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
    __atomic_fetch_add(&g_ext4_fault_calls, 1, __ATOMIC_RELAXED);

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
        if (bytes_to_read == PGSIZE) {
            __atomic_fetch_add(&g_ext4_fault_zero_copy, 1,
                               __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_ext4_fault_ticks, r_time() - fault_start,
                               __ATOMIC_RELAXED);
            return pcn->data;
        }

        __atomic_fetch_add(&g_ext4_fault_partial_copy, 1,
                           __ATOMIC_RELAXED);

        void *pa = page_alloc(0, PAGE_TYPE_ANON);
        if (pa == NULL) {
            pcache_put_page(pc, pcpage);
            return NULL;
        }

        memcpy(pa, pcn->data, bytes_to_read);
        memset((char *)pa + bytes_to_read, 0, PGSIZE - bytes_to_read);

        pcache_put_page(pc, pcpage);
        __atomic_fetch_add(&g_ext4_fault_ticks, r_time() - fault_start,
                           __ATOMIC_RELAXED);
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

    vfs_ilock(inode);

    /* Entirely beyond EOF — return a zero page */
    if (file_off >= (uint64)inode->size) {
        vfs_iunlock(inode);
        memset(pa, 0, PGSIZE);
        return pa;
    }

    uint64 bytes_to_read = PGSIZE;
    if (file_off + PGSIZE > (uint64)inode->size)
        bytes_to_read = (uint64)inode->size - file_off;

    ext4fs_lock(esb);

    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)inode->ino, &ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        vfs_iunlock(inode);
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
            vfs_iunlock(inode);
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
                vfs_iunlock(inode);
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
    vfs_iunlock(inode);

    /* Zero-fill remainder if partial page */
    if (bytes_to_read < PGSIZE)
        memset((char *)pa + bytes_to_read, 0, PGSIZE - bytes_to_read);

    __atomic_fetch_add(&g_ext4_fault_ticks, r_time() - fault_start,
                       __ATOMIC_RELAXED);

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

    ext4fs_inode_map_cache_invalidate(inode);

    if (inode->i_data.active)
        pcache_teardown(&inode->i_data);

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
