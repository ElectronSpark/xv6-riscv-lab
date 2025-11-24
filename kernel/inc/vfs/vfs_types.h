#ifndef KERNEL_VIRTUAL_FILE_SYSTEM_TYPES_H
#define KERNEL_VIRTUAL_FILE_SYSTEM_TYPES_H

#include "compiler.h"
#include "types.h"
#include "list.h"
#include "spinlock.h"
#include "mutex_types.h"
#include "rwlock_types.h"
#include "hlist_type.h"
#include "pcache_types.h"
#include "kobject.h"
#include "vfs/stat.h"

struct pcache;
typedef struct cdev cdev_t;
typedef struct blkdev blkdev_t;

struct vfs_fs_type;
struct vfs_fs_type_ops;
struct vfs_superblock;
struct vfs_superblock_ops;
struct vfs_inode;
struct vfs_inode_ops;
struct vfs_dentry;
struct vfs_file;
struct vfs_file_ops;

// Filesystem type structure
// Protected by global vfs_fs_types_lock
struct vfs_fs_type {
    list_node_t list_entry;
    list_node_t superblocks; // list of struct vfs_superblock
    struct kobject kobj; // for sysfs representation
    struct {
        uint64 registered: 1;
    };
    int sb_count;
    const char *name;
    struct vfs_fs_type_ops *ops;
};

#define VFS_SUPERBLOCK_HASH_BUCKETS 61

// Filesystem type operations
// mount:
//    Create and fully initialize a superblock for the filesystem, returning it in
//    `ret_sb`. Implementations should allocate the superblock, fill in its fields,
//    and leave it in an unmounted state (mountpoint/parent unset) so that the VFS
//    core can attach it to the mount tree.
// free:
//    Tear down a superblock instance that has not been mounted, or that must be
//    discarded after a failed mount attempt.
struct vfs_fs_type_ops {
    int (*mount)(struct vfs_inode *mountpoint, struct vfs_inode *device,
                 int flags, const char *data, struct vfs_superblock **ret_sb);
    void (*free)(struct vfs_superblock *sb);
};

struct vfs_superblock {
    list_node_t siblings; // entry in vfs_fs_type.superblocks
    struct vfs_fs_type *fs_type;
    struct {
        hlist_t inodes; // hash list of inodes
        hlist_bucket_t inodes_buckets[VFS_SUPERBLOCK_HASH_BUCKETS];
    };
    struct {
        uint64 valid: 1;
        uint64 dirty: 1;
    };
    struct vfs_superblock *parent_sb; // parent superblock if mounted on another fs
    struct vfs_inode *mountpoint; // inode where this sb is mounted
    struct vfs_inode *device;     // device inode (NULL for non-dev fs)
    struct vfs_inode *root_inode; // root inode of this superblock
    struct vfs_superblock_ops *ops;
    struct rwlock lock; // protects the superblock
    void *fs_data; // filesystem-specific data
    int mount_count; // Number of superblocks directly mounted under this superblock
};

struct vfs_superblock_ops {
    int (*alloc_inode)(struct vfs_superblock *sb, struct vfs_inode **ret_inode);
    int (*get_inode)(struct vfs_superblock *sb, uint64 ino,
                     struct vfs_inode **ret_inode);
    int (*sync_fs)(struct vfs_superblock *sb, int wait);
    void (*unmount_begin)(struct vfs_superblock *sb);
};

// @TODO: Who protects inode?
struct vfs_inode {
    hlist_entry_t hash_entry; // entry in vfs_superblock.inodes
    vfs_inode_type_t type;
    uint64 ino; // inode number
    uint32 n_links; // number of hard links
    uint64 n_blocks; // number of blocks allocated
    loff_t size; // size in bytes
    uint32 mode; // permission and type bits
    uint32 uid;  // owner user id
    uint32 gid;  // owner group id
    uint64 atime; // access time
    uint64 mtime; // modification time
    uint64 ctime; // change time

    struct {
        uint64 valid: 1;
        uint64 dirty: 1;
    };
    struct vfs_superblock *sb;
    struct pcache *i_mapping; // page cache for its backend inode data
    struct pcache i_data; // page cache for its data blocks
    struct vfs_inode_ops *ops;
    struct kobject kobj; // for sysfs representation
    void *fs_data; // filesystem-specific data
    union {
        cdev_t *cdev; // for character device inode
        blkdev_t *bdev; // for block device inode
        struct vfs_superblock *mnt_sb; // the mounted superblock
    };

    struct mutex lock; // protects the inode
};

// Inode operations focuses mainly on metadata operations
// Data read/write operations are handled by file operations
struct vfs_inode_ops {
    int (*lookup)(struct vfs_inode *dir, struct vfs_dentry *dentry);
    int (*readlink)(struct vfs_inode *inode, char *buf, size_t buflen, bool user);
    int (*create)(struct vfs_inode *dir, struct vfs_dentry *dentry, uint32 mode);               // Create a regular file
    int (*link)(struct vfs_dentry *old, struct vfs_inode *dir, struct vfs_dentry *new);         // Create a hard link
    int (*unlink)(struct vfs_inode *dir, struct vfs_dentry *dentry);
    int (*mkdir)(struct vfs_inode *dir, struct vfs_dentry *dentry, uint32 mode);
    int (*rmdir)(struct vfs_inode *dir, struct vfs_dentry *dentry);
    int (*mknod)(struct vfs_inode *dir, struct vfs_dentry *dentry, uint32 mode, uint32 dev);    // Create a file of special types
    int (*move)(struct vfs_inode *old_dir, struct vfs_dentry *old_dentry,
                struct vfs_inode *new_dir, struct vfs_dentry *new_dentry);  // Move (rename) a file or directory whithin the same filesystem
    int (*symlink)(struct vfs_inode *dir, struct vfs_dentry *dentry,
                   const char *target);
    int (*truncate)(struct vfs_inode *inode, uint64 new_size);
    int (*idup)(struct vfs_inode *inode);       // Increase ref count
    int (*iput)(struct vfs_inode *inode);       // Decrease ref count and free if needed
    void (*destroy_inode)(struct vfs_inode *inode); // Release on-disk inode resources
    void (*free_inode)(struct vfs_inode *inode);    // Release in-memory inode structure 
    void (*dirty_inode)(struct vfs_inode *inode);   // Mark inode as dirty
    int (*sync_inode)(struct vfs_inode *inode);     // Write inode to disk
};

struct vfs_dentry {
    uint64 ino; // inode number
    char *name;
    uint16 name_len;
};

struct vfs_file {
    struct kobject kobj; // for sysfs representation
    struct vfs_inode *inode;
    loff_t f_pos; // file position
    uint32 f_flags; // file flags
    struct vfs_file_ops *ops;
    void *private_data; // filesystem-specific data
    mutex_t lock; // protects the file structure
};

struct vfs_file_ops {
    int (*read)(struct vfs_file *file, char *buf, size_t count, size_t *bytes_read);
    int (*write)(struct vfs_file *file, const char *buf, size_t count, size_t *bytes_written);
    int (*llseek)(struct vfs_file *file, loff_t offset, int whence, loff_t *new_pos);
    int (*open)(struct vfs_inode *inode, struct vfs_file *file, int flags);
    int (*release)(struct vfs_inode *inode, struct vfs_file *file);
    int (*fsync)(struct vfs_file *file, int datasync);
};

#endif // KERNEL_VIRTUAL_FILE_SYSTEM_TYPES_H
