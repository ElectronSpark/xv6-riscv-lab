/*
 * xv6fs file operations
 *
 * TRANSACTION MANAGEMENT: FS-INTERNAL (not VFS-managed)
 * =====================================================
 * File write manages transactions internally because:
 * 1. Large writes require BATCHED transactions (multiple begin/end cycles)
 * 2. VFS holds inode lock before calling file->ops->write
 *
 * This is the "hybrid approach" documented in superblock.c:
 * - Metadata ops: VFS manages transactions via callbacks
 * - File ops: FS manages transactions internally (here)
 *
 * Lock ordering for file write: inode_mutex → transaction
 * (Reversed from metadata ops, but safe because different inodes are involved)
 *
 * See superblock.c "Transaction Callbacks" comment for full design explanation.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "lock/spinlock.h"
#include "lock/mutex_types.h"
#include "proc/proc.h"
#include <mm/vm.h>
#include "dev/buf.h"
#include "vfs/fs.h"
#include "vfs/stat.h"
#include "vfs/fcntl.h"
#include "../vfs_private.h"
#include "xv6fs_private.h"

/******************************************************************************
 * File read
 ******************************************************************************/

ssize_t xv6fs_file_read(struct vfs_file *file, char *buf, size_t count) {
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    struct xv6fs_inode *ip = container_of(inode, struct xv6fs_inode, vfs_inode);
    
    if (!S_ISREG(inode->mode)) {
        return -EINVAL;
    }
    
    loff_t pos = file->f_pos;
    if (pos >= inode->size) {
        return 0;  // EOF
    }
    if (pos + count > inode->size) {
        count = inode->size - pos;
    }
    
    size_t bytes_read = 0;
    while (bytes_read < count) {
        uint bn = pos / BSIZE;
        uint off = pos % BSIZE;
        uint n = BSIZE - off;
        if (n > count - bytes_read) {
            n = count - bytes_read;
        }
        
        uint addr = xv6fs_bmap_read(ip, bn);
        if (addr == 0) {
            // Sparse file - return zeros
            memset(buf + bytes_read, 0, n);
        } else {
            struct buf *bp = bread(ip->dev, addr);
            if (bp == NULL) {
                if (bytes_read == 0) {
                    return -EIO;
                }
                break;
            }
            memmove(buf + bytes_read, bp->data + off, n);
            brelse(bp);
        }
        
        bytes_read += n;
        pos += n;
    }
    
    return bytes_read;
}

/******************************************************************************
 * File write
 ******************************************************************************/

ssize_t xv6fs_file_write(struct vfs_file *file, const char *buf, size_t count) {
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    struct xv6fs_inode *ip = container_of(inode, struct xv6fs_inode, vfs_inode);
    struct xv6fs_superblock *xv6_sb = container_of(inode->sb, struct xv6fs_superblock, vfs_sb);
    
    if (!S_ISREG(inode->mode)) {
        return -EINVAL;
    }
    
    loff_t pos = file->f_pos;
    loff_t end_pos = pos + count;
    
    // Check file size limit
    if (end_pos > XV6FS_MAXFILE * BSIZE) {
        return -EFBIG;
    }
    
    // Write in chunks to avoid exceeding log transaction size
    // Maximum blocks per transaction: MAXOPBLOCKS - overhead
    int max = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * BSIZE;
    size_t bytes_written = 0;
    
    while (bytes_written < count) {
        size_t n = count - bytes_written;
        if (n > max) {
            n = max;
        }
        
        xv6fs_begin_op(xv6_sb);
        
        size_t chunk_written = 0;
        while (chunk_written < n) {
            uint bn = pos / BSIZE;
            uint off = pos % BSIZE;
            uint chunk = BSIZE - off;
            if (chunk > n - chunk_written) {
                chunk = n - chunk_written;
            }
            
            uint addr = xv6fs_bmap(ip, bn);
            if (addr == 0) {
                xv6fs_end_op(xv6_sb);
                if (bytes_written == 0) {
                    return -ENOSPC;
                }
                goto done;
            }
            
            struct buf *bp = bread(ip->dev, addr);
            if (bp == NULL) {
                xv6fs_end_op(xv6_sb);
                if (bytes_written == 0) {
                    return -EIO;
                }
                goto done;
            }
            
            memmove(bp->data + off, buf + bytes_written + chunk_written, chunk);
            xv6fs_log_write(xv6_sb, bp);
            brelse(bp);
            
            chunk_written += chunk;
            pos += chunk;
        }
        
        // Update size if we extended the file
        if (pos > inode->size) {
            inode->size = pos;
        }
        xv6fs_iupdate(ip);
        
        xv6fs_end_op(xv6_sb);
        
        bytes_written += chunk_written;
    }
    
done:
    return bytes_written;
}

/******************************************************************************
 * File seek
 ******************************************************************************/

loff_t xv6fs_file_llseek(struct vfs_file *file, loff_t offset, int whence) {
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
        new_pos = inode->size + offset;
        break;
    default:
        return -EINVAL;
    }
    
    if (new_pos < 0) {
        return -EINVAL;
    }
    
    return new_pos;
}

/******************************************************************************
 * File stat
 ******************************************************************************/

int xv6fs_file_stat(struct vfs_file *file, struct stat *stat) {
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    struct xv6fs_inode *ip = container_of(inode, struct xv6fs_inode, vfs_inode);
    
    if (stat == NULL) {
        return -EINVAL;
    }
    
    memset(stat, 0, sizeof(*stat));
    stat->dev = ip->dev;
    stat->ino = inode->ino;
    stat->mode = inode->mode;
    stat->nlink = inode->n_links;
    stat->size = inode->size;
    
    return 0;
}

/******************************************************************************
 * VFS file operations structure
 ******************************************************************************/

struct vfs_file_ops xv6fs_file_ops = {
    .read = xv6fs_file_read,
    .write = xv6fs_file_write,
    .llseek = xv6fs_file_llseek,
    .release = NULL,
    .fsync = NULL,  // TODO: Implement fsync
    .stat = xv6fs_file_stat,
};
