#ifndef __KERNEL_VFS_TYPES_H
#define __KERNEL_VFS_TYPES_H

#include "types.h"
#include "list_type.h"
#include "sleeplock.h"
#include "hlist_type.h"

struct vfs_dentry;
struct vfs_inode;
struct super_block;
struct vfs_file;
struct vfs_dirent;

enum inode_type {
    I_NONE = (int)0,
    I_PIPE,             // Pipe inode
    I_REG,              // Regular file inode
    I_DEVICE,           // Device inode
    I_SOCK,             // Socket inode
    I_DIR,              // Directory inode
    I_SYMLINK,          // Symbolic link inode
};

#ifndef __KERNEL_FILE_TYPES_H
#define __KERNEL_FILE_TYPES_H
enum file_type {
    FD_NONE = I_NONE,
    FD_PIPE = I_PIPE,
    FD_INODE = I_REG,
    FD_DEVICE = I_DEVICE,
    FD_SOCK = I_SOCK,
    FD_DIR = I_DIR,
    FD_SYMLINK = I_SYMLINK,
};
#endif // __KERNEL_FILE_TYPES_H

struct fs_type_ops {
    struct super_block *(*mount)(struct vfs_dentry *dentry, dev_t dev);
    struct super_block *(*mount_root)(dev_t dev);
    void (*umount)(struct super_block *sb);
};

// Filesystem type identifier
// All filesystems of the same type are linked to the same fs_type
struct fs_type {
    const char *name;
    uint64 f_type;                  // Filesystem type identifier
    struct fs_type_ops *ops;
    list_node_t registered_entry;   // To link all registered fs_types
    list_node_t s_list_head;        // List of superblocks for this fs_type
    int active_sbs;                 // Count of active superblocks for this fs_type
    struct {
        uint64 frozen: 1;           // unregistering
        uint64 root_mounted: 1;     // mounted as root
    };
};

// File System Statistics
struct statfs {
    // File system type. From fs_type->f_type.
    uint64  f_type;
    long    f_bsize;    // Block size in Bytes
    long    f_blocks;   // Total number of blocks
    long    f_bfree;    // Free blocks count
};

// Operations on the super block:
// All functions other than lockfs and unlockfs should assume the super block is locked.
// - ialloc: Allocate a free inode and return it. 
//           Should return NULL if no free inodes are available.
// - iget: Try to get an inode by number. 
//         The returned inode is not locked, also is not necessarily.
//         Will not increase its reference count.
// - idestroy: Destroy the inode and releasing its resources
//             after its reference count drops to zero.
// - lockfs: Lock the filesystem for exclusive access.
// - unlockfs: Unlock the filesystem.
// - holdingfs: Check if the filesystem is hold by the current process.
//              Returns 1 if locked, 0 if not.
//              Returns -1 if error.
// - syncfs: Sync the filesystem to disk if dirty.
//           Returns 0 on success, -1 on failure.
// - freezefs: Freeze the filesystem, preventing any further modifications
//             before unmounting it.
//             Returns 0 on success, -1 on failure.
// - statfs: Get filesystem statistics and fill the provided statfs structure.
//           Returns 0 on success, -1 on failure.
struct super_block_ops {
    struct vfs_inode *(*ialloc)(struct super_block *sb);
    struct vfs_inode *(*iget)(struct super_block *sb, uint64 inum);
    void (*idestroy)(struct vfs_inode *inode);
    void (*lockfs)(struct super_block *sb);
    void (*unlockfs)(struct super_block *sb);
    int (*holdingfs)(struct super_block *sb);
    int (*syncfs)(struct super_block *sb);
    int (*freezefs)(struct super_block *sb);
    int (*statfs)(struct super_block *sb, struct statfs *buf);
};

struct super_block {
    list_node_t s_list_entry;   // List entry for superblock list
    list_node_t inode_list;     // List of active inodes in this superblock
    list_node_t dentry_list;    // List of active dentries in this superblock
    list_node_t mount_list;     // List of mount points in this file system
    hlist_t inode_hash;         // Inode hash table for this superblock
    struct fs_type *fs_type;    // Filesystem type
    dev_t       dev;            // Device number
    uint64      blocksize;      // Block size in Bytes
    uint64      blocks_count;   // Total number of blocks
    uint64      free_blocks;    // Free blocks count
    uint64      inodes_count;   // Total number of inodes
    uint64      free_inodes;    // Free inodes count
    uint64      max_bytes;      // Maximum file size in Bytes
    void        *private_data;  // Private data for filesystem
    struct {
        uint64  valid: 1;
        uint64  dirty: 1;
        uint64  frozen: 1;
    };
    struct super_block_ops  *ops;           // Operations on the super block
    struct vfs_dentry       *root;          // mount point
    char                name[32];
};

struct vfs_mount_point {
    list_node_t mount_list_entry; // List entry for mount points in superblock
    struct vfs_dentry *dentry;    // Dentry for the mount point
    struct super_block *sb;       // Superblock for the mounted filesystem
};

// Operations on the inode:
// All functions other than lockfs and unlockfs should assume the inode block is locked.
// Need to acquire the lock of its super block if necessary.
// - idup: Increment reference count of the inode.
// - iput: Decrement reference count of the inode, and destroy it if it reaches zero.
// - isync: Sync inode to disk if dirty.
// - ilock (Optional): Lock the inode for exclusive access.
// - iunlock (Optional): Unlock the inode.
// - idirity: Mark inode as dirty, indicating it has been modified.
// - iread: Read data from the inode into a buffer.
// - iwrite: Write data from a buffer to the inode.
// - itruncate: Truncate the inode to a specified length.
// - bmap: Get the block address for a given block number in the inode.
// - open: Open the inode to a file descriptor.
// - close: Close the inode as a file.
// - isymlink: Create a symbolic link in the inode.
// - ireadlink: Read the target of a symbolic link from the inode.
struct vfs_inode_ops {
    struct vfs_inode *(*idup)(struct vfs_inode *inode); // Increment reference count
    void (*iput)(struct vfs_inode *inode);  // Decrement reference count
    void (*isync)(struct vfs_inode *inode); // Sync inode to disk
    void (*ilock)(struct vfs_inode *inode); // Lock the inode
    void (*iunlock)(struct vfs_inode *inode); // Unlock the inode
    void (*idirty)(struct vfs_inode *inode);    // Mark inode as dirty
    ssize_t (*iread)(struct vfs_inode *inode, void *buf, size_t size, loff_t offset);
    ssize_t (*iwrite)(struct vfs_inode *inode, const void *buf, size_t size, loff_t offset);
    int (*itruncate)(struct vfs_inode *inode, loff_t length); // Truncate the inode
    int64 (*bmap)(struct vfs_inode *inode, uint64 block); // Get block address
    int (*open)(struct vfs_inode *inode, struct vfs_file *file);
    int (*close)(struct vfs_inode *inode, struct vfs_file *file);
    int (*isymlink)(struct vfs_inode *inode, const char *target, size_t target_len);
    ssize_t (*ireadlink)(struct vfs_inode *inode, char *buf, size_t bufsize);
};

struct vfs_inode {
    hlist_entry_t hlist_entry;      // for inode hash list
    list_node_t i_list_entry;       // List entry for inodes in superblock
    struct super_block *sb;         // Superblock this inode belongs to
    struct vfs_inode_ops *ops;      // Operations on the inode
    enum inode_type type;           // Type of the inode (e.g., file, directory)
    dev_t dev;                      // Device number
    uint64 inum;                    // Inode number
    int ref;                        // Reference count
    loff_t size;                    // Size of the file in bytes
    struct sleeplock lock;          // protects everything below here
    struct {
        uint64 valid: 1;            // inode has been read from disk?
        uint64 dirty: 1;            // inode has been modified
    };
};

// Dentry operations:
// - d_lookup: Look up a dentry by name in the parent directory, 
//             will increase the reference count by 1 if found a valid dentry.
//             If create is true, it will attempt to create a new dentry if not found.
//             The dentry is returned is not guaranteed to be valid,
//             '..' and '.' needs to be treayed specially.
// - d_destroy: Pop the dentry from the children list of its parent, 
//              and destroy the dentry and free its resources.
//              Return 0 on success, -1 on failure.
// - d_link: Link a dentry to an inode.
// - d_unlink: Unlink a dentry from its parent directory.
// - d_mknod: Create a new inode and link it to the dentry.
// - d_mkdir: Create a new directory dentry and link it to an inode.
// - d_rmdir: Remove a directory dentry.
// - d_rename: Rename a dentry to a new name.
// - d_hash: Compute the hash value for a dentry based on its name.
// - d_compare: Compare a dentry with a name to check for equality.
// - d_sync: Sync dentry and its direct children to disk.
// - d_validate: Read the dentry from the disk.
//               and it was previously invalid.
//               Return 0 when success. Return -1 when failed.
// - d_invalidate: Invalidate a dentry, marking it as no longer valid.
//                 This will be called in vfs_dentry_put when the reference count reaches zero.
//                 VFS will try to invalidate if all the offspring dentry hit 0 ref count.
//                 Dentry will not necessarily be marked as invalid by this function, since
//                 the file system could be ramfs or tmpfs, which does not require
//                 After if it is marked as invalid, it should not contain any cached children, 
//                 and could be freed.
// - d_inode: Get the inode associated with the dentry.
//            It will try tp validate the dentry and load the inode from disk if necessary.
//            Return NULL if failed to validate the dentry or load the inode.
//            Return NULL if the dentry is a mount point.
// - d_is_symlink: Check if the dentry is a symbolic link.
//                 Return true if it is a symlink, false otherwise.
//                 It will try to validate the dentry and load the inode from disk if necessary.
//                 Return false if the dentry is a mount point.
struct vfs_dentry_ops {
    struct vfs_dentry *(*d_lookup)(struct vfs_dentry *dentry, const char *name, size_t len, bool create);
    int (*d_destroy)(struct vfs_dentry *dentry); // Destroy dentry and free resources
    int (*d_link)(struct vfs_dentry *dentry, struct vfs_inode *inode);
    int (*d_unlink)(struct vfs_dentry *dentry);
    int (*d_mknod)(struct vfs_dentry *dentry, struct vfs_inode *inode, int type, dev_t dev);
    int (*d_mkdir)(struct vfs_dentry *dentry, struct vfs_inode *inode);
    int (*d_rmdir)(struct vfs_dentry *dentry);
    int (*d_rename)(struct vfs_dentry *old_dentry, struct vfs_dentry *new_dentry);
    ht_hash_t (*d_hash)(struct vfs_dentry *dentry, const char *name, size_t len);
    int (*d_compare)(const struct vfs_dentry *dentry, const char *name, size_t len);
    void (*d_sync)(struct vfs_dentry *dentry); // Sync dentry to disk
    int (*d_validate)(struct vfs_dentry *dentry);
    void (*d_invalidate)(struct vfs_dentry *dentry); // Invalidate dentry
    struct vfs_inode *(*d_inode)(struct vfs_dentry *dentry); // Get inode from dentry
    bool (*d_is_symlink)(struct vfs_dentry *dentry); // Check if dentry is a symlink
};

#define NAME_MAX 255 // Maximum length of a filename

struct vfs_dentry {
    list_node_t sibling;            // List of all dentries in the same directory
    list_node_t children;           // List of child dentries
    struct vfs_dentry *parent;      // Parent dentry
    struct vfs_dentry *root;        // Root dentry of the file system
    struct super_block *sb;         // Superblock this dentry belongs to
    union {
        struct vfs_mount_point  *mount;     // Mount point associated with this dentry
        struct vfs_inode        *inode;     // Inode associated with this dentry
        uint64  inode_num;                  // Inode number. Need to load the inode from disk.
    };
    struct vfs_dentry_ops *ops;     // Operations on the dentry
    struct {
        uint64  valid: 1;
        uint64  inode_cached: 1;    // Is the inode cached in memory?
        uint64  dirty: 1;
        uint64  deleted: 1;         // Is this dentry deleted?
        uint64  mounted: 1;         // Is this dentry a mount point? Ignore inode_valid if mounted.
    };
    size_t namelen;                 // Length of the name
    char name[NAME_MAX];            // Name of the dentry
    ht_hash_t hash;                 // Hash value of the name
    int ref_count;                  // Reference count for the dentry
};

struct vfs_file_ops {
    int (*dopen)(struct vfs_file *file, struct vfs_dentry *dentry);
    struct vfs_dentry *(*dnext)(struct vfs_file *file, struct vfs_dirent *dirent);
};

struct vfs_file {
    hlist_entry_t hlist_entry;       // for file hash list
    int fd;                          // Global file descriptor number

    struct vfs_file_ops *ops;        // Operations on the file
    struct vfs_inode *inode;         // Inode associated with the file
    loff_t offset;                   // Current file offset
    int flags;                       // File access flags (e.g., read, write)
    int type;                        // Type of the file (corresponds to inode type)
    int ref_count;                   // Reference count for the file
};

// Used to traverse directory entries
struct vfs_dirent {
    struct vfs_dentry *dentry; // The current dentry position
    struct vfs_file *file; // File of the parent dentry
    loff_t next_off; // Offset for the next directory entry
};

#endif // __KERNEL_VFS_TYPES_H
