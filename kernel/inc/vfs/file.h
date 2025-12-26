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

// File descriptor table operations
// Caller should hold the proc lock when manipulating the fdtable
// vfs_fdtable_init, vfs_fdtable_clone, and vfs_fdtable_destroy don't 
// require the victim proc lock to be held
void vfs_fdtable_init(struct vfs_fdtable *fdtable);
int vfs_fdtable_alloc_fd(struct vfs_fdtable *fdtable, struct vfs_file *file);
int vfs_fdtable_clone(struct vfs_fdtable *dest, struct vfs_fdtable *src);
void vfs_fdtable_destroy(struct vfs_fdtable *fdtable, int start_fd);
struct vfs_file *vfs_fdtable_get_file(struct vfs_fdtable *fdtable, int fd);
struct vfs_file *vfs_fdtable_dealloc_fd(struct vfs_fdtable *fdtable, int fd);

#endif // KERNEL_VIRTUAL_FILE_SYSTEM_FILE_H
