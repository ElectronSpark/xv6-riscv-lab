/*
 * VFS inode operations
 *
 * Locking order (must acquire in this order to avoid deadlock):
 * 1. mount mutex (via vfs_mount_lock)
 * 2. vfs_superblock rwlock (via vfs_superblock_rlock/wlock)
 * 3. vfs_inode mutex (via vfs_ilock)
 * 4. buffer mutex (via bread/brelse)
 * 5. log spinlock (filesystem internal, e.g., xv6fs log->lock)
 *
 * CRITICAL: Operations that hold superblock wlock + inode lock must NOT
 * call filesystem operations that can sleep waiting for log space or I/O,
 * as this can cause priority inversion with file I/O paths that only
 * hold inode lock.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
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
    list_entry_init(&inode->orphan_entry);
    inode->orphan = 0;
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
    assert(inode->sb == NULL || !vfs_superblock_wholding(inode->sb),
           "vfs_iput: cannot hold superblock read lock when calling");
    assert(!holding_mutex(&inode->mutex), "vfs_iput: cannot hold inode lock when calling");

    // tried to cleanup the inode but failed
    bool failed_clean = false;
    bool should_free_sb = false;
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

    // Retry decreasing refcount again, as it may have changed meanwhile
    if (atomic_dec_unless(&inode->ref_count, 1)) {
        // Someone else grabbed a reference while we were acquiring locks,
        // and we just decremented it. The inode is still in use, don't free.
        vfs_iunlock(inode);
        vfs_superblock_unlock(sb);
        return;
    }

    // For backendless filesystems (e.g., tmpfs), keep inodes alive as long as
    // they have positive link count AND the superblock is still attached.
    // Mountpoint inodes keep an extra reference from the mount.
    // When detached, we must clean up all inodes regardless of n_links.
    if (sb->backendless && sb->attached && (inode->n_links > 0 || inode->mount)) {
        // Decrement refcount to 0 but keep inode in cache
        atomic_dec(&inode->ref_count);
        assert(inode->ref_count >= 0, "vfs_iput: backendless inode refcount underflow");
        vfs_iunlock(inode);
        vfs_superblock_unlock(sb);
        return;
    }

    assert(!inode->mount, "vfs_iput: refcount of mountpoint inode reached zero");

    // Handle orphan cleanup: remove from orphan list
    if (inode->orphan) {
        list_node_detach(inode, orphan_entry);
        sb->orphan_count--;
        inode->orphan = 0;
        
        // For backend fs: remove from on-disk orphan journal
        if (sb->ops->remove_orphan) {
            ret = sb->ops->remove_orphan(sb, inode);
            if (ret != 0) {
                printf("vfs_iput: warning: failed to remove orphan inode %lu from journal\n", 
                       inode->ino);
            }
        }
    }

    // If no one increased its refcount meanwhile, we can delete it
    // First check if it is dirty and sync if needed
    // If sync failed, just delete it.
    if (inode->dirty && inode->valid && !failed_clean && sb->attached) {
        vfs_iunlock(inode);
        vfs_superblock_unlock(inode->sb);
        failed_clean = vfs_sync_inode(inode) != 0;
        // Someone else may have acquired the inode meanwhile, so retry
        goto retry;
    }

    if (S_ISDIR(inode->mode) && inode->parent != inode && sb->attached) {
        // For non-root directory inode, decrease parent dir refcount
        // Root directory's parent is itself
        // Skip parent handling for detached fs (parent may already be freed)
        parent = inode->parent;
    }

    // If inode has no links left (or fs is detached), destroy its data before freeing
    if ((inode->n_links == 0 || !sb->attached) && inode->ops->destroy_inode != NULL) {
        // Mark inode as being destroyed so other threads looking up this inode
        // number will not try to use it while destroy_inode is in progress.
        // The inode stays in the cache until destroy_inode completes.
        inode->destroying = 1;
        
        // Release superblock lock before calling destroy_inode, which may sleep
        // (e.g., xv6fs_begin_op can sleep waiting for log space).
        // Keep the inode lock to ensure exclusive access during destruction.
        vfs_superblock_unlock(sb);
        
        inode->ops->destroy_inode(inode);
        
        // Re-acquire superblock lock to remove inode from cache
        vfs_superblock_wlock(sb);
        
        // After destroy, the inode's on-disk data is freed.
        // Mark it invalid and not dirty so we don't try to sync it.
        inode->valid = 0;
        inode->dirty = 0;
        inode->destroying = 0;
    }

    ret = vfs_remove_inode(inode->sb, inode);
    assert(ret == 0, "vfs_iput: failed to remove inode from superblock inode cache");
    
    // Check if this was the last orphan on a detached fs
    should_free_sb = (!sb->attached && sb->orphan_count == 0);
    
    vfs_iunlock(inode);
    vfs_superblock_unlock(sb);
    assert(completion_done(&inode->completion),
           "vfs_iput: someone is waiting on inode completion without reference");
out:
    inode->ops->free_inode(inode);

    // Final superblock cleanup if all orphans are gone on detached fs
    if (should_free_sb) {
        __vfs_final_unmount_cleanup(sb);
    }

    // If this is a directory inode, decrease the refcount of its parent
    if (parent != NULL) {
        // Due to the limited kernel space stack, we avoid recursive calls here
        inode = parent;
        parent = NULL;
        failed_clean = false;
        should_free_sb = false;
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

// Get the outmost layer of mount point
// Caller should hold the reference of rooti or its descendants
// Because VFS will always cache the ancestors of cached directories,
// no need to worry about the mountpoint inode being freed here
static struct vfs_inode *__get_mnt_recursive(struct vfs_inode *rooti) {
    struct vfs_inode *inode = rooti;
    struct vfs_superblock *sb = rooti->sb;
    struct vfs_inode *proc_rooti = vfs_inode_deref(&myproc()->fs->rooti);
    while (true) {
        if (inode == proc_rooti) {
            // Reached process root
            return inode;
        }
        assert(sb != NULL, "__get_mnt_recursive: inode's superblock mismatch");
        if (inode != sb->root_inode) {
            assert(sb->root_inode != NULL,
                   "__get_mnt_recursive: superblock root inode is NULL");
            return inode; // Reached the outmost mountpoint
        }
        // Otherwise, go up one level
        inode = sb->mountpoint;
        assert(inode != NULL, "__get_mnt_recursive: mountpoint inode is NULL");
        sb = inode->sb;
    }
}

// Get the parent inode of the mountpoint recursively
// Caller should hold the reference of dir or its descendants
// Because VFS will always cache the ancestors of cached directories,
// no need to worry about the mountpoint inode being freed here
static struct vfs_inode *__mountpoint_go_up(struct vfs_inode *dir) {
    struct vfs_inode *inode = dir;
    struct vfs_inode *proc_rooti = vfs_inode_deref(&myproc()->fs->rooti);
    while (true) {
        if (inode == proc_rooti) {
            // Reached process root
            return inode;
        }
        if (inode->parent != inode) {
            assert(inode->parent != NULL,
                   "__mountpoint_go_up: inode's parent is NULL");
            return inode->parent; // Found the parent inode
        }
        // Otherwise, go up one level
        inode = __get_mnt_recursive(inode);
    }
}

// Resolve ".." for a directory inode.
// Returns the target inode for ".." traversal:
// - If dir is the process root, returns dir (can't go higher)
// - If dir is a local filesystem root, returns the parent across mount boundary
// - Otherwise returns NULL (caller should use driver lookup for normal "..")
static struct vfs_inode *__vfs_dotdot_target(struct vfs_inode *dir) {
    struct vfs_inode *proc_rooti = vfs_inode_deref(&myproc()->fs->rooti);
    if (dir == proc_rooti) {
        return dir;
    }
    if (vfs_inode_is_local_root(dir)) {
        return __mountpoint_go_up(dir);
    }
    return NULL;
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
        dentry->name = strndup(".", 1);
        if (dentry->name == NULL) {
            return -ENOMEM;
        }
        dentry->name_len = 1;
        dentry->cookies = 0; // cookie values are filesystem-private; opaque to VFS
        return 0;
    }

    if (name_len == 2 && name[0] == '.' && name[1] == '.') {
        struct vfs_inode *target = __vfs_dotdot_target(dir);
        if (target != NULL) {
            dentry->sb = target->sb;
            dentry->ino = target->ino;
            dentry->parent = (target == dir) ? NULL : target;
            dentry->name = strndup("..", 2);
            if (dentry->name == NULL) {
                return -ENOMEM;
            }
            dentry->name_len = 2;
            dentry->cookies = 0;
            return 0;
        }
        // Otherwise, fall through to driver lookup for normal ".."
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

static int __make_iter_present(struct vfs_dir_iter *iter,
                               struct vfs_dentry *ret_dentry) {
    ret_dentry->name = strndup(".", 1);
    if (ret_dentry->name == NULL) {
        return -ENOMEM;
    }
    ret_dentry->name_len = 1;
    ret_dentry->cookies = 0;
    iter->cookies = 0;
    iter->index = 1;
    return 0;
}

static int __make_iter_parent(struct vfs_dir_iter *iter,
                              struct vfs_dentry *ret_dentry) {
    vfs_release_dentry(ret_dentry); // release "."
    ret_dentry->name = strndup("..", 2);
    if (ret_dentry->name == NULL) {
        return -ENOMEM;
    }
    ret_dentry->name_len = 2;
    ret_dentry->cookies = 0;
    iter->cookies = 0;
    iter->index = 2;
    return 0;
}

// Iterate over directory entries in a directory inode
// Drivers should look at iter->cookies and update new cookies in ret_dentry->cookies
// Drivers should release the content of ret_dentry before writing new content
// Drivers only need to fill:
// - ret_dentry->name
// - ret_dentry->name_len
// - ret_dentry->ino
// - ret_dentry->cookies
// VFS will fill ret_dentry->sb and ret_dentry->parent as needed
// When Reaching end of directory, drivers should set ret_dentry->name to NULL
// Drivers don't need to update iter, it will be updated by VFS after successful return
// When drivers see iter->index == 2, they should not return the name
int vfs_dir_iter(struct vfs_inode *dir, struct vfs_dir_iter *iter,
                 struct vfs_dentry *ret_dentry) {
    if (dir == NULL || dir->sb == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (iter == NULL || ret_dentry == NULL) {
        return -EINVAL; // Invalid argument
    }

    vfs_superblock_rlock(dir->sb);
    vfs_ilock(dir);

    int ret = __vfs_inode_valid(dir);
    bool need_lookup = false; // Need to lookup across file system boundary
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

    // Check if iteration already completed (index == -1 means end of directory)
    if (iter->index == -1) {
        ret_dentry->name = NULL;
        ret_dentry->name_len = 0;
        ret = 0;
        goto out;
    }

    // Synthesize "." on the first iteration to keep cookies opaque at the VFS layer
    if (iter->index == 0) {
        ret = __make_iter_present(iter, ret_dentry);
        if (ret != 0) {
            goto out;
        }
        ret_dentry->ino = dir->ino;
        ret_dentry->sb = dir->sb;
        ret_dentry->parent = NULL;
        ret = 0;
        goto out;
    }

    // For process root or a mounted root, synthesize ".." on the second iteration
    if (iter->index == 1) {
        struct vfs_inode *proc_rooti = vfs_inode_deref(&myproc()->fs->rooti);
        if (dir == proc_rooti) {
            // Process root: ".." points to self
            ret = __make_iter_parent(iter, ret_dentry);
            if (ret != 0) {
                goto out;
            }
            ret_dentry->ino = dir->ino;
            ret_dentry->sb = dir->sb;
            ret_dentry->parent = NULL;
            ret = 0;
            goto out;
        } else if (vfs_inode_is_local_root(dir)) {
            // Mounted root: ".." crosses mount boundary, fill in after unlock
            ret = __make_iter_parent(iter, ret_dentry);
            if (ret != 0) {
                goto out;
            }
            ret_dentry->parent = NULL;
            need_lookup = true;
            ret = 0;
            goto out;
        }
        // Ordinary directory: let driver return ".." with correct parent ino
        // Fall through to driver call without modifying iter->index
    }
    
    if (iter->index > 1) {
        iter->index++;
        ret_dentry->sb = dir->sb;
        ret_dentry->parent = dir;
    }
    ret = dir->ops->dir_iter(dir, iter, ret_dentry);
    if (ret == 0 && iter->index == 1) {
        // Driver returned ".." successfully, advance index
        iter->index = 2;
    }

out:
    vfs_iunlock(dir);
    vfs_superblock_unlock(dir->sb);

    if (ret == 0) {
        if (iter->index == 2 && need_lookup) {
            // when synthesizing ".." for a mounted root, fill in the correct parent inode now
            struct vfs_inode *target = __vfs_dotdot_target(dir);
            ret_dentry->ino = target->ino;
            ret_dentry->sb = target->sb;
            ret_dentry->parent = target;
        }
        if (iter->index > 2) {
            if (ret_dentry->name != NULL) {
                // Normal entry returned
                iter->cookies = ret_dentry->cookies;
                return 0;
            }
            // Otherwise, reached end of directory; reset iterator
            iter->index = -1;
            iter->cookies = 0;
            ret_dentry->parent = NULL;
            ret_dentry->cookies = 0;
            ret_dentry->ino = 0;
            ret_dentry->sb = NULL;
            return 0;
        }
    }

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
    
    // Begin transaction BEFORE acquiring any locks
    if (dir->sb->ops->begin_transaction != NULL) {
        int ret = dir->sb->ops->begin_transaction(dir->sb);
        if (ret != 0) {
            return ERR_PTR(ret);
        }
    }
    
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
    
    // End transaction AFTER releasing locks
    if (dir->sb->ops->end_transaction != NULL) {
        int end_ret = dir->sb->ops->end_transaction(dir->sb);
        if (end_ret != 0) {
            printf("vfs_create: warning: end_transaction failed with error %d\n", end_ret);
        }
    }
    
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
    
    // Begin transaction BEFORE acquiring any locks
    if (dir->sb->ops->begin_transaction != NULL) {
        int ret = dir->sb->ops->begin_transaction(dir->sb);
        if (ret != 0) {
            return ERR_PTR(ret);
        }
    }
    
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
    
    // End transaction AFTER releasing locks
    if (dir->sb->ops->end_transaction != NULL) {
        int end_ret = dir->sb->ops->end_transaction(dir->sb);
        if (end_ret != 0) {
            printf("vfs_mknod: warning: end_transaction failed with error %d\n", end_ret);
        }
    }
    
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
    
    // Begin transaction BEFORE acquiring any locks
    if (dir->sb->ops->begin_transaction != NULL) {
        ret = dir->sb->ops->begin_transaction(dir->sb);
        if (ret != 0) {
            vfs_iput(target);
            return ret;
        }
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
    
    // End transaction AFTER releasing locks
    if (dir->sb->ops->end_transaction != NULL) {
        int end_ret = dir->sb->ops->end_transaction(dir->sb);
        if (end_ret != 0) {
            printf("vfs_link: warning: end_transaction failed with error %d\n", end_ret);
        }
    }
    
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
    int ret = 0;
    struct vfs_inode *ret_ptr = NULL;
    
    // Begin transaction BEFORE acquiring any locks
    if (dir->sb->ops->begin_transaction != NULL) {
        ret = dir->sb->ops->begin_transaction(dir->sb);
        if (ret != 0) {
            return ret;
        }
    }
    
    vfs_superblock_wlock(dir->sb);
    vfs_ilock(dir);
    ret = __vfs_inode_valid(dir);
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
    ret_ptr = dir->ops->unlink(dir, name, name_len);
    
    // If unlink succeeded and the inode still has references beyond ours,
    // mark it as orphan. This is checked while we still hold the locks.
    if (!IS_ERR_OR_NULL(ret_ptr) && ret_ptr->n_links == 0 && 
        ret_ptr->ref_count > 1 && !ret_ptr->orphan) {
        vfs_ilock(ret_ptr);
        vfs_make_orphan(ret_ptr);
        vfs_iunlock(ret_ptr);
    }
out:
    vfs_iunlock(dir);
    vfs_superblock_unlock(dir->sb);
    
    // End transaction AFTER releasing locks
    if (dir->sb->ops->end_transaction != NULL) {
        int end_ret = dir->sb->ops->end_transaction(dir->sb);
        if (end_ret != 0) {
            printf("vfs_unlink: warning: end_transaction failed with error %d\n", end_ret);
        }
    }
    
    if (ret != 0) {
        return ret;
    }
    if (IS_ERR(ret_ptr)) {
        return PTR_ERR(ret_ptr);
    }
    if (ret_ptr != NULL) {
        // Decrease the unlinked inode refcount
        vfs_iput(ret_ptr);
    }
    return 0;
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
    
    // Begin transaction BEFORE acquiring any locks
    if (dir->sb->ops->begin_transaction != NULL) {
        int ret = dir->sb->ops->begin_transaction(dir->sb);
        if (ret != 0) {
            return ERR_PTR(ret);
        }
    }
    
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
    
    // End transaction AFTER releasing locks
    if (dir->sb->ops->end_transaction != NULL) {
        int end_ret = dir->sb->ops->end_transaction(dir->sb);
        if (end_ret != 0) {
            printf("vfs_mkdir: warning: end_transaction failed with error %d\n", end_ret);
        }
    }
    
    return ret_ptr;
}

int vfs_rmdir(struct vfs_inode *dir, const char *name, size_t name_len) {
    if (dir == NULL || dir->sb == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (name == NULL || name_len == 0) {
        return -EINVAL; // Invalid argument
    }
    int ret = 0;
    struct vfs_inode *ret_ptr = NULL;
    
    // Begin transaction BEFORE acquiring any locks
    if (dir->sb->ops->begin_transaction != NULL) {
        ret = dir->sb->ops->begin_transaction(dir->sb);
        if (ret != 0) {
            return ret;
        }
    }
    
    vfs_superblock_wlock(dir->sb);
    vfs_ilock(dir);
    ret = __vfs_inode_valid(dir);
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
    ret_ptr = dir->ops->rmdir(dir, name, name_len);
    
    // If rmdir succeeded and the inode still has references beyond ours,
    // mark it as orphan. This is checked while we still hold the locks.
    if (!IS_ERR_OR_NULL(ret_ptr) && ret_ptr->n_links == 0 && 
        ret_ptr->ref_count > 1 && !ret_ptr->orphan) {
        vfs_ilock(ret_ptr);
        vfs_make_orphan(ret_ptr);
        vfs_iunlock(ret_ptr);
    }
out:
    vfs_iunlock(dir);
    vfs_superblock_unlock(dir->sb);
    
    // End transaction AFTER releasing locks
    if (dir->sb->ops->end_transaction != NULL) {
        int end_ret = dir->sb->ops->end_transaction(dir->sb);
        if (end_ret != 0) {
            printf("vfs_rmdir: warning: end_transaction failed with error %d\n", end_ret);
        }
    }
    
    if (ret != 0) {
        return ret;
    }
    if (IS_ERR(ret_ptr)) {
        return PTR_ERR(ret_ptr);
    }
    if (ret_ptr != NULL) {
        // Decrease the unlinked inode refcount
        vfs_iput(ret_ptr);
    }
    return 0;
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
    
    // Begin transaction BEFORE acquiring any locks
    if (dir->sb->ops->begin_transaction != NULL) {
        int ret = dir->sb->ops->begin_transaction(dir->sb);
        if (ret != 0) {
            return ERR_PTR(ret);
        }
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
    if (!IS_ERR_OR_NULL(ret_ptr) && ret_ptr->parent == NULL) {
        ret_ptr->parent = dir;
        vfs_idup(dir); // increase parent dir refcount
    }
out:
    vfs_iunlock(dir);
    vfs_superblock_unlock(dir->sb);
    
    // End transaction AFTER releasing locks
    if (dir->sb->ops->end_transaction != NULL) {
        int end_ret = dir->sb->ops->end_transaction(dir->sb);
        if (end_ret != 0) {
            printf("vfs_symlink: warning: end_transaction failed with error %d\n", end_ret);
        }
    }
    
    return ret_ptr;
}

int vfs_itruncate(struct vfs_inode *inode, loff_t new_size) {
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
    if (new_cwd == &vfs_root_inode) {
        // not allow to change to the dummy rooti
        return -EINVAL;
    }
    if (new_cwd == vfs_inode_deref(&myproc()->fs->cwd)) {
        // No change
        return 0;
    }
    vfs_superblock_rlock(new_cwd->sb);
    vfs_ilock(new_cwd);
    int ret = __vfs_inode_valid(new_cwd);
    if (ret != 0) {
        goto out_locked;
    }
    if (!S_ISDIR(new_cwd->mode)) {
        ret = -ENOTDIR; // Inode is not a directory
        goto out_locked;
    }
    struct vfs_inode_ref ref ={ 0 };
    struct vfs_inode_ref old = { 0 };
    ret = vfs_inode_get_ref(new_cwd, &ref);
    if (ret != 0) {
        goto out_locked;
    }
    vfs_iunlock(new_cwd);
    vfs_superblock_unlock(new_cwd->sb);

    // to keep it simple, we don't lock fs struct here
    vfs_struct_lock(myproc()->fs);
    old = myproc()->fs->cwd;
    myproc()->fs->cwd = ref;
    vfs_struct_unlock(myproc()->fs);
    ret = 0;
    goto out;

out_locked:
    vfs_iunlock(new_cwd);
    vfs_superblock_unlock(new_cwd->sb);
out:
    vfs_inode_put_ref(&old);
    return ret;
}

int vfs_chroot(struct vfs_inode *new_root)  {
    int ret = vfs_chdir(new_root);
    if (new_root == &vfs_root_inode) {
        // not allow to change to the dummy rooti
        return -EINVAL;
    }
    if (new_root == vfs_inode_deref(&myproc()->fs->rooti)) {
        // No change
        return 0;
    }
    struct vfs_inode_ref ref ={ 0 };
    struct vfs_inode_ref old = { 0 };
    ret = vfs_inode_get_ref(new_root, &ref);
    if (ret != 0) {
        return ret;
    }
    vfs_idup(new_root);
    vfs_struct_lock(myproc()->fs);
    old = myproc()->fs->rooti;
    myproc()->fs->rooti = ref;
    vfs_struct_unlock(myproc()->fs);
    vfs_inode_put_ref(&old);
    return 0;
}

// Get current working directory inode of the current process
// Caller needs to call vfs_iput on the returned inode when done
struct vfs_inode *vfs_curdir(void) {
    // Since only the current process can change its cwd,
    // we don't need to lock the inode here
    struct vfs_inode *cwd = vfs_inode_deref(&myproc()->fs->cwd);
    assert(cwd != NULL, "vfs_curdir: current working directory inode is NULL");
    vfs_idup(cwd);
    return cwd;
}

// Get current root directory inode of the current process
// Caller needs to call vfs_iput on the returned inode when done
struct vfs_inode *vfs_curroot(void) {
    // Since only the current process can change its root,
    // we don't need to lock the inode here
    struct vfs_inode *rooti = vfs_inode_deref(&myproc()->fs->rooti);
    assert(rooti != NULL, "vfs_curroot: current root directory inode is NULL");
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

// vfs_nameiparent resolves the parent directory of a path and copies the final
// name component into the provided buffer. Returns the parent directory inode
// with a reference held on success, or ERR_PTR on failure.
struct vfs_inode *vfs_nameiparent(const char *path, size_t path_len, 
                                   char *name, size_t name_size) {
    if (path == NULL || path_len == 0 || name == NULL || name_size == 0) {
        return ERR_PTR(-EINVAL);
    }
    if (path_len > VFS_PATH_MAX) {
        return ERR_PTR(-ENAMETOOLONG);
    }

    // Find the last path component
    size_t end = path_len;
    
    // Skip trailing slashes
    while (end > 0 && path[end - 1] == '/') {
        end--;
    }
    
    if (end == 0) {
        // Path is just "/" or empty after trimming
        return ERR_PTR(-EINVAL);
    }
    
    // Find the start of the last component
    size_t name_start = end;
    while (name_start > 0 && path[name_start - 1] != '/') {
        name_start--;
    }
    
    // Extract the name component
    size_t final_name_len = end - name_start;
    if (final_name_len >= name_size) {
        return ERR_PTR(-ENAMETOOLONG);
    }
    
    memmove(name, path + name_start, final_name_len);
    name[final_name_len] = '\0';
    
    // Now get the parent path
    size_t parent_len = name_start;
    
    // Skip trailing slashes from parent path
    while (parent_len > 0 && path[parent_len - 1] == '/') {
        parent_len--;
    }
    
    if (parent_len == 0) {
        // Parent is root
        if (path[0] == '/') {
            return vfs_curroot();
        } else {
            // Relative path with just one component, parent is cwd
            return vfs_curdir();
        }
    }
    
    // Resolve the parent path
    return vfs_namei(path, parent_len);
}
