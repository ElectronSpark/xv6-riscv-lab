#ifndef KERNEL_VFS_FILE_LOCK_H
#define KERNEL_VFS_FILE_LOCK_H

#include "types.h"

struct flock;
struct vfs_file;

void __vfs_file_lock_global_init(void);
int vfs_file_lock_ctl(struct vfs_file *file, pid_t owner, int cmd,
                      struct flock *fl);
void vfs_file_lock_release(struct vfs_file *file, pid_t owner);

#endif /* KERNEL_VFS_FILE_LOCK_H */