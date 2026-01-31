/*
 * VFS System Call Implementation
 *
 * This file implements the VFS-based system calls that replace the
 * original xv6 file system calls. All file operations now go through
 * the VFS layer.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "lock/spinlock.h"
#include "lock/mutex_types.h"
#include "lock/rcu.h"
#include "proc/proc.h"
#include "vfs_private.h"
#include "vfs/fs.h"
#include "vfs/file.h"
#include "vfs/fcntl.h"
#include "vfs/stat.h"
#include "vfs/xv6fs/ondisk.h"  // for DIRSIZ
#include <mm/vm.h>
#include "printf.h"
#include "dev/cdev.h"

// Forward declaration for syscall argument helpers
void argint(int n, int *ip);
void argaddr(int n, uint64 *ip);
int argstr(int n, char *buf, int max);

/******************************************************************************
 * Helper functions
 *
 * These helpers manage file descriptor operations with proper RCU and
 * refcount handling. The pattern for syscalls is:
 *
 *   1. Get file: __vfs_argfd(fd) returns file with +1 refcount
 *   2. Use file: perform operations
 *   3. Put file: vfs_fput(f) decrements refcount
 *
 * For fd allocation:
 *   1. Acquire fdtable->lock
 *   2. Call __vfs_fdalloc(file) which adds +1 refcount
 *   3. Release fdtable->lock
 *
 * For fd deallocation (close):
 *   1. Acquire fdtable->lock
 *   2. Call __vfs_fdfree(fd) to remove from table
 *   3. Release fdtable->lock
 *   4. Call __vfs_fput_call_rcu(file) to defer refcount decrement
 *      until RCU grace period completes (no concurrent readers)
 *
 ******************************************************************************/

/**
 * __vfs_fd_rcucb - RCU callback to release file reference
 * @data: Pointer to vfs_file to release
 *
 * Called after RCU grace period to safely decrement file refcount.
 */
static void __vfs_fd_rcucb(void *data) {
    struct vfs_file *fd = (struct vfs_file *)data;
    vfs_fput(fd);
}

/**
 * __vfs_fput_call_rcu - Defer file release until RCU grace period
 * @file: The file to release
 *
 * Schedules vfs_fput() to be called after all concurrent RCU readers
 * have finished. Used when closing a file descriptor to ensure no
 * concurrent vfs_fdtable_get_file() calls can still be accessing
 * the file.
 */
static void __vfs_fput_call_rcu(struct vfs_file *file) {
    call_rcu(NULL, __vfs_fd_rcucb, file);
}

/**
 * __vfs_argfd - Get file from fd with refcount increment
 * @fd: File descriptor from userspace
 *
 * Looks up the file for the given fd in current process's fdtable.
 * Returns file with incremented refcount - caller must call vfs_fput().
 *
 * Returns: File pointer, or NULL if fd is invalid
 */
static struct vfs_file *__vfs_argfd(int fd) {
    struct proc *p = myproc();
    if (fd < 0 || fd >= NOFILE) {
        return NULL;
    }
    return vfs_fdtable_get_file(p->fdtable, fd);
}

/**
 * __vfs_fdalloc - Allocate fd for file in current process
 * @file: The file to allocate an fd for
 *
 * LOCKING: Caller MUST hold myproc()->fdtable->lock
 *
 * Returns: Non-negative fd on success, negative errno on failure
 */
static int __vfs_fdalloc(struct vfs_file *file) {
    struct proc *p = myproc();
    return vfs_fdtable_alloc_fd(p->fdtable, file);
}

/**
 * __vfs_fdfree - Deallocate fd and return associated file
 * @fd: The file descriptor to free
 *
 * LOCKING: Caller MUST hold myproc()->fdtable->lock
 *
 * Returns: The file that was at fd, or NULL if fd was invalid
 */
static struct vfs_file *__vfs_fdfree(int fd) {
    struct proc *p = myproc();
    return vfs_fdtable_dealloc_fd(p->fdtable, fd);
}

/******************************************************************************
 * File Operations Syscalls
 ******************************************************************************/

uint64 sys_vfs_dup(void) {
    int fd;
    argint(0, &fd);
    
    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL) {
        return -EBADF;
    }
    
    spin_lock(&myproc()->fdtable->lock);
    int newfd = __vfs_fdalloc(f);
    spin_unlock(&myproc()->fdtable->lock);

    vfs_fput(f); // remove the reference from __vfs_argfd 
    return newfd;
}

uint64 sys_vfs_read(void) {
    int fd, n;
    uint64 p;
    
    argint(0, &fd);
    argaddr(1, &p);
    argint(2, &n);
    
    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL) {
        return -EBADF;
    }
    
    ssize_t ret = vfs_fileread(f, (void *)p, n, true);
    vfs_fput(f);
    return ret;
}

uint64 sys_vfs_write(void) {
    int fd, n;
    uint64 p;
    
    argint(0, &fd);
    argaddr(1, &p);
    argint(2, &n);
    
    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL) {
        return -EBADF;
    }
    
    ssize_t ret = vfs_filewrite(f, (const void *)p, n, true);
    vfs_fput(f);
    return ret;
}

uint64 sys_vfs_close(void) {
    int fd;
    argint(0, &fd);
    
    spin_lock(&myproc()->fdtable->lock);
    struct vfs_file *f = __vfs_fdfree(fd);
    if (f == NULL) {
        spin_unlock(&myproc()->fdtable->lock);
        return -EBADF;
    }
    spin_unlock(&myproc()->fdtable->lock);
    
    __vfs_fput_call_rcu(f);
    return 0;
}

uint64 sys_vfs_fstat(void) {
    int fd;
    uint64 st;
    
    argint(0, &fd);
    argaddr(1, &st);
    
    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL) {
        return -EBADF;
    }
    
    struct stat kst;
    int ret = vfs_filestat(f, &kst);
    if (ret != 0) {
        vfs_fput(f); // remove the reference from __vfs_argfd
        return ret;
    }
    
    if (vm_copyout(myproc()->vm, st, (char *)&kst, sizeof(kst)) < 0) {
        vfs_fput(f); // remove the reference from __vfs_argfd
        return -EFAULT;
    }
    
    vfs_fput(f); // remove the reference from __vfs_argfd
    return 0;
}

/******************************************************************************
 * File System Namespace Syscalls
 ******************************************************************************/

/*
 * FIX: Maximum symlink depth during path resolution.
 * POSIX systems typically limit symlink following to prevent infinite loops
 * from circular symlinks (e.g., a -> b -> a). Without this limit, such
 * loops would cause the kernel to hang or stack overflow.
 * Linux uses SYMLOOP_MAX=40, we use 8 for simplicity.
 */
#define VFS_SYMLOOP_MAX 8

uint64 sys_vfs_open(void) {
    char path[MAXPATH];
    char name[DIRSIZ];
    int omode;
    int n;
    
    argint(1, &omode);
    if ((n = argstr(0, path, MAXPATH)) < 0) {
        return -EFAULT;
    }
    
    struct vfs_inode *inode = NULL;
    int ret = 0;
    
    if (omode & O_CREAT) {
        // Create file if it doesn't exist
        struct vfs_inode *parent = vfs_nameiparent(path, n, name, DIRSIZ);
        if (IS_ERR(parent)) {
            return PTR_ERR(parent);
        }
        if (parent == NULL) {
            return -ENOENT;
        }
        
        size_t name_len = strlen(name);
        
        // Try to create
        inode = vfs_create(parent, 0644, name, name_len);
        vfs_iput(parent);
        
        if (IS_ERR(inode)) {
            if (PTR_ERR(inode) == -EEXIST && !(omode & O_EXCL)) {
                // File exists, try to open it
                inode = vfs_namei(path, n);
                // O_CREAT on an existing directory is not allowed
                if (!IS_ERR_OR_NULL(inode) && S_ISDIR(inode->mode)) {
                    vfs_iput(inode);
                    return -EISDIR;
                }
            } else {
                return PTR_ERR(inode);
            }
        }
    } else {
        // Open existing file - follow symlinks unless O_NOFOLLOW
        int symloop_count = 0;
        
        do {
            inode = vfs_namei(path, strlen(path));
            if (IS_ERR(inode)) {
                return PTR_ERR(inode);
            }
            if (inode == NULL) {
                return -ENOENT;
            }
            
            // Check if it's a symlink and we should follow it
            if (!S_ISLNK(inode->mode) || (omode & O_NOFOLLOW)) {
                break;  // Not a symlink or O_NOFOLLOW set
            }
            
            // Read the symlink target
            ssize_t link_len = vfs_readlink(inode, path, MAXPATH - 1);
            vfs_iput(inode);
            inode = NULL;
            
            if (link_len < 0) {
                return link_len;  // Error reading symlink
            }
            path[link_len] = '\0';
            
            symloop_count++;
        } while (symloop_count < VFS_SYMLOOP_MAX);
        
        if (symloop_count >= VFS_SYMLOOP_MAX) {
            return -ELOOP;  // Too many symlink levels
        }
    }
    
    if (IS_ERR(inode)) {
        return PTR_ERR(inode);
    }
    if (inode == NULL) {
        return -ENOENT;
    }
    
    // Check if trying to write to a directory
    if (S_ISDIR(inode->mode) && (omode & O_WRONLY || omode & O_RDWR)) {
        vfs_iput(inode);
        return -EISDIR;
    }
    
    // Check for O_TRUNC before releasing inode reference
    int should_truncate = (omode & O_TRUNC) && S_ISREG(inode->mode);
    
    struct vfs_file *f = vfs_fileopen(inode, omode);
    vfs_iput(inode);  // Release local inode reference (file holds its own ref)
    
    if (IS_ERR(f)) {
        return PTR_ERR(f);
    }
    
    // Handle O_TRUNC - truncate the file to zero length
    if (should_truncate) {
        ret = vfs_itruncate(vfs_inode_deref(&f->inode), 0);
        if (ret != 0) {
            vfs_fput(f);
            return ret;
        }
    }
    
    spin_lock(&myproc()->fdtable->lock);
    int fd = __vfs_fdalloc(f);
    spin_unlock(&myproc()->fdtable->lock);
    // When success, the refcount of f will be increased by fdtable, thus we do not put f here.
    // When failure, we need to put f anyway.
    vfs_fput(f);
    return fd;
}

uint64 sys_vfs_mkdir(void) {
    char path[MAXPATH];
    char name[DIRSIZ];
    int n;
    
    if ((n = argstr(0, path, MAXPATH)) < 0) {
        return -EFAULT;
    }
    
    struct vfs_inode *parent = vfs_nameiparent(path, n, name, DIRSIZ);
    if (IS_ERR(parent)) {
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        return -ENOENT;
    }
    
    size_t name_len = strlen(name);
    
    struct vfs_inode *dir = vfs_mkdir(parent, 0755, name, name_len);
    vfs_iput(parent);
    
    if (IS_ERR(dir)) {
        return PTR_ERR(dir);
    }
    
    vfs_iput(dir);
    return 0;
}

uint64 sys_vfs_mknod(void) {
    char path[MAXPATH];
    char name[DIRSIZ];
    int mode, major, minor;
    int n;
    
    if ((n = argstr(0, path, MAXPATH)) < 0) {
        return -EFAULT;
    }
    argint(1, &mode);
    argint(2, &major);
    argint(3, &minor);
    
    struct vfs_inode *parent = vfs_nameiparent(path, n, name, DIRSIZ);
    if (IS_ERR(parent)) {
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        return -ENOENT;
    }
    
    size_t name_len = strlen(name);
    
    dev_t dev = mkdev(major, minor);
    struct vfs_inode *node = vfs_mknod(parent, (mode_t)mode, dev, name, name_len);
    vfs_iput(parent);
    
    if (IS_ERR(node)) {
        return PTR_ERR(node);
    }
    
    vfs_iput(node);
    return 0;
}

uint64 sys_vfs_unlink(void) {
    char path[MAXPATH];
    char name[DIRSIZ];
    int n;
    
    if ((n = argstr(0, path, MAXPATH)) < 0) {
        return -EFAULT;
    }
    
    struct vfs_inode *parent = vfs_nameiparent(path, n, name, DIRSIZ);
    if (IS_ERR(parent)) {
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        return -ENOENT;
    }
    
    size_t name_len = strlen(name);
    
    // Cannot unlink "." or ".."
    if ((name_len == 1 && name[0] == '.') ||
        (name_len == 2 && name[0] == '.' && name[1] == '.')) {
        vfs_iput(parent);
        return -EINVAL;
    }
    
    int ret = vfs_unlink(parent, name, name_len);
    vfs_iput(parent);
    
    return ret;
}

uint64 sys_vfs_rmdir(void) {
    char path[MAXPATH];
    char name[DIRSIZ];
    int n;
    
    if ((n = argstr(0, path, MAXPATH)) < 0) {
        return -EFAULT;
    }
    
    struct vfs_inode *parent = vfs_nameiparent(path, n, name, DIRSIZ);
    if (IS_ERR(parent)) {
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        return -ENOENT;
    }
    
    size_t name_len = strlen(name);
    
    // Cannot rmdir "." or ".."
    if ((name_len == 1 && name[0] == '.') ||
        (name_len == 2 && name[0] == '.' && name[1] == '.')) {
        vfs_iput(parent);
        return -EINVAL;
    }
    
    int ret = vfs_rmdir(parent, name, name_len);
    vfs_iput(parent);
    
    return ret;
}

uint64 sys_vfs_link(void) {
    char old[MAXPATH], new[MAXPATH];
    char name[DIRSIZ];
    int n1, n2;
    
    if ((n1 = argstr(0, old, MAXPATH)) < 0 ||
        (n2 = argstr(1, new, MAXPATH)) < 0) {
        return -EFAULT;
    }
    
    // Get the source inode
    struct vfs_inode *src = vfs_namei(old, n1);
    if (IS_ERR(src)) {
        return PTR_ERR(src);
    }
    if (src == NULL) {
        return -ENOENT;
    }
    
    // Cannot link directories
    if (S_ISDIR(src->mode)) {
        vfs_iput(src);
        return -EPERM;
    }
    
    // Get parent directory of new path
    struct vfs_inode *parent = vfs_nameiparent(new, n2, name, DIRSIZ);
    if (IS_ERR(parent)) {
        vfs_iput(src);
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        vfs_iput(src);
        return -ENOENT;
    }
    
    size_t name_len = strlen(name);
    
    // Create a dentry for the source
    struct vfs_dentry old_dentry = {
        .sb = src->sb,
        .ino = src->ino,
        .name = NULL,
        .name_len = 0,
    };
    
    int ret = vfs_link(&old_dentry, parent, name, name_len);
    
    vfs_iput(src);
    vfs_iput(parent);
    
    return ret;
}

/**
 * vfs_make_absolute_path - Convert a relative path to absolute based on cwd
 * @relpath: the relative path to convert
 * @relpath_len: length of the relative path
 * @abspath: buffer to store the absolute path (must be MAXPATH size)
 *
 * If relpath is already absolute (starts with '/'), it is copied as-is.
 * Otherwise, the current working directory is prepended.
 *
 * Returns: length of the absolute path, or negative errno on error
 */
static int vfs_make_absolute_path(const char *relpath, int relpath_len, char *abspath) {
    if (relpath_len <= 0) {
        return -EINVAL;
    }
    
    // Already absolute - just copy
    if (relpath[0] == '/') {
        if (relpath_len >= MAXPATH) {
            return -ENAMETOOLONG;
        }
        memmove(abspath, relpath, relpath_len);
        abspath[relpath_len] = '\0';
        return relpath_len;
    }
    
    // Relative path - need to prepend cwd
    struct proc *p = myproc();
    vfs_struct_lock(p->fs);
    struct vfs_inode *cwd = vfs_inode_deref(&p->fs->cwd);
    struct vfs_inode *root = vfs_inode_deref(&p->fs->rooti);
    vfs_struct_unlock(p->fs);
    
    if (cwd == NULL) {
        return -ENOENT;
    }
    
    // Collect names from cwd to root
    char *names[MAXPATH / 2];
    int name_count = 0;
    
    struct vfs_inode *inode = cwd;
    while (inode != root) {
        if (inode->parent == inode) {
            // Cross mount boundary
            struct vfs_inode *mountpoint = inode->sb->mountpoint;
            if (mountpoint == NULL) {
                break;
            }
            if (mountpoint->name != NULL) {
                names[name_count++] = mountpoint->name;
            }
            inode = mountpoint->parent;
            if (inode == NULL || inode == mountpoint) {
                break;
            }
            continue;
        }
        
        if (inode->name != NULL) {
            names[name_count++] = inode->name;
        }
        inode = inode->parent;
        if (inode == NULL) {
            break;
        }
    }
    
    // Build absolute path: /cwd/relpath
    int pathlen = 0;
    abspath[pathlen++] = '/';
    for (int i = name_count - 1; i >= 0; i--) {
        int len = strlen(names[i]);
        if (pathlen + len + 1 >= MAXPATH) {
            return -ENAMETOOLONG;
        }
        memmove(abspath + pathlen, names[i], len);
        pathlen += len;
        abspath[pathlen++] = '/';
    }
    // Append relative path
    if (pathlen + relpath_len >= MAXPATH) {
        return -ENAMETOOLONG;
    }
    memmove(abspath + pathlen, relpath, relpath_len);
    pathlen += relpath_len;
    abspath[pathlen] = '\0';
    
    return pathlen;
}

uint64 sys_vfs_symlink(void) {
    char target[MAXPATH], linkpath[MAXPATH];
    char name[DIRSIZ];
    int n1, n2;
    
    if ((n1 = argstr(0, target, MAXPATH)) < 0 ||
        (n2 = argstr(1, linkpath, MAXPATH)) < 0) {
        return -EFAULT;
    }
    
    // Convert target to absolute path if it's relative
    char abs_target[MAXPATH];
    int abs_len = vfs_make_absolute_path(target, n1, abs_target);
    if (abs_len < 0) {
        return abs_len;
    }
    
    struct vfs_inode *parent = vfs_nameiparent(linkpath, n2, name, DIRSIZ);
    if (IS_ERR(parent)) {
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        return -ENOENT;
    }
    
    size_t name_len = strlen(name);
    
    struct vfs_inode *sym = vfs_symlink(parent, 0777, name, name_len, abs_target, abs_len);
    vfs_iput(parent);
    
    if (IS_ERR(sym)) {
        return PTR_ERR(sym);
    }
    
    vfs_iput(sym);
    return 0;
}

uint64 sys_vfs_chdir(void) {
    char path[MAXPATH];
    int n;
    
    if ((n = argstr(0, path, MAXPATH)) < 0) {
        return -EFAULT;
    }
    
    struct vfs_inode *inode = vfs_namei(path, n);
    if (IS_ERR(inode)) {
        return PTR_ERR(inode);
    }
    if (inode == NULL) {
        return -ENOENT;
    }
    
    if (!S_ISDIR(inode->mode)) {
        vfs_iput(inode);
        return -ENOTDIR;
    }
    
    // Update process cwd
    struct proc *p = myproc();
    vfs_struct_lock(p->fs);
    // Save old cwd, in order to release it out of the lock
    struct vfs_inode_ref old_cwd = p->fs->cwd;
    // Set new cwd
    vfs_inode_get_ref(inode, &p->fs->cwd);
    vfs_struct_unlock(p->fs);
    
    // Release old cwd
    vfs_inode_put_ref(&old_cwd);
    vfs_iput(inode);
    
    return 0;
}

/******************************************************************************
 * Getcwd Syscall
 ******************************************************************************/

/*
 * sys_getcwd - Get current working directory path
 *
 * Builds the path by walking from cwd up to root using parent pointers
 * and inode name fields. Directory inodes store their name when loaded.
 *
 * Args:
 *   arg0: buf - user buffer to store path
 *   arg1: size - buffer size
 *
 * Returns:
 *   Pointer to buf on success, or negative errno on failure.
 */
uint64 sys_getcwd(void) {
    uint64 buf_addr;
    int size;
    
    argaddr(0, &buf_addr);
    argint(1, &size);
    
    if (size <= 0) {
        return -EINVAL;
    }
    
    char path[MAXPATH];
    int pathlen = 0;
    
    struct proc *p = myproc();
    vfs_struct_lock(p->fs);
    struct vfs_inode *cwd = vfs_inode_deref(&p->fs->cwd);
    struct vfs_inode *root = vfs_inode_deref(&p->fs->rooti);
    vfs_struct_unlock(p->fs);
    
    if (cwd == NULL) {
        return -ENOENT;
    }
    
    // Build path from cwd to root by collecting names
    // We build it in reverse, then reverse the result
    char *names[MAXPATH / 2];  // Stack of name pointers
    int name_count = 0;
    
    struct vfs_inode *inode = cwd;
    while (inode != root) {
        // Check if we're at a local root (mount point)
        if (inode->parent == inode) {
            // Cross mount boundary: get the mountpoint from the superblock
            struct vfs_inode *mountpoint = inode->sb->mountpoint;
            if (mountpoint == NULL) {
                // We're at the global root
                break;
            }
            // Use the mountpoint's name and continue from the mountpoint
            if (mountpoint->name != NULL) {
                names[name_count++] = mountpoint->name;
            }
            inode = mountpoint->parent;
            if (inode == NULL || inode == mountpoint) {
                break;
            }
            continue;
        }
        
        if (inode->name != NULL) {
            names[name_count++] = inode->name;
        }
        inode = inode->parent;
        if (inode == NULL) {
            break;
        }
    }
    
    // Build path from names (in reverse order)
    path[pathlen++] = '/';
    for (int i = name_count - 1; i >= 0; i--) {
        int len = strlen(names[i]);
        if (pathlen + len + 1 >= MAXPATH) {
            return -ENAMETOOLONG;
        }
        memmove(path + pathlen, names[i], len);
        pathlen += len;
        if (i > 0) {
            path[pathlen++] = '/';
        }
    }
    path[pathlen] = '\0';
    
    if (pathlen + 1 > size) {
        return -ERANGE;
    }
    
    if (vm_copyout(p->vm, buf_addr, path, pathlen + 1) < 0) {
        return -EFAULT;
    }
    
    return buf_addr;
}

/******************************************************************************
 * Pipe Syscall
 ******************************************************************************/

uint64 sys_vfs_pipe(void) {
    uint64 fdarray;
    argaddr(0, &fdarray);
    
    struct vfs_file *rf = NULL, *wf = NULL;
    int ret = vfs_pipealloc(&rf, &wf);
    if (ret != 0) {
        return ret;
    }
    
    spin_lock(&myproc()->fdtable->lock);
    int fd0 = __vfs_fdalloc(rf);
    if (fd0 < 0) {
        spin_unlock(&myproc()->fdtable->lock);
        // Decrease the refcounts allocated by pipealloc
        vfs_fput(rf);
        vfs_fput(wf);
        return fd0;
    }
    
    int fd1 = __vfs_fdalloc(wf);
    if (fd1 < 0) {
        __vfs_fdfree(fd0);
        spin_unlock(&myproc()->fdtable->lock);
        // Decrease the refcounts allocated by pipealloc
        vfs_fput(rf);
        vfs_fput(wf);
        // Decrease the refcounts allocated by fdtable
        __vfs_fput_call_rcu(rf);
        return fd1;
    }
    spin_unlock(&myproc()->fdtable->lock);
    
    // vm_copyout may sleep (acquires rwlock), so must be outside spinlock
    struct proc *p = myproc();
    if (vm_copyout(p->vm, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
        vm_copyout(p->vm, fdarray + sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0) {
        // Re-acquire lock to deallocate fds
        spin_lock(&myproc()->fdtable->lock);
        __vfs_fdfree(fd0);
        __vfs_fdfree(fd1);
        spin_unlock(&myproc()->fdtable->lock);

        // Decrease the refcounts allocated by pipealloc
        vfs_fput(rf);
        vfs_fput(wf);
        // Decrease the refcounts allocated by fdtable
        __vfs_fput_call_rcu(rf);
        __vfs_fput_call_rcu(wf);
        return -EFAULT;
    }
    
    // Release the references from pipealloc - fdtable holds its own references now
    // (same pattern as sys_vfs_open which calls vfs_fput after __vfs_fdalloc)
    vfs_fput(rf);
    vfs_fput(wf);
    
    return 0;
}

/******************************************************************************
 * Socket Syscall
 ******************************************************************************/

uint64 sys_vfs_connect(void) {
    uint32 raddr, lport, rport;
    
    argint(0, (int *)&raddr);
    argint(1, (int *)&lport);
    argint(2, (int *)&rport);
    
    struct vfs_file *f = NULL;
    int ret = vfs_sockalloc(&f, raddr, (uint16)lport, (uint16)rport);
    if (ret != 0) {
        return ret;
    }
    
    spin_lock(&myproc()->fdtable->lock);
    int fd = __vfs_fdalloc(f);
    spin_unlock(&myproc()->fdtable->lock);

    // When success, the refcount of f will be increased by fdtable, thus we do not put f here.
    // When failure, we need to put f anyway.
    vfs_fput(f);
    return fd;
}

/******************************************************************************
 * Directory Operations - getdents
 ******************************************************************************/

// Linux-compatible dirent structure
struct linux_dirent64 {
    uint64 d_ino;      // Inode number
    int64 d_off;       // Offset to next structure
    uint16 d_reclen;   // Size of this dirent
    uint8 d_type;      // File type
    char d_name[];     // Filename (null-terminated)
};

// File type constants
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK      10
#define DT_SOCK     12

static uint8 __mode_to_dtype(mode_t mode) {
    if (S_ISREG(mode)) return DT_REG;
    if (S_ISDIR(mode)) return DT_DIR;
    if (S_ISCHR(mode)) return DT_CHR;
    if (S_ISBLK(mode)) return DT_BLK;
    if (S_ISFIFO(mode)) return DT_FIFO;
    if (S_ISLNK(mode)) return DT_LNK;
    if (S_ISSOCK(mode)) return DT_SOCK;
    return DT_UNKNOWN;
}

uint64 sys_getdents(void) {
    int fd;
    uint64 dirp;
    int count;
    
    argint(0, &fd);
    argaddr(1, &dirp);
    argint(2, &count);
    
    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL) {
        return -EBADF;
    }
    
    struct vfs_inode *inode = vfs_inode_deref(&f->inode);
    if (inode == NULL || !S_ISDIR(inode->mode)) {
        vfs_fput(f); // remove the reference from __vfs_argfd
        return -ENOTDIR;
    }
    
    // Allocate kernel buffer
    char *kbuf = kmm_alloc(count);
    if (kbuf == NULL) {
        vfs_fput(f); // remove the reference from __vfs_argfd
        return -ENOMEM;
    }
    
    int bytes_written = 0;
    struct vfs_dentry dentry = {0};
    int ret;
    
    while (bytes_written < count) {
        ret = vfs_dir_iter(inode, &f->dir_iter, &dentry);
        if (ret != 0) {
            kmm_free(kbuf);
            vfs_fput(f); // remove the reference from __vfs_argfd
            return ret;
        }
        
        if (dentry.name == NULL) {
            // End of directory
            break;
        }
        
        // Calculate record length (must be 8-byte aligned)
        size_t name_len = dentry.name_len;
        size_t reclen = sizeof(struct linux_dirent64) + name_len + 1;
        reclen = (reclen + 7) & ~7; // Align to 8 bytes
        
        if (bytes_written + (int)reclen > count) {
            // Not enough space, put back for next call
            vfs_release_dentry(&dentry);
            break;
        }
        
        // Get inode info for d_type
        struct vfs_inode *child = vfs_get_dentry_inode(&dentry);
        uint8 d_type = DT_UNKNOWN;
        if (!IS_ERR_OR_NULL(child)) {
            d_type = __mode_to_dtype(child->mode);
            vfs_iput(child);
        }
        
        // Fill dirent
        struct linux_dirent64 *de = (struct linux_dirent64 *)(kbuf + bytes_written);
        de->d_ino = dentry.ino;
        de->d_off = f->dir_iter.index;
        de->d_reclen = reclen;
        de->d_type = d_type;
        memmove(de->d_name, dentry.name, name_len);
        de->d_name[name_len] = '\0';
        
        bytes_written += reclen;
        vfs_release_dentry(&dentry);
        memset(&dentry, 0, sizeof(dentry));
    }
    
    // Copy to user space
    if (bytes_written > 0) {
        if (vm_copyout(myproc()->vm, dirp, kbuf, bytes_written) < 0) {
            kmm_free(kbuf);
            vfs_fput(f); // remove the reference from __vfs_argfd
            return -EFAULT;
        }
    }
    
    kmm_free(kbuf);
    vfs_fput(f); // remove the reference from __vfs_argfd
    return bytes_written;
}

/******************************************************************************
 * chroot - Change root directory
 ******************************************************************************/

uint64 sys_chroot(void) {
    char path[MAXPATH];
    int n;
    
    if ((n = argstr(0, path, MAXPATH)) < 0) {
        return -EFAULT;
    }
    
    struct vfs_inode *new_root = vfs_namei(path, n);
    if (IS_ERR(new_root)) {
        return PTR_ERR(new_root);
    }
    if (new_root == NULL) {
        return -ENOENT;
    }
    
    if (!S_ISDIR(new_root->mode)) {
        vfs_iput(new_root);
        return -ENOTDIR;
    }
    
    // Use the VFS helper functions
    int ret = vfs_chroot(new_root);
    if (ret < 0) {
        vfs_iput(new_root);
        return ret;
    }
    
    ret = vfs_chdir(new_root);
    vfs_iput(new_root);
    
    return ret;
}

/******************************************************************************
 * mount - Mount a filesystem
 ******************************************************************************/

/**
 * vfs_mount_path - Mount a filesystem at the given path
 * @fstype: filesystem type name (e.g., "tmpfs", "xv6fs")
 * @target: target mount point path
 * @target_len: length of target path
 * @source: source device path (may be NULL for pseudo filesystems)
 * @source_len: length of source path
 *
 * This is the kernel-internal mount function that handles path resolution,
 * device lookup, locking, and calling vfs_mount(). Can be called from both
 * kernel initialization code and sys_mount.
 *
 * Returns 0 on success, negative errno on failure.
 */
int vfs_mount_path(const char *fstype, const char *target, int target_len,
                   const char *source, int source_len) {
    // Look up target directory
    struct vfs_inode *target_dir = vfs_namei(target, target_len);
    if (IS_ERR(target_dir)) {
        return PTR_ERR(target_dir);
    }
    if (target_dir == NULL) {
        return -ENOENT;
    }
    
    if (!S_ISDIR(target_dir->mode)) {
        vfs_iput(target_dir);
        return -ENOTDIR;
    }
    
    // Parse source device (for block-device-based filesystems)
    struct vfs_inode *source_inode = NULL;
    if (source != NULL && source_len > 0) {
        struct vfs_inode *source_dev = vfs_namei(source, source_len);
        if (!IS_ERR_OR_NULL(source_dev)) {
            if (S_ISBLK(source_dev->mode)) {
                source_inode = source_dev;
            } else {
                vfs_iput(source_dev);
            }
        }
    }
    
    // Acquire required locks for vfs_mount:
    // 1. Mount mutex
    // 2. Superblock write lock
    // 3. Inode lock on mountpoint
    vfs_mount_lock();
    vfs_superblock_wlock(target_dir->sb);
    vfs_ilock(target_dir);
    
    // Mount the filesystem
    int ret = vfs_mount(fstype, target_dir, source_inode, 0, NULL);
    
    // Release locks in reverse order
    vfs_iunlock(target_dir);
    vfs_superblock_unlock(target_dir->sb);
    vfs_mount_unlock();
    
    if (source_inode) {
        vfs_iput(source_inode);
    }
    vfs_iput(target_dir);
    
    return ret;
}

uint64 sys_mount(void) {
    char source[MAXPATH];
    char target[MAXPATH];
    char fstype[32];
    int n1, n2;
    
    if ((n1 = argstr(0, source, MAXPATH)) < 0 ||
        (n2 = argstr(1, target, MAXPATH)) < 0 ||
        argstr(2, fstype, 32) < 0) {
        return -EFAULT;
    }
    
    return vfs_mount_path(fstype, target, n2, source, n1);
}

/******************************************************************************
 * umount - Unmount a filesystem
 ******************************************************************************/

/**
 * vfs_umount_path - Unmount a filesystem at the given path
 * @target: target mount point path
 * @target_len: length of target path
 *
 * This is the kernel-internal unmount function that handles path resolution,
 * locking, and calling vfs_unmount(). Can be called from both kernel code
 * and sys_umount.
 *
 * Returns 0 on success, negative errno on failure.
 */
int vfs_umount_path(const char *target, int target_len) {
    // Look up target directory - vfs_namei follows mounts, so we get the
    // mounted filesystem's root inode, not the mountpoint directory itself
    struct vfs_inode *mounted_root = vfs_namei(target, target_len);
    if (IS_ERR(mounted_root)) {
        return PTR_ERR(mounted_root);
    }
    if (mounted_root == NULL) {
        return -ENOENT;
    }
    
    // Check if this is a mounted filesystem root (parent == self for local root)
    if (!vfs_inode_is_local_root(mounted_root)) {
        vfs_iput(mounted_root);
        return -EINVAL;  // Not a mounted filesystem root
    }
    
    // Get the mountpoint from the superblock
    struct vfs_superblock *child_sb = mounted_root->sb;
    if (child_sb == NULL || child_sb->mountpoint == NULL) {
        vfs_iput(mounted_root);
        return -EINVAL;  // Not mounted or no mountpoint
    }
    
    struct vfs_inode *target_dir = child_sb->mountpoint;
    if (!target_dir->mount) {
        vfs_iput(mounted_root);
        return -EINVAL;  // Mountpoint not marked as mount
    }
    
    // Acquire required locks for vfs_unmount:
    // 1. Mount mutex
    // 2. Parent superblock write lock
    // 3. Child superblock write lock
    // 4. Mountpoint inode lock
    // 5. Mounted root inode lock
    vfs_mount_lock();
    vfs_superblock_wlock(target_dir->sb);
    vfs_superblock_wlock(child_sb);
    vfs_ilock(target_dir);
    vfs_ilock(mounted_root);
    
    int ret = vfs_unmount(target_dir);
    
    if (ret != 0) {
        // On failure, release locks in reverse order
        // (vfs_unmount did not free anything)
        vfs_iunlock(mounted_root);
        vfs_iunlock(target_dir);
        vfs_superblock_unlock(child_sb);
        vfs_superblock_unlock(target_dir->sb);
        vfs_mount_unlock();
        vfs_iput(mounted_root);
        return ret;
    }
    
    // On success, vfs_unmount has already:
    // - Unlocked and freed mounted_root
    // - Unlocked and freed child_sb
    // We just need to release what's left
    vfs_iunlock(target_dir);
    vfs_superblock_unlock(target_dir->sb);
    vfs_mount_unlock();
    
    return 0;
}

uint64 sys_umount(void) {
    char target[MAXPATH];
    int n;
    
    if ((n = argstr(0, target, MAXPATH)) < 0) {
        return -EFAULT;
    }
    
    return vfs_umount_path(target, n);
}
