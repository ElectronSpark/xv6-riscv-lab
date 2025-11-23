#ifndef KERNEL_VIRTUAL_FILE_SYSTEM_FILE_H
#define KERNEL_VIRTUAL_FILE_SYSTEM_FILE_H

#include "vfs/vfs_types.h"

// The file offset is set to offset bytes.
#define SEEK_SET 0
//   The file offset is set to its current location plus offset
//   bytes.
#define SEEK_CUR 1
// The file offset is set to the size of the file plus offset
// bytes.
#define SEEK_END 2

int vfs_fileopen(struct vfs_inode *inode, int f_flags, struct vfs_file **ret_file);
void vfs_fileclose(struct vfs_file *file);
struct vfs_file *vfs_filedup(struct vfs_file *file);
ssize_t vfs_fileread(struct vfs_file *file, void *buf, size_t n);
int vfs_filestat(struct vfs_file *file, struct stat *stat);
ssize_t vfs_filewrite(struct vfs_file *file, const void *buf, size_t n);
int vfs_filelseek(struct vfs_file *file, loff_t offset, int whence, loff_t *new_pos);

#endif // KERNEL_VIRTUAL_FILE_SYSTEM_FILE_H
