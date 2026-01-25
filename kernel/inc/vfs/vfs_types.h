#ifndef KERNEL_VIRTUAL_FILE_SYSTEM_TYPES_H
#define KERNEL_VIRTUAL_FILE_SYSTEM_TYPES_H

#include "compiler.h"
#include "types.h"
#include "list.h"
#include "lock/spinlock.h"
#include "lock/mutex_types.h"
#include "lock/rwlock_types.h"
#include "hlist_type.h"
#include "pcache_types.h"
#include "kobject.h"
#include "vfs/stat.h"

struct pcache;
typedef struct cdev cdev_t;
typedef struct blkdev blkdev_t;
struct proc;

struct vfs_fs_type;
struct vfs_fs_type_ops;
struct vfs_superblock;
struct vfs_superblock_ops;
struct vfs_inode;
struct vfs_inode_ops;
struct vfs_dentry;
struct vfs_dir_iter;
struct vfs_file;
struct vfs_file_ops;

/*
 * Filesystem type structure
 * Protected by global vfs_fs_types_lock
 */
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

/*
 * Filesystem type operations
 * mount:
 *   Create and fully initialize a superblock for the filesystem, returning it in
 *   `ret_sb`. Implementations should allocate the superblock, fill in its fields,
 *   and leave it in an unmounted state (mountpoint/parent unset) so that the VFS
 *   core can attach it to the mount tree.
 *   The returned superblock should have its root_inode preloaded and the root inode's
 *   ref count set to 1.
 * free:
 *   Tear down a superblock instance that has not been mounted, or that must be
 *   discarded after a failed mount attempt.
 *   It should release all inodes and resources associated with the superblock,
 *   including its root inode if present.
 */
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
        uint64 dirty: 1;    // Only indicates whether the metadata of the superblock is dirty
        uint64 backendless: 1; // Indicates whether the filesystem is backendless (e.g., tmpfs)
        uint64 initialized: 1; // Indicates whether the superblock has been initialized
        uint64 registered: 1;    // Indicates whether the superblock is attached to a filesystem type
        uint64 syncing: 1;       // Currently syncing to backend storage
        uint64 unmounting: 1;    // Unmount initiated, blocking new operations
        uint64 attached: 1;      // Attached to mount tree (set on mount, cleared on lazy unmount)
    };
    struct vfs_superblock *parent_sb; // parent superblock if mounted on another fs
    struct vfs_inode *mountpoint; // inode where this sb is mounted
    struct vfs_inode *device;     // device inode (NULL for non-dev fs)
    struct vfs_inode *root_inode; // root inode of this superblock
    struct vfs_superblock_ops *ops;
    
    struct rwlock lock; // protects the superblock
    void *fs_data; // filesystem-specific data
    int mount_count; // Number of superblocks directly mounted under this superblock
    int refcount; // Reference count for the superblock
    int orphan_count; // Number of orphan inodes (n_links=0, ref>0)
    list_node_t orphan_list; // List of orphan inodes
    
    // Filesystem statistics
    struct spinlock spinlock; // protects the counters below 
    size_t block_size; // Filesystem block size
    // May be 0 if the filesystem does not track total/used blocks
    // (e.g., tmpfs)
    uint64 total_blocks;
    uint64 used_blocks;  // Number of blocks used in the filesystem
};

/*
 * Superblock operations
 * alloc_inode:
 *   Allocate a new inode in the superblock. The returned inode should have its
 *   ref count set to 1.
 *   write lock on the superblock will be held during this operation.
 *   Return ERR_PTR(-ENOSPC) if there is no space to allocate a new inode.
 *
 * get_inode:
 *   Get a inode with the given inode number from the file system on disk.
 *   Write lock on the superblock will be held during this operation.
 *   Filesystem drivers must fill in the following fields:
 *     - ino
 *     - type
 *     - size
 *     - mode
 *     - ops
 *     - one of: cdev, bdev
 *   Filesystem drivers may fill in the following fields if applicable:
 *     - n_links - number of hard links
 *     - n_blocks - number of blocks allocated
 *     - uid - owner user id
 *     - gid - owner group id
 *     - atime - access time
 *     - mtime - modification time
 *     - ctime - change time
 *     - fs_data - filesystem-specific data
 *   The returned inode should have its ref count set to 1.
 *   Return ERR_PTR(-ENOENT) if the inode is not found or not allocated.
 *   Note: This function will be called only when inode is not found in memory,
 *         thus write lock of the superblock is held to protect the inode hash list.
 *   Note: Filesystem drivers should zero-initialize and fill in the necessary fields
 *         of the returned inode, but should NOT lock or mark it valid. The VFS core
 *         will lock and mark the inode valid after adding it to the superblock's
 *         inode hash list.
 *
 * sync_fs:
 *   Synchronize the superblock's state with the underlying storage.
 *   This callback function will be called by vfs_sync_superblock(), which will
 *   hold the superblock write lock during the operation. Thus, implementations do not
 *   need to acquire additional locks on the superblock structure, and if wait is false,
 *   write lock should be acquired in other threads.
 *   The `wait` parameter indicates whether the operation should be synchronous.
 *   Return 0 on success or a negative error code on failure.
 *
 * unmount_begin:
 *   Prepare the superblock for unmounting. This function should ensure that:
 *   - The superblock is clean (no dirty data).
 *   - There are no active inodes associated with the superblock.
 *   - No other superblocks are mounted under this superblock.
 *   After this function returns, the VFS core will proceed with unmounting the
 *   superblock and freeing its resources.
 */
struct vfs_superblock_ops {
    struct vfs_inode *(*alloc_inode)(struct vfs_superblock *sb);
    struct vfs_inode *(*get_inode)(struct vfs_superblock *sb, uint64 ino);
    int (*sync_fs)(struct vfs_superblock *sb, int wait);
    void (*unmount_begin)(struct vfs_superblock *sb);
    // Orphan management for crash recovery (optional, for backend filesystems)
    int (*add_orphan)(struct vfs_superblock *sb, struct vfs_inode *inode);
    int (*remove_orphan)(struct vfs_superblock *sb, struct vfs_inode *inode);
    int (*recover_orphans)(struct vfs_superblock *sb);
    
    /*
     * Transaction/journaling support (optional)
     *
     * DESIGN CHOICE - Register callbacks OR manage internally, not both:
     *
     * Option 1: REGISTER CALLBACKS (recommended for simple transactions)
     *   - Set begin_transaction and end_transaction to non-NULL
     *   - VFS will call these for METADATA operations (create, unlink, etc.)
     *   - VFS calls begin_transaction BEFORE acquiring any locks
     *   - VFS calls end_transaction AFTER releasing all locks
     *   - Ensures correct lock ordering: transaction → superblock → inode
     *   - FS inode callbacks must NOT call begin/end internally
     *
     * Option 2: MANAGE INTERNALLY (for complex transaction needs)
     *   - Set begin_transaction and end_transaction to NULL
     *   - FS manages ALL transactions internally in its callbacks
     *   - Required for: batched transactions, nested transactions, etc.
     *   - FS must ensure correct lock ordering to avoid deadlock
     *   - WARNING: Calling begin while holding VFS locks can deadlock!
     *
     * HYBRID APPROACH (xv6fs example):
     *   - Register callbacks for metadata ops (single transaction each)
     *   - File ops (write, truncate) manage transactions internally
     *     because they need batching for large files
     *   - This works because file ops use different inodes than metadata ops
     *
     * Returns: 0 on success, negative error code on failure
     */
    int (*begin_transaction)(struct vfs_superblock *sb);
    int (*end_transaction)(struct vfs_superblock *sb);
};

struct vfs_inode {
    hlist_entry_t hash_entry; // entry in vfs_superblock.inodes
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

    mutex_t mutex; // mutex to protect inode structure
    /**
     * All inodes must be valid to perform operations involving callbacks.
     * only the following operations are excluded:
     * - vfs_idup: to increase ref count
     * - vfs_iput: to decrease ref count and free if needed
        * - vfs_ilock: to acquire inode lock
     * - vfs_iunlock: to release inode lock
     * 
     * When an inode is being created, its inode mutex (via vfs_ilock) is held
     * to prevent other operations from touching it until it is fully initialized
     * and attached to the superblock's inode hash list. Only after that point
     * may the inode be marked valid (valid=1).
     * 
     * When an inode is being deleted, it will be marked invalid (valid=0) to
     * prevent new operations from starting on it. Existing operations should
     * complete before the inode is fully deleted and freed.
     * 
     * An inode marked invalid should be removed from the superblock's inode
     * hash list.
     * 
     * Typically, the inode mutex remains held throughout the deletion process,
     * and the last reference release will free the inode if it is marked invalid.
     * 
     * dirty indicates whether the inode's on disk metadata has been modified 
     * and needs to be synced to disk. Callers must hold the inode mutex when
     * modifying inode metadata so updates to the valid/dirty flags remain ordered.
     */
    struct {
        uint64 valid: 1;
        uint64 dirty: 1;
        uint64 mount: 1; // indicates whether this inode is a mountpoint
        uint64 orphan: 1; // inode is orphaned (n_links=0, ref>0, on orphan_list)
        uint64 destroying: 1; // inode is being destroyed (destroy_inode in progress)
    };
    list_node_t orphan_entry; // entry in sb->orphan_list when orphaned
    struct proc *owner; // process that holds the lock
    struct vfs_superblock *sb;
    // The two pcaches below are managed by the drivers/filesystems
    // Initialize them as needed
    struct pcache *i_mapping; // page cache for its backend inode data
    struct pcache i_data; // page cache for its data blocks
    struct vfs_inode_ops *ops;
    int ref_count; // reference count
    void *fs_data; // filesystem-specific data
    struct vfs_inode *parent; // parent inode for directories (self for root inodes)
    char *name; // directory name (for directories only, NULL for root)
    union {
        uint32 cdev; // for character device inode
        uint32 bdev; // for block device inode
        struct {
            struct vfs_superblock *mnt_sb; // the mounted superblock
            struct vfs_inode *mnt_rooti; // root inode of the mounted superblock
        };
    };
    completion_t completion;
};

/*
 * Inode operations focus mainly on metadata operations
 * Data read/write operations are handled by file operations
 * The VFS core acquires the inode mutex before invoking inode operation callbacks.
 * Operations will require write lock on the superblock:
 * - create
 * - mkdir
 * - rmdir
 * - unlink
 * - mknod
 * - move
 * - destroy_inode
 * The following operations must increase the refcount of the returned inode:
 * - create
 * - mkdir
 * - mknod
 * - symlink
 */
struct vfs_inode_ops {
    int (*lookup)(struct vfs_inode *dir, struct vfs_dentry *dentry, 
                  const char *name, size_t name_len);
    int (*dir_iter)(struct vfs_inode *dir, struct vfs_dir_iter *iter, 
                    struct vfs_dentry *ret_dentry);
    ssize_t (*readlink)(struct vfs_inode *inode, char *buf, size_t buflen);
    struct vfs_inode *(*create)(struct vfs_inode *dir, mode_t mode,
                       const char *name, size_t name_len);        // Create a regular file
    int (*link)(struct vfs_inode *old, struct vfs_inode *dir,
            const char *name, size_t name_len);         // Create a hard link
    struct vfs_inode *(*unlink)(struct vfs_inode *dir, const char *name, size_t name_len);
    struct vfs_inode *(*mkdir)(struct vfs_inode *dir, mode_t mode,
                               const char *name, size_t name_len);
    struct vfs_inode *(*rmdir)(struct vfs_inode *dir, const char *name, size_t name_len);
    struct vfs_inode *(*mknod)(struct vfs_inode *dir, mode_t mode,
                               dev_t dev, const char *name, size_t name_len);    // Create a file of special types
    int (*move)(struct vfs_inode *old_dir, struct vfs_dentry *old_dentry,
                struct vfs_inode *new_dir, const char *name, size_t name_len);  // Move (rename) a file or directory within the same filesystem
    struct vfs_inode *(*symlink)(struct vfs_inode *dir, mode_t mode, 
                                 const char *name, size_t name_len,
                                 const char *target, size_t target_len);
    int (*truncate)(struct vfs_inode *inode, loff_t new_size);
    void (*destroy_inode)(struct vfs_inode *inode); // Release on-disk inode resources
    void (*free_inode)(struct vfs_inode *inode);    // Release in-memory inode structure
    int (*dirty_inode)(struct vfs_inode *inode);   // Mark inode as dirty
    int (*sync_inode)(struct vfs_inode *inode);     // Write inode to disk
    int (*open)(struct vfs_inode *inode, struct vfs_file *file, int f_flags);
};

/* No dentry cache right now */
struct vfs_dentry {
    struct vfs_superblock *sb;
    struct vfs_inode *parent; // parent inode
    uint64 ino; // inode number
    // The `name` field is managed by slab allocator
    char *name;
    uint16 name_len;
    int64 cookies;      // Filesystem-private cookie; opaque to callers. The VFS uses internal
                        // sentinel values during directory iteration, but external callers
                        // must not rely on any specific value.
};

struct vfs_dir_iter {
    int64 cookies;      // Filesystem-private cookie; opaque to callers
    int64 index;       // Number of entries successfully returned so far
};

struct vfs_file {
    list_node_t list_entry; // entry in global open file table
    struct vfs_inode_ref inode;
    int fd; // file descriptor number
    int f_flags; // file access mode
    int ref_count; // reference count
    struct vfs_file_ops *ops;
    void *private_data; // filesystem-specific data
    mutex_t lock; // protects f_pos
    union {
        loff_t f_pos; // file position(regular files)
        struct vfs_dir_iter dir_iter; // directory iterator state (directories)
        cdev_t *cdev; // reference to character device
        blkdev_t *blkdev; // reference to block device
        struct pipe *pipe; // FD_PIPE
        struct sock *sock;  // FD_SOCKET
    };
};

struct vfs_file_ops {
    ssize_t (*read)(struct vfs_file *file, char *buf, size_t count);
    ssize_t (*write)(struct vfs_file *file, const char *buf, size_t count);
    loff_t (*llseek)(struct vfs_file *file, loff_t offset, int whence);
    int (*release)(struct vfs_inode *inode, struct vfs_file *file);
    int (*fsync)(struct vfs_file *file);
    int (*stat)(struct vfs_file *file, struct stat *stat);
};

struct vfs_fdtable {
    spinlock_t lock; // protects the fdtable
    int fd_count;
    int next_fd;
    struct vfs_file *files[NOFILE];
    int ref_count;
};

// Per-process filesystem state
// Allocated on the kernel stack, below utrapframe
struct fs_struct {
    spinlock_t lock; // protects the fs_struct
    struct vfs_inode_ref rooti; // Root inode
    struct vfs_inode_ref cwd;   // Current working directory inode
    int ref_count; // Reference count
};

#endif // KERNEL_VIRTUAL_FILE_SYSTEM_TYPES_H
