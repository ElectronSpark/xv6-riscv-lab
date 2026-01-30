/**
 * tmpfs file operations
 * 
 * This file implements the VFS file operations for tmpfs regular files.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include <smp/atomic.h>
#include "param.h"
#include "errno.h"
#include "bits.h"
#include "vfs/stat.h"
#include "lock/spinlock.h"
#include "lock/mutex_types.h"
#include "lock/rwlock.h"
#include "lock/completion.h"
#include "proc/proc.h"
#include "vfs/fs.h"
#include "vfs/file.h"
#include "vfs/fcntl.h"
#include "list.h"
#include "hlist.h"
#include <mm/slab.h>
#include <mm/vm.h>
#include "tmpfs_private.h"

// Forward declarations
static ssize_t __tmpfs_file_read(struct vfs_file *file, char *buf, size_t count, bool user);
static ssize_t __tmpfs_file_write(struct vfs_file *file, const char *buf, size_t count, bool user);
static loff_t __tmpfs_file_llseek(struct vfs_file *file, loff_t offset, int whence);
static int __tmpfs_file_stat(struct vfs_file *file, struct stat *stat);

struct vfs_file_ops tmpfs_file_ops = {
    .read = __tmpfs_file_read,
    .write = __tmpfs_file_write,
    .llseek = __tmpfs_file_llseek,
    .release = NULL,
    .fsync = NULL,
    .stat = __tmpfs_file_stat,
};

static ssize_t __tmpfs_file_read(struct vfs_file *file, char *buf, size_t count, bool user) {
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    struct tmpfs_inode *ti = container_of(inode, struct tmpfs_inode, vfs_inode);
    
    if (!S_ISREG(inode->mode)) {
        return -EINVAL;
    }
    
    loff_t pos = file->f_pos;
    if (pos >= inode->size) {
        return 0; // EOF
    }
    if (pos + count > inode->size) {
        count = inode->size - pos;
    }
    
    // Handle embedded data
    if (ti->embedded) {
        if (pos + count > TMPFS_INODE_EMBEDDED_DATA_LEN) {
            // This shouldn't happen - embedded files are limited in size
            count = TMPFS_INODE_EMBEDDED_DATA_LEN - pos;
        }
        if (user) {
            if (vm_copyout(myproc()->vm, (uint64)buf, ti->file.data + pos, count) < 0) {
                return -EFAULT;
            }
        } else {
            memmove(buf, ti->file.data + pos, count);
        }
        return count;
    }
    
    size_t bytes_read = 0;
    while (bytes_read < count) {
        size_t block_idx = TMPFS_IBLOCK(pos);
        size_t block_off = TMPFS_IBLOCK_OFFSET(pos);
        size_t chunk = PAGE_SIZE - block_off;
        if (chunk > count - bytes_read) {
            chunk = count - bytes_read;
        }
        
        void *block = __tmpfs_lookup_block(ti, block_idx, false);
        if (block == NULL) {
            // Hole in file, return zeros
            if (user) {
                char zeros[64];
                memset(zeros, 0, sizeof(zeros));
                size_t remaining = chunk;
                while (remaining > 0) {
                    size_t c = remaining < sizeof(zeros) ? remaining : sizeof(zeros);
                    if (vm_copyout(myproc()->vm, (uint64)(buf + bytes_read + (chunk - remaining)), zeros, c) < 0) {
                        if (bytes_read == 0) return -EFAULT;
                        return bytes_read;
                    }
                    remaining -= c;
                }
            } else {
                memset(buf + bytes_read, 0, chunk);
            }
        } else {
            if (user) {
                if (vm_copyout(myproc()->vm, (uint64)(buf + bytes_read), (char *)block + block_off, chunk) < 0) {
                    if (bytes_read == 0) return -EFAULT;
                    return bytes_read;
                }
            } else {
                memmove(buf + bytes_read, (char *)block + block_off, chunk);
            }
        }
        
        bytes_read += chunk;
        pos += chunk;
    }
    
    return bytes_read;
}

static ssize_t __tmpfs_file_write(struct vfs_file *file, const char *buf, size_t count, bool user) {
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    struct tmpfs_inode *ti = container_of(inode, struct tmpfs_inode, vfs_inode);
    
    if (!S_ISREG(inode->mode)) {
        return -EINVAL;
    }
    
    loff_t pos = file->f_pos;
    loff_t end_pos = pos + count;
    
    // Check for file size limits
    if (end_pos > TMPFS_MAX_FILE_SIZE) {
        return -EFBIG;
    }
    
    // Handle embedded data
    if (ti->embedded) {
        if (end_pos <= TMPFS_INODE_EMBEDDED_DATA_LEN) {
            // Still fits in embedded storage
            if (user) {
                if (vm_copyin(myproc()->vm, ti->file.data + pos, (uint64)buf, count) < 0) {
                    return -EFAULT;
                }
            } else {
                memmove(ti->file.data + pos, buf, count);
            }
            return count;
        }
        // Need to migrate to block storage
        int ret = __tmpfs_migrate_to_allocated_blocks(ti);
        if (ret != 0) {
            return ret;
        }
    }
    
    // VFS core already called truncate to extend the file if needed.
    // All blocks should be allocated.
    
    size_t bytes_written = 0;
    while (bytes_written < count) {
        size_t block_idx = TMPFS_IBLOCK(pos);
        size_t block_off = TMPFS_IBLOCK_OFFSET(pos);
        size_t chunk = PAGE_SIZE - block_off;
        if (chunk > count - bytes_written) {
            chunk = count - bytes_written;
        }
        
        // Block should already be allocated by VFS core's truncate call
        void *block = __tmpfs_lookup_block(ti, block_idx, false);
        if (block == NULL) {
            // This shouldn't happen after truncate, but handle gracefully
            printf("tmpfs_file_write: block_idx=%lu is NULL! pos=%lld, n_blocks=%ld\n",
                   (unsigned long)block_idx, (long long)pos, (long)inode->n_blocks);
            if (bytes_written == 0) {
                return -EIO;
            }
            break;
        }
        
        if (user) {
            if (vm_copyin(myproc()->vm, (char *)block + block_off, (uint64)(buf + bytes_written), chunk) < 0) {
                if (bytes_written == 0) return -EFAULT;
                break;
            }
        } else {
            memmove((char *)block + block_off, buf + bytes_written, chunk);
        }
        
        bytes_written += chunk;
        pos += chunk;
    }
    
    return bytes_written;
}

static loff_t __tmpfs_file_llseek(struct vfs_file *file, loff_t offset, int whence) {
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

static int __tmpfs_file_stat(struct vfs_file *file, struct stat *stat) {
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    
    memset(stat, 0, sizeof(*stat));
    stat->dev = inode->sb ? (int)(uint64)inode->sb : 0;
    stat->ino = inode->ino;
    stat->mode = inode->mode;
    stat->nlink = inode->n_links;
    stat->size = inode->size;
    
    return 0;
}

// Open callback for tmpfs inodes
// Sets up file operations based on inode type
int tmpfs_open(struct vfs_inode *inode, struct vfs_file *file, int f_flags) {
    if (inode == NULL || file == NULL) {
        return -EINVAL;
    }
    
    if (S_ISREG(inode->mode)) {
        file->ops = &tmpfs_file_ops;
        return 0;
    }
    
    if (S_ISDIR(inode->mode)) {
        // Directories don't need special file ops - they use dir_iter
        file->ops = &tmpfs_file_ops;
        return 0;
    }
    
    if (S_ISLNK(inode->mode)) {
        /*
         * Allow opening symlinks with O_NOFOLLOW flag.
         * POSIX requires that symlinks can be opened with O_NOFOLLOW to allow
         * fstat() on the symlink itself (not its target). This is needed by
         * programs like ls and symlinktest that want to stat symlink info.
         */
        file->ops = &tmpfs_file_ops;
        return 0;
    }
    
    // Character/block devices and pipes are handled by VFS core
    // They should not reach here as vfs_fileopen handles them
    if (S_ISCHR(inode->mode) || S_ISBLK(inode->mode) || S_ISFIFO(inode->mode)) {
        return -EINVAL; // Should be handled by VFS
    }
    
    return -ENOSYS;
}
