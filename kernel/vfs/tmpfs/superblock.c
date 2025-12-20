#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "bits.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "mutex_types.h"
#include "rwlock.h"
#include "completion.h"
#include "vfs/fs.h"
#include "../vfs_private.h"
#include "list.h"
#include "hlist.h"
#include "slab.h"
#include "page.h"
#include "tmpfs_private.h"
#include "tmpfs_smoketest.h"


static slab_cache_t __tmpfs_sb_cache = { 0 };
static slab_cache_t __tmpfs_inode_cache = { 0 };

/******************************************************************************
 * tmpfs predeclared functions
 *****************************************************************************/
int tmpfs_get_inode(struct vfs_superblock *sb, uint64 ino,
                    struct vfs_inode **ret_inode);

/******************************************************************************
 * tmpfs Cache operations
 *****************************************************************************/

static int __tmpfs_init_cache(void) {
    int ret = slab_cache_init(&__tmpfs_inode_cache, "tmpfs_inode_cache",
                              sizeof(struct tmpfs_inode), 
                              SLAB_FLAG_EMBEDDED);
    if (ret != 0) {
        return ret; // Failed to initialize tmpfs inode cache
    }
    return slab_cache_init(&__tmpfs_sb_cache, "tmpfs_superblock_cache",
                           sizeof(struct tmpfs_superblock), 
                           SLAB_FLAG_EMBEDDED);
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

int tmpfs_alloc_inode(struct vfs_superblock *sb, struct vfs_inode **ret_inode) {
    if (sb == NULL || ret_inode == NULL) {
        return -EINVAL; // Invalid arguments
    }
    struct tmpfs_sb_private *private_data = (struct tmpfs_sb_private *)sb->fs_data;
    if (private_data == NULL) {
        return -EINVAL; // Superblock private data is NULL
    }
    struct tmpfs_inode *tmpfs_inode = __tmpfs_alloc_inode_structure();
    if (tmpfs_inode == NULL) {
        return -ENOMEM; // Memory allocation failed
    }
    // Allocate a new inode number
    uint64 ino = __tmpfs_ino_alloc(private_data);
    tmpfs_inode->vfs_inode.ino = ino;
    if (ino == 0) {
        slab_free(tmpfs_inode);
        return -ENOENT; // Inode number allocation failed
    }
    // Add the inode to the superblock's inode hash list
    *ret_inode = &tmpfs_inode->vfs_inode;
    return 0;
}

// Free a tmpfs inode and its associated data.
// VFS should guarantee the size of inodes are truncated to zero if they are regular files
// or be empty if they are directories before calling this function
void tmpfs_free_inode(struct vfs_inode *inode) {
    struct tmpfs_inode *tmpfs_inode = container_of(inode, struct tmpfs_inode, vfs_inode);
    if (S_ISLNK(inode->mode)) {
        tmpfs_free_symlink_target(tmpfs_inode);
    }
    slab_free(inode);
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
    slab_free(container_of(sb, struct tmpfs_superblock, vfs_sb));
}

/******************************************************************************
 * Superblock Inode operations
 ******************************************************************************/

int tmpfs_get_inode(struct vfs_superblock *sb, uint64 ino,
                    struct vfs_inode **ret_inode) {
    if (sb == NULL || ret_inode == NULL) {
        return -EINVAL; // Invalid arguments
    }
    // tmpfs does not persist inodes, so cannot load inode by number
    *ret_inode = NULL;
    return -ENOENT;
}

int tmpfs_sync_fs(struct vfs_superblock *sb, int wait) {
    // tmpfs is an in-memory filesystem, nothing to sync
    sb->dirty = 0;
    return 0;
}

void tmpfs_unmount_begin(struct vfs_superblock *sb) {

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
                int flags, const char *data, struct vfs_superblock **ret_sb
) {
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
    tmpfs_make_directory(root_inode, root_inode); // Root's parent is itself
    root_inode->vfs_inode.ino = 1; // Root inode number is 1
    root_inode->vfs_inode.n_links = 1;
    // Initialize superblock fields
    sb->vfs_sb.block_size = PAGE_SIZE;
    sb->vfs_sb.root_inode = &root_inode->vfs_inode;
    sb->private_data.next_ino = 2;
    sb->vfs_sb.ops = &tmpfs_superblock_ops;
    
    *ret_sb = &sb->vfs_sb;
    return 0;
}


struct vfs_fs_type_ops tmpfs_fs_type_ops = {
    .mount = tmpfs_mount,
    .free = tmpfs_free,
};

void tmpfs_init_fs_type(void) {
    // Initialize tmpfs inode and superblock caches
    int ret = __tmpfs_init_cache();
    assert(ret == 0, "tmpfs_init_fs_type: __tmpfs_init_cache failed, errno=%d", ret);

    // Allocate and initialize the tmpfs filesystem type
    struct vfs_fs_type *fs_type = vfs_fs_type_allocate();
    assert(fs_type != NULL, "tmpfs_init_fs_type: vfs_fs_type_allocate failed");
    fs_type->name = "tmpfs";
    fs_type->ops = &tmpfs_fs_type_ops;

    // Register the filesystem type with the VFS
    vfs_mount_lock();
    ret = vfs_register_fs_type(fs_type);
    assert(ret == 0, "tmpfs_init_fs_type: vfs_register_fs_type failed, errno=%d", ret);
    vfs_ilock(&vfs_root_inode);

    // Mount the tmpfs at the root inode
    ret = vfs_mount("tmpfs", &vfs_root_inode, NULL, 0, NULL);
    assert(ret == 0, "tmpfs_init_fs_type: vfs_mount failed, errno=%d", ret);
    vfs_iunlock(&vfs_root_inode);
    vfs_mount_unlock();

    printf("sizeof(tmpfs_inode)=%lu, TMPFS_INODE_EMBEDDED_DATA_LEN=%lu\n",
           sizeof(struct tmpfs_inode), TMPFS_INODE_EMBEDDED_DATA_LEN);
    printf("tmpfs max file size=%lu bytes\n", TMPFS_MAX_FILE_SIZE);
    printf("TMPFS_INODE_DBLOCKS=%lu, TMPFS_INODE_INDRECT_ITEMS=%lu\n",
           TMPFS_INODE_DBLOCKS, TMPFS_INODE_INDRECT_ITEMS);

    // Shrink all caches before baseline to get a clean state
    tmpfs_shrink_caches();
    kmm_shrink_all();

    // Memory leak check for inode smoketest
    uint64 before_pages = get_total_free_pages();
    tmpfs_run_inode_smoketest();
    tmpfs_shrink_caches();
    kmm_shrink_all();
    uint64 after_pages = get_total_free_pages();
    if (before_pages != after_pages) {
        printf("MEMORY LEAK: inode_smoketest leaked %ld pages\n", 
               (long)(before_pages - after_pages));
    } else {
        printf("inode_smoketest: no memory leak detected\n");
    }

    // Memory leak check for truncate smoketest
    before_pages = get_total_free_pages();
    tmpfs_run_truncate_smoketest();
    tmpfs_shrink_caches();
    kmm_shrink_all();
    after_pages = get_total_free_pages();
    if (before_pages != after_pages) {
        printf("MEMORY LEAK: truncate_smoketest leaked %ld pages\n",
               (long)(before_pages - after_pages));
    } else {
        printf("truncate_smoketest: no memory leak detected\n");
    }
}
