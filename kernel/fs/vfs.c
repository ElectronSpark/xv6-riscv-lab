#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs/vfs.h"
#include "hlist.h"
#include "slab.h"

spinlock_t vfs_lock;
list_node_t vfs_fs_types;               // List of registered filesystem types
struct vfs_dentry *vfs_root_dentry;     // Root dentry for the virtual filesystem
slab_cache_t vfs_fs_type_cache;         // Cache for filesystem types


// Allocate a new filesystem type structure.
static struct fs_type *__fs_type_alloc(void) {
    struct fs_type *fs_type = slab_alloc(&vfs_fs_type_cache);
    if (fs_type == NULL) {
        panic("vfs_fs_type_alloc: slab_alloc failed");
    }
    memset(fs_type, 0, sizeof(*fs_type));
    fs_type->name = "null"; // Initialize name to NULL
    fs_type->f_type = 0;  // Initialize filesystem type identifier to 0
    fs_type->active_sbs = 0;
    fs_type->ops = NULL;
    list_entry_init(&fs_type->s_list_head);
    list_entry_init(&fs_type->registered_entry);
    return fs_type;
}

// free a filesystem type structure.
static void __fs_type_free(struct fs_type *fs_type) {
    if (fs_type == NULL) {
        return;
    }
    assert(fs_type->active_sbs == 0, "vfs_fs_type_free: active superblocks count is not zero");
    assert(LIST_NODE_IS_DETACHED(fs_type, registered_entry), "vfs_fs_type_free: fs_type is still registered");
    assert(LIST_NODE_IS_DETACHED(fs_type, s_list_head), "vfs_fs_type_free: fs_type is still in superblock list");
    slab_free(fs_type);
}

// Locking functions for the VFS
static void __vfs_lock(void) {
    spin_acquire(&vfs_lock);
}

static void __vfs_unlock(void) {
    spin_release(&vfs_lock);
}

static void __vfs_assert_holding(void) {
    assert(spin_holding(&vfs_lock), "vfs_lock is not held");
}

void vfs_init(void) {
    spin_init(&vfs_lock, "vfs_lock");
    list_entry_init(&vfs_fs_types);
    slab_cache_init(&vfs_fs_type_cache, "fs_type_cache", sizeof(struct fs_type), SLAB_FLAG_STATIC);
    vfs_root_dentry = NULL; // Initialize root dentry to NULL
}

int vfs_register_fs_type(const char *name, uint64 f_type, struct fs_type_ops *ops) {
    int ret_val = -1;
    __vfs_lock();
    if (f_type == 0) {
        ret_val = -1; // Invalid filesystem type identifier
        goto ret;
    }
    if (ops == NULL || ops->mount == NULL || ops->umount == NULL) {
        ret_val = -1; // Invalid parameters
        goto ret;
    }

    struct fs_type *existing_fs_type = vfs_get_fs_type(f_type);
    if (existing_fs_type != NULL) {
        ret_val = -1; // Filesystem type already registered
        goto ret;
    }

    struct fs_type *fs_type = __fs_type_alloc();
    if (fs_type == NULL) {
        ret_val = -1; // Failed to allocate fs_type
        goto ret;
    }

    fs_type->name = name ? name : "null";
    fs_type->f_type = f_type;
    fs_type->ops = ops;

    // Register the filesystem type
    list_node_push(&vfs_fs_types, fs_type, registered_entry);
    ret_val = 0; // Success

ret:
    __vfs_unlock();
    return ret_val;
}

// When trying to unregister a filesystem type, it will be frozen first.
// And after all superblocks are unmounted, it can be safely removed.
void vfs_freeze_fs_type(struct fs_type *fs_type) {
    // @TODO: Implement vfs_freeze_fs_type
    panic("vfs_freeze_fs_type: not implemented yet");
}

void vfs_unregister_fs_type(struct fs_type *fs_type) {
    if (fs_type == NULL) {
        return; // Nothing to unregister
    }
    __vfs_lock();
    assert(fs_type->frozen, "vfs_unregister_fs_type: fs_type is not frozen");
    list_node_detach(fs_type, registered_entry);
    __fs_type_free(fs_type);
    __vfs_unlock();
}

struct fs_type *vfs_get_fs_type(uint64 f_type) {
    __vfs_lock();
    struct fs_type *fs_type;
    struct fs_type *tmp;
    list_foreach_node_safe(&vfs_fs_types, fs_type, tmp, registered_entry) {
        if (fs_type->f_type == f_type) {
            __vfs_unlock();
            return fs_type;
        }
    }
    __vfs_unlock();
    return NULL; // Not found
}

int vfs_mount(struct vfs_dentry *dentry, dev_t dev) {
    __vfs_assert_holding();
    panic("vfs_mount: not implemented yet");
    // @TODO: check if dentry is a valid mount point(directories only)
    // @TODO: check if dentry is not already mounted
    // 
}

int vfs_umount(struct super_block *sb) {
    __vfs_assert_holding();
    // @TODO:
    panic("vfs_umount: not implemented yet");
}

// Mount a device as root
// VFS lock must be held
void vfs_mount_root(dev_t dev, uint64 f_type) {
    __vfs_assert_holding();
    struct fs_type *type = vfs_get_fs_type(f_type);
    assert(type != NULL, "Failed to get the FS type of the root FS!");
    struct super_block *root_sb = type->ops->mount_root(dev);
    assert(root_sb != NULL, "Failed to get the root FS superblock!");
    assert(root_sb->valid, "Root FS superblock is not valid!");
    assert(root_sb->root != NULL, "Root FS has a NULL root entry!");
    vfs_root_dentry = root_sb->root;
}

// Get the root dentry of the mounted filesystem
// This function will be called when encountering a mount point
int vfs_mounted_root(struct vfs_mount_point *mp, struct vfs_dentry **dentry) {
    if (mp == NULL || dentry == NULL) {
        return -1; // Invalid parameters
    }
    if (mp->sb == NULL) {
        return -1;
    }
    if (!mp->sb->valid || mp->sb->frozen) {
        return -1; // Superblock is not valid or frozen
    }
    if (mp->sb->root == NULL) {
        return -1; // No root dentry for the mounted filesystem
    }
    *dentry = mp->sb->root; // Copy the root dentry
    return 0; // Success
}

// Look up for a directory entry under a dentry of a directory.
// It will first try to look up the cached children dentry list.
// Will call dentry->ops->d_lookup in the following cases:
// - Target dentry is not found in the cached children dentry list
// - Target dentry is found, but its not valid
// - Target dentry is found and create is true, but it's marked as deleted
// Will return NULL in the following cases:
// - Target dentry is not found in both the cached children dentry list
//   and on the disk, and create is false
// - Found a dentry marked as deleted, but create is false
// - The dentry is linked to a symbolic link
// - The dentry is a mount point
// - dentry->ops->d_lookup returns NULL
// It may return deleted, invalid, or mounted dentry.
// The refcount of the returned dentry will increase by 1
int vfs_dlookup(struct vfs_dentry *dentry, const char *name, 
                size_t len, bool create, struct vfs_dentry **ret_dentry) {
    if (dentry == NULL || name == NULL || len == 0 || ret_dentry == NULL) {
        return -1;
    }
    if (!dentry->valid && vfs_d_validate(dentry) != 0) {
        return -1;
    }
    if (!dentry->valid) {
        return -1;
    }
    if (dentry->mounted) {
        return -1;
    }
    struct vfs_dentry *pos = NULL;
    struct vfs_dentry *tmp = NULL;
    list_foreach_node_safe(&dentry->children, pos, tmp, sibling) {
        if (vfs_d_compare(pos, name, len) == 0) {
            break;
        }
    }
    if (pos == NULL || pos->deleted) {
        if (!create) {
            return -1;
        }
        pos = vfs_d_lookup(dentry, name, len, create);
        if (pos == NULL) {
            return -1;
        }
    }
    pos->ref_count++;
    *ret_dentry = pos;
    return 0;
}

// Decrease the reference count of a dentry and all its ancestors
// until the root dentry of its file system.
// Will try to free the resources is all the offspring dentry hit 0 ref count.
// Return 0 when success, otherwise error.
int vfs_dentry_put(struct vfs_dentry *dentry) {
    if (dentry == NULL) {
        return -1;
    }
    struct vfs_dentry *pos = dentry;
    while (pos != NULL) {
        struct vfs_dentry *parent = pos->parent;
        pos->ref_count--;
        assert(pos->ref_count >= 0, "vfs_dentry_put: ref_count is negative");
        if (pos->ref_count == 0) {
            vfs_d_invalidate(pos);
        }
        if (!pos->valid) {
            assert(pos->ops->d_destroy(pos) == 0, "vfs_dentry_put: dentry destroy failed");
        }
        pos = parent;
    }
    return 0; // Success
}

// Get the super block of the file system of the dentry
int vfs_dentry_sb(struct vfs_dentry *dentry, struct super_block **ret_sb) {
    if (dentry == NULL || ret_sb == NULL) {
        return -1;
    }
    if (dentry->sb) {
        *ret_sb = dentry->sb;
        return 0;
    }
    return -1;
}

// Parse flags string from fopen
int fcntl_flags_from_string(const char *flags) {
    if (flags == NULL) {
        return -1; // Invalid flags
    }

    bool a = false, r = false, w = false, plus = false;
    for (int i = 0; i < 3 && flags[i] != '\0'; i++) {
        switch (flags[i]) {
            case 'r':
                r = true;
                break;
            case 'w':
                w = true;
                break;
            case 'a':
                a = true;
                break;
            case '+':
                plus = true;
                break;
            default:
                return -1;
        }
    }
    if (r && !w && !a) {
        if (plus) {
            return O_RDWR; // Read and write
        } else {
            return O_RDONLY; // Read only
        }
    } else if (w && !r && !a) {
        if (plus) {
            return O_RDWR | O_CREATE | O_TRUNC; // Read and write
        } else {
            return O_WRONLY | O_CREATE | O_TRUNC; // Write only
        }
    } else if (a && !r && !w) {
        if (plus) {
            return O_RDWR | O_CREATE | O_APPEND; // Read and write
        } else {
            return O_WRONLY | O_CREATE | O_APPEND; // Write only
        }
    }

    return -1;
}

int vfs_fopen(struct vfs_file *file, const char *path, const char *flags) {
    if (file == NULL || path == NULL || flags == NULL) {
        return -1; // Invalid parameters
    }
    int flags_int = fcntl_flags_from_string(flags);
    if (flags_int < 0) {
        return -1; // Invalid flags
    }
    return vfs_fopen2(file, path, flags_int);
}

int vfs_fopen2(struct vfs_file *file, const char *path, int flags) {
    if (file == NULL || path == NULL) {
        return -1; // Invalid parameters
    }


    return -1; // @TODO: Implement vfs_fopen2
}
