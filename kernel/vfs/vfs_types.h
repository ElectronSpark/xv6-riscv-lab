#ifndef __KERNEL_VFS_TYPES_H
#define __KERNEL_VFS_TYPES_H

#include <types.h>
#include <list_type.h>
#include <sleeplock.h>
#include <hlist_type.h>

#define NAME_MAX 255 // Maximum length of a filename

struct vfs_inode;
struct super_block;
struct vfs_file;
struct vfs_dirent;
struct vfs_mount_point;

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
    struct super_block *(*mount)(struct vfs_inode *inode, dev_t dev);
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
//         The returned inode is locked, and the ref count will increase by one.
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
    // void (*lockfs)(struct super_block *sb);
    // void (*unlockfs)(struct super_block *sb);
    int (*holdingfs)(struct super_block *sb);
    int (*syncfs)(struct super_block *sb);
    int (*freezefs)(struct super_block *sb);
    int (*statfs)(struct super_block *sb, struct statfs *buf);
};

struct super_block {
    list_node_t s_list_entry;   // List entry for superblock list
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
    int64       ref;            // Reference count. Changes with iput and idup.
    struct {
        uint64  valid: 1;
        uint64  dirty: 1;
        uint64  frozen: 1;
    };
    struct super_block_ops  *ops;           // Operations on the super block
    struct vfs_inode        *root;          // mount point
    struct vfs_inode        *mount_point;   // Mount point for this superblock
    char                    name[32];
};

struct vfs_mount_point {
    list_node_t mount_list_entry;       // List entry for mount points in superblock
    struct vfs_inode *mount_point;      // Inode for the mount point
    struct super_block *sb;             // Superblock for the mounted filesystem
};

// Operations on the inode:
// All functions other than lockfs and unlockfs should assume the inode block is locked.
// Need to acquire the lock of its super block if necessary.
// - idup: Increment reference count of the inode.
// - iput: Decrement reference count of the inode, and destroy it if it reaches zero.
//         iput will not sync the inode to disk.
//         The inode needs to be locked before calling iput.
// - isync: Sync inode to disk if dirty.
// - ilock (Optional): Lock the inode for exclusive access.
// - iunlock (Optional): Unlock the inode.
// - iholding: Check if the inode is locked by the current process.
// - idirity: Mark inode as dirty, indicating it has been modified.
// - validate: Validate the inode, checking its type and other properties.
//             Return 0 on success, -1 on failure.
// - iread: Read data from the inode into a buffer.
// - iwrite: Write data from a buffer to the inode.
// - itruncate: Truncate the inode to a specified length.
// - bmap: Get the block address for a given block number in the inode.
// - open: Open the inode to a file descriptor.
// - close: Close the inode as a file.
// - isymlink: Create a symbolic link in the inode.
// - ireadlink: Read the target of a symbolic link from the inode.
// - d_lookup: Look up a inode by name in the parent directory.
//             The ref count of the inode will be incremented, and the inode will be locked.
// - d_link: Link a inode to an inode.
// - d_unlink: Unlink a inode from its parent directory.
// - d_mknod: Initlialize a new inode.
// - d_mkdir: Create a new inode a new directory.
// - d_rmdir: Cleanup an empty directory and make it ready for deletion.
// - d_mount: Preparation before mounting a filesystem on this inode.
// - d_umount: Cleanup after unmounting a filesystem from this inode.
struct vfs_inode_ops {
    struct vfs_inode *(*idup)(struct vfs_inode *inode); // Increment reference count
    void (*iput)(struct vfs_inode *inode);  // Decrement reference count
    void (*isync)(struct vfs_inode *inode); // Sync inode to disk
    void (*ilock)(struct vfs_inode *inode); // Lock the inode
    void (*iunlock)(struct vfs_inode *inode); // Unlock the inode
    bool (*iholding)(struct vfs_inode *inode); // Check if inode is locked
    void (*idirty)(struct vfs_inode *inode);    // Mark inode as dirty
    int (*validate)(struct vfs_inode *inode); // validate the inode
    ssize_t (*iread)(struct vfs_inode *inode, void *buf, size_t size, loff_t offset);
    ssize_t (*iwrite)(struct vfs_inode *inode, const void *buf, size_t size, loff_t offset);
    int (*itruncate)(struct vfs_inode *inode, loff_t length); // Truncate the inode
    int64 (*bmap)(struct vfs_inode *inode, uint64 block); // Get block address
    int (*open)(struct vfs_inode *inode, struct vfs_file *file);
    int (*close)(struct vfs_inode *inode, struct vfs_file *file);
    int (*isymlink)(struct vfs_inode *inode, const char *target, size_t target_len);
    ssize_t (*ireadlink)(struct vfs_inode *inode, char *buf, size_t bufsize);
    struct vfs_inode *(*d_lookup)(struct vfs_inode *inode, const char *name, size_t len);
    int (*d_link)(struct vfs_inode *base, const char *name, size_t namelen, struct vfs_inode *inode);
    int (*d_unlink)(struct vfs_inode *base, const char *name, size_t namelen);
    int (*d_mknod)(struct vfs_inode *inode, int type, dev_t dev);
    int (*d_mkdir)(struct vfs_inode *inode);
    int (*d_rmdir)(struct vfs_inode *inode);
    int (*d_mount)(struct vfs_inode *inode, struct super_block *sb);
    int (*d_umount)(struct vfs_inode *inode);
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
    struct vfs_mount_point *mp;     // Mount point for this inode, if any
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
    char name[NAME_MAX + 1]; // Name of the directory entry
    uint64 inum; // Inode number of the entry
    struct vfs_inode *inode; // The inode of the current directory
    loff_t offset; // Offset in the directory for the current entry
    ssize_t size; // Size of the current entry in bytes
};

#endif // __KERNEL_VFS_TYPES_H
