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

struct vfs_file *vfs_fileopen(struct vfs_inode *inode, int f_flags) {
    if (inode == NULL || inode->sb == NULL) {
        return ERR_PTR(-EINVAL); // Invalid arguments
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
        // File descriptors are shared shrough dup, thus when refcount reach 1,
        // no other process will be using it. No need to lock the file structure.
        __vfs_ftable_detatch(file);
        vfs_inode_put_ref(&file->inode);
        __vfs_file_free(file);
    }
}

struct vfs_file *vfs_filedup(struct vfs_file *file) {
    if (file == NULL) {
        return NULL;
    }
    bool success = atomic_inc_unless(&file->ref_count, 0);
    assert(success, "vfs_filedup: trying to dup a closed file");
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
    vfs_ilock(inode);
    ssize_t ret = __vfs_inode_valid(inode);
    if (ret != 0) {
        goto out;
    }
    if (!S_ISREG(inode->mode) && !S_ISCHR(inode->mode) && !S_ISBLK(inode->mode)) {
        // @TODO: other types of file
        ret = -EINVAL; // Inode is not a regular file or device
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
    if (file->ops == NULL || file->ops->stat == NULL) {
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
    vfs_ilock(inode);
    ssize_t ret = __vfs_inode_valid(inode);
    if (ret != 0) {
        goto out;
    }
    if (!S_ISREG(inode->mode) && !S_ISCHR(inode->mode) && !S_ISBLK(inode->mode)) {
        // @TODO: other types of file
        ret = -EINVAL; // Inode is not a regular file or device
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
        if (!(file->f_flags & O_APPEND)) {
            // For simplicity, we do not support sparse file write
            ret = -EFBIG; // File too large
            goto out;
        }
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
    __vfs_file_lock(file);
    // ilock will be acquired in vfs_truncate
    int ret = vfs_truncate(inode, length);
    __vfs_file_unlock(file);
    return ret;
}
