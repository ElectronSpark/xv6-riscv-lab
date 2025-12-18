#include "types.h"
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
#include "vfs_private.h"
#include "list.h"
#include "hlist.h"
#include "slab.h"

// When need to acquire multiple inode locks:
// - First acquire directory inode lock
// - When both are non-directory inodes, acquire the one at the lower memory address first
// - @TODO: because we do not have dcache, we don't consider hierarchy of inodes here
// - When both inodes are directories, acquire the one at the lower memory address first
// - Do not acquire inodes cross filesystem at the same time
// to prevent deadlock.

/******************************************************************************
 * Inode Private APIs
 *****************************************************************************/
// Initilize VFS managed inode fields
// Will be used to initialize a newly allocated inode(returned from get_inode callback)
// before adding it  to the inode hash list
// Caller should ensure the inode pointer is valid
void __vfs_inode_init(struct vfs_inode *inode, struct vfs_superblock *sb) {
    mutex_init(&inode->mutex, "vfs_inode_mutex");
    completion_init(&inode->completion);
    hlist_entry_init(&inode->hash_entry);
    inode->ref_count = 1;
    inode->sb = sb;
}

 /******************************************************************************
 * Inode Public APIs
 *****************************************************************************/

void vfs_ilock(struct vfs_inode *inode) {
    assert(inode != NULL, "vfs_ilock: inode is NULL");
    assert(mutex_lock(&inode->mutex) == 0, "vfs_ilock: failed to lock inode mutex");
}

void vfs_iunlock(struct vfs_inode *inode) {
    assert(inode != NULL, "vfs_iunlock: inode is NULL");
    mutex_unlock(&inode->mutex);
}

void vfs_idup(struct vfs_inode *inode) {
    assert(inode != NULL, "vfs_idup: inode is NULL");
    assert(inode->sb != NULL, "vfs_idup: inode's superblock is NULL");
    bool success = atomic_inc_unless(&inode->ref_count, VFS_INODE_MAX_REFCOUNT);
    assert(success, "vfs_idup: inode refcount overflow");
}

// Decrease inode ref count and release inode lock
// Will assume the caller not holding the lock of the inode and its ancestors or the superblock lock
void vfs_iput(struct vfs_inode *inode) {
    assert(inode != NULL, "vfs_iput: inode is NULL");
    assert(inode->sb != NULL, "vfs_iput: inode's superblock is NULL");
    // @TODO:
    // assert(!rwlock_is_write_holding(&inode->sb->lock),
    //        "vfs_iput: cannot hold superblock read lock when calling");
    // assert(!holding_mutex(&inode->mutex), "vfs_iput: cannot hold inode lock when calling");

    // tried to cleanup the inode but failed
    bool failed_clean = false;
    struct vfs_inode *parent = NULL;
    struct vfs_superblock *sb = inode->sb;
    int ret = 0;

retry:
    // If refcount is greater than 1, just decrease and return
    if (atomic_dec_unless(&inode->ref_count, 1)) {
        return;
    }

    assert(!rwlock_is_write_holding(&inode->sb->lock),
           "vfs_iput: cannot hold superblock read lock when calling");
    assert(!holding_mutex(&inode->mutex), "vfs_iput: cannot hold inode lock when calling");

    // acquire related locks to delete the inode
    vfs_superblock_wlock(sb);
    vfs_ilock(inode);

    assert(inode->type != VFS_I_TYPE_MNT,
           "vfs_iput: refount of mountpoint inode reached zero");

    // Retry decreasing refcount again, as it may have changed meanwhile
    if (atomic_dec_unless(&inode->ref_count, 1)) {
        vfs_iunlock(inode);
        goto out_locked;
    }

    // If no one increased its refcount meanwhile, we can delete it
    // First check if it is dirty and sync if needed
    // If sync failed, just delete it.
    if (inode->dirty && inode->valid && !failed_clean) {
        vfs_iunlock(inode);
        vfs_superblock_unlock(inode->sb);
        failed_clean = vfs_sync_inode(inode) != 0;
        // Someone else may have acquired the inode meanwhile, so retry
        goto retry;
    }

    if (inode->type == VFS_I_TYPE_DIR) {
        parent = inode->parent;
    }

    ret = vfs_remove_inode(inode->sb, inode);
    assert(ret == 0, "vfs_iput: failed to remove inode from superblock inode cache");
    vfs_iunlock(inode);
    
out_locked:
    vfs_superblock_unlock(sb);
    assert(completion_done(&inode->completion),
           "vfs_iput: someone is waiting on inode completion without reference");
    inode->ops->free_inode(inode);

    // If this is a directory inode, decrease the refcount of its parent
    if (parent != NULL) {
        // Due to the limited kernel space stack, we avoid recursive calls here
        inode = parent;
        parent = NULL;
        failed_clean = false;
        goto retry;
    }
}

// Mark inode as dirty
int vfs_dirty_inode(struct vfs_inode *inode) {
    if (inode == NULL || inode->sb == NULL) {
        return -EINVAL; // Invalid argument
    }
    int ret = __vfs_inode_valid(inode);
    if (ret != 0) {
        return ret;
    }

    if (inode->ops->dirty_inode != NULL) {
        ret = inode->ops->dirty_inode(inode);
    }
    return ret;
}

// Sync inode to disk
int vfs_sync_inode(struct vfs_inode *inode) {
    if (inode == NULL || inode->sb == NULL) {
        return -EINVAL; // Invalid argument
    }
    int ret = __vfs_inode_valid(inode);
    if (ret != 0) {
        return ret; // Inode is not valid or caller does not hold the ilock
    }

    if (inode->ops->sync_inode != NULL) {
        ret = inode->ops->sync_inode(inode);
    }
    return ret;
}

// Lookup a dentry in a directory inode
int vfs_ilookup(struct vfs_inode *dir, struct vfs_dentry *dentry, 
                const char *name, size_t name_len, bool user) {
    if (dir == NULL || dir->sb == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (dentry == NULL || name == NULL || name_len == 0) {
        return -EINVAL; // Invalid argument
    }
    vfs_superblock_rlock(dir->sb);
    vfs_ilock(dir);
    int ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        goto out;
    }
    if (dir->type != VFS_I_TYPE_DIR && dir->type != VFS_I_TYPE_ROOT) {
        ret = -ENOTDIR; // Inode is not a directory
        goto out;
    }
    if (dir->ops->lookup == NULL) {
        ret = -ENOSYS; // Lookup operation not supported
        goto out;
    }
    ret = dir->ops->lookup(dir, dentry, name, name_len, user);
out:
    vfs_iunlock(dir);
    vfs_superblock_unlock(dir->sb);
    return ret;
}

int vfs_readlink(struct vfs_inode *inode, char *buf, size_t buflen, bool user) {
    if (inode == NULL || inode->sb == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (buf == NULL || buflen == 0) {
        return -EINVAL; // Invalid argument
    }
    vfs_ilock(inode);
    int ret = __vfs_inode_valid(inode);
    if (ret != 0) {
        goto out;
    }
    if (inode->type != VFS_I_TYPE_SYMLINK) {
        ret = -EINVAL; // Inode is not a symlink
        goto out;
    }
    if (inode->ops->readlink == NULL) {
        ret = -ENOSYS; // Readlink operation not supported
        goto out;
    }
    ret = inode->ops->readlink(inode, buf, buflen, user);
out:
    vfs_iunlock(inode);
    return ret;
}

int vfs_create(struct vfs_inode *dir, uint32 mode, struct vfs_inode **new_inode,
               const char *name, size_t name_len, bool user) {
    if (dir == NULL || dir->sb == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (name == NULL || name_len == 0 || new_inode == NULL) {
        return -EINVAL; // Invalid argument
    }
    vfs_superblock_wlock(dir->sb);
    vfs_ilock(dir);
    int ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        goto out;
    }
    if (dir->type != VFS_I_TYPE_DIR && dir->type != VFS_I_TYPE_ROOT) {
        ret = -ENOTDIR; // Inode is not a directory
        goto out;
    }
    if (dir->ops->create == NULL) {
        ret = -ENOSYS; // Create operation not supported
        goto out;
    }
    ret = dir->ops->create(dir, mode, new_inode, name, name_len, user);
out:
    vfs_iunlock(dir);
    vfs_superblock_unlock(dir->sb);
    return ret;
}

int vfs_mknod(struct vfs_inode *dir, uint32 mode, struct vfs_inode **new_inode, 
              dev_t dev, const char *name, size_t name_len, bool user) {
    if (dir == NULL || dir->sb == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (name == NULL || name_len == 0 || new_inode == NULL) {
        return -EINVAL; // Invalid argument
    }
    vfs_superblock_wlock(dir->sb);
    vfs_ilock(dir);
    int ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        goto out;
    }
    if (dir->type != VFS_I_TYPE_DIR && dir->type != VFS_I_TYPE_ROOT) {
        ret = -ENOTDIR; // Inode is not a directory
        goto out;
    }
    if (dir->ops->mknod == NULL) {
        ret = -ENOSYS; // mknod operation not supported
        goto out;
    }
    ret = dir->ops->mknod(dir, mode, new_inode, dev, name, name_len, user);
out:
    vfs_iunlock(dir);
    vfs_superblock_unlock(dir->sb);
    return ret;
}

int vfs_link(struct vfs_dentry *old, struct vfs_inode *dir, 
             const char *name, size_t name_len, bool user) {
    if (dir == NULL || dir->sb == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (name == NULL || name_len == 0 || old == NULL) {
        return -EINVAL; // Invalid argument
    }
    vfs_superblock_wlock(dir->sb);
    vfs_ilock(dir);
    int ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        goto out;
    }
    if (dir->type != VFS_I_TYPE_DIR && dir->type != VFS_I_TYPE_ROOT) {
        ret = -ENOTDIR; // Inode is not a directory
        goto out;
    }
    if (dir->ops->link == NULL) {
        ret = -ENOSYS; // Link operation not supported
        goto out;
    }
    ret = dir->ops->link(old, dir, name, name_len, user);
out:
    vfs_iunlock(dir);
    vfs_superblock_unlock(dir->sb);
    return ret;
}

int vfs_unlink(struct vfs_inode *dir, const char *name, size_t name_len, bool user) {
    if (dir == NULL || dir->sb == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (name == NULL || name_len == 0) {
        return -EINVAL; // Invalid argument
    }
    vfs_superblock_wlock(dir->sb);
    vfs_ilock(dir);
    int ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        goto out;
    }
    if (dir->type != VFS_I_TYPE_DIR && dir->type != VFS_I_TYPE_ROOT) {
        ret = -EISDIR; // Inode is a directory
        goto out;
    }
    if (dir->ops->unlink == NULL) {
        ret = -ENOSYS; // Unlink operation not supported
        goto out;
    }
    ret = dir->ops->unlink(dir, name, name_len, user);
out:
    vfs_iunlock(dir);
    vfs_superblock_unlock(dir->sb);
    return ret;
}

int vfs_mkdir(struct vfs_inode *dir, uint32 mode, struct vfs_inode **new_dir,
              const char *name, size_t name_len, bool user) {
    if (dir == NULL || dir->sb == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (name == NULL || name_len == 0) {
        return -EINVAL; // Invalid argument
    }
    vfs_superblock_wlock(dir->sb);
    vfs_ilock(dir);
    int ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        goto out;
    }
    if (dir->type != VFS_I_TYPE_DIR && dir->type != VFS_I_TYPE_ROOT) {
        ret = -ENOTDIR; // Inode is not a directory
        goto out;
    }
    if (dir->ops->mkdir == NULL) {
        ret = -ENOSYS; // Mkdir operation not supported
        goto out;
    }
    ret = dir->ops->mkdir(dir, mode, new_dir, name, name_len, user);
out:
    vfs_iunlock(dir);
    vfs_superblock_unlock(dir->sb);
    return ret;
}

int vfs_rmdir(struct vfs_inode *dir, const char *name, size_t name_len, bool user) {
    if (dir == NULL || dir->sb == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (name == NULL || name_len == 0) {
        return -EINVAL; // Invalid argument
    }
    vfs_superblock_wlock(dir->sb);
    vfs_ilock(dir);
    int ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        goto out;
    }
    if (dir->type != VFS_I_TYPE_DIR && dir->type != VFS_I_TYPE_ROOT) {
        ret = -ENOTDIR; // Inode is not a directory
        goto out;
    }
    if (dir->ops->rmdir == NULL) {
        ret = -ENOSYS; // Rmdir operation not supported
        goto out;
    }
    ret = dir->ops->rmdir(dir, name, name_len, user);
out:
    vfs_iunlock(dir);
    vfs_superblock_unlock(dir->sb);
    return ret;
}

// @TODO:
int vfs_move(struct vfs_inode *old_dir, struct vfs_dentry *old_dentry,
             struct vfs_inode *new_dir, const char *name, size_t name_len, 
             bool user) {
    if (old_dir == NULL || old_dir->sb == NULL ||
        new_dir == NULL || new_dir->sb == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (old_dentry == NULL || name == NULL || name_len == 0) {
        return -EINVAL; // Invalid arguments
    }
    int ret = __vfs_inode_valid(old_dir);
    if (ret != 0 && ret != -EPERM) {
        return ret;
    }
    ret = __vfs_inode_valid(new_dir);
    if (ret != 0 && ret != -EPERM) {
        return ret;
    }
    if (old_dir->sb != new_dir->sb) {
        return -EXDEV; // Cross-device move not supported
    }
    vfs_superblock_wlock(old_dir->sb);
    if (old_dir->type != VFS_I_TYPE_DIR && old_dir->type != VFS_I_TYPE_ROOT) {
        ret = -ENOTDIR; // Inode is not a directory
        goto out;
    }
    if (new_dir->type != VFS_I_TYPE_DIR && new_dir->type != VFS_I_TYPE_ROOT) {
        ret = -ENOTDIR; // Inode is not a directory
        goto out;
    }
    if (old_dir->ops->move == NULL || old_dir->ops->move != new_dir->ops->move) {
        ret = -ENOSYS; // Move operation not supported
        goto out;
    }
    ret = old_dir->ops->move(old_dir, old_dentry, new_dir, name, name_len, user);
out:
    vfs_superblock_unlock(old_dir->sb);
    return ret;
}

int vfs_symlink(struct vfs_inode *dir, struct vfs_inode **new_inode,
                uint32 mode, const char *name, size_t name_len,
                const char *target, size_t target_len, bool user) {
    if (dir == NULL || dir->sb == NULL || new_inode == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (target == NULL || target_len == 0 || target_len > VFS_PATH_MAX) {
        return -EINVAL; // Invalid argument
    }
    if (name == NULL || name_len == 0) {
        return -EINVAL; // Invalid symlink name
    }
    vfs_superblock_wlock(dir->sb);
    vfs_ilock(dir);
    int ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        goto out;
    }
    if (dir->type != VFS_I_TYPE_DIR && dir->type != VFS_I_TYPE_ROOT) {
        ret = -ENOTDIR; // Inode is not a directory
        goto out;
    }
    if (dir->ops->symlink == NULL) {
        ret = -ENOSYS; // Symlink operation not supported
        goto out;
    }
    ret = dir->ops->symlink(dir, new_inode, mode, name, name_len, target, target_len, user);
out:
    vfs_iunlock(dir);
    vfs_superblock_unlock(dir->sb);
    return ret;
}

int vfs_truncate(struct vfs_inode *inode, uint64 new_size) {
    if (inode == NULL || inode->sb == NULL) {
        return -EINVAL; // Invalid argument
    }
    vfs_ilock(inode);
    int ret = __vfs_inode_valid(inode);
    if (ret != 0) {
        goto out; // Inode is not valid or caller does not hold the ilock
    }
    if (inode->type != VFS_I_TYPE_FILE) {
        ret = -EINVAL; // Inode is not a regular file
        goto out;
    }
    if (inode->ops->truncate == NULL) {
        ret = -ENOSYS; // Truncate operation not supported
        goto out;
    }
    ret = inode->ops->truncate(inode, new_size);
out:
    vfs_iunlock(inode);
    return ret;
}
