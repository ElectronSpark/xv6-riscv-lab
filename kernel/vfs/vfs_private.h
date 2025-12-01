#ifndef KERNEL_VIRTUAL_FILE_SYSTEM_PRIVATE_H
#define KERNEL_VIRTUAL_FILE_SYSTEM_PRIVATE_H

#include "vfs/vfs_types.h"


int vfs_get_inode_cached(struct vfs_superblock *sb, uint64 ino,
                         struct vfs_inode **ret_inode);
int vfs_add_inode(struct vfs_superblock *sb,
                  struct vfs_inode *inode,
                  struct vfs_inode **ret_inode);
int vfs_remove_inode(struct vfs_superblock *sb, struct vfs_inode *inode);

// Assert holding the spinlock of the inode
#define VFS_INODE_ASSERT_SPIN_HOLDING(__inode, __fmt, ...) do {                  \
    assert((__inode) != NULL, "VFS_INODE_ASSERT_SPIN_HOLDING: inode is NULL");   \
    assert(spin_holding(&(__inode)->spinlock), __fmt, ##__VA_ARGS__);       \
} while (0)

#define VFS_INODE_HOLDING(__inode)      \
    ((__inode) && myproc() == (__inode)->owner)

// Assert holding ilock of the inode
#define VFS_INODE_ASSERT_HOLDING(__inode, __fmt, ...) do {                  \
    assert(VFS_INODE_HOLDING(__inode), __fmt, ##__VA_ARGS__);               \
} while (0)

#define VFS_SUPERBLOCK_ASSERT_WHOLDING(__sb, __fmt, ...) do {                  \
    assert((__sb) != NULL, "VFS_SUPERBLOCK_ASSERT_HOLDING: sb is NULL");   \
    assert(rwlock_is_write_holding(&(__sb)->lock), __fmt, ##__VA_ARGS__);  \
} while (0) 


static inline void __vfs_i_spin_lock(struct vfs_inode *inode) {
    spin_acquire(&inode->spinlock);
}

static inline void __vfs_i_spin_unlock(struct vfs_inode *inode) {
    spin_release(&inode->spinlock);
}

static inline void __vfs_i_wait_completion(struct vfs_inode *inode) {
    wait_for_completion(&inode->completion);
}

static inline void __vfs_i_complete(struct vfs_inode *inode) {
    complete(&inode->completion);
}

static inline void __vfs_i_reinit_completion(struct vfs_inode *inode) {
    completion_reinit(&inode->completion);
}

static inline int __vfs_idup_no_lock(struct vfs_inode *inode) {
    if (inode == NULL) {
        return -EINVAL; // Invalid argument
    }
    kobject_get(&inode->kobj);
    return 0;
}

// Many operations will check valid field of the superblock before proceeding.
// But they should not assume the superblock remains valid during the operation,
// thus invalidate a superblock will only prevent new operations from starting.
// Existing operations should complete before the superblock is fully unmounted.
static inline bool __vfs_sb_valid(struct vfs_superblock *sb) {
    __sync_synchronize();
    return sb && sb->valid;
}

static inline void __vfs_sb_mark_valid(struct vfs_superblock *sb) {
    if (sb) {
        sb->valid = 1;
        __sync_synchronize();
    }
}

static inline void __vfs_sb_mark_invalid(struct vfs_superblock *sb) {
    if (sb) {
        sb->valid = 0;
        __sync_synchronize();
    }
}


#endif // KERNEL_VIRTUAL_FILE_SYSTEM_PRIVATE_H
