#ifndef KERNEL_VIRTUAL_FILE_SYSTEM_XV6FS_PRIVATE_H
#define KERNEL_VIRTUAL_FILE_SYSTEM_XV6FS_PRIVATE_H

#include "types.h"
#include "vfs/vfs_types.h"
#include "lock/spinlock.h"
#include "proc/proc_queue.h"
#include "vfs/xv6fs/ondisk.h"  // xv6 on-disk format definitions
#include <mm/slab.h>
#include "dev/blkdev.h"

// Block size for xv6 filesystem
#define XV6FS_BSIZE BSIZE

// Inode index macros
#define XV6FS_IBLOCK(ino, sb) (((ino) / IPB) + (sb)->inodestart)

// Maximum block count
#define XV6FS_NDIRECT NDIRECT
#define XV6FS_NINDIRECT NINDIRECT
#define XV6FS_NDINDIRECT NDINDIRECT
#define XV6FS_MAXFILE MAXFILE

// Log size
#define XV6FS_LOGSIZE LOGSIZE

// VFS dentry cookie definitions
#define VFS_DENTRY_COOKIE_END ((int64)0)
#define VFS_DENTRY_COOKIE_SELF ((int64)1)
#define VFS_DENTRY_COOKIE_PARENT ((int64)2)

// xv6 file types to VFS mode conversion
#define XV6FS_T_DIR     1   // Directory
#define XV6FS_T_FILE    2   // File
#define XV6FS_T_CDEVICE  3   // Character device
#define XV6FS_T_SYMLINK 4   // Symbolic link
#define XV6FS_T_BLKDEVICE 5   // Block device

/*
 * xv6fs log header structure
 * Used both on-disk and in-memory to track logged blocks
 */
struct xv6fs_logheader {
    int n;
    int block[XV6FS_LOGSIZE];
};

/*
 * xv6fs log structure
 * Per-superblock logging for crash recovery
 * 
 * The wait_queue is used instead of global sleep_on_chan() to avoid
 * contention on the global sleep_lock. Waiters in begin_op() use the
 * per-log queue, and end_op() wakes them after commit completes.
 * 
 * Pattern: end_op() uses proc_queue_bulk_move() to a temp queue, then
 * wakes outside the lock to avoid lock convoy (woken processes competing
 * to reacquire log->lock).
 */
struct xv6fs_log {
    struct spinlock lock;
    proc_queue_t wait_queue;  /**< Per-log wait queue for begin_op waiters */
    int start;          // Log start block
    int size;           // Log size in blocks
    int outstanding;    // How many FS ops are executing
    int committing;     // In commit(), please wait
    int dev;            // Device number
    struct xv6fs_logheader lh;
};

/*
 * xv6fs superblock structure
 * Contains the VFS superblock and xv6-specific data
 */
struct xv6fs_superblock {
    struct vfs_superblock vfs_sb;
    struct superblock disk_sb;   // Copy of the on-disk superblock
    blkdev_t *blkdev;             // Block device descriptor reference
    int dirty;                    // Superblock metadata dirty flag
    struct xv6fs_log log;         // Per-superblock logging
};

// Helper to get device number from xv6fs superblock
#define xv6fs_sb_dev(xv6_sb) \
    mkdev((xv6_sb)->blkdev->dev.major, (xv6_sb)->blkdev->dev.minor)

/*
 * xv6fs inode structure
 * Contains the VFS inode and xv6-specific data
 */
struct xv6fs_inode {
    struct vfs_inode vfs_inode;
    uint dev;                     // Device number (for lookup)
    uint addrs[XV6FS_NDIRECT + 2]; // Block addresses (direct + indirect + double indirect)
    short major;                  // Major device number (for device files)
    short minor;                  // Minor device number (for device files)
};

// External declarations
extern struct vfs_inode_ops xv6fs_inode_ops;
extern struct vfs_file_ops xv6fs_file_ops;
extern struct vfs_superblock_ops xv6fs_superblock_ops;
extern struct vfs_fs_type_ops xv6fs_fs_type_ops;

// Log operations
void xv6fs_initlog(struct xv6fs_superblock *xv6_sb);
void xv6fs_begin_op(struct xv6fs_superblock *xv6_sb);
void xv6fs_end_op(struct xv6fs_superblock *xv6_sb);
void xv6fs_log_write(struct xv6fs_superblock *xv6_sb, struct buf *b);

// Superblock operations
struct vfs_inode *xv6fs_alloc_inode(struct vfs_superblock *sb);
struct vfs_inode *xv6fs_get_inode(struct vfs_superblock *sb, uint64 ino);
int xv6fs_sync_fs(struct vfs_superblock *sb, int wait);
void xv6fs_unmount_begin(struct vfs_superblock *sb);
void xv6fs_free(struct vfs_superblock *sb);
int xv6fs_mount(struct vfs_inode *mountpoint, struct vfs_inode *device,
                int flags, const char *data, struct vfs_superblock **ret_sb);

// Inode operations
int xv6fs_lookup(struct vfs_inode *dir, struct vfs_dentry *dentry,
                 const char *name, size_t name_len);
int xv6fs_dir_iter(struct vfs_inode *dir, struct vfs_dir_iter *iter,
                   struct vfs_dentry *ret_dentry);
ssize_t xv6fs_readlink(struct vfs_inode *inode, char *buf, size_t buflen);
struct vfs_inode *xv6fs_create(struct vfs_inode *dir, mode_t mode,
                                const char *name, size_t name_len);
struct vfs_inode *xv6fs_mkdir(struct vfs_inode *dir, mode_t mode,
                               const char *name, size_t name_len);
int xv6fs_unlink(struct vfs_dentry *dentry, struct vfs_inode *target);
int xv6fs_rmdir(struct vfs_dentry *dentry, struct vfs_inode *target);
struct vfs_inode *xv6fs_mknod(struct vfs_inode *dir, mode_t mode,
                               dev_t dev, const char *name, size_t name_len);
struct vfs_inode *xv6fs_symlink(struct vfs_inode *dir, mode_t mode,
                                 const char *name, size_t name_len,
                                 const char *target, size_t target_len);
int xv6fs_link(struct vfs_inode *old, struct vfs_inode *dir,
               const char *name, size_t name_len);
int xv6fs_truncate(struct vfs_inode *inode, loff_t new_size);
void xv6fs_destroy_inode(struct vfs_inode *inode);
void xv6fs_free_inode(struct vfs_inode *inode);
int xv6fs_dirty_inode(struct vfs_inode *inode);
int xv6fs_sync_inode(struct vfs_inode *inode);
int xv6fs_open(struct vfs_inode *inode, struct vfs_file *file, int f_flags);

// File operations
ssize_t xv6fs_file_read(struct vfs_file *file, char *buf, size_t count, bool user);
ssize_t xv6fs_file_write(struct vfs_file *file, const char *buf, size_t count, bool user);
loff_t xv6fs_file_llseek(struct vfs_file *file, loff_t offset, int whence);
int xv6fs_file_stat(struct vfs_file *file, struct stat *stat);

// Helper functions
void xv6fs_init(void);
void xv6fs_mount_root(void);
uint xv6fs_bmap(struct xv6fs_inode *ip, uint bn);
uint xv6fs_bmap_read(struct xv6fs_inode *ip, uint bn);
void xv6fs_itrunc(struct xv6fs_inode *ip);
void xv6fs_iupdate(struct xv6fs_inode *ip);
void xv6fs_shrink_caches(void);

// Slab caches (extern)
extern slab_cache_t __xv6fs_inode_cache;

// Convert xv6 type to VFS mode
static inline mode_t xv6fs_type_to_mode(short type) {
    switch (type) {
    case XV6FS_T_DIR:
        return S_IFDIR | 0755;
    case XV6FS_T_FILE:
        return S_IFREG | 0644;
    case XV6FS_T_CDEVICE:
        return S_IFCHR | 0666;
    case XV6FS_T_BLKDEVICE:
        return S_IFBLK | 0660;
    case XV6FS_T_SYMLINK:
        return S_IFLNK | 0777;
    default:
        return 0;
    }
}

// Convert VFS mode to xv6 type
static inline short xv6fs_mode_to_type(mode_t mode) {
    if (S_ISDIR(mode))
        return XV6FS_T_DIR;
    if (S_ISREG(mode))
        return XV6FS_T_FILE;
    if (S_ISCHR(mode))
        return XV6FS_T_CDEVICE;
    if (S_ISBLK(mode))
        return XV6FS_T_BLKDEVICE;
    if (S_ISLNK(mode))
        return XV6FS_T_SYMLINK;
    return 0;
}

#endif // KERNEL_VIRTUAL_FILE_SYSTEM_XV6FS_PRIVATE_H
