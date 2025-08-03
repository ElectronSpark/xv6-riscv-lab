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
list_node_t vfs_fs_types;       // List of registered filesystem types
struct dentry *vfs_root_dentry; // Root dentry for the virtual filesystem
slab_cache_t vfs_fs_type_cache; // Cache for filesystem types


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

void vfs_mount_root(dev_t dev) {
    __vfs_assert_holding();
    // @TODO:
    panic("vfs_mount_root: not implemented yet");
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
