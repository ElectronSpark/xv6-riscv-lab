#ifndef KERNEL_VIRTUAL_FILE_SYSTEM_TMPFS_PRIVATE_H
#define KERNEL_VIRTUAL_FILE_SYSTEM_TMPFS_PRIVATE_H

#include "hlist_type.h"
#include "vfs/vfs_types.h"

struct tmpfs_inode {
    struct vfs_inode vfs_inode;
    // tmpfs specific inode data can be added here

};

int tmpfs_alloc_inode(struct vfs_superblock *sb, struct vfs_inode **ret_inode);
void tmpfs_free_inode(struct vfs_inode *inode);


#endif // KERNEL_VIRTUAL_FILE_SYSTEM_TMPFS_PRIVATE_H
