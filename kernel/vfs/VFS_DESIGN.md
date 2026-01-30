# Virtual File System (VFS) Design Document

## Table of Contents
1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Core Data Structures](#core-data-structures)
4. [Locking Strategy](#locking-strategy)
5. [Component Design](#component-design)
6. [Mount and Unmount](#mount-and-unmount)
7. [Path Resolution](#path-resolution)
8. [Reference Counting and Lifetime Management](#reference-counting-and-lifetime-management)
9. [File Operations](#file-operations)
10. [Special Features](#special-features)

---

## Overview

The VFS layer provides a unified interface for different file system implementations in xv6. It abstracts file system operations and provides a common API for user-space programs, allowing multiple file systems to coexist and be mounted at different points in the directory tree.

### Design Goals
- **Abstraction**: Provide a uniform interface for different file system types
- **Extensibility**: Easy to add new file system implementations
- **POSIX Compatibility**: Support standard UNIX/POSIX file operations
- **Safety**: Robust reference counting and locking to prevent use-after-free bugs
- **Performance**: Efficient caching and minimal lock contention

### Supported Features
- Multiple file system types (xv6fs, tmpfs)
- Hierarchical mounting (file systems mounted on other file systems)
- Symbolic links with loop detection
- Hard links
- Character and block device files
- Pipes and sockets
- Directory iteration
- Orphan inode management (inodes with nlink=0 but still open)

---

## Architecture

### Layer Structure

```
┌─────────────────────────────────────────┐
│        System Call Layer                │
│    (sys_vfs_open, sys_vfs_read, etc.)   │
└─────────────────────────────────────────┘
                   ↓
┌─────────────────────────────────────────┐
│          VFS Core Layer                 │
│  - Path resolution                      │
│  - File descriptor management           │
│  - Inode cache                          │
│  - Mount point management               │
└─────────────────────────────────────────┘
                   ↓
┌─────────────────────────────────────────┐
│     File System Implementations         │
│    - xv6fs (original xv6 file system)   │
│    - tmpfs (memory-based file system)   │
└─────────────────────────────────────────┘
                   ↓
┌─────────────────────────────────────────┐
│         Device Layer                    │
│  - Block devices (virtio_disk)          │
│  - Character devices (console, etc.)    │
└─────────────────────────────────────────┘
```

### Key Components

1. **File System Type (`vfs_fs_type`)**: Represents a file system implementation (e.g., xv6fs, tmpfs)
2. **Superblock (`vfs_superblock`)**: Represents a mounted instance of a file system
3. **Inode (`vfs_inode`)**: Represents a file, directory, or special file
4. **File (`vfs_file`)**: Represents an open file descriptor
5. **File Descriptor Table (`vfs_fdtable`)**: Per-process table of open files
6. **Dentry (`vfs_dentry`)**: Temporary structure for directory entry lookup results

---

## Core Data Structures

### 1. File System Type (`struct vfs_fs_type`)

```c
struct vfs_fs_type {
    list_node_t list_entry;       // Global fs type list
    list_node_t superblocks;      // List of mounted instances
    struct kobject kobj;          // Kernel object for sysfs
    int sb_count;                 // Number of superblocks
    const char *name;             // File system name (e.g., "xv6fs")
    struct vfs_fs_type_ops *ops;  // Operations
};
```

**Operations:**
- `mount()`: Create and initialize a new superblock instance
- `free()`: Destroy a superblock instance

**Lifecycle:**
- Created via `vfs_fs_type_allocate()`
- Registered via `vfs_register_fs_type()`
- Reference counted via kobject
- Unregistered via `vfs_unregister_fs_type()`

### 2. Superblock (`struct vfs_superblock`)

```c
struct vfs_superblock {
    list_node_t siblings;           // Entry in fs_type.superblocks
    struct vfs_fs_type *fs_type;    // File system type
    
    // State flags
    uint8 valid:1;        // Superblock is valid
    uint8 dirty:1;        // Needs sync
    uint8 attached:1;     // Attached to mount tree
    uint8 syncing:1;      // Sync in progress
    uint8 unmounting:1;   // Unmount in progress
    uint8 lazy_unmount:1; // Lazy unmount requested
    
    // Mount hierarchy
    struct vfs_superblock *parent_sb;  // Parent superblock
    struct vfs_inode *mountpoint;      // Where this is mounted
    struct vfs_inode *device;          // Device inode (or NULL)
    struct vfs_inode *root_inode;      // Root inode of this fs
    
    struct vfs_superblock_ops *ops;
    struct rwlock lock;                // Protects metadata
    void *fs_data;                     // FS-specific data
    
    int mount_count;    // # of child mounts
    int refcount;       // Reference count
    int orphan_count;   // # of orphan inodes
    list_node_t orphan_list;
    
    // Statistics
    struct spinlock spinlock;
    size_t block_size;
    uint64 total_blocks;
    uint64 used_blocks;
};
```

**Operations:**
- `alloc_inode()`: Allocate a new inode on disk
- `get_inode()`: Read inode from disk by inode number
- `sync_fs()`: Sync file system state to disk
- `unmount_begin()`: Begin unmount process
- `unmount_complete()`: Complete unmount and free resources

**Lifecycle:**
- Created by file system's `mount()` callback
- Added to mount tree via `vfs_mount()`
- Reference counted via `vfs_superblock_dup()/vfs_superblock_put()`
- Removed from mount tree via `vfs_unmount()`
- Freed by file system's `free()` callback

### 3. Inode (`struct vfs_inode`)

```c
struct vfs_inode {
    uint64 ino;                    // Inode number
    mode_t mode;                   // Permissions and file type (S_IFREG, S_IFDIR, etc.)
    loff_t size;                   // File size
    
    struct vfs_superblock *sb;     // Owning superblock
    struct vfs_inode_ops *ops;     // Operations
    
    struct mutex mutex;            // Protects inode fields
    struct completion completion;  // For I/O completion
    hlist_entry_t hash_entry;      // Entry in sb->inode_hash
    list_node_t orphan_entry;      // Entry in sb->orphan_list
    
    int ref_count;                 // Reference count
    
    uint8 valid:1;       // Inode is valid
    uint8 dirty:1;       // Needs writeback
    uint8 is_orphan:1;   // In orphan list
    
    // File metadata
    uint32 n_links;      // Hard link count
    uint32 n_blocks;     // Allocated blocks
    uint32 uid, gid;     // Owner
    uint64 atime, mtime, ctime;
    
    union {
        dev_t cdev;              // Character device
        dev_t bdev;              // Block device
        struct vfs_superblock *mounted_sb;  // Mounted fs
    };
    
    void *fs_data;       // FS-specific data
};
```

**File Type Determination:**

The `mode` field uses standard POSIX file type bits (from `stat.h`):
- `S_IFREG` (0100000): Regular file
- `S_IFDIR` (0040000): Directory
- `S_IFLNK` (0120000): Symbolic link
- `S_IFCHR` (0020000): Character device
- `S_IFBLK` (0060000): Block device
- `S_IFSOCK` (0140000): Socket
- `S_IFIFO` (0010000): Named pipe (FIFO)

Use the standard macros to test file types:
- `S_ISREG(mode)`: Check if regular file
- `S_ISDIR(mode)`: Check if directory
- `S_ISLNK(mode)`: Check if symbolic link
- `S_ISCHR(mode)`: Check if character device
- `S_ISBLK(mode)`: Check if block device
- `S_ISSOCK(mode)`: Check if socket
- `S_ISFIFO(mode)`: Check if FIFO

**Operations:**
- `lookup()`: Look up a directory entry
- `readlink()`: Read symbolic link target
- `create()`: Create a regular file
- `mknod()`: Create a special file (device, fifo)
- `mkdir()`: Create a directory
- `link()`: Create a hard link
- `unlink()`: Remove a directory entry
- `rmdir()`: Remove a directory
- `move()`: Rename/move a file
- `symlink()`: Create a symbolic link
- `truncate()`: Change file size
- `read()`: Read file data
- `write()`: Write file data
- `readdir()`: Read directory entries
- `writeback()`: Write inode metadata to disk

**Lifecycle:**
- Loaded from disk via `vfs_get_inode()`
- Cached in superblock's inode hash table
- Reference counted via `vfs_idup()/vfs_iput()`
- Removed from cache when refcount reaches 0
- Written back to disk when dirty

### 4. File (`struct vfs_file`)

```c
struct vfs_file {
    list_node_t list_entry;        // Entry in global file table
    struct vfs_inode *inode;       // Associated inode
    struct vfs_file_ops *ops;      // Operations
    struct mutex lock;             // Protects file state
    
    int ref_count;                 // Reference count
    loff_t offset;                 // Current file position
    int f_flags;                   // Open flags (O_RDONLY, etc.)
    int fd;                        // File descriptor number
    
    union {
        struct cdev cdev;          // Character device
        struct blkdev blkdev;      // Block device
        struct pipe *pipe;         // Pipe
        struct sock *sock;         // Socket
    };
};
```

**Operations:**
- `read()`: Read from file
- `write()`: Write to file
- `lseek()`: Seek to position
- `release()`: Release file resources

**Lifecycle:**
- Created via `vfs_fileopen()`
- Reference counted via `vfs_filedup()`
- Closed via `vfs_fileclose()`
- Freed when refcount reaches 0

### 5. File Descriptor Table (`struct vfs_fdtable`)

```c
struct vfs_fdtable {
    struct spinlock lock;            // Protects writes to table
    struct vfs_file *files[NOFILE];  // Array of file pointers (RCU-protected)
    uint64 files_bitmap[...];        // Bitmap for fast fd allocation
    int fd_count;                    // Number of open files (atomic)
    int ref_count;                   // Reference count (for CLONE_FILES)
};
```

**Synchronization:**

The fdtable uses a hybrid **RCU + spinlock** approach for concurrent access:

- **Readers** (`vfs_fdtable_get_file`): Use RCU read-side critical section
  - No spinlock needed - wait-free read path
  - Must increment file refcount before exiting RCU critical section
  
- **Writers** (`vfs_fdtable_alloc_fd`, `vfs_fdtable_dealloc_fd`): 
  - Must hold `fdtable->lock` spinlock
  - Use `rcu_assign_pointer()` for pointer updates
  - File release deferred via `call_rcu()` for safe reclamation

- **Cloning** (`vfs_fdtable_clone`):
  - Uses RCU read lock to safely iterate source fdtable
  - Supports `CLONE_FILES` flag for shared fdtable (just increments refcount)

**RCU-Safe File Close Pattern:**

When closing a file descriptor, the file cannot be freed immediately because
concurrent readers may still be accessing it via RCU. The pattern is:

```c
spin_lock(&fdtable->lock);
file = vfs_fdtable_dealloc_fd(fdtable, fd);  // Remove from table
spin_unlock(&fdtable->lock);

call_rcu(NULL, rcu_callback, file);  // Defer vfs_fput() until grace period
```

This ensures all concurrent `vfs_fdtable_get_file()` calls complete before
the file's refcount is decremented.

**Design:**
- Uses bitmap for O(1) allocation of lowest available fd
- Values in `files[]` array are RCU-protected pointers
- Valid file pointers have addresses > NOFILE
- Reference counted for shared fdtables (CLONE_FILES)

**Operations:**
- `vfs_fdtable_init()`: Initialize empty table
- `vfs_fdtable_alloc_fd()`: Allocate lowest available fd (caller holds lock)
- `vfs_fdtable_get_file()`: Get file by fd (RCU-protected, increments refcount)
- `vfs_fdtable_dealloc_fd()`: Free fd and return file (caller holds lock)
- `vfs_fdtable_clone()`: Duplicate or share table (for fork/clone)
- `vfs_fdtable_put()`: Release reference, close all files when last ref

**Locking Requirements:**

| Function | Lock Required | Notes |
|----------|---------------|-------|
| `vfs_fdtable_get_file()` | None (uses RCU) | Increments file refcount |
| `vfs_fdtable_alloc_fd()` | `fdtable->lock` | Caller must hold spinlock |
| `vfs_fdtable_dealloc_fd()` | `fdtable->lock` | Caller must hold spinlock |
| `vfs_fdtable_clone()` | None (uses RCU) | Safe for concurrent reads |
| `vfs_fdtable_put()` | None | Only when last reference |

### 6. Dentry (`struct vfs_dentry`)

```c
struct vfs_dentry {
    char name[256];              // Entry name
    size_t name_len;             // Name length
    uint64 ino;                  // Inode number
    struct vfs_superblock *sb;   // Superblock
    struct vfs_inode *inode;     // Cached inode (may be NULL)
    struct vfs_inode *parent;    // Parent directory
    void *cookies;               // FS-specific iterator state
};
```

**Purpose:**
- Temporary structure for directory lookup results
- Not cached (unlike Linux dcache)
- Used during path resolution and directory iteration

---

## Locking Strategy

### Lock Hierarchy

To prevent deadlock, locks must be acquired in this order:

```
1. Global mount mutex (__mount_mutex)
2. Superblock rwlock (parent before child)
3. Inode mutex (parent/ancestor before child/descendant)
4. File descriptor table spinlock (fdtable->lock)
5. File mutex
6. Buffer cache mutex
7. Log spinlock (xv6fs internal)
```

**Important:** RCU read-side critical sections (`rcu_read_lock/unlock`) are
lightweight and can be nested with any of the above locks. However, spinlocks
must NOT be held when calling functions that may sleep (e.g., `vm_copyout`).

### Rules

1. **Mount Mutex**: Protects the global mount tree structure
   - Acquired via `vfs_mount_lock()`
   - Must be held when modifying mount relationships

2. **Superblock RWLock**: Protects superblock metadata and inode hash table
   - Read lock: Lookup inodes, read metadata
   - Write lock: Modify metadata, add/remove inodes, mount/unmount
   - Special case: File I/O operations do NOT require superblock lock

3. **Inode Mutex**: Protects individual inode state
   - Must be held when accessing inode fields
   - Multiple inode locks:
     - Acquire parent before child
     - For siblings: acquire lower memory address first
     - Use special helpers: `vfs_ilock_two_nondirectories()`, `vfs_ilock_two_directories()`

4. **File Descriptor Table Spinlock**: Protects fdtable modifications
   - Must be held for `vfs_fdtable_alloc_fd()` and `vfs_fdtable_dealloc_fd()`
   - NOT required for `vfs_fdtable_get_file()` (uses RCU instead)
   - Must NOT call sleeping functions while holding this lock

5. **File Mutex**: Protects file descriptor state (offset, flags)
   - Rarely held for extended periods
   - File I/O may hold both inode and file locks

### RCU Usage

RCU (Read-Copy-Update) is used for wait-free file descriptor table lookups:

```c
// Reader pattern (vfs_fdtable_get_file)
rcu_read_lock();
file = rcu_dereference(fdtable->files[fd]);
if (IS_VALID(file)) {
    vfs_fdup(file);  // Increment refcount before leaving RCU
}
rcu_read_unlock();

// Writer pattern (vfs_fdtable_dealloc_fd)
spin_lock(&fdtable->lock);
file = fdtable->files[fd];
rcu_assign_pointer(fdtable->files[fd], NULL);  // Safe for concurrent readers
spin_unlock(&fdtable->lock);

// Defer actual file release until all readers complete
call_rcu(NULL, callback, file);
```

**RCU Guarantees:**
- Readers see consistent pointer values (old or new, never torn)
- Writers use `rcu_assign_pointer()` for proper memory ordering
- `call_rcu()` defers callback until all pre-existing readers complete

### Critical Locking Patterns

#### Path Resolution
```c
// Acquire parent directory lock
vfs_ilock(parent);
// Lookup child (may load from disk)
vfs_ilookup(parent, &dentry, name, len);
// Get child inode
child = vfs_get_dentry_inode(&dentry);
vfs_iunlock(parent);
// Now hold reference to child
```

#### Two-Inode Operations (link, rename)
```c
// For non-directories
vfs_ilock_two_nondirectories(inode1, inode2);
// ... perform operation ...
vfs_iunlock_two(inode1, inode2);

// For directories
if (vfs_ilock_two_directories(dir1, dir2) < 0) {
    // One is ancestor of the other, abort
}
// ... perform operation ...
vfs_iunlock_two(dir1, dir2);
```

#### Mount/Unmount
```c
vfs_mount_lock();
vfs_superblock_wlock(parent_sb);
vfs_ilock(mountpoint);
// ... modify mount tree ...
vfs_iunlock(mountpoint);
vfs_superblock_unlock(parent_sb);
vfs_mount_unlock();
```

### Deadlock Avoidance

1. **Never hold locks across filesystems**: Drop inode locks before acquiring locks from a different superblock

2. **Parent-child ordering**: Always lock parent/ancestor before child/descendant

3. **Address ordering**: For siblings, lock lower address first

4. **Read-then-write**: Don't upgrade read locks to write locks (drop and reacquire)

5. **Completion-based I/O**: Use completion variables to wait for I/O without holding locks

6. **No sleeping with spinlocks**: Never call functions that may sleep (e.g., `vm_copyout`, rwlock operations) while holding a spinlock. Release spinlocks before such operations.

7. **RCU is lightweight**: `rcu_read_lock/unlock` can be nested with any lock, but the RCU critical section must not sleep. RCU is ideal for read-heavy paths like fd table lookups.

---

## Component Design

### File System Type Management

#### Registration
```c
struct vfs_fs_type *fs = vfs_fs_type_allocate();
fs->name = "xv6fs";
fs->ops = &xv6fs_ops;
vfs_register_fs_type(fs);
```

#### Lookup
```c
struct vfs_fs_type *fs = vfs_get_fs_type("xv6fs");
// Use fs...
vfs_put_fs_type(fs);  // Release reference
```

#### Global State
- All registered file system types are in a global list
- Protected by mount mutex
- Each fs_type has a list of its mounted superblocks

### Superblock Management

#### Inode Hash Table
- Each superblock has a hash table of cached inodes
- Hash function: `ino % VFS_SUPERBLOCK_HASH_BUCKETS`
- Protected by superblock write lock
- Inodes are never removed from hash while valid

#### Mount Tree
- Root of entire VFS: `vfs_root_inode` (special inode, no superblock)
- Each superblock has:
  - `parent_sb`: Parent superblock in mount hierarchy
  - `mountpoint`: Directory inode where this is mounted
  - `mounted_sb`: For mountpoint inodes, the mounted superblock
  
#### Example Mount Tree
```
vfs_root_inode (VFS root)
  └─ mounted_sb → sb1 (xv6fs on /)
       └─ mountpoint → /mnt (type=MNT)
            └─ mounted_sb → sb2 (tmpfs on /mnt)
```

### Inode Management

#### Inode Cache
- Inodes are cached in superblock's hash table
- Lookup sequence:
  1. Check hash table (`__vfs_inode_hash_get()`)
  2. If not found, call `sb->ops->get_inode()` to load from disk
  3. Add to hash table (`__vfs_inode_hash_add()`)
  
#### Inode Lifetime
1. **Created**: Allocated and added to hash table (refcount=1)
2. **In Use**: Referenced by dentries, files, or code (refcount>0)
3. **Orphan**: nlink=0 but refcount>0 (added to orphan list)
4. **Removal**: refcount=0, removed from hash and freed

#### Orphan Management
- When a file is unlinked but still open, it becomes an orphan
- Orphans are kept in superblock's orphan list
- On unmount, orphans prevent unmount (busy)
- Lazy unmount allows orphans to persist until last close

### File Descriptor Management

#### Process File State
```c
struct proc {
    struct vfs_struct *fs;       // VFS state (cwd, root) - may be shared
    struct vfs_fdtable *fdtable; // File descriptor table - may be shared
};

struct vfs_struct {
    struct vfs_inode_ref rooti;  // Root directory reference (for chroot)
    struct vfs_inode_ref cwd;    // Current working directory reference
    struct spinlock lock;         // Protects cwd/root modifications
    int ref_count;                // Reference count (for CLONE_FS)
};
```

**Note on Inode References:**

The process structure uses `vfs_inode_ref` instead of direct `vfs_inode*` pointers to handle potential inode invalidation across mount/unmount operations. This ensures safe access to the process's root and current working directories even if the underlying filesystem is unmounted.

See `vfs_inode_get_ref()`, `vfs_inode_put_ref()`, and `vfs_inode_deref()` for reference management operations.

#### File Descriptor Allocation

The fdtable uses **RCU-protected** lookups with **spinlock-protected** modifications:

```c
// Allocating a new file descriptor
spin_lock(&p->fdtable->lock);
int fd = vfs_fdtable_alloc_fd(p->fdtable, file);  // Increments file refcount
spin_unlock(&p->fdtable->lock);

// Getting a file from fd (wait-free)
struct vfs_file *f = vfs_fdtable_get_file(p->fdtable, fd);  // Uses RCU
// ... use file ...
vfs_fput(f);  // Must release the reference

// Closing a file descriptor
spin_lock(&p->fdtable->lock);
struct vfs_file *f = vfs_fdtable_dealloc_fd(p->fdtable, fd);
spin_unlock(&p->fdtable->lock);
call_rcu(NULL, callback, f);  // Defer vfs_fput() until RCU grace period
```

**Key Points:**
- Uses bitmap for O(1) allocation of lowest available fd
- Lookups are wait-free via RCU (no lock contention on read path)
- File release deferred via RCU to prevent use-after-free
- Supports shared fdtables (`CLONE_FILES`) via reference counting

---

## Mount and Unmount

### Mount Process

#### 1. Preparation
```c
vfs_mount(type, mountpoint, device, flags, data);
```

#### 2. Validation
- Verify mountpoint is a valid directory
- Check permissions
- Verify file system type exists

#### 3. Call FS Driver
```c
fs_type->ops->mount(mountpoint, device, flags, data, &new_sb);
```
- Driver creates and initializes superblock
- Loads root inode
- Sets up FS-specific structures

#### 4. Attach to Mount Tree
```c
vfs_mount_lock();
vfs_superblock_wlock(parent_sb);
vfs_ilock(mountpoint);

// Convert directory to mountpoint (mark as having mounted fs)
mountpoint->mounted_sb = new_sb;
vfs_idup(mountpoint);  // Extra reference

// Link superblocks
new_sb->parent_sb = parent_sb;
new_sb->mountpoint = mountpoint;
new_sb->attached = 1;

vfs_iunlock(mountpoint);
vfs_superblock_unlock(parent_sb);
vfs_mount_unlock();
```

### Unmount Process

#### Normal Unmount
```c
vfs_unmount(mountpoint);
```

1. **Validation**
   - Check mountpoint is valid
   - Verify caller has permissions
   
2. **Check Busy**
   - No processes have cwd in this filesystem
   - No open files from this filesystem
   - No orphan inodes
   - No child mounts
   
3. **Detach**
   - Set `sb->unmounting = 1`
   - Remove from mount tree
   - Restore mountpoint to directory type
   
4. **Sync and Free**
   - Call `sb->ops->unmount_begin()`
   - Sync all dirty inodes
   - Call `sb->ops->unmount_complete()`
   - Free superblock

#### Lazy Unmount
```c
vfs_unmount_lazy(mountpoint);
```

- Sets `sb->lazy_unmount = 1`
- Detaches from mount tree immediately
- Allows orphan inodes to persist
- Final cleanup happens when last inode is closed

### Mount Point Traversal

During path resolution, when a mount point is encountered:

```c
static struct vfs_inode *__get_mnt_recursive(struct vfs_inode *dir) {
    while (dir->mounted_sb != NULL) {
        dir = dir->mounted_sb->root_inode;
    }
    return dir;
}
```

---

## Path Resolution

### Overview

Path resolution converts a pathname to an inode. It handles:
- Absolute paths (start from root)
- Relative paths (start from cwd)
- Symbolic links (with loop detection)
- Mount point traversal
- "." and ".." special directories

### Main Functions

#### `vfs_namei(path, path_len)`
Resolve complete path to inode.

#### `vfs_nameiparent(path, path_len, name, name_size)`
Resolve to parent directory, return last component in `name`.

### Algorithm

```
1. Determine starting point:
   - Absolute path: start from process root
   - Relative path: start from process cwd

2. Split path into components (separated by '/')

3. For each component:
   a. If ".", stay at current inode
   b. If "..", go to parent (handle mount boundaries)
   c. Otherwise:
      - Acquire current directory lock
      - Call vfs_ilookup() to find entry
      - Handle symbolic links:
        * Check symloop count
        * Read link target
        * Recursively resolve
      - Handle mount points:
        * Traverse to mounted filesystem root
   
4. Return final inode
```

### Special Cases

#### Symbolic Link Resolution
- Maximum depth: `VFS_SYMLOOP_MAX` (8)
- Counter incremented for each symlink
- Both absolute and relative symlink targets
- Last component: follow only if O_NOFOLLOW not set

#### ".." Handling
- **Local root**: Return itself (can't go higher)
- **Mount boundary**: Jump to parent filesystem
  ```c
  if (dir == dir->sb->root_inode && dir->sb->parent_sb) {
      return dir->sb->mountpoint->parent;  // Cross mount boundary
  }
  ```
- **Normal directory**: Let driver handle it

#### Root Isolation (chroot)
- Each process has `fs.root` inode
- ".." at process root returns process root
- Path resolution never escapes chroot jail

---

## Reference Counting and Lifetime Management

### Inode Reference Counting

#### Rules
1. Any code using an inode must hold a reference
2. Functions returning inodes return them with +1 refcount
3. Use `vfs_idup()` to add a reference
4. Use `vfs_iput()` to drop a reference
5. Never access an inode after last `vfs_iput()`

#### Reference Sources
- **Inode hash table**: Always has 1 reference (the "cache" reference)
- **Parent directory**: Implicit reference (parent is cached → child can be looked up)
- **Mount tree**: Mountpoint inodes have extra reference
- **Open files**: Each open file holds inode reference
- **Current directory**: Process cwd holds reference
- **Active operations**: Code actively using inode holds reference

#### Example
```c
struct vfs_inode *inode = vfs_namei("/path/to/file", 14);
if (IS_ERR_OR_NULL(inode)) return PTR_ERR(inode);

// inode has refcount=N (at least 1 from cache + 1 from vfs_namei)

vfs_ilock(inode);
// Use inode...
vfs_iunlock(inode);

vfs_iput(inode);  // Drop the reference from vfs_namei
// inode may still exist in cache if other references remain
```

### Superblock Reference Counting

#### Rules
1. Superblock has refcount for active users
2. Use `vfs_superblock_dup()` to add reference
3. Use `vfs_superblock_put()` to drop reference
4. Superblock cannot be freed while refcount > 0

#### Reference Sources
- **Mount tree attachment**: +1 while attached
- **Root inode**: +1 while root inode exists
- **Active inodes**: Each inode implicitly holds sb reference (via sb->valid)

### File Reference Counting

#### Rules
1. Each file descriptor holds a reference
2. Use `vfs_fdup()` to duplicate (increments refcount)
3. Use `vfs_fput()` to release (decrements refcount)
4. File freed when refcount reaches 0

#### RCU-Safe File Descriptor Close

When closing a file descriptor, concurrent readers may still be accessing
the file via `vfs_fdtable_get_file()`. To prevent use-after-free:

```c
// Step 1: Remove fd from table (under spinlock)
spin_lock(&fdtable->lock);
file = vfs_fdtable_dealloc_fd(fdtable, fd);
spin_unlock(&fdtable->lock);

// Step 2: Defer refcount decrement until RCU grace period
call_rcu(NULL, callback_that_calls_vfs_fput, file);
```

The RCU grace period ensures all `rcu_read_lock()` holders (from concurrent
`vfs_fdtable_get_file()` calls) have completed before the file is released.

#### Example (fork with fdtable cloning)
```c
// In parent process
struct vfs_file *file = vfs_fdtable_get_file(p->fdtable, 3);  // refcount incremented

// Fork (without CLONE_FILES - creates new fdtable)
if (fork() == 0) {
    // Child has cloned fdtable with duplicated file references
    // file->refcount includes child's reference
}

// Both parent and child can close independently
vfs_fput(file);  // Release our lookup reference
```

### File Descriptor Table Reference Counting

#### Rules
1. Each process holds a reference to its fdtable
2. `CLONE_FILES` flag shares fdtable (increments refcount)
3. Use `vfs_fdtable_clone()` during fork/clone
4. Use `vfs_fdtable_put()` on process exit

#### Example (clone with CLONE_FILES)
```c
// Thread creation with shared fdtable
clone(CLONE_FILES, ...);  // fdtable->ref_count++

// Both threads share the same fdtable
// Opening a file in one thread is visible to the other
```

### Orphan Inode Management

#### What is an Orphan?
- Inode with `n_links=0` but `ref_count>0`
- Happens when file is unlinked while open
- Must be tracked to prevent space leaks

#### Orphan List
- Maintained in `sb->orphan_list`
- Added when inode becomes orphan: `vfs_make_orphan()`
- Removed when inode is freed or re-linked

#### Unmount Behavior
- Normal unmount: Fails if orphans exist (filesystem busy)
- Lazy unmount: Allows orphans, cleans up when closed

---

## File Operations

### Opening Files

#### Sequence
```c
// System call
sys_vfs_open() {
    // 1. Resolve path
    inode = vfs_namei(path, len);
    
    // 2. Create file structure
    file = vfs_fileopen(inode, flags);
    
    // 3. Allocate file descriptor
    fd = __vfs_fdalloc(file);
    
    return fd;
}
```

#### File Type Dispatch
```c
vfs_fileopen(inode, flags) {
    switch (inode->mode & S_IFMT) {
    case S_IFREG:
    case S_IFDIR:
        file->ops = inode->ops->file_ops;
        break;
    case S_IFCHR:
        file->cdev = cdev_get(major, minor);
        break;
    case S_IFBLK:
        file->blkdev = blkdev_get(major, minor);
        break;
    // ... other types ...
    }
}
```

### Reading Files

#### Generic Read
```c
vfs_fileread(file, buf, n) {
    // Device files
    if (S_ISCHR(file->inode->mode))
        return cdev_read(file->cdev, buf, n);
    if (S_ISBLK(file->inode->mode))
        return blkdev_read(file->blkdev, buf, n);
    
    // Regular files
    vfs_ilock(file->inode);
    ret = file->ops->read(file, buf, n);
    vfs_iunlock(file->inode);
    
    return ret;
}
```

#### Locking for I/O
- File I/O acquires inode lock but NOT superblock lock
- This allows concurrent reads from different files
- Superblock lock only needed for metadata operations (create, unlink, etc.)

### Writing Files

#### Generic Write
```c
vfs_filewrite(file, buf, n) {
    // Check permissions
    if ((file->f_flags & O_WRONLY) == 0 && 
        (file->f_flags & O_RDWR) == 0)
        return -EBADF;
    
    // Device files
    if (S_ISCHR(...))
        return cdev_write(...);
    
    // Regular files
    vfs_ilock(file->inode);
    
    // Append mode
    if (file->f_flags & O_APPEND)
        file->offset = file->inode->size;
    
    ret = file->ops->write(file, buf, n);
    if (ret > 0) {
        file->offset += ret;
        file->inode->dirty = 1;
    }
    
    vfs_iunlock(file->inode);
    return ret;
}
```

### Closing Files

#### Sequence
```c
sys_vfs_close(fd) {
    // 1. Remove from fd table
    file = __vfs_fdfree(fd);
    
    // 2. Close file
    vfs_fileclose(file);
}

vfs_fileclose(file) {
    // Decrement refcount
    if (--file->ref_count > 0)
        return;
    
    // Last reference - cleanup
    
    // Special case: anonymous pipes BEFORE inode check!
    if (file->pipe && file->inode == NULL) {
        pipeclose(file->pipe, file->f_flags & O_WRONLY);
        __vfs_file_free(file);
        return;
    }
    
    // Release inode reference
    if (file->inode)
        vfs_iput(file->inode);
    
    // Free file structure
    __vfs_file_free(file);
}
```

**Important**: The anonymous pipe check must come BEFORE the inode NULL check, otherwise pipes leak (bug fixed in December 2024).

---

## Special Features

### 1. Symbolic Links

#### Creation
```c
inode = vfs_symlink(dir, mode, name, name_len, target, target_len);
```

#### Resolution
- During path resolution, when a symlink is encountered:
  ```c
  char linkbuf[VFS_PATH_MAX];
  ssize_t len = vfs_readlink(inode, linkbuf, sizeof(linkbuf));
  // Recursively resolve linkbuf
  ```
- Maximum depth: `VFS_SYMLOOP_MAX` (8)
- Returns `-ELOOP` if exceeded

### 2. Hard Links

#### Creation
```c
int vfs_link(old_dentry, new_dir, new_name, new_name_len);
```

#### Constraints
- Both inodes must be in same filesystem
- Cannot link directories (to prevent cycles)
- Cannot link across mount boundaries

### 3. Device Files

#### Character Devices
- Stored as `inode->cdev` (major:minor)
- Operations dispatched to character device driver
- Examples: console, null, zero, random

#### Block Devices
- Stored as `inode->bdev` (major:minor)
- Operations dispatched to block device driver
- Examples: disk partitions

### 4. Pipes

#### Anonymous Pipes
```c
vfs_pipealloc(&readfile, &writefile);
```
- Created via `pipe()` syscall
- Two file structures, one shared pipe buffer
- `inode == NULL` for both files
- Unidirectional data flow

#### Named Pipes (FIFOs)
```c
inode = vfs_mknod(dir, S_IFIFO | mode, 0, name, len);
```
- Has inode in filesystem
- Can be opened by unrelated processes

### 5. Sockets

#### VFS Integration
```c
file = vfs_sockalloc(raddr, lport, rport);
```
- Socket structure embedded in file
- VFS provides fd management
- Network layer handles actual communication

### 6. Directory Iteration

#### Iterator Structure
```c
struct vfs_dir_iter {
    uint64 index;      // Current position
    void *cookies;     // FS-specific state
};
```

#### Usage
```c
struct vfs_dir_iter iter = {0};
struct vfs_dentry dentry;

while (1) {
    int ret = vfs_dir_iter(dir, &iter, &dentry);
    if (ret < 0) break;
    if (dentry.name == NULL) break;  // End of directory
    
    // Process entry: dentry.name, dentry.ino
}
```

#### Special Entries
- Index 0: "." (present directory)
- Index 1: ".." (parent directory)
- Index 2+: Actual directory entries

### 7. Truncation

#### File Truncation
```c
truncate(file, new_size);
```
- Can grow or shrink file
- Growing: fills with zeros
- Shrinking: frees blocks beyond new size

#### Inode Truncation
```c
vfs_itruncate(inode, new_size);
```
- Direct inode truncation
- Used by `open()` with `O_TRUNC`

---

## Error Handling

### Error Code Standards

The VFS uses standard Linux error codes (POSIX):
- `-ENOENT`: No such file or directory
- `-EACCES`: Permission denied
- `-EEXIST`: File exists
- `-ENOTDIR`: Not a directory
- `-EISDIR`: Is a directory
- `-EINVAL`: Invalid argument
- `-ENOSPC`: No space left on device
- `-EMFILE`: Too many open files
- `-ENOMEM`: Out of memory
- `-EBUSY`: Device or resource busy
- `-ESHUTDOWN`: Cannot send after transport endpoint shutdown
- `-ELOOP`: Too many symbolic links

### Error Propagation

#### Pointer-based Errors
```c
struct vfs_inode *inode = vfs_namei(path, len);
if (IS_ERR(inode))
    return PTR_ERR(inode);  // Extract error code
if (inode == NULL)
    return -ENOENT;
```

#### Return Code Errors
```c
int ret = vfs_ilookup(dir, &dentry, name, len);
if (ret < 0)
    return ret;  // Propagate error
```

### Common Error Scenarios

1. **File Not Found**
   - `vfs_namei()` returns `-ENOENT`
   - `vfs_ilookup()` returns `-ENOENT`

2. **Permission Denied**
   - Syscalls check permissions
   - Return `-EACCES`

3. **Resource Exhaustion**
   - No space: `-ENOSPC`
   - Too many files: `-EMFILE`
   - Out of memory: `-ENOMEM`

4. **Invalid Operations**
   - Unmount busy filesystem: `-EBUSY`
   - Remove non-empty directory: `-ENOTEMPTY`
   - Cross-filesystem link: `-EXDEV`
   - Too many symlinks: `-ELOOP`

5. **Shutdown Errors**
   - Operations on unmounting fs: `-ESHUTDOWN`
   - Checked via `vfs_sb_check_usable()`

---

## Implementation Notes

### Memory Management

#### Slab Allocators
- `vfs_fs_type_cache`: File system types
- `vfs_superblock_cache`: Superblocks
- `__vfs_file_slab`: Open files

#### Cache Shrinking
```c
__vfs_shrink_caches();  // Shrink all VFS caches
__vfs_file_shrink_cache();  // Shrink file cache specifically
```

### Debugging Support

#### Assertions
```c
VFS_INODE_ASSERT_HOLDING(inode, "must hold inode lock");
VFS_SUPERBLOCK_ASSERT_WHOLDING(sb, "must hold sb write lock");
```

#### State Checking
```c
assert(holding_mutex(&inode->mutex), "inode lock required");
assert(vfs_superblock_wholding(sb), "sb write lock required");
```

### Performance Considerations

1. **Inode Caching**: Frequently accessed inodes stay in cache
2. **Parallel I/O**: Multiple files can be read/written concurrently
3. **Lock-free Reads**: File reads don't require superblock lock
4. **Slab Allocation**: Fast allocation for common structures

### Known Limitations

1. **No Dentry Cache**: Path resolution may be slower than Linux
2. **Global Mount Lock**: Serializes all mount/unmount operations
3. **No Readahead**: Sequential access not optimized
4. **Simple Hash**: Inode hash may have collisions under load

---

## Future Enhancements

1. **Dentry Cache**: Cache path → inode mappings
2. **Page Cache**: Unified page cache for all file data
3. **Async I/O**: Support for asynchronous file operations
4. **Read-Copy-Update (RCU)**: Lock-free path lookup
5. **Extended Attributes**: Support for xattrs
6. **Access Control Lists**: Fine-grained permissions
7. **Quotas**: Per-user disk usage limits
8. **Journaling**: Filesystem consistency guarantees

---

## Conclusion

This VFS implementation provides a robust, extensible foundation for supporting multiple file systems in xv6. Key strengths include:

- Clean abstraction layer separating VFS core from FS implementations
- Comprehensive reference counting preventing use-after-free bugs
- Hierarchical locking strategy preventing deadlocks
- Support for modern UNIX features (symlinks, devices, mounts)
- POSIX-compatible API for user-space programs

The design prioritizes correctness and simplicity over raw performance, making it suitable for educational purposes and small-scale production use.
