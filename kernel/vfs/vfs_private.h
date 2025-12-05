#ifndef KERNEL_VIRTUAL_FILE_SYSTEM_PRIVATE_H
#define KERNEL_VIRTUAL_FILE_SYSTEM_PRIVATE_H

#include "vfs/vfs_types.h"


int vfs_get_inode_cached(struct vfs_superblock *sb, uint64 ino,
                         struct vfs_inode **ret_inode);
int vfs_add_inode(struct vfs_superblock *sb,
                  struct vfs_inode *inode,
                  struct vfs_inode **ret_inode);
int vfs_remove_inode(struct vfs_superblock *sb, struct vfs_inode *inode);
void __vfs_inode_init(struct vfs_inode *inode, struct vfs_superblock *sb);

// Assert holding the spinlock of the inode
#define VFS_INODE_ASSERT_HOLDING(__inode, __fmt, ...) do {                  \
    assert((__inode) != NULL, "VFS_INODE_ASSERT_HOLDING: inode is NULL");   \
    assert(holding_mutex(&(__inode)->mutex), __fmt, ##__VA_ARGS__);       \
} while (0)

#define VFS_SUPERBLOCK_ASSERT_WHOLDING(__sb, __fmt, ...) do {                  \
    assert((__sb) != NULL, "VFS_SUPERBLOCK_ASSERT_HOLDING: sb is NULL");   \
    assert(rwlock_is_write_holding(&(__sb)->lock), __fmt, ##__VA_ARGS__);  \
} while (0) 

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

// Validate that the inode is valid and caller holds the ilock
static inline int __vfs_inode_valid_holding(struct vfs_inode *inode) {
    if (inode == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (!holding_mutex(&inode->mutex)) {
        return -EPERM; // Caller does not hold the inode lock
    }
    if (!inode->valid) {
        return -EINVAL; // Inode is not valid
    }
    if (!inode->sb || !inode->sb->valid) {
        printf("vfs_inode_valid_holding: inode's superblock is not valid\n");
        return -EINVAL; // Inode's superblock is not valid
    }
    return 0;
}


#endif // KERNEL_VIRTUAL_FILE_SYSTEM_PRIVATE_H
