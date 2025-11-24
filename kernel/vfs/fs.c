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

static slab_cache_t vfs_fs_type_cache = { 0 };
static slab_cache_t vfs_superblock_cache = { 0 };
static struct spinlock __fs_type_spinlock = { 0 };
static list_node_t vfs_fs_types = { 0 };
static uint16 vfs_fs_type_count = 0;


#define MAX_FS_TYPES 256

/******************************************************************************
 * Private functions
 *****************************************************************************/
static void __vfs_register_fs_type_locked(struct vfs_fs_type *fs_type) {
    list_node_push(&vfs_fs_types, fs_type, list_entry);
    fs_type->registered = 1;
    vfs_fs_type_count++;
    assert(vfs_fs_type_count <= MAX_FS_TYPES, "Exceeded maximum filesystem types");
}

static void __vfs_unregister_fs_type_locked(struct vfs_fs_type *fs_type) {
    list_node_detach(fs_type, list_entry);
    fs_type->registered = 0;
    vfs_fs_type_count--;
    assert(vfs_fs_type_count <= MAX_FS_TYPES, "Filesystem types count underflow");
}

static struct vfs_fs_type* __vfs_get_fs_type_locked(const char *name) {
    struct vfs_fs_type *pos, *tmp;
    list_foreach_node_safe(&vfs_fs_types, pos, tmp, list_entry) {
        if (strcmp(pos->name, name) == 0) {
            vfs_fs_type_unlock();
            return pos; // Found
        }
    }
    return NULL; // Not found
}

// File System Type Kobject callbacks
static void __vfs_fs_type_kobj_release(struct kobject *kobj) {
    struct vfs_fs_type *fs_type = container_of(kobj, struct vfs_fs_type, kobj);
    slab_free(fs_type);
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
    if (sb->device != NULL) {
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

static int __vfs_turn_mountpoint(struct vfs_inode *mountpoint) {
    assert(vfs_superblock_wholding(mountpoint->sb), "Mountpoint inode's superblock lock must be write held to turn into mountpoint");
    assert(holding_mutex(&mountpoint->lock), "Mountpoint inode lock must be held to turn into mountpoint");
    if (mountpoint->type != VFS_I_TYPE_DIR) {
        return -ENOTDIR; // Mountpoint must be a directory
    }
    if (vfs_idup(mountpoint) < 0) {
        return -EIO; // Failed to increase ref count
    }
    mountpoint->type = VFS_I_TYPE_MNT;
    mountpoint->sb->mount_count++;
    assert(mountpoint->sb->mount_count > 0, "Mountpoint superblock mount count overflow");
    return 0;
}

// Set the mountpoint inode of a superblock
// Caller needs to ensure incrementing the ref count of mountpoint inode before calling this function
static void __vfs_set_mountpoint(struct vfs_superblock *sb, struct vfs_inode *mountpoint) {
    assert(rwlock_is_write_holding(&mountpoint->sb->lock), "Mountpoint inode's superblock lock must be write held to set mountpoint");
    assert(rwlock_is_write_holding(&sb->lock), "Superblock lock must be write held to set mountpoint");
    assert(holding_mutex(&mountpoint->lock), "Mountpoint inode lock must be held to set mountpoint");
    assert(mountpoint->type == VFS_I_TYPE_MNT, "Mountpoint inode type is not MNT");
    assert(sb->mountpoint == NULL, "Superblock mountpoint is already set");
    sb->mountpoint = mountpoint;
    sb->parent_sb = mountpoint->sb;
    mountpoint->mnt_sb = sb;
}

// Clear the mountpoint inode of a superblock
// Caller needs to ensure decrementing the ref count of mountpoint inode after calling this function
static void __vfs_clear_mountpoint(struct vfs_inode *mountpoint) {
    assert(rwlock_is_write_holding(&mountpoint->sb->lock), "Superblock lock must be write held to clear mountpoint");
    assert(mountpoint != NULL, "Superblock mountpoint is already NULL");
    assert(holding_mutex(&mountpoint->lock), "Mountpoint inode lock must be held to clear mountpoint");
    assert(mountpoint->type == VFS_I_TYPE_MNT, "Mountpoint inode type is not MNT");
    mountpoint->sb->mount_count--;
    assert(mountpoint->sb->mount_count >= 0, "Mountpoint superblock mount count underflow");
    mountpoint->mnt_sb = NULL;
    mountpoint->type = VFS_I_TYPE_DIR;
    vfs_iput(mountpoint);
}

 /******************************************************************************
 * Files System Type Public APIs
 *****************************************************************************/
// Initialize VFS subsystem
void vfs_init(void) {
    list_entry_init(&vfs_fs_types);
    spin_init(&__fs_type_spinlock, "vfs_fs_types_lock");
    int ret = slab_cache_init(&vfs_superblock_cache, "vfs_superblock_cache",
                              sizeof(struct vfs_superblock), 0);
    assert(ret == 0, "Failed to initialize vfs_superblock_cache slab cache, errno=%d", ret);
    ret = slab_cache_init(&vfs_fs_type_cache, "vfs_fs_type_cache",
                          sizeof(struct vfs_fs_type), 0);
    assert(ret == 0, "Failed to initialize vfs_fs_type_cache slab cache, errno=%d", ret);
    vfs_fs_type_count = 0;
    __vfs_rooti_init(); // Initialize root inode
}

struct vfs_fs_type *vfs_fs_type_allocate(void) {
    struct vfs_fs_type *fs_type = slab_alloc(&vfs_fs_type_cache);
    if (fs_type == NULL) {
        return NULL;
    }
    memset(fs_type, 0, sizeof(*fs_type));
    list_entry_init(&fs_type->list_entry);
    list_entry_init(&fs_type->superblocks);
    return fs_type;
}

int vfs_register_fs_type(struct vfs_fs_type *fs_type) {
    // Make sure the fs_type is not registered and is empty
    if (fs_type == NULL || fs_type->name == NULL || fs_type->ops == NULL) {
        return -EINVAL; // Invalid arguments
    }
    if (fs_type->ops->mount == NULL || fs_type->ops->free == NULL) {
        return -EINVAL; // Invalid filesystem operations
    }
    if (fs_type->sb_count != 0) {
        return -EINVAL; // sb_count must be zero for a new fs_type
    }
    if (fs_type->registered) {
        return -EALREADY; // Filesystem type already registered
    }
    fs_type->kobj.ops.release = __vfs_fs_type_kobj_release;
    fs_type->kobj.name = "fs_type";
    kobject_init(&fs_type->kobj);
    vfs_fs_type_lock();
    if (vfs_fs_type_count >= MAX_FS_TYPES) {
        vfs_fs_type_unlock();
        return -ENOSPC; // No space for more filesystem types
    }
    struct vfs_fs_type *existing = __vfs_get_fs_type_locked(fs_type->name);
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
    struct vfs_fs_type *pos = __vfs_get_fs_type_locked(name);
    if (pos != NULL) {
        // @ TODO: inform all mounted superblocks of this fs_type after unregistering
        __vfs_unregister_fs_type_locked(pos);
        kobject_put(&pos->kobj);
        vfs_fs_type_unlock();
        return 0; // Successfully unregistered
    }
    vfs_fs_type_unlock();
    return -ENOENT; // Filesystem type not found
}

void vfs_fs_type_lock(void) {
    spin_acquire(&__fs_type_spinlock);
}

void vfs_fs_type_unlock(void) {
    spin_release(&__fs_type_spinlock);
}

 /******************************************************************************
 * Superblock Public APIs
 *****************************************************************************/
 int vfs_mount(const char *type, struct vfs_inode *mountpoint,
              struct vfs_inode *device, int flags, const char *data) {
    struct vfs_fs_type *fs_type = NULL;
    struct vfs_superblock *sb = NULL;
    int ret_val = 0;

    if (type == NULL || mountpoint == NULL) {
        return -EINVAL; // Invalid arguments
    }

    if (mountpoint->sb == NULL) {
        return -EINVAL; // Mountpoint inode has no superblock
    }

    vfs_superblock_wlock(mountpoint->sb);
    vfs_ilock(mountpoint);
    ret_val = __vfs_turn_mountpoint(mountpoint);
    if (ret_val != 0) {
        vfs_iunlock(mountpoint);
        vfs_superblock_unlock(mountpoint->sb);
        return ret_val; // Failed to turn mountpoint
    }
    vfs_iunlock(mountpoint);
    vfs_superblock_unlock(mountpoint->sb);
    
    fs_type = vfs_get_fs_type(type);
    if (fs_type == NULL) {
        ret_val = -ENOENT; // Filesystem type not found
        goto ret;
    }
    if (!fs_type->registered) {
        ret_val = -ENOENT; // Filesystem type not registered
        goto ret;
    }
    // Call mount_begin to prepare for mounting
    ret_val = fs_type->ops->mount(mountpoint, device, flags, data, &sb);
    if (ret_val != 0) {
        goto ret; // mount failed
    }
    // Validate the returned superblock
    if (!__vfs_init_superblock_valid(sb)) {
        fs_type->ops->free(sb);
        ret_val = -EINVAL; // Invalid superblock returned by mount
        goto ret;
    }
    __vfs_init_superblock_structure(sb, fs_type);
    // Attach superblock to filesystem type
    vfs_fs_type_lock();
    vfs_superblock_wlock(mountpoint->sb);
    vfs_superblock_wlock(sb);
    __vfs_attach_superblock_to_fstype(sb);
    vfs_fs_type_unlock();
    sb->device = device;
    vfs_ilock(mountpoint);
    __vfs_set_mountpoint(sb, mountpoint);
    vfs_iunlock(mountpoint);
    vfs_superblock_unlock(sb);
    vfs_superblock_unlock(mountpoint->sb);
    ret_val = 0; // Successfully mounted
ret:
    if (ret_val != 0) {
        // On failure, need to revert mountpoint inode type
        vfs_superblock_wlock(mountpoint->sb);
        vfs_ilock(mountpoint);
        __vfs_clear_mountpoint(mountpoint);
        vfs_iunlock(mountpoint);
        vfs_superblock_unlock(mountpoint->sb);
    }
    vfs_put_fs_type(fs_type);
    return ret_val;
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
    int ret = vfs_idup(sb->mountpoint); // Increase ref count
    vfs_superblock_unlock(sb);
    if (ret != 0) {
        *ret_mountpoint = NULL;
    }
    *ret_mountpoint = sb->mountpoint;
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

bool vfs_superblock_wholding(struct vfs_superblock *sb) {
    if (!sb) {
        return false;
    }
    return rwlock_is_write_holding(&sb->lock);
}

void vfs_superblock_unlock(struct vfs_superblock *sb) {
    if (sb) {
        rwlock_release(&sb->lock);
    }
}

int vfs_sync_superblock(struct vfs_superblock *sb, int wait);
void vfs_unmount_begin(struct vfs_superblock *sb);

struct vfs_fs_type *vfs_get_fs_type(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    vfs_fs_type_lock();
    struct vfs_fs_type *fs_type = __vfs_get_fs_type_locked(name);
    if (fs_type != NULL) {
        kobject_get(&fs_type->kobj);    // Increase ref count
    }
    vfs_fs_type_unlock();
    return fs_type;
}

void vfs_put_fs_type(struct vfs_fs_type *fs_type) {
    if (fs_type == NULL) {
        return;
    }
    vfs_fs_type_lock();
    kobject_put(&fs_type->kobj);    // Decrease ref count
    vfs_fs_type_unlock();
}
