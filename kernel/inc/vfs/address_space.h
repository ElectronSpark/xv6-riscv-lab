#ifndef __KERNEL_VFS_ADDRESS_SPACE_H
#define __KERNEL_VFS_ADDRESS_SPACE_H

#include "types.h"
#include "dev/dev_types.h"

struct vfs_inode;

/*
 * Generic regular-file data I/O mapping contract.
 *
 * The VFS/page-cache layer owns byte transfer and BIO construction.  A
 * filesystem that installs these operations owns only logical-file-block to
 * physical-device-block mapping, allocation, metadata transaction boundaries,
 * truncate, and exact size commit.
 */

/* Request flags passed to map/allocate callbacks. */
#define VFS_ASPACE_F_NOWAIT       (1U << 0)
#define VFS_ASPACE_F_WRITE        (1U << 1)
#define VFS_ASPACE_F_MMAP         (1U << 2)
#define VFS_ASPACE_F_WRITEBACK    (1U << 3)
#define VFS_ASPACE_F_READAHEAD    (1U << 4)

/* Extent flags returned by map/allocate callbacks. */
#define VFS_EXTENT_F_MAPPED       (1U << 0)
#define VFS_EXTENT_F_HOLE         (1U << 1)
#define VFS_EXTENT_F_UNWRITTEN    (1U << 2)
#define VFS_EXTENT_F_NEW          (1U << 3)

/*
 * One logical file extent mapped to a device extent.
 *
 * logical_block and block_count are in filesystem block units.  block_size
 * records that unit size in bytes.  physical_block is in the target block
 * device's native block units, matching blkdev_submit_bio() addressing after
 * the generic layer converts to BIO sectors as needed.
 */
struct vfs_mapped_extent {
    uint64 logical_block;
    uint64 physical_block;
    uint64 block_count;
    size_t block_size;
    blkdev_t *bdev;
    uint32 flags;
};

struct vfs_address_space_limits {
    uint64 max_file_size;
    size_t block_size;
    size_t max_write_bytes;
    uint8 preferred_folio_order;
    uint8 supports_sparse;
    uint8 writeback_may_allocate;
};

struct vfs_address_space_ops {
    /*
     * Map existing logical file blocks.  Return the number of extents filled
     * or a negative errno.  Holes must be reported with VFS_EXTENT_F_HOLE
     * rather than silently skipped.
     */
    int (*map_blocks)(struct vfs_inode *inode, uint64 logical_block,
                      uint64 max_blocks, uint32 flags,
                      struct vfs_mapped_extent *extents, int max_extents);

    /*
     * Allocate missing logical file blocks and return their mappings.  This is
     * called before generic write code dirties page-cache data that requires
     * stable backing blocks.
     */
    int (*allocate_blocks)(struct vfs_inode *inode, uint64 logical_block,
                           uint64 max_blocks, uint32 flags,
                           struct vfs_mapped_extent *extents,
                           int max_extents);

    /*
     * Write lifecycle hooks.  Filesystems may use these for transaction/log
     * boundaries.  begin_write is intentionally separate so xv6fs can begin a
     * transaction before inode locking in callers that need that ordering.
     */
    int (*begin_write)(struct vfs_inode *inode, loff_t pos, size_t len,
                       uint32 flags);
    int (*end_write)(struct vfs_inode *inode, loff_t pos, size_t len,
                     ssize_t written, int error);
    int (*commit_size)(struct vfs_inode *inode, loff_t new_size);

    /* Cache and metadata coordination hooks. */
    int (*invalidate_mapping)(struct vfs_inode *inode, loff_t start,
                              loff_t len);
    int (*truncate_mapping)(struct vfs_inode *inode, loff_t new_size);
    int (*sync_mapping_metadata)(struct vfs_inode *inode, int wait);
    int (*flush_device_cache)(struct vfs_inode *inode);

    struct vfs_address_space_limits limits;
};

bool vfs_inode_has_address_space(struct vfs_inode *inode);
int vfs_aspace_map_blocks(struct vfs_inode *inode, uint64 logical_block,
                          uint64 max_blocks, uint32 flags,
                          struct vfs_mapped_extent *extents,
                          int max_extents);
int vfs_aspace_allocate_blocks(struct vfs_inode *inode, uint64 logical_block,
                               uint64 max_blocks, uint32 flags,
                               struct vfs_mapped_extent *extents,
                               int max_extents);
int vfs_aspace_begin_write(struct vfs_inode *inode, loff_t pos, size_t len,
                           uint32 flags);
int vfs_aspace_end_write(struct vfs_inode *inode, loff_t pos, size_t len,
                         ssize_t written, int error);
int vfs_aspace_commit_size(struct vfs_inode *inode, loff_t new_size);
int vfs_aspace_invalidate_mapping(struct vfs_inode *inode, loff_t start,
                                  loff_t len);
int vfs_aspace_truncate_mapping(struct vfs_inode *inode, loff_t new_size);
int vfs_aspace_sync_mapping_metadata(struct vfs_inode *inode, int wait);
int vfs_aspace_flush_device_cache(struct vfs_inode *inode);

#endif /* __KERNEL_VFS_ADDRESS_SPACE_H */
