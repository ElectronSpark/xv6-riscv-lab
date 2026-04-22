/*
 * VFS file operations
 *
 * Locking order (must acquire in this order to avoid deadlock):
 * 1. vfs_superblock rwsem (via vfs_superblock_rlock/wlock) - for metadata ops
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
 *   pipe != NULL but inode == NULL. vfs_fput() must call pipe_close()
 *   for these pipes BEFORE the inode NULL check, otherwise pipe buffers leak.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include <smp/atomic.h>
#include "kqueue_types.h"
#include "param.h"
#include "errno.h"
#include "bits.h"
#include "vfs/stat.h"
#include "lock/spinlock.h"
#include "lock/mutex_types.h"
#include "lock/rwsem.h"
#include "proc/thread.h"
#include "vfs/fs.h"
#include "printf.h"
#include "vfs/file.h"
#include "vfs_private.h"
#include "vfs/fcntl.h"
#include "list.h"
#include "hlist.h"
#include <mm/slab.h>
#include "dev/dev.h"
#include "dev/cdev.h"
#include "dev/blkdev.h"
#include <mm/vm.h>
#include "dev/net.h"
#include "vfs/pipe.h"
#include "proc/tq.h"

static slab_cache_t __vfs_file_slab = {0};
static spinlock_t __vfs_ftable_lock = {0};
static list_node_t __vfs_ftable = {0};
static int __vfs_open_file_count = 0;

static void __vfs_file_lock(struct vfs_file *file) {
    mutex_lock(&file->lock);
}

static void __vfs_file_unlock(struct vfs_file *file) {
    mutex_unlock(&file->lock);
}

static void __vfs_ftable_attatch(struct vfs_file *file) {
    spin_lock(&__vfs_ftable_lock);
    list_node_push(&__vfs_ftable, file, list_entry);
    int count = __atomic_add_fetch(&__vfs_open_file_count, 1, __ATOMIC_SEQ_CST);
    spin_unlock(&__vfs_ftable_lock);
    assert(count > 0, "vfs file open count overflow");
}

static void __vfs_ftable_detatch(struct vfs_file *file) {
    spin_lock(&__vfs_ftable_lock);
    list_node_detach(file, list_entry);
    int count = __atomic_sub_fetch(&__vfs_open_file_count, 1, __ATOMIC_SEQ_CST);
    spin_unlock(&__vfs_ftable_lock);
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
    spin_init(&file->knote_lock, "vfs_file_knote");
    list_entry_init(&file->knote_list);
    /* list_entry must be self-referencing (detached) after init */
    list_entry_init(&file->list_entry);
    return file;
}

/*
 * RCU callback: actually return the vfs_file slab slot to the allocator.
 * Runs after a full grace period, guaranteeing all concurrent RCU readers
 * (vfs_fdtable_get_file, sock_netconn_callback, etc.) have finished
 * accessing the file memory and have either taken a real reference via
 * vfs_fdup() or observed ref_count == 0 and bailed out.
 */
static void __vfs_file_slab_free_rcu(void *data) {
    slab_free(data);
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
    /*
     * Defer the actual slab deallocation until after a grace period.
     * This protects concurrent RCU readers that may still hold a raw
     * pointer to this vfs_file (obtained before the refcount hit 0).
     * They can safely call vfs_fdup() — which reads ref_count == 0 and
     * returns NULL — without risking a use-after-free on freed slab memory.
     */
    call_rcu(NULL, __vfs_file_slab_free_rcu, file);
}

void __vfs_file_init(void) {
    int ret = slab_cache_init(&__vfs_file_slab, "vfs_file_cache",
                              sizeof(struct vfs_file),
                              SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
    assert(ret == 0, "Failed to initialize vfs_file_cache slab cache, errno=%d",
           ret);
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

    /*
     * If the cdev provides open_file, let it customise the file
     * (e.g. install file->ops / private_data for /dev/ptmx).
     * The cdev is responsible for managing its own refcount in
     * that case — the VFS will NOT store file->cdev.
     */
    if (cdev->ops.open_file != NULL) {
        int ret = cdev->ops.open_file(cdev, file);
        if (ret != 0) {
            cdev_put(cdev);
            return ret;
        }
        /* open_file must have set file->ops; if it didn't, fall
         * through to the normal cdev path. */
        if (file->ops != NULL) {
            cdev_put(cdev); /* file no longer holds a cdev ref */
            return 0;
        }
    }

    file->cdev = cdev;
    file->ops = NULL; // Device files use direct device I/O

    /* Invoke the cdev's open callback (e.g. /dev/tty returns -ENXIO
     * when the process has no controlling terminal). */
    if (cdev->ops.open != NULL) {
        int ret = cdev->ops.open(cdev);
        if (ret != 0) {
            file->cdev = NULL;
            cdev_put(cdev);
            return ret;
        }
    }

    return 0;
}

// Open a block device file
static int __vfs_open_blkdev(struct vfs_inode *inode, struct vfs_file *file) {
    blkdev_t *blkdev = blkdev_get(major(inode->bdev), minor(inode->bdev));
    if (IS_ERR(blkdev)) {
        // Device not found - allow open for stat but not I/O
        file->blkdev = NULL;
        file->ops = NULL;
        return 0;
    }
    if (blkdev == NULL) {
        file->blkdev = NULL;
        file->ops = NULL;
        return 0;
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
 * Decrements the file's reference count. When the count reaches 1 (last
 * reference), performs cleanup including:
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
        return;
    }
    /*
     * CAS loop implementing "atomic_dec_unless(0)" with old-value capture.
     *
     * - Refuses to decrement when ref_count == 0  →  double-free guard
     *   (unlike raw fetch_sub which would corrupt the counter to -1).
     * - Captures the pre-decrement value so we know exactly who performed
     *   the 1 → 0 transition — only that thread runs the cleanup path.
     * - Once ref_count reaches 0 it is terminal: vfs_fdup() uses
     *   atomic_inc_unless(&ref_count, 0) and will refuse to resurrect it.
     * - Combined with RCU-deferred fd close (call_rcu → vfs_fput) and
     *   RCU-deferred slab_free in __vfs_file_free, concurrent RCU readers
     *   (e.g. vfs_fdtable_get_file, sock_netconn_callback) can safely
     *   call vfs_fdup() on a stale pointer — they will see ref_count == 0
     *   and return NULL without touching freed memory.
     */
    int old = __atomic_load_n(&file->ref_count, __ATOMIC_RELAXED);
    for (;;) {
        if (old <= 0) {
            printf("vfs_fput: ref_count=%d on file %p (double free?)\n",
                   old, file);
            return;
        }
        if (__atomic_compare_exchange_n(&file->ref_count, &old, old - 1,
                                        /*weak=*/1,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            break;
        /* CAS failed — 'old' has been reloaded by the CAS, retry */
    }
    if (old > 1)
        return;          /* other references remain, nothing to do */
    /* old == 1  →  we just set ref_count to 0  →  last user, clean up */
    __vfs_ftable_detatch(file);

    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    int ret = 0;

    // Flush dirty pages before releasing the inode reference.
    // This ensures all data is written to disk before the inode can be
    // torn down (avoids flush worker racing with inode destruction).
    if (file->ops != NULL && file->ops->fflush != NULL) {
        ret = file->ops->fflush(file);
        if (ret != 0 && ret != -EAGAIN) {
            printf("vfs_fput: fflush failed: %d\n", ret);
        }
    }

    // Handle special file cleanup
    // Note: anonymous pipe cleanup (pipe != NULL, inode == NULL) is now
    // handled by pipe_file_ops.release via __vfs_file_free below.
    if (inode != NULL) {
        if (S_ISCHR(inode->mode) && file->cdev != NULL) {
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
            pipe_close(file->pipe, (file->f_flags & O_ACCMODE) != O_RDONLY);
        }
        // Note: sockets are not opened via inodes, so no cleanup here
    }

    vfs_inode_put_ref(&file->inode);
    __vfs_file_free(file);
}

/**
 * @brief Duplicate a file reference
 *
 * Increments the file's reference count, allowing the same file structure
 * to be shared across multiple file descriptors (e.g., via dup() syscall).
 *
 * @param file File to duplicate (may be NULL)
 * @return Same file pointer with incremented refcount, or NULL if file was
 * NULL/closed
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

int vfs_ioctl(struct vfs_file *file, uint64 cmd, void *arg) {
    if (file == NULL) {
        return -EBADF;
    }

    /* Fast path: character / block device files — dispatch to device layer */
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (inode != NULL && (S_ISBLK(inode->mode) || S_ISCHR(inode->mode))) {
        /* Prefer custom file ops (installed by cdev open_file, e.g. PTY master) */
        if (file->ops != NULL && file->ops->ioctl != NULL)
            return file->ops->ioctl(file, cmd, arg);
        device_t *dev = (device_t *)file->cdev;
        if (dev != NULL)
            return dev_ioctl(dev, cmd, arg);
        return -ENODEV;
    }

    /* Fallback: custom file descriptors (pipes, sockets, etc.) */
    if (file->ops != NULL && file->ops->ioctl != NULL)
        return file->ops->ioctl(file, cmd, arg);

    return -ENOTTY;
}

ssize_t vfs_fileread(struct vfs_file *file, void *buf, size_t n, bool user) {
    if (file == NULL) {
        return -EBADF;
    }
    if (buf == NULL) {
        return -EFAULT;
    }
    if (n == 0) {
        return 0; // POSIX: zero-length read succeeds
    }

    struct vfs_inode *inode = vfs_inode_deref(&file->inode);

    // Handle pipe/socket read - these don't have inodes
    if (inode == NULL) {
        if (file->ops == NULL || file->ops->read == NULL) {
            return -EBADF; // Not a readable file object
        }
        __vfs_file_lock(file);
        if ((file->f_flags & O_ACCMODE) == O_WRONLY) {
            __vfs_file_unlock(file);
            return -EBADF; // File not opened for reading
        }
        ssize_t ret = file->ops->read(file, buf, n, user);
        __vfs_file_unlock(file);
        return ret;
    }

    // Handle character device read - check access and call without holding lock
    // (cdev operations may sleep and handle their own synchronization).
    // The file lock is released before the cdev call so that a blocking read
    // on one fd (e.g. stdin) does not prevent concurrent writes on another fd
    // (e.g. stdout) that shares the same vfs_file via dup().
    if (S_ISCHR(inode->mode)) {
        if ((file->f_flags & O_ACCMODE) == O_WRONLY) {
            return -EBADF;
        }
        ssize_t ret;
        if (file->ops != NULL && file->ops->read != NULL) {
            /* Custom file ops installed by cdev open_file (e.g. PTY master) */
            ret = file->ops->read(file, buf, n, user);
        } else {
            struct cdev *cdev = file->cdev;
            ret = cdev_read(cdev, user, buf, n);
        }
        return ret;
    }

    __vfs_file_lock(file);
    if ((file->f_flags & O_ACCMODE) == O_WRONLY) {
        __vfs_file_unlock(file);
        return -EBADF; // File not opened for reading
    }

    ssize_t ret = 0;

    // Handle block device read - not directly supported, use buffer cache
    if (S_ISBLK(inode->mode)) {
        __vfs_file_unlock(file);
        return -EOPNOTSUPP; // Operation not supported on this file
    }

    // Regular files
    // Note: We do NOT lock the inode here. The driver callback (e.g.,
    // xv6fs_file_read) is responsible for acquiring the inode lock to read size
    // and data. This avoids lock ordering issues where the driver may need to
    // acquire transactions that conflict with VFS locking order (transaction →
    // superblock → inode).
    if (!S_ISREG(inode->mode)) {
        ret = S_ISDIR(inode->mode) ? -EISDIR : -EINVAL;
        goto out;
    }
    if (file->ops == NULL || file->ops->read == NULL) {
        ret = -EOPNOTSUPP; // Read operation not supported
        goto out;
    }
    // Pass requested size to driver; driver handles EOF and size checks
    // internally
    ret = file->ops->read(file, buf, n, user);
    if (ret > 0) {
        file->f_pos += ret;
    }
out:
    __vfs_file_unlock(file);
    return ret;
}

int vfs_filestat(struct vfs_file *file, struct stat *stat) {
    if (file == NULL) {
        return -EBADF;
    }
    if (stat == NULL) {
        return -EFAULT;
    }

    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (inode == NULL) {
        /*
         * Custom file descriptors (PTY slaves, etc.) have no backing inode.
         * Return a synthetic stat indicating a character device so that
         * isatty() works correctly.
         */
        if (file->ops != NULL) {
            memset(stat, 0, sizeof(*stat));
            stat->st_mode = S_IFCHR | 0666;
            stat->st_blksize = 4096;
            return 0;
        }
        return -EBADF;
    }

    if (inode->ops && inode->ops->getattr) {
        memset(stat, 0, sizeof(*stat));
        return inode->ops->getattr(inode, stat);
    }

    // Generic fallback when filesystem doesn't implement inode getattr yet.
    vfs_ilock(inode);
    memset(stat, 0, sizeof(*stat));
    stat->st_dev = inode->sb ? (uint64)inode->sb : 0;
    stat->st_ino = inode->ino;
    stat->st_mode = inode->mode;
    stat->st_nlink = inode->n_links;
    stat->st_uid = inode->uid;
    stat->st_gid = inode->gid;
    stat->st_size = inode->size;
    stat->st_blksize = 4096;
    stat->st_blocks = (inode->size + 511) >> 9;
    stat->st_atime_sec = inode->atime;
    stat->st_mtime_sec = inode->mtime;
    stat->st_ctime_sec = inode->ctime;
    vfs_iunlock(inode);
    return 0;
}

ssize_t vfs_filewrite(struct vfs_file *file, const void *buf, size_t n,
                      bool user) {
    if (file == NULL) {
        return -EBADF;
    }
    if (buf == NULL) {
        return -EFAULT;
    }
    if (n == 0) {
        return 0; // POSIX: zero-length write succeeds
    }

    struct vfs_inode *inode = vfs_inode_deref(&file->inode);

    // Handle pipe/socket write - these don't have inodes
    if (inode == NULL) {
        if (file->ops == NULL || file->ops->write == NULL) {
            return -EBADF; // Not a writable file object
        }
        __vfs_file_lock(file);
        if ((file->f_flags & O_ACCMODE) == O_RDONLY) {
            __vfs_file_unlock(file);
            return -EBADF; // File not opened for writing
        }
        ssize_t ret = file->ops->write(file, buf, n, user);
        __vfs_file_unlock(file);
        return ret;
    }

    // Handle character device write - check access and call without holding
    // lock (cdev operations may sleep and handle their own synchronization).
    // See vfs_fileread() comment for rationale.
    if (S_ISCHR(inode->mode)) {
        if ((file->f_flags & O_ACCMODE) == O_RDONLY) {
            return -EBADF;
        }
        ssize_t ret;
        if (file->ops != NULL && file->ops->write != NULL) {
            /* Custom file ops installed by cdev open_file (e.g. PTY master) */
            ret = file->ops->write(file, buf, n, user);
        } else {
            struct cdev *cdev = file->cdev;
            ret = cdev_write(cdev, user, buf, n);
        }
        return ret;
    }

    __vfs_file_lock(file);
    if ((file->f_flags & O_ACCMODE) == O_RDONLY) {
        __vfs_file_unlock(file);
        return -EBADF; // File not opened for writing
    }

    ssize_t ret = 0;

    // Handle block device write - not directly supported, use buffer cache
    if (S_ISBLK(inode->mode)) {
        __vfs_file_unlock(file);
        return -EOPNOTSUPP; // Operation not supported on this file
    }

    // Regular files
    // Note: We do NOT lock the inode here. The driver callback (e.g.,
    // xv6fs_file_write) is responsible for acquiring transactions and inode
    // lock in the correct order: transaction → inode lock. This avoids deadlock
    // because xv6fs_truncate also needs to acquire transactions, and we can't
    // hold the inode lock when calling into the filesystem which needs
    // transactions. The file lock still protects file position and serializes
    // concurrent writes.
    if (!S_ISREG(inode->mode)) {
        ret = S_ISDIR(inode->mode) ? -EISDIR : -EINVAL;
        goto out;
    }
    if (file->ops == NULL || file->ops->write == NULL) {
        ret = -EOPNOTSUPP; // Write operation not supported
        goto out;
    }
    // The driver handles file extension, size updates, and truncation
    // internally
    ret = file->ops->write(file, buf, n, user);
    if (ret > 0) {
        file->f_pos += ret;
        /* kqueue: notify EVFILT_VNODE watchers of write to file */
        struct vfs_inode *ino = vfs_inode_deref(&file->inode);
        if (ino != NULL)
            vfs_inode_knote_notify(ino, NOTE_WRITE);
    }
out:
    __vfs_file_unlock(file);
    return ret;
}

/******************************************************************************
 * VFS Vectored Read (readv)
 ******************************************************************************/

/*
 * __vfs_generic_readv_locked - fall-back loop over per-segment read()
 *
 * Called with the file lock held.  Iterates the iov_iter and calls the
 * per-file read() callback for each segment.  This is the slow path used
 * when the filesystem does not provide a native readv callback.
 */
static ssize_t __vfs_generic_readv_locked(struct vfs_file *file,
                                          struct iov_iter *iter, bool user,
                                          bool advance_pos)
{
    ssize_t total = 0;
    while (iter->nr_segs > 0 && iter->count > 0) {
        size_t seg_len = iter->iov->iov_len - iter->iov_off;
        if (seg_len == 0) {
            iov_iter_advance(iter, 0);
            continue;
        }
        uint64 base = iter->iov->iov_base + iter->iov_off;
        ssize_t n = file->ops->read(file, (char *)base, seg_len, user);
        if (n < 0) {
            if (total > 0) break;
            return n;
        }
        if (n == 0) break; /* EOF */
        total += n;
        if (advance_pos)
            file->f_pos += n;
        iov_iter_advance(iter, (size_t)n);
        if ((size_t)n < seg_len) break; /* short read */
    }
    return total;
}

ssize_t vfs_filereadv(struct vfs_file *file, struct iov_iter *iter, bool user)
{
    if (file == NULL)
        return -EBADF;
    if (iter == NULL || iter->count == 0)
        return 0;

    struct vfs_inode *inode = vfs_inode_deref(&file->inode);

    /* Pipe / socket / custom-fd path (no backing inode) */
    if (inode == NULL) {
        if (file->ops == NULL || (file->ops->readv == NULL && file->ops->read == NULL))
            return -EBADF;
        __vfs_file_lock(file);
        if ((file->f_flags & O_ACCMODE) == O_WRONLY) {
            __vfs_file_unlock(file);
            return -EBADF;
        }
        ssize_t ret;
        if (file->ops->readv != NULL)
            ret = file->ops->readv(file, iter, user);
        else
            ret = __vfs_generic_readv_locked(file, iter, user, false);
        __vfs_file_unlock(file);
        return ret;
    }

    /* Character device — no file lock held (see vfs_fileread comment) */
    if (S_ISCHR(inode->mode)) {
        if ((file->f_flags & O_ACCMODE) == O_WRONLY) {
            return -EBADF;
        }
        ssize_t ret;
        if (file->ops != NULL && file->ops->readv != NULL) {
            ret = file->ops->readv(file, iter, user);
        } else if (file->ops != NULL && file->ops->read != NULL) {
            ret = __vfs_generic_readv_locked(file, iter, user, false);
        } else {
            /* cdev read — fall back to per-segment cdev_read */
            ssize_t total = 0;
            struct cdev *cdev = file->cdev;
            while (iter->nr_segs > 0 && iter->count > 0) {
                size_t seg_len = iter->iov->iov_len - iter->iov_off;
                if (seg_len == 0) { iov_iter_advance(iter, 0); continue; }
                uint64 base = iter->iov->iov_base + iter->iov_off;
                ssize_t n = cdev_read(cdev, user, (char *)base, seg_len);
                if (n < 0) { if (total > 0) break; total = n; break; }
                if (n == 0) break;
                total += n;
                iov_iter_advance(iter, (size_t)n);
                if ((size_t)n < seg_len) break;
            }
            ret = total;
        }
        return ret;
    }

    __vfs_file_lock(file);
    if ((file->f_flags & O_ACCMODE) == O_WRONLY) {
        __vfs_file_unlock(file);
        return -EBADF;
    }

    ssize_t ret = 0;

    if (S_ISBLK(inode->mode)) {
        __vfs_file_unlock(file);
        return -EOPNOTSUPP;
    }

    if (!S_ISREG(inode->mode)) {
        ret = S_ISDIR(inode->mode) ? -EISDIR : -EINVAL;
        goto readv_out;
    }

    if (file->ops == NULL || (file->ops->readv == NULL && file->ops->read == NULL)) {
        ret = -EOPNOTSUPP;
        goto readv_out;
    }

    /* Prefer native readv; fall back to per-segment read */
    if (file->ops->readv != NULL) {
        ret = file->ops->readv(file, iter, user);
    } else {
        ret = __vfs_generic_readv_locked(file, iter, user, true);
    }

readv_out:
    __vfs_file_unlock(file);
    return ret;
}

/******************************************************************************
 * VFS Vectored Write (writev)
 ******************************************************************************/

/*
 * __vfs_generic_writev_locked - fall-back loop over per-segment write()
 *
 * Called with the file lock held.
 */
static ssize_t __vfs_generic_writev_locked(struct vfs_file *file,
                                           struct iov_iter *iter, bool user,
                                           bool advance_pos)
{
    ssize_t total = 0;
    while (iter->nr_segs > 0 && iter->count > 0) {
        size_t seg_len = iter->iov->iov_len - iter->iov_off;
        if (seg_len == 0) {
            iov_iter_advance(iter, 0);
            continue;
        }
        uint64 base = iter->iov->iov_base + iter->iov_off;
        ssize_t n = file->ops->write(file, (const char *)base, seg_len, user);
        if (n < 0) {
            if (total > 0) break;
            return n;
        }
        total += n;
        if (advance_pos)
            file->f_pos += n;
        iov_iter_advance(iter, (size_t)n);
        if ((size_t)n < seg_len) break; /* short write */
    }
    return total;
}

ssize_t vfs_filewritev(struct vfs_file *file, struct iov_iter *iter, bool user)
{
    if (file == NULL)
        return -EBADF;
    if (iter == NULL || iter->count == 0)
        return 0;

    struct vfs_inode *inode = vfs_inode_deref(&file->inode);

    /* Pipe / socket / custom-fd path (no backing inode) */
    if (inode == NULL) {
        if (file->ops == NULL || (file->ops->writev == NULL && file->ops->write == NULL))
            return -EBADF;
        __vfs_file_lock(file);
        if ((file->f_flags & O_ACCMODE) == O_RDONLY) {
            __vfs_file_unlock(file);
            return -EBADF;
        }
        ssize_t ret;
        if (file->ops->writev != NULL)
            ret = file->ops->writev(file, iter, user);
        else
            ret = __vfs_generic_writev_locked(file, iter, user, false);
        __vfs_file_unlock(file);
        return ret;
    }

    /* Character device — no file lock held (see vfs_fileread comment) */
    if (S_ISCHR(inode->mode)) {
        if ((file->f_flags & O_ACCMODE) == O_RDONLY) {
            return -EBADF;
        }
        ssize_t ret;
        if (file->ops != NULL && file->ops->writev != NULL) {
            ret = file->ops->writev(file, iter, user);
        } else if (file->ops != NULL && file->ops->write != NULL) {
            ret = __vfs_generic_writev_locked(file, iter, user, false);
        } else {
            /* cdev write — per-segment cdev_write */
            ssize_t total = 0;
            struct cdev *cdev = file->cdev;
            while (iter->nr_segs > 0 && iter->count > 0) {
                size_t seg_len = iter->iov->iov_len - iter->iov_off;
                if (seg_len == 0) { iov_iter_advance(iter, 0); continue; }
                uint64 base = iter->iov->iov_base + iter->iov_off;
                ssize_t n = cdev_write(cdev, user, (const char *)base, seg_len);
                if (n < 0) { if (total > 0) break; total = n; break; }
                total += n;
                iov_iter_advance(iter, (size_t)n);
                if ((size_t)n < seg_len) break;
            }
            ret = total;
        }
        if (ret > 0) {
            struct vfs_inode *ino = vfs_inode_deref(&file->inode);
            if (ino != NULL)
                vfs_inode_knote_notify(ino, NOTE_WRITE);
        }
        return ret;
    }

    __vfs_file_lock(file);
    if ((file->f_flags & O_ACCMODE) == O_RDONLY) {
        __vfs_file_unlock(file);
        return -EBADF;
    }

    ssize_t ret = 0;

    if (S_ISBLK(inode->mode)) {
        __vfs_file_unlock(file);
        return -EOPNOTSUPP;
    }

    if (!S_ISREG(inode->mode)) {
        ret = S_ISDIR(inode->mode) ? -EISDIR : -EINVAL;
        goto writev_out;
    }

    if (file->ops == NULL || (file->ops->writev == NULL && file->ops->write == NULL)) {
        ret = -EOPNOTSUPP;
        goto writev_out;
    }

    /* Prefer native writev; fall back to per-segment write */
    if (file->ops->writev != NULL) {
        ret = file->ops->writev(file, iter, user);
    } else {
        ret = __vfs_generic_writev_locked(file, iter, user, true);
    }

    if (ret > 0) {
        struct vfs_inode *ino = vfs_inode_deref(&file->inode);
        if (ino != NULL)
            vfs_inode_knote_notify(ino, NOTE_WRITE);
    }

writev_out:
    __vfs_file_unlock(file);
    return ret;
}

loff_t vfs_filelseek(struct vfs_file *file, loff_t offset, int whence) {
    if (file == NULL) {
        return -EBADF; // Invalid file descriptor
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
    // Note: We do NOT lock the inode here. The driver callback (e.g.,
    // xv6fs_file_llseek) is responsible for acquiring the inode lock when
    // needed (e.g., for SEEK_END). This matches the design used for read/write
    // operations.
    if (file->ops == NULL || file->ops->llseek == NULL) {
        ret = -ESPIPE; // Not seekable
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
        return -EBADF; // Invalid file descriptor
    }
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (inode == NULL) {
        return -EINVAL; // Invalid arguments
    }

    // truncate only applies to regular files
    if (!S_ISREG(inode->mode)) {
        return S_ISDIR(inode->mode) ? -EISDIR : -EINVAL;
    }
    if (length < 0) {
        return -EINVAL;
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

    pi = pipe_alloc(0);
    if (IS_ERR(pi)) {
        __vfs_file_free(*rf);
        __vfs_file_free(*wf);
        *rf = NULL;
        *wf = NULL;
        return PTR_ERR(pi);
    }

    // Initialize read file
    pipe_open(*rf, pi, O_RDONLY);
    __vfs_ftable_attatch(*rf);

    // Initialize write file
    pipe_open(*wf, pi, O_WRONLY);
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
    spinlock_t lock;   // protects the rxq
    struct mbufq rxq;  // a queue of packets waiting to be received
};

extern spinlock_t sock_lock;
extern struct sock *sockets;

/**
 * Allocate a vfs_file with caller-supplied ops and private_data, attach it
 * to the global file table, install it into the current process's fd table,
 * and return the fd number.  Returns negative errno on failure.
 */
int vfs_custom_fd_alloc(struct vfs_file_ops *ops, void *private_data,
                        int flags)
{
    struct vfs_file *f = __vfs_file_alloc();
    if (f == NULL)
        return -ENOMEM;

    f->f_flags      = flags;
    f->ops          = ops;
    f->private_data = private_data;
    __vfs_ftable_attatch(f);

    spin_lock(&current->fdtable->lock);
    int fd = vfs_fdtable_alloc_fd(current->fdtable, f);
    spin_unlock(&current->fdtable->lock);
    vfs_fput(f);          /* drop our ref; fdtable now owns it */

    if (fd < 0) {
        return -EMFILE;
    }

    return fd;
}

int vfs_sockalloc(struct vfs_file **f, uint32 raddr, uint16 lport,
                  uint16 rport) {
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
    spin_lock(&sock_lock);
    pos = sockets;
    while (pos) {
        if (pos->raddr == raddr && pos->lport == lport && pos->rport == rport) {
            spin_unlock(&sock_lock);
            kfree((char *)si);
            __vfs_ftable_detatch(*f);
            __vfs_file_free(*f);
            *f = NULL;
            return -EADDRINUSE;
        }
        pos = pos->next;
    }
    si->next = sockets;
    sockets = si;
    spin_unlock(&sock_lock);

    return 0;
}
