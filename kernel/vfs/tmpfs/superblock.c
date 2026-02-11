#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "bits.h"
#include "vfs/stat.h"
#include "lock/mutex_types.h"
#include "lock/rwsem.h"
#include "lock/completion.h"
#include "vfs/fs.h"
#include "../vfs_private.h"
#include "list.h"
#include "hlist.h"
#include <mm/slab.h>
#include <mm/page.h>
#include "tmpfs_private.h"
#include "tmpfs_smoketest.h"

static slab_cache_t __tmpfs_sb_cache = {0};
static slab_cache_t __tmpfs_inode_cache = {0};

/******************************************************************************
 * tmpfs predeclared functions
 *****************************************************************************/
struct vfs_inode *tmpfs_get_inode(struct vfs_superblock *sb, uint64 ino);

/******************************************************************************
 * tmpfs Cache operations
 *****************************************************************************/

static int __tmpfs_init_cache(void) {
    int ret = slab_cache_init(&__tmpfs_inode_cache, "tmpfs_inode_cache",
                              sizeof(struct tmpfs_inode),
                              SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
    if (ret != 0) {
        return ret; // Failed to initialize tmpfs inode cache
    }
    ret = slab_cache_init(&__tmpfs_sb_cache, "tmpfs_superblock_cache",
                          sizeof(struct tmpfs_superblock),
                          SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
    return ret;
}

// Shrink all tmpfs slab caches to release unused memory
void tmpfs_shrink_caches(void) {
    slab_cache_shrink(&__tmpfs_inode_cache, 0x7fffffff);
    slab_cache_shrink(&__tmpfs_sb_cache, 0x7fffffff);
}

static uint64 __tmpfs_ino_alloc(struct tmpfs_sb_private *private_data) {
    return private_data->next_ino++;
}

static struct tmpfs_inode *__tmpfs_alloc_inode_structure(void) {
    struct tmpfs_inode *tmpfs_inode = slab_alloc(&__tmpfs_inode_cache);
    if (tmpfs_inode == NULL) {
        return NULL;
    }
    memset(tmpfs_inode, 0, sizeof(*tmpfs_inode));
    tmpfs_inode->vfs_inode.ops = &tmpfs_inode_ops;
    return tmpfs_inode;
}

struct vfs_inode *tmpfs_alloc_inode(struct vfs_superblock *sb) {
    if (sb == NULL) {
        return ERR_PTR(-EINVAL); // Invalid arguments
    }
    struct tmpfs_sb_private *private_data =
        (struct tmpfs_sb_private *)sb->fs_data;
    if (private_data == NULL) {
        return ERR_PTR(-EINVAL); // Superblock private data is NULL
    }
    struct tmpfs_inode *tmpfs_inode = __tmpfs_alloc_inode_structure();
    if (tmpfs_inode == NULL) {
        return ERR_PTR(-ENOMEM); // Memory allocation failed
    }
    // Allocate a new inode number
    uint64 ino = __tmpfs_ino_alloc(private_data);
    tmpfs_inode->vfs_inode.ino = ino;
    if (ino == 0) {
        slab_free(tmpfs_inode);
        return ERR_PTR(-ENOENT); // Inode number allocation failed
    }
    // Add the inode to the superblock's inode hash list
    return &tmpfs_inode->vfs_inode;
}

// Free a tmpfs inode and its associated data.
// VFS should guarantee the size of inodes are truncated to zero if they are
// regular files or be empty if they are directories before calling this
// function
void tmpfs_free_inode(struct vfs_inode *inode) {
    struct tmpfs_inode *tmpfs_inode =
        container_of(inode, struct tmpfs_inode, vfs_inode);
    slab_free(tmpfs_inode);
}

struct tmpfs_superblock *tmpfs_alloc_superblock(void) {
    struct tmpfs_superblock *sb = slab_alloc(&__tmpfs_sb_cache);
    if (sb == NULL) {
        return NULL;
    }
    memset(sb, 0, sizeof(*sb));
    sb->vfs_sb.backendless = 1; // tmpfs is a backendless filesystem
    sb->vfs_sb.fs_data = &sb->private_data;
    return sb;
}

void tmpfs_free(struct vfs_superblock *sb) {
    struct tmpfs_superblock *tsb =
        container_of(sb, struct tmpfs_superblock, vfs_sb);
    slab_free(tsb);
}

/******************************************************************************
 * Superblock Inode operations
 ******************************************************************************/

struct vfs_inode *tmpfs_get_inode(struct vfs_superblock *sb, uint64 ino) {
    if (sb == NULL) {
        return ERR_PTR(-EINVAL); // Invalid arguments
    }
    // tmpfs does not persist inodes, so cannot load inode by number
    return ERR_PTR(-ENOENT);
}

int tmpfs_sync_fs(struct vfs_superblock *sb, int wait) {
    // tmpfs is an in-memory filesystem, nothing to sync
    sb->dirty = 0;
    return 0;
}

/*
 * tmpfs_unmount_begin - Prepare tmpfs for unmount by evicting unreferenced
 * inodes.
 *
 * For strict unmount to succeed, we need to evict all cached inodes with
 * ref_count == 0 from the hash list. Backendless filesystems keep inodes
 * alive in the cache as long as they have positive n_links, so we need to
 * explicitly clean them up before unmounting.
 *
 * Locking: Caller holds the superblock write lock.
 */
void tmpfs_unmount_begin(struct vfs_superblock *sb) {
    if (sb == NULL) {
        return;
    }

    struct vfs_inode *rooti = sb->root_inode;
    struct vfs_inode *inode, *tmp_inode;

    // Evict all unreferenced inodes from the cache
    // We iterate safely because we may remove nodes during iteration
    hlist_foreach_node_safe(&sb->inodes, inode, tmp_inode, hash_entry) {
        // Skip root inode - it will be handled by vfs_unmount
        if (inode == rooti) {
            continue;
        }

        // Only evict inodes with no references
        if (inode->ref_count > 0) {
            continue;
        }

        vfs_ilock(inode);

        // Double-check ref_count under lock
        if (inode->ref_count > 0) {
            vfs_iunlock(inode);
            continue;
        }

        // Destroy the inode's data if it has any
        if (inode->ops->destroy_inode) {
            inode->ops->destroy_inode(inode);
        }

        // Remove from hash and mark invalid
        inode->valid = 0;
        vfs_remove_inode(sb, inode);
        vfs_iunlock(inode);

        // Free the inode structure
        inode->ops->free_inode(inode);
    }
}

struct vfs_superblock_ops tmpfs_superblock_ops = {
    .alloc_inode = tmpfs_alloc_inode,
    .get_inode = tmpfs_get_inode,
    .sync_fs = tmpfs_sync_fs,
    .unmount_begin = tmpfs_unmount_begin,
};

/******************************************************************************
 * tmpfs Filesystem Type Implementation
 *****************************************************************************/

int tmpfs_mount(struct vfs_inode *mountpoint, struct vfs_inode *device,
                int flags, const char *data, struct vfs_superblock **ret_sb) {
    if (mountpoint == NULL || ret_sb == NULL) {
        return -EINVAL; // Invalid arguments
    }
    if (device != NULL) {
        return -EINVAL; // tmpfs does not support device inode
    }
    struct tmpfs_superblock *sb = tmpfs_alloc_superblock();
    if (sb == NULL) {
        return -ENOMEM; // Failed to allocate superblock
    }
    struct tmpfs_inode *root_inode = __tmpfs_alloc_inode_structure();
    if (root_inode == NULL) {
        tmpfs_free(&sb->vfs_sb);
        return -ENOMEM; // Failed to allocate root inode
    }
    tmpfs_make_directory(root_inode);
    root_inode->vfs_inode.ino = 1; // Root inode number is 1
    root_inode->vfs_inode.n_links =
        2; // Root has 2 links: "." and ".." (both point to self)
    // Initialize superblock fields
    sb->vfs_sb.block_size = PAGE_SIZE;
    sb->vfs_sb.root_inode = &root_inode->vfs_inode;
    sb->private_data.next_ino = 2;
    sb->vfs_sb.backendless = 1;
    sb->vfs_sb.ops = &tmpfs_superblock_ops;

    *ret_sb = &sb->vfs_sb;
    return 0;
}

struct vfs_fs_type_ops tmpfs_fs_type_ops = {
    .mount = tmpfs_mount,
    .free = tmpfs_free,
};

/*
 * tmpfs_init - Initialize tmpfs filesystem type (caches and registration).
 *
 * This only initializes the tmpfs infrastructure. It does NOT mount anything.
 * Call this once during kernel initialization before any tmpfs can be mounted.
 */
void tmpfs_init(void) {
    // Initialize tmpfs inode and superblock caches
    int ret = __tmpfs_init_cache();
    assert(ret == 0, "tmpfs_init: __tmpfs_init_cache failed, errno=%d", ret);

    // Allocate and initialize the tmpfs filesystem type
    struct vfs_fs_type *fs_type = vfs_fs_type_allocate();
    assert(fs_type != NULL, "tmpfs_init: vfs_fs_type_allocate failed");
    fs_type->name = "tmpfs";
    fs_type->ops = &tmpfs_fs_type_ops;

    // Register the filesystem type with the VFS
    vfs_mount_lock();
    ret = vfs_register_fs_type(fs_type);
    assert(ret == 0, "tmpfs_init: vfs_register_fs_type failed, errno=%d", ret);
    vfs_mount_unlock();

    printf("sizeof(tmpfs_inode)=%lu, TMPFS_INODE_EMBEDDED_DATA_LEN=%lu\n",
           sizeof(struct tmpfs_inode), TMPFS_INODE_EMBEDDED_DATA_LEN);
    printf("tmpfs max file size=%lu bytes\n",
           (unsigned long)TMPFS_MAX_FILE_SIZE);
}

/*
 * tmpfs_mount_root - Mount tmpfs as the root filesystem.
 *
 * This mounts a fresh tmpfs instance at the VFS root inode and sets it
 * as the process root. Call this after tmpfs_init() during early boot.
 */
void tmpfs_mount_root(void) {
    int ret;

    vfs_mount_lock();
    vfs_ilock(&vfs_root_inode);

    // Mount the tmpfs at the root inode
    ret = vfs_mount("tmpfs", &vfs_root_inode, NULL, 0, NULL);
    assert(ret == 0, "tmpfs_mount_root: vfs_mount failed, errno=%d", ret);
    // On success, release locks. On failure, vfs_mount already released them.
    if (ret == 0) {
        vfs_iunlock(&vfs_root_inode);
    }
    vfs_mount_unlock();

    ret = vfs_chroot(vfs_root_inode.mnt_rooti);
    assert(ret == 0, "tmpfs_mount_root: vfs_chroot failed, errno=%d", ret);
}
