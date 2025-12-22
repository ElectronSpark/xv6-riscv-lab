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
#include "vfs_private.h"
#include "list.h"
#include "hlist.h"
#include "slab.h"

// When need to acquire multiple inode locks:
// - First acquire directory inode lock
// - When both are non-directory inodes, acquire the one at the lower memory address first
// - When both inodes are directories and one is ancestor of the other, acquire ancestor first
// - Otherwise, acquire the one at the lower memory address first
// - Do not acquire inodes cross filesystem at the same time
// to prevent deadlock.

/******************************************************************************
 * Inode Private APIs
 *****************************************************************************/
// Initilize VFS managed inode fields
// Will be used to initialize a newly allocated inode(returned from get_inode callback)
// before adding it  to the inode hash list
// Caller should ensure the inode pointer is valid
void __vfs_inode_init(struct vfs_inode *inode) {
    mutex_init(&inode->mutex, "vfs_inode_mutex");
    completion_init(&inode->completion);
    hlist_entry_init(&inode->hash_entry);
    inode->ref_count = 1;
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

// Decrease inode ref count; free the inode when the last reference is dropped.
// Caller must not hold the inode lock when calling (vfs_iput() will acquire locks internally
// when it needs to remove/free an inode).
void vfs_iput(struct vfs_inode *inode) {
    assert(inode != NULL, "vfs_iput: inode is NULL");
    // assert(inode->sb == NULL || !vfs_superblock_wholding(inode->sb),
    //        "vfs_iput: cannot hold superblock read lock when calling");
    // assert(!holding_mutex(&inode->mutex), "vfs_iput: cannot hold inode lock when calling");

    if (inode == &vfs_root_inode) {
        // The root inode should never be freed
        return;
    }

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

    if (sb == NULL) {
        // No superblock, just free the inode
        goto out;
    }

    // acquire related locks to delete the inode
    vfs_superblock_wlock(sb);
    vfs_ilock(inode);

    assert(!inode->mount, "vfs_iput: refcount of mountpoint inode reached zero");

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

    if (S_ISDIR(inode->mode) && inode->parent != inode) {
        // For non-root directory inode, decrease parent dir refcount
        // Root directory's parent is itself
        parent = inode->parent;
    }

    ret = vfs_remove_inode(inode->sb, inode);
    assert(ret == 0, "vfs_iput: failed to remove inode from superblock inode cache");
    vfs_iunlock(inode);
    
out_locked:
    vfs_superblock_unlock(sb);
    assert(completion_done(&inode->completion),
           "vfs_iput: someone is waiting on inode completion without reference");
out:
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
// Will assume the VFS handled "."
int vfs_ilookup(struct vfs_inode *dir, struct vfs_dentry *dentry, 
                const char *name, size_t name_len) {
    if (dir == NULL || dir->sb == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (dentry == NULL || name == NULL || name_len == 0) {
        return -EINVAL; // Invalid argument
    }
    if (name_len == 1 && name[0] == '.') {
        dentry->sb = dir->sb;
        dentry->ino = dir->ino;
        dentry->parent = dir;
        dentry->name = kmm_alloc(2);
        if (dentry->name == NULL) {
            return -ENOMEM;
        }
        dentry->name[0] = '.';
        dentry->name[1] = '\0';
        dentry->name_len = 1;
        dentry->cookies = VFS_DENTRY_COOKIE_SELF;
        return 0;
    }

    if (name_len == 2 && name[0] == '.' && name[1] == '.') {
        if (dir == myproc()->fs.rooti || vfs_inode_is_local_root(dir)) {
            struct vfs_inode *target = dir;
            if (dir != myproc()->fs.rooti && dir->sb->mountpoint != NULL) {
                target = dir->sb->mountpoint;
            } else if (dir->parent != NULL) {
                target = dir->parent;
            }

            dentry->sb = target->sb;
            dentry->ino = target->ino;
            dentry->parent = target;
            dentry->name = kmm_alloc(3);
            if (dentry->name == NULL) {
                return -ENOMEM;
            }
            dentry->name[0] = '.';
            dentry->name[1] = '.';
            dentry->name[2] = '\0';
            dentry->name_len = 2;
            dentry->cookies = VFS_DENTRY_COOKIE_PARENT;
            return 0;
        }
    }
    vfs_superblock_rlock(dir->sb);
    vfs_ilock(dir);
    int ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        goto out;
    }
    if (!S_ISDIR(dir->mode)) {
        ret = -ENOTDIR; // Inode is not a directory
        goto out;
    }
    if (dir->ops->lookup == NULL) {
        ret = -ENOSYS; // Lookup operation not supported
        goto out;
    }
    ret = dir->ops->lookup(dir, dentry, name, name_len);
out:
    vfs_iunlock(dir);
    vfs_superblock_unlock(dir->sb);
    return ret;
}

int vfs_dir_iter(struct vfs_inode *dir, struct vfs_dir_iter *iter) {
    if (dir == NULL || dir->sb == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (iter == NULL) {
        return -EINVAL; // Invalid argument
    }
    vfs_superblock_rlock(dir->sb);
    vfs_ilock(dir);
    int ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        goto out;
    }
    if (!S_ISDIR(dir->mode)) {
        ret = -ENOTDIR; // Inode is not a directory
        goto out;
    }
    if (dir->ops->dir_iter == NULL) {
        ret = -ENOSYS; // dir_iter operation not supported
        goto out;
    }
    if (iter->current.cookies == VFS_DENTRY_COOKIE_END) {
        iter->current.cookies = VFS_DENTRY_COOKIE_SELF;
        iter->current.ino = dir->ino;
        iter->current.sb = dir->sb;
        iter->current.name = kmm_alloc(2);
        memmove(iter->current.name, ".", 2);
        iter->current.name_len = 1;
        ret = 0;
        goto out;
    }
    if (iter->current.cookies == VFS_DENTRY_COOKIE_SELF && vfs_inode_is_local_root(dir)) {
        // If it's local root, ".." should indicate the mountpoint's parent
        iter->current.cookies = VFS_DENTRY_COOKIE_PARENT;
        iter->current.ino = dir->parent->ino;
        iter->current.sb = dir->sb;
        iter->current.name = kmm_alloc(3);
        memmove(iter->current.name, "..", 3);
        iter->current.name_len = 2;
        ret = 0;
        goto out;
    }
    ret = dir->ops->dir_iter(dir, iter);
out:
    vfs_iunlock(dir);
    vfs_superblock_unlock(dir->sb);
    return ret;
}

ssize_t vfs_readlink(struct vfs_inode *inode, char *buf, size_t buflen) {
    if (inode == NULL || inode->sb == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (buf == NULL || buflen == 0) {
        return -EINVAL; // Invalid argument
    }
    vfs_ilock(inode);
    ssize_t ret = __vfs_inode_valid(inode);
    if (ret != 0) {
        goto out;
    }
    if (!S_ISLNK(inode->mode)) {
        ret = -EINVAL; // Inode is not a symlink
        goto out;
    }
    if (inode->ops->readlink == NULL) {
        ret = -ENOSYS; // Readlink operation not supported
        goto out;
    }
    ret = inode->ops->readlink(inode, buf, buflen);
    if (ret >= 0 && (size_t)ret >= buflen) {
        ret = -ENAMETOOLONG;
        goto out;
    }
out:
    vfs_iunlock(inode);
    return ret;
}

struct vfs_inode *vfs_create(struct vfs_inode *dir, uint32 mode,
               const char *name, size_t name_len) {
    if (dir == NULL || dir->sb == NULL) {
        return ERR_PTR(-EINVAL); // Invalid argument
    }
    if (name == NULL || name_len == 0) {
        return ERR_PTR(-EINVAL); // Invalid argument
    }
    struct vfs_inode *ret_ptr = NULL;
    vfs_superblock_wlock(dir->sb);
    vfs_ilock(dir);
    int ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        ret_ptr = ERR_PTR(ret);
        goto out;
    }
    if (!S_ISDIR(dir->mode)) {
        ret_ptr = ERR_PTR(-ENOTDIR); // Inode is not a directory
        goto out;
    }
    if (dir->ops->create == NULL) {
        ret_ptr = ERR_PTR(-ENOSYS); // Create operation not supported
        goto out;
    }
    ret_ptr = dir->ops->create(dir, mode, name, name_len);
out:
    vfs_iunlock(dir);
    vfs_superblock_unlock(dir->sb);
    return ret_ptr;
}

struct vfs_inode *vfs_mknod(struct vfs_inode *dir, uint32 mode,
              dev_t dev, const char *name, size_t name_len) {
    if (dir == NULL || dir->sb == NULL) {
        return ERR_PTR(-EINVAL); // Invalid argument
    }
    if (name == NULL || name_len == 0) {
        return ERR_PTR(-EINVAL); // Invalid argument
    }
    struct vfs_inode *ret_ptr = NULL;
    vfs_superblock_wlock(dir->sb);
    vfs_ilock(dir);
    int ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        ret_ptr = ERR_PTR(ret);
        goto out;
    }
    if (!S_ISDIR(dir->mode)) {
        ret_ptr = ERR_PTR(-ENOTDIR); // Inode is not a directory
        goto out;
    }
    if (dir->ops->mknod == NULL) {
        ret_ptr = ERR_PTR(-ENOSYS); // mknod operation not supported
        goto out;
    }
    ret_ptr = dir->ops->mknod(dir, mode, dev, name, name_len);
out:
    vfs_iunlock(dir);
    vfs_superblock_unlock(dir->sb);
    return ret_ptr;
}

int vfs_link(struct vfs_dentry *old, struct vfs_inode *dir, 
             const char *name, size_t name_len) {
    if (dir == NULL || dir->sb == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (name == NULL || name_len == 0 || old == NULL) {
        return -EINVAL; // Invalid argument
    }
    int ret = 0;
    struct vfs_inode *target = vfs_get_dentry_inode(old);
    if (IS_ERR(target)) {
        return PTR_ERR(target);
    }
    assert(target != NULL, "vfs_link: old dentry inode is NULL");
    if (target->sb != dir->sb) {
        vfs_iput(target);
        return -EXDEV; // Cross-device hard link not supported
    }
    vfs_superblock_wlock(dir->sb);
    if (S_ISDIR(target->mode)) {
        ret = -EPERM; // Cannot create hard link to a directory
        goto out_unlock_sb;
    }
    if (!S_ISDIR(dir->mode)) {
        ret = -ENOTDIR; // Inode is not a directory
        goto out_unlock_sb;
    }
    vfs_ilock_two_nondirectories(dir, target);
    ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        goto out;
    }
    ret = __vfs_inode_valid(target);
    if (ret != 0) {
        goto out;
    }
    
    if (dir->ops->link == NULL) {
        ret = -ENOSYS; // Link operation not supported
        goto out;
    }
    ret = dir->ops->link(target, dir, name, name_len);
out:
    vfs_iunlock_two(target, dir);
out_unlock_sb:
    vfs_superblock_unlock(dir->sb);
    vfs_iput(target);
    return ret;
}

int vfs_unlink(struct vfs_inode *dir, const char *name, size_t name_len) {
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
    if (!S_ISDIR(dir->mode)) {
        ret = -EISDIR; // Inode is a directory
        goto out;
    }
    if (dir->ops->unlink == NULL) {
        ret = -ENOSYS; // Unlink operation not supported
        goto out;
    }
    ret = dir->ops->unlink(dir, name, name_len);
out:
    vfs_iunlock(dir);
    vfs_superblock_unlock(dir->sb);
    return ret;
}

struct vfs_inode *vfs_mkdir(struct vfs_inode *dir, uint32 mode,
                            const char *name, size_t name_len) {
    if (dir == NULL || dir->sb == NULL) {
        return ERR_PTR(-EINVAL); // Invalid argument
    }
    if (name == NULL || name_len == 0) {
        return ERR_PTR(-EINVAL); // Invalid argument
    }
    struct vfs_inode *ret_ptr = NULL;
    vfs_superblock_wlock(dir->sb);
    vfs_ilock(dir);
    int ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        ret_ptr = ERR_PTR(ret);
        goto out;
    }
    if (!S_ISDIR(dir->mode)) {
        ret_ptr = ERR_PTR(-ENOTDIR); // Inode is not a directory
        goto out;
    }
    if (dir->ops->mkdir == NULL) {
        ret_ptr = ERR_PTR(-ENOSYS); // Mkdir operation not supported
        goto out;
    }
    ret_ptr = dir->ops->mkdir(dir, mode, name, name_len);
    if (!IS_ERR(ret_ptr)) {
        vfs_ilock(ret_ptr);
        ret_ptr->parent = dir;
        vfs_idup(dir); // increase parent dir refcount
        vfs_iunlock(ret_ptr);
    }
out:
    vfs_iunlock(dir);
    vfs_superblock_unlock(dir->sb);
    return ret_ptr;
}

int vfs_rmdir(struct vfs_inode *dir, const char *name, size_t name_len) {
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
    if (!S_ISDIR(dir->mode)) {
        ret = -ENOTDIR; // Inode is not a directory
        goto out;
    }
    if (dir->ops->rmdir == NULL) {
        ret = -ENOSYS; // Rmdir operation not supported
        goto out;
    }
    ret = dir->ops->rmdir(dir, name, name_len);
out:
    vfs_iunlock(dir);
    vfs_superblock_unlock(dir->sb);
    if (ret == 0) {
        // Decrease parent dir refcount
        vfs_iput(dir);
    }
    return ret;
}

int vfs_move(struct vfs_inode *old_dir, struct vfs_dentry *old_dentry,
             struct vfs_inode *new_dir, const char *name, size_t name_len) {
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
    if (!S_ISDIR(old_dir->mode)) {
        ret = -ENOTDIR; // Inode is not a directory
        goto out;
    }
    if (!S_ISDIR(new_dir->mode)) {
        ret = -ENOTDIR; // Inode is not a directory
        goto out;
    }
    ret = vfs_ilock_two_directories(old_dir, new_dir);
    if (old_dir->ops->move == NULL || old_dir->ops->move != new_dir->ops->move) {
        ret = -ENOSYS; // Move operation not supported
        goto out_iunlock;
    }
    ret = old_dir->ops->move(old_dir, old_dentry, new_dir, name, name_len);
out_iunlock:
    vfs_iunlock_two(old_dir, new_dir);
out:
    vfs_superblock_unlock(old_dir->sb);
    return ret;
}

struct vfs_inode *vfs_symlink(struct vfs_inode *dir, uint32 mode, 
                              const char *name, size_t name_len,
                              const char *target, size_t target_len) {
    if (dir == NULL || dir->sb == NULL) {
        return ERR_PTR(-EINVAL); // Invalid argument
    }
    if (target == NULL || target_len == 0 || target_len > VFS_PATH_MAX) {
        return ERR_PTR(-EINVAL); // Invalid argument
    }
    if (name == NULL || name_len == 0) {
        return ERR_PTR(-EINVAL); // Invalid symlink name
    }
    vfs_superblock_wlock(dir->sb);
    vfs_ilock(dir);
    long ret = __vfs_inode_valid(dir);
    struct vfs_inode *ret_ptr = NULL;
    if (ret != 0) {
        ret_ptr = ERR_PTR(ret);
        goto out;
    }
    if (!S_ISDIR(dir->mode)) {
        ret_ptr = ERR_PTR(-ENOTDIR); // Inode is not a directory
        goto out;
    }
    if (dir->ops->symlink == NULL) {
        ret_ptr = ERR_PTR(-ENOSYS); // Symlink operation not supported
        goto out;
    }
    ret_ptr = dir->ops->symlink(dir, mode, name, name_len, target, target_len);
out:
    vfs_iunlock(dir);
    vfs_superblock_unlock(dir->sb);
    return ret_ptr;
}

int vfs_truncate(struct vfs_inode *inode, loff_t new_size) {
    if (inode == NULL || inode->sb == NULL) {
        return -EINVAL; // Invalid argument
    }
    vfs_ilock(inode);
    int ret = __vfs_inode_valid(inode);
    if (ret != 0) {
        goto out; // Inode is not valid or caller does not hold the ilock
    }
    if (!S_ISREG(inode->mode)) {
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

// Lock two non-directory inodes to prevent deadlock
void vfs_ilock_two_nondirectories(struct vfs_inode *inode1, struct vfs_inode *inode2) {
    assert(inode1 != NULL && inode2 != NULL, "vfs_ilock_two_nondirectories: inode is NULL");
    if (inode1 < inode2) {
        vfs_ilock(inode1);
        vfs_ilock(inode2);
    } else if (inode1 > inode2) {
        vfs_ilock(inode2);
        vfs_ilock(inode1);
    } else {
        // same inode
        vfs_ilock(inode1);
    }
}

// Lock two directory inodes to prevent deadlock
// Return 0 on success, negative error code on failure
// Caller should hold the superblock read lock of the superblock
// Caller should ensure both inodes are directories
int vfs_ilock_two_directories(struct vfs_inode *inode1, struct vfs_inode *inode2) {
    if (inode1 == inode2) {
        vfs_ilock(inode1);
        return 0;
    }
    if (inode1->sb != inode2->sb) {
        return -EXDEV; // Cross-filesystem locking not supported
    }
    // Borrowed from Linux kernel's lockdep strategy
    struct vfs_inode *p = inode1;
    struct vfs_inode *q = inode2;
    struct vfs_inode *r;
    while ((r = p->parent) != inode2 && r != p) {
        // keep going up until we reach the root or find inode2
        p = r;
    }
    if (r == inode2) {
        // inode2 is the ancestor of inode1
        vfs_ilock(inode2);
        vfs_ilock(inode1);
        return 0;
    }
    while ((r = q->parent) != inode1 && r != q && r != p) {
        // keep going up until we reach the root or find inode1
        q = r;
    }
    if (r == inode1) {
        // inode1 is the ancestor of inode2
        vfs_ilock(inode1);
        vfs_ilock(inode2);
        return 0;
    } else if (r == p) {
        // inode1 and inode2 are in different branches
        if (inode1 < inode2) {
            vfs_ilock(inode1);
            vfs_ilock(inode2);
        } else {
            vfs_ilock(inode2);
            vfs_ilock(inode1);
        }
        return 0;
    }
    
    // Since we are sure both inodes belong to the same filesystem,
    // thry must have a common ancestor(FS root)
    panic("vfs_ilock_two_directories: unexpected condition");
    return -EINVAL;
}

void vfs_iunlock_two(struct vfs_inode *inode1, struct vfs_inode *inode2) {
    if (inode1 != NULL) {
        vfs_iunlock(inode1);
    }
    if (inode2 != NULL && inode2 != inode1) {
        vfs_iunlock(inode2);
    }
}

int vfs_chdir(struct vfs_inode *new_cwd) {
    if (new_cwd == NULL || new_cwd->sb == NULL) {
        return -EINVAL; // Invalid argument
    }
    vfs_superblock_rlock(new_cwd->sb);
    vfs_ilock(new_cwd);
    int ret = __vfs_inode_valid(new_cwd);
    if (ret != 0) {
        goto out;
    }
    if (!S_ISDIR(new_cwd->mode)) {
        ret = -ENOTDIR; // Inode is not a directory
        goto out;
    }
    vfs_idup(new_cwd);
    proc_lock(myproc());
    struct vfs_inode *old_cwd = myproc()->fs.cwd;
    myproc()->fs.cwd = new_cwd;
    proc_unlock(myproc());
    vfs_iunlock(new_cwd);
    vfs_superblock_unlock(new_cwd->sb);
    vfs_iput(old_cwd);
    ret = 0;
out:
    return ret;
}

int vfs_chroot(struct vfs_inode *new_root)  {
    int ret = vfs_chdir(new_root);
    if (ret != 0) {
        return ret;
    }
    vfs_idup(new_root);
    proc_lock(myproc());
    struct vfs_inode *old_root = myproc()->fs.rooti;
    myproc()->fs.rooti = new_root;
    proc_unlock(myproc());
    vfs_iput(old_root);
    return 0;
}

// Get current working directory inode of the current process
// Caller needs to call vfs_iput on the returned inode when done
struct vfs_inode *vfs_curdir(void) {
    // Since only the current process can change its cwd,
    // we don't need to lock the inode here
    struct vfs_inode *cwd = myproc()->fs.cwd;
    vfs_idup(cwd);
    return cwd;
}

// Get current root directory inode of the current process
// Caller needs to call vfs_iput on the returned inode when done
struct vfs_inode *vfs_curroot(void) {
    // Since only the current process can change its root,
    // we don't need to lock the inode here
    struct vfs_inode *rooti = myproc()->fs.rooti;
    vfs_idup(rooti);
    return rooti;
}

struct vfs_inode *vfs_namei(const char *path, size_t path_len) {
    if (path == NULL || path_len == 0) {
        return ERR_PTR(-EINVAL); // Invalid argument
    }
    if (path_len > VFS_PATH_MAX) {
        return ERR_PTR(-ENAMETOOLONG); // Path too long
    }

    struct vfs_inode *pos = NULL;
    struct vfs_inode *rooti = NULL;
    char *pathbuf = NULL;
    int ret = 0;
    struct vfs_inode *ret_inode = NULL;

    pathbuf = kmm_alloc(path_len + 1);
    if (pathbuf == NULL) {
        return ERR_PTR(-ENOMEM);
    }

    // Get current root for ".." at root handling
    rooti = vfs_curroot();
    if (IS_ERR_OR_NULL(rooti)) {
        kmm_free(pathbuf);
        if (rooti == NULL) {
            return ERR_PTR(-EINVAL);
        }
        return rooti;
    }
    if (rooti->mount) {
        if (rooti->mnt_rooti == NULL) {
            vfs_iput(rooti);
            kmm_free(pathbuf);
            return ERR_PTR(-EINVAL); // Mounted root inode has no mounted root
        }
        struct vfs_inode *mnt_root = rooti->mnt_rooti;
        vfs_idup(mnt_root);
        vfs_iput(rooti);
        rooti = mnt_root;
    }

    if (path[0] == '/') {
        // Absolute path, start from root
        pos = rooti;
        vfs_idup(pos);
        path++; // skip leading '/'
        path_len--;
    } else {
        // Relative path, start from cwd
        pos = vfs_curdir();
        if (IS_ERR(pos)) {
            vfs_iput(rooti);
            kmm_free(pathbuf);
            if (pos == NULL) {
                return ERR_PTR(-EINVAL);
            }
            return pos;
        }
    }

    // Make a copy of path since strtok_r modifies the string
    if (path_len > 0) {
        memmove(pathbuf, path, path_len);
    }
    pathbuf[path_len] = '\0';

    char *token, *saveptr;
    struct vfs_dentry dentry = {0};
    struct vfs_inode *next = NULL;

    token = strtok_r(pathbuf, "/", &saveptr);
    while (token != 0) {
        size_t token_len = strlen(token);

        memset(&dentry, 0, sizeof(dentry));
        ret = vfs_ilookup(pos, &dentry, token, token_len);
        if (ret != 0) {
            vfs_iput(pos);
            pos = NULL;
            ret_inode = ERR_PTR(ret);
            goto out;
        }

        next = vfs_get_dentry_inode(&dentry);
        vfs_release_dentry(&dentry);
        if (IS_ERR(next)) {
            vfs_iput(pos);
            pos = NULL;
            ret_inode = next;
            goto out;
        }

        vfs_iput(pos);
        pos = next;

        while (pos->mount && pos->mnt_rooti != NULL) {
            struct vfs_inode *mnt_root = pos->mnt_rooti;
            vfs_idup(mnt_root);
            vfs_iput(pos);
            pos = mnt_root;
        }

        token = strtok_r(0, "/", &saveptr);
    }

    ret_inode = pos;
out:
    vfs_iput(rooti);
    kmm_free(pathbuf);
    if (pos == NULL && !IS_ERR(ret_inode)) {
        return ERR_PTR(-ENOENT);
    }
    return ret_inode;
}
