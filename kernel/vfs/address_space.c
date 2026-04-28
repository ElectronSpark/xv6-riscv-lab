#include "errno.h"
#include "vfs/address_space.h"
#include "vfs/vfs_types.h"

static int __vfs_aspace_validate_map_args(struct vfs_inode *inode,
                                          uint64 max_blocks,
                                          struct vfs_mapped_extent *extents,
                                          int max_extents) {
    if (inode == NULL || max_blocks == 0 || extents == NULL ||
        max_extents <= 0) {
        return -EINVAL;
    }
    if (inode->a_ops == NULL) {
        return -EOPNOTSUPP;
    }
    return 0;
}

bool vfs_inode_has_address_space(struct vfs_inode *inode) {
    return inode != NULL && inode->a_ops != NULL;
}

int vfs_aspace_map_blocks(struct vfs_inode *inode, uint64 logical_block,
                          uint64 max_blocks, uint32 flags,
                          struct vfs_mapped_extent *extents,
                          int max_extents) {
    int ret = __vfs_aspace_validate_map_args(inode, max_blocks, extents,
                                             max_extents);
    if (ret != 0) {
        return ret;
    }
    if (inode->a_ops->map_blocks == NULL) {
        return -EOPNOTSUPP;
    }
    return inode->a_ops->map_blocks(inode, logical_block, max_blocks, flags,
                                    extents, max_extents);
}

int vfs_aspace_allocate_blocks(struct vfs_inode *inode, uint64 logical_block,
                               uint64 max_blocks, uint32 flags,
                               struct vfs_mapped_extent *extents,
                               int max_extents) {
    int ret = __vfs_aspace_validate_map_args(inode, max_blocks, extents,
                                             max_extents);
    if (ret != 0) {
        return ret;
    }
    if (inode->a_ops->allocate_blocks == NULL) {
        return -EOPNOTSUPP;
    }
    return inode->a_ops->allocate_blocks(inode, logical_block, max_blocks,
                                         flags, extents, max_extents);
}

int vfs_aspace_begin_write(struct vfs_inode *inode, loff_t pos, size_t len,
                           uint32 flags) {
    if (inode == NULL) {
        return -EINVAL;
    }
    if (inode->a_ops == NULL) {
        return -EOPNOTSUPP;
    }
    if (inode->a_ops->begin_write == NULL) {
        return 0;
    }
    return inode->a_ops->begin_write(inode, pos, len, flags);
}

int vfs_aspace_end_write(struct vfs_inode *inode, loff_t pos, size_t len,
                         ssize_t written, int error) {
    if (inode == NULL) {
        return -EINVAL;
    }
    if (inode->a_ops == NULL) {
        return -EOPNOTSUPP;
    }
    if (inode->a_ops->end_write == NULL) {
        return 0;
    }
    return inode->a_ops->end_write(inode, pos, len, written, error);
}

int vfs_aspace_commit_size(struct vfs_inode *inode, loff_t new_size) {
    if (inode == NULL) {
        return -EINVAL;
    }
    if (inode->a_ops == NULL || inode->a_ops->commit_size == NULL) {
        return -EOPNOTSUPP;
    }
    return inode->a_ops->commit_size(inode, new_size);
}

int vfs_aspace_invalidate_mapping(struct vfs_inode *inode, loff_t start,
                                  loff_t len) {
    if (inode == NULL) {
        return -EINVAL;
    }
    if (inode->a_ops == NULL) {
        return -EOPNOTSUPP;
    }
    if (inode->a_ops->invalidate_mapping == NULL) {
        return 0;
    }
    return inode->a_ops->invalidate_mapping(inode, start, len);
}

int vfs_aspace_truncate_mapping(struct vfs_inode *inode, loff_t new_size) {
    if (inode == NULL) {
        return -EINVAL;
    }
    if (inode->a_ops == NULL) {
        return -EOPNOTSUPP;
    }
    if (inode->a_ops->truncate_mapping == NULL) {
        return 0;
    }
    return inode->a_ops->truncate_mapping(inode, new_size);
}

int vfs_aspace_sync_mapping_metadata(struct vfs_inode *inode, int wait) {
    if (inode == NULL) {
        return -EINVAL;
    }
    if (inode->a_ops == NULL) {
        return -EOPNOTSUPP;
    }
    if (inode->a_ops->sync_mapping_metadata == NULL) {
        return 0;
    }
    return inode->a_ops->sync_mapping_metadata(inode, wait);
}

int vfs_aspace_flush_device_cache(struct vfs_inode *inode) {
    if (inode == NULL) {
        return -EINVAL;
    }
    if (inode->a_ops == NULL) {
        return -EOPNOTSUPP;
    }
    if (inode->a_ops->flush_device_cache == NULL) {
        return 0;
    }
    return inode->a_ops->flush_device_cache(inode);
}
