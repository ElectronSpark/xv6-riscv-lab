#include "types.h"
#include "string.h"
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
#include "vfs_private.h"
#include "list.h"
#include "hlist.h"
#include "slab.h"

// Locking order
// 1. mount mutex acquired via vfs_mount_lock()
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
static struct mutex __mount_mutex = { 0 };
static list_node_t vfs_fs_types = { 0 };
static uint16 vfs_fs_type_count = 0;

// The absolute root inode of the VFS
// It is a special inode that serves as the root of the entire filesystem tree
// It does not belong to any superblock or filesystem
// It does not have any data or operations associated with it
struct vfs_inode vfs_root_inode = { 0 };


#define MAX_FS_TYPES 256

/******************************************************************************
 * Pre declareed functions
 *****************************************************************************/
static struct vfs_inode *__vfs_inode_hash_add(struct vfs_superblock *sb, struct vfs_inode *inode);
static void __vfs_init_superblock_structure(struct vfs_superblock *sb, struct vfs_fs_type *fs_type);
static bool __vfs_superblock_ops_valid(struct vfs_superblock *sb);
static bool __vfs_init_superblock_valid(struct vfs_superblock *sb);
static void __vfs_attach_superblock_to_fstype(struct vfs_superblock *sb);
static void __vfs_detach_superblock_from_fstype(struct vfs_superblock *sb);
static int __vfs_turn_mountpoint(struct vfs_inode *mountpoint);
static void __vfs_set_mountpoint(struct vfs_superblock *sb, struct vfs_inode *mountpoint);
static void __vfs_clear_mountpoint(struct vfs_inode *mountpoint);

/******************************************************************************
 * Private functions
 *****************************************************************************/
// initialize rooti
static void __vfs_rooti_init(void) {
    memset(&vfs_root_inode, 0, sizeof(vfs_root_inode));
    vfs_root_inode.ino = 0;
    vfs_root_inode.mode = S_IFDIR | 0755;
    vfs_root_inode.valid = 1;
}

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
    size_t name_len = strlen(name);
    list_foreach_node_safe(&vfs_fs_types, pos, tmp, list_entry) {
        if (strncmp(pos->name, name, name_len) == 0) {
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

// Try to get an inode from superblock's inode hash list by inode number
static struct vfs_inode *__vfs_inode_hash_get(struct vfs_superblock *sb, uint64 ino) {
    struct vfs_inode key_inode = { 0 };
    key_inode.ino = ino;
    hlist_entry_t *entry = hlist_get(&sb->inodes, &key_inode);
    if (entry == NULL) {
        return NULL;
    }
    return container_of(entry, struct vfs_inode, hash_entry);
}

static struct vfs_inode *__vfs_inode_hash_add(struct vfs_superblock *sb, struct vfs_inode *inode) {
    return hlist_put(&sb->inodes, inode, false);
}

// Superblock structure helpers

static void __vfs_init_superblock_structure(struct vfs_superblock *sb, struct vfs_fs_type *fs_type) {
    list_entry_init(&sb->siblings);
    hlist_init(&sb->inodes, VFS_SUPERBLOCK_HASH_BUCKETS, &__vfs_superblock_inode_hlist_funcs);
    sb->fs_type = fs_type;
    rwlock_init(&sb->lock, RWLOCK_PRIO_READ, "vfs_superblock_lock");
}

static int __vfs_init_sb_rooti(struct vfs_superblock *sb) {
    __vfs_inode_init(sb->root_inode);
    int ret = vfs_add_inode(sb, sb->root_inode, NULL);
    if (ret == 0) {
        sb->root_inode->parent = sb->root_inode;
    }
    return ret;
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

// After a filesystem's mount callback returns a freshly allocated superblock,
// the VFS validates it with this helper before attaching it to the mount tree.
static bool __vfs_init_superblock_valid(struct vfs_superblock *sb) {
    if (sb == NULL) {
        return false;
    }
    if (sb->valid || sb->dirty) {
        return false;
    }
    if (!__vfs_superblock_ops_valid(sb)) {
        return false;
    }
    if (sb->backendless && (sb->device != NULL)) {
        return false;
    }
    if (!sb->backendless && (sb->device == NULL)) {
        return false;
    }
    if (sb->mountpoint != NULL || sb->parent_sb != NULL) {
        // At this point, the superblock has not been mounted yet
        return false;
    }
    return true;
}

static void __vfs_attach_superblock_to_fstype(struct vfs_superblock *sb) {
    list_node_push_back(&sb->fs_type->superblocks, sb, siblings);
    sb->fs_type->sb_count++;
    assert(sb->fs_type->sb_count > 0, "Filesystem type superblock count overflow");
}

static void __vfs_detach_superblock_from_fstype(struct vfs_superblock *sb) {
    list_node_detach(sb, siblings);
    sb->fs_type->sb_count--;
    sb->valid = 0;
    assert(sb->fs_type->sb_count >= 0, "Filesystem type superblock count underflow");
}

// Turn a directory inode into a temporary mountpoint placeholder.
// Caller must hold the parent superblock write lock and the inode lock.
// On success the inode type is flipped to MNT and its refcount is incremented.
static int __vfs_turn_mountpoint(struct vfs_inode *mountpoint) {
    if (mountpoint != &vfs_root_inode) {
        VFS_SUPERBLOCK_ASSERT_WHOLDING(mountpoint->sb, 
            "Mountpoint inode's superblock lock must be write held to turn into mountpoint");
    }
    VFS_INODE_ASSERT_HOLDING(mountpoint, "Mountpoint inode lock must be held to turn into mountpoint");
    if (vfs_inode_refcount(mountpoint) > 2) {
        return -EBUSY; // Mountpoint inode is in use
    }
    if (!S_ISDIR(mountpoint->mode)) {
        return -ENOTDIR; // Mountpoint must be a directory
    }
    if (vfs_inode_is_local_root(mountpoint)) {
        return -EBUSY; // Cannot mount on root inode
    }
    if (mountpoint->mount) {
        return -EBUSY; // Already a mountpoint
    }
    mountpoint->mount = 1;
    mountpoint->mnt_rooti = NULL;
    mountpoint->mnt_sb = NULL;
    if (mountpoint != &vfs_root_inode) {
        mountpoint->sb->mount_count++;
        assert(mountpoint->sb->mount_count > 0, "Mountpoint superblock mount count overflow");
    }
    return 0;
}

// Set the mountpoint inode of a superblock.
// Caller must hold the parent superblock write lock and the mountpoint inode lock;
// this helper assumes the refcount was raised by __vfs_turn_mountpoint().
static void __vfs_set_mountpoint(struct vfs_superblock *sb, struct vfs_inode *mountpoint) {
    if (mountpoint != &vfs_root_inode) {
        VFS_SUPERBLOCK_ASSERT_WHOLDING(mountpoint->sb, "Mountpoint inode's superblock lock must be write held to set mountpoint");
    }
    VFS_SUPERBLOCK_ASSERT_WHOLDING(sb, "Superblock lock must be write held to set mountpoint");
    VFS_INODE_ASSERT_HOLDING(mountpoint, "Mountpoint inode lock must be held to set mountpoint");
    assert(mountpoint->mount, "Mountpoint inode is not marked as a mountpoint");
    assert(sb->mountpoint == NULL, "Superblock mountpoint is already set");
    sb->mountpoint = mountpoint;
    sb->parent_sb = mountpoint->sb;
    mountpoint->mnt_sb = sb;
    mountpoint->mnt_rooti = sb->root_inode;
}

// Clear the mountpoint inode of a superblock, undoing __vfs_set_mountpoint().
// Caller must hold the parent superblock write lock and the mountpoint inode lock;
// this helper drops the reference taken in __vfs_turn_mountpoint().
static void __vfs_clear_mountpoint(struct vfs_inode *mountpoint) {
    if (mountpoint != &vfs_root_inode) {
        VFS_SUPERBLOCK_ASSERT_WHOLDING(mountpoint->sb, "Mountpoint inode's superblock lock must be write held to clear mountpoint");
    }
    VFS_INODE_ASSERT_HOLDING(mountpoint, "Mountpoint inode lock must be held to clear mountpoint");
    assert(mountpoint->mount, "Mountpoint inode type is not MNT");
    if (mountpoint != &vfs_root_inode) {
        mountpoint->sb->mount_count--;
        assert(mountpoint->sb->mount_count >= 0, "Mountpoint superblock mount count underflow");
    }
    mountpoint->mnt_sb = NULL;
    mountpoint->mnt_rooti = NULL;
    mountpoint->mount = 0;
}

 /******************************************************************************
 * Files System Type Public APIs
 *****************************************************************************/
/*
 * vfs_init - Initialize the VFS subsystem and root inode.
 *
 * Locking:
 *   - None.
 */
void vfs_init(void) {
    __vfs_rooti_init();
    list_entry_init(&vfs_fs_types);
    mutex_init(&__mount_mutex, "vfs_mount_mutex");
    int ret = slab_cache_init(&vfs_superblock_cache, "vfs_superblock_cache",
                              sizeof(struct vfs_superblock), 0);
    assert(ret == 0, "Failed to initialize vfs_superblock_cache slab cache, errno=%d", ret);
    ret = slab_cache_init(&vfs_fs_type_cache, "vfs_fs_type_cache",
                          sizeof(struct vfs_fs_type), 0);
    assert(ret == 0, "Failed to initialize vfs_fs_type_cache slab cache, errno=%d", ret);
    vfs_fs_type_count = 0;
    struct proc *proc = myproc();
    assert(proc != NULL, "vfs_init must be called from a process context");
    __vfs_inode_init(&vfs_root_inode);
    proc_lock(proc);
    proc->fs.rooti = &vfs_root_inode;
    proc->fs.cwd = &vfs_root_inode;
    proc_unlock(proc);
    tmpfs_init_fs_type();
}

/*
 * vfs_fs_type_allocate - Allocate an empty filesystem type descriptor.
 *
 * Locking:
 *   - None. Caller performs subsequent initialization before registration.
 *
 * Returns:
 *   - Pointer to struct vfs_fs_type on success, NULL on allocation failure.
 */
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

void vfs_fs_type_free(struct vfs_fs_type *fs_type) {
    slab_free(fs_type);
}

/*
 * vfs_register_fs_type - Register a filesystem driver in the global list.
 *
 * Locking:
 *   - Caller must hold the mount mutex via vfs_mount_lock().
 *
 * Returns:
 *   - 0 on success or negative errno on failure.
 */
int vfs_register_fs_type(struct vfs_fs_type *fs_type) {
    // Make sure the fs_type is not registered and is empty
    if (!holding_mutex(&__mount_mutex)) {
        return -EPERM; // Must hold mount mutex to register fs_type
    }
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
    if (vfs_fs_type_count >= MAX_FS_TYPES) {
        return -ENOSPC; // No space for more filesystem types
    }
    struct vfs_fs_type *existing = __vfs_get_fs_type_locked(fs_type->name);
    if (existing != NULL) {
        return -EEXIST; // Filesystem type with the same name already exists
    }
    __vfs_register_fs_type_locked(fs_type);
    return 0;
}

/*
 * vfs_unregister_fs_type - Remove a filesystem driver from the global list.
 *
 * Locking:
 *   - Caller must hold the mount mutex via vfs_mount_lock().
 *
 * Returns:
 *   - 0 on success or negative errno if the driver is busy/not found.
 */
int vfs_unregister_fs_type(const char *name) {
    if (name == NULL) {
        return -EINVAL; // Invalid argument
    }
    if (!holding_mutex(&__mount_mutex)) {
        return -EPERM; // Must hold mount mutex to register fs_type
    }
    struct vfs_fs_type *pos = __vfs_get_fs_type_locked(name);
    if (pos != NULL) {
        // @ TODO: inform all mounted superblocks of this fs_type after unregistering
        __vfs_unregister_fs_type_locked(pos);
        kobject_put(&pos->kobj);
        return 0; // Successfully unregistered
    }
    return -ENOENT; // Filesystem type not found
}

/*
 * vfs_mount_lock - Acquire the global mount mutex.
 *
 * Locking:
 *   - None; this helper acquires the mutex on behalf of the caller.
 */
void vfs_mount_lock(void) {
    mutex_lock(&__mount_mutex);
}

/*
 * vfs_mount_unlock - Release the global mount mutex.
 *
 * Locking:
 *   - Caller must currently hold the mutex via vfs_mount_lock().
 */
void vfs_mount_unlock(void) {
    mutex_unlock(&__mount_mutex);
}

 /******************************************************************************
 * Superblock Public APIs
 *****************************************************************************/
/*
 * vfs_mount - Attach a filesystem instance to a directory mountpoint.
 *
 * Locking:
 *   - Caller holds vfs_mount_lock().
 *   - Caller holds the parent superblock write lock.
 *   - Caller holds the mountpoint inode mutex (and any device inode lock if applicable).
 *
 * Returns:
 *   - 0 on success or negative errno on failure.
 */
 int vfs_mount(const char *type, struct vfs_inode *mountpoint,
              struct vfs_inode *device, int flags, const char *data) {
    struct vfs_fs_type *fs_type = NULL;
    struct vfs_superblock *sb = NULL;
    int ret_val = 0;

    if (type == NULL || mountpoint == NULL) {
        printf("vfs_mount: invalid arguments\n");
        return -EINVAL; // Invalid arguments
    }

    if (!holding_mutex(&__mount_mutex)) {
        printf("vfs_mount: mount mutex not held\n");
        return -EPERM; // Must hold mount mutex to register fs_type
    }

    ret_val = __vfs_dir_inode_valid_holding(mountpoint);
    if (ret_val != 0) {
        printf("vfs_mount: mountpoint inode not valid, errno=%d\n", ret_val);
        return ret_val;
    }
    if (mountpoint != &vfs_root_inode) {
        if (!vfs_superblock_wholding(mountpoint->sb)) {
            printf("vfs_mount: mountpoint superblock write lock not held\n");
            return -EPERM; // Caller must hold write lock of the superblock
        }
        if (!mountpoint->sb->valid) {
            printf("vfs_mount: mountpoint superblock is not valid\n");
            return -EINVAL; // Mountpoint's superblock is not valid
        }
        if (!S_ISDIR(mountpoint->mode)) {
            printf("vfs_mount: mountpoint is not a directory\n");
            return -EINVAL; // Mountpoint must be a directory
        }
    }

    ret_val = __vfs_turn_mountpoint(mountpoint);
    if (ret_val != 0) {
        printf("vfs_mount: failed to turn mountpoint, errno=%d\n", ret_val);
        return ret_val; // Failed to turn mountpoint
    }
    
    fs_type = vfs_get_fs_type(type);
    if (fs_type == NULL) {
        printf("vfs_mount: filesystem type '%s' not found\n", type);
        ret_val = -ENOENT; // Filesystem type not found
        goto ret;
    }
    if (!fs_type->registered) {
        printf("vfs_mount: filesystem type '%s' not registered\n", type);
        ret_val = -ENOENT; // Filesystem type not registered
        goto ret;
    }
    // Ask the filesystem type to allocate and initialise a new superblock
    // The superblock is private to the filesystem until we attach it, so no locking is needed yet
    ret_val = fs_type->ops->mount(mountpoint, device, flags, data, &sb);
    if (ret_val != 0) {
        printf("vfs_mount: filesystem type '%s' mount failed, errno=%d\n", type, ret_val);
        goto ret; // mount failed
    }
    // Validate the returned superblock
    if (!__vfs_init_superblock_valid(sb)) {
        printf("vfs_mount: invalid superblock returned by mount\n");
        ret_val = -EINVAL; // Invalid superblock returned by mount
        goto ret;
    }
    if (sb->total_blocks && (sb->used_blocks > sb->total_blocks)) {
        printf("vfs_mount: superblock used_blocks exceeds total_blocks\n");
        ret_val = -EINVAL; // Invalid superblock block usage
        goto ret;
    }
    if (sb->root_inode == NULL) {
        printf("vfs_mount: superblock has no root inode\n");
        ret_val = -EINVAL; // Superblock has no root inode
        goto ret;
    }
    if (sb->root_inode->valid) {
        printf("vfs_mount: root inode already marked valid\n");
        ret_val = -EINVAL; // Root inode is not ready
        goto ret;
    }
    __vfs_init_superblock_structure(sb, fs_type);
    vfs_superblock_wlock(sb);   // Must hold superblock lock to init root inode
    ret_val = __vfs_init_sb_rooti(sb);
    if (ret_val != 0) {
        printf("vfs_mount: failed to initialize superblock root inode, errno=%d\n", ret_val);
        goto ret;
    }

    // Attach superblock to filesystem type
    __vfs_attach_superblock_to_fstype(sb);
    sb->device = device;
    __vfs_set_mountpoint(sb, mountpoint);
    sb->root_inode->sb = sb; // Associate root inode with superblock
    ret_val = 0; // Successfully mounted
ret:
    if (ret_val != 0) {
        if (sb) {
            if (sb->root_inode) {
                sb->root_inode->ops->free_inode(sb->root_inode);
            }
            fs_type->ops->free(sb);
        }
        // On failure, need to revert mountpoint inode type
        __vfs_clear_mountpoint(mountpoint);
    } else if (vfs_superblock_wholding(sb)) {
        sb->initialized = 1;
        sb->valid = 1;
        vfs_superblock_unlock(sb);
    }
    vfs_put_fs_type(fs_type);
    return ret_val;
}

/*
 * vfs_unmount - Detach the filesystem rooted at `mountpoint`.
 *
 * Locking:
 *   - Caller holds vfs_mount_lock().
 *   - Caller holds the parent and child superblock write locks.
 *   - Caller holds the mountpoint inode mutex and the mounted root inode mutex.
 *
 * Returns:
 *   - 0 on success or negative errno if busy/invalid.
 */
int vfs_unmount(struct vfs_inode *mountpoint) {
    struct vfs_superblock *sb = NULL; // Superblock of the mounted filesystem
    struct vfs_inode *mounted_inode = NULL; // Root inode of the mounted filesystem
    int ret_val = 0;

    if (mountpoint == NULL) {
        return -EINVAL; // Invalid argument
    }

    if (!holding_mutex(&__mount_mutex)) {
        return -EPERM; // Must hold mount mutex to register fs_type
    }
    if (!holding_mutex(&mountpoint->mutex)) {
        return -EPERM; // Caller does not hold the mountpoint inode lock
    }
    ret_val = __vfs_inode_valid(mountpoint);
    if (ret_val != 0) {
        return ret_val;
    }
    if (!vfs_superblock_wholding(mountpoint->sb)) {
        return -EPERM; // Caller must hold write lock of the superblock
    }
    if (!mountpoint->sb->valid) {
        return -EINVAL; // Mountpoint's superblock is not valid
    }

    if (!S_ISDIR(mountpoint->mode) || !mountpoint->mount) {
        return -EINVAL; // Inode is not a mountpoint
    }
    sb = mountpoint->mnt_sb;
    if (sb == NULL) {
        return -EINVAL; // Mountpoint has no mounted superblock
    }
    mounted_inode = sb->root_inode;
    if (mounted_inode == NULL) {
        return -EINVAL; // Mounted superblock has no root inode
    }
    if (!holding_mutex(&mounted_inode->mutex)) {
        return -EPERM; // Caller does not hold the inode lock
    }
    ret_val = __vfs_inode_valid(mounted_inode);
    if (ret_val != 0) {
        return ret_val;
    }
    if (!vfs_superblock_wholding(sb)) {
        return -EPERM; // Caller must hold write lock of the superblock
    }
    if (!sb->valid) {
        return -EINVAL; // Mountpoint's superblock is not valid
    }
    // Superblock should have no mounted superblocks under it
    if (sb->mount_count > 0) {
        printf("vfs_unmount: mount_count=%d\n", sb->mount_count);
        return -EBUSY; // There are still mounted superblocks under this superblock
    }
    // After unmount_begin, superblock should be clean and have no active inodes
    if (sb->dirty) {
        printf("vfs_unmount: sb valid=%u dirty=%u\n", sb->valid, sb->dirty);
        return -EBUSY; // Superblock is still valid or dirty
    }

    // Begin unmounting
    sb->ops->unmount_begin(sb);

    // Superblock also should have no active inodes before unmounting
    size_t remaining_inodes = hlist_len(&sb->inodes);
    if (remaining_inodes > 0) {
        printf("vfs_unmount: remaining inodes=%lu\n", remaining_inodes);
        return -EBUSY; // There are still active inodes
    }

    // Detach superblock from filesystem type
    __vfs_detach_superblock_from_fstype(sb);
    __vfs_clear_mountpoint(mountpoint);

    // Free the superblock
    sb->fs_type->ops->free(sb);

    return 0; // Successfully unmounted
}

/*
 * vfs_get_mnt_rooti - Fetch the root inode of a mountpoint.
 *
 * Locking:
 *   - Caller should not hold inode or superblock locks of the mounted filesystem;
 *   - Caller should not hold the mountpoint inode lock;
 *   - Caller should hold the parent superblock read lock.
 *   - After returning, caller should release the parent superblock lock, acquire
 *     the mounted superblock lock and the root inode lock, and verify the returned 
 *     inode is still valid.
 *
 * Returns:
 *   - 0 with *ret_rooti referencing the root inode (refcount incremented, unlocked).
 *   - Negative errno on failure.
 *
 * Notes:
 *   - Callers must drop the inode via vfs_iput() and verify the inode's
 *     valid flag before invoking callbacks.
 */
int vfs_get_mnt_rooti(struct vfs_inode *mountpoint, struct vfs_inode **ret_rooti) {
    if (mountpoint == NULL || ret_rooti == NULL) {
        return -EINVAL; // Invalid arguments
    }
    int ret_val = 0;
    struct vfs_superblock *sb = NULL;
    struct vfs_inode *rooti = NULL;
    vfs_ilock(mountpoint);
    ret_val = __vfs_dir_inode_valid_holding(mountpoint);
    if (ret_val != 0) {
        vfs_iunlock(mountpoint);
        return ret_val; // Mountpoint inode is not valid
    }
    if (!S_ISDIR(mountpoint->mode) || !mountpoint->mount) {
        vfs_iunlock(mountpoint);
        return -EINVAL; // Inode is not a mountpoint
    }
    if ((sb = mountpoint->mnt_sb) == NULL) {
        vfs_iunlock(mountpoint);
        return -EINVAL; // Mountpoint has no mounted superblock
    }
    if ((rooti = sb->root_inode) == NULL) {
        vfs_iunlock(mountpoint);
        return -EINVAL; // Mounted superblock has no root inode
    }
    vfs_iunlock(mountpoint);

    // Avoid acquiring multiple superblock locks and inode locks at once
    // So we first increase the refcount of the root inode to keep it alive
    vfs_idup(rooti);
    vfs_ilock(rooti);
    *ret_rooti = rooti;
    return ret_val;
}

/*
 * vfs_superblock_rlock - Acquire a superblock read lock.
 *
 * Locking:
 *   - None; acquires the lock when sb != NULL.
 */
void vfs_superblock_rlock(struct vfs_superblock *sb) {
    if (sb) {
        rwlock_acquire_read(&sb->lock);
    }
}

/*
 * vfs_superblock_wlock - Acquire a superblock write lock.
 *
 * Locking:
 *   - None; acquires the lock when sb != NULL.
 */
void vfs_superblock_wlock(struct vfs_superblock *sb) {
    if (sb) {
        rwlock_acquire_write(&sb->lock);
    }
}

/*
 * vfs_superblock_wholding - Test whether the caller holds the write lock.
 *
 * Locking:
 *   - None.
 */
bool vfs_superblock_wholding(struct vfs_superblock *sb) {
    if (!sb) {
        return false;
    }
    return rwlock_is_write_holding(&sb->lock);
}

/*
 * vfs_superblock_unlock - Release a previously acquired superblock lock.
 *
 * Locking:
 *   - Caller must hold the read or write lock.
 */
void vfs_superblock_unlock(struct vfs_superblock *sb) {
    if (sb) {
        rwlock_release(&sb->lock);
    }
}

/*
 * vfs_alloc_inode - Ask the filesystem to allocate a new inode object.
 *
 * Locking:
 *   - None required by the caller; this helper acquires/releases the
 *     superblock write lock internally.
 *
 * Returns:
 *   - 0 on success with *ret_inode set, or negative errno on failure.
 */
int vfs_alloc_inode(struct vfs_superblock *sb, struct vfs_inode **ret_inode) {
    if (sb == NULL || ret_inode == NULL) {
        return -EINVAL; // Invalid arguments
    }
    VFS_SUPERBLOCK_ASSERT_WHOLDING(sb, "vfs_alloc_inode: must hold superblock write lock");
    if (!sb->valid) {
        return -EINVAL; // Superblock is not valid
    }
    struct vfs_inode *inode = NULL;
    int ret = sb->ops->alloc_inode(sb, &inode);
    if (ret == 0) {
        *ret_inode = inode;
        __vfs_inode_init(inode);
        ret = vfs_add_inode(sb, inode, ret_inode);
        if (ret != 0) {
            inode->ops->free_inode(inode);
        }
    }
    return ret;
}

/*
 * vfs_get_inode - Load an inode from the filesystem driver.
 *
 * Locking:
 *   - Caller must hold the superblock write lock.
 *   - On success, the returned inode is locked and its refcount is set to 1.
 *
 * Returns:
 *   - 0 on success with *ret_inode set (locked), or negative errno on failure.
 */
int vfs_get_inode(struct vfs_superblock *sb, uint64 ino,
                  struct vfs_inode **ret_inode) {
    if (sb == NULL || ret_inode == NULL) {
        return -EINVAL; // Invalid arguments
    }
    VFS_SUPERBLOCK_ASSERT_WHOLDING(sb, "vfs_get_inode: must hold superblock write lock");
    if (!sb->valid) {
        return -EINVAL; // Superblock is not valid
    }
    int ret = sb->ops->get_inode(sb, ino, ret_inode);
    if (ret_inode != NULL) {
        __vfs_inode_init(*ret_inode);
        ret = vfs_add_inode(sb, *ret_inode, NULL);
        if (ret != 0) {
            (*ret_inode)->ops->free_inode(*ret_inode);
            *ret_inode = NULL;
        }
    }
    return ret;
}

/*
 * vfs_sync_superblock - Flush superblock metadata to backing storage.
 *
 * Locking:
 *   - Caller must hold the superblock write lock.
 *
 * Returns:
 *   - 0 on success or negative errno on failure.
 */
int vfs_sync_superblock(struct vfs_superblock *sb, int wait) {
    if (sb == NULL) {
        return -EINVAL; // Invalid argument
    }
    VFS_SUPERBLOCK_ASSERT_WHOLDING(sb, "vfs_sync_superblock: must hold superblock write lock");
    if (!sb->valid) {
        return -EINVAL; // Superblock is not valid
    }
    if (!sb->dirty) {
        return 0; // Superblock is already clean
    }
    int ret = sb->ops->sync_fs(sb, wait);
    if (ret == 0) {
        sb->dirty = 0; // Mark superblock as clean
    }
    return ret;
}

/*
 * vfs_get_fs_type - Look up a filesystem driver by name and take a reference.
 *
 * Locking:
 *   - Caller must hold the mount mutex via vfs_mount_lock().
 *
 * Returns:
 *   - Pointer to struct vfs_fs_type or NULL if not found.
 */
struct vfs_fs_type *vfs_get_fs_type(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    assert(holding_mutex(&__mount_mutex), "vfs_put_fs_type: must hold mount mutex");
    struct vfs_fs_type *fs_type = __vfs_get_fs_type_locked(name);
    if (fs_type != NULL) {
        kobject_get(&fs_type->kobj);    // Increase ref count
    }
    return fs_type;
}

/*
 * vfs_put_fs_type - Drop a reference obtained via vfs_get_fs_type().
 *
 * Locking:
 *   - Caller must hold the mount mutex via vfs_mount_lock().
 */
void vfs_put_fs_type(struct vfs_fs_type *fs_type) {
    if (fs_type == NULL) {
        return;
    }
    assert(holding_mutex(&__mount_mutex), "vfs_put_fs_type: must hold mount mutex");
    kobject_put(&fs_type->kobj);    // Decrease ref count
}

/*
 * vfs_get_dentry_inode - Resolve a dentry to an inode, populating cache as needed.
 *
 * Locking:
 *   - Caller holds the dentry's superblock read lock on entry. This helper may
 *     drop the read lock and acquire the write lock internally, and it returns
 *     with the lock released.
 *
 * Returns:
 *   - 0 on success with *ret_inode referencing the inode (refcount incremented).
 *   - Negative errno on failure.
 *
 * Notes:
 *   - Callers must avoid holding inode locks that could deadlock with these
 *     lock transitions and must release the inode via vfs_iput().
 */
int vfs_get_dentry_inode(struct vfs_dentry *dentry, struct vfs_inode **ret_inode) {
    int ret = 0;
    struct vfs_inode *inode = NULL;
    if (dentry == NULL || ret_inode == NULL) {
        return -EINVAL; // Invalid arguments
    }
    if (dentry->sb == NULL) {
        return -EINVAL; // Dentry has no associated superblock
    }

    // First try to get the inode from inode cache
    if (!dentry->sb->valid) {
        return -EINVAL; // Superblock is not valid
    }
    ret = vfs_get_inode_cached(dentry->sb, dentry->ino, &inode);
    if (ret != -ENOENT) {
        // Inode is found or error occurred
        // When successful, inode ref count is already increased
        // When failed, vfs_get_inode_cached set inode to NULL
        *ret_inode = inode;
        return ret;
    }
    if (!vfs_superblock_wholding(dentry->sb)) {
        // Suppose readlock is holding here
        // Upgrade to write lock
        vfs_superblock_unlock(dentry->sb);
        vfs_superblock_wlock(dentry->sb);
    }
    // Double check cache again after acquiring write lock
    if (!dentry->sb->valid) {
        return -EINVAL; // Superblock is not valid
    }
    ret = vfs_get_inode_cached(dentry->sb, dentry->ino, &inode);
    if (ret != -ENOENT) {
        // Inode is found or error occurred
        // When successful, inode ref count is already increased
        // When failed, vfs_get_inode_cached set inode to NULL
        *ret_inode = inode;
        return ret;
    }
    ret = vfs_get_inode(dentry->sb, dentry->ino, &inode);
    if (ret != 0) {
        *ret_inode = NULL;
        return ret; // Failed to load inode
    }
    vfs_ilock(inode);
    if (S_ISDIR(inode->mode)) {
        // Initialize directory-specific fields if needed
        assert(dentry->parent != NULL, "Directory inode must have a parent dentry");
        inode->parent = dentry->parent;
        vfs_idup(dentry->parent);
    }
    // vfs_iunlock(inode);
    *ret_inode = inode; // Return the loaded inode
    return 0;
}

 /******************************************************************************
 * Module scope private functions
 *****************************************************************************/
/*
 * vfs_get_inode_cached - Lookup an inode in a superblock's in-memory cache.
 *
 * Locking:
 *   - Caller holds the superblock read or write lock for the entire call.
 *   - On success, the returned inode is locked; caller must call vfs_iunlock()
 *     when done.
 *
 * Returns:
 *   - 0 on success with *ret_inode set (locked).
 *   - -ENOENT if the inode is not cached.
 *   - -EINVAL or other negative errno on failure.
 */
int vfs_get_inode_cached(struct vfs_superblock *sb, uint64 ino,
                         struct vfs_inode **ret_inode) {
    if (sb == NULL || ret_inode == NULL) {
        return -EINVAL; // Invalid arguments
    }
    if (!sb->valid) {
        return -EINVAL; // Superblock is not valid
    }
    struct vfs_inode *inode = __vfs_inode_hash_get(sb, ino);
    if (inode == NULL) {
        *ret_inode = NULL;
        return -ENOENT; // Inode not found
    }
    vfs_ilock(inode);
    if (!inode->valid) {
        // Inode should be valid when first gotten from the cache,
        // but it may have been invalidated during the windows between
        // getting from cache and acquiring the inode lock.
        // In this case, the inode should have been removed from the cache already.
        vfs_iunlock(inode);
        *ret_inode = NULL;
        return -ENOENT; // Inode is not valid
    }
    *ret_inode = inode;
    // vfs_idup(inode);
    // vfs_iunlock(inode);
    return 0;
}

/*
 * vfs_add_inode - Insert a newly loaded inode into the cache.
 *
 * Locking:
 *   - Caller holds the superblock write lock.
 *   - On success, if ret_inode is non-NULL, the inode is returned locked;
 *     caller must call vfs_iunlock() when done.
 *
 * Returns:
 *   - 0 on success with *ret_inode referencing the canonical inode (locked if ret_inode != NULL).
 *   - -EEXIST if another inode with the same number already exists; *ret_inode
 *     is set to the existing inode (locked) if ret_inode != NULL.
 *   - Other negative errno codes on failure.
 */
int vfs_add_inode(struct vfs_superblock *sb,
                  struct vfs_inode *inode,
                  struct vfs_inode **ret_inode) {
    if (sb == NULL || inode == NULL) {
        return -EINVAL; // Invalid arguments
    }
    VFS_SUPERBLOCK_ASSERT_WHOLDING(sb, "Superblock lock must be write held to add inode");
    if (!sb->valid && sb->initialized) {
        return -EINVAL; // Superblock is not valid
    }
    if (inode->sb != NULL) {
        return -EINVAL; // Inode's superblock does not match
    }
    if (inode->valid) {
        return -EINVAL; // Inode to add must not be valid yet
    }
    struct vfs_inode *existing = __vfs_inode_hash_get(sb, inode->ino);
    if (existing != NULL) {
        if (ret_inode != NULL) {
            vfs_ilock(existing);
            // vfs_idup(existing);
            *ret_inode = existing;
        }
        return -EEXIST; // Inode with the same number already exists
    }
    struct vfs_inode *popped = __vfs_inode_hash_add(sb, inode);
    if (popped != NULL) {
        // At this point, something is wrong in the hash list implementation
        panic("vfs_add_inode: inode hash add returned existing inode unexpectedly");
    }
    inode->valid = 1; // Mark inode as valid
    inode->sb = sb; // Associate inode with superblock
    if (ret_inode != NULL) {
        vfs_ilock(inode);
        // vfs_idup(inode);
        *ret_inode = inode;
    }
    return 0;
}

/*
 * vfs_remove_inode - Drop an inode from the cache and mark it invalid.
 *
 * Locking:
 *   - Caller holds the superblock write lock and the inode mutex.
 *
 * Returns:
 *   - 0 on success or negative errno on failure.
 */
int vfs_remove_inode(struct vfs_superblock *sb, struct vfs_inode *inode) {
    if (sb == NULL || inode == NULL) {
        return -EINVAL; // Invalid arguments
    }
    VFS_SUPERBLOCK_ASSERT_WHOLDING(sb, "Superblock lock must be write held to remove inode");
    VFS_INODE_ASSERT_HOLDING(inode, "Inode lock must be held to remove inode");
    if (!sb->valid) {
        return -EINVAL; // Superblock is not valid
    }
    struct vfs_inode *existing = __vfs_inode_hash_get(sb, inode->ino);
    if (existing == NULL) {
        return -ENOENT; // Inode not found in cache
    }
    if (existing != inode) {
        return -ENOENT; // Inode to remove does not match the cached inode
    }
    struct vfs_inode *popped = hlist_pop(&sb->inodes, inode);
    if (popped != inode) {
        // At this point, something is wrong in the hash list implementation
        panic("vfs_remove_inode: inode hash pop returned unexpected inode");
    }
    inode->valid = 0; // Mark inode as invalid
    inode->sb = NULL; // Disassociate inode from superblock
    return 0;
}

void vfs_release_dentry(struct vfs_dentry *dentry) {
    if (dentry == NULL) {
        return;
    }
    if (dentry->name) {
        kmm_free(dentry->name);
        dentry->name = NULL;
        dentry->name_len = 0;
    }
}
