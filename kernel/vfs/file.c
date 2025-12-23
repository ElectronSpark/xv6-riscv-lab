#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "atomic.h"
#include "param.h"
#include "errno.h"
#include "bits.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "mutex_types.h"
#include "rwlock.h"
#include "completion.h"
#include "vfs/fs.h"
#include "vfs/file.h"
#include "vfs_private.h"
#include "list.h"
#include "hlist.h"
#include "slab.h"

// static slab_cache_t __vfs_file_slab = { 0 };
// static 

// struct vfs_file *vfs_fileopen(struct vfs_inode *inode, int f_flags) {

// }

// void vfs_fileclose(struct vfs_file *file);
// struct vfs_file *vfs_filedup(struct vfs_file *file);
// ssize_t vfs_fileread(struct vfs_file *file, void *buf, size_t n);
// int vfs_filestat(struct vfs_file *file, struct stat *stat);
// ssize_t vfs_filewrite(struct vfs_file *file, const void *buf, size_t n);
// loff_t vfs_filelseek(struct vfs_file *file, loff_t offset, int whence);
