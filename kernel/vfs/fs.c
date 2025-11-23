#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "bits.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "mutex_types.h"
#include "rwlock.h"
#include "vfs/fs.h"
#include "list.h"
#include "hlist.h"
#include "slab.h"
#include "errno.h"

// Locking order
// 1. vfs_fs_types_lock
// 2. vfs_superblock lock
// 3. vfs_inode lock
//
// When multiple locks of the same type are needed, always acquire in hierarchical order
// For example:
// 1. acquire parent superblock lock before child superblock lock
// 2. acquire directory inode lock before child inode lock
// When cross filesystem, need to release inode lock before acquiring another filesystem's inode lock.
// Acquiring inode lock must be done after acquiring any superblock lock, including mounted superblock lock.

static slab_cache_t vfs_superblock_cache = { 0 };
static struct spinlock __fs_type_spinlock = { 0 };
mutex_t vfs_mount_lock = { 0 };
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

static struct vfs_fs_type* __vfs_get_name_fstype_locked(const char *name) {
    struct vfs_fs_type *pos, *tmp;
    list_foreach_node_safe(&vfs_fs_types, pos, tmp, list_entry) {
        if (strcmp(pos->name, name) == 0) {
            __vfs_fs_type_unlock();
            return pos; // Found
        }
    }
    return NULL; // Not found
}

// Superblock hash callback functions
static uint64 __vfs_superblock_inode_hash_func(void *node) {
    struct vfs_inode *inode = (struct vfs_inode *)node;
    return hlist_hash_uint64(inode->ino);
}

static int __vfs_superblock_inode_id_cmp_func(hlist_t *hlist, void *node, void *key) {
    (void)hlist;
    struct vfs_inode *inode_node = (struct vfs_inode *)node;
    struct vfs_inode *inode_key = (struct vfs_inode *)key;
    if (inode_node->ino > inode_key->ino) {
        return 1;
    } else if (inode_node->ino < inode_key->ino) {
        return -1;
    } else {
        return 0;
    }
}

static void *__vfs_superblock_inode_get_node_func(hlist_entry_t *entry) {
    if (entry == NULL) {
        return NULL;
    }
    return container_of(entry, struct vfs_inode, hash_entry);
}

static hlist_entry_t *__vfs_superblock_inode_get_entry_func(void *node) {
    if (node == NULL) {
        return NULL;
    }
    struct vfs_inode *inode = (struct vfs_inode *)node;
    return &inode->hash_entry;
}

static struct hlist_func_struct __vfs_superblock_inode_hlist_funcs = {
    .hash = __vfs_superblock_inode_hash_func,
    .cmp_node = __vfs_superblock_inode_id_cmp_func,
    .get_node = __vfs_superblock_inode_get_node_func,
    .get_entry = __vfs_superblock_inode_get_entry_func,
};

// Superblock structure helpers

static void __vfs_init_superblock_structure(struct vfs_superblock *sb, struct vfs_fs_type *fs_type) {
    list_entry_init(&sb->siblings);
    hlist_init(&sb->inodes, VFS_SUPERBLOCK_HASH_BUCKETS, &__vfs_superblock_inode_hlist_funcs);
    list_entry_init(&sb->inodes);
    sb->fs_type = fs_type;
    rwlock_init(&sb->lock, RWLOCK_PRIO_READ, "vfs_superblock_lock");
}

static bool __vfs_superblock_ops_valid(struct vfs_superblock *sb) {
    if (sb->ops == NULL) {
        return false;
    }
    if (sb->ops->alloc_inode == NULL || sb->ops->get_inode == NULL
        || sb->ops->sync_fs == NULL || sb->ops->unmount_begin == NULL) {
        return false;
    }
    return true;
}

// After mount_begin, mount will use this function to verify the returned superblock
static bool __vfs_init_superblock_valid(struct vfs_superblock *sb) {
    if (sb == NULL) {
        return false;
    }
    if (sb->ops)
    if (sb->dirty != 0 || sb->valid != 0) {
        return false;
    }
    if (sb->device == NULL) {
        return false;
    }
    if (sb->mountpoint != NULL || sb->parent_sb != NULL) {
        // At this point, the superblock has not been mounted yet
        return false;
    }
    return __vfs_superblock_ops_valid(sb);
}

static void __vfs_attach_superblock_to_fstype(struct vfs_superblock *sb) {
    list_node_push_back(&sb->fs_type->superblocks, sb, siblings);
    sb->fs_type->sb_count++;
    assert(sb->fs_type->sb_count > 0, "Filesystem type superblock count overflow");
}

static void __vfs_detach_superblock_from_fstype(struct vfs_superblock *sb) {
    list_node_detach(sb, siblings);
    sb->fs_type->sb_count--;
    assert(sb->fs_type->sb_count >= 0, "Filesystem type superblock count underflow");
}

// Set the mountpoint inode of a superblock
// Caller needs to ensure incrementing the ref count of mountpoint inode before calling this function
static void __vfs_set_mountpoint(struct vfs_superblock *sb, struct vfs_inode *mountpoint) {
    assert(rwlock_is_write_holding(mountpoint->sb) != NULL, "Mountpoint inode's superblock lock must be write held to set mountpoint");
    assert(rwlock_is_write_holding(&sb->lock), "Superblock lock must be write held to set mountpoint");
    assert(holding_mutex(&mountpoint->lock), "Mountpoint inode lock must be held to set mountpoint");
    assert(mountpoint->type == VFS_I_TYPE_MNT, "Mountpoint inode type is not MNT");
    assert(sb->mountpoint == NULL, "Superblock mountpoint is already set");
    sb->mountpoint = mountpoint;
    sb->parent_sb = mountpoint->sb;
    mountpoint->mnt_sb = sb;
    mountpoint->sb->mount_count++;
    assert(mountpoint->sb->mount_count > 0, "Mountpoint superblock mount count overflow");
}

// Clear the mountpoint inode of a superblock
// Caller needs to ensure decrementing the ref count of mountpoint inode after calling this function
static void __vfs_clear_mountpoint(struct vfs_superblock *sb) {
    struct vfs_inode *mountpoint = sb->mountpoint;
    assert(rwlock_is_write_holding(&sb->lock), "Superblock lock must be write held to clear mountpoint");
    assert(mountpoint != NULL, "Superblock mountpoint is already NULL");
    assert(holding_mutex(&mountpoint->lock), "Mountpoint inode lock must be held to clear mountpoint");
    assert(mountpoint->type == VFS_I_TYPE_MNT, "Mountpoint inode type is not MNT");
    assert(mountpoint->mnt_sb == sb, "Mountpoint inode's mounted superblock does not match");
    mountpoint->sb->mount_count--;
    assert(mountpoint->sb->mount_count >= 0, "Mountpoint superblock mount count underflow");
    mountpoint->mnt_sb = NULL;
    mountpoint->type = VFS_I_TYPE_DIR;
    sb->mountpoint = NULL;
    sb->parent_sb = NULL;
}

 /******************************************************************************
 * Files System Type Public APIs
 *****************************************************************************/
// Initialize VFS subsystem
void vfs_init(void) {
    list_entry_init(&vfs_fs_types);
    spin_init(&__fs_type_spinlock, "vfs_fs_types_lock");
    mutex_init(&vfs_mount_lock, "vfs_mount_lock");
    int ret = slab_cache_init(&vfs_superblock_cache, "vfs_superblock_cache",
                              sizeof(struct vfs_superblock), 0);
    assert(ret == 0, "Failed to initialize vfs_superblock_cache slab cache, errno=%d", ret);
    vfs_fs_type_count = 0;
    __vfs_rooti_init(); // Initialize root inode
}

int vfs_register_fs_type(struct vfs_fs_type *fs_type) {
    // Make sure the fs_type is not registered and is empty
    if (fs_type == NULL || fs_type->name == NULL || fs_type->ops == NULL) {
        return -EINVAL; // Invalid arguments
    }
    if (fs_type->ops->mount == NULL || fs_type->ops->unmount == NULL
        || fs_type->ops->mount_begin == NULL) {
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
    struct vfs_fs_type *existing = __vfs_get_name_fstype_locked(fs_type->name);
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
    struct vfs_fs_type *pos = __vfs_get_name_fstype_locked(name);
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

 /******************************************************************************
 * Superblock Public APIs
 *****************************************************************************/
 int vfs_mount(const char *type, struct vfs_inode *mountpoint,
              struct vfs_inode *device, int flags, const char *data) {
    struct vfs_fs_type *fs_type = NULL;
    struct vfs_superblock *sb = NULL;
    bool sb_attached = false;
    int ret_val = 0;

    if (type == NULL || mountpoint == NULL) {
        return -EINVAL; // Invalid arguments
    }
    vfs_ilock(mountpoint);
    if (mountpoint->type != VFS_I_TYPE_DIR) {
        vfs_iunlock(mountpoint);
        return -EINVAL; // Mountpoint must be a directory
    }
    mountpoint->type = VFS_I_TYPE_MNT; // Temporarily mark as MNT to prevent nested mounts
    vfs_iunlock(mountpoint);
    
    vfs_fs_type_lock();
    // @TODO: Need to move some mount logic out of fs type lock
//     fs_type = __vfs_get_fs_type_by_name(type);
//     if (fs_type == NULL || !fs_type->registered) {
//         ret_val = -ENOENT; // Filesystem type not found
//         vfs_fs_type_unlock();
//         goto ret_unmount;
//     }
//     ret_val = fs_type->ops->mount_begin(mountpoint, device, flags, data, &sb);
//     if (ret_val != 0) {
//         vfs_fs_type_unlock();
//         goto ret_unmount;
//     }
//     if (!__vfs_init_superblock_valid(sb)) {
//         vfs_fs_type_unlock();
//         ret_val = -EINVAL; // Invalid superblock returned by mount_begin
//         goto ret_unmount;
//     }
//     // Add the superblock to the fs_type's superblock list
//     __vfs_init_superblock_structure(sb, fs_type);
//     __vfs_attach_superblock_to_fstype(sb);
//     sb_attached = true;
//     // @TODO: Need to avoid sleeping while holding fs_type lock
//     vfs_superblock_wlock(sb);
//     vfs_fs_type_unlock();
//     ret_val = fs_type->ops->mount(mountpoint, device, flags, data);
//     vfs_superblock_unlock(sb);
//     if (ret_val != 0) {
//         goto ret_unmount;
//     }
//     vfs_superblock_wlock(mountpoint->sb);
//     vfs_superblock_wlock(sb);
//     vfs_ilock(mountpoint);
//     __vfs_set_mountpoint(sb, mountpoint);
//     vfs_iunlock(mountpoint);
//     vfs_superblock_unlock(sb);
//     vfs_superblock_unlock(mountpoint->sb);
// ret_unmount:
//     if (ret_val != 0) {
//         if (sb != NULL) {
//             if (sb_attached) {
//                 vfs_superblock_wlock(sb);
//                 vfs_fs_type_lock();
//                 __vfs_detach_superblock_from_fstype(sb);
//                 vfs_fs_type_unlock();
//                 vfs_superblock_unlock(sb);
//             }
//             if (fs_type != NULL) {
//                 fs_type->ops->unmount(sb); // Clean up filesystem state
//             }
//         }
//         vfs_ilock(mountpoint);
//         mountpoint->type = VFS_I_TYPE_DIR;
//         vfs_iunlock(mountpoint);
//     }
//     return ret_val;
}

int vfs_unmount(struct vfs_inode *mountpoint) {
 

}

int vfs_mnt_sb_lookup(struct vfs_inode *mountpoint, struct vfs_superblock **ret_sb);
int vfs_mnt_rooti_lookup(struct vfs_inode *mountpoint, struct vfs_inode **ret_rooti);
int vfs_get_sb_mnt(struct vfs_superblock *sb, struct vfs_inode **ret_mountpoint);
int vfs_get_sb_rooti(struct vfs_superblock *sb, struct vfs_inode **ret_rooti);
int vfs_get_rooti_sb(struct vfs_inode **ret_rooti, struct vfs_superblock **ret_sb);
int vfs_get_rooti_mnt(struct vfs_inode **ret_rooti, struct vfs_inode **ret_mountpoint);

// Get the mountpoint inode of a superblock
// It will try to increment the ref count of the mountpoint inode before returning
// Caller should call vfs_iput() on the returned inode when done
int vfs_get_mountpoint(struct vfs_superblock *sb, struct vfs_inode **ret_mountpoint) {
    if (sb == NULL || ret_mountpoint == NULL) {
        return -EINVAL; // Invalid arguments
    }
    vfs_superblock_rlock(sb);
    if (sb->mountpoint == NULL) {
        vfs_superblock_unlock(sb);
        return -ENODEV; // Superblock is not mounted
    }
    struct vfs_inode *ret_mountpoint = sb->mountpoint;
    int ret = vfs_idup(ret_mountpoint); // Increase ref count
    vfs_superblock_unlock(sb);
    if (ret != 0) {
        ret_mountpoint = NULL;
    }
    *ret_mountpoint = ret_mountpoint;
    return ret;
}

void vfs_superblock_rlock(struct vfs_superblock *sb) {
    if (sb) {
        rwlock_acquire_read(&sb->lock);
    }
}

void vfs_superblock_wlock(struct vfs_superblock *sb) {
    if (sb) {
        rwlock_acquire_write(&sb->lock);
    }
}

void vfs_superblock_unlock(struct vfs_superblock *sb) {
    if (sb) {
        rwlock_unlock(&sb->lock);
    }
}

int vfs_sync_superblock(struct vfs_superblock *sb, int wait);
void vfs_unmount_begin(struct vfs_superblock *sb);
