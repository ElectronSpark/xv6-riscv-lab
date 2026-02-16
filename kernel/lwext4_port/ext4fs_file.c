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
#include "vfs/fs.h"
#include "vfs/stat.h"
#include "vfs/fcntl.h"
#include "ext4fs_private.h"

#include <ext4_errno.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_super.h>
#include <ext4_blockdev.h>
#include <ext4_bcache.h>

/* ──────────────────────────────────────────────────────────────────────────── */
/* File read                                                                   */
/* ──────────────────────────────────────────────────────────────────────────── */

ssize_t ext4fs_file_read(struct vfs_file *file, char *buf, size_t count,
                         bool user)
{
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (!S_ISREG(inode->mode))
        return -EINVAL;

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
        r = ext4_fs_get_inode_dblk_idx(&ref, iblock, &fblock, false);
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

    /* Compute the number of existing file blocks (for allocation decision) */
    uint32_t ifile_blocks =
        (uint32_t)((ext4_inode_get_size(&fs->sb, ref.inode) + block_size - 1) /
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

        /* Mark block dirty */
        blk.buf->flags |= 0x02; /* BC_DIRTY */
        ext4_block_set(&esb->bdev, &blk);
        bytes_written += n;
    }

    /* Update file size if we extended it.
     * ext4_fs_append_inode_dblk already bumps the on-disk inode size by
     * whole blocks, so we just need to ensure the final size reflects the
     * actual number of bytes written (not rounded up to block boundary). */
    uint64_t new_pos = (uint64_t)pos + bytes_written;
    uint64_t ondisk_size = ext4_inode_get_size(&fs->sb, ref.inode);
    if (new_pos > ondisk_size) {
        ext4_inode_set_size(ref.inode, new_pos);
        ref.dirty = true;
    }
    if ((loff_t)new_pos > inode->size)
        inode->size = (loff_t)new_pos;

    ext4_fs_put_inode_ref(&ref);

    /* Flush write-back cache */
    ext4_block_cache_write_back(&esb->bdev, 0);

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
/* VFS file operations table                                                   */
/* ──────────────────────────────────────────────────────────────────────────── */

struct vfs_file_ops ext4fs_file_ops = {
    .read    = ext4fs_file_read,
    .write   = ext4fs_file_write,
    .llseek  = ext4fs_file_llseek,
    .release = NULL,
    .fsync   = ext4fs_file_fsync,
    .fflush  = ext4fs_file_fflush,
    .fault   = NULL,
};
