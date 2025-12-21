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
    if (!S_ISDIR(dir->mode)) {
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
    if (!S_ISLNK(inode->mode)) {
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
    if (!S_ISDIR(dir->mode)) {
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
    if (!S_ISDIR(dir->mode)) {
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
    struct vfs_inode *target = NULL;
    int ret = vfs_get_dentry_inode(old, &target);
    if (ret != 0) {
        vfs_superblock_unlock(dir->sb);
        vfs_iunlock(dir);
        return ret;
    }
    assert(target != NULL, "vfs_link: old dentry inode is NULL");
    ret = __vfs_inode_valid(dir);
    if (ret != 0) {
        goto out;
    }
    ret = __vfs_inode_valid(target);
    if (ret != 0) {
        goto out;
    }
    if (S_ISDIR(target->mode)) {
        ret = -EPERM; // Cannot create hard link to a directory
        goto out;
    }
    if (!S_ISDIR(dir->mode)) {
        ret = -ENOTDIR; // Inode is not a directory
        goto out;
    }
    if (dir->ops->link == NULL) {
        ret = -ENOSYS; // Link operation not supported
        goto out;
    }
    ret = dir->ops->link(target, dir, name, name_len, user);
out:
    vfs_iunlock(target);
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
    if (!S_ISDIR(dir->mode)) {
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

int vfs_mkdir(struct vfs_inode *dir, uint32 mode, struct vfs_inode **ret_dir,
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
    if (!S_ISDIR(dir->mode)) {
        ret = -ENOTDIR; // Inode is not a directory
        goto out;
    }
    if (dir->ops->mkdir == NULL) {
        ret = -ENOSYS; // Mkdir operation not supported
        goto out;
    }
    struct vfs_inode *new_dir = NULL;
    ret = dir->ops->mkdir(dir, mode, &new_dir, name, name_len, user);
    if (ret == 0) {
        assert(new_dir != NULL, "vfs_mkdir: new_dir is NULL on success");
        *ret_dir = new_dir;
        vfs_ilock(new_dir);
        new_dir->parent = dir;
        vfs_idup(dir); // increase parent dir refcount
        vfs_iunlock(new_dir);
    }
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
    if (!S_ISDIR(dir->mode)) {
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
    if (ret == 0) {
        // Decrease parent dir refcount
        vfs_iput(dir);
    }
    return ret;
}

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
    ret = old_dir->ops->move(old_dir, old_dentry, new_dir, name, name_len, user);
out_iunlock:
    vfs_iunlock_two(old_dir, new_dir);
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
    if (!S_ISDIR(dir->mode)) {
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
int vfs_curdir(struct vfs_inode **res_inode) {
    if (res_inode == NULL) {
        return -EINVAL; // Invalid argument
    }
    // Since only the current process can change its cwd,
    // we don't need to lock the inode here
    struct vfs_inode *cwd = myproc()->fs.cwd;
    vfs_idup(cwd);
    *res_inode = cwd;
    return 0;
}

// Get current root directory inode of the current process
// Caller needs to call vfs_iput on the returned inode when done
int vfs_curroot(struct vfs_inode **res_inode) {
    if (res_inode == NULL) {
        return -EINVAL; // Invalid argument
    }
    // Since only the current process can change its root,
    // we don't need to lock the inode here
    struct vfs_inode *rooti = myproc()->fs.rooti;
    vfs_idup(rooti);
    *res_inode = rooti;
    return 0;
}

int vfs_namei(const char *path, size_t path_len, struct vfs_inode **res_inode) {
    if (path == NULL || path_len == 0 || res_inode == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (path_len > VFS_PATH_MAX) {
        return -ENAMETOOLONG; // Path too long
    }
    struct vfs_inode *pos = NULL;
    struct vfs_inode *rooti = NULL;
    char *pathbuf = NULL;
    int ret = 0;

    // Allocate buffer for path copy since strtok_r modifies the string
    pathbuf = kmm_alloc(path_len + 1);
    if (pathbuf == NULL) {
        return -ENOMEM;
    }

    // Get current root for ".." at root handling
    ret = vfs_curroot(&rooti);
    if (ret != 0) {
        kmm_free(pathbuf);
        return ret;
    }
    if (rooti->mount) {
        if (rooti->mnt_rooti == NULL) {
            vfs_iput(rooti);
            kmm_free(pathbuf);
            return -EINVAL; // Mounted root inode has no mounted root
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
        ret = vfs_curdir(&pos);
        if (ret != 0) {
            vfs_iput(rooti);
            kmm_free(pathbuf);
            return ret;
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

        // Handle "." - stay at current position
        if (token_len == 1 && token[0] == '.') {
            token = strtok_r(0, "/", &saveptr);
            continue;
        }

        // Handle ".."
        if (token_len == 2 && token[0] == '.' && token[1] == '.') {
            // If at process root, don't move
            if (pos == rooti) {
                token = strtok_r(0, "/", &saveptr);
                continue;
            }

            // If at mounted root, go to mountpoint first, then lookup ".."
            if (pos->sb != NULL && pos->sb->mountpoint != NULL && 
                vfs_inode_is_local_root(pos)) {
                struct vfs_inode *mountpoint = pos->sb->mountpoint;
                vfs_idup(mountpoint);
                vfs_iput(pos);
                pos = mountpoint;
                // Now lookup ".." at the mountpoint
            }
        }

        // Lookup the token in the current directory
        memset(&dentry, 0, sizeof(dentry));
        ret = vfs_ilookup(pos, &dentry, token, token_len, false);
        if (ret != 0) {
            vfs_iput(pos);
            vfs_iput(rooti);
            kmm_free(pathbuf);
            *res_inode = NULL;
            return ret;
        }

        // Get the inode from dentry
        ret = vfs_get_dentry_inode(&dentry, &next);
        vfs_release_dentry(&dentry);
        if (ret != 0) {
            vfs_iput(pos);
            vfs_iput(rooti);
            kmm_free(pathbuf);
            *res_inode = NULL;
            return ret;
        }
        // vfs_get_dentry_inode returns a locked inode, unlock it
        vfs_idup(next); // increase refcount for next position
        vfs_iunlock(next);

        // Release old position, move to next
        vfs_iput(pos);
        pos = next;

        // If the new position is a mountpoint, switch to mounted root
        while (pos->mount && pos->mnt_rooti != NULL) {
            struct vfs_inode *mnt_root = pos->mnt_rooti;
            vfs_idup(mnt_root);
            vfs_iput(pos);
            pos = mnt_root;
        }

        token = strtok_r(0, "/", &saveptr);
    }

    vfs_iput(rooti);
    kmm_free(pathbuf);
    *res_inode = pos;
    return 0;
}
