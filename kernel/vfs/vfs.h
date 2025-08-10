#ifndef __KERNEL_VFS_H
#define __KERNEL_VFS_H

#include <vfs/vfs_types.h>
#include <fcntl.h>

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

void vfs_begin_op(struct super_block *sb);
void vfs_end_op(struct super_block *sb);
void vfs_ibegin_op(struct vfs_inode *inode);
void vfs_iend_op(struct vfs_inode *inode);

/***************************** General file operations *****************************/
// The following functions partly refer to lwext4's file operations.
int vfs_isopen(struct vfs_file *file);
int vfs_fopen(struct vfs_file *file, const char *path, const char *flags);
int vfs_fopen2(struct vfs_file *file, const char *path, int flags);
int vfs_fclose(struct vfs_file *file);
struct vfs_file *vfs_filedup(struct vfs_file *file);
int vfs_ftruncate(struct vfs_file *file, loff_t length);
int vfs_fread(struct vfs_file *file, void *buf, size_t size, size_t *rcnt);
int vfs_fwrite(struct vfs_file *file, const void *buf, size_t size, size_t *wcnt);
loff_t vfs_ftell(struct vfs_file *file);
int vfs_fseek(struct vfs_file *file, loff_t offset, int whence);
int vfs_fsize(struct vfs_file *file);
int vfs_fstat(struct vfs_file *file, struct stat *buf);
// int vfs_fsymlink(const char *target, const char *path);
// int vfs_freadlink(const char *path, char *buf, size_t bufsize, size_t *rcnt);
// int vfs_fmknod(const char *path, int type, dev_t dev);
// int vfs_fhardlink(const char *oldpath, const char *newpath);
// int vfs_fremove(const char *path);
// int vfs_frename(const char *oldpath, const char *newpath);

/***************************** General directory operations *****************************/
// The following functions also partly refer to lwext4's directory operations.
int vfs_dir_remove(const char *path);
int vfs_dir_rename(const char *oldpath, const char *newpath);
int vfs_mkdir(const char *path);
int vfs_rmdir(const char *path, bool recursive);
int vfs_dopen(struct vfs_file *file, struct vfs_dirent *dirent);
int vfs_dnext(struct vfs_dirent *dirent);
int vfs_drewind(struct vfs_dirent *dirent);

/***************************** General inode operations *****************************/
int vfs_dirlink(struct vfs_inode *dp, char *name, size_t namelen, struct vfs_inode *inode);
struct vfs_inode *vfs_dirlookup(struct vfs_inode *inode, const char *name, size_t len);
struct vfs_inode *vfs_ialloc(struct super_block *sb, enum inode_type type);
struct vfs_inode *vfs_idup(struct vfs_inode *inode);
void vfs_iput(struct vfs_inode *inode);
void vfs_ilock(struct vfs_inode *inode);
void vfs_iunlock(struct vfs_inode *inode);
void vfs_iunlockput(struct vfs_inode *inode);
void vfs_iupdate(struct vfs_inode *inode);
struct vfs_inode *vfs_namei(const char *path, size_t len);
struct vfs_inode *vfs_nameiparent(const char *path, size_t pathlen, char *name, size_t namelen);
int vfs_readi(struct vfs_inode *inode, void *buf, size_t size, loff_t offset);
int vfs_writei(struct vfs_inode *inode, const void *buf, size_t size, loff_t offset);
int vfs_itrunc(struct vfs_inode *inode, loff_t length);


#endif // __KERNEL_VFS_H
