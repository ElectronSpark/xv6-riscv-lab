# Doxygen Documentation Guide for xv6-RISCV

This guide explains how to write Doxygen comments for the xv6-RISCV codebase.

## Quick Start

### Generating Documentation

```bash
# Method 1: Using Doxygen directly
doxygen Doxyfile

# Method 2: Using CMake
cd build
cmake .. -DBUILD_DOCUMENTATION=ON
make doc

# View the documentation
firefox docs/html/index.html
```

## Comment Styles

Doxygen supports multiple comment styles. For C code, we recommend:

### File Documentation

At the top of each file:

```c
/**
 * @file vfs_syscall.c
 * @brief VFS System Call Implementation
 * 
 * This file implements the VFS-based system calls that replace the
 * original xv6 file system calls. All file operations now go through
 * the VFS layer.
 * 
 * @author Your Name
 * @date December 2025
 */
```

### Function Documentation

Before each function:

```c
/**
 * @brief Open a file with the specified flags
 * 
 * Opens a file and creates a file descriptor for it. Handles symbolic
 * link resolution and mount point traversal.
 * 
 * @param path Path to the file to open
 * @param path_len Length of the path string
 * @param flags Open flags (O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, etc.)
 * @param mode Permission mode if creating a new file
 * @return File descriptor number on success, negative error code on failure
 * 
 * @note The caller must hold no locks when calling this function
 * @warning This function may sleep while reading from disk
 * 
 * @see vfs_fileopen()
 * @see vfs_namei()
 */
int sys_vfs_open(const char *path, size_t path_len, int flags, mode_t mode);
```

### Structure Documentation

Before structures:

```c
/**
 * @brief Virtual File System inode structure
 * 
 * Represents a file, directory, or special file in the VFS layer.
 * Each inode is cached in memory and backed by storage (for persistent
 * file systems) or exists only in memory (for tmpfs).
 * 
 * @note Inodes are reference counted. Use vfs_idup() and vfs_iput()
 *       to manage references.
 */
struct vfs_inode {
    uint64 ino;                    /**< Inode number */
    mode_t mode;                   /**< Permissions and file type (S_IFREG, S_IFDIR, etc.) */
    loff_t size;                   /**< File size in bytes */
    
    struct vfs_superblock *sb;     /**< Owning superblock */
    struct vfs_inode_ops *ops;     /**< Inode operations */
    
    struct mutex mutex;            /**< Protects inode fields */
    int ref_count;                 /**< Reference count */
    
    uint8 valid:1;                 /**< Inode is valid */
    uint8 dirty:1;                 /**< Needs writeback */
};
```

### Scheduler Structure Documentation

For scheduler-related structures:

```c
/**
 * @brief Scheduling entity for process scheduling
 * 
 * Contains scheduling-related state separate from the process structure.
 * Modeled after Linux kernel's struct sched_entity. Each process has
 * a pointer to its scheduling entity (p->sched_entity).
 * 
 * @note The pi_lock, on_rq, on_cpu, and context fields were moved here
 *       from struct proc to separate scheduling concerns.
 * 
 * @see struct proc
 * @see struct rq
 * @see struct sched_class
 */
struct sched_entity {
    struct rq *rq;                 /**< Current run queue */
    int priority;                  /**< Scheduling priority (major + minor) */
    struct proc *proc;             /**< Back pointer to owning process */
    struct sched_class *sched_class; /**< Scheduling class (FIFO, IDLE, etc.) */
    spinlock_t pi_lock;            /**< Priority inheritance lock for wakeup */
    int on_rq;                     /**< 1 if on a ready queue */
    int on_cpu;                    /**< 1 if currently running on a CPU */
    int cpu_id;                    /**< CPU this entity is running on */
    cpumask_t affinity_mask;       /**< CPU affinity bitmask */
    struct context context;        /**< Saved context for swtch() */
};

/**
 * @brief Per-CPU run queue structure
 * 
 * Each CPU has its own run queue protected by a per-CPU lock.
 * The run queue delegates task management to its scheduling class.
 * 
 * @note Use rq_lock(cpu_id) and rq_unlock(cpu_id) for synchronization.
 * 
 * @see rq_enqueue_task()
 * @see rq_pick_next_task()
 */
struct rq {
    struct sched_class *sched_class; /**< Scheduling class for this queue */
    int class_id;                    /**< Scheduling class identifier */
    int task_count;                  /**< Number of tasks in queue */
    int cpu_id;                      /**< Owning CPU identifier */
};
```

### Macro Documentation

```c
/**
 * @def VFS_PATH_MAX
 * @brief Maximum path length supported by the VFS
 */
#define VFS_PATH_MAX 65535

/**
 * @def S_ISDIR
 * @brief Test if mode represents a directory
 * @param m Mode bits from stat structure
 * @return Non-zero if directory, zero otherwise
 */
#define S_ISDIR(m) (((m)&S_IFMT) == S_IFDIR)
```

### Enum Documentation

For file type checking, use the standard POSIX macros with mode_t:

```c
/**
 * @brief Check if inode is a directory
 * @param inode Pointer to inode structure
 * @return Non-zero if directory, zero otherwise
 */
static inline int is_directory(struct vfs_inode *inode) {
    return S_ISDIR(inode->mode);
}

/**
 * @brief Check if inode is a regular file
 * @param inode Pointer to inode structure  
 * @return Non-zero if regular file, zero otherwise
 */
static inline int is_regular_file(struct vfs_inode *inode) {
    return S_ISREG(inode->mode);
}
```

**Available file type macros:**
- `S_ISREG(mode)`: Regular file
- `S_ISDIR(mode)`: Directory
- `S_ISLNK(mode)`: Symbolic link
- `S_ISCHR(mode)`: Character device
- `S_ISBLK(mode)`: Block device
- `S_ISSOCK(mode)`: Socket
- `S_ISFIFO(mode)`: Named pipe (FIFO)

### Inline Comments

For member variables:

```c
struct vfs_file {
    struct vfs_inode *inode;       /**< Associated inode */
    struct vfs_file_ops *ops;      /**< File operations */
    int ref_count;                 /**< Reference count */
    loff_t offset;                 /**< Current file position */
    int f_flags;                   /**< Open flags (O_RDONLY, etc.) */
};
```

### Callback Structure Documentation

For structures with function pointers (like scheduling classes):

```c
/**
 * @brief Scheduling class operations
 * 
 * Defines the interface for pluggable scheduling policies. Each scheduling
 * class implements these callbacks to manage tasks on run queues.
 * Modeled after Linux kernel's struct sched_class.
 * 
 * @note At minimum, pick_next_task must be implemented.
 * 
 * Task Switch Flow:
 * 1. pick_next_task() - Select next task (keep in queue)
 * 2. set_next_task() - Remove from queue, set as current
 * 3. Context switch occurs
 * 4. put_prev_task() - Insert prev back to queue
 * 
 * @see struct rq
 * @see struct sched_entity
 */
struct sched_class {
    /**
     * @brief Add task to run queue
     * @param rq Run queue to add to
     * @param se Scheduling entity to enqueue
     */
    void (*enqueue_task)(struct rq *rq, struct sched_entity *se);
    
    /**
     * @brief Remove task from run queue
     * @param rq Run queue to remove from
     * @param se Scheduling entity to dequeue
     */
    void (*dequeue_task)(struct rq *rq, struct sched_entity *se);
    
    /**
     * @brief Select next task to run
     * @param rq Run queue to pick from
     * @return Next scheduling entity, or NULL if queue empty
     * @note Task remains in queue until set_next_task() is called
     */
    struct sched_entity* (*pick_next_task)(struct rq *rq);
    
    /**
     * @brief Put previous task back on queue after context switch
     * @param rq Run queue
     * @param se Previous task's scheduling entity
     */
    void (*put_prev_task)(struct rq *rq, struct sched_entity *se);
};
```

## Special Commands

### Grouping Related Functions

```c
/**
 * @defgroup vfs_core VFS Core
 * @brief Core VFS functionality
 * @{
 */

/** Function 1 */
void vfs_init(void);

/** Function 2 */
struct vfs_inode *vfs_namei(const char *path, size_t len);

/** @} */ // end of vfs_core group
```

### Scheduler Module Grouping

```c
/**
 * @defgroup scheduler Process Scheduler
 * @brief Linux-style process scheduling infrastructure
 * @{
 */

/**
 * @defgroup scheduler_rq Run Queue Management
 * @ingroup scheduler
 * @brief Per-CPU run queue operations
 * @{
 */

void rq_enqueue_task(struct rq *rq, struct sched_entity *se);
struct sched_entity *rq_pick_next_task(struct rq *rq);

/** @} */ // end of scheduler_rq

/**
 * @defgroup scheduler_class Scheduling Classes
 * @ingroup scheduler
 * @brief Pluggable scheduling policies (FIFO, IDLE, etc.)
 * @{
 */

void sched_class_register(int id, struct sched_class *cls);

/** @} */ // end of scheduler_class
/** @} */ // end of scheduler
```

### Cross-References

```c
/**
 * @brief Get inode by path
 * 
 * This is a wrapper around vfs_namei().
 * 
 * @see vfs_namei()
 * @see vfs_nameiparent()
 */
```

### Parameters and Return Values

```c
/**
 * @param[in] dir Directory to search in
 * @param[out] dentry Result dentry structure
 * @param[in,out] iter Directory iterator state
 * @retval 0 Success
 * @retval -ENOENT No such file or directory
 * @retval -EINVAL Invalid argument
 */
```

### Preconditions and Postconditions

```c
/**
 * @pre Caller must hold inode->mutex
 * @post Inode is marked dirty if modified
 * @invariant ref_count > 0 while function executes
 */
```

### Examples

```c
/**
 * @brief Example function with usage example
 * 
 * @code
 * struct vfs_inode *inode = vfs_namei("/etc/passwd", 11);
 * if (!IS_ERR_OR_NULL(inode)) {
 *     vfs_ilock(inode);
 *     // Use inode...
 *     vfs_iunlock(inode);
 *     vfs_iput(inode);
 * }
 * @endcode
 */
```

### TODO and Bug Tracking

```c
/**
 * @todo Implement caching for this lookup
 * @bug Race condition when multiple threads access simultaneously
 * @deprecated Use vfs_new_function() instead
 */
```

## Documentation Best Practices

### 1. Document the Interface, Not Implementation

Focus on what the function does, not how:

**Good:**
```c
/**
 * @brief Allocate a new inode in the file system
 * @return Pointer to new inode, or ERR_PTR on failure
 */
```

**Bad:**
```c
/**
 * @brief Calls malloc, checks for null, sets fields to zero
 */
```

### 2. Be Specific About Errors

```c
/**
 * @retval 0 Success
 * @retval -ENOMEM Out of memory
 * @retval -ENOSPC No space left on device
 * @retval -EIO I/O error
 */
```

### 3. Document Locking Requirements

```c
/**
 * @note Caller must hold sb->lock in write mode
 * @warning Function may sleep - do not call with spinlocks held
 */
```

### 4. Document Ownership and Reference Counting

```c
/**
 * @brief Increment inode reference count
 * @note Caller must already hold a reference to inode
 * @post inode->ref_count increased by 1
 */
```

### 5. Link Related Functions

```c
/**
 * @brief Lock an inode
 * @see vfs_iunlock() - Release the lock
 * @see vfs_idup() - Increment reference count
 * @see vfs_iput() - Decrement reference count
 */
```

## Modules Organization

Organize code into logical modules:

```c
/**
 * @defgroup vfs Virtual File System
 * @brief Virtual File System implementation
 * @{
 */

/**
 * @defgroup vfs_inode Inode Operations
 * @ingroup vfs
 * @brief Inode management and operations
 * @{
 */

// Inode functions here...

/** @} */ // end of vfs_inode

/**
 * @defgroup vfs_mount Mount Management  
 * @ingroup vfs
 * @brief File system mounting and unmounting
 * @{
 */

// Mount functions here...

/** @} */ // end of vfs_mount
/** @} */ // end of vfs
```

### Process and Scheduler Modules

```c
/**
 * @defgroup proc Process Management
 * @brief Process lifecycle and scheduling
 * @{
 */

/**
 * @defgroup proc_sched Scheduler
 * @ingroup proc
 * @brief Linux-style scheduler infrastructure
 * @{
 */

/**
 * @defgroup proc_sched_rq Run Queues
 * @ingroup proc_sched
 * @brief Per-CPU run queue management
 */

/**
 * @defgroup proc_sched_class Scheduling Classes
 * @ingroup proc_sched
 * @brief Pluggable scheduling policies
 */

/** @} */ // end of proc_sched

/**
 * @defgroup proc_rcu RCU Synchronization
 * @ingroup proc
 * @brief Read-Copy-Update for lock-free reads
 * @{
 */

// RCU functions: rcu_read_lock(), call_rcu(), synchronize_rcu()

/** @} */ // end of proc_rcu
/** @} */ // end of proc
```

## Markdown in Comments

Doxygen supports Markdown:

```c
/**
 * @brief Process a file operation
 * 
 * The function follows these steps:
 * 1. Validate parameters
 * 2. Acquire necessary locks
 * 3. Perform operation
 * 4. Release locks
 * 
 * **Important**: This function may block.
 * 
 * Error codes:
 * - `-EINVAL`: Invalid argument
 * - `-EACCES`: Permission denied
 * - `-EIO`: I/O error
 * 
 * @note See `kernel/vfs/VFS_DESIGN.md` for detailed design
 */
```

## Configuration

The Doxyfile is configured to:
- Extract all documentation (even undocumented items)
- Generate call graphs and caller graphs
- Generate include dependency diagrams
- Enable source browsing
- Create a searchable index
- Use the treeview navigation

## Tips

1. **Start Small**: Begin by documenting public APIs and headers
2. **Be Consistent**: Use the same style throughout the codebase
3. **Update as You Go**: Update documentation when changing code
4. **Review Generated Docs**: Check that formatting looks correct
5. **Use Groups**: Organize related functions into modules

## Example: Well-Documented Function

```c
/**
 * @brief Look up a directory entry in a directory inode
 * 
 * Searches for a directory entry with the given name in the specified
 * directory. Handles special entries "." and ".." at the VFS level.
 * Delegates actual lookup to the file system driver.
 * 
 * @param[in] dir Directory inode to search in (must be locked)
 * @param[out] dentry Result dentry structure (filled on success)
 * @param[in] name Name to look up (not null-terminated)
 * @param[in] name_len Length of name string
 * 
 * @return 0 on success, negative error code on failure
 * @retval 0 Entry found, dentry filled in
 * @retval -ENOENT Entry not found
 * @retval -ENOTDIR dir is not a directory
 * @retval -EINVAL Invalid parameters
 * 
 * @pre dir->mutex must be held by caller
 * @pre dir->valid must be true
 * @post On success, dentry contains valid entry data
 * 
 * @note For "." and "..", VFS handles directly without calling driver
 * @note dentry->inode may be NULL; use vfs_get_dentry_inode() to load
 * 
 * @warning This function may sleep while reading from disk
 * 
 * @see vfs_get_dentry_inode() - Load inode from dentry
 * @see vfs_release_dentry() - Release dentry resources
 * 
 * @code
 * struct vfs_dentry dentry;
 * vfs_ilock(dir);
 * int ret = vfs_ilookup(dir, &dentry, "file.txt", 8);
 * if (ret == 0) {
 *     struct vfs_inode *inode = vfs_get_dentry_inode(&dentry);
 *     // Use inode...
 *     vfs_iput(inode);
 *     vfs_release_dentry(&dentry);
 * }
 * vfs_iunlock(dir);
 * @endcode
 */
int vfs_ilookup(struct vfs_inode *dir, struct vfs_dentry *dentry, 
                const char *name, size_t name_len);
```

## Further Reading

- [Doxygen Manual](https://www.doxygen.nl/manual/)
- [Doxygen Special Commands](https://www.doxygen.nl/manual/commands.html)
- [Doxygen Markdown Support](https://www.doxygen.nl/manual/markdown.html)

## Contributing

When contributing code, please:
1. Document all public APIs
2. Use consistent Doxygen style
3. Generate and review documentation
4. Update this guide if needed
