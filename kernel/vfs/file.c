#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "atomic.h"
#include "param.h"
#include "errno.h"
#include "bits.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "mutex_types.h"
#include "rwlock.h"
#include "completion.h"
#include "vfs/fs.h"
#include "vfs/file.h"
#include "vfs_private.h"
#include "vfs/fcntl.h"
#include "list.h"
#include "hlist.h"
#include "slab.h"
#include "cdev.h"
#include "blkdev.h"
#include "vm.h"

static slab_cache_t __vfs_file_slab = { 0 };
static struct spinlock __vfs_ftable_lock = { 0 };
static list_node_t __vfs_ftable = { 0 };
static int __vfs_open_file_count = 0;

static void __vfs_file_lock(struct vfs_file *file) {
    assert(mutex_lock(&file->lock) == 0, "__vfs_file_lock: failed to lock file mutex");
}

static void __vfs_file_unlock(struct vfs_file *file) {
    mutex_unlock(&file->lock);
}

static void __vfs_ftable_attatch(struct vfs_file *file) {
    spin_acquire(&__vfs_ftable_lock);
    list_node_push(&__vfs_ftable, file, list_entry);
    int count = __atomic_add_fetch(&__vfs_open_file_count, 1, __ATOMIC_SEQ_CST);
    spin_release(&__vfs_ftable_lock);
    assert(count > 0, "vfs file open count overflow");
}

static void __vfs_ftable_detatch(struct vfs_file *file) {
    spin_acquire(&__vfs_ftable_lock);
    list_node_detach(file, list_entry);
    int count = __atomic_sub_fetch(&__vfs_open_file_count, 1, __ATOMIC_SEQ_CST);
    spin_release(&__vfs_ftable_lock);
    assert(count >= 0, "vfs file open count underflow");
}

static struct vfs_file *__vfs_file_alloc(void) {
    struct vfs_file *file = slab_alloc(&__vfs_file_slab);
    if (file == NULL) {
        return NULL;
    }
    memset(file, 0, sizeof(*file));
    mutex_init(&file->lock, "vfs_file_lock");
    file->ref_count = 1;
    return file;
}

static void __vfs_file_free(struct vfs_file *file) {
    if (file == NULL) {
        return;
    }
    slab_free(file);
}

void __vfs_file_init(void) {
    int ret = slab_cache_init(&__vfs_file_slab, "vfs_file_cache",
                              sizeof(struct vfs_file), 0);
    assert(ret == 0, "Failed to initialize vfs_file_cache slab cache, errno=%d", ret);
    spin_init(&__vfs_ftable_lock, "vfs_file_table_lock");
    list_entry_init(&__vfs_ftable);
    __atomic_store_n(&__vfs_open_file_count, 0, __ATOMIC_SEQ_CST);
}

void __vfs_file_shrink_cache(void) {
    slab_cache_shrink(&__vfs_file_slab, 0x7fffffff);
}

// Open a character device file
static int __vfs_open_cdev(struct vfs_inode *inode, struct vfs_file *file) {
    cdev_t *cdev = cdev_get(major(inode->cdev), minor(inode->cdev));
    if (IS_ERR(cdev)) {
        return PTR_ERR(cdev);
    }
    if (cdev == NULL) {
        return -ENODEV;
    }
    file->cdev = *cdev;
    file->ops = NULL; // Device files use direct device I/O
    return 0;
}

// Open a block device file
static int __vfs_open_blkdev(struct vfs_inode *inode, struct vfs_file *file) {
    blkdev_t *blkdev = blkdev_get(major(inode->bdev), minor(inode->bdev));
    if (IS_ERR(blkdev)) {
        return PTR_ERR(blkdev);
    }
    if (blkdev == NULL) {
        return -ENODEV;
    }
    file->blkdev = *blkdev;
    file->ops = NULL; // Device files use direct device I/O
    return 0;
}

struct vfs_file *vfs_fileopen(struct vfs_inode *inode, int f_flags) {
    if (inode == NULL || inode->sb == NULL) {
        return ERR_PTR(-EINVAL); // Invalid arguments
    }
    
    // Sockets cannot be opened via inode
    if (S_ISSOCK(inode->mode)) {
        return ERR_PTR(-ENXIO); // No such device or address
    }
    // Pipes are created via pipe(), not open()
    if (S_ISFIFO(inode->mode)) {
        return ERR_PTR(-ENXIO); // Named pipes not supported via open yet
    }

    vfs_ilock(inode);
    // @TODO: check permission
    struct vfs_file *file = __vfs_file_alloc();
    int ret = 0;
    if (file == NULL) {
        vfs_iunlock(inode);
        return ERR_PTR(-ENOMEM); // Out of memory
    }
    ret = vfs_inode_get_ref(inode, &file->inode);
    if (ret != 0) {
        __vfs_file_free(file);
        vfs_iunlock(inode);
        return ERR_PTR(ret);
    }

    // Handle special file types
    if (S_ISCHR(inode->mode)) {
        ret = __vfs_open_cdev(inode, file);
        if (ret != 0) {
            vfs_iunlock(inode);
            vfs_inode_put_ref(&file->inode);
            __vfs_file_free(file);
            return ERR_PTR(ret);
        }
        vfs_iunlock(inode);
        __vfs_ftable_attatch(file);
        file->f_flags = f_flags;
        return file;
    }
    
    if (S_ISBLK(inode->mode)) {
        ret = __vfs_open_blkdev(inode, file);
        if (ret != 0) {
            vfs_iunlock(inode);
            vfs_inode_put_ref(&file->inode);
            __vfs_file_free(file);
            return ERR_PTR(ret);
        }
        vfs_iunlock(inode);
        __vfs_ftable_attatch(file);
        file->f_flags = f_flags;
        return file;
    }

    // Regular files and directories use inode->ops->open
    if (inode->ops == NULL || inode->ops->open == NULL) {
        vfs_iunlock(inode);
        vfs_inode_put_ref(&file->inode);
        __vfs_file_free(file);
        return ERR_PTR(-ENOSYS); // Open operation not supported
    }
    ret = inode->ops->open(inode, file, f_flags);
    if (ret != 0) {
        vfs_iunlock(inode);
        vfs_inode_put_ref(&file->inode);
        __vfs_file_free(file);
        return ERR_PTR(ret);
    }
    if (file->ops == NULL) {
        vfs_iunlock(inode);
        vfs_inode_put_ref(&file->inode);
        __vfs_file_free(file);
        printf("vfs_fileopen: file operations not set by inode open\n");
        return ERR_PTR(-EINVAL); // Invalid file operations
    }

    vfs_iunlock(inode);
    __vfs_ftable_attatch(file);
    file->f_flags = f_flags;
    file->f_pos = 0;
    return file;
}

void vfs_fileclose(struct vfs_file *file) {
    if (file == NULL) {
        printf("vfs_fileclose: file is NULL\n");
        return;
    }
    if (!atomic_dec_unless(&file->ref_count, 1)) {
        // File descriptors are shared through dup, thus when refcount reach 1,
        // no other process will be using it. No need to lock the file structure.
        __vfs_ftable_detatch(file);
        
        struct vfs_inode *inode = vfs_inode_deref(&file->inode);
        int ret = 0;
        
        // Handle special file cleanup
        if (inode != NULL) {
            if (S_ISCHR(inode->mode)) {
                ret = cdev_put(&file->cdev);
                if (ret != 0) {
                    printf("vfs_fileclose: cdev_put failed: %d\n", ret);
                }
            } else if (S_ISBLK(inode->mode)) {
                ret = blkdev_put(&file->blkdev);
                if (ret != 0) {
                    printf("vfs_fileclose: blkdev_put failed: %d\n", ret);
                }
            } else if (S_ISFIFO(inode->mode) && file->pipe != NULL) {
                pipeclose(file->pipe, (file->f_flags & O_ACCMODE) != O_RDONLY);
            }
            // Note: sockets are not opened via inodes, so no cleanup here
        }
        
        vfs_inode_put_ref(&file->inode);
        __vfs_file_free(file);
    }
}

struct vfs_file *vfs_filedup(struct vfs_file *file) {
    if (file == NULL) {
        return NULL;
    }
    
    // First, increment the file refcount - if this fails, we shouldn't dup device refs
    bool success = atomic_inc_unless(&file->ref_count, 0);
    if (!success) {
        // File was already closed
        return NULL;
    }
    
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    
    // For device files, we need to dup the device reference
    if (inode != NULL) {
        int ret = 0;
        if (S_ISCHR(inode->mode)) {
            ret = cdev_dup(&file->cdev);
        } else if (S_ISBLK(inode->mode)) {
            ret = blkdev_dup(&file->blkdev);
        }
        
        if (ret != 0) {
            // Device dup failed, undo the file refcount increment
            // Use atomic_dec_unless to prevent underflow (should never happen
            // since we just incremented, but be safe)
            atomic_dec_unless(&file->ref_count, 0);
            return NULL;
        }
    }
    
    return file;
}

ssize_t vfs_fileread(struct vfs_file *file, void *buf, size_t n) {
    if (file == NULL || buf == NULL || n == 0) {
        return -EINVAL; // Invalid arguments
    }
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (inode == NULL) {
        return -EINVAL; // Invalid arguments
    }
    __vfs_file_lock(file);
    if ((file->f_flags & O_ACCMODE) == O_WRONLY) {
        __vfs_file_unlock(file);
        return -EBADF; // File not opened for reading
    }
    
    ssize_t ret = 0;
    
    // Handle character device read
    if (S_ISCHR(inode->mode)) {
        ret = cdev_read(&file->cdev, true, buf, n);
        __vfs_file_unlock(file);
        return ret;
    }
    
    // Handle block device read - not directly supported, use buffer cache
    if (S_ISBLK(inode->mode)) {
        __vfs_file_unlock(file);
        return -ENOSYS; // Direct block device read not implemented
    }
    
    // Handle pipe read
    if (S_ISFIFO(inode->mode)) {
        if (file->pipe == NULL) {
            __vfs_file_unlock(file);
            return -EINVAL;
        }
        ret = piperead(file->pipe, (uint64)buf, n);
        __vfs_file_unlock(file);
        return ret;
    }
    
    // Regular files
    vfs_ilock(inode);
    ret = __vfs_inode_valid(inode);
    if (ret != 0) {
        goto out;
    }
    if (!S_ISREG(inode->mode)) {
        ret = -EINVAL; // Inode is not a regular file
        goto out;
    }
    if (file->ops == NULL || file->ops->read == NULL) {
        ret = -ENOSYS; // Read operation not supported
        goto out;
    }
    if (file->f_pos >= inode->size) {
        ret = 0; // EOF
        goto out;
    }
    if (inode->size - file->f_pos < n) {
        n = inode->size - file->f_pos; // Adjust n to read up to EOF
    }
    ret = file->ops->read(file, buf, n);
    if (ret > 0) {
        file->f_pos += ret;
    }
out:
    vfs_iunlock(inode);
    __vfs_file_unlock(file);
    return ret;
}

int vfs_filestat(struct vfs_file *file, struct stat *stat) {
    if (file == NULL || stat == NULL) {
        return -EINVAL; // Invalid arguments
    }
    
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (inode == NULL) {
        return -EINVAL;
    }
    
    // For special files without file ops, provide generic stat from inode
    if (file->ops == NULL || file->ops->stat == NULL) {
        if (S_ISCHR(inode->mode) || S_ISBLK(inode->mode) || 
            S_ISFIFO(inode->mode) || S_ISSOCK(inode->mode)) {
            memset(stat, 0, sizeof(*stat));
            stat->dev = inode->sb ? (int)(uint64)inode->sb : 0;
            stat->ino = inode->ino;
            stat->type = T_DEVICE;
            stat->nlink = inode->n_links;
            stat->size = inode->size;
            return 0;
        }
        return -ENOSYS; // Stat operation not supported
    }
    return file->ops->stat(file, stat);
}

ssize_t vfs_filewrite(struct vfs_file *file, const void *buf, size_t n) {
    if (file == NULL || buf == NULL || n == 0) {
        return -EINVAL; // Invalid arguments
    }
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (inode == NULL) {
        return -EINVAL; // Invalid arguments
    }
    __vfs_file_lock(file);
    if ((file->f_flags & O_ACCMODE) == O_RDONLY) {
        __vfs_file_unlock(file);
        return -EBADF; // File not opened for writing
    }
    
    ssize_t ret = 0;
    
    // Handle character device write
    if (S_ISCHR(inode->mode)) {
        ret = cdev_write(&file->cdev, true, buf, n);
        __vfs_file_unlock(file);
        return ret;
    }
    
    // Handle block device write - not directly supported, use buffer cache
    if (S_ISBLK(inode->mode)) {
        __vfs_file_unlock(file);
        return -ENOSYS; // Direct block device write not implemented
    }
    
    // Handle pipe write
    if (S_ISFIFO(inode->mode)) {
        if (file->pipe == NULL) {
            __vfs_file_unlock(file);
            return -EINVAL;
        }
        ret = pipewrite(file->pipe, (uint64)buf, n);
        __vfs_file_unlock(file);
        return ret;
    }
    
    // Regular files
    vfs_ilock(inode);
    ret = __vfs_inode_valid(inode);
    if (ret != 0) {
        goto out;
    }
    if (!S_ISREG(inode->mode)) {
        ret = -EINVAL; // Inode is not a regular file
        goto out;
    }
    if (file->ops == NULL || file->ops->write == NULL) {
        ret = -ENOSYS; // Write operation not supported
        goto out;
    }
    loff_t new_pos = file->f_pos + n;
    if (new_pos < file->f_pos) {
        // Overflow
        ret = -EFBIG; // File too large
        goto out;
    }
    if (new_pos > inode->size) {
        // Need to extend the file - use truncate to allocate blocks
        if (inode->ops == NULL || inode->ops->truncate == NULL) {
            ret = -EFBIG; // Truncate operation not supported
            goto out;
        }
        ret = inode->ops->truncate(inode, new_pos);
        if (ret != 0) {
            goto out;
        }
    }
    ret = file->ops->write(file, buf, n);
    if (ret > 0) {
        file->f_pos += ret;
    }
out:
    vfs_iunlock(inode);
    __vfs_file_unlock(file);
    return ret;
}

loff_t vfs_filelseek(struct vfs_file *file, loff_t offset, int whence) {
    if (file == NULL) {
        return -EINVAL; // Invalid arguments
    }
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    loff_t ret = 0;
    if (inode == NULL) {
        return -EINVAL; // Invalid arguments
    }
    
    // lseek only applies to regular files
    if (!S_ISREG(inode->mode)) {
        return -ESPIPE; // Illegal seek
    }
    __vfs_file_lock(file);
    vfs_ilock(inode);
    ret = __vfs_inode_valid(inode);
    if (ret != 0) {
        vfs_iunlock(inode);
        goto out;
    }
    vfs_iunlock(inode);
    if (file->ops == NULL || file->ops->llseek == NULL) {
        ret = -ENOSYS; // Llseek operation not supported
        goto out;
    }
    
    ret = file->ops->llseek(file, offset, whence);
    if (ret >= 0) {
        file->f_pos = ret;
    }
out:
    __vfs_file_unlock(file);
    return ret;
}

int truncate(struct vfs_file *file, loff_t length) {
    if (file == NULL) {
        return -EINVAL; // Invalid arguments
    }
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (inode == NULL) {
        return -EINVAL; // Invalid arguments
    }
    
    // truncate only applies to regular files
    if (!S_ISREG(inode->mode)) {
        return -EINVAL; // Not a regular file
    }
    
    __vfs_file_lock(file);
    // ilock will be acquired in vfs_itruncate
    int ret = vfs_itruncate(inode, length);
    __vfs_file_unlock(file);
    return ret;
}
