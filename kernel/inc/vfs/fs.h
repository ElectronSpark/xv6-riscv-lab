#ifndef __KERNEL_VIRTUAL_FILE_SYSTEM_FS_H
#define __KERNEL_VIRTUAL_FILE_SYSTEM_FS_H

#include "vfs/vfs_types.h"
#include "printf.h"
#include "atomic.h"
#include "clone_flags.h"

#define VFS_PATH_MAX 65535
#define VFS_INODE_MAX_REFCOUNT 0x7FFF0000

void vfs_init(void);

// Filesystem type registration
struct vfs_fs_type *vfs_fs_type_allocate(void);
void vfs_fs_type_free(struct vfs_fs_type *fs_type);
int vfs_register_fs_type(struct vfs_fs_type *fs_type);
int vfs_unregister_fs_type(const char *name);
void vfs_mount_lock(void);
void vfs_mount_unlock(void);
struct vfs_fs_type *vfs_get_fs_type(const char *name);
void vfs_put_fs_type(struct vfs_fs_type *fs_type);

// Mount and unmount filesystems
int vfs_mount(const char *type, struct vfs_inode *mountpoint,
              struct vfs_inode *device, int flags, const char *data);
int vfs_mount_path(const char *fstype, const char *target, int target_len,
                   const char *source, int source_len);
int vfs_unmount(struct vfs_inode *mountpoint);
int vfs_unmount_lazy(struct vfs_inode *mountpoint);
int vfs_umount_path(const char *target, int target_len);

// superblock operations
void vfs_superblock_rlock(struct vfs_superblock *sb);
void vfs_superblock_wlock(struct vfs_superblock *sb);
bool vfs_superblock_wholding(struct vfs_superblock *sb);
void vfs_superblock_unlock(struct vfs_superblock *sb);
void vfs_superblock_spin_lock(struct vfs_superblock *sb);
void vfs_superblock_spin_unlock(struct vfs_superblock *sb);
void vfs_superblock_mountcount_inc(struct vfs_superblock *sb);
void vfs_superblock_mountcount_dec(struct vfs_superblock *sb);
void vfs_superblock_dup(struct vfs_superblock *sb);
void vfs_superblock_put(struct vfs_superblock *sb);

struct vfs_inode *vfs_alloc_inode(struct vfs_superblock *sb);
struct vfs_inode *vfs_get_inode(struct vfs_superblock *sb, uint64 ino);
int vfs_sync_superblock(struct vfs_superblock *sb, int wait);

// inode operations
// Inode lifetime is managed via a reference count.
// - Any code that uses a struct vfs_inode must hold a reference to it.
// - Functions that return an inode pointer return it with a reference held.
// - Use vfs_idup() to take an additional reference; pair it with vfs_iput().
// - Do not access an inode after calling the last vfs_iput() on it.

void vfs_ilock(struct vfs_inode *inode);
void vfs_iunlock(struct vfs_inode *inode);
void vfs_idup(struct vfs_inode *inode);         // Increase ref count
void vfs_iput(struct vfs_inode *inode);         // Decrease ref count. Caller must not hold inode lock when calling
int vfs_invalidate(struct vfs_inode *inode);    // Decrease ref count and invalidate inode
int vfs_dirty_inode(struct vfs_inode *inode);   // Mark inode as dirty
int vfs_sync_inode(struct vfs_inode *inode);    // Write inode to disk

int vfs_ilookup(struct vfs_inode *dir, struct vfs_dentry *dentry, 
                const char *name, size_t name_len);
ssize_t vfs_readlink(struct vfs_inode *inode, char *buf, size_t buflen);
struct vfs_inode *vfs_create(struct vfs_inode *dir, mode_t mode,
                            const char *name, size_t name_len);
struct vfs_inode *vfs_mknod(struct vfs_inode *dir, mode_t mode, 
                           dev_t dev, const char *name, size_t name_len);
int vfs_link(struct vfs_dentry *old, struct vfs_inode *dir,
             const char *name, size_t name_len);
int vfs_unlink(struct vfs_inode *dir, const char *name, size_t name_len);
struct vfs_inode *vfs_mkdir(struct vfs_inode *dir, mode_t mode,
                           const char *name, size_t name_len);
int vfs_rmdir(struct vfs_inode *dir, const char *name, size_t name_len);
int vfs_move(struct vfs_inode *old_dir, struct vfs_dentry *old_dentry,
             struct vfs_inode *new_dir, const char *name, size_t name_len);
struct vfs_inode *vfs_symlink(struct vfs_inode *dir, mode_t mode,
                              const char *name, size_t name_len,
                              const char *target, size_t target_len);
int vfs_itruncate(struct vfs_inode *inode, loff_t new_size);
int vfs_dir_iter(struct vfs_inode *dir, struct vfs_dir_iter *iter, 
                 struct vfs_dentry *ret_dentry);

// Special inode locking operations for deadlock avoidance
void vfs_ilock_two_nondirectories(struct vfs_inode *inode1, struct vfs_inode *inode2);
int vfs_ilock_two_directories(struct vfs_inode *inode1, struct vfs_inode *inode2);
void vfs_iunlock_two(struct vfs_inode *inode1, struct vfs_inode *inode2);

// Public APIs not tied to specific callbacks
struct vfs_inode *vfs_namei(const char *path, size_t path_len);
struct vfs_inode *vfs_nameiparent(const char *path, size_t path_len,
                                   char *name, size_t name_size);
struct vfs_inode *vfs_curdir(void);
struct vfs_inode *vfs_curroot(void);
int vfs_chroot(struct vfs_inode *new_root);
int vfs_chdir(struct vfs_inode *new_cwd);
struct vfs_inode *vfs_get_dentry_inode_locked(struct vfs_dentry *dentry);
struct vfs_inode *vfs_get_dentry_inode(struct vfs_dentry *dentry);
void vfs_release_dentry(struct vfs_dentry *dentry);
int vfs_superblock_set_dirty(struct vfs_superblock *sb);

// fs_struct locking operations
// fs_struct lock should be the least significant lock to avoid deadlocks
static inline void vfs_struct_lock(struct fs_struct *fs) {
    spin_acquire(&fs->lock);
}

static inline void vfs_struct_unlock(struct fs_struct *fs) {
    spin_release(&fs->lock);
}

struct fs_struct *vfs_struct_init(void);    // Create the first fs_struct
struct fs_struct *vfs_struct_clone(struct fs_struct *old_fs, uint64 clone_flags);
void vfs_struct_put(struct fs_struct *fs);

// Inode reference operations
int vfs_inode_get_ref(struct vfs_inode *inode, struct vfs_inode_ref *ref);
void vfs_inode_put_ref(struct vfs_inode_ref *ref);
struct vfs_inode *vfs_inode_deref(struct vfs_inode_ref *ref);


// Get the reference count of an inode
static inline int vfs_inode_refcount(struct vfs_inode *inode) {
    if (inode == NULL) {
        return -1; // Invalid argument
    }
    return __atomic_load_n(&inode->ref_count, __ATOMIC_SEQ_CST);
}

// Get the reference count of a superblock
static inline int vfs_superblock_refcount(struct vfs_superblock *sb) {
    if (sb == NULL) {
        return -1; // Invalid argument
    }
    return __atomic_load_n(&sb->refcount, __ATOMIC_SEQ_CST);
}

// Get the mount count of a superblock
static inline int vfs_superblock_mountcount(struct vfs_superblock *sb) {
    if (sb == NULL) {
        return -1; // Invalid argument
    }
    return __atomic_load_n(&sb->mount_count, __ATOMIC_SEQ_CST);
}

// Check whether an inode is the local root
static inline bool vfs_inode_is_local_root(struct vfs_inode *inode) {
    if (inode == NULL || inode->sb == NULL) {
        return false;
    }

    if (inode == inode->sb->root_inode) {
        assert(inode->parent == inode, "vfs_inode_is_local_root: root inode's parent is not itself");
        return true;
    }
    return false;
}


#endif // __KERNEL_VIRTUAL_FILE_SYSTEM_FS_H
