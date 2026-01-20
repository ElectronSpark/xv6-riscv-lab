/*
 * VFS file operations
 *
 * Locking order (must acquire in this order to avoid deadlock):
 * 1. vfs_superblock rwlock (via vfs_superblock_rlock/wlock) - for metadata ops
 * 2. vfs_inode mutex (via vfs_ilock) - for inode access
 * 3. vfs_file mutex (via __vfs_file_lock) - for file descriptor state
 * 4. buffer mutex (via bread/brelse) - for block cache
 * 5. log spinlock (xv6fs internal) - for transaction management
 *
 * IMPORTANT: File read/write operations acquire inode lock WITHOUT superblock
 * lock, since they don't modify filesystem metadata. This is safe as long as
 * operations that DO hold superblock lock don't block waiting for file I/O.
 *
 * BUG FIXES:
 * - Anonymous pipe leak (Dec 2024): Pipes created via pipe() syscall have
 *   pipe != NULL but inode == NULL. vfs_fput() must call pipeclose()
 *   for these pipes BEFORE the inode NULL check, otherwise pipe buffers leak.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "atomic.h"
#include "param.h"
#include "errno.h"
#include "bits.h"
#include "vfs/stat.h"
#include "spinlock.h"
#include "proc/proc.h"
#include "mutex_types.h"
#include "rwlock.h"
#include "completion.h"
#include "vfs/fs.h"
#include "printf.h"
#include "vfs/file.h"
#include "vfs_private.h"
#include "vfs/fcntl.h"
#include "list.h"
#include "hlist.h"
#include "slab.h"
#include "cdev.h"
#include "blkdev.h"
#include "vm.h"
#include "net.h"
#include "pipe.h"
#include "proc/proc_queue.h"

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
    if (file->ops != NULL && file->ops->release != NULL) {
        // Call file release operation
        int ret = file->ops->release(file->inode.inode, file);
        if (ret != 0) {
            printf("__vfs_file_free: file ops release failed, errno=%d\n", ret);
        }
    }
    slab_free(file);
}

void __vfs_file_init(void) {
    int ret = slab_cache_init(&__vfs_file_slab, 
                              "vfs_file_cache",
                              sizeof(struct vfs_file),  
                              SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
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
    file->cdev = cdev;
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
    file->blkdev = blkdev;
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

/**
 * @brief Release a file reference
 *
 * Decrements the file's reference count. When the count reaches 1 (last reference),
 * performs cleanup including:
 *   - Detaching from global file table
 *   - Closing pipes (both anonymous and named)
 *   - Releasing character/block device references
 *   - Releasing inode reference
 *   - Freeing the file structure
 *
 * @param file File to release (may be NULL)
 * @note Thread-safe via atomic reference counting
 */
void vfs_fput(struct vfs_file *file) {
    if (file == NULL) {
        printf("vfs_fput: file is NULL\n");
        return;
    }
    if (!atomic_dec_unless(&file->ref_count, 1)) {
        // File descriptors are shared through dup, thus when refcount reach 1,
        // no other process will be using it. No need to lock the file structure.
        __vfs_ftable_detatch(file);
        
        struct vfs_inode *inode = vfs_inode_deref(&file->inode);
        int ret = 0;
        
        // Handle pipe cleanup for pipes without inodes (created via pipe() syscall)
        // Must be done before the inode check since anonymous pipes have no inode
        if (file->pipe != NULL && inode == NULL) {
            pipeclose(file->pipe, (file->f_flags & O_ACCMODE) != O_RDONLY);
        }
        
        // Handle special file cleanup
        if (inode != NULL) {
            if (S_ISCHR(inode->mode)) {
                ret = cdev_put(file->cdev);
                file->cdev = NULL;
                if (ret != 0) {
                    printf("vfs_fput: cdev_put failed: %d\n", ret);
                }
            } else if (S_ISBLK(inode->mode)) {
                ret = blkdev_put(file->blkdev);
                file->blkdev = NULL;
                if (ret != 0) {
                    printf("vfs_fput: blkdev_put failed: %d\n", ret);
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

/**
 * @brief Duplicate a file reference
 *
 * Increments the file's reference count, allowing the same file structure
 * to be shared across multiple file descriptors (e.g., via dup() syscall).
 *
 * @param file File to duplicate (may be NULL)
 * @return Same file pointer with incremented refcount, or NULL if file was NULL/closed
 * @note Thread-safe via atomic reference counting
 */
struct vfs_file *vfs_fdup(struct vfs_file *file) {
    if (file == NULL) {
        return NULL;
    }
    
    // Only increase the ref count of the file descriptor
    bool success = atomic_inc_unless(&file->ref_count, 0);
    if (!success) {
        // File was already closed
        return NULL;
    }
    
    return file;
}

ssize_t vfs_fileread(struct vfs_file *file, void *buf, size_t n) {
    if (file == NULL || buf == NULL || n == 0) {
        return -EINVAL; // Invalid arguments
    }
    
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    
    // Handle pipe read - pipes don't have inodes
    if (inode == NULL) {
        // No inode means this must be a pipe or socket
        if (file->pipe == NULL) {
            return -EINVAL; // Not a valid file
        }
        __vfs_file_lock(file);
        if ((file->f_flags & O_ACCMODE) == O_WRONLY) {
            __vfs_file_unlock(file);
            return -EBADF; // File not opened for reading
        }
        ssize_t ret = piperead_kernel(file->pipe, buf, n);
        __vfs_file_unlock(file);
        return ret;
    }
    
    __vfs_file_lock(file);
    if ((file->f_flags & O_ACCMODE) == O_WRONLY) {
        __vfs_file_unlock(file);
        return -EBADF; // File not opened for reading
    }
    
    ssize_t ret = 0;
    
    // Handle character device read
    if (S_ISCHR(inode->mode)) {
        ret = cdev_read(file->cdev, false, buf, n);  // false = kernel buffer
        __vfs_file_unlock(file);
        return ret;
    }
    
    // Handle block device read - not directly supported, use buffer cache
    if (S_ISBLK(inode->mode)) {
        __vfs_file_unlock(file);
        return -ENOSYS; // Direct block device read not implemented
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
            stat->mode = inode->mode;
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
    
    // Handle pipe write - pipes don't have inodes
    if (inode == NULL) {
        // No inode means this must be a pipe or socket
        if (file->pipe == NULL) {
            return -EINVAL; // Not a valid file
        }
        __vfs_file_lock(file);
        if ((file->f_flags & O_ACCMODE) == O_RDONLY) {
            __vfs_file_unlock(file);
            return -EBADF; // File not opened for writing
        }
        ssize_t ret = pipewrite_kernel(file->pipe, buf, n);
        __vfs_file_unlock(file);
        return ret;
    }
    
    __vfs_file_lock(file);
    if ((file->f_flags & O_ACCMODE) == O_RDONLY) {
        __vfs_file_unlock(file);
        return -EBADF; // File not opened for writing
    }
    
    ssize_t ret = 0;
    
    // Handle character device write
    if (S_ISCHR(inode->mode)) {
        ret = cdev_write(file->cdev, false, buf, n);  // false = kernel buffer
        __vfs_file_unlock(file);
        return ret;
    }
    
    // Handle block device write - not directly supported, use buffer cache
    if (S_ISBLK(inode->mode)) {
        __vfs_file_unlock(file);
        return -ENOSYS; // Direct block device write not implemented
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

/******************************************************************************
 * VFS Pipe Allocation
 ******************************************************************************/

int vfs_pipealloc(struct vfs_file **rf, struct vfs_file **wf) {
    struct pipe *pi = NULL;
    *rf = NULL;
    *wf = NULL;
    
    // Allocate read file
    *rf = __vfs_file_alloc();
    if (*rf == NULL) {
        return -ENOMEM;
    }
    
    // Allocate write file
    *wf = __vfs_file_alloc();
    if (*wf == NULL) {
        __vfs_file_free(*rf);
        *rf = NULL;
        return -ENOMEM;
    }
    
    // Allocate pipe structure
    pi = (struct pipe *)kalloc();
    if (pi == NULL) {
        __vfs_file_free(*rf);
        __vfs_file_free(*wf);
        *rf = NULL;
        *wf = NULL;
        return -ENOMEM;
    }
    
    // Initialize pipe
    smp_store_release(&pi->flags, PIPE_FLAGS_RW);
    pi->nwrite = 0;
    pi->nread = 0;
    spin_init(&pi->reader_lock, "vfs_pipe_reader");
    spin_init(&pi->writer_lock, "vfs_pipe_writer");
    proc_queue_init(&pi->nread_queue, "pipe_nread_queue", NULL);
    proc_queue_init(&pi->nwrite_queue, "pipe_nwrite_queue", NULL);
    
    // Initialize read file
    (*rf)->f_flags = O_RDONLY;
    (*rf)->pipe = pi;
    (*rf)->ops = NULL; // Pipes use direct pipe I/O
    __vfs_ftable_attatch(*rf);
    
    // Initialize write file
    (*wf)->f_flags = O_WRONLY;
    (*wf)->pipe = pi;
    (*wf)->ops = NULL;
    __vfs_ftable_attatch(*wf);
    
    return 0;
}

/******************************************************************************
 * VFS Socket Allocation
 ******************************************************************************/

// Socket structure from sysnet.c
struct sock {
  struct sock *next; // the next socket in the list
  uint32 raddr;      // the remote IPv4 address
  uint16 lport;      // the local UDP port number
  uint16 rport;      // the remote UDP port number
  struct spinlock lock; // protects the rxq
  struct mbufq rxq;  // a queue of packets waiting to be received
};

extern struct spinlock sock_lock;
extern struct sock *sockets;

int vfs_sockalloc(struct vfs_file **f, uint32 raddr, uint16 lport, uint16 rport) {
    struct sock *si = NULL;
    struct sock *pos;
    *f = NULL;
    
    // Allocate file
    *f = __vfs_file_alloc();
    if (*f == NULL) {
        return -ENOMEM;
    }
    
    // Allocate socket
    si = (struct sock *)kalloc();
    if (si == NULL) {
        __vfs_file_free(*f);
        *f = NULL;
        return -ENOMEM;
    }
    
    // Initialize socket
    si->raddr = raddr;
    si->lport = lport;
    si->rport = rport;
    spin_init(&si->lock, "sock");
    mbufq_init(&si->rxq);
    
    // Initialize file
    (*f)->f_flags = O_RDWR;
    (*f)->sock = si;
    (*f)->ops = NULL; // Sockets use direct socket I/O
    __vfs_ftable_attatch(*f);
    
    // Add to list of sockets (check for duplicates)
    spin_acquire(&sock_lock);
    pos = sockets;
    while (pos) {
        if (pos->raddr == raddr &&
            pos->lport == lport &&
            pos->rport == rport) {
            spin_release(&sock_lock);
            kfree((char *)si);
            __vfs_file_free(*f);
            *f = NULL;
            return -EADDRINUSE;
        }
        pos = pos->next;
    }
    si->next = sockets;
    sockets = si;
    spin_release(&sock_lock);
    
    return 0;
}
