#ifndef __KERNEL_VFS_H
#define __KERNEL_VFS_H

#include "fs/vfs_types.h"
#include "fcntl.h"

void vfs_init(void);
int vfs_register_fs_type(const char *name, uint64 f_type, struct fs_type_ops *ops);
void vfs_freeze_fs_type(struct fs_type *fs_type);
void vfs_unregister_fs_type(struct fs_type *fs_type);
struct fs_type *vfs_get_fs_type(uint64 f_type);

int vfs_mount(struct vfs_dentry *dentry, dev_t dev);
void vfs_mount_root(dev_t dev, uint64 f_type);
int vfs_umount(struct super_block *sb);
int vfs_mounted_root(struct vfs_mount_point *mp, struct vfs_dentry **ret_d);

int vfs_dlookup(struct vfs_dentry *dentry, const char *name, 
                size_t len, bool create, struct vfs_dentry **ret_dentry);
int vfs_dentry_put(struct vfs_dentry *dentry, struct vfs_dentry *base, bool including_base);
int vfs_dentry_sb(struct vfs_dentry *dentry, struct super_block **ret_sb);

int fcntl_flags_from_string(const char *flags);
int vfs_namex(const char *path, size_t len, struct vfs_dentry **retd, 
              struct vfs_inode **reti, struct vfs_dentry *base, 
              int max_follow);

/***************************** General file operations *****************************/
// The following functions partly refer to lwext4's file operations.
int vfs_fopen(struct vfs_file *file, const char *path, const char *flags);
int vfs_fopen2(struct vfs_file *file, const char *path, int flags);
int vfs_fclose(struct vfs_file *file);
int vfs_ftruncate(struct vfs_file *file, loff_t length);
int vfs_fread(struct vfs_file *file, void *buf, size_t size, size_t *rcnt);
int vfs_fwrite(struct vfs_file *file, const void *buf, size_t size, size_t *wcnt);
loff_t vfs_ftell(struct vfs_file *file);
int vfs_fseek(struct vfs_file *file, loff_t offset, int whence);
int vfs_fsize(struct vfs_file *file);
int vfs_fsymlink(const char *target, const char *path);
int vfs_freadlink(const char *path, char *buf, size_t bufsize, size_t *rcnt);
int vfs_fmknod(const char *path, int type, dev_t dev);
int vfs_fhardlink(const char *oldpath, const char *newpath);
int vfs_fremove(const char *path);
int vfs_frename(const char *oldpath, const char *newpath);

/***************************** General directory operations *****************************/
// The following functions also partly refer to lwext4's directory operations.
int vfs_dir_remove(const char *path);
int vfs_dir_rename(const char *oldpath, const char *newpath);
int vfs_mkdir(const char *path);
int vfs_rmdir(const char *path, bool recursive);
int vfs_dopen(struct vfs_file *file, struct vfs_dentry *dentry);
struct vfs_dentry *vfs_dnext(struct vfs_file *file, struct vfs_dirent *dirent);
int vfs_drewind(struct vfs_file *file, struct vfs_dirent *dentry);

/***************************** super block operations wrappers *****************************/
static inline struct vfs_inode *vfs_ialloc(struct super_block *sb) {
    return sb->ops ? sb->ops->ialloc(sb) : 0;
}

static inline struct vfs_inode *vfs_iget(struct super_block *sb, uint64 inum) {
    return sb->ops ? sb->ops->iget(sb, inum) : 0;
}

static inline void vfs_idestroy(struct vfs_inode *inode) {
    if (inode->sb->ops && inode->sb->ops->idestroy)
        inode->sb->ops->idestroy(inode);
}

static inline void vfs_lockfs(struct super_block *sb) {
    if (sb->ops && sb->ops->lockfs)
        sb->ops->lockfs(sb);
}

static inline void vfs_unlockfs(struct super_block *sb) {
    if (sb->ops && sb->ops->unlockfs)
        sb->ops->unlockfs(sb);
}

static inline int vfs_holdingfs(struct super_block *sb) {
    return sb->ops ? sb->ops->holdingfs(sb) : -1;
}

static inline int vfs_syncfs(struct super_block *sb) {
    return sb->ops ? sb->ops->syncfs(sb) : -1;
}

static inline int vfs_freezefs(struct super_block *sb) {
    return sb->ops ? sb->ops->freezefs(sb) : -1;
}

static inline int vfs_statfs(struct super_block *sb, struct statfs *buf) {
    return sb->ops ? sb->ops->statfs(sb, buf) : -1;
}

// inode operations wrappers
static inline struct vfs_inode *vfs_idup(struct vfs_inode *inode) {
    return inode->ops ? inode->ops->idup(inode) : 0;
}

static inline void vfs_iput(struct vfs_inode *inode) {
    if (inode->ops && inode->ops->iput)
        inode->ops->iput(inode);
}

static inline void vfs_isync(struct vfs_inode *inode) {
    if (inode->ops && inode->ops->isync)
        inode->ops->isync(inode);
}

static inline void vfs_ilock(struct vfs_inode *inode) {
    if (inode->ops && inode->ops->ilock)
        inode->ops->ilock(inode);
}

static inline void vfs_iunlock(struct vfs_inode *inode) {
    if (inode->ops && inode->ops->iunlock)
        inode->ops->iunlock(inode);
}

static inline void vfs_idirty(struct vfs_inode *inode) {
    if (inode->ops && inode->ops->idirty)
        inode->ops->idirty(inode);
}

static inline ssize_t vfs_iread(struct vfs_inode *inode, void *buf, size_t size, loff_t offset) {
    return inode->ops ? inode->ops->iread(inode, buf, size, offset) : -1;
}

static inline ssize_t vfs_iwrite(struct vfs_inode *inode, const void *buf, size_t size, loff_t offset) {
    return inode->ops ? inode->ops->iwrite(inode, buf, size, offset) : -1;
}

static inline int vfs_itruncate(struct vfs_inode *inode, loff_t length) {
    return inode->ops ? inode->ops->itruncate(inode, length) : -1;
}

static inline int64 vfs_ibmap(struct vfs_inode *inode, uint64 block) {
    return inode->ops ? inode->ops->bmap(inode, block) : -1;
}

static inline int vfs_iopen(struct vfs_inode *inode, struct vfs_file *file) {
    return inode->ops ? inode->ops->open(inode, file) : -1;
}

static inline int vfs_iclose(struct vfs_inode *inode, struct vfs_file *file) {
    return inode->ops ? inode->ops->close(inode, file) : -1;
}

static inline int vfs_isymlink(struct vfs_inode *inode, const char *target, size_t target_len) {
    return inode->ops ? inode->ops->isymlink(inode, target, target_len) : -1;
}

static inline ssize_t vfs_ireadlink(struct vfs_inode *inode, char *buf, size_t bufsize) {
    return inode->ops ? inode->ops->ireadlink(inode, buf, bufsize) : -1;
}

// dentry operations wrappers
static inline struct vfs_dentry *vfs_d_lookup(struct vfs_dentry *dentry, const char *name, size_t len, bool create) {
    return dentry->ops ? dentry->ops->d_lookup(dentry, name, len, create) : NULL;
}

static inline int vfs_d_link(struct vfs_dentry *dentry, struct vfs_inode *inode) {
    return dentry->ops ? dentry->ops->d_link(dentry, inode) : -1;
}

static inline int vfs_d_unlink(struct vfs_dentry *dentry) {
    return dentry->ops ? dentry->ops->d_unlink(dentry) : -1;
}

static inline int vfs_d_mknod(struct vfs_dentry *dentry, struct vfs_inode *inode, int type, dev_t dev) {
    return dentry->ops ? dentry->ops->d_mknod(dentry, inode, type, dev) : -1;
}

static inline int vfs_d_mkdir(struct vfs_dentry *dentry, struct vfs_inode *inode) {
    return dentry->ops ? dentry->ops->d_mkdir(dentry, inode) : -1;
}

static inline int vfs_d_rmdir(struct vfs_dentry *dentry) {
    return dentry->ops ? dentry->ops->d_rmdir(dentry) : -1;
}

static inline int vfs_d_rename(struct vfs_dentry *old_dentry, struct vfs_dentry *new_dentry) {
    return old_dentry->ops ? old_dentry->ops->d_rename(old_dentry, new_dentry) : -1;
}

static inline ht_hash_t vfs_d_hash(struct vfs_dentry *dentry, const char *name, size_t len) {
    return dentry->ops ? dentry->ops->d_hash(dentry, name, len) : 0;
}

static inline int vfs_d_compare(const struct vfs_dentry *dentry, const char *name, size_t len) {
    return dentry->ops ? dentry->ops->d_compare(dentry, name, len) : -1;
}

static inline void vfs_d_sync(struct vfs_dentry *dentry) {
    if (dentry->ops && dentry->ops->d_sync)
        dentry->ops->d_sync(dentry);
}

static inline int vfs_d_validate(struct vfs_dentry *dentry) {
    if (dentry->ops && dentry->ops->d_invalidate) {
        return dentry->ops->d_validate(dentry);
    }
    if (dentry->valid) {
        return 0; // Already valid
    }
    return -1;
}

static inline void vfs_d_invalidate(struct vfs_dentry *dentry) {
    if (dentry->ops && dentry->ops->d_invalidate)
        dentry->ops->d_invalidate(dentry);
}

static inline struct vfs_inode *vfs_d_inode(struct vfs_dentry *dentry) {
    return dentry->ops ? dentry->ops->d_inode(dentry) : NULL;
}

static inline bool vfs_d_is_symlink(struct vfs_dentry *dentry) {
    return dentry->ops ? dentry->ops->d_is_symlink(dentry) : false;
}

#endif // __KERNEL_VFS_H
