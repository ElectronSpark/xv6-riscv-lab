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
// Default mount point is that of the root filesystem.
// This function will be called when encountering a mount point
// The refcount of the returned dentry will increase by 1
int vfs_mounted_root(struct vfs_mount_point *mp, struct vfs_dentry **ret_dentry) {
    if (ret_dentry == NULL) {
        return -1; // Invalid parameters
    }
    if (mp == NULL) {
        if (vfs_root_dentry != NULL) {
            *ret_dentry = vfs_root_dentry; // Use the global root dentry
            return 0; // Success
        }
        return -1; // No root dentry available
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
    *ret_dentry = mp->sb->root; // Copy the root dentry
    (*ret_dentry)->ref_count++; // Increase the refcount
    return 0; // Success
}

// Look up for a directory entry under a dentry of a directory.
// It will first try to look up the cached children dentry list.
// Will call dentry->ops->d_lookup in the following cases:
// - Target dentry is not found in the cached children dentry list
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
    if (vfs_d_validate(dentry) != 0) {
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

// Decrease the reference count of a dentry and all its ancestors until the given base dentry.
// until the root dentry of its file system.
// Will try to free the resources is all the offspring dentry hit 0 ref count.
// Return 0 when success, otherwise error.
int vfs_dentry_put(struct vfs_dentry *dentry, struct vfs_dentry *base, bool including_base) {
    if (dentry == NULL) {
        return -1;
    }
    if (base == NULL) {
        // Default base is the root dentry of the current file system
        base = dentry->root;
        assert(base != NULL, "vfs_dentry_put: root dentry is NULL");
        if (base == dentry) {
            // root dentry is not allowed to be put
            return -1;
        }
    }
    struct vfs_dentry *pos = dentry;
    while (1) {
        struct vfs_dentry *parent = pos->parent;
        pos->ref_count--;
        assert(pos->ref_count >= 0, "vfs_dentry_put: ref_count is negative");
        if (pos->ref_count == 0) {
            vfs_d_invalidate(pos);
        }
        if (!pos->valid) {
            assert(pos->ops->d_destroy(pos) == 0, "vfs_dentry_put: dentry destroy failed");
        }
        if (!including_base && parent == base) {
            // If we are not including the base dentry, stop here
            break;
        } else if (pos == base) {
            // If we reach the base dentry, stop here
            break;
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
            return O_RDWR | O_CREAT | O_TRUNC; // Read and write
        } else {
            return O_WRONLY | O_CREAT | O_TRUNC; // Write only
        }
    } else if (a && !r && !w) {
        if (plus) {
            return O_RDWR | O_CREAT | O_APPEND; // Read and write
        } else {
            return O_WRONLY | O_CREAT | O_APPEND; // Write only
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

    struct vfs_inode *inode = NULL;
    if (vfs_namex(path, strlen(path), NULL, &inode, NULL, 0) != 0) {
        return -1; // Failed to resolve the path
    }

    assert(inode->sb != NULL, "vfs_fopen2: inode's superblock is NULL");


    return -1; // @TODO: Implement vfs_fopen2
}

// Find the position of the first '/' or the end of the string in the path.
static int __pathname_get_toplayer(const char *path, size_t len) {
    if (path == NULL || len == 0) {
        return -1;
    }
    if (path[0] == '/') {
        return 0; // Absolute path, top layer is the root
    }
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/' || path[i] == '\0') {
            return i; // Found the first '/' or end of string
        }
    }
    return len; // No '/' found, the whole path is a single layer
}

int vfs_namex(const char *path, size_t len, struct vfs_dentry **retd, 
              struct vfs_inode **reti, struct vfs_dentry *base, 
              int max_follow)
{
    if (path == NULL || len == 0) {
        return -1; // Invalid parameters
    }
    if (retd == NULL && reti == NULL) {
        return -1; // Invalid parameters
    }

    const char *to_lookup = path;
    ssize_t to_lookup_len = len;
    struct vfs_dentry *original_base = base;
    struct vfs_dentry *top_dentry = base;
    if (to_lookup[0] == '/') {
        // Omit base dentry if the path is absolute
        if (vfs_mounted_root(NULL, &top_dentry) != 0) {
            return -1; // Failed to get the root dentry
        }
        base = top_dentry; // Use the root dentry as base
        // Omit the leading '/' from the lookup
        to_lookup_len--;
        to_lookup++;
    } else {
        if (base == NULL) {
            top_dentry = myproc()->_cwd;
            base = top_dentry; // Use current working directory as base
        } else {
            top_dentry = base;
        }
        if (vfs_d_validate(top_dentry) != 0) {
            return -1; // Invalid top dentry
        }
        if (top_dentry->mounted) {
            // To avoid invalidating a mount point here, we will not follow the mount point.
            return -1;
        }
    }

    while (to_lookup_len > 0) {
        ssize_t top_len = __pathname_get_toplayer(path, len);
        struct vfs_dentry *next_dentry = NULL;
        if (top_len < 0) {
            return -1; // Invalid path
        } else if (top_len == 0) {
            // skip extra '/'
            to_lookup += 1;
            to_lookup_len -= 1;
            continue;
        }
        if (vfs_dlookup(top_dentry, to_lookup, top_len, false, &next_dentry) != 0) {
            if (top_dentry->mounted) {
                if (vfs_mounted_root(top_dentry->mount, &next_dentry) != 0) {
                    vfs_dentry_put(top_dentry, base, original_base == NULL);
                    return -1; // Failed to get the root dentry
                } else {
                    vfs_dentry_put(top_dentry, base, original_base == NULL);
                    original_base = NULL;
                    base = next_dentry; // Use the root dentry of the mounted filesystem
                    top_dentry = next_dentry; // Update top dentry to the root of the mounted filesystem
                    continue; // Continue with the next layer
                }
            } else if (vfs_d_is_symlink(top_dentry)) {
                if (max_follow <= 0) {
                    vfs_dentry_put(top_dentry, base, original_base == NULL);
                    return -1; // Too many symlinks
                }
                struct vfs_inode *inode = vfs_d_inode(top_dentry);
                if (inode == NULL) {
                    vfs_dentry_put(top_dentry, base, original_base == NULL);
                    return -1; // Failed to get the inode
                }
                assert(inode->type == I_SYMLINK, "vfs_namex: symlink dentry is not a symlink inode");
                char *symlink_target = kmm_alloc(NAME_MAX + 1);
                if (symlink_target == NULL) {
                    vfs_dentry_put(top_dentry, base, original_base == NULL);
                    return -1; // Memory allocation failed
                }
                int ret = vfs_ireadlink(inode, symlink_target, NAME_MAX + 1);
                vfs_dentry_put(top_dentry, base, original_base == NULL);
                if (ret != 0) {
                    kmm_free(symlink_target);
                    return -1; // Failed to read symlink
                }
                // Follow the symlink
                to_lookup = symlink_target;
                to_lookup_len = strlen(symlink_target);
                ret = vfs_namex(to_lookup, to_lookup_len, retd, reti, NULL, max_follow - 1);
                kmm_free(symlink_target);
                return ret; // Return the result of following the symlink
            }
        }

        top_dentry = next_dentry;
        to_lookup += top_len + 1;
        to_lookup_len -= top_len + 1;
    }

    if (reti != NULL) {
        *reti = vfs_d_inode(top_dentry);
    }
    if (retd != NULL) {
        *retd = top_dentry;
    } else {
        // If retd is NULL, we need to put the dentry to avoid memory leak
        vfs_dentry_put(top_dentry, base, original_base == NULL);
    }

    return -1; // @TODO: Implement vfs_namex
}
