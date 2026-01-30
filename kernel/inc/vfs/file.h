/**
 * @file file.h
 * @brief VFS file operations interface
 *
 * Provides the public API for file operations including open, read, write,
 * seek, and file descriptor management. Uses reference counting for
 * shared file descriptors.
 */

#ifndef KERNEL_VIRTUAL_FILE_SYSTEM_FILE_H
#define KERNEL_VIRTUAL_FILE_SYSTEM_FILE_H

#include "vfs/vfs_types.h"
#include "clone_flags.h"

/**
 * @brief Open a file from an inode
 * @param inode The inode to open
 * @param f_flags Open flags (O_RDONLY, O_WRONLY, O_RDWR, etc.)
 * @return File pointer or ERR_PTR on error
 */
struct vfs_file *vfs_fileopen(struct vfs_inode *inode, int f_flags);

/**
 * @brief Release a file reference (put)
 * @param file File to release
 * @note Decrements reference count; frees file when count reaches zero
 * @note Formerly named vfs_fileclose()
 */
void vfs_fput(struct vfs_file *file);

/**
 * @brief Duplicate a file reference (get)
 * @param file File to duplicate
 * @return Same file pointer with incremented reference count, or NULL
 * @note Formerly named vfs_filedup()
 */
struct vfs_file *vfs_fdup(struct vfs_file *file);

ssize_t vfs_fileread(struct vfs_file *file, void *buf, size_t n, bool user);
int vfs_filestat(struct vfs_file *file, struct stat *stat);
ssize_t vfs_filewrite(struct vfs_file *file, const void *buf, size_t n, bool user);
loff_t vfs_filelseek(struct vfs_file *file, loff_t offset, int whence);
int truncate(struct vfs_file *file, loff_t length);

// VFS Pipe allocation
int vfs_pipealloc(struct vfs_file **rf, struct vfs_file **wf);

// VFS Socket allocation
int vfs_sockalloc(struct vfs_file **f, uint32 raddr, uint16 lport, uint16 rport);

// File descriptor table operations
// Caller should hold the proc lock when manipulating the fdtable
// vfs_fdtable_init, vfs_fdtable_clone, and vfs_fdtable_destroy don't 
// require the victim proc lock to be held
struct vfs_fdtable *vfs_fdtable_init(void);
struct vfs_fdtable *vfs_fdtable_clone(struct vfs_fdtable *src, int clone_flags);
void vfs_fdtable_put(struct vfs_fdtable *fdtable);
int vfs_fdtable_alloc_fd(struct vfs_fdtable *fdtable, struct vfs_file *file);
struct vfs_file *vfs_fdtable_get_file(struct vfs_fdtable *fdtable, int fd);
struct vfs_file *vfs_fdtable_dealloc_fd(struct vfs_fdtable *fdtable, int fd);

#endif // KERNEL_VIRTUAL_FILE_SYSTEM_FILE_H
