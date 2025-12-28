/*
 * xv6fs superblock operations
 * 
 * Handles mounting, syncing, and managing the xv6 filesystem superblock.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"
#include "mutex_types.h"
#include "buf.h"
#include "vfs/fs.h"
#include "../vfs_private.h"
#include "slab.h"
#include "xv6fs_private.h"
#include "xv6fs_smoketest.h"

// Slab caches for xv6fs structures
slab_cache_t __xv6fs_inode_cache;
static slab_cache_t __xv6fs_sb_cache;

/******************************************************************************
 * Slab cache initialization
 ******************************************************************************/

static int __xv6fs_init_cache(void) {
    int ret = 0;
    ret = slab_cache_init(&__xv6fs_inode_cache, "xv6fs_inode",
                          sizeof(struct xv6fs_inode), 0);
    if (ret != 0) {
        return ret;
    }
    ret = slab_cache_init(&__xv6fs_sb_cache, "xv6fs_sb",
                          sizeof(struct xv6fs_superblock), 0);
    if (ret != 0) {
        return ret;
    }
    return 0;
}

// Shrink xv6fs slab caches to release unused pages
void xv6fs_shrink_caches(void) {
    slab_cache_shrink(&__xv6fs_inode_cache, 0x7fffffff);
    slab_cache_shrink(&__xv6fs_sb_cache, 0x7fffffff);
}

/******************************************************************************
 * Superblock read/write helpers
 ******************************************************************************/

// Read the superblock from disk
static int __xv6fs_read_superblock(uint dev, struct superblock *disk_sb) {
    struct buf *bp = bread(dev, 1);
    if (bp == NULL) {
        return -EIO;
    }
    memmove(disk_sb, bp->data, sizeof(*disk_sb));
    brelse(bp);
    
    if (disk_sb->magic != FSMAGIC) {
        return -EINVAL;
    }
    return 0;
}

// Write the superblock to disk
static int __xv6fs_write_superblock(uint dev, struct superblock *disk_sb) {
    struct buf *bp = bread(dev, 1);
    if (bp == NULL) {
        return -EIO;
    }
    memmove(bp->data, disk_sb, sizeof(*disk_sb));
    bwrite(bp);
    brelse(bp);
    return 0;
}

/******************************************************************************
 * Inode allocation
 ******************************************************************************/

static struct xv6fs_inode *__xv6fs_alloc_inode_structure(void) {
    struct xv6fs_inode *xv6fs_inode = slab_alloc(&__xv6fs_inode_cache);
    if (xv6fs_inode == NULL) {
        return NULL;
    }
    memset(xv6fs_inode, 0, sizeof(*xv6fs_inode));
    xv6fs_inode->vfs_inode.ops = &xv6fs_inode_ops;
    return xv6fs_inode;
}

struct vfs_inode *xv6fs_alloc_inode(struct vfs_superblock *sb) {
    if (sb == NULL) {
        return ERR_PTR(-EINVAL);
    }
    
    struct xv6fs_superblock *xv6_sb = container_of(sb, struct xv6fs_superblock, vfs_sb);
    struct superblock *disk_sb = &xv6_sb->disk_sb;
    uint dev = xv6_sb->dev;
    
    // Find a free inode on disk
    struct buf *bp;
    struct dinode *dip;
    
    for (uint inum = 1; inum < disk_sb->ninodes; inum++) {
        bp = bread(dev, XV6FS_IBLOCK(inum, disk_sb));
        if (bp == NULL) {
            return ERR_PTR(-EIO);
        }
        dip = (struct dinode*)bp->data + inum % IPB;
        if (dip->type == 0) {
            // Found a free inode
            memset(dip, 0, sizeof(*dip));
            // Mark as allocated but type will be set by caller
            xv6fs_log_write(xv6_sb, bp);
            brelse(bp);
            
            // Allocate in-memory structure
            struct xv6fs_inode *xv6fs_inode = __xv6fs_alloc_inode_structure();
            if (xv6fs_inode == NULL) {
                return ERR_PTR(-ENOMEM);
            }
            
            xv6fs_inode->dev = dev;
            xv6fs_inode->vfs_inode.ino = inum;
            // Note: Do NOT set vfs_inode.sb here - VFS will set it in vfs_add_inode
            xv6fs_inode->vfs_inode.ref_count = 1;
            
            return &xv6fs_inode->vfs_inode;
        }
        brelse(bp);
    }
    
    return ERR_PTR(-ENOSPC);
}

/******************************************************************************
 * Get inode from disk
 ******************************************************************************/

struct vfs_inode *xv6fs_get_inode(struct vfs_superblock *sb, uint64 ino) {
    if (sb == NULL || ino == 0) {
        return ERR_PTR(-EINVAL);
    }
    
    struct xv6fs_superblock *xv6_sb = container_of(sb, struct xv6fs_superblock, vfs_sb);
    struct superblock *disk_sb = &xv6_sb->disk_sb;
    uint dev = xv6_sb->dev;
    
    if (ino >= disk_sb->ninodes) {
        return ERR_PTR(-ENOENT);
    }
    
    // Read inode from disk
    struct buf *bp = bread(dev, XV6FS_IBLOCK(ino, disk_sb));
    if (bp == NULL) {
        return ERR_PTR(-EIO);
    }
    
    struct dinode *dip = (struct dinode*)bp->data + ino % IPB;
    if (dip->type == 0) {
        brelse(bp);
        return ERR_PTR(-ENOENT);
    }
    
    // Allocate in-memory inode
    struct xv6fs_inode *xv6fs_inode = __xv6fs_alloc_inode_structure();
    if (xv6fs_inode == NULL) {
        brelse(bp);
        return ERR_PTR(-ENOMEM);
    }
    
    // Fill in VFS inode fields
    xv6fs_inode->dev = dev;
    xv6fs_inode->vfs_inode.ino = ino;
    // Note: Do NOT set vfs_inode.sb here - VFS will set it when adding to hash
    xv6fs_inode->vfs_inode.ref_count = 1;
    xv6fs_inode->vfs_inode.mode = xv6fs_type_to_mode(dip->type);
    xv6fs_inode->vfs_inode.n_links = dip->nlink;
    xv6fs_inode->vfs_inode.size = dip->size;
    
    // Fill in xv6fs-specific fields
    xv6fs_inode->major = dip->major;
    xv6fs_inode->minor = dip->minor;
    memmove(xv6fs_inode->addrs, dip->addrs, sizeof(dip->addrs));
    
    // Set device number for character/block devices
    if (S_ISCHR(xv6fs_inode->vfs_inode.mode)) {
        xv6fs_inode->vfs_inode.cdev = (xv6fs_inode->major << 8) | xv6fs_inode->minor;
    }
    
    brelse(bp);
    return &xv6fs_inode->vfs_inode;
}

/******************************************************************************
 * Sync operations
 ******************************************************************************/

int xv6fs_sync_fs(struct vfs_superblock *sb, int wait) {
    if (sb == NULL) {
        return -EINVAL;
    }
    
    struct xv6fs_superblock *xv6_sb = container_of(sb, struct xv6fs_superblock, vfs_sb);
    
    // Write superblock to disk if dirty
    if (xv6_sb->dirty) {
        int ret = __xv6fs_write_superblock(xv6_sb->dev, &xv6_sb->disk_sb);
        if (ret != 0) {
            return ret;
        }
        xv6_sb->dirty = 0;
    }
    
    sb->dirty = 0;
    return 0;
}

void xv6fs_unmount_begin(struct vfs_superblock *sb) {
    // Sync any pending changes before unmount
    xv6fs_sync_fs(sb, 1);
}

/******************************************************************************
 * Mount/Free operations
 ******************************************************************************/

void xv6fs_free(struct vfs_superblock *sb) {
    struct xv6fs_superblock *xv6_sb = container_of(sb, struct xv6fs_superblock, vfs_sb);
    slab_free(xv6_sb);
}

int xv6fs_mount(struct vfs_inode *mountpoint, struct vfs_inode *device,
                int flags, const char *data, struct vfs_superblock **ret_sb) {
    if (mountpoint == NULL || ret_sb == NULL) {
        return -EINVAL;
    }
    
    // Use the second disk (mkdev(2, 2)) for xv6fs VFS mount
    // This avoids conflicts with the first disk used by the legacy fs subsystem
    uint dev = mkdev(2, 2);
    
    // Allocate superblock
    struct xv6fs_superblock *xv6_sb = slab_alloc(&__xv6fs_sb_cache);
    if (xv6_sb == NULL) {
        return -ENOMEM;
    }
    memset(xv6_sb, 0, sizeof(*xv6_sb));
    
    // Read on-disk superblock
    int ret = __xv6fs_read_superblock(dev, &xv6_sb->disk_sb);
    if (ret != 0) {
        slab_free(xv6_sb);
        return ret;
    }
    
    xv6_sb->dev = dev;
    xv6_sb->dirty = 0;
    
    // Initialize logging layer
    xv6fs_initlog(xv6_sb);
    
    // Initialize VFS superblock
    xv6_sb->vfs_sb.block_size = XV6FS_BSIZE;
    xv6_sb->vfs_sb.total_blocks = xv6_sb->disk_sb.size;
    // Mark as backendless from VFS perspective since xv6fs manages device access
    // internally via bread(dev, blockno) rather than through a VFS device inode
    xv6_sb->vfs_sb.backendless = 1;
    xv6_sb->vfs_sb.ops = &xv6fs_superblock_ops;
    xv6_sb->vfs_sb.fs_data = xv6_sb;
    
    // Load root inode (inode 1 in xv6)
    struct vfs_inode *root_inode = xv6fs_get_inode(&xv6_sb->vfs_sb, ROOTINO);
    if (IS_ERR_OR_NULL(root_inode)) {
        slab_free(xv6_sb);
        return root_inode == NULL ? -ENOMEM : PTR_ERR(root_inode);
    }
    
    xv6_sb->vfs_sb.root_inode = root_inode;
    
    *ret_sb = &xv6_sb->vfs_sb;
    return 0;
}

/******************************************************************************
 * Orphan inode operations
 *
 * These operations are used by the VFS layer to track orphan inodes (inodes
 * with n_links=0 but ref_count>0) for crash recovery. When an inode becomes
 * an orphan, add_orphan is called to record it persistently. When the last
 * reference drops and the inode is destroyed, remove_orphan is called.
 * On mount, recover_orphans is called to clean up orphans from a previous
 * crash.
 *
 * TODO: Implement persistent orphan journal. For now, these are stubs that
 * allow the VFS unmount path to work correctly. If the system crashes with
 * orphan inodes, those inodes will leak until fsck is run.
 ******************************************************************************/

// Add an inode to the orphan list (called when n_links drops to 0)
static int xv6fs_add_orphan(struct vfs_superblock *sb, struct vfs_inode *inode) {
    // TODO: Implement persistent orphan journal
    // For now, just return success - the VFS layer maintains an in-memory list
    (void)sb;
    (void)inode;
    return 0;
}

// Remove an inode from the orphan list (called after destroy_inode)
static int xv6fs_remove_orphan(struct vfs_superblock *sb, struct vfs_inode *inode) {
    // TODO: Implement persistent orphan journal
    (void)sb;
    (void)inode;
    return 0;
}

// Recover orphan inodes from a previous crash (called during mount)
static int xv6fs_recover_orphans(struct vfs_superblock *sb) {
    // TODO: Implement persistent orphan recovery
    // Walk the orphan journal and destroy/free each orphan inode
    (void)sb;
    return 0;
}

/******************************************************************************
 * VFS operations structures
 ******************************************************************************/

struct vfs_superblock_ops xv6fs_superblock_ops = {
    .alloc_inode = xv6fs_alloc_inode,
    .get_inode = xv6fs_get_inode,
    .sync_fs = xv6fs_sync_fs,
    .unmount_begin = xv6fs_unmount_begin,
    .add_orphan = xv6fs_add_orphan,
    .remove_orphan = xv6fs_remove_orphan,
    .recover_orphans = xv6fs_recover_orphans,
};

struct vfs_fs_type_ops xv6fs_fs_type_ops = {
    .mount = xv6fs_mount,
    .free = xv6fs_free,
};

/******************************************************************************
 * Filesystem type initialization
 ******************************************************************************/

void xv6fs_init_fs_type(void) {
    // Initialize caches
    int ret = __xv6fs_init_cache();
    assert(ret == 0, "xv6fs_init_fs_type: __xv6fs_init_cache failed, errno=%d", ret);
    
    // Allocate and register filesystem type
    struct vfs_fs_type *fs_type = vfs_fs_type_allocate();
    assert(fs_type != NULL, "xv6fs_init_fs_type: vfs_fs_type_allocate failed");
    
    fs_type->name = "xv6fs";
    fs_type->ops = &xv6fs_fs_type_ops;
    
    vfs_mount_lock();
    ret = vfs_register_fs_type(fs_type);
    assert(ret == 0, "xv6fs_init_fs_type: vfs_register_fs_type failed, errno=%d", ret);
    vfs_mount_unlock();
    
    printf("xv6fs: filesystem type registered\n");
    
    // Mount xv6fs at /disk for smoke tests
    struct vfs_inode *tmpfs_root = vfs_root_inode.mnt_rooti;
    if (tmpfs_root != NULL) {
        // Create /disk directory in tmpfs root (vfs_mkdir handles its own locking)
        struct vfs_inode *disk_dir = vfs_mkdir(tmpfs_root, 0755, "disk", 4);
        
        if (!IS_ERR_OR_NULL(disk_dir)) {
            // Mount xv6fs at /disk
            // vfs_mount requires: mount mutex, superblock write lock, and inode lock
            vfs_mount_lock();
            vfs_superblock_wlock(disk_dir->sb);
            vfs_ilock(disk_dir);
            ret = vfs_mount("xv6fs", disk_dir, NULL, 0, NULL);
            vfs_iunlock(disk_dir);
            vfs_superblock_unlock(disk_dir->sb);
            vfs_mount_unlock();
            
            if (ret == 0) {
                printf("xv6fs: mounted at /disk\n");
            } else {
                printf("xv6fs: failed to mount at /disk, errno=%d\n", ret);
            }
            vfs_iput(disk_dir);
        } else {
            printf("xv6fs: failed to create /disk directory\n");
        }
    }
    
    xv6fs_run_all_smoketests();
}
