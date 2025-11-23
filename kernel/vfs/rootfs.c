// File System of the absolute root directory "/"
// This root directory can act only as a mount point for other filesystems.
// It is a dummy inode and does not contain any real data itself.

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "bits.h"
#include "errno.h"
#include "vfs/fs.h"



static struct vfs_fs_type_ops rooti_fs_type_ops;
static struct vfs_fs_type rooti_fs_type = {
    .name = "rooti",
    .ops = &rooti_fs_type_ops,
};
static struct vfs_superblock rooti_sb = {0};
static struct vfs_inode rooti_inode = {0};







static struct vfs_fs_type_ops rooti_fs_type_ops = {
    // No mount operation, as this is the root inode filesystem
    .mount = NULL,
    .unmount = NULL,
};

// Initialize the root filesystem and chroot to it.
void __vfs_rooti_init(void) {
    
}
