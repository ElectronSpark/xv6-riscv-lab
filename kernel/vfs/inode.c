#include "types.h"
#include "riscv.h"
#include "defs.h"
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

// When need to acquire multiple inode locks, always acquire in inode number order
// to prevent deadlock.

/******************************************************************************
 * Inode Private APIs
 *****************************************************************************/

// Inode kobject release callback
static void __vfs_inode_kobj_release(struct kobject *kobj) {
    // Inodes are freed via vfs_iput, so we need a placeholder kobject callback
    // here to avoid kobject from freeing the inode memory directly.
    (void)kobj;
}

// Initilize VFS managed inode fields
// Will be used to initialize a newly allocated inode(returned from get_inode callback)
// before adding it  to the inode hash list
// Caller should ensure the inode pointer is valid
void __vfs_inode_init(struct vfs_inode *inode, struct vfs_superblock *sb) {
    mutex_init(&inode->mutex, "vfs_inode_mutex");
    completion_init(&inode->completion);
    hlist_entry_init(&inode->hash_entry);
    inode->kobj.ops.release = __vfs_inode_kobj_release;
    inode->kobj.name = "vfs_inode";
    kobject_init(&inode->kobj);
    inode->sb = sb;
}

 /******************************************************************************
 * Inode Public APIs
 *****************************************************************************/

int vfs_ilock(struct vfs_inode *inode) {
    if (inode == NULL) {
        return -EINVAL; // Invalid argument
    }
    return mutex_lock(&inode->mutex);
}

void vfs_iunlock(struct vfs_inode *inode) {
    if (inode == NULL) {
        return; // Invalid argument
    }
    mutex_unlock(&inode->mutex);
}

int vfs_idup(struct vfs_inode *inode) {
    int ret = __vfs_inode_valid(inode);
    if (ret != 0) {
        return ret; // Inode is not valid or caller does not hold the ilock
    }
    if (!holding_mutex(&inode->mutex)) {
        return -EPERM; // Caller does not hold the inode lock
    }
    kobject_get(&inode->kobj);
    return 0;
}

int vfs_ilockdup(struct vfs_inode *inode) {
    if (inode == NULL) {
        return -EINVAL; // Invalid argument
    }
    int ret = vfs_ilock(inode);
    if (ret != 0) {
        return ret; // Failed to lock inode
    }
    ret = vfs_idup(inode);
    if (ret != 0) {
        vfs_iunlock(inode);
    }
    return ret;
}

void vfs_iput (struct vfs_inode *inode) {
    if (inode == NULL) {
        return;
    }
    assert(holding_mutex(&inode->mutex), "vfs_iput: must hold inode lock");
    int64 refcount = kobject_refcount(&inode->kobj);
    if (refcount <= 1) {
        assert (refcount > 0, "vfs_iput: inode refcount underflow");
        return; // Do nothing
    }
    // Decrease ref count
    kobject_put(&inode->kobj);
}

// Decrease inode ref count and release inode lock
// Will assume the caller holds readlock of the superblock, which cannot be tested
void vfs_iputunlock(struct vfs_inode *inode) {
    int ret = 0;
    if (inode == NULL) {
        return;
    }
    assert(holding_mutex(&inode->mutex), "vfs_iput: must hold inode lock");
retry:
    int64 refcount = kobject_refcount(&inode->kobj);
    assert(refcount > 0, "vfs_iput: inode refcount underflow");
    if (refcount == 1) {
        // Last reference, need to sync the inode, detach it from superblock, and free it
        vfs_iunlock(inode);
        vfs_superblock_wlock(inode->sb);
        ret = vfs_ilock(inode);
        assert(ret == 0, "vfs_iput: failed to lock inode");
        // Double check refcount after acquiring superblock lock
        if (kobject_refcount(&inode->kobj) > 1) {
            // If other references acquired meanwhile, just release locks and return
            vfs_superblock_unlock(inode->sb);
            kobject_put(&inode->kobj);
            vfs_iunlock(inode);
            return;
        }
        if (inode->dirty && inode->valid) {
            vfs_iunlock(inode);
            vfs_superblock_unlock(inode->sb);
            int sync_ret = vfs_sync_inode(inode);
            if (sync_ret != 0) {
                printf("warning: vfs_iput: failed to sync inode %lu before deletion: %d\n",
                       inode->ino, sync_ret);
            }
            goto retry;
        }

        int remove_ret = vfs_remove_inode(inode->sb, inode);
        assert(remove_ret == 0, "vfs_iput: failed to remove inode from superblock inode cache");
        vfs_iunlock(inode);
        vfs_superblock_unlock(inode->sb);
        inode->ops->free_inode(inode);
        return;
    }
    // Decrease ref count
    kobject_put(&inode->kobj);
    vfs_iunlock(inode);
}

// Mark inode as dirty
// Caller needs to hold the ilock of the inode
// Caller should not hold additional locks beyond the inode mutex
int vfs_dirty_inode(struct vfs_inode *inode) {
    int ret = __vfs_inode_valid(inode);
    if (ret != 0) {
        return ret; // Inode is not valid or caller does not hold the ilock
    }
    if (inode->dirty) {
        return 0; // Already dirty
    }

    if (inode->ops->dirty_inode != NULL) {
        ret = inode->ops->dirty_inode(inode);
    }
    return ret;
}

// Sync inode to disk
// Caller should hold the ilock of the inode
// Caller should not hold additional locks beyond the inode mutex
int vfs_sync_inode(struct vfs_inode *inode) {
    int ret = __vfs_inode_valid(inode);
    if (ret != 0) {
        return ret; // Inode is not valid or caller does not hold the ilock
    }
    if (!inode->dirty) {
        return 0; // Inode is already clean
    }

    if (inode->ops->sync_inode != NULL) {
        ret = inode->ops->sync_inode(inode);
    }
    return ret;
}

// Lookup a dentry in a directory inode
int vfs_ilookup(struct vfs_inode *dir, struct vfs_dentry *dentry, 
                const char *name, size_t name_len, bool user) {
    if (dentry == NULL || name == NULL || name_len == 0) {
        return -EINVAL; // Invalid argument
    }
    int ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        return ret;
    }
    if (dir->type != VFS_I_TYPE_DIR && dir->type != VFS_I_TYPE_ROOT) {
        return -ENOTDIR; // Inode is not a directory
    }
    if (dir->ops->lookup == NULL) {
        return -ENOSYS; // Lookup operation not supported
    }
    return dir->ops->lookup(dir, dentry, name, name_len, user);
}

int vfs_readlink(struct vfs_inode *inode, char *buf, size_t buflen, bool user) {
    if (buf == NULL) {
        return -EINVAL; // Invalid argument
    }
    int ret = __vfs_inode_valid(inode);
    if (ret != 0) {
        return ret; // Inode is not valid or caller does not hold the ilock
    }
    if (inode->type != VFS_I_TYPE_SYMLINK) {
        return -EINVAL; // Inode is not a symlink
    }
    if (inode->ops->readlink == NULL) {
        return -ENOSYS; // Readlink operation not supported
    }
    return inode->ops->readlink(inode, buf, buflen, user);
}

int vfs_create(struct vfs_inode *dir, uint32 mode, struct vfs_inode **new_inode,
               const char *name, size_t name_len, bool user) {
    if (name == NULL || name_len == 0 || new_inode == NULL) {
        return -EINVAL; // Invalid argument
    }
    int ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        return ret;
    }
    if (!vfs_superblock_wholding(dir->sb)) {
        return -EPERM; // Caller must hold write lock of the superblock
    }
    if (dir->type != VFS_I_TYPE_DIR && dir->type != VFS_I_TYPE_ROOT) {
        return -ENOTDIR; // Inode is not a directory
    }
    if (dir->ops->create == NULL) {
        return -ENOSYS; // Create operation not supported
    }
    return dir->ops->create(dir, mode, new_inode, name, name_len, user);
}

int vfs_mknod(struct vfs_inode *dir, uint32 mode, struct vfs_inode **new_inode, 
              uint32 dev, const char *name, size_t name_len, bool user) {
    if (name == NULL || name_len == 0 || new_inode == NULL) {
        return -EINVAL; // Invalid argument
    }
    int ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        return ret;
    }
    if (!vfs_superblock_wholding(dir->sb)) {
        return -EPERM; // Caller must hold write lock of the superblock
    }
    if (dir->type != VFS_I_TYPE_DIR && dir->type != VFS_I_TYPE_ROOT) {
        return -ENOTDIR; // Inode is not a directory
    }
    if (dir->ops->mknod == NULL) {
        return -ENOSYS; // Mknod operation not supported
    }
    return dir->ops->mknod(dir, mode, new_inode, dev, name, name_len, user);
}

int vfs_link(struct vfs_dentry *old, struct vfs_inode *dir, 
             const char *name, size_t name_len, bool user) {
    if (old == NULL || name == NULL || name_len == 0) {
        return -EINVAL; // Invalid argument
    }
    int ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        return ret;
    }
    if (!vfs_superblock_wholding(dir->sb)) {
        return -EPERM; // Caller must hold write lock of the superblock
    }
    if (dir->type != VFS_I_TYPE_DIR && dir->type != VFS_I_TYPE_ROOT) {
        return -ENOTDIR; // Inode is not a directory
    }
    if (dir->ops->link == NULL) {
        return -ENOSYS; // Link operation not supported
    }
    return dir->ops->link(old, dir, name, name_len, user);
}

int vfs_unlink(struct vfs_inode *dir, const char *name, size_t name_len, bool user) {
    if (name == NULL || name_len == 0) {
        return -EINVAL; // Invalid argument
    }
    int ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        return ret;
    }
    if (!vfs_superblock_wholding(dir->sb)) {
        return -EPERM; // Caller must hold write lock of the superblock
    }
    if (dir->type != VFS_I_TYPE_DIR && dir->type != VFS_I_TYPE_ROOT) {
        return -ENOTDIR; // Inode is not a directory
    }
    if (dir->ops->unlink == NULL) {
        return -ENOSYS; // Unlink operation not supported
    }
    return dir->ops->unlink(dir, name, name_len, user);
}

int vfs_mkdir(struct vfs_inode *dir, uint32 mode, struct vfs_inode **new_dir,
              const char *name, size_t name_len, bool user) {
    if (name == NULL || name_len == 0 || new_dir == NULL) {
        return -EINVAL; // Invalid argument
    }
    int ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        return ret;
    }
    if (!vfs_superblock_wholding(dir->sb)) {
        return -EPERM; // Caller must hold write lock of the superblock
    }
    if (dir->type != VFS_I_TYPE_DIR && dir->type != VFS_I_TYPE_ROOT) {
        return -ENOTDIR; // Inode is not a directory
    }
    if (dir->ops->mkdir == NULL) {
        return -ENOSYS; // Mkdir operation not supported
    }
    return dir->ops->mkdir(dir, mode, new_dir, name, name_len, user);
}

int vfs_rmdir(struct vfs_inode *dir, const char *name, size_t name_len, bool user) {
    if (name == NULL || name_len == 0) {
        return -EINVAL; // Invalid argument
    }
    int ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        return ret;
    }
    if (!vfs_superblock_wholding(dir->sb)) {
        return -EPERM; // Caller must hold write lock of the superblock
    }
    if (dir->type != VFS_I_TYPE_DIR && dir->type != VFS_I_TYPE_ROOT) {
        return -ENOTDIR; // Inode is not a directory
    }
    if (dir->ops->rmdir == NULL) {
        return -ENOSYS; // Rmdir operation not supported
    }
    return dir->ops->rmdir(dir, name, name_len, user);
}

int vfs_move(struct vfs_inode *old_dir, struct vfs_dentry *old_dentry,
             struct vfs_inode *new_dir, const char *name, size_t name_len, 
             bool user) {
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
    if (!vfs_superblock_wholding(old_dir->sb)) {
        return -EPERM; // Caller must hold write lock of the superblock
    }
    if (old_dir->type != VFS_I_TYPE_DIR && old_dir->type != VFS_I_TYPE_ROOT) {
        return -ENOTDIR; // Inode is not a directory
    }
    if (new_dir->type != VFS_I_TYPE_DIR && new_dir->type != VFS_I_TYPE_ROOT) {
        return -ENOTDIR; // Inode is not a directory
    }
    if (old_dir->ops->move == NULL || old_dir->ops->move != new_dir->ops->move) {
        return -ENOSYS; // Move operation not supported
    }
    return old_dir->ops->move(old_dir, old_dentry, new_dir, name, name_len, user);
}

int vfs_symlink(struct vfs_inode *dir, struct vfs_inode **new_inode,
                uint32 mode, const char *name, size_t name_len,
                const char *target, size_t target_len, bool user) {
    if (dir == NULL || new_inode == NULL || target == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (name == NULL || name_len == 0) {
        return -EINVAL; // Invalid symlink name
    }
    if (target_len == 0 || target_len > VFS_PATH_MAX) {
        return -EINVAL; // Invalid symlink target length
    }
    int ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        return ret;
    }
    if (!vfs_superblock_wholding(dir->sb)) {
        return -EPERM; // Caller must hold write lock of the superblock
    }
    if (dir->type != VFS_I_TYPE_DIR && dir->type != VFS_I_TYPE_ROOT) {
        return -ENOTDIR; // Inode is not a directory
    }
    if (dir->ops->symlink == NULL) {
        return -ENOSYS; // Symlink operation not supported
    }
    return dir->ops->symlink(dir, new_inode, mode, name, name_len, target, target_len, user);
}

int vfs_truncate(struct vfs_inode *inode, uint64 new_size) {
    int ret = __vfs_inode_valid(inode);
    if (ret != 0) {
        return ret; // Inode is not valid or caller does not hold the ilock
    }
    if (!vfs_superblock_wholding(inode->sb)) {
        return -EPERM; // Caller must hold write lock of the superblock
    }
    if (inode->ops->truncate == NULL) {
        return -ENOSYS; // Truncate operation not supported
    }
    return inode->ops->truncate(inode, new_size);
}
