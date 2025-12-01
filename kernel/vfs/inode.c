#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "bits.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "mutex_types.h"
#include "rwlock.h"
#include "vfs/fs.h"
#include "vfs_private.h"
#include "list.h"
#include "hlist.h"
#include "slab.h"
#include "errno.h"

int vfs_ilock(struct vfs_inode *inode) {
    if (inode == NULL) {
        return -EINVAL; // Invalid argument
    }
    assert(myproc() != NULL, "vfs_ilock: current process is NULL");
retry:
    __vfs_i_spin_lock(inode);
    int ret = 0;
    if (!inode->valid) {
        __vfs_i_spin_unlock(inode);
        return -EINVAL; // Inode is not valid
    }
    if (inode->locked) {
        __vfs_i_spin_unlock(inode);
        // Already locked, wait and retry
        __vfs_i_wait_completion(inode);
        goto retry;
    }
    assert(inode->owner == NULL, "vfs_ilock: inode lock owner is not NULL when locked");
    
    // Temporarily claim the lock to prevent others from acquiring it.
    // May release if filesystem specific ilock callback exists but fails.
    inode->locked = 1;
    inode->owner = myproc();
    if (inode->ops->ilock != NULL) {
        __vfs_i_spin_unlock(inode);
        ret = inode->ops->ilock(inode);
        __vfs_i_spin_lock(inode);
        if (ret == 0) {
            __vfs_i_reinit_completion(inode);
        } else {
            // Failed to acquire filesystem specific lock, release the lock
            inode->locked = 0;
            inode->owner = NULL;
            __vfs_i_complete(inode);
        }
    }
    __vfs_i_spin_unlock(inode);
    return ret;
}

void vfs_iunlock(struct vfs_inode *inode) {
    if (inode == NULL) {
        return; // Invalid argument
    }
    assert(myproc() != NULL, "vfs_iunlock: current process is NULL");
    __vfs_i_spin_lock(inode);
    int ret = 0;
    if (inode->owner != myproc()) {
        __vfs_i_spin_unlock(inode);
        printf("warning: vfs_iunlock: current process does not own the inode lock");
        return;
    }

    // Knowing the current process owns the lock, it's safe to unlock the inode
    // and call filesystem specific unlock callback
    __vfs_i_spin_unlock(inode);
    if (inode->ops->iunlock != NULL) {
        inode->ops->iunlock(inode);
    }

    // reset lock owner and mark as unlocked at the end
    __vfs_i_spin_lock(inode);
    inode->locked = 0;
    inode->owner = NULL;
    __vfs_i_complete(inode);
    __vfs_i_spin_unlock(inode);
}

int vfs_idup(struct vfs_inode *inode) {
    if (inode == NULL) {
        return -EINVAL; // Invalid argument
    }
    __vfs_i_spin_lock(inode);
    kobject_get(&inode->kobj);
    __vfs_i_spin_unlock(inode);
    return 0;
}

void vfs_iput(struct vfs_inode *inode) {
    if (inode == NULL) {
        return;
    }
retry:
    __vfs_i_spin_lock(inode);
    int64 refcount = kobject_refcount(&inode->kobj);
    assert(refcount > 0, "vfs_iput: inode refcount underflow");
    if (refcount == 1) {
        // Last reference, need to sync the inode, detach it from superblock, and free it
        __vfs_i_spin_unlock(inode);
        superblock_wlock(inode->sb);
        __vfs_i_spin_lock(inode);
        // Double check refcount after acquiring superblock lock
        if (kobject_refcount(&inode->kobj) > 1) {
            // If other references acquired meanwhile, just release locks and return
            superblock_unlock(inode->sb);
            kobject_put(&inode->kobj);
            __vfs_i_spin_unlock(inode);
            return;
        }
        if (inode->dirty && inode->valid) {
            __vfs_i_spin_unlock(inode);
            superblock_unlock(inode->sb);
            int sync_ret = vfs_sync_inode(inode);
            if (sync_ret != 0) {
                printf("warning: vfs_iput: failed to sync inode %lu before deletion: %d\n",
                       inode->ino, sync_ret);
            }
            goto retry;
        }

        int remove_ret = vfs_remove_inode(inode->sb, inode);
        assert(remove_ret == 0, "vfs_iput: failed to remove inode from superblock inode cache");
        __vfs_i_spin_unlock(inode);
        superblock_unlock(inode->sb);
        inode->ops->free_inode(inode);
        return;
    }
    // Decrease ref count
    kobject_put(&inode->kobj);
    __vfs_i_spin_unlock(inode);
}

// Mark inode as dirty
// Caller needs to hold the ilock of the inode
// Caller should not hold the spinlock of the inode
int vfs_dirty_inode(struct vfs_inode *inode) {
    if (inode == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (!VFS_INODE_HOLDING(inode)) {
        return -EPERM; // Caller does not hold the inode lock
    }
    int ret = 0;
    bool prev_dirty = 0;
    __vfs_i_spin_lock(inode);
    if (!__vfs_sb_valid(inode->sb)) {
        __vfs_i_spin_unlock(inode);
        return -EINVAL; // Superblock is not valid
    }
    // Since caller holds the ilock, the valid flag should be set
    assert(inode->valid, "vfs_dirty_inode: inode is not valid");
    prev_dirty = inode->dirty;
    inode->dirty = 1;
    __vfs_i_spin_unlock(inode);

    if (prev_dirty == 0 && inode->ops->dirty_inode != NULL) {
        ret = inode->ops->dirty_inode(inode);
    }
    if (ret != 0) {
        // On failure, revert dirty flag
        __vfs_i_spin_lock(inode);
        inode->dirty = prev_dirty;
        __vfs_i_spin_unlock(inode);
    }
    return ret;
}

// Sync inode to disk
// Caller should hold the ilock of the inode
// Caller should not hold the spinlock of the inode
int vfs_sync_inode(struct vfs_inode *inode) {
    if (inode == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (!VFS_INODE_HOLDING(inode)) {
        return -EPERM; // Caller does not hold the inode lock
    }
    int ret = 0;
    bool was_dirty = 0;
    __vfs_i_spin_lock(inode);
    // Synching a inode on an invalid superblock is allowed, as the `valid` flag
    // of the superblock only prevents new operations from starting.
    
    // Since caller holds the ilock, the valid flag should be set
    assert(inode->valid, "vfs_dirty_inode: inode is not valid");
    was_dirty = inode->dirty;
    __vfs_i_spin_unlock(inode);

    if (was_dirty && inode->ops->sync_inode != NULL) {
        ret = inode->ops->sync_inode(inode);
    }
    if (ret == 0) {
        // On success, clear dirty flag
        __vfs_i_spin_lock(inode);
        inode->dirty = 0;
        __vfs_i_spin_unlock(inode);
    }
    return ret;
}
