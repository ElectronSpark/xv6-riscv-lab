/*
 * xv6fs file operations
 *
 * LOCKING DESIGN: DRIVER-MANAGED INODE LOCKS
 * ==========================================
 * VFS file operations (vfs_fileread, vfs_filewrite, etc.) do NOT acquire
 * the inode lock before calling into the driver. Instead, each driver
 * callback is responsible for acquiring the inode lock when needed.
 *
 * This design is necessary because:
 * 1. xv6fs_file_write needs to acquire a transaction (begin_op) BEFORE
 *    locking the inode, to match VFS lock ordering: transaction → superblock → inode
 * 2. If VFS held the inode lock when calling write, and write called begin_op,
 *    it would cause deadlock with other paths that do begin_op → ilock.
 *
 * LOCK ORDERING:
 * - xv6fs_file_write: begin_op → vfs_ilock → work → vfs_iunlock → end_op
 * - xv6fs_file_read: vfs_ilock → read → vfs_iunlock (no transaction needed)
 * - xv6fs_file_llseek: vfs_ilock → read size → vfs_iunlock (for SEEK_END)
 * - xv6fs_file_stat: vfs_ilock → read fields → vfs_iunlock
 *
 * The VFS file lock (per-file mutex) still serializes concurrent operations
 * on the same file descriptor and protects the file position.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "lock/spinlock.h"
#include "lock/mutex_types.h"
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

ssize_t xv6fs_file_read(struct vfs_file *file, char *buf, size_t count, bool user) {
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    struct xv6fs_inode *ip = container_of(inode, struct xv6fs_inode, vfs_inode);
    
    if (!S_ISREG(inode->mode)) {
        return -EINVAL;
    }
    
    // Acquire inode lock to safely read size and prevent truncation during read.
    // Read doesn't need a transaction (no modifications), so we can just lock the inode.
    // Note: The file reference guarantees the inode remains allocated - no validity
    // check needed per Linux VFS model (file holds inode reference).
    vfs_ilock(inode);
    
    loff_t pos = file->f_pos;
    if (pos >= inode->size) {
        vfs_iunlock(inode);
        return 0;  // EOF
    }
    if (pos + count > inode->size) {
        count = inode->size - pos;
    }
    
    // Capture size for later checks (size won't change while we hold the lock,
    // and we don't release it until we're done reading)
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
            if (user) {
                // Zero out user buffer
                char zeros[64];
                memset(zeros, 0, sizeof(zeros));
                uint remaining = n;
                while (remaining > 0) {
                    uint chunk = remaining < sizeof(zeros) ? remaining : sizeof(zeros);
                    if (vm_copyout(myproc()->vm, (uint64)(buf + bytes_read + (n - remaining)), zeros, chunk) < 0) {
                        vfs_iunlock(inode);
                        if (bytes_read == 0) return -EFAULT;
                        return bytes_read;
                    }
                    remaining -= chunk;
                }
            } else {
                memset(buf + bytes_read, 0, n);
            }
        } else {
            struct buf *bp = bread(ip->dev, addr);
            if (bp == NULL) {
                vfs_iunlock(inode);
                if (bytes_read == 0) {
                    return -EIO;
                }
                return bytes_read;
            }
            if (user) {
                if (vm_copyout(myproc()->vm, (uint64)(buf + bytes_read), bp->data + off, n) < 0) {
                    brelse(bp);
                    vfs_iunlock(inode);
                    if (bytes_read == 0) return -EFAULT;
                    return bytes_read;
                }
            } else {
                memmove(buf + bytes_read, bp->data + off, n);
            }
            brelse(bp);
        }
        
        bytes_read += n;
        pos += n;
    }
    
    vfs_iunlock(inode);
    return bytes_read;
}

/******************************************************************************
 * File write
 ******************************************************************************/

ssize_t xv6fs_file_write(struct vfs_file *file, const char *buf, size_t count, bool user) {
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
        
        // Acquire transaction BEFORE inode lock to match VFS locking order:
        // transaction → superblock → inode
        // VFS releases inode lock before calling this function to avoid deadlock.
        xv6fs_begin_op(xv6_sb);
        
        // Now acquire inode lock to protect inode metadata during write.
        // The file reference guarantees the inode remains allocated.
        vfs_ilock(inode);
        
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
                vfs_iunlock(inode);
                xv6fs_end_op(xv6_sb);
                if (bytes_written == 0) {
                    return -ENOSPC;
                }
                goto done;
            }
            
            struct buf *bp = bread(ip->dev, addr);
            if (bp == NULL) {
                vfs_iunlock(inode);
                xv6fs_end_op(xv6_sb);
                if (bytes_written == 0) {
                    return -EIO;
                }
                goto done;
            }
            
            if (user) {
                if (vm_copyin(myproc()->vm, bp->data + off, (uint64)(buf + bytes_written + chunk_written), chunk) < 0) {
                    brelse(bp);
                    vfs_iunlock(inode);
                    xv6fs_end_op(xv6_sb);
                    if (bytes_written == 0) return -EFAULT;
                    goto done;
                }
            } else {
                memmove(bp->data + off, buf + bytes_written + chunk_written, chunk);
            }
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
        
        // Release inode lock before ending transaction
        vfs_iunlock(inode);
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
        // Need to lock inode to safely read size
        vfs_ilock(inode);
        new_pos = inode->size + offset;
        vfs_iunlock(inode);
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
    
    // Lock inode to get consistent snapshot of inode fields.
    // The file reference guarantees the inode remains allocated.
    vfs_ilock(inode);
    
    memset(stat, 0, sizeof(*stat));
    stat->dev = ip->dev;
    stat->ino = inode->ino;
    stat->mode = inode->mode;
    stat->nlink = inode->n_links;
    stat->size = inode->size;
    
    vfs_iunlock(inode);
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
