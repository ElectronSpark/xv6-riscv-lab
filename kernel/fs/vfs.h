#ifndef __KERNEL_VFS_H
#define __KERNEL_VFS_H

#include "fs/vfs_types.h"
#include "fcntl.h"

void vfs_init(void);
int vfs_register_fs_type(const char *name, uint64 f_type, struct fs_type_ops *ops);
void vfs_freeze_fs_type(struct fs_type *fs_type);
void vfs_unregister_fs_type(struct fs_type *fs_type);
struct fs_type *vfs_get_fs_type(uint64 f_type);
int vfs_sb_refinc(struct super_block *sb);
int vfs_sb_refdec(struct super_block *sb);

int vfs_mount(struct vfs_inode *inode, dev_t dev);
void vfs_mount_root(dev_t dev, uint64 f_type);
int vfs_umount(struct super_block *sb);
int vfs_mounted_root(struct vfs_mount_point *mp, struct vfs_inode **reti);

int fcntl_flags_from_string(const char *flags);
int vfs_namex(const char *path, size_t len, struct vfs_inode **reti, 
              struct vfs_inode *base, int max_follow);

/***************************** General file operations *****************************/
// The following functions partly refer to lwext4's file operations.
int vfs_isopen(struct vfs_file *file);
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
int vfs_dopen(struct vfs_file *file, struct vfs_dirent *dirent);
int vfs_dnext(struct vfs_dirent *dirent);
int vfs_drewind(struct vfs_dirent *dirent);

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
static inline bool vfs_iholding(struct vfs_inode *inode) {
    assert(inode->ops, "vfs_iholding called on an inode without operations");
    assert(inode->ops->iholding, "vfs_iholding called on an inode without iholding operation");
    return inode->ops->iholding(inode);
}

static inline struct vfs_inode *vfs_idup(struct vfs_inode *inode) {
    assert(inode->ops, "vfs_idup called on an inode without operations");
    assert(inode->ops->idup, "vfs_idup called on an inode without idup operation");
    assert(vfs_iholding(inode), "vfs_idup called on an unheld inode");
    assert(vfs_holdingfs(inode->sb), "vfs_idup called on an inode with an unheld superblock");
    struct vfs_inode *ret = inode->ops->idup(inode);
    if (ret != NULL) {
        inode->sb->ref++;
    }
    return ret;
}

static inline void vfs_iput(struct vfs_inode *inode) {
    assert(inode->ops, "vfs_iput called on an inode without operations");
    assert(inode->ops->iput, "vfs_iput called on an inode without iput operation");
    assert(vfs_iholding(inode), "vfs_iput called on an unheld inode");
    assert(vfs_holdingfs(inode->sb), "vfs_iput called on an inode with an unheld superblock");
    assert(inode->ref > 0, "vfs_iput called on an inode with zero reference count");
    inode->ops->iput(inode);
    inode->sb->ref--;
}

static inline void vfs_isync(struct vfs_inode *inode) {
    assert(inode->ops, "vfs_isync called on an inode without operations");
    assert(inode->ops->isync, "vfs_isync called on an inode without isync operation");
    inode->ops->isync(inode);
}

static inline void vfs_ilock(struct vfs_inode *inode) {
    assert(inode->ops, "vfs_ilock called on an inode without operations");
    assert(inode->ops->ilock, "vfs_ilock called on an inode without ilock operation");
    inode->ops->ilock(inode);
}

static inline void vfs_iunlock(struct vfs_inode *inode) {
    assert(inode->ops, "vfs_iunlock called on an inode without operations");
    assert(inode->ops->iunlock, "vfs_iunlock called on an inode without iunlock operation");
    inode->ops->iunlock(inode);
}

static inline void vfs_idirty(struct vfs_inode *inode) {
    assert(inode->ops, "vfs_idirty called on an inode without operations");
    assert(inode->ops->idirty, "vfs_idirty called on an inode without idirty operation");
    inode->ops->idirty(inode);
}

static inline int vfs_validate_inode(struct vfs_inode *inode) {
    assert(inode->ops, "vfs_validate_inode called on an inode without operations");
    assert(inode->ops->validate, "vfs_validate_inode called on an inode without validate operation");
    return inode->ops->validate(inode);
}

static inline ssize_t vfs_iread(struct vfs_inode *inode, void *buf, size_t size, loff_t offset) {
    assert(inode->ops, "vfs_iread called on an inode without operations");
    assert(inode->ops->iread, "vfs_iread called on an inode without iread operation");
    return inode->ops->iread(inode, buf, size, offset);
}

static inline ssize_t vfs_iwrite(struct vfs_inode *inode, const void *buf, size_t size, loff_t offset) {
    assert(inode->ops, "vfs_iwrite called on an inode without operations");
    assert(inode->ops->iwrite, "vfs_iwrite called on an inode without iwrite operation");
    return inode->ops->iwrite(inode, buf, size, offset);
}

static inline int vfs_itruncate(struct vfs_inode *inode, loff_t length) {
    assert(inode->ops, "vfs_itruncate called on an inode without operations");
    assert(inode->ops->itruncate, "vfs_itruncate called on an inode without itruncate operation");
    return inode->ops->itruncate(inode, length);
}

static inline int64 vfs_ibmap(struct vfs_inode *inode, uint64 block) {
    assert(inode->ops, "vfs_ibmap called on an inode without operations");
    assert(inode->ops->bmap, "vfs_ibmap called on an inode without bmap operation");
    return inode->ops->bmap(inode, block);
}

static inline int vfs_iopen(struct vfs_inode *inode, struct vfs_file *file) {
    assert(inode->ops, "vfs_iopen called on an inode without operations");
    assert(inode->ops->open, "vfs_iopen called on an inode without open operation");
    return inode->ops->open(inode, file);
}

static inline int vfs_iclose(struct vfs_inode *inode, struct vfs_file *file) {
    assert(inode->ops, "vfs_iclose called on an inode without operations");
    assert(inode->ops->close, "vfs_iclose called on an inode without close operation");
    return inode->ops->close(inode, file);
}

static inline int vfs_isymlink(struct vfs_inode *inode, const char *target, size_t target_len) {
    assert(inode->ops, "vfs_isymlink called on an inode without operations");
    assert(inode->ops->isymlink, "vfs_isymlink called on an inode without isymlink operation");
    return inode->ops->isymlink(inode, target, target_len);
}

static inline ssize_t vfs_ireadlink(struct vfs_inode *inode, char *buf, size_t bufsize) {
    assert(inode->ops, "vfs_ireadlink called on an inode without operations");
    assert(inode->ops->ireadlink, "vfs_ireadlink called on an inode without ireadlink operation");
    return inode->ops->ireadlink(inode, buf, bufsize);
}

// directory operations wrappers
static inline struct vfs_inode *vfs_dlookup(struct vfs_inode *inode, const char *name, size_t len) {
    assert(inode->ops, "vfs_dlookup called on an inode without operations");
    assert(inode->ops->d_lookup, "vfs_dlookup called on an inode without d_lookup operation");
    return inode->ops->d_lookup(inode, name, len);
}

static inline int vfs_dlink(struct vfs_inode *base, const char *name, size_t namelen, struct vfs_inode *inode) {
    assert(base->ops, "vfs_dlink called on an inode without operations");
    assert(base->ops->d_link, "vfs_dlink called on an inode without d_link operation");
    return base->ops->d_link(base, name, namelen, inode);
}

static inline int vfs_dunlink(struct vfs_inode *base, const char *name, size_t namelen) {
    assert(base->ops, "vfs_dunlink called on an inode without operations");
    assert(base->ops->d_unlink, "vfs_dunlink called on an inode without d_unlink operation");
    return base->ops->d_unlink(base, name, namelen);
}

static inline int vfs_dmknod(struct vfs_inode *inode, int type, dev_t dev) {
    assert(inode->ops, "vfs_dmknod called on an inode without operations");
    assert(inode->ops->d_mknod, "vfs_dmknod called on an inode without d_mknod operation");
    return inode->ops->d_mknod(inode, type, dev);
}

static inline int vfs_dmkdir(struct vfs_inode *inode) {
    assert(inode->ops, "vfs_dmkdir called on an inode without operations");
    assert(inode->ops->d_mkdir, "vfs_dmkdir called on an inode without d_mkdir operation");
    return inode->ops->d_mkdir(inode);
}

static inline int vfs_drmdir(struct vfs_inode *inode) {
    assert(inode->ops, "vfs_drmdir called on an inode without operations");
    assert(inode->ops->d_rmdir, "vfs_drmdir called on an inode without d_rmdir operation");
    return inode->ops->d_rmdir(inode);
}

static inline int vfs_dmount(struct vfs_inode *inode, struct super_block *sb) {
    assert(inode->ops, "vfs_dmount called on an inode without operations");
    assert(inode->ops->d_mount, "vfs_dmount called on an inode without d_mount operation");
    return inode->ops->d_mount(inode, sb);
}

static inline int vfs_dumount(struct vfs_inode *inode) {
    assert(inode->ops, "vfs_dumount called on an inode without operations");
    assert(inode->ops->d_umount, "vfs_dumount called on an inode without d_umount operation");
    return inode->ops->d_umount(inode);
}

#endif // __KERNEL_VFS_H
