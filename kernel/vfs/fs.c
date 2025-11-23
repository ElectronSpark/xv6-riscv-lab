#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "bits.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "mutex_types.h"
#include "vfs/fs.h"
#include "list.h"
#include "hlist.h"
#include "slab.h"
#include "errno.h"

static struct spinlock __fs_type_spinlock = { 0 };
static list_node_t vfs_fs_types = { 0 };
static uint16 vfs_fs_type_count = 0;


#define MAX_FS_TYPES 256

/******************************************************************************
 * Private functions
 *****************************************************************************/
static void __vfs_register_fs_type_locked(struct vfs_fs_type *fs_type) {
    list_add_tail(&vfs_fs_types, &fs_type->list_entry);
    fs_type->registered = 1;
    vfs_fs_type_count++;
    assert(vfs_fs_type_count <= MAX_FS_TYPES, "Exceeded maximum filesystem types");
}

static void __vfs_unregister_fs_type_locked(struct vfs_fs_type *fs_type) {
    list_remove(&fs_type->list_entry);
    fs_type->registered = 0;
    vfs_fs_type_count--;
    assert(vfs_fs_type_count <= MAX_FS_TYPES, "Filesystem types count underflow");
}

static struct vfs_fs_type* __vfs_get_fs_type_by_name(const char *name) {
    struct vfs_fs_type *pos, *tmp;
    list_foreach_node_safe(&vfs_fs_types, pos, tmp, list_entry) {
        if (strcmp(pos->name, name) == 0) {
            __vfs_fs_type_unlock();
            return pos; // Found
        }
    }
    return NULL; // Not found
}


 /******************************************************************************
 * Private functions
 *****************************************************************************/
// Initialize VFS subsystem
void vfs_init(void) {
    list_entry_init(&vfs_fs_types);
    spin_init(&__fs_type_spinlock, "vfs_fs_types_lock");
    vfs_fs_type_count = 0;
    __vfs_rooti_init(); // Initialize root inode
}

int vfs_register_fs_type(struct vfs_fs_type *fs_type) {
    // Make sure the fs_type is not registered and is empty
    if (fs_type == NULL || fs_type->name == NULL || fs_type->ops == NULL) {
        return -EINVAL; // Invalid arguments
    }
    if (fs_type->ops->mount == NULL || fs_type->ops->unmount == NULL) {
        return -EINVAL; // Invalid filesystem operations
    }
    if (fs_type->sb_count != 0) {
        return -EINVAL; // sb_count must be zero for a new fs_type
    }
    if (fs_type->registered) {
        return -EALREADY; // Filesystem type already registered
    }
    list_entry_init(&fs_type->list_entry);
    vfs_fs_type_lock();
    if (vfs_fs_type_count >= MAX_FS_TYPES) {
        vfs_fs_type_unlock();
        return -ENOSPC; // No space for more filesystem types
    }
    struct vfs_fs_type *existing = __vfs_get_fs_type_by_name(fs_type->name);
    if (existing != NULL) {
        vfs_fs_type_unlock();
        return -EEXIST; // Filesystem type with the same name already exists
    }
    __vfs_register_fs_type_locked(fs_type);
    vfs_fs_type_unlock();
    return 0;
}

int vfs_unregister_fs_type(const char *name) {
    if (name == NULL) {
        return -EINVAL; // Invalid argument
    }
    vfs_fs_type_lock();
    struct vfs_fs_type *pos = __vfs_get_fs_type_by_name(name);
    if (pos != NULL) {
        if (pos->sb_count > 0) {
            vfs_fs_type_unlock();
            return -EBUSY; // Cannot unregister fs_type with mounted superblocks
        }
        __vfs_unregister_fs_type_locked(pos);
        vfs_fs_type_unlock();
        return 0; // Successfully unregistered
    }
    vfs_fs_type_unlock();
    return -ENOENT; // Filesystem type not found
}

void vfs_fs_type_lock(void) {
    spin_lock(&__fs_type_spinlock);
}

void vfs_fs_type_unlock(void) {
    spin_unlock(&__fs_type_spinlock);
}

