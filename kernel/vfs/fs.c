#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "param.h"
#include "errno.h"
#include "bits.h"
#include "vfs/stat.h"
#include "spinlock.h"
#include "proc/proc.h"
#include "mutex_types.h"
#include "rwlock.h"
#include "completion.h"
#include "vfs/fs.h"
#include "vfs/file.h"
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
static slab_cache_t vfs_struct_cache = { 0 };
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
static struct fs_struct *__vfs_struct_alloc_init(void);
static void __vfs_struct_free(struct fs_struct *fs);

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
    list_entry_init(&sb->orphan_list);
    hlist_init(&sb->inodes, VFS_SUPERBLOCK_HASH_BUCKETS, &__vfs_superblock_inode_hlist_funcs);
    sb->fs_type = fs_type;
    sb->orphan_count = 0;
    __atomic_store_n(&sb->refcount, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&sb->mount_count, 0, __ATOMIC_SEQ_CST);
    rwlock_init(&sb->lock, RWLOCK_PRIO_READ, "vfs_superblock_lock");
    spin_init(&sb->spinlock, "vfs_superblock_spinlock");
}

static int __vfs_init_sb_rooti(struct vfs_superblock *sb) {
    __vfs_inode_init(sb->root_inode);
retry_add:;
    struct vfs_inode *inode = vfs_add_inode(sb, sb->root_inode);
    if (IS_ERR_OR_NULL(inode)) {
        if (PTR_ERR(inode) == -EAGAIN) {
            // Should not happen during init, but handle gracefully
            vfs_superblock_unlock(sb);
            yield();
            vfs_superblock_wlock(sb);
            if (!sb->valid && sb->initialized) {
                return -EINVAL;
            }
            goto retry_add;
        }
        if (inode == NULL) {
            return -ENOENT; // Failed to add root inode
        }
        return PTR_ERR(inode);
    }
    if (inode != sb->root_inode) {
        vfs_iunlock(inode);
        return -EEXIST; // Root inode already exists
    }
    sb->root_inode->parent = sb->root_inode;
    vfs_iunlock(sb->root_inode);
    return 0;
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
    sb->registered = 1;
    assert(sb->fs_type->sb_count > 0, "Filesystem type superblock count overflow");
}

static void __vfs_detach_superblock_from_fstype(struct vfs_superblock *sb) {
    list_node_detach(sb, siblings);
    sb->fs_type->sb_count--;
    sb->registered = 0;
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
        vfs_superblock_mountcount_inc(mountpoint->sb);
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
        vfs_superblock_mountcount_dec(mountpoint->sb);
    }
    mountpoint->mnt_sb = NULL;
    mountpoint->mnt_rooti = NULL;
    mountpoint->mount = 0;
}

static struct fs_struct *__vfs_struct_alloc_init(void) {
    struct fs_struct *fs = slab_alloc(&vfs_struct_cache);
    if (fs == NULL) {
        return NULL;
    }
    memset(fs, 0, sizeof(*fs));
    spin_init(&fs->lock, "fs_struct_lock");
    smp_store_release(&fs->ref_count, 1);
    return fs;
}

static void __vfs_struct_free(struct fs_struct *fs) {
    slab_free(fs);
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
    __vfs_fdtable_global_init();
    int ret = slab_cache_init(&vfs_superblock_cache, 
                              "vfs_superblock_cache",
                              sizeof(struct vfs_superblock), 
                              SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
    assert(ret == 0, "Failed to initialize vfs_superblock_cache slab cache, errno=%d", ret);
    ret = slab_cache_init(&vfs_fs_type_cache, 
                          "vfs_fs_type_cache",
                          sizeof(struct vfs_fs_type), 
                          SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
    assert(ret == 0, "Failed to initialize vfs_fs_type_cache slab cache, errno=%d", ret);\
    ret = slab_cache_init(&vfs_struct_cache,
                          "vfs_struct_cache",
                          sizeof(struct fs_struct),
                          SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
    assert(ret == 0, "Failed to initialize vfs_struct_cache slab cache, errno=%d", ret);
    vfs_fs_type_count = 0;
    struct proc *proc = myproc();
    assert(proc != NULL, "vfs_init must be called from a process context");
    __vfs_inode_init(&vfs_root_inode);
    __vfs_file_init();
    proc->fs = vfs_struct_init();
    proc->fdtable = vfs_fdtable_init();
    tmpfs_init_fs_type();
    xv6fs_init_fs_type();
}

/*
 * __vfs_shrink_caches - Shrink VFS slab caches to release unused pages.
 *
 * This should be called when checking for memory leaks to ensure that
 * empty slab pages are returned to the page allocator.
 */
void __vfs_shrink_caches(void) {
    slab_cache_shrink(&vfs_superblock_cache, 0x7fffffff);
    slab_cache_shrink(&vfs_fs_type_cache, 0x7fffffff);
    __vfs_file_shrink_cache();
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
        sb->attached = 1;
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
    if ((ret_val = vfs_superblock_mountcount(sb)) > 0) {
        printf("vfs_unmount: mount_count=%d\n", ret_val);
        return -EBUSY; // There are still mounted superblocks under this superblock
    }
    ret_val = 0;
    // After unmount_begin, superblock should be clean and have no active inodes
    if (sb->dirty) {
        printf("vfs_unmount: sb valid=%u dirty=%u\n", sb->valid, sb->dirty);
        return -EBUSY; // Superblock is still valid or dirty
    }

    // Begin unmounting
    if (sb->ops->unmount_begin) {
        sb->ops->unmount_begin(sb);
    }

    // Superblock should have no active inodes except the root inode.
    // The root inode is expected to still be in the cache - it will be
    // removed and freed below.
    size_t remaining_inodes = hlist_len(&sb->inodes);
    if (remaining_inodes > 1) {
        printf("vfs_unmount: remaining inodes=%lu (expected 1 for root)\n", remaining_inodes);
        return -EBUSY; // There are still active inodes besides root
    }
    // Verify the only remaining inode is the root
    if (remaining_inodes == 1) {
        struct vfs_inode *only_inode = HLIST_FIRST_NODE(&sb->inodes, struct vfs_inode, hash_entry);
        if (only_inode != mounted_inode) {
            printf("vfs_unmount: remaining inode is not root (ino=%lu)\n", only_inode->ino);
            return -EBUSY;
        }
    }

    // Destroy root inode's data before freeing
    if (mounted_inode->ops->destroy_inode) {
        mounted_inode->ops->destroy_inode(mounted_inode);
    }
    mounted_inode->valid = 0;
    vfs_remove_inode(sb, mounted_inode);

    // Detach superblock from filesystem type
    __vfs_detach_superblock_from_fstype(sb);
    __vfs_clear_mountpoint(mountpoint);

    // Unlock root inode before freeing (caller expects it unlocked after free)
    vfs_iunlock(mounted_inode);
    mounted_inode->ops->free_inode(mounted_inode);
    sb->root_inode = NULL;

    // Free the superblock (caller must release sb lock before this)
    struct vfs_fs_type *fs_type = sb->fs_type;
    vfs_superblock_unlock(sb);
    fs_type->ops->free(sb);

    return 0; // Successfully unmounted
}

/*
 * vfs_make_orphan - Mark an inode as orphan when it's unlinked but still referenced.
 *
 * Locking:
 *   - Caller must hold the superblock write lock.
 *   - Caller must hold the inode mutex.
 *
 * Returns:
 *   - 0 on success, negative errno on failure.
 */
int vfs_make_orphan(struct vfs_inode *inode) {
    if (inode == NULL) {
        return -EINVAL;
    }
    struct vfs_superblock *sb = inode->sb;
    if (sb == NULL) {
        return -EINVAL;
    }
    
    VFS_SUPERBLOCK_ASSERT_WHOLDING(sb, "Must hold sb wlock to make orphan");
    VFS_INODE_ASSERT_HOLDING(inode, "Must hold inode lock to make orphan");
    
    if (inode->orphan) {
        return 0;  // Already orphan
    }
    if (inode->n_links != 0) {
        return -EINVAL;  // Not unlinked yet
    }
    
    inode->orphan = 1;
    list_node_push(&sb->orphan_list, inode, orphan_entry);
    sb->orphan_count++;
    
    // For backend fs: persist to on-disk orphan journal
    if (sb->ops->add_orphan) {
        int ret = sb->ops->add_orphan(sb, inode);
        if (ret != 0) {
            // Log error but continue - worst case is block leak on crash
            printf("vfs: warning: failed to persist orphan inode %lu, errno=%d\n", 
                   inode->ino, ret);
        }
    }
    
    return 0;
}

/*
 * __vfs_final_unmount_cleanup - Final cleanup after all orphans are gone.
 *
 * Called from vfs_iput when the last orphan inode is freed on a detached fs.
 * This function frees the superblock and its resources.
 */
void __vfs_final_unmount_cleanup(struct vfs_superblock *sb) {
    if (sb == NULL) {
        return;
    }
    
    // Must be detached with no orphans
    assert(!sb->attached, "__vfs_final_unmount_cleanup: sb still attached");
    assert(sb->orphan_count == 0, "__vfs_final_unmount_cleanup: orphans remain");
    
    vfs_mount_lock();
    vfs_superblock_wlock(sb);
    
    // Detach from fs_type if still registered
    if (sb->registered) {
        __vfs_detach_superblock_from_fstype(sb);
    }
    
    // Free root inode if not already freed
    if (sb->root_inode != NULL) {
        struct vfs_inode *rooti = sb->root_inode;
        vfs_ilock(rooti);
        if (rooti->ops->destroy_inode) {
            rooti->ops->destroy_inode(rooti);
        }
        rooti->valid = 0;
        vfs_remove_inode(sb, rooti);
        vfs_iunlock(rooti);
        rooti->ops->free_inode(rooti);
        sb->root_inode = NULL;
    }
    
    struct vfs_fs_type *fs_type = sb->fs_type;
    vfs_superblock_unlock(sb);
    vfs_mount_unlock();
    
    // Free superblock
    fs_type->ops->free(sb);
}

/*
 * vfs_unmount_lazy - Detach filesystem immediately, defer cleanup.
 *
 * Locking:
 *   - Caller holds vfs_mount_lock().
 *   - Caller holds the parent superblock write lock.
 *   - Caller holds the mountpoint inode mutex.
 *
 * Returns:
 *   - 0 on success (filesystem detached, cleanup may be deferred).
 *   - -EBUSY if child mounts exist.
 *   - Other negative errno on failure.
 */
int vfs_unmount_lazy(struct vfs_inode *mountpoint) {
    struct vfs_superblock *sb = NULL;
    struct vfs_superblock *parent_sb = NULL;
    struct vfs_inode *rooti = NULL;
    int ret = 0;

    if (mountpoint == NULL) {
        return -EINVAL;
    }

    if (!holding_mutex(&__mount_mutex)) {
        return -EPERM;
    }
    if (!holding_mutex(&mountpoint->mutex)) {
        return -EPERM;
    }
    
    parent_sb = mountpoint->sb;
    if (parent_sb != NULL && !vfs_superblock_wholding(parent_sb)) {
        return -EPERM;
    }
    
    ret = __vfs_inode_valid(mountpoint);
    if (ret != 0) {
        return ret;
    }
    
    if (!S_ISDIR(mountpoint->mode) || !mountpoint->mount) {
        return -EINVAL;
    }
    
    sb = mountpoint->mnt_sb;
    if (sb == NULL) {
        return -EINVAL;
    }
    
    // Phase 1: Check for child mounts
    vfs_superblock_wlock(sb);
    
    if (vfs_superblock_mountcount(sb) > 0) {
        vfs_superblock_unlock(sb);
        return -EBUSY;  // Child mounts exist
    }
    
    // Set unmounting flag to block new operations
    sb->unmounting = 1;
    
    // Phase 2: Detach from mount tree
    // Note: __vfs_clear_mountpoint already decrements parent's mount count
    __vfs_clear_mountpoint(mountpoint);
    sb->mountpoint = NULL;
    sb->parent_sb = NULL;
    sb->attached = 0;
    sb->valid = 0;  // Prevent new lookups
    
    // Phase 3: Sync if needed (for backend filesystems)
    if (!sb->backendless && sb->dirty) {
        sb->syncing = 1;
        ret = sb->ops->sync_fs(sb, 1);
        sb->syncing = 0;
        if (ret != 0) {
            printf("vfs_unmount_lazy: warning: sync failed, errno=%d\n", ret);
        }
    }
    
    // Call unmount_begin callback
    if (sb->ops->unmount_begin) {
        sb->ops->unmount_begin(sb);
    }
    
    // Phase 4: Mark all referenced inodes as orphans
    // Walk the inode hash and mark referenced inodes as orphans
    rooti = sb->root_inode;
    struct vfs_inode *inode, *tmp_inode;
    hlist_foreach_node_safe(&sb->inodes, inode, tmp_inode, hash_entry) {
        if (inode != rooti && inode->ref_count > 0) {
            // Mark as orphan - will be cleaned up when last ref drops
            if (!inode->orphan) {
                vfs_ilock(inode);
                inode->orphan = 1;
                list_node_push(&sb->orphan_list, inode, orphan_entry);
                sb->orphan_count++;
                vfs_iunlock(inode);
            }
        }
    }
    
    // Phase 5: Check if immediate cleanup possible
    if (sb->orphan_count == 0) {
        // No orphans - cleanup immediately
        __vfs_detach_superblock_from_fstype(sb);
        
        // Free root inode
        if (rooti != NULL) {
            vfs_ilock(rooti);
            if (rooti->ops->destroy_inode) {
                rooti->ops->destroy_inode(rooti);
            }
            rooti->valid = 0;
            vfs_remove_inode(sb, rooti);
            vfs_iunlock(rooti);
            rooti->ops->free_inode(rooti);
            sb->root_inode = NULL;
        }
        
        struct vfs_fs_type *fs_type = sb->fs_type;
        vfs_superblock_unlock(sb);
        fs_type->ops->free(sb);
    } else {
        // Orphans exist - cleanup deferred to vfs_iput
        vfs_superblock_unlock(sb);
    }
    
    return 0;
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

// VFS superblock spinlock protects simple fields that need atomic access
void vfs_superblock_spin_lock(struct vfs_superblock *sb) {
    assert(sb != NULL, "Superblock cannot be NULL when acquiring spinlock");
    spin_acquire(&sb->spinlock);
}

void vfs_superblock_spin_unlock(struct vfs_superblock *sb) {
    assert(sb != NULL, "Superblock cannot be NULL when releasing spinlock");
    spin_release(&sb->spinlock);
}

void vfs_superblock_mountcount_inc(struct vfs_superblock *sb) {
    assert(sb != NULL, "Superblock cannot be NULL when incrementing mount count");
    int cnt = __atomic_add_fetch(&sb->mount_count, 1, __ATOMIC_SEQ_CST);
    assert(cnt > 0, "Superblock mount count overflow");
}

void vfs_superblock_mountcount_dec(struct vfs_superblock *sb) {
    assert(sb != NULL, "Superblock cannot be NULL when decrementing mount count");
    int cnt = __atomic_sub_fetch(&sb->mount_count, 1, __ATOMIC_SEQ_CST);
    assert(cnt >= 0, "Superblock mount count underflow");
    // Note: We don't call vfs_superblock_put here because mount count and
    // refcount are independent. The mount count tracks child mounts, not
    // references to the superblock itself.
}

void vfs_superblock_dup(struct vfs_superblock *sb) {
    assert(sb != NULL, "Superblock cannot be NULL when duplicating");
    int ret = __atomic_add_fetch(&sb->refcount, 1, __ATOMIC_SEQ_CST);
    assert(ret > 0, "Superblock refcount overflow");
}

void vfs_superblock_put(struct vfs_superblock *sb) {
    assert(sb != NULL, "Superblock cannot be NULL when putting");
    assert(!vfs_superblock_wholding(sb), "Cannot put superblock while holding its lock");
    assert(!holding_mutex(&__mount_mutex), "Cannot put superblock while holding mount mutex");
    assert(atomic_dec_unless(&sb->refcount, 0), "Superblock refcount underflow");
}

/*
 * vfs_alloc_inode - Ask the filesystem to allocate a new inode object.
 *
 * Locking:
 *   - Caller must hold the superblock write lock.
 *
 * Returns:
 *   - inode* (locked) on success with refcount=1, or ERR_PTR(errno).
 */
struct vfs_inode *vfs_alloc_inode(struct vfs_superblock *sb) {
    if (sb == NULL) {
        return ERR_PTR(-EINVAL); // Invalid arguments
    }
    VFS_SUPERBLOCK_ASSERT_WHOLDING(sb, "vfs_alloc_inode: must hold superblock write lock");
    if (!sb->valid) {
        return ERR_PTR(-EINVAL); // Superblock is not valid
    }
    struct vfs_inode *inode = sb->ops->alloc_inode(sb);
    if (IS_ERR(inode)) {
        return inode;
    }
    __vfs_inode_init(inode);
retry_add:;
    struct vfs_inode *existing = vfs_add_inode(sb, inode);
    if (IS_ERR_OR_NULL(existing)) {
        if (PTR_ERR(existing) == -EAGAIN) {
            // An inode with the same number is being destroyed.
            // Release sb lock to let destruction complete, then retry.
            vfs_superblock_unlock(sb);
            yield();  // Give the destroying thread a chance to run
            vfs_superblock_wlock(sb);
            if (!sb->valid) {
                inode->ops->free_inode(inode);
                return ERR_PTR(-EINVAL);
            }
            goto retry_add;
        }
        inode->ops->free_inode(inode);
        if (existing == NULL) {
            return ERR_PTR(-ENOENT); // Failed to add inode
        }
        return existing;
    }
    return inode; // locked
}

/*
 * vfs_get_inode - Load an inode from the filesystem driver.
 *
 * Locking:
 *   - Caller must hold the superblock write lock.
 *   - On success, the returned inode is locked and its refcount is set to 1.
 *
 * Returns:
 *   - inode* (locked) on success or ERR_PTR(errno) on failure.
 */
struct vfs_inode *vfs_get_inode(struct vfs_superblock *sb, uint64 ino) {
    if (sb == NULL) {
        return ERR_PTR(-EINVAL); // Invalid arguments
    }
    VFS_SUPERBLOCK_ASSERT_WHOLDING(sb, "vfs_get_inode: must hold superblock write lock");
    if (!sb->valid) {
        return ERR_PTR(-EINVAL); // Superblock is not valid
    }
    struct vfs_inode *inode = sb->ops->get_inode(sb, ino);
    if (IS_ERR(inode)) {
        return inode;
    }
    __vfs_inode_init(inode);
retry_add:;
    struct vfs_inode *existing = vfs_add_inode(sb, inode);
    if (IS_ERR_OR_NULL(existing)) {
        if (PTR_ERR(existing) == -EAGAIN) {
            // An inode with the same number is being destroyed.
            // Release sb lock to let destruction complete, then retry.
            vfs_superblock_unlock(sb);
            yield();  // Give the destroying thread a chance to run
            vfs_superblock_wlock(sb);
            if (!sb->valid) {
                inode->ops->free_inode(inode);
                return ERR_PTR(-EINVAL);
            }
            goto retry_add;
        }
        inode->ops->free_inode(inode);
        if (existing == NULL) {
            return ERR_PTR(-ENOENT); // Failed to add inode
        }
        return existing;
    }
    if (existing != inode) {
        // Found existing inode in hash - free the newly loaded one
        // The existing inode is already locked by vfs_add_inode
        inode->ops->free_inode(inode);
        return existing; // Return the existing inode (locked)
    }
    return inode; // locked
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
 * __vfs_dentry_get_self_inode - Check if dentry->parent is the target inode and return it.
 *
 * For VFS-synthesized entries (e.g., "." or ".." across mount boundaries),
 * dentry->parent may already reference the target inode. This helper checks
 * that condition and returns the inode with an incremented refcount.
 *
 * Locking:
 *   - None required; the parent inode is guaranteed to be alive as long as
 *     the dentry is valid (VFS always caches ancestor directories).
 *
 * Returns:
 *   - Inode pointer with refcount incremented if parent matches the target.
 *   - NULL if parent is not set or does not match the target.
 */
static struct vfs_inode *__vfs_dentry_get_self_inode(struct vfs_dentry *dentry) {
    if (dentry == NULL || dentry->parent == NULL) {
        return NULL;
    }
    if (dentry->parent->sb == dentry->sb &&
        dentry->parent->ino == dentry->ino) {
        vfs_idup(dentry->parent);
        return dentry->parent;
    }
    return NULL;
}

/*
 * __vfs_get_dentry_inode_impl - Internal helper to resolve a dentry to an inode.
 *
 * This is the core implementation that performs cache lookup and, if necessary,
 * upgrades to a write lock to load the inode from disk. It does NOT check for
 * VFS-synthesized self-references (dentry->parent == target); callers must
 * handle that case before calling this function.
 *
 * Locking:
 *   - Caller holds the dentry's superblock read lock on entry. This helper may
 *     drop the read lock and acquire the write lock internally.
 *
 * Returns:
 *   - Inode pointer on success with refcount incremented (inode unlocked).
 *   - ERR_PTR(errno) on failure.
 */
static struct vfs_inode *__vfs_get_dentry_inode_impl(struct vfs_dentry *dentry) {
    struct vfs_inode *inode = NULL;

    inode = vfs_get_inode_cached(dentry->sb, dentry->ino);
    if (!IS_ERR_OR_NULL(inode)) {
        vfs_idup(inode);
        vfs_iunlock(inode);
        return inode;
    }

    if (PTR_ERR(inode) != -ENOENT) {
        return inode;
    }

    if (!vfs_superblock_wholding(dentry->sb)) {
        // Suppose holding read lock
        vfs_superblock_unlock(dentry->sb);
        vfs_superblock_wlock(dentry->sb);
    }

    if (!dentry->sb->valid) {
        return ERR_PTR(-EINVAL);
    }

    inode = vfs_get_inode_cached(dentry->sb, dentry->ino);
    if (!IS_ERR_OR_NULL(inode)) {
        vfs_idup(inode);
        vfs_iunlock(inode);
        return inode;
    }

    if (PTR_ERR(inode) != -ENOENT) {
        return inode;
    }

    inode = vfs_get_inode(dentry->sb, dentry->ino);
    if (IS_ERR_OR_NULL(inode)) {
        return inode;
    }

    vfs_idup(inode);
    vfs_iunlock(inode);
    return inode;
}

/*
 * vfs_get_dentry_inode_locked - Resolve a dentry to an inode within a superblock, populating cache as needed.
 *
 * Locking:
 *   - Caller holds the dentry's superblock read lock on entry. This helper may
 *     drop the read lock and acquire the write lock internally.
 *
 * Returns:
 *   - Inode pointer on success with refcount incremented (inode unlocked).
 *   - ERR_PTR(errno) on failure.
 *
 * Notes:
 *   - Callers must avoid holding inode locks that could deadlock with these
 *     lock transitions and must release the inode via vfs_iput().
 *   - For VFS-synthesized entries (e.g., "." or ".." across mount boundaries),
 *     dentry->parent may already reference the target inode; this helper uses
 *     that shortcut when available.
 */
struct vfs_inode *vfs_get_dentry_inode_locked(struct vfs_dentry *dentry) {
    struct vfs_inode *inode = NULL;
    if (dentry == NULL) {
        return ERR_PTR(-EINVAL);
    }
    if (dentry->sb == NULL) {
        return ERR_PTR(-EINVAL);
    }

    if (!dentry->sb->valid) {
        return ERR_PTR(-EINVAL);
    }

    // Fast path: if dentry->parent is the target inode itself
    // (e.g., "." or ".." synthesized by VFS for mount boundaries),
    // just duplicate the reference instead of cache lookup
    inode = __vfs_dentry_get_self_inode(dentry);
    if (inode != NULL) {
        return inode;
    }

    return __vfs_get_dentry_inode_impl(dentry);
}

/*
 * vfs_get_dentry_inode - Resolve a dentry to an inode, handling cross-filesystem transitions.
 *
 * Locking:
 *   - None required on entry; this helper acquires and releases the dentry's
 *     superblock lock internally.
 *
 * Returns:
 *   - Inode pointer on success with refcount incremented (inode unlocked).
 *   - ERR_PTR(errno) on failure.
 *
 * Notes:
 *   - Handles dentries from vfs_ilookup() and vfs_dir_iter(), including "." and
 *     ".." entries that may cross filesystem boundaries (mount points).
 *   - For cross-filesystem "..", dentry->sb points to the parent filesystem's
 *     superblock, allowing correct resolution.
 *   - Callers must release the inode via vfs_iput().
 */
struct vfs_inode *vfs_get_dentry_inode(struct vfs_dentry *dentry) {
    struct vfs_inode *inode = NULL;
    if (dentry == NULL) {
        return ERR_PTR(-EINVAL);
    }
    if (dentry->sb == NULL) {
        return ERR_PTR(-EINVAL);
    }

    // Fast path: if dentry->parent is the target inode itself
    // (e.g., "." or ".." synthesized by VFS for mount boundaries),
    // just duplicate the reference without acquiring locks
    inode = __vfs_dentry_get_self_inode(dentry);
    if (inode != NULL) {
        return inode;
    }

    vfs_superblock_rlock(dentry->sb);
    if (!dentry->sb->valid) {
        vfs_superblock_unlock(dentry->sb);
        return ERR_PTR(-EINVAL);
    }
    inode = __vfs_get_dentry_inode_impl(dentry);
    vfs_superblock_unlock(dentry->sb);
    return inode;
}

 /******************************************************************************
 * Module scope private functions
 *****************************************************************************/
/*
 * vfs_get_inode_cached - Lookup an inode in a superblock's in-memory cache.
 *
 * @sb:  Superblock to search.
 * @ino: Inode number to look up.
 *
 * Locking:
 *   - Caller holds the superblock read or write lock for the entire call.
 *   - On success, the returned inode is locked; caller must call vfs_iunlock()
 *     when done.
 *
 * Returns:
 *   - A pointer to the cached inode (locked) on success.
 *   - ERR_PTR(-ENOENT) if the inode is not cached or was invalidated.
 *   - ERR_PTR(-EINVAL) if @sb is NULL or the superblock is not valid.
 */
struct vfs_inode *vfs_get_inode_cached(struct vfs_superblock *sb, uint64 ino) {
    if (sb == NULL) {
        return ERR_PTR(-EINVAL); // Invalid arguments
    }
    if (!sb->valid) {
        return ERR_PTR(-EINVAL); // Superblock is not valid
    }
    struct vfs_inode *inode = __vfs_inode_hash_get(sb, ino);
    if (inode == NULL) {
        return ERR_PTR(-ENOENT); // Inode not found
    }
    vfs_ilock(inode);
    if (!inode->valid || inode->destroying) {
        // Inode should be valid when first gotten from the cache,
        // but it may have been invalidated or is being destroyed.
        // In this case, the inode should be treated as not found.
        vfs_iunlock(inode);
        return ERR_PTR(-ENOENT); // Inode is not valid or being destroyed
    }
    return inode;
}

/*
 * vfs_add_inode - Insert a newly loaded inode into the cache.
 *
 * @sb:    Superblock that owns the inode cache.
 * @inode: Newly allocated inode to insert (must have inode->ino set,
 *         inode->sb == NULL, and inode->valid == 0).
 *
 * If an inode with the same number already exists in the cache, the existing
 * inode is returned instead and @inode is left unchanged (caller should free it).
 *
 * Locking:
 *   - Caller holds the superblock write lock.
 *   - On success, the returned inode is locked; caller must call vfs_iunlock()
 *     when done.
 *
 * Returns:
 *   - A pointer to the canonical inode (locked) on success. This is @inode
 *     itself if newly inserted, or the pre-existing cached inode if a
 *     duplicate was found.
 *   - ERR_PTR(-EINVAL) if @sb or @inode is NULL, the superblock is invalid,
 *     @inode->sb is already set, or @inode->valid is already true.
 */
struct vfs_inode *vfs_add_inode(struct vfs_superblock *sb,
                                struct vfs_inode *inode) {
    if (sb == NULL || inode == NULL) {
        return ERR_PTR(-EINVAL); // Invalid arguments
    }
    VFS_SUPERBLOCK_ASSERT_WHOLDING(sb, "Superblock lock must be write held to add inode");
    if (!sb->valid && sb->initialized) {
        return ERR_PTR(-EINVAL); // Superblock is not valid
    }
    if (inode->sb != NULL) {
        return ERR_PTR(-EINVAL); // Inode's superblock does not match
    }
    if (inode->valid) {
        return ERR_PTR(-EINVAL); // Inode to add must not be valid yet
    }
    struct vfs_inode *existing = __vfs_inode_hash_get(sb, inode->ino);
    if (existing != NULL) {
        // Check if the existing inode is being destroyed.
        // We check the flag WITHOUT locking the inode to avoid deadlock:
        // - vfs_iput holds inode lock, releases sb lock, calls destroy_inode
        // - We hold sb lock, if we tried to lock inode we'd deadlock
        // The destroying flag is set while holding sb lock + inode lock,
        // so if it's set and we hold sb lock, the destroying thread has
        // released sb lock and is in destroy_inode.
        if (existing->destroying) {
            // Inode is being destroyed. The destroying thread will remove it
            // from cache once it re-acquires sb lock (which we currently hold).
            // Return EAGAIN so caller can release sb lock and retry.
            return ERR_PTR(-EAGAIN);
        }
        // When existing inode is found and not being destroyed, lock and return it
        vfs_ilock(existing);
        // Double-check after locking in case it started destroying
        if (existing->destroying || !existing->valid) {
            vfs_iunlock(existing);
            return ERR_PTR(-EAGAIN);
        }
        return existing; // Inode with the same number already exists
    }
    struct vfs_inode *popped = __vfs_inode_hash_add(sb, inode);
    if (popped != NULL) {
        // At this point, something is wrong in the hash list implementation
        panic("vfs_add_inode: inode hash add returned existing inode unexpectedly");
    }
    inode->valid = 1; // Mark inode as valid
    inode->sb = sb; // Associate inode with superblock
    vfs_ilock(inode);
    return inode;
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
    // Allow removal from detached superblocks (lazy unmount cleanup)
    if (!sb->valid && sb->attached) {
        return -EINVAL; // Superblock is not valid and still attached
    }
    
    // If inode was already destroyed (n_links == 0 and destroy_inode called),
    // valid is already 0. Just remove from hash and clear sb.
    bool already_destroyed = !inode->valid;
    
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
    
    if (!already_destroyed) {
        // Normal cache eviction - mark invalid but data may still be on disk
        inode->valid = 0;
    }
    // For destroyed inodes, valid is already 0
    
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

struct fs_struct *vfs_struct_init(void) {
    struct fs_struct *fs = __vfs_struct_alloc_init();
    assert(fs != NULL, "idle_proc_init: failed to create fs_struct");
    smp_store_release(&fs->ref_count, 1);
    spin_init(&fs->lock, "fs_struct_lock");
    fs->rooti.sb = NULL;
    fs->rooti.inode = NULL;
    fs->cwd.sb = NULL;
    fs->cwd.inode = NULL;
    return fs;
}

struct fs_struct *vfs_struct_clone(struct fs_struct *old_fs, uint64 clone_flags) {
    if (old_fs == NULL) {
        return ERR_PTR(-EINVAL);
    }

    if (clone_flags & CLONE_FS) {
        // share the fs_struct
        atomic_inc(&old_fs->ref_count);
        return old_fs;
    }

    struct fs_struct *new_fs = __vfs_struct_alloc_init();
    if (new_fs == NULL) {
        return ERR_PTR(-ENOMEM);
    }

    vfs_struct_lock(old_fs);
    // Clone root and cwd
    int ret = 0;
    struct vfs_inode *rooti = vfs_inode_deref(&old_fs->rooti);
    if (rooti) {
        ret = vfs_inode_get_ref(rooti, &new_fs->rooti);
        if (ret != 0) {
            goto out_locked;
        }
    }
    struct vfs_inode *cwdi = vfs_inode_deref(&old_fs->cwd);
    if (cwdi) {
        ret = vfs_inode_get_ref(cwdi, &new_fs->cwd);
        if (ret != 0) {
            goto out_locked;
        }
    }
    ret = 0;
out_locked:
    vfs_struct_unlock(old_fs);
    if (ret != 0) {
        vfs_inode_put_ref(&new_fs->rooti);
        vfs_inode_put_ref(&new_fs->cwd);
        __vfs_struct_free(new_fs);
        return ERR_PTR(ret);
    }
    return new_fs;
}

void vfs_struct_put(struct fs_struct *fs) {
    if (fs == NULL) {
        return;
    }
    if (!atomic_dec_unless(&fs->ref_count, 1)) {
        // Release root and cwd inodes
        vfs_inode_put_ref(&fs->rooti);
        vfs_inode_put_ref(&fs->cwd);
        __vfs_struct_free(fs);
    }
}

int vfs_inode_get_ref(struct vfs_inode *inode, struct vfs_inode_ref *ref) {
    if (inode == NULL || ref == NULL) {
        return -EINVAL;
    }
    struct vfs_superblock *sb = inode->sb;
    if (!inode->valid || sb == NULL || !sb->valid) {
        return -EINVAL; // Inode is not valid
    }
    vfs_superblock_dup(sb);
    vfs_idup(inode);
    ref->sb = sb;
    ref->inode = inode;
    return 0;
}

void vfs_inode_put_ref(struct vfs_inode_ref *ref) {
    if (ref == NULL) {
        return;
    }
    if (ref->inode) {
        vfs_iput(ref->inode);
        ref->inode = NULL;
    }
    if (ref->sb) {
        vfs_superblock_put(ref->sb);
        ref->sb = NULL;
    }
}

struct vfs_inode *vfs_inode_deref(struct vfs_inode_ref *ref) {
    if (ref == NULL) {
        return NULL;
    }
    return ref->inode;
}
