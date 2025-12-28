#ifndef KERNEL_VIRTUAL_FILE_SYSTEM_PRIVATE_H
#define KERNEL_VIRTUAL_FILE_SYSTEM_PRIVATE_H

#include "vfs/vfs_types.h"
#include "completion.h"
#include "printf.h"

extern struct vfs_inode vfs_root_inode;


struct vfs_inode *vfs_get_inode_cached(struct vfs_superblock *sb, uint64 ino);
struct vfs_inode *vfs_add_inode(struct vfs_superblock *sb,
                                struct vfs_inode *inode);
int vfs_remove_inode(struct vfs_superblock *sb, struct vfs_inode *inode);
void __vfs_inode_init(struct vfs_inode *inode);
void __vfs_file_init(void);
void __vfs_file_shrink_cache(void);
void __vfs_shrink_caches(void);
void tmpfs_init_fs_type(void);
void xv6fs_init_fs_type(void);

// Orphan management
int vfs_make_orphan(struct vfs_inode *inode);
void __vfs_final_unmount_cleanup(struct vfs_superblock *sb);

// Check if superblock is usable for new operations
static inline int vfs_sb_check_usable(struct vfs_superblock *sb) {
    if (sb == NULL) return -EINVAL;
    if (!sb->valid) return -EINVAL;
    if (sb->unmounting) return -ESHUTDOWN;
    if (!sb->attached) return -ENOENT;
    return 0;
}

static inline bool vfs_sb_is_attached(struct vfs_superblock *sb) {
    return sb != NULL && sb->attached;
}

static inline bool vfs_sb_is_syncing(struct vfs_superblock *sb) {
    return sb != NULL && sb->syncing;
}

static inline bool vfs_sb_is_unmounting(struct vfs_superblock *sb) {
    return sb != NULL && sb->unmounting;
}

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

// Validate that the inode is valid and caller holds the ilock
static inline int __vfs_inode_valid(struct vfs_inode *inode) {
    if (!inode->valid) {
        return -EINVAL; // Inode is not valid
    }
    // Allow orphan inodes on detached superblocks - they're still usable
    // until their last reference is dropped
    if (!inode->sb->valid && inode->sb->attached) {
        printf("__vfs_inode_valid: inode's superblock is not valid\n");
        return -EINVAL; // Inode's superblock is not valid
    }
    return 0;
}

// Validate the validity of a directory
// Assume the validity of superblock is already checked
static inline int __vfs_dir_inode_valid_holding(struct vfs_inode *inode) {
    if (inode == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (!holding_mutex(&inode->mutex)) {
        return -EPERM; // Caller does not hold the inode lock
    }
    if (!inode->valid) {
        return -EINVAL; // Inode is not valid
    }
    if (!S_ISDIR(inode->mode)) {
        return -EINVAL; // Inode is not a mountpoint
    }
    if (inode != &vfs_root_inode) {
        if (inode->sb == NULL || !inode->sb->valid) {
            return -EINVAL; // Inode's superblock is not valid
        }
    }
    return 0;
}


#endif // KERNEL_VIRTUAL_FILE_SYSTEM_PRIVATE_H
