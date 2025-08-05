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
struct vfs_inode *vfs_root_inode;       // Root inode for the virtual filesystem
slab_cache_t vfs_fs_type_cache;         // Cache for filesystem types

// locking order:
// 1. fd lock
// 2. vfs_lock
// 3. mount point inode lock
// 4. superblock lock
// 5. inode lock

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
    vfs_root_inode = NULL; // Initialize root inode to NULL
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

int vfs_mount(struct vfs_inode *inode, dev_t dev) {
    __vfs_assert_holding();
    panic("vfs_mount: not implemented yet");
    // @TODO: check if inode is a valid mount point(directories only)
    // @TODO: check if inode is not already mounted
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
    vfs_root_inode = root_sb->root;
}

int vfs_sb_refinc(struct super_block *sb) {
    if (sb == NULL) {
        return -1; // Nothing to increment
    }
    assert(vfs_holdingfs(sb) == 1, "vfs_sb_refinc: filesystem is not held");
    sb->ref++;
    return sb->ref;
}

int vfs_sb_refdec(struct super_block *sb) {
    if (sb == NULL) {
        return -1; // Nothing to decrement
    }
    assert(vfs_holdingfs(sb) == 1, "vfs_sb_refdec: filesystem is not held");
    assert(sb->ref > 0, "vfs_sb_refdec: superblock reference count is already zero");
    sb->ref--;
    return sb->ref;
}

// Get the root inode of the mounted filesystem, and lock the root inode
// Default mount point is that of the root filesystem.
// This function will be called when encountering a mount point
int vfs_mounted_root(struct vfs_mount_point *mp, struct vfs_inode **reti) {
    if (reti == NULL) {
        return -1; // Invalid parameters
    }
    if (mp == NULL) {
        if (vfs_root_inode != NULL) {
            vfs_lockfs(vfs_root_inode->sb);
            vfs_ilock(vfs_root_inode);
            if ((*reti = vfs_idup(vfs_root_inode)) == NULL) {
                vfs_iunlock(vfs_root_inode);
                vfs_unlockfs(vfs_root_inode->sb);
                return -1;
            }
            vfs_unlockfs(vfs_root_inode->sb);
            return 0; // Success
        }
        return -1; // No root inode available
    }
    if (mp->sb == NULL) {
        return -1;
    }
    vfs_lockfs(mp->sb);
    if (!mp->sb->valid || mp->sb->frozen) {
        vfs_unlockfs(mp->sb);
        return -1; // Superblock is not valid or frozen
    }
    if (mp->sb->root == NULL) {
        vfs_unlockfs(mp->sb);
        return -1; // No root inode for the mounted filesystem
    }
    vfs_ilock(mp->sb->root);
    if ((*reti = vfs_idup(mp->sb->root)) == NULL) {
        vfs_iunlock(mp->sb->root);
        vfs_unlockfs(mp->sb);
        return -1;
    }
    vfs_unlockfs(mp->sb);
    return 0; // Success
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
    // @TODO: symlink support
    if (vfs_namex(path, strlen(path), &inode, NULL, 0) != 0) {
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

int vfs_namex(const char *path, size_t len, struct vfs_inode **reti, 
              struct vfs_inode *base, int max_follow)
{
    if (path == NULL || len == 0) {
        return -1; // Invalid parameters
    }
    if (reti == NULL) {
        return -1; // Invalid parameters
    }

    const char *to_lookup = path;
    ssize_t to_lookup_len = len;
    struct vfs_inode *top_inode = NULL;
    if (to_lookup[0] == '/') {
        // Omit base inode if the path is absolute
        if (vfs_mounted_root(NULL, &top_inode) != 0) {
            return -1; // Failed to get the root inode
        }
        base = top_inode; // Use the root inode as base
        // Omit the leading '/' from the lookup
        to_lookup_len--;
        to_lookup++;
    } else {
        if (base == NULL) {
            if (myproc()->_cwd == NULL) {
                return -1;
            }
            vfs_ilock(myproc()->_cwd);
            top_inode = vfs_idup(myproc()->_cwd);
            if (top_inode == NULL) {
                vfs_iunlock(myproc()->_cwd);
                return -1; // Failed to get the current working directory inode
            }
        } else {
            vfs_ilock(base);
            top_inode = vfs_idup(base);
            if (top_inode == NULL) {
                vfs_iunlock(base);
                return -1; // Failed to duplicate the base inode
            }
        }
        if (vfs_validate_inode(top_inode) != 0) {
            vfs_iput(top_inode);
            vfs_iunlock(top_inode);
            return -1; // Invalid top inode
        }
        if (top_inode->mp != NULL) {
            struct vfs_inode *tmp = NULL;
            int tmp_ret = vfs_mounted_root(top_inode->mp, &tmp);
            vfs_iput(top_inode);
            vfs_iunlock(top_inode);
            if (tmp_ret != 0) {
                return -1; // Failed to get the root inode
            }
        }
    }

    while (to_lookup_len > 0) {
        ssize_t top_len = __pathname_get_toplayer(path, len);
        struct vfs_inode *next_inode = NULL;
        if (top_len < 0) {
            return -1; // Invalid path
        } else if (top_len == 0) {
            if (to_lookup_len == 1) {
                // If path ends with '/', it must be a directory
                if (top_inode->type != I_DIR) {
                    vfs_iput(top_inode);
                    vfs_iunlock(top_inode);
                    return -1; // Not a directory
                }
                *reti = top_inode; // Return the current inode
                return 0; // Success
            }
            // skip extra '/'
            to_lookup += 1;
            to_lookup_len -= 1;
            continue;
        }

        next_inode = vfs_dlookup(top_inode, to_lookup, top_len);
        if (next_inode == NULL) {
            vfs_iput(top_inode);
            vfs_iunlock(top_inode);
            return -1; // Failed to lookup the next inode
        }

        vfs_iput(top_inode);
        vfs_iunlock(top_inode);
        top_inode = next_inode;
        if (top_inode->mp != NULL) {
            int tmp_ret = vfs_mounted_root(top_inode->mp, &next_inode);
            vfs_iput(top_inode);
            vfs_iunlock(top_inode);
            if (tmp_ret != 0) {
                return -1; // Failed to get the root inode
            }
            top_inode = next_inode;
            continue; // Continue with the next layer
        } else if (top_inode->type == I_SYMLINK) {
            if (max_follow <= 0) {
                vfs_iput(top_inode);
                vfs_iunlock(top_inode);
                return -1; // Too many symlinks
            }
            char *symlink_target = kmm_alloc(NAME_MAX + 1);
            if (symlink_target == NULL) {
                vfs_iput(top_inode);
                vfs_iunlock(top_inode);
                return -1; // Memory allocation failed
            }
            int ret = vfs_ireadlink(top_inode, symlink_target, NAME_MAX + 1);
            vfs_iput(top_inode);
            vfs_iunlock(top_inode);
            if (ret != 0) {
                kmm_free(symlink_target);
                return -1; // Failed to read symlink
            }
            // Follow the symlink
            to_lookup = symlink_target;
            to_lookup_len = strlen(symlink_target);
            ret = vfs_namex(to_lookup, to_lookup_len, reti, NULL, max_follow - 1);
            kmm_free(symlink_target);
            return ret; // Return the result of following the symlink
        }

        to_lookup += top_len + 1;
        to_lookup_len -= top_len + 1;
    }

    if (reti != NULL) {
        *reti = top_inode; // Return the found inode
        return 0;
    }

    return -1; // @TODO: Implement vfs_namex
}

