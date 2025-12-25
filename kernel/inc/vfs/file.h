#ifndef KERNEL_VIRTUAL_FILE_SYSTEM_FILE_H
#define KERNEL_VIRTUAL_FILE_SYSTEM_FILE_H

#include "vfs/vfs_types.h"

struct vfs_file *vfs_fileopen(struct vfs_inode *inode, int f_flags);
void vfs_fileclose(struct vfs_file *file);
struct vfs_file *vfs_filedup(struct vfs_file *file);
ssize_t vfs_fileread(struct vfs_file *file, void *buf, size_t n);
int vfs_filestat(struct vfs_file *file, struct stat *stat);
ssize_t vfs_filewrite(struct vfs_file *file, const void *buf, size_t n);
loff_t vfs_filelseek(struct vfs_file *file, loff_t offset, int whence);
int truncate(struct vfs_file *file, loff_t length);

#endif // KERNEL_VIRTUAL_FILE_SYSTEM_FILE_H
