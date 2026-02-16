#ifndef KERNEL_VIRTUAL_FILE_SYSTEM_TYPES_H
#define KERNEL_VIRTUAL_FILE_SYSTEM_TYPES_H

#include "compiler.h"
#include "types.h"
#include "list.h"
#include "lock/spinlock.h"
#include "lock/mutex_types.h"
#include "lock/rwsem_types.h"
#include "hlist_type.h"
#include "mm/pcache_types.h"
#include "kobject.h"
#include "vfs/stat.h"

struct statfs;

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
struct vma;

/*
 * Filesystem type structure
 * Protected by global vfs_fs_types_lock
 */
struct vfs_fs_type {
    list_node_t list_entry;
    list_node_t superblocks; // list of struct vfs_superblock
    struct kobject kobj;     // for sysfs representation
    struct {
        uint64 registered : 1;
    };
    int sb_count;
    const char *name;
    struct vfs_fs_type_ops *ops;
};

#define VFS_SUPERBLOCK_HASH_BUCKETS 61

struct vfs_fs_type_ops {
    /*
     * mount - create and initialize a superblock for the filesystem
     * @mountpoint: the inode where the filesystem will be mounted
     * @device:     the device inode backing the filesystem (NULL for non-dev)
     * @flags:      mount flags
     * @data:       filesystem-specific mount data string
     * @ret_sb:     output pointer to the newly created superblock
     *
     * Allocate a superblock, fill in its fields, and leave it in an
     * unmounted state (mountpoint/parent unset) so that the VFS core can
     * attach it to the mount tree.  The returned superblock should have
     * its root_inode preloaded and the root inode's ref count set to 1.
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*mount)(struct vfs_inode *mountpoint, struct vfs_inode *device,
                 int flags, const char *data, struct vfs_superblock **ret_sb);

    /*
     * free - tear down an unmounted or failed superblock
     * @sb: the superblock to free
     *
     * Release all inodes and resources associated with the superblock,
     * including its root inode if present.  Called for superblocks that
     * have not been mounted, or that must be discarded after a failed
     * mount attempt.
     */
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
        uint64 valid : 1;
        uint64 dirty : 1;       // Only indicates whether the metadata of the
                                // superblock is dirty
        uint64 backendless : 1; // Indicates whether the filesystem is
                                // backendless (e.g., tmpfs)
        uint64 initialized
            : 1; // Indicates whether the superblock has been initialized
        uint64 registered : 1; // Indicates whether the superblock is attached
                               // to a filesystem type
        uint64 syncing : 1;    // Currently syncing to backend storage
        uint64 unmounting : 1; // Unmount initiated, blocking new operations
        uint64 attached : 1; // Attached to mount tree (set on mount, cleared on
                             // lazy unmount)
    };
    struct vfs_superblock
        *parent_sb;               // parent superblock if mounted on another fs
    struct vfs_inode *mountpoint; // inode where this sb is mounted
    struct vfs_inode *device;     // device inode (NULL for non-dev fs)
    struct vfs_inode *root_inode; // root inode of this superblock
    struct vfs_superblock_ops *ops;

    struct rwsem lock; // protects the superblock
    void *fs_data;     // filesystem-specific data
    int mount_count;   // Number of superblocks directly mounted under this
                       // superblock
    int refcount;      // Reference count for the superblock
    int orphan_count;  // Number of orphan inodes (n_links=0, ref>0)
    list_node_t orphan_list; // List of orphan inodes

    // Filesystem statistics
    spinlock_t spinlock; // protects the counters below
    size_t block_size;   // Filesystem block size
    // May be 0 if the filesystem does not track total/used blocks
    // (e.g., tmpfs)
    uint64 total_blocks;
    uint64 used_blocks; // Number of blocks used in the filesystem
};

struct vfs_superblock_ops {
    /*
     * alloc_inode - allocate a new on-disk inode
     * @sb: the superblock in which to allocate
     *
     * The returned inode should have its ref count set to 1.
     * The superblock write lock is held during this operation.
     *
     * Returns the new inode, or ERR_PTR(-ENOSPC) if no space remains.
     */
    struct vfs_inode *(*alloc_inode)(struct vfs_superblock *sb);

    /*
     * get_inode - read an inode from the backing store
     * @sb:  the superblock owning the inode
     * @ino: the inode number to fetch
     *
     * Called only when the inode is not already cached in memory; the
     * superblock write lock is held to protect the inode hash list.
     *
     * Filesystem drivers must fill in the following fields:
     *   ino, type, size, mode, ops, and one of: cdev, bdev.
     * Filesystem drivers may also fill in:
     *   n_links, n_blocks, uid, gid, atime, mtime, ctime, fs_data.
     *
     * The returned inode should have its ref count set to 1.
     * Drivers should zero-initialize and populate the inode but should
     * NOT lock or mark it valid; the VFS core does that after inserting
     * the inode into the superblock's hash list.
     *
     * Returns the inode, or ERR_PTR(-ENOENT) if not found/not allocated.
     */
    struct vfs_inode *(*get_inode)(struct vfs_superblock *sb, uint64 ino);

    /*
     * sync_fs - synchronize the superblock with underlying storage
     * @sb:   the superblock to synchronize
     * @wait: if true, the operation should be synchronous
     *
     * Called by vfs_sync_superblock() with the superblock write lock
     * held, so implementations need not acquire additional superblock
     * locks.  If @wait is false, any background I/O should be started
     * but need not complete before returning.
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*sync_fs)(struct vfs_superblock *sb, int wait);

    /*
     * unmount_begin - prepare the superblock for unmounting
     * @sb: the superblock being unmounted
     *
     * Should ensure that:
     *   - The superblock is clean (no dirty data).
     *   - There are no active inodes associated with the superblock.
     *   - No other superblocks are mounted under this superblock.
     *
     * After this function returns, the VFS core will proceed with
     * unmounting the superblock and freeing its resources.
     */
    void (*unmount_begin)(struct vfs_superblock *sb);

    /*
     * add_orphan - register an inode on the orphan list (optional)
     * @sb:    the superblock owning the inode
     * @inode: the orphaned inode (n_links == 0, ref > 0)
     *
     * Used for crash-recovery in backend-backed filesystems.
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*add_orphan)(struct vfs_superblock *sb, struct vfs_inode *inode);

    /*
     * remove_orphan - un-register an inode from the orphan list (optional)
     * @sb:    the superblock owning the inode
     * @inode: the inode to remove from the orphan list
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*remove_orphan)(struct vfs_superblock *sb, struct vfs_inode *inode);

    /*
     * recover_orphans - replay the orphan list after a crash (optional)
     * @sb: the superblock to recover
     *
     * Walks the on-disk orphan list and frees any inodes whose link
     * count reached zero before the previous unclean shutdown.
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*recover_orphans)(struct vfs_superblock *sb);

    /*
     * statfs - report filesystem statistics
     * @sb:  the superblock to query
     * @buf: output buffer for filesystem statistics
     *
     * Fills in filesystem-specific fields of @buf (block size, total
     * blocks, free blocks, inode counts, etc.).  Generic fields are
     * pre-populated by the VFS before this callback is invoked.
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*statfs)(struct vfs_superblock *sb, struct statfs *buf);

    /*
     * begin_transaction - start a filesystem transaction (optional)
     * @sb: the superblock on which to open a transaction
     *
     * DESIGN CHOICE — register callbacks OR manage internally, not both:
     *
     *  Option 1: REGISTER CALLBACKS (simple transactions)
     *    Set begin_transaction and end_transaction to non-NULL.
     *    The VFS calls begin_transaction BEFORE acquiring any locks and
     *    end_transaction AFTER releasing all locks, ensuring correct
     *    lock ordering: transaction → superblock → inode.
     *    FS inode callbacks must NOT call begin/end internally.
     *
     *  Option 2: MANAGE INTERNALLY (complex transaction needs)
     *    Set both to NULL; the FS manages all transactions internally.
     *    Required for batched or nested transactions.
     *    WARNING: calling begin while holding VFS locks can deadlock.
     *
     *  HYBRID (xv6fs example):
     *    Register callbacks for metadata ops (single transaction each);
     *    file ops (write, truncate) manage transactions internally
     *    because they need batching for large files.
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*begin_transaction)(struct vfs_superblock *sb);

    /*
     * end_transaction - commit/close a filesystem transaction (optional)
     * @sb: the superblock whose transaction should be committed
     *
     * See begin_transaction for the design-choice discussion.
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*end_transaction)(struct vfs_superblock *sb);
};

struct vfs_inode {
    hlist_entry_t hash_entry; // entry in vfs_superblock.inodes
    uint64 ino;               // inode number
    uint32 n_links;           // number of hard links
    uint64 n_blocks;          // number of blocks allocated
    loff_t size;              // size in bytes
    uint32 mode;              // permission and type bits
    uint32 uid;               // owner user id
    uint32 gid;               // owner group id
    uint64 atime;             // access time
    uint64 mtime;             // modification time
    uint64 ctime;             // change time

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
     * to prevent other operations from touching it until it is fully
     * initialized and attached to the superblock's inode hash list. Only after
     * that point may the inode be marked valid (valid=1).
     *
     * When an inode is being deleted, it will be marked invalid (valid=0) to
     * prevent new operations from starting on it. Existing operations should
     * complete before the inode is fully deleted and freed.
     *
     * An inode marked invalid should be removed from the superblock's inode
     * hash list.
     *
     * Typically, the inode mutex remains held throughout the deletion process,
     * and the last reference release will free the inode if it is marked
     * invalid.
     *
     * dirty indicates whether the inode's on disk metadata has been modified
     * and needs to be synced to disk. Callers must hold the inode mutex when
     * modifying inode metadata so updates to the valid/dirty flags remain
     * ordered.
     */
    struct {
        uint64 valid : 1;
        uint64 dirty : 1;
        uint64 mount : 1; // indicates whether this inode is a mountpoint
        uint64 orphan
            : 1; // inode is orphaned (n_links=0, ref>0, on orphan_list)
        uint64 destroying
            : 1; // inode is being destroyed (destroy_inode in progress)
        uint64 delay_put
            : 1; // delay freeing inode until later (used by some filesystems)
    };
    list_node_t orphan_entry; // entry in sb->orphan_list when orphaned
    struct thread *owner;     // thread that holds the lock
    struct vfs_superblock *sb;
    // The two pcaches below are managed by the drivers/filesystems
    // Initialize them as needed
    struct pcache *i_mapping; // page cache for its backend inode data
    struct pcache i_data;     // page cache for its data blocks
    struct vfs_inode_ops *ops;
    int ref_count; // reference count
    void *fs_data; // filesystem-specific data
    struct vfs_inode
        *parent; // parent inode for directories (self for root inodes)
    char *name;  // directory name (for directories only, NULL for root)
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
 * Inode operations — primarily metadata operations.
 *
 * Data read/write is handled by vfs_file_ops.
 * The VFS core acquires the inode mutex before invoking any callback.
 *
 * The following operations additionally require the superblock write lock:
 *   create, mkdir, rmdir, unlink, mknod, move, destroy_inode
 *
 * The following operations must increment the refcount of the returned inode:
 *   create, mkdir, mknod, symlink
 */
struct vfs_inode_ops {
    /*
     * lookup - look up a name inside a directory
     * @dir:      the directory inode to search
     * @dentry:   output dentry to populate on success
     * @name:     the name to look up
     * @name_len: length of @name in bytes
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*lookup)(struct vfs_inode *dir, struct vfs_dentry *dentry,
                  const char *name, size_t name_len);

    /*
     * dir_iter - iterate to the next directory entry
     * @dir:        the directory inode
     * @iter:       iteration state (cookies / index)
     * @ret_dentry: output dentry for the next entry
     *
     * Returns 0 on success, -ENOENT when no more entries remain,
     * or another negative error code on failure.
     */
    int (*dir_iter)(struct vfs_inode *dir, struct vfs_dir_iter *iter,
                    struct vfs_dentry *ret_dentry);

    /*
     * readlink - read the target of a symbolic link
     * @inode:  the symlink inode
     * @buf:    buffer to receive the link target
     * @buflen: size of @buf in bytes
     *
     * Returns the number of bytes placed in @buf (excluding any
     * NUL terminator), or a negative error code on failure.
     */
    ssize_t (*readlink)(struct vfs_inode *inode, char *buf, size_t buflen);

    /*
     * create - create a regular file
     * @dir:      the parent directory inode
     * @mode:     permission bits for the new file
     * @name:     name of the new file
     * @name_len: length of @name in bytes
     *
     * Returns the new inode (ref count 1), or ERR_PTR on failure.
     */
    struct vfs_inode *(*create)(struct vfs_inode *dir, mode_t mode,
                                const char *name, size_t name_len);

    /*
     * getattr - retrieve inode attributes
     * @inode: the inode to query
     * @stat:  output stat buffer to populate
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*getattr)(struct vfs_inode *inode, struct stat *stat);

    /*
     * setattr - update inode attributes
     * @inode: the inode to modify
     * @stat:  stat buffer containing the new attribute values
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*setattr)(struct vfs_inode *inode, const struct stat *stat);

    /*
     * link - create a hard link
     * @old:      the existing inode to link to
     * @dir:      the directory in which the new link is created
     * @name:     name for the new link
     * @name_len: length of @name in bytes
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*link)(struct vfs_inode *old, struct vfs_inode *dir, const char *name,
                size_t name_len);

    /*
     * unlink - remove a directory entry
     * @dentry: the dentry describing the entry to remove
     * @target: the inode the entry refers to
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*unlink)(struct vfs_dentry *dentry, struct vfs_inode *target);

    /*
     * mkdir - create a directory
     * @dir:      the parent directory inode
     * @mode:     permission bits for the new directory
     * @name:     name of the new directory
     * @name_len: length of @name in bytes
     *
     * Returns the new inode (ref count 1), or ERR_PTR on failure.
     */
    struct vfs_inode *(*mkdir)(struct vfs_inode *dir, mode_t mode,
                               const char *name, size_t name_len);

    /*
     * rmdir - remove an empty directory
     * @dentry: the dentry describing the directory to remove
     * @target: the directory inode to remove
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*rmdir)(struct vfs_dentry *dentry, struct vfs_inode *target);

    /*
     * mknod - create a special file (device node, FIFO, etc.)
     * @dir:      the parent directory inode
     * @mode:     permission and type bits
     * @dev:      device number (for device nodes)
     * @name:     name of the new node
     * @name_len: length of @name in bytes
     *
     * Returns the new inode (ref count 1), or ERR_PTR on failure.
     */
    struct vfs_inode *(*mknod)(struct vfs_inode *dir, mode_t mode, dev_t dev,
                               const char *name, size_t name_len);

    /*
     * move - rename or move a file/directory within the same filesystem
     * @old_dir:    the source directory inode
     * @old_dentry: the dentry of the entry being moved
     * @new_dir:    the destination directory inode
     * @name:       new name for the entry
     * @name_len:   length of @name in bytes
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*move)(struct vfs_inode *old_dir, struct vfs_dentry *old_dentry,
                struct vfs_inode *new_dir, const char *name, size_t name_len);

    /*
     * symlink - create a symbolic link
     * @dir:        the parent directory inode
     * @mode:       permission bits for the new symlink
     * @name:       name of the new symlink
     * @name_len:   length of @name in bytes
     * @target:     the symlink target path
     * @target_len: length of @target in bytes
     *
     * Returns the new inode (ref count 1), or ERR_PTR on failure.
     */
    struct vfs_inode *(*symlink)(struct vfs_inode *dir, mode_t mode,
                                 const char *name, size_t name_len,
                                 const char *target, size_t target_len);

    /*
     * truncate - change the size of a file
     * @inode:    the inode to truncate
     * @new_size: the desired new size in bytes
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*truncate)(struct vfs_inode *inode, loff_t new_size);

    /*
     * destroy_inode - release on-disk inode resources
     * @inode: the inode being destroyed
     *
     * Frees the backing on-disk storage (blocks, etc.) associated
     * with the inode.  Called with the superblock write lock held.
     */
    void (*destroy_inode)(struct vfs_inode *inode);

    /*
     * free_inode - release the in-memory inode structure
     * @inode: the inode to free
     *
     * Frees any filesystem-private memory attached to the inode.
     * Called after the inode has been removed from the hash list.
     */
    void (*free_inode)(struct vfs_inode *inode);

    /*
     * dirty_inode - mark an inode as dirty
     * @inode: the inode whose metadata has changed
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*dirty_inode)(struct vfs_inode *inode);

    /*
     * sync_inode - write an inode to disk
     * @inode: the inode to synchronize
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*sync_inode)(struct vfs_inode *inode);

    /*
     * open - perform filesystem-specific open processing
     * @inode:   the inode being opened
     * @file:    the file structure being initialized
     * @f_flags: open flags (O_RDONLY, O_WRONLY, etc.)
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*open)(struct vfs_inode *inode, struct vfs_file *file, int f_flags);
};

/* No dentry cache right now */
struct vfs_dentry {
    struct vfs_superblock *sb;
    struct vfs_inode *parent; // parent inode
    uint64 ino;               // inode number
    // The `name` field is managed by slab allocator
    char *name;
    uint16 name_len;
    int64 cookies; // Filesystem-private cookie; opaque to callers. The VFS uses
                   // internal sentinel values during directory iteration, but
                   // external callers must not rely on any specific value.
};

struct vfs_dir_iter {
    int64 cookies; // Filesystem-private cookie; opaque to callers
    int64 index;   // Number of entries successfully returned so far
};

struct vfs_file {
    list_node_t list_entry; // entry in global open file table
    struct vfs_inode_ref inode;
    int f_flags;   // file access mode
    int ref_count; // reference count
    struct vfs_file_ops *ops;
    void *private_data; // filesystem-specific data
    mutex_t lock;       // protects f_pos
    union {
        loff_t f_pos;                 // file position(regular files)
        struct vfs_dir_iter dir_iter; // directory iterator state (directories)
        cdev_t *cdev;                 // reference to character device
        blkdev_t *blkdev;             // reference to block device
        struct pipe *pipe;            // FD_PIPE
        struct sock *sock;            // FD_SOCKET
    };
};

struct vfs_file_ops {
    /*
     * read - read data from a file
     * @file:  the open file
     * @buf:   destination buffer
     * @count: number of bytes to read
     * @user:  true if @buf is a user-space address
     *
     * Returns the number of bytes read, or a negative error code.
     */
    ssize_t (*read)(struct vfs_file *file, char *buf, size_t count, bool user);

    /*
     * write - write data to a file
     * @file:  the open file
     * @buf:   source buffer
     * @count: number of bytes to write
     * @user:  true if @buf is a user-space address
     *
     * Returns the number of bytes written, or a negative error code.
     */
    ssize_t (*write)(struct vfs_file *file, const char *buf, size_t count,
                     bool user);

    /*
     * llseek - reposition the file offset
     * @file:   the open file
     * @offset: byte offset (interpretation depends on @whence)
     * @whence: SEEK_SET, SEEK_CUR, or SEEK_END
     *
     * Returns the resulting file position, or a negative error code.
     */
    loff_t (*llseek)(struct vfs_file *file, loff_t offset, int whence);

    /*
     * release - clean up when the last reference to a file is closed
     * @inode: the inode associated with the file
     * @file:  the file being released
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*release)(struct vfs_inode *inode, struct vfs_file *file);

    /*
     * fsync - synchronize a range of file data to storage
     * @file:  the open file
     * @start: starting byte offset of the range
     * @len:   length of the range (0 means to end of file)
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*fsync)(struct vfs_file *file, loff_t start, loff_t len);

    /*
     * fflush - flush all dirty data for a file
     * @file: the open file
     *
     * Returns 0 on success, or a negative error code on failure.
     */
    int (*fflush)(struct vfs_file *file);

    /*
     * poll - check whether the file is ready for I/O
     * @file:   the open file
     * @events: requested poll events (POLLIN, POLLOUT, ...)
     *
     * Returns the subset of @events that are currently satisfied.
     * If NULL, the VFS poll code falls back to "always ready".
     */
    int (*poll)(struct vfs_file *file, short events);

    /*
     * fault - Demand-page a single page for a file-backed mapping
     * @file:  the file being mapped
     * @vma:   the faulting VMA (start/end/flags/pgoff already set)
     * @va:    faulting virtual address (page-aligned)
     *
     * Called WITHOUT the vm pgtable spinlock held (may sleep for I/O).
     * The vm rlock is held by the caller.
     *
     * Must allocate a physical page, populate it with file data (or
     * zeroes for pages beyond the file), and return the physical address.
     * Returns NULL on failure.
     *
     * If this callback is NULL the VM uses its generic page-cache fault path.
     */
    void *(*fault)(struct vfs_file *file, struct vma *vma, uint64 va);
};

struct vfs_fdtable {
    spinlock_t lock; // protects the fdtable
    int fd_count;
    struct vfs_file *files[NOFILE];
    uint64 files_bitmap[1];
    uint64 cloexec_bitmap[1];
    int ref_count;
};

// Per-process filesystem state
// Allocated on the kernel stack, below utrapframe
struct fs_struct {
    spinlock_t lock;            // protects the fs_struct
    struct vfs_inode_ref rooti; // Root inode
    struct vfs_inode_ref cwd;   // Current working directory inode
    int ref_count;              // Reference count
};

#endif // KERNEL_VIRTUAL_FILE_SYSTEM_TYPES_H
