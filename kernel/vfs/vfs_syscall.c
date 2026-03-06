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
#include "proc/thread.h"
#include "proc/sched.h"
#include "proc/workqueue.h"
#include "vfs_private.h"
#include "vfs/fs.h"
#include "vfs/file.h"
#include "vfs/pipe.h"
#include "vfs/fcntl.h"
#include "vfs/stat.h"
#include "vfs/xv6fs/ondisk.h" // for DIRSIZ
#include "proc/cred.h"
#include <mm/vm.h>
#include "printf.h"
#include "dev/cdev.h"
#include "accounting.h"
#include "dev/blkdev.h"
#include "dev/gendisk.h"
#include "dev/loop.h"
#include "dev/gpt.h"
#include "vfs/poll.h"
#include "tty/termios.h"
#include "timer/timer.h"
#include "signal.h"
#include "kqueue.h"
#include "kqueue_types.h"

// Forward declaration for syscall argument helpers
void argint(int n, int *ip);
void argint64(int n, int64 *ip);
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
 * __vfs_fput_work_func - Workqueue callback to release file reference
 * @work: Work struct containing file to release
 *
 * Called by workqueue worker to perform vfs_fput(). This runs in a normal
 * kthread context where blocking on locks is allowed (unlike RCU callbacks).
 */
static void __vfs_fput_work_func(struct work_struct *work) {
    struct vfs_file *fd = (struct vfs_file *)work->data;
    vfs_fput(fd);
    free_work_struct(work);
}

/**
 * __vfs_fd_rcucb - RCU callback to queue deferred file release
 * @data: Pointer to vfs_file to release
 *
 * Called after RCU grace period. Instead of calling vfs_fput() directly
 * (which can block on superblock wlock/inode mutex and cause RCU callback
 * deadlocks), we queue the work to a workqueue. This allows the RCU callback
 * to complete immediately and unblocks RCU grace period completion.
 */
static void __vfs_fd_rcucb(void *data) {
    struct vfs_file *fd = (struct vfs_file *)data;
    struct workqueue *wq = vfs_get_deferred_iput_wq();

    // If workqueue not available (early init or shutdown), fall back to direct
    // call
    if (wq == NULL) {
        vfs_fput(fd);
        return;
    }

    struct work_struct *work =
        create_work_struct(__vfs_fput_work_func, (uint64)fd);
    if (work == NULL) {
        // Allocation failed, fall back to direct call (risky but better than
        // leak)
        printf("__vfs_fd_rcucb: failed to allocate work_struct, falling back "
               "to direct vfs_fput\n");
        vfs_fput(fd);
        return;
    }

    queue_work(wq, work);
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
    struct thread *p = current;
    if (fd < 0 || fd >= NOFILE) {
        return NULL;
    }
    return vfs_fdtable_get_file(p->fdtable, fd);
}

/**
 * __vfs_fdalloc - Allocate fd for file in current process
 * @file: The file to allocate an fd for
 *
 * LOCKING: Caller MUST hold current->fdtable->lock
 *
 * Returns: Non-negative fd on success, negative errno on failure
 */
static int __vfs_fdalloc(struct vfs_file *file) {
    struct thread *p = current;
    return vfs_fdtable_alloc_fd(p->fdtable, file);
}

/**
 * __vfs_fdfree - Deallocate fd and return associated file
 * @fd: The file descriptor to free
 *
 * LOCKING: Caller MUST hold current->fdtable->lock
 *
 * Returns: The file that was at fd, or NULL if fd was invalid
 */
static struct vfs_file *__vfs_fdfree(int fd) {
    struct thread *p = current;
    return vfs_fdtable_dealloc_fd(p->fdtable, fd);
}

/**
 * __vfs_close_fd - Close a file descriptor internally (for kernel use)
 * @fd: The file descriptor to close
 *
 * Equivalent to sys_vfs_close but callable from within the kernel without
 * going through the syscall argument layer.
 */
static void __attribute__((unused)) __vfs_close_fd(int fd) {
    spin_lock(&current->fdtable->lock);
    struct vfs_file *f = __vfs_fdfree(fd);
    spin_unlock(&current->fdtable->lock);
    if (f != NULL)
        __vfs_fput_call_rcu(f);
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

    spin_lock(&current->fdtable->lock);
    int newfd = __vfs_fdalloc(f);
    spin_unlock(&current->fdtable->lock);

    vfs_fput(f); // remove the reference from __vfs_argfd
    return newfd;
}

uint64 sys_vfs_dup2(void) {
    int oldfd, newfd;
    argint(0, &oldfd);
    argint(1, &newfd);

    if (newfd < 0 || newfd >= NOFILE) {
        return -EBADF;
    }

    struct vfs_file *f = __vfs_argfd(oldfd);
    if (f == NULL) {
        return -EBADF;
    }

    if (oldfd == newfd) {
        vfs_fput(f);
        return newfd;
    }

    spin_lock(&current->fdtable->lock);
    struct vfs_file *old_newfd = __vfs_fdfree(newfd);
    int ret = vfs_fdtable_alloc_fd_from(current->fdtable, f, newfd);
    spin_unlock(&current->fdtable->lock);

    if (old_newfd) {
        __vfs_fput_call_rcu(old_newfd);
    }
    vfs_fput(f);
    return ret;
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
    if (ret > 0)
        ACCT_ADD(current->thread_group, fs_bytes_read, (uint64)ret);
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
    if (ret > 0)
        ACCT_ADD(current->thread_group, fs_bytes_written, (uint64)ret);
    return ret;
}

uint64 sys_vfs_close(void) {
    int fd;
    argint(0, &fd);

    spin_lock(&current->fdtable->lock);
    struct vfs_file *f = __vfs_fdfree(fd);
    if (f == NULL) {
        spin_unlock(&current->fdtable->lock);
        return -EBADF;
    }
    spin_unlock(&current->fdtable->lock);

    __vfs_fput_call_rcu(f);
    ACCT_INC(current->thread_group, fs_closes);
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

    if (vm_copyout(current->vm, st, (char *)&kst, sizeof(kst)) < 0) {
        vfs_fput(f); // remove the reference from __vfs_argfd
        return -EFAULT;
    }

    vfs_fput(f); // remove the reference from __vfs_argfd
    return 0;
}

uint64 sys_vfs_lseek(void) {
    int fd, whence;
    int64 offset;
    argint(0, &fd);
    argint64(1, &offset);
    argint(2, &whence);

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL) {
        return -EBADF;
    }

    loff_t ret = vfs_filelseek(f, offset, whence);
    vfs_fput(f);
    return ret;
}

uint64 sys_vfs_ftruncate(void) {
    int fd;
    int64 length;
    argint(0, &fd);
    argint64(1, &length);

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL) {
        return -EBADF;
    }

    int ret = truncate(f, length);
    vfs_fput(f);
    return ret;
}

uint64 sys_vfs_fcntl(void) {
    int fd, cmd, arg;
    argint(0, &fd);
    argint(1, &cmd);
    argint(2, &arg);

    if (fd < 0 || fd >= NOFILE) {
        return -EBADF;
    }

    if (cmd == F_GETFD || cmd == F_SETFD) {
        spin_lock(&current->fdtable->lock);
        int ret = (cmd == F_GETFD)
                      ? vfs_fdtable_get_fdflags(current->fdtable, fd)
                      : vfs_fdtable_set_fdflags(current->fdtable, fd,
                                               arg & FD_CLOEXEC);
        spin_unlock(&current->fdtable->lock);
        return ret;
    }

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL) {
        return -EBADF;
    }

    int ret = -EINVAL;
    int normalized_cmd = cmd;
    if (cmd == 14) { /* newlib F_DUPFD_CLOEXEC */
        normalized_cmd = F_DUPFD_CLOEXEC;
    }
    switch (normalized_cmd) {
    case F_GETFL:
        ret = f->f_flags & ~O_CLOEXEC;
        break;
    case F_SETFL:
        f->f_flags = (f->f_flags & O_ACCMODE) | (arg & ~(O_ACCMODE | O_CLOEXEC));
        ret = 0;
        break;
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        if (arg < 0 || arg >= NOFILE) {
            ret = -EINVAL;
            break;
        }
        spin_lock(&current->fdtable->lock);
        ret = vfs_fdtable_alloc_fd_from(current->fdtable, f, arg);
        if (ret >= 0 && normalized_cmd == F_DUPFD_CLOEXEC) {
            (void)vfs_fdtable_set_fdflags(current->fdtable, ret, FD_CLOEXEC);
        }
        spin_unlock(&current->fdtable->lock);
        break;
    default:
        ret = -EINVAL;
        break;
    }

    vfs_fput(f);
    return ret;
}

static int __vfs_inode_stat(struct vfs_inode *inode, struct stat *kst) {
    if (inode->ops && inode->ops->getattr) {
        memset(kst, 0, sizeof(*kst));
        return inode->ops->getattr(inode, kst);
    }

    vfs_ilock(inode);
    memset(kst, 0, sizeof(*kst));
    kst->st_dev = inode->sb ? (uint64)inode->sb : 0;
    kst->st_ino = inode->ino;
    kst->st_mode = inode->mode;
    kst->st_nlink = inode->n_links;
    kst->st_uid = inode->uid;
    kst->st_gid = inode->gid;
    kst->st_size = inode->size;
    kst->st_blksize = 1024;
    kst->st_blocks = (inode->size + 511) / 512;
    kst->st_atime_sec = inode->atime;
    kst->st_mtime_sec = inode->mtime;
    kst->st_ctime_sec = inode->ctime;
    vfs_iunlock(inode);
    return 0;
}

uint64 sys_vfs_stat(void) {
    char path[MAXPATH];
    uint64 st_addr;
    int n = argstr(0, path, MAXPATH);
    argaddr(1, &st_addr);
    if (n < 0) {
        return -EFAULT;
    }

    /* vfs_namei now follows all symlinks automatically */
    struct vfs_inode *inode = vfs_namei(path, strlen(path));
    if (IS_ERR(inode)) {
        return PTR_ERR(inode);
    }
    if (inode == NULL) {
        return -ENOENT;
    }

    struct stat kst;
    int ret = __vfs_inode_stat(inode, &kst);
    vfs_iput(inode);
    if (ret != 0) {
        return ret;
    }
    if (either_copyout(1, st_addr, &kst, sizeof(kst)) < 0) {
        return -EFAULT;
    }
    return 0;
}

uint64 sys_vfs_lstat(void) {
    char path[MAXPATH];
    char name[DIRSIZ + 1];
    uint64 st_addr;
    int n = argstr(0, path, MAXPATH);
    argaddr(1, &st_addr);
    if (n < 0) {
        return -EFAULT;
    }

    struct vfs_inode *parent = vfs_nameiparent(path, n, name, DIRSIZ + 1);
    if (IS_ERR(parent)) {
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        return -ENOENT;
    }

    struct vfs_dentry dentry = {.sb = parent->sb, .parent = parent};
    int ret = vfs_ilookup(parent, &dentry, name, strlen(name));
    if (ret != 0) {
        vfs_iput(parent);
        return ret;
    }

    struct vfs_inode *inode = vfs_get_dentry_inode(&dentry);
    vfs_release_dentry(&dentry);
    vfs_iput(parent);
    if (IS_ERR(inode)) {
        return PTR_ERR(inode);
    }
    if (inode == NULL) {
        return -ENOENT;
    }

    struct stat kst;
    ret = __vfs_inode_stat(inode, &kst);
    vfs_iput(inode);
    if (ret != 0) {
        return ret;
    }
    if (either_copyout(1, st_addr, &kst, sizeof(kst)) < 0) {
        return -EFAULT;
    }
    return 0;
}

uint64 sys_vfs_access(void) {
    char path[MAXPATH];
    int mode;
    int n = argstr(0, path, MAXPATH);
    argint(1, &mode);
    if (n < 0) {
        return -EFAULT;
    }

    struct vfs_inode *inode = vfs_namei(path, n);
    if (IS_ERR(inode)) {
        return PTR_ERR(inode);
    }
    if (inode == NULL) {
        return -ENOENT;
    }

    if (mode != 0) {
        mode_t perm = inode->mode;
        if ((mode & 4) && !(perm & (S_IRUSR | S_IRGRP | S_IROTH))) {
            vfs_iput(inode);
            return -EACCES;
        }
        if ((mode & 2) && !(perm & (S_IWUSR | S_IWGRP | S_IWOTH))) {
            vfs_iput(inode);
            return -EACCES;
        }
        if ((mode & 1) && !(perm & (S_IXUSR | S_IXGRP | S_IXOTH))) {
            vfs_iput(inode);
            return -EACCES;
        }
    }

    vfs_iput(inode);
    return 0;
}

uint64 sys_vfs_readlink(void) {
    char path[MAXPATH];
    char name[DIRSIZ + 1];
    uint64 buf_addr;
    int bufsz;

    int n = argstr(0, path, MAXPATH);
    argaddr(1, &buf_addr);
    argint(2, &bufsz);

    if (n < 0) {
        return -EFAULT;
    }
    if (bufsz <= 0) {
        return -EINVAL;
    }

    struct vfs_inode *parent = vfs_nameiparent(path, n, name, DIRSIZ + 1);
    if (IS_ERR(parent)) {
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        return -ENOENT;
    }

    struct vfs_dentry dentry = {.sb = parent->sb, .parent = parent};
    int ret = vfs_ilookup(parent, &dentry, name, strlen(name));
    if (ret != 0) {
        vfs_iput(parent);
        return ret;
    }

    struct vfs_inode *inode = vfs_get_dentry_inode(&dentry);
    vfs_release_dentry(&dentry);
    vfs_iput(parent);
    if (IS_ERR(inode)) {
        return PTR_ERR(inode);
    }
    if (inode == NULL) {
        return -ENOENT;
    }

    char *kbuf = kvmalloc(bufsz);
    if (kbuf == NULL) {
        vfs_iput(inode);
        return -ENOMEM;
    }

    ssize_t len = vfs_readlink(inode, kbuf, bufsz);
    vfs_iput(inode);

    if (len < 0) {
        kvfree(kbuf);
        return len;
    }

    if (either_copyout(1, buf_addr, kbuf, len) < 0) {
        kvfree(kbuf);
        return -EFAULT;
    }
    kvfree(kbuf);
    return len;
}

uint64 sys_vfs_rename(void) {
    char oldpath[MAXPATH], newpath[MAXPATH];
    char oldname[DIRSIZ + 1], newname[DIRSIZ + 1];
    int n1 = argstr(0, oldpath, MAXPATH);
    int n2 = argstr(1, newpath, MAXPATH);
    if (n1 < 0 || n2 < 0) {
        return -EFAULT;
    }

    struct vfs_inode *old_parent =
        vfs_nameiparent(oldpath, n1, oldname, DIRSIZ + 1);
    if (IS_ERR(old_parent)) {
        return PTR_ERR(old_parent);
    }
    if (old_parent == NULL) {
        return -ENOENT;
    }

    struct vfs_inode *new_parent =
        vfs_nameiparent(newpath, n2, newname, DIRSIZ + 1);
    if (IS_ERR(new_parent)) {
        vfs_iput(old_parent);
        return PTR_ERR(new_parent);
    }
    if (new_parent == NULL) {
        vfs_iput(old_parent);
        return -ENOENT;
    }

    struct vfs_dentry old_dentry = {.sb = old_parent->sb, .parent = old_parent};
    int ret = vfs_ilookup(old_parent, &old_dentry, oldname, strlen(oldname));
    if (ret != 0) {
        vfs_iput(old_parent);
        vfs_iput(new_parent);
        return ret;
    }

    ret = vfs_move(old_parent, &old_dentry, new_parent, newname, strlen(newname));
    vfs_release_dentry(&old_dentry);
    vfs_iput(old_parent);
    vfs_iput(new_parent);
    if (ret == 0)
        ACCT_INC(current->thread_group, fs_renames);
    return ret;
}

/******************************************************************************
 * File System Namespace Syscalls
 ******************************************************************************/

uint64 sys_vfs_open(void) {
    char path[MAXPATH];
    char name[DIRSIZ + 1];
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
        struct vfs_inode *parent = vfs_nameiparent(path, n, name, DIRSIZ + 1);
        if (IS_ERR(parent)) {
            return PTR_ERR(parent);
        }
        if (parent == NULL) {
            return -ENOENT;
        }

        size_t name_len = strlen(name);

        // Try to create (apply umask to default mode)
        inode = vfs_create(parent, 0666 & ~current_umask(), name, name_len);
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
        /*
         * Open existing file.
         * vfs_namei now follows all symlinks automatically.
         * For O_NOFOLLOW, use nameiparent+ilookup to avoid resolving
         * the final symlink.
         */
        if (omode & O_NOFOLLOW) {
            char oname[DIRSIZ + 1];
            struct vfs_inode *parent =
                vfs_nameiparent(path, strlen(path), oname, DIRSIZ + 1);
            if (IS_ERR(parent))
                return PTR_ERR(parent);
            if (parent == NULL)
                return -ENOENT;

            struct vfs_dentry dentry = {.sb = parent->sb, .parent = parent};
            int lr = vfs_ilookup(parent, &dentry, oname, strlen(oname));
            if (lr != 0) {
                vfs_iput(parent);
                return lr;
            }
            inode = vfs_get_dentry_inode(&dentry);
            vfs_release_dentry(&dentry);
            vfs_iput(parent);
            if (IS_ERR(inode))
                return PTR_ERR(inode);
            if (inode == NULL)
                return -ENOENT;
            /* O_NOFOLLOW + symlink → ELOOP */
            if (S_ISLNK(inode->mode)) {
                vfs_iput(inode);
                return -ELOOP;
            }
        } else {
            inode = vfs_namei(path, strlen(path));
            if (IS_ERR(inode))
                return PTR_ERR(inode);
            if (inode == NULL)
                return -ENOENT;
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
    vfs_iput(inode); // Release local inode reference (file holds its own ref)

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

    spin_lock(&current->fdtable->lock);
    int fd = __vfs_fdalloc(f);
    if (fd >= 0 && (omode & O_CLOEXEC)) {
        (void)vfs_fdtable_set_fdflags(current->fdtable, fd, FD_CLOEXEC);
    }
    spin_unlock(&current->fdtable->lock);

    // When success, the refcount of f will be increased by fdtable, thus we do
    // not put f here. When failure, we need to put f anyway.
    vfs_fput(f);
    if (fd >= 0)
        ACCT_INC(current->thread_group, fs_opens);
    return fd;
}

uint64 sys_vfs_mkdir(void) {
    char path[MAXPATH];
    char name[DIRSIZ + 1];
    int n;

    if ((n = argstr(0, path, MAXPATH)) < 0) {
        return -EFAULT;
    }

    struct vfs_inode *parent = vfs_nameiparent(path, n, name, DIRSIZ + 1);
    if (IS_ERR(parent)) {
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        return -ENOENT;
    }

    size_t name_len = strlen(name);

    struct vfs_inode *dir = vfs_mkdir(parent, 0777 & ~current_umask(), name, name_len);
    vfs_iput(parent);

    if (IS_ERR(dir)) {
        return PTR_ERR(dir);
    }

    vfs_iput(dir);
    ACCT_INC(current->thread_group, fs_creates);
    return 0;
}

uint64 sys_vfs_mknod(void) {
    if (!capable())
        return (uint64)-EPERM;

    char path[MAXPATH];
    char name[DIRSIZ + 1];
    int mode, major, minor;
    int n;

    if ((n = argstr(0, path, MAXPATH)) < 0) {
        return -EFAULT;
    }
    argint(1, &mode);
    argint(2, &major);
    argint(3, &minor);

    struct vfs_inode *parent = vfs_nameiparent(path, n, name, DIRSIZ + 1);
    if (IS_ERR(parent)) {
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        return -ENOENT;
    }

    size_t name_len = strlen(name);

    dev_t dev = mkdev(major, minor);
    struct vfs_inode *node =
        vfs_mknod(parent, (mode_t)mode, dev, name, name_len);
    vfs_iput(parent);

    if (IS_ERR(node)) {
        return PTR_ERR(node);
    }

    vfs_iput(node);
    return 0;
}

uint64 sys_vfs_unlink(void) {
    char path[MAXPATH];
    char name[DIRSIZ + 1];
    int n;

    if ((n = argstr(0, path, MAXPATH)) < 0) {
        return -EFAULT;
    }

    struct vfs_inode *parent = vfs_nameiparent(path, n, name, DIRSIZ + 1);
    if (IS_ERR(parent)) {
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        return -ENOENT;
    }

    size_t name_len = strlen(name);

    int ret = vfs_unlink(parent, name, name_len);
    vfs_iput(parent);
    if (ret == 0)
        ACCT_INC(current->thread_group, fs_deletes);
    return ret;
}

uint64 sys_vfs_link(void) {
    char old[MAXPATH], new[MAXPATH];
    char name[DIRSIZ + 1];
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
    struct vfs_inode *parent = vfs_nameiparent(new, n2, name, DIRSIZ + 1);
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
    if (ret == 0)
        ACCT_INC(current->thread_group, fs_links);
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
static int vfs_make_absolute_path(const char *relpath, int relpath_len,
                                  char *abspath) {
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
    struct thread *p = current;
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
    char name[DIRSIZ + 1];
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

    struct vfs_inode *parent = vfs_nameiparent(linkpath, n2, name, DIRSIZ + 1);
    if (IS_ERR(parent)) {
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        return -ENOENT;
    }

    size_t name_len = strlen(name);

    struct vfs_inode *sym =
        vfs_symlink(parent, 0777, name, name_len, abs_target, abs_len);
    vfs_iput(parent);

    if (IS_ERR(sym)) {
        return PTR_ERR(sym);
    }

    vfs_iput(sym);
    ACCT_INC(current->thread_group, fs_links);
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

    // Get reference to new cwd BEFORE acquiring spinlock
    // (vfs_inode_get_ref may acquire the inode mutex internally)
    struct vfs_inode_ref new_cwd_ref;
    int ret = vfs_inode_get_ref(inode, &new_cwd_ref);
    if (ret != 0) {
        vfs_iput(inode);
        return ret;
    }

    // Update process cwd (only assignment under spinlock)
    struct thread *p = current;
    vfs_struct_lock(p->fs);
    struct vfs_inode_ref old_cwd = p->fs->cwd;
    p->fs->cwd = new_cwd_ref;
    vfs_struct_unlock(p->fs);

    // Release old cwd
    vfs_inode_put_ref(&old_cwd);
    vfs_iput(inode);

    ACCT_INC(current->thread_group, fs_chdirs);
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

    struct thread *p = current;
    vfs_struct_lock(p->fs);
    struct vfs_inode *cwd = vfs_inode_deref(&p->fs->cwd);
    struct vfs_inode *root = vfs_inode_deref(&p->fs->rooti);
    vfs_struct_unlock(p->fs);

    if (cwd == NULL) {
        return -ENOENT;
    }

    // Build path from cwd to root by collecting names
    // We build it in reverse, then reverse the result
    char *names[MAXPATH / 2]; // Stack of name pointers
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

    spin_lock(&current->fdtable->lock);
    int fd0 = __vfs_fdalloc(rf);
    if (fd0 < 0) {
        spin_unlock(&current->fdtable->lock);
        // Decrease the refcounts allocated by pipealloc
        vfs_fput(rf);
        vfs_fput(wf);
        return fd0;
    }

    int fd1 = __vfs_fdalloc(wf);
    if (fd1 < 0) {
        __vfs_fdfree(fd0);
        spin_unlock(&current->fdtable->lock);
        // Decrease the refcounts allocated by pipealloc
        vfs_fput(rf);
        vfs_fput(wf);
        // Decrease the refcounts allocated by fdtable
        __vfs_fput_call_rcu(rf);
        return fd1;
    }
    spin_unlock(&current->fdtable->lock);

    // vm_copyout may sleep (acquires rwsem), so must be outside spinlock
    struct thread *p = current;
    if (vm_copyout(p->vm, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
        vm_copyout(p->vm, fdarray + sizeof(fd0), (char *)&fd1, sizeof(fd1)) <
            0) {
        // Re-acquire lock to deallocate fds
        spin_lock(&current->fdtable->lock);
        __vfs_fdfree(fd0);
        __vfs_fdfree(fd1);
        spin_unlock(&current->fdtable->lock);

        // Decrease the refcounts allocated by pipealloc
        vfs_fput(rf);
        vfs_fput(wf);
        // Decrease the refcounts allocated by fdtable
        __vfs_fput_call_rcu(rf);
        __vfs_fput_call_rcu(wf);
        return -EFAULT;
    }

    // Release the references from pipealloc - fdtable holds its own references
    // now (same pattern as sys_vfs_open which calls vfs_fput after
    // __vfs_fdalloc)
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

    spin_lock(&current->fdtable->lock);
    int fd = __vfs_fdalloc(f);
    spin_unlock(&current->fdtable->lock);

    // When success, the refcount of f will be increased by fdtable, thus we do
    // not put f here. When failure, we need to put f anyway.
    vfs_fput(f);
    return fd;
}

/******************************************************************************
 * Directory Operations - getdents
 ******************************************************************************/

// Linux-compatible dirent structure
struct linux_dirent64 {
    uint64 d_ino;    // Inode number
    int64 d_off;     // Offset to next structure
    uint16 d_reclen; // Size of this dirent
    uint8 d_type;    // File type
    char d_name[];   // Filename (null-terminated)
};

// File type constants
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12

static uint8 __mode_to_dtype(mode_t mode) {
    if (S_ISREG(mode))
        return DT_REG;
    if (S_ISDIR(mode))
        return DT_DIR;
    if (S_ISCHR(mode))
        return DT_CHR;
    if (S_ISBLK(mode))
        return DT_BLK;
    if (S_ISFIFO(mode))
        return DT_FIFO;
    if (S_ISLNK(mode))
        return DT_LNK;
    if (S_ISSOCK(mode))
        return DT_SOCK;
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
    char *kbuf = kvmalloc(count);
    if (kbuf == NULL) {
        vfs_fput(f); // remove the reference from __vfs_argfd
        return -ENOMEM;
    }

    int bytes_written = 0;
    struct vfs_dentry dentry = {0};
    int ret;

    while (bytes_written < count) {
        // Save iterator state before calling dir_iter, in case we need to
        // revert
        uint64 saved_cookies = f->dir_iter.cookies;
        uint64 saved_index = f->dir_iter.index;

        ret = vfs_dir_iter(inode, &f->dir_iter, &dentry);
        if (ret != 0) {
            kvfree(kbuf);
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
            // Not enough space, restore iterator state for next call
            f->dir_iter.cookies = saved_cookies;
            f->dir_iter.index = saved_index;
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
        struct linux_dirent64 *de =
            (struct linux_dirent64 *)(kbuf + bytes_written);
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
        if (vm_copyout(current->vm, dirp, kbuf, bytes_written) < 0) {
            kvfree(kbuf);
            vfs_fput(f); // remove the reference from __vfs_argfd
            return -EFAULT;
        }
    }

    kvfree(kbuf);
    vfs_fput(f); // remove the reference from __vfs_argfd
    return bytes_written;
}

/******************************************************************************
 * chroot - Change root directory
 ******************************************************************************/

uint64 sys_chroot(void) {
    if (!capable())
        return (uint64)-EPERM;

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

    if (ret == 0)
        ACCT_INC(current->thread_group, fs_chdirs);
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
 * @source: source device path, UUID=<guid>, file path, or NULL
 * @source_len: length of source path
 *
 * Source resolution order:
 *   1. "UUID=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
 *      → look up partition by GUID → mount the partition block device
 *   2. Path to a block device (e.g., "/dev/disk1p1")
 *      → mount that block device directly
 *   3. Path to a regular file (e.g., "/mnt/image.ext4")
 *      → auto-attach to a free loop device → mount the loop device
 *   4. NULL or empty → pseudo-filesystem (tmpfs, devtmpfs, etc.)
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
    int loop_used = -1;  /* loop device index if auto-setup, else -1 */

    if (source != NULL && source_len > 0) {
        /*
         * Case 1: UUID=<guid> — resolve partition by GUID
         */
        if (source_len > 5 &&
            source[0] == 'U' && source[1] == 'U' &&
            source[2] == 'I' && source[3] == 'D' && source[4] == '=') {
            const char *guid_str = source + 5;
            struct gpt_guid guid;
            if (gendisk_guid_parse(guid_str, &guid) != 0) {
                printf("mount: invalid UUID format: %s\n", guid_str);
                vfs_iput(target_dir);
                return -EINVAL;
            }

            blkdev_t *bdev = gendisk_find_by_guid(&guid);
            if (bdev == NULL) {
                printf("mount: no partition found with UUID=%s\n", guid_str);
                vfs_iput(target_dir);
                return -ENODEV;
            }

            /* Build /dev/<devname> path and look it up */
            char dev_path[MAXPATH];
            int pos = 0;
            const char *pfx = "/dev/";
            while (*pfx && pos < MAXPATH - 1)
                dev_path[pos++] = *pfx++;
            const char *dn = bdev->dev.devname;
            if (dn != NULL) {
                while (*dn && pos < MAXPATH - 1)
                    dev_path[pos++] = *dn++;
            }
            dev_path[pos] = '\0';

            struct vfs_inode *dev_inode = vfs_namei(dev_path, pos);
            if (!IS_ERR_OR_NULL(dev_inode) && S_ISBLK(dev_inode->mode)) {
                source_inode = dev_inode;
            } else {
                if (!IS_ERR_OR_NULL(dev_inode))
                    vfs_iput(dev_inode);
                printf("mount: UUID resolved to %s but device not found\n",
                       dev_path);
                vfs_iput(target_dir);
                return -ENODEV;
            }
        } else {
            /*
             * Try vfs_namei on the source path first.
             */
            struct vfs_inode *source_dev = vfs_namei(source, source_len);
            if (!IS_ERR_OR_NULL(source_dev)) {
                if (S_ISBLK(source_dev->mode)) {
                    /*
                     * Case 2: Block device path
                     */
                    source_inode = source_dev;
                } else if (S_ISREG(source_dev->mode)) {
                    /*
                     * Case 3: Regular file — auto-setup loop device
                     */
                    /* Open the file to get a vfs_file for loop_setup */
                    struct vfs_file *file = vfs_fileopen(source_dev, O_RDWR);
                    if (IS_ERR_OR_NULL(file)) {
                        /* Try read-only */
                        file = vfs_fileopen(source_dev, O_RDONLY);
                    }
                    vfs_iput(source_dev);
                    source_dev = NULL;

                    if (IS_ERR_OR_NULL(file)) {
                        printf("mount: cannot open file %s\n", source);
                        vfs_iput(target_dir);
                        return IS_ERR(file) ? PTR_ERR(file) : -EIO;
                    }

                    /* Find a free loop device */
                    int loop_num = -1;
                    for (int i = 0; i < NLOOP; i++) {
                        if (loop_is_free(i)) {
                            loop_num = i;
                            break;
                        }
                    }
                    if (loop_num < 0) {
                        printf("mount: no free loop device available\n");
                        vfs_fput(file);
                        vfs_iput(target_dir);
                        return -EBUSY;
                    }

                    int ret = loop_setup(loop_num, file, 0);
                    vfs_fput(file); /* loop_setup dups internally */
                    if (ret != 0) {
                        printf("mount: failed to setup loop%d: %d\n",
                               loop_num, ret);
                        vfs_iput(target_dir);
                        return ret;
                    }
                    loop_used = loop_num;

                    /* Build /dev/loopN path */
                    char dev_path[MAXPATH];
                    blkdev_t *ldev = loop_get_blkdev(loop_num);
                    int pos = 0;
                    const char *pfx = "/dev/";
                    while (*pfx && pos < MAXPATH - 1)
                        dev_path[pos++] = *pfx++;
                    const char *dn = ldev->dev.devname;
                    if (dn != NULL) {
                        while (*dn && pos < MAXPATH - 1)
                            dev_path[pos++] = *dn++;
                    }
                    dev_path[pos] = '\0';

                    struct vfs_inode *loop_inode = vfs_namei(dev_path, pos);
                    if (!IS_ERR_OR_NULL(loop_inode) &&
                        S_ISBLK(loop_inode->mode)) {
                        source_inode = loop_inode;
                    } else {
                        if (!IS_ERR_OR_NULL(loop_inode))
                            vfs_iput(loop_inode);
                        loop_clear(loop_num);
                        printf("mount: loop device %s not found in devtmpfs\n",
                               dev_path);
                        vfs_iput(target_dir);
                        return -ENODEV;
                    }
                } else {
                    vfs_iput(source_dev);
                }
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

    // On success, release locks. On failure, vfs_mount already released them.
    if (ret == 0) {
        vfs_iunlock(target_dir);
        vfs_superblock_unlock(target_dir->sb);
    }
    vfs_mount_unlock();

    /* On failure, clean up auto-attached loop device */
    if (ret != 0 && loop_used >= 0) {
        loop_clear(loop_used);
    }

    if (source_inode) {
        vfs_iput(source_inode);
    }
    vfs_iput(target_dir);

    return ret;
}

uint64 sys_mount(void) {
    if (!capable())
        return (uint64)-EPERM;

    char source[MAXPATH];
    char target[MAXPATH];
    char fstype[32];
    int n1, n2;

    if ((n1 = argstr(0, source, MAXPATH)) < 0 ||
        (n2 = argstr(1, target, MAXPATH)) < 0 || argstr(2, fstype, 32) < 0) {
        return -EFAULT;
    }

    int ret = vfs_mount_path(fstype, target, n2, source, n1);
    if (ret == 0)
        ACCT_INC(current->thread_group, fs_mounts);
    return ret;
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

    // Check if this is a mounted filesystem root (parent == self for local
    // root)
    if (!vfs_inode_is_local_root(mounted_root)) {
        vfs_iput(mounted_root);
        return -EINVAL; // Not a mounted filesystem root
    }

    // Get the mountpoint from the superblock
    struct vfs_superblock *child_sb = mounted_root->sb;
    if (child_sb == NULL || child_sb->mountpoint == NULL) {
        vfs_iput(mounted_root);
        return -EINVAL; // Not mounted or no mountpoint
    }

    struct vfs_inode *target_dir = child_sb->mountpoint;
    if (!target_dir->mount) {
        vfs_iput(mounted_root);
        return -EINVAL; // Mountpoint not marked as mount
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
    // - Removed, unlocked and freed mounted_root (root inode)
    // - Unlocked and freed child_sb
    // - Unlocked target_dir (mountpoint) and target_dir->sb
    // - Called vfs_iput on target_dir
    // We just need to release the mount mutex.
    // Note: mounted_root is freed by vfs_unmount; do NOT access it.
    vfs_mount_unlock();

    return 0;
}

uint64 sys_umount(void) {
    if (!capable())
        return (uint64)-EPERM;

    char target[MAXPATH];
    int n;

    if ((n = argstr(0, target, MAXPATH)) < 0) {
        return -EFAULT;
    }

    int ret = vfs_umount_path(target, n);
    if (ret == 0)
        ACCT_INC(current->thread_group, fs_mounts);
    return ret;
}

/******************************************************************************
 * Debug: Dump active inodes
 * If path is provided (non-null arg0), dump only the superblock containing that
 * path. Otherwise, dump all superblocks.
 ******************************************************************************/

uint64 sys_dumpinode(void) {
    char path[MAXPATH];
    int n;

    // Try to get path argument - if not provided, dump all
    n = argstr(0, path, MAXPATH);
    if (n < 0) {
        // No path argument, dump all inodes
        vfs_dump_inodes();
        return 0;
    }

    // Path provided - resolve it to find its superblock
    struct vfs_inode *inode = vfs_namei(path, n);
    if (!inode) {
        printf("dumpinode: cannot find path '%s'\n", path);
        return -ENOENT;
    }

    struct vfs_superblock *sb = inode->sb;
    vfs_iput(inode);

    if (!sb) {
        printf("dumpinode: inode has no superblock\n");
        return -EINVAL;
    }

    vfs_dump_sb_inodes(sb);
    return 0;
}

/******************************************************************************
 * TTY / ioctl Syscalls
 ******************************************************************************/

/**
 * sys_vfs_ioctl - generic ioctl syscall
 *
 * Arguments: a0 = fd, a1 = cmd, a2 = arg (user-space pointer)
 *
 * Copies data in/out of user space based on the ioctl command,
 * then calls vfs_ioctl with a kernel pointer.
 */
uint64 sys_vfs_ioctl(void) {
    int fd;
    uint64 cmd, arg;

    argint(0, &fd);
    argaddr(1, &cmd);
    argaddr(2, &arg);

    /* ioctl command codes are 32-bit; user-space (musl) passes the code as
     * a signed int, so the register value may be sign-extended on 64-bit
     * (e.g. TIOCGPTN 0x80045430 → 0xFFFFFFFF80045430).  Truncate back to
     * unsigned-32 so the switch cases match correctly. */
    cmd = (unsigned int)cmd;

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        return -EBADF;

    int ret;

    /*
     * For known TTY ioctls, copy the data in/out of user space here,
     * then pass the kernel buffer to vfs_ioctl.  For unknown commands,
     * pass the raw arg through as an opaque void* (the handler is
     * responsible for interpreting it).
     */
    switch (cmd) {
    case TCGETS: {
        struct termios kt;
        ret = vfs_ioctl(f, cmd, &kt);
        if (ret == 0) {
            if (either_copyout(1, arg, &kt, sizeof(kt)) < 0)
                ret = -EFAULT;
        }
        break;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        struct termios kt;
        if (either_copyin(&kt, 1, arg, sizeof(kt)) < 0) {
            ret = -EFAULT;
        } else {
            ret = vfs_ioctl(f, cmd, &kt);
        }
        break;
    }
    case TIOCGWINSZ: {
        struct winsize kws;
        ret = vfs_ioctl(f, cmd, &kws);
        if (ret == 0) {
            if (either_copyout(1, arg, &kws, sizeof(kws)) < 0)
                ret = -EFAULT;
        }
        break;
    }
    case TIOCSWINSZ: {
        struct winsize kws;
        if (either_copyin(&kws, 1, arg, sizeof(kws)) < 0) {
            ret = -EFAULT;
        } else {
            ret = vfs_ioctl(f, cmd, &kws);
        }
        break;
    }
    case TIOCGPGRP: {
        pid_t kpgid;
        ret = vfs_ioctl(f, cmd, &kpgid);
        if (ret == 0) {
            if (either_copyout(1, arg, &kpgid, sizeof(kpgid)) < 0)
                ret = -EFAULT;
        }
        break;
    }
    case TIOCSPGRP: {
        pid_t kpgid;
        if (either_copyin(&kpgid, 1, arg, sizeof(kpgid)) < 0) {
            ret = -EFAULT;
        } else {
            ret = vfs_ioctl(f, cmd, &kpgid);
        }
        break;
    }
    case TIOCGPTN: {
        int kptn;
        ret = vfs_ioctl(f, cmd, &kptn);
        if (ret == 0) {
            if (either_copyout(1, arg, &kptn, sizeof(kptn)) < 0)
                ret = -EFAULT;
        }
        break;
    }
    case TIOCSCTTY: {
        /* arg is an integer flag (usually 0), pass through */
        ret = vfs_ioctl(f, cmd, (void *)arg);
        break;
    }
    default:
        /* Unknown ioctl — pass arg through as opaque pointer */
        ret = vfs_ioctl(f, cmd, (void *)arg);
        break;
    }

    vfs_fput(f);
    return ret;
}

/**
 * sys_tcgetattr - get terminal attributes
 *
 * Arguments: a0 = fd, a1 = termios_p (user pointer)
 *
 * Equivalent to ioctl(fd, TCGETS, termios_p).
 */
uint64 sys_tcgetattr(void) {
    int fd;
    uint64 termios_p;

    argint(0, &fd);
    argaddr(1, &termios_p);

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        return -EBADF;

    struct termios kt;
    int ret = vfs_ioctl(f, TCGETS, &kt);
    if (ret == 0) {
        if (either_copyout(1, termios_p, &kt, sizeof(kt)) < 0)
            ret = -EFAULT;
    }
    vfs_fput(f);
    return ret;
}

/**
 * sys_tcsetattr - set terminal attributes
 *
 * Arguments: a0 = fd, a1 = optional_actions, a2 = termios_p (user pointer)
 *
 * optional_actions: TCSANOW (0), TCSADRAIN (1), TCSAFLUSH (2)
 * Maps to TCSETS / TCSETSW / TCSETSF ioctls respectively.
 */
uint64 sys_tcsetattr(void) {
    int fd, optional_actions;
    uint64 termios_p;

    argint(0, &fd);
    argint(1, &optional_actions);
    argaddr(2, &termios_p);

    uint64 cmd;
    switch (optional_actions) {
    case 0: /* TCSANOW */
        cmd = TCSETS;
        break;
    case 1: /* TCSADRAIN */
        cmd = TCSETSW;
        break;
    case 2: /* TCSAFLUSH */
        cmd = TCSETSF;
        break;
    default:
        return -EINVAL;
    }

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        return -EBADF;

    struct termios kt;
    if (either_copyin(&kt, 1, termios_p, sizeof(kt)) < 0) {
        vfs_fput(f);
        return -EFAULT;
    }

    int ret = vfs_ioctl(f, cmd, &kt);
    vfs_fput(f);
    return ret;
}

struct pollfd_k {
    int fd;
    short events;
    short revents;
};

/*
 * __vfs_poll_always_ready - report requested events as ready based on
 * file access mode.  Used as fallback when no specific poll callback
 * is available (regular files, block devices, /dev/null, etc.).
 */
static inline short __vfs_poll_always_ready(short events, int f_flags) {
    short revents = 0;
    if ((events & (POLLIN | POLLRDNORM)) &&
        ((f_flags & O_ACCMODE) != O_WRONLY))
        revents |= (events & (POLLIN | POLLRDNORM));
    if ((events & (POLLOUT | POLLWRNORM)) &&
        ((f_flags & O_ACCMODE) != O_RDONLY))
        revents |= (events & (POLLOUT | POLLWRNORM));
    return revents;
}

/*
 * __vfs_poll_scan - check readiness of a set of file descriptors
 *
 * Dispatches poll queries based on file type:
 *
 *   1. f->ops->poll != NULL           → delegate to VFS file_ops poll
 *      (covers pipes, lwIP sockets, and any future pollable file type)
 *
 *   2. inode == NULL && f->ops == NULL → legacy socket (struct sock)
 *      → call sockpoll()
 *
 *   3. S_ISCHR(inode->mode)           → character device
 *      → call cdev->ops.poll() if present, else always ready
 *
 *   4. everything else                → always ready
 *      (regular files, directories, block devices)
 */
static int __vfs_poll_scan(struct pollfd_k *pfds, int nfds) {
    int ready = 0;

    for (int i = 0; i < nfds; i++) {
        struct pollfd_k *pfd = &pfds[i];
        pfd->revents = 0;

        if (pfd->fd < 0)
            continue;

        struct vfs_file *f = __vfs_argfd(pfd->fd);
        if (f == NULL) {
            pfd->revents |= POLLNVAL;
            ready++;
            continue;
        }

        /*
         * Priority 1: if the file has vfs_file_ops with a poll callback,
         * use it.  This is the primary dispatch path for pipes, lwIP
         * sockets, and any file type that defines file_ops->poll.
         */
        if (f->ops != NULL && f->ops->poll != NULL) {
            pfd->revents = f->ops->poll(f, pfd->events);
            goto done;
        }

        struct vfs_inode *inode = f->inode.inode;

        if (inode == NULL) {
            /*
             * No inode AND no file_ops (or no poll callback in ops).
             *
             * If f->ops == NULL entirely, this is a legacy socket
             * created via vfs_sockalloc() — poll its rxq.
             *
             * If f->ops != NULL but ops->poll == NULL, treat as
             * always ready (shouldn't normally happen).
             */
            if (f->ops == NULL && f->sock != NULL) {
                pfd->revents = sockpoll(f->sock, pfd->events);
            } else {
                pfd->revents = __vfs_poll_always_ready(pfd->events,
                                                       f->f_flags);
            }
        } else if (S_ISCHR(inode->mode)) {
            /*
             * Character device — delegate to the device's poll callback
             * if one is registered.  Otherwise fall back to always ready.
             */
            cdev_t *cdev = f->cdev;
            if (cdev != NULL && cdev->ops.poll != NULL) {
                pfd->revents = cdev->ops.poll(cdev, pfd->events);
            } else {
                pfd->revents = __vfs_poll_always_ready(pfd->events,
                                                       f->f_flags);
            }
        } else {
            /*
             * Regular files, directories, block devices.
             * If file_ops provides a poll callback, use it; otherwise
             * report always ready (standard POSIX behaviour for
             * regular files).
             */
            pfd->revents = __vfs_poll_always_ready(pfd->events, f->f_flags);
        }

done:
        vfs_fput(f);

        if (pfd->revents != 0)
            ready++;
    }

    return ready;
}

/*
 * sys_vfs_poll - event polling over file descriptors using kqueue
 *
 * Arguments:
 *   a0 = pointer to struct pollfd array
 *   a1 = nfds
 *   a2 = timeout_ms (-1: infinite, 0: non-blocking)
 *
 * Uses the kqueue subsystem for proper event-driven waiting instead of
 * spin-polling.  For non-blocking polls (timeout_ms=0), a direct scan
 * is performed without kqueue overhead.
 */
static uint64 __vfs_poll_impl(uint64 fds_addr, int nfds, int timeout_ms) {

    if (nfds < 0 || nfds > NOFILE) {
        return -EINVAL;
    }

    /* Empty fd set: just sleep for timeout */
    if (nfds == 0) {
        if (timeout_ms == 0)
            return 0;
        uint64 start = get_jiffs();
        while (timeout_ms < 0 || (int)(get_jiffs() - start) < timeout_ms) {
            sleep_ms(1);
            if (signal_pending(current))
                return -EINTR;
        }
        return 0;
    }

    size_t bytes = (size_t)nfds * sizeof(struct pollfd_k);
    struct pollfd_k *pfds = kvmalloc(bytes);
    if (pfds == NULL)
        return -ENOMEM;

    if (either_copyin(pfds, 1, fds_addr, bytes) < 0) {
        kvfree(pfds);
        return -EFAULT;
    }

    /* --- Non-blocking fast path (timeout_ms == 0) --- */
    int ready = __vfs_poll_scan(pfds, nfds);
    if (timeout_ms == 0 || ready > 0)
        goto copyout;

    /* --- Blocking path: use kqueue for event-driven wait --- */

    /*
     * Always use periodic rescan.  Some fd types (e.g. PTY master) have
     * a poll callback for readiness checks but never call
     * vfs_file_knote_notify(), so relying on kqueue alone would block
     * forever when only those fds become ready.  The 10 ms rescan
     * interval (POLL_RESCAN_MS) adds negligible overhead while
     * guaranteeing correctness for all fd types.
     */
    int has_unnotified_fds = 1;

    int kqfd = kqueue_create();
    if (kqfd < 0) {
        /* If kqueue_create fails, fall back to a single scan */
        goto copyout;
    }

    /* Resolve kqueue from fd */
    struct vfs_file *kqfile = __vfs_argfd(kqfd);
    struct kqueue *kq = kqfile ? (struct kqueue *)kqfile->private_data : NULL;
    if (kq == NULL) {
        if (kqfile)
            vfs_fput(kqfile);
        __vfs_close_fd(kqfd);
        goto copyout;
    }
    vfs_fput(kqfile);

    /*
     * Register EVFILT_READ and/or EVFILT_WRITE for each polled fd.
     * Knotes stay registered (no EV_ONESHOT) so they can re-trigger
     * across loop iterations.  kqueue_close cleans them up.
     * Store the pollfd index in udata for result mapping.
     */
    int max_changes = nfds * 2; /* worst case: READ+WRITE per fd */
    struct kevent *changes = kvmalloc(max_changes * sizeof(struct kevent));
    if (changes == NULL) {
        __vfs_close_fd(kqfd);
        goto copyout;
    }
    int nchanges = 0;

    for (int i = 0; i < nfds; i++) {
        if (pfds[i].fd < 0)
            continue;
        if (pfds[i].events & (POLLIN | POLLRDNORM)) {
            changes[nchanges].ident = pfds[i].fd;
            changes[nchanges].filter = EVFILT_READ;
            changes[nchanges].flags = EV_ADD;
            changes[nchanges].fflags = 0;
            changes[nchanges].data = 0;
            changes[nchanges].udata = (uint64)i;
            nchanges++;
        }
        if (pfds[i].events & (POLLOUT | POLLWRNORM)) {
            changes[nchanges].ident = pfds[i].fd;
            changes[nchanges].filter = EVFILT_WRITE;
            changes[nchanges].flags = EV_ADD;
            changes[nchanges].fflags = 0;
            changes[nchanges].data = 0;
            changes[nchanges].udata = (uint64)i;
            nchanges++;
        }
    }

    if (nchanges > 0)
        kqueue_register(kq, changes, nchanges);

    /*
     * Wait for events.  If any polled fd lacks kqueue notification
     * support (e.g. chardevs), cap each kqueue_wait and re-scan.
     * For fds with full kqueue support (pipes, sockets), wakeup is
     * instant via vfs_file_knote_notify.
     */
    #define POLL_RESCAN_MS 10

    struct kevent *events = kvmalloc(nfds * sizeof(struct kevent));
    if (events == NULL) {
        kvfree(changes);
        __vfs_close_fd(kqfd);
        goto copyout;
    }

    uint64 poll_start = get_jiffs();
    for (;;) {
        /* Compute kqueue_wait timeout for this iteration */
        int kq_tmo;
        if (has_unnotified_fds) {
            if (timeout_ms < 0) {
                kq_tmo = POLL_RESCAN_MS;
            } else {
                int remaining = timeout_ms - (int)(get_jiffs() - poll_start);
                if (remaining <= 0)
                    break;
                kq_tmo = remaining < POLL_RESCAN_MS ?
                         remaining : POLL_RESCAN_MS;
            }
        } else {
            /* All fds support kqueue notification — full timeout */
            if (timeout_ms < 0) {
                kq_tmo = -1;
            } else {
                int remaining = timeout_ms - (int)(get_jiffs() - poll_start);
                if (remaining <= 0)
                    break;
                kq_tmo = remaining;
            }
        }

        int nevents = kqueue_wait(kq, events, nfds, kq_tmo);

        if (nevents < 0 && nevents == -EINTR) {
            ready = -EINTR;
            break;
        }

        /* Always re-scan: catches chardev events and ensures
         * revents is correctly populated for copyout. */
        ready = __vfs_poll_scan(pfds, nfds);
        if (ready > 0)
            break;

        if (signal_pending(current)) {
            ready = -EINTR;
            break;
        }
    }

    #undef POLL_RESCAN_MS

    /* Close the temporary kqueue fd (detaches all knotes) */
    __vfs_close_fd(kqfd);
    kvfree(changes);
    kvfree(events);

    if (ready == -EINTR) {
        kvfree(pfds);
        return -EINTR;
    }

copyout:
    if (either_copyout(1, fds_addr, pfds, bytes) < 0) {
        kvfree(pfds);
        return -EFAULT;
    }

    kvfree(pfds);
    return ready;
}

/*
 * sys_vfs_poll — poll(2) syscall wrapper.
 *
 * Arguments: a0 = pollfd*, a1 = nfds, a2 = timeout_ms
 */
uint64 sys_vfs_poll(void) {
    uint64 fds_addr;
    int nfds, timeout_ms;

    argaddr(0, &fds_addr);
    argint(1, &nfds);
    argint(2, &timeout_ms);

    return __vfs_poll_impl(fds_addr, nfds, timeout_ms);
}

/*
 * sys_vfs_ppoll — ppoll(2) syscall.
 *
 * Arguments:
 *   a0 = pollfd*
 *   a1 = nfds
 *   a2 = struct timespec* (NULL = infinite wait)
 *   a3 = sigset_t*        (ignored for now)
 *   a4 = sigsetsize        (ignored for now)
 */
uint64 sys_vfs_ppoll(void) {
    uint64 fds_addr;
    int nfds;
    uint64 tmo_p;

    argaddr(0, &fds_addr);
    argint(1, &nfds);
    argaddr(2, &tmo_p);
    /* a3 (sigmask) and a4 (sigsetsize) ignored */

    int timeout_ms;
    if (tmo_p == 0) {
        /* NULL timespec → infinite wait */
        timeout_ms = -1;
    } else {
        struct { int64 tv_sec; int64 tv_nsec; } ts;
        if (either_copyin(&ts, 1, tmo_p, sizeof(ts)) < 0)
            return (uint64)-EFAULT;
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL)
            return (uint64)-EINVAL;
        int64 ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if (ms > (int64)0x7FFFFFFF)
            timeout_ms = 0x7FFFFFFF;
        else
            timeout_ms = (int)ms;
    }

    return __vfs_poll_impl(fds_addr, nfds, timeout_ms);
}

/*
 * sys_pselect6 — pselect6_time64 syscall for musl libc
 *
 * Implements the select/pselect interface by converting fd_set bitmaps
 * into pollfd arrays and reusing the poll infrastructure.
 *
 * Arguments (Linux pselect6_time64 ABI):
 *   a0 = nfds
 *   a1 = readfds   (fd_set __user *, or NULL)
 *   a2 = writefds   (fd_set __user *, or NULL)
 *   a3 = exceptfds  (fd_set __user *, or NULL)
 *   a4 = timeout    (struct __kernel_timespec __user *, or NULL)
 *          { int64 tv_sec; int64 tv_nsec; }
 *   a5 = sig_data   (struct { sigset_t *ss; size_t ss_len; } __user *, or NULL)
 *          — signal mask argument (ignored — xv6 has limited signal support)
 *
 * Returns:
 *   >= 0  number of ready file descriptors
 *   < 0   -errno on error
 */
uint64 sys_pselect6(void) {
    int nfds;
    uint64 readfds_addr, writefds_addr, exceptfds_addr;
    uint64 timeout_addr, sig_addr;

    argint(0, &nfds);
    argaddr(1, &readfds_addr);
    argaddr(2, &writefds_addr);
    argaddr(3, &exceptfds_addr);
    argaddr(4, &timeout_addr);
    argaddr(5, &sig_addr);

    if (nfds < 0 || nfds > NOFILE)
        return -EINVAL;

    /* Parse timeout: NULL means block indefinitely */
    int timeout_ms = -1;
    if (timeout_addr != 0) {
        int64 ts[2]; /* { tv_sec, tv_nsec } */
        if (either_copyin(ts, 1, timeout_addr, sizeof(ts)) < 0)
            return -EFAULT;
        if (ts[0] < 0 || ts[1] < 0)
            return -EINVAL;
        /* Convert to milliseconds, clamping to INT_MAX */
        int64 ms = ts[0] * 1000 + ts[1] / 1000000;
        if (ms > 0x7fffffff)
            ms = 0x7fffffff;
        timeout_ms = (int)ms;
    }

    /* No fds to watch — just sleep */
    if (nfds == 0) {
        if (timeout_ms == 0)
            return 0;
        uint64 start = get_jiffs();
        while (timeout_ms < 0 || (int)(get_jiffs() - start) < timeout_ms) {
            sleep_ms(1);
            if (signal_pending(current))
                return -EINTR;
        }
        return 0;
    }

    /*
     * Copy in the fd_set bitmaps.
     * fd_set is an array of unsigned long (64-bit on riscv64/x86_64).
     * We copy ceil(nfds / 8) bytes = the bits that matter.
     */
    int set_bytes = ((nfds + 7) / 8);
    /* Align to 8-byte boundary for clean word access */
    int set_words = (nfds + 63) / 64;
    int alloc_bytes = set_words * 8;

    uint64 *rfds = NULL, *wfds = NULL, *efds = NULL;

    if (readfds_addr) {
        rfds = kvmalloc(alloc_bytes);
        if (!rfds) return -ENOMEM;
        memset(rfds, 0, alloc_bytes);
        if (either_copyin(rfds, 1, readfds_addr, set_bytes) < 0) {
            kvfree(rfds);
            return -EFAULT;
        }
    }
    if (writefds_addr) {
        wfds = kvmalloc(alloc_bytes);
        if (!wfds) { if (rfds) kvfree(rfds); return -ENOMEM; }
        memset(wfds, 0, alloc_bytes);
        if (either_copyin(wfds, 1, writefds_addr, set_bytes) < 0) {
            if (rfds) kvfree(rfds);
            kvfree(wfds);
            return -EFAULT;
        }
    }
    if (exceptfds_addr) {
        efds = kvmalloc(alloc_bytes);
        if (!efds) { if (rfds) kvfree(rfds); if (wfds) kvfree(wfds); return -ENOMEM; }
        memset(efds, 0, alloc_bytes);
        if (either_copyin(efds, 1, exceptfds_addr, set_bytes) < 0) {
            if (rfds) kvfree(rfds);
            if (wfds) kvfree(wfds);
            kvfree(efds);
            return -EFAULT;
        }
    }

    /*
     * Build a pollfd array from the fd_set bitmaps.
     * Each set fd gets an entry with the appropriate events mask.
     * We need at most 'nfds' entries.
     */
    struct pollfd_k *pfds = kvmalloc(nfds * sizeof(struct pollfd_k));
    if (!pfds) {
        if (rfds) kvfree(rfds);
        if (wfds) kvfree(wfds);
        if (efds) kvfree(efds);
        return -ENOMEM;
    }

    int npfds = 0;
    /* For each fd in [0, nfds), check if any fd_set has it set.
     * Track which pollfd index maps to which fd for result conversion. */
    int *fd_map = kvmalloc(nfds * sizeof(int)); /* fd_map[i] = original fd */
    if (!fd_map) {
        kvfree(pfds);
        if (rfds) kvfree(rfds);
        if (wfds) kvfree(wfds);
        if (efds) kvfree(efds);
        return -ENOMEM;
    }

    for (int fd = 0; fd < nfds; fd++) {
        int word = fd / 64;
        uint64 bit = 1ULL << (fd % 64);
        short events = 0;

        if (rfds && (rfds[word] & bit))
            events |= (POLLIN | POLLRDNORM);
        if (wfds && (wfds[word] & bit))
            events |= (POLLOUT | POLLWRNORM);
        if (efds && (efds[word] & bit))
            events |= POLLPRI;

        if (events) {
            pfds[npfds].fd = fd;
            pfds[npfds].events = events;
            pfds[npfds].revents = 0;
            fd_map[npfds] = fd;
            npfds++;
        }
    }

    /* Perform the actual poll */
    int ready;
    if (npfds == 0) {
        ready = 0;
    } else {
        /* Non-blocking fast path */
        ready = __vfs_poll_scan(pfds, npfds);
        if (ready == 0 && timeout_ms != 0) {
            /* Blocking path: simple sleep+rescan loop */
            uint64 poll_start = get_jiffs();
            for (;;) {
                int sleep_chunk = 10; /* ms */
                if (timeout_ms > 0) {
                    int remaining = timeout_ms - (int)(get_jiffs() - poll_start);
                    if (remaining <= 0)
                        break;
                    if (sleep_chunk > remaining)
                        sleep_chunk = remaining;
                }
                sleep_ms(sleep_chunk);
                if (signal_pending(current)) {
                    ready = -EINTR;
                    break;
                }
                ready = __vfs_poll_scan(pfds, npfds);
                if (ready > 0)
                    break;
            }
        }
    }

    /*
     * Convert poll results back to fd_set bitmaps.
     * Clear all sets first, then set bits for ready fds.
     */
    if (ready >= 0) {
        if (rfds) memset(rfds, 0, alloc_bytes);
        if (wfds) memset(wfds, 0, alloc_bytes);
        if (efds) memset(efds, 0, alloc_bytes);

        int count = 0;
        for (int i = 0; i < npfds; i++) {
            if (pfds[i].revents == 0)
                continue;
            int fd = fd_map[i];
            int word = fd / 64;
            uint64 bit = 1ULL << (fd % 64);
            int got = 0;

            if (rfds && (pfds[i].revents & (POLLIN | POLLRDNORM | POLLHUP | POLLERR))) {
                rfds[word] |= bit;
                got = 1;
            }
            if (wfds && (pfds[i].revents & (POLLOUT | POLLWRNORM | POLLERR))) {
                wfds[word] |= bit;
                got = 1;
            }
            if (efds && (pfds[i].revents & (POLLPRI | POLLNVAL))) {
                efds[word] |= bit;
                got = 1;
            }
            if (got)
                count++;
        }
        ready = count;

        /* Copy results back to user space */
        if (rfds && either_copyout(1, readfds_addr, rfds, set_bytes) < 0)
            ready = -EFAULT;
        if (ready >= 0 && wfds && either_copyout(1, writefds_addr, wfds, set_bytes) < 0)
            ready = -EFAULT;
        if (ready >= 0 && efds && either_copyout(1, exceptfds_addr, efds, set_bytes) < 0)
            ready = -EFAULT;
    }

    kvfree(fd_map);
    kvfree(pfds);
    if (rfds) kvfree(rfds);
    if (wfds) kvfree(wfds);
    if (efds) kvfree(efds);

    return ready;
}

/******************************************************************************
 * Extended Syscalls for musl libc compatibility
 ******************************************************************************/

/*
 * openat(dirfd, path, flags, mode) — open relative to directory fd.
 *
 * When dirfd == AT_FDCWD (-100), behaves like open(path, flags, mode).
 * Other dirfd values are not yet supported (returns -ENOSYS).
 */
uint64 sys_vfs_openat(void) {
    int dirfd;
    argint(0, &dirfd);

    // Shift arguments: openat(dirfd, path, flags, mode)
    // Rewrite a0-a3 so we can reuse sys_vfs_open logic on a1, a2
    // For now, only support AT_FDCWD
    if (dirfd != -100) { // AT_FDCWD = -100
        return -ENOSYS; // TODO: support real dirfd
    }

    // path is in a1, flags in a2, mode in a3
    char path[MAXPATH];
    char name[DIRSIZ + 1];
    int omode;
    int n;

    if (argstr(1, path, MAXPATH) < 0)
        return -EFAULT;
    argint(2, &omode);

    struct vfs_inode *inode = NULL;

    if (omode & O_CREAT) {
        struct vfs_inode *parent = vfs_nameiparent(path, strlen(path), name, DIRSIZ + 1);
        if (IS_ERR(parent))
            return PTR_ERR(parent);
        if (parent == NULL)
            return -ENOENT;

        size_t name_len = strlen(name);
        inode = vfs_create(parent, 0644, name, name_len);
        vfs_iput(parent);

        if (IS_ERR(inode)) {
            if (PTR_ERR(inode) == -EEXIST && !(omode & O_EXCL)) {
                inode = vfs_namei(path, strlen(path));
                if (!IS_ERR_OR_NULL(inode) && S_ISDIR(inode->mode)) {
                    vfs_iput(inode);
                    return -EISDIR;
                }
            } else {
                return PTR_ERR(inode);
            }
        }
    } else {
        int symloop_count = 0;
        do {
            inode = vfs_namei(path, strlen(path));
            if (IS_ERR(inode))
                return PTR_ERR(inode);
            if (inode == NULL)
                return -ENOENT;
            if (!S_ISLNK(inode->mode) || (omode & O_NOFOLLOW))
                break;
            ssize_t link_len = vfs_readlink(inode, path, MAXPATH - 1);
            vfs_iput(inode);
            inode = NULL;
            if (link_len < 0)
                return link_len;
            path[link_len] = '\0';
            symloop_count++;
        } while (symloop_count < 8);
        if (symloop_count >= 8)
            return -ELOOP;
    }

    if (IS_ERR(inode))
        return PTR_ERR(inode);
    if (inode == NULL)
        return -ENOENT;

    if (S_ISDIR(inode->mode) && (omode & (O_WRONLY | O_RDWR))) {
        vfs_iput(inode);
        return -EISDIR;
    }

    struct vfs_file *f = vfs_fileopen(inode, omode);
    vfs_iput(inode);
    if (IS_ERR(f))
        return PTR_ERR(f);
    if (f == NULL)
        return -ENOMEM;

    if (omode & O_TRUNC) {
        truncate(f, 0);
    }

    spin_lock(&current->fdtable->lock);
    n = __vfs_fdalloc(f);
    spin_unlock(&current->fdtable->lock);
    vfs_fput(f);

    if (n < 0)
        return n;

    if (omode & O_CLOEXEC) {
        vfs_fdtable_set_fdflags(current->fdtable, n, FD_CLOEXEC);
    }

    if (n >= 0)
        ACCT_INC(current->thread_group, fs_opens);
    return n;
}

/*
 * User-space iovec structure (matches Linux / musl).
 */
struct __k_iovec {
    uint64 iov_base; // void * in user space
    uint64 iov_len;  // size_t
};

#define UIO_MAXIOV 1024

/*
 * writev(fd, iov, iovcnt) — scatter-gather write.
 */
uint64 sys_vfs_writev(void) {
    int fd, iovcnt;
    uint64 iov_addr;

    argint(0, &fd);
    argaddr(1, &iov_addr);
    argint(2, &iovcnt);

    if (iovcnt <= 0 || iovcnt > UIO_MAXIOV)
        return -EINVAL;

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        return -EBADF;

    struct __k_iovec iovs[8]; // stack-allocated for small iovcnt
    struct __k_iovec *iov = iovs;
    struct __k_iovec *heap_iov = NULL;

    if (iovcnt > 8) {
        heap_iov = kvmalloc(iovcnt * sizeof(struct __k_iovec));
        if (heap_iov == NULL) {
            vfs_fput(f);
            return -ENOMEM;
        }
        iov = heap_iov;
    }

    if (either_copyin(iov, 1, iov_addr, iovcnt * sizeof(struct __k_iovec)) < 0) {
        vfs_fput(f);
        if (heap_iov) kvfree(heap_iov);
        return -EFAULT;
    }

    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0)
            continue;
        ssize_t n = vfs_filewrite(f, (const void *)iov[i].iov_base, iov[i].iov_len, true);
        if (n < 0) {
            if (total > 0) break;
            total = n;
            break;
        }
        total += n;
        if ((uint64)n < iov[i].iov_len)
            break; // Short write
    }

    vfs_fput(f);
    if (heap_iov) kvfree(heap_iov);
    if (total > 0)
        ACCT_ADD(current->thread_group, fs_bytes_written, (uint64)total);
    return total;
}

/*
 * readv(fd, iov, iovcnt) — scatter-gather read.
 */
uint64 sys_vfs_readv(void) {
    int fd, iovcnt;
    uint64 iov_addr;

    argint(0, &fd);
    argaddr(1, &iov_addr);
    argint(2, &iovcnt);

    if (iovcnt <= 0 || iovcnt > UIO_MAXIOV)
        return -EINVAL;

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        return -EBADF;

    struct __k_iovec iovs[8];
    struct __k_iovec *iov = iovs;
    struct __k_iovec *heap_iov = NULL;

    if (iovcnt > 8) {
        heap_iov = kvmalloc(iovcnt * sizeof(struct __k_iovec));
        if (heap_iov == NULL) {
            vfs_fput(f);
            return -ENOMEM;
        }
        iov = heap_iov;
    }

    if (either_copyin(iov, 1, iov_addr, iovcnt * sizeof(struct __k_iovec)) < 0) {
        vfs_fput(f);
        if (heap_iov) kvfree(heap_iov);
        return -EFAULT;
    }

    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0)
            continue;
        ssize_t n = vfs_fileread(f, (void *)iov[i].iov_base, iov[i].iov_len, true);
        if (n < 0) {
            if (total > 0) break;
            total = n;
            break;
        }
        total += n;
        if ((uint64)n < iov[i].iov_len)
            break; // Short read
    }

    vfs_fput(f);
    if (heap_iov) kvfree(heap_iov);
    if (total > 0)
        ACCT_ADD(current->thread_group, fs_bytes_read, (uint64)total);
    return total;
}

/*
 * pread64(fd, buf, count, offset) — read at position without changing file offset.
 *
 * Atomically: save f_pos, seek to offset, read, restore f_pos — all under
 * the file lock so concurrent read/write/lseek can't interleave.
 */
uint64 sys_vfs_pread64(void) {
    int fd, count;
    uint64 buf_addr;
    int64 offset;

    argint(0, &fd);
    argaddr(1, &buf_addr);
    argint(2, &count);
    argint64(3, &offset);

    if (offset < 0)
        return -EINVAL;

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        return -EBADF;

    struct vfs_inode *inode = vfs_inode_deref(&f->inode);
    if (inode == NULL || !S_ISREG(inode->mode)) {
        vfs_fput(f);
        return -ESPIPE;
    }

    mutex_lock(&f->lock);
    loff_t saved = f->f_pos;
    f->f_pos = offset;

    ssize_t ret;
    if (f->ops == NULL || f->ops->read == NULL)
        ret = -EOPNOTSUPP;
    else
        ret = f->ops->read(f, (void *)buf_addr, count, true);

    /* Restore original position (don't advance f_pos) */
    f->f_pos = saved;
    mutex_unlock(&f->lock);

    if (ret > 0)
        ACCT_ADD(current->thread_group, fs_bytes_read, (uint64)ret);
    vfs_fput(f);
    return ret;
}

/*
 * pwrite64(fd, buf, count, offset) — write at position without changing file offset.
 *
 * Atomic: save/set/restore f_pos under single file lock.
 */
uint64 sys_vfs_pwrite64(void) {
    int fd, count;
    uint64 buf_addr;
    int64 offset;

    argint(0, &fd);
    argaddr(1, &buf_addr);
    argint(2, &count);
    argint64(3, &offset);

    if (offset < 0)
        return -EINVAL;

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        return -EBADF;

    struct vfs_inode *inode = vfs_inode_deref(&f->inode);
    if (inode == NULL || !S_ISREG(inode->mode)) {
        vfs_fput(f);
        return -ESPIPE;
    }

    mutex_lock(&f->lock);
    loff_t saved = f->f_pos;
    f->f_pos = offset;

    ssize_t ret;
    if (f->ops == NULL || f->ops->write == NULL)
        ret = -EOPNOTSUPP;
    else
        ret = f->ops->write(f, (const void *)buf_addr, count, true);

    /* Restore original position (don't advance f_pos) */
    f->f_pos = saved;
    mutex_unlock(&f->lock);

    if (ret > 0)
        ACCT_ADD(current->thread_group, fs_bytes_written, (uint64)ret);
    vfs_fput(f);
    return ret;
}

/*
 * fstatat(dirfd, path, statbuf, flags) — stat relative to directory fd.
 *
 * For now, only AT_FDCWD is supported for dirfd.
 * flags: AT_SYMLINK_NOFOLLOW (0x100) — use lstat instead of stat.
 */
uint64 sys_vfs_fstatat(void) {
    int dirfd, flags;
    uint64 stat_addr;
    char path[MAXPATH];

    argint(0, &dirfd);
    if (argstr(1, path, MAXPATH) < 0)
        return -EFAULT;
    argaddr(2, &stat_addr);
    argint(3, &flags);

    if (dirfd != -100) // AT_FDCWD
        return -ENOSYS;

    struct vfs_inode *inode = NULL;

    if (flags & 0x100) { // AT_SYMLINK_NOFOLLOW — don't follow symlinks
        // Use the lstat approach: look up the final component directly
        char name[DIRSIZ + 1];
        int n = strlen(path);
        struct vfs_inode *parent = vfs_nameiparent(path, n, name, DIRSIZ + 1);
        if (IS_ERR(parent))
            return PTR_ERR(parent);
        if (parent == NULL)
            return -ENOENT;

        struct vfs_dentry dentry = {.sb = parent->sb, .parent = parent};
        int ret = vfs_ilookup(parent, &dentry, name, strlen(name));
        if (ret != 0) {
            vfs_iput(parent);
            return ret;
        }
        inode = vfs_get_dentry_inode(&dentry);
        vfs_release_dentry(&dentry);
        vfs_iput(parent);
    } else {
        /* vfs_namei follows all symlinks automatically */
        inode = vfs_namei(path, strlen(path));
    }

    if (IS_ERR(inode))
        return PTR_ERR(inode);
    if (inode == NULL)
        return -ENOENT;

    struct stat st;
    int ret = __vfs_inode_stat(inode, &st);
    vfs_iput(inode);
    if (ret != 0)
        return ret;

    // Copy the 128-byte struct stat directly to userspace.
    // Kernel struct stat layout matches musl's riscv64 layout exactly.
    if (either_copyout(1, stat_addr, &st, sizeof(st)) < 0)
        return -EFAULT;

    return 0;
}

/*
 * pipe2(pipefd[2], flags) — create pipe with flags (O_CLOEXEC, O_NONBLOCK).
 */
uint64 sys_vfs_pipe2(void) {
    uint64 fdarray;
    int flags;
    argaddr(0, &fdarray);
    argint(1, &flags);

    struct vfs_file *rf = NULL, *wf = NULL;
    int ret = vfs_pipealloc(&rf, &wf);
    if (ret != 0)
        return ret;

    spin_lock(&current->fdtable->lock);
    int fd0 = __vfs_fdalloc(rf);
    if (fd0 < 0) {
        spin_unlock(&current->fdtable->lock);
        vfs_fput(rf);
        vfs_fput(wf);
        return fd0;
    }

    int fd1 = __vfs_fdalloc(wf);
    if (fd1 < 0) {
        __vfs_fdfree(fd0);
        spin_unlock(&current->fdtable->lock);
        vfs_fput(rf);
        vfs_fput(wf);
        __vfs_fput_call_rcu(rf);
        return fd1;
    }
    spin_unlock(&current->fdtable->lock);

    struct thread *p = current;
    if (vm_copyout(p->vm, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
        vm_copyout(p->vm, fdarray + sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0) {
        spin_lock(&current->fdtable->lock);
        __vfs_fdfree(fd0);
        __vfs_fdfree(fd1);
        spin_unlock(&current->fdtable->lock);
        vfs_fput(rf);
        vfs_fput(wf);
        __vfs_fput_call_rcu(rf);
        __vfs_fput_call_rcu(wf);
        return -EFAULT;
    }

    vfs_fput(rf);
    vfs_fput(wf);

    if (flags & O_CLOEXEC) {
        vfs_fdtable_set_fdflags(current->fdtable, fd0, FD_CLOEXEC);
        vfs_fdtable_set_fdflags(current->fdtable, fd1, FD_CLOEXEC);
    }

    return 0;
}

/* ===========================================================================
 * Linux-compatible *at() syscall variants for musl libc
 *
 * These accept a dirfd as the first argument. Currently only AT_FDCWD (-100)
 * is supported for dirfd; other values return -EBADF.
 * ===========================================================================
 */

#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif

/**
 * sys_vfs_mkdirat - Create a directory (AT_FDCWD-only)
 * Args: a0=dirfd, a1=path, a2=mode
 */
uint64 sys_vfs_mkdirat(void) {
    int dirfd;
    char path[MAXPATH];
    char name[DIRSIZ + 1];
    int mode;

    argint(0, &dirfd);
    if (dirfd != AT_FDCWD)
        return -EBADF;

    int n = argstr(1, path, MAXPATH);
    argint(2, &mode);
    if (n < 0)
        return -EFAULT;

    struct vfs_inode *parent = vfs_nameiparent(path, n, name, DIRSIZ + 1);
    if (IS_ERR(parent))
        return PTR_ERR(parent);
    if (parent == NULL)
        return -ENOENT;

    struct vfs_inode *dir = vfs_mkdir(parent, (mode_t)mode, name, strlen(name));
    vfs_iput(parent);
    if (IS_ERR(dir))
        return PTR_ERR(dir);
    vfs_iput(dir);
    ACCT_INC(current->thread_group, fs_creates);
    return 0;
}

/**
 * sys_vfs_mknodat - Create a special file (AT_FDCWD-only)
 * Args: a0=dirfd, a1=path, a2=mode, a3=dev
 *
 * musl packs major/minor into a single dev_t. xv6 also uses mkdev().
 */
uint64 sys_vfs_mknodat(void) {
    if (!capable())
        return (uint64)-EPERM;

    int dirfd;
    char path[MAXPATH];
    char name[DIRSIZ + 1];
    int mode;
    uint64 dev;

    argint(0, &dirfd);
    if (dirfd != AT_FDCWD)
        return -EBADF;

    int n = argstr(1, path, MAXPATH);
    argint(2, &mode);
    argaddr(3, &dev);
    if (n < 0)
        return -EFAULT;

    struct vfs_inode *parent = vfs_nameiparent(path, n, name, DIRSIZ + 1);
    if (IS_ERR(parent))
        return PTR_ERR(parent);
    if (parent == NULL)
        return -ENOENT;

    struct vfs_inode *node =
        vfs_mknod(parent, (mode_t)mode, (dev_t)dev, name, strlen(name));
    vfs_iput(parent);
    if (IS_ERR(node))
        return PTR_ERR(node);
    vfs_iput(node);
    return 0;
}

/**
 * sys_vfs_unlinkat - Remove a file or directory (AT_FDCWD-only)
 * Args: a0=dirfd, a1=path, a2=flags (AT_REMOVEDIR for rmdir)
 */
uint64 sys_vfs_unlinkat(void) {
    int dirfd, flags;
    char path[MAXPATH];
    char name[DIRSIZ + 1];

    argint(0, &dirfd);
    if (dirfd != AT_FDCWD)
        return -EBADF;

    int n = argstr(1, path, MAXPATH);
    argint(2, &flags);
    if (n < 0)
        return -EFAULT;

    struct vfs_inode *parent = vfs_nameiparent(path, n, name, DIRSIZ + 1);
    if (IS_ERR(parent))
        return PTR_ERR(parent);
    if (parent == NULL)
        return -ENOENT;

    /* vfs_unlink handles both files and directories internally.
     * AT_REMOVEDIR is a hint that we expect a directory, but
     * vfs_unlink checks the type and calls rmdir when appropriate. */
    (void)flags;  /* AT_REMOVEDIR handled by vfs_unlink */
    int ret = vfs_unlink(parent, name, strlen(name));
    vfs_iput(parent);
    if (ret == 0)
        ACCT_INC(current->thread_group, fs_deletes);
    return ret;
}

/**
 * sys_vfs_linkat - Create a hard link (AT_FDCWD-only)
 * Args: a0=olddirfd, a1=oldpath, a2=newdirfd, a3=newpath, a4=flags
 */
uint64 sys_vfs_linkat(void) {
    int olddirfd, newdirfd, flags;
    char old[MAXPATH], new[MAXPATH];
    char name[DIRSIZ + 1];

    argint(0, &olddirfd);
    argint(2, &newdirfd);
    argint(4, &flags);
    if (olddirfd != AT_FDCWD || newdirfd != AT_FDCWD)
        return -EBADF;

    int n1 = argstr(1, old, MAXPATH);
    int n2 = argstr(3, new, MAXPATH);
    if (n1 < 0 || n2 < 0)
        return -EFAULT;

    struct vfs_inode *src = vfs_namei(old, n1);
    if (IS_ERR(src))
        return PTR_ERR(src);
    if (src == NULL)
        return -ENOENT;
    if (S_ISDIR(src->mode)) {
        vfs_iput(src);
        return -EPERM;
    }

    struct vfs_inode *parent = vfs_nameiparent(new, n2, name, DIRSIZ + 1);
    if (IS_ERR(parent)) {
        vfs_iput(src);
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        vfs_iput(src);
        return -ENOENT;
    }

    struct vfs_dentry old_dentry = {
        .sb = src->sb,
        .ino = src->ino,
        .name = NULL,
        .name_len = 0,
    };

    int ret = vfs_link(&old_dentry, parent, name, strlen(name));
    vfs_iput(src);
    vfs_iput(parent);
    if (ret == 0)
        ACCT_INC(current->thread_group, fs_links);
    return ret;
}

/**
 * sys_vfs_symlinkat - Create a symbolic link (AT_FDCWD-only)
 * Args: a0=target, a1=newdirfd, a2=linkpath
 *
 * Note: Linux symlinkat has (target, newdirfd, linkpath) ordering.
 */
uint64 sys_vfs_symlinkat(void) {
    int newdirfd;
    char target[MAXPATH], linkpath[MAXPATH];
    char name[DIRSIZ + 1];

    argint(1, &newdirfd);
    if (newdirfd != AT_FDCWD)
        return -EBADF;

    int n1 = argstr(0, target, MAXPATH);
    int n2 = argstr(2, linkpath, MAXPATH);
    if (n1 < 0 || n2 < 0)
        return -EFAULT;

    /* Convert target to absolute path if relative */
    char abs_target[MAXPATH];
    int abs_len = vfs_make_absolute_path(target, n1, abs_target);
    if (abs_len < 0)
        return abs_len;

    struct vfs_inode *parent = vfs_nameiparent(linkpath, n2, name, DIRSIZ + 1);
    if (IS_ERR(parent))
        return PTR_ERR(parent);
    if (parent == NULL)
        return -ENOENT;

    struct vfs_inode *sym =
        vfs_symlink(parent, 0777, name, strlen(name), abs_target, abs_len);
    vfs_iput(parent);
    if (IS_ERR(sym))
        return PTR_ERR(sym);
    vfs_iput(sym);
    ACCT_INC(current->thread_group, fs_links);
    return 0;
}

/**
 * sys_vfs_readlinkat - Read a symbolic link (AT_FDCWD-only)
 * Args: a0=dirfd, a1=path, a2=buf, a3=bufsiz
 */
uint64 sys_vfs_readlinkat(void) {
    int dirfd;
    char path[MAXPATH];
    char name[DIRSIZ + 1];
    uint64 buf_addr;
    int bufsz;

    argint(0, &dirfd);
    if (dirfd != AT_FDCWD)
        return -EBADF;

    int n = argstr(1, path, MAXPATH);
    argaddr(2, &buf_addr);
    argint(3, &bufsz);
    if (n < 0)
        return -EFAULT;
    if (bufsz <= 0)
        return -EINVAL;

    struct vfs_inode *parent = vfs_nameiparent(path, n, name, DIRSIZ + 1);
    if (IS_ERR(parent))
        return PTR_ERR(parent);
    if (parent == NULL)
        return -ENOENT;

    struct vfs_dentry dentry = {.sb = parent->sb, .parent = parent};
    int ret = vfs_ilookup(parent, &dentry, name, strlen(name));
    if (ret != 0) {
        vfs_iput(parent);
        return ret;
    }

    struct vfs_inode *inode = vfs_get_dentry_inode(&dentry);
    vfs_release_dentry(&dentry);
    vfs_iput(parent);
    if (IS_ERR(inode))
        return PTR_ERR(inode);
    if (inode == NULL)
        return -ENOENT;

    char *kbuf = kvmalloc(bufsz);
    if (kbuf == NULL) {
        vfs_iput(inode);
        return -ENOMEM;
    }

    ssize_t len = vfs_readlink(inode, kbuf, bufsz);
    vfs_iput(inode);
    if (len < 0) {
        kvfree(kbuf);
        return len;
    }

    if (either_copyout(1, buf_addr, kbuf, len) < 0) {
        kvfree(kbuf);
        return -EFAULT;
    }
    kvfree(kbuf);
    return len;
}

/**
 * sys_vfs_renameat - Rename a file (AT_FDCWD-only)
 * Args: a0=olddirfd, a1=oldpath, a2=newdirfd, a3=newpath
 */
uint64 sys_vfs_renameat(void) {
    int olddirfd, newdirfd;
    char oldpath[MAXPATH], newpath[MAXPATH];
    char oldname[DIRSIZ + 1], newname[DIRSIZ + 1];

    argint(0, &olddirfd);
    argint(2, &newdirfd);
    if (olddirfd != AT_FDCWD || newdirfd != AT_FDCWD)
        return -EBADF;

    int n1 = argstr(1, oldpath, MAXPATH);
    int n2 = argstr(3, newpath, MAXPATH);
    if (n1 < 0 || n2 < 0)
        return -EFAULT;

    struct vfs_inode *old_parent =
        vfs_nameiparent(oldpath, n1, oldname, DIRSIZ + 1);
    if (IS_ERR(old_parent))
        return PTR_ERR(old_parent);
    if (old_parent == NULL)
        return -ENOENT;

    struct vfs_inode *new_parent =
        vfs_nameiparent(newpath, n2, newname, DIRSIZ + 1);
    if (IS_ERR(new_parent)) {
        vfs_iput(old_parent);
        return PTR_ERR(new_parent);
    }
    if (new_parent == NULL) {
        vfs_iput(old_parent);
        return -ENOENT;
    }

    struct vfs_dentry old_dentry = {.sb = old_parent->sb, .parent = old_parent};
    int ret = vfs_ilookup(old_parent, &old_dentry, oldname, strlen(oldname));
    if (ret != 0) {
        vfs_iput(old_parent);
        vfs_iput(new_parent);
        return ret;
    }

    ret = vfs_move(old_parent, &old_dentry, new_parent, newname, strlen(newname));
    vfs_release_dentry(&old_dentry);
    vfs_iput(old_parent);
    vfs_iput(new_parent);
    if (ret == 0)
        ACCT_INC(current->thread_group, fs_renames);
    return ret;
}

/**
 * sys_vfs_faccessat - Check file accessibility (AT_FDCWD-only)
 * Args: a0=dirfd, a1=path, a2=mode, a3=flags
 */
uint64 sys_vfs_faccessat(void) {
    int dirfd, mode, flags;
    char path[MAXPATH];

    argint(0, &dirfd);
    if (dirfd != AT_FDCWD)
        return -EBADF;

    int n = argstr(1, path, MAXPATH);
    argint(2, &mode);
    argint(3, &flags);
    if (n < 0)
        return -EFAULT;

    struct vfs_inode *inode = vfs_namei(path, n);
    if (IS_ERR(inode))
        return PTR_ERR(inode);
    if (inode == NULL)
        return -ENOENT;

    if (mode != 0) {
        mode_t perm = inode->mode;
        if ((mode & 4) && !(perm & (S_IRUSR | S_IRGRP | S_IROTH))) {
            vfs_iput(inode);
            return -EACCES;
        }
        if ((mode & 2) && !(perm & (S_IWUSR | S_IWGRP | S_IWOTH))) {
            vfs_iput(inode);
            return -EACCES;
        }
        if ((mode & 1) && !(perm & (S_IXUSR | S_IXGRP | S_IXOTH))) {
            vfs_iput(inode);
            return -EACCES;
        }
    }

    vfs_iput(inode);
    return 0;
}

/**
 * sys_vfs_dup3 - dup2 with flags (O_CLOEXEC)
 * Args: a0=oldfd, a1=newfd, a2=flags
 */
uint64 sys_vfs_dup3(void) {
    int oldfd, newfd, flags;
    argint(0, &oldfd);
    argint(1, &newfd);
    argint(2, &flags);

    if (oldfd == newfd)
        return -EINVAL;  /* Linux dup3 behavior: EINVAL if oldfd == newfd */
    if (newfd < 0 || newfd >= NOFILE)
        return -EBADF;

    struct vfs_file *f = __vfs_argfd(oldfd);
    if (f == NULL)
        return -EBADF;

    spin_lock(&current->fdtable->lock);
    struct vfs_file *old_newfd = __vfs_fdfree(newfd);
    int ret = vfs_fdtable_alloc_fd_from(current->fdtable, f, newfd);
    spin_unlock(&current->fdtable->lock);

    if (old_newfd)
        __vfs_fput_call_rcu(old_newfd);
    vfs_fput(f);

    if (ret >= 0 && (flags & O_CLOEXEC))
        vfs_fdtable_set_fdflags(current->fdtable, ret, FD_CLOEXEC);

    return ret;
}

/* ========================================================================== */
/* sendfile(out_fd, in_fd, offset_ptr, count) → bytes / -errno                */
/* ========================================================================== */

/*
 * sys_sendfile(out_fd, in_fd, offset_ptr, count) → bytes_written / -errno
 *
 * Copies data from in_fd (regular file) to out_fd (socket/pipe/file)
 * entirely in kernel space, avoiding user-space bounce buffers.
 *
 * If offset_ptr is non-NULL, reads from *offset_ptr and updates it on
 * return; the in_fd file position is unchanged.
 * If offset_ptr is NULL, reads from (and updates) the in_fd file position.
 */
uint64 sys_sendfile(void)
{
    int out_fd, in_fd;
    uint64 u_offset;
    int64 count;
    argint(0, &out_fd);
    argint(1, &in_fd);
    argaddr(2, &u_offset);
    argint64(3, &count);

    if (count < 0)
        return (uint64)-EINVAL;
    if (count == 0)
        return 0;

    struct vfs_file *in_f = __vfs_argfd(in_fd);
    if (in_f == NULL)
        return (uint64)-EBADF;

    struct vfs_file *out_f = __vfs_argfd(out_fd);
    if (out_f == NULL) {
        vfs_fput(in_f);
        return (uint64)-EBADF;
    }

    /* Determine if an explicit offset was supplied */
    bool use_offset = (u_offset != 0);
    loff_t offset = 0;
    if (use_offset) {
        if (vm_copyin(current->vm, &offset, u_offset, sizeof(offset)) < 0) {
            vfs_fput(in_f);
            vfs_fput(out_f);
            return (uint64)-EFAULT;
        }
        if (offset < 0) {
            vfs_fput(in_f);
            vfs_fput(out_f);
            return (uint64)-EINVAL;
        }
    }

    /*
     * Transfer loop — use a 4 KiB kernel buffer (one page).
     * Each iteration:
     *   1. Optionally seek in_f to the requested offset
     *   2. Read into kernel buf via vfs_fileread(user=false)
     *   3. Write from kernel buf via vfs_filewrite(user=false)
     */
    char buf[4096];
    ssize_t total = 0;

    while (total < count) {
        size_t chunk = (size_t)(count - total);
        if (chunk > sizeof(buf))
            chunk = sizeof(buf);

        /*
         * If an explicit offset was provided, temporarily set f_pos
         * so that vfs_fileread reads from the right place.
         * vfs_fileread will advance f_pos; we capture the new value
         * and restore the original afterwards.
         */
        loff_t saved_pos = 0;
        if (use_offset) {
            mutex_lock(&in_f->lock);
            saved_pos = in_f->f_pos;
            in_f->f_pos = offset;
            mutex_unlock(&in_f->lock);
        }

        ssize_t nr = vfs_fileread(in_f, buf, chunk, false);

        if (use_offset) {
            mutex_lock(&in_f->lock);
            offset = in_f->f_pos;   /* capture newly advanced pos */
            in_f->f_pos = saved_pos; /* restore original */
            mutex_unlock(&in_f->lock);
        }

        if (nr <= 0)
            break;   /* EOF or error */

        ssize_t nw = vfs_filewrite(out_f, buf, (size_t)nr, false);
        if (nw <= 0) {
            if (total == 0)
                total = nw;  /* propagate error if nothing sent yet */
            break;
        }
        total += nw;
        if (nw < nr)
            break;   /* short write — back-pressure */
    }

    /* Write back updated offset if explicit offset was provided */
    if (use_offset && total > 0) {
        vm_copyout(current->vm, u_offset, &offset, sizeof(offset));
    }

    vfs_fput(in_f);
    vfs_fput(out_f);

    return (uint64)total;
}

/******************************************************************************
 * File Ownership and Permission Syscalls (chown/chmod/umask)
 ******************************************************************************/

/**
 * sys_vfs_fchmod - change file mode bits
 * fchmod(int fd, mode_t mode)
 */
uint64 sys_vfs_fchmod(void) {
    int fd;
    int mode;
    argint(0, &fd);
    argint(1, &mode);

    struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, fd);
    if (f == NULL)
        return (uint64)-EBADF;

    struct vfs_inode *inode = vfs_inode_deref(&f->inode);
    if (inode == NULL) {
        vfs_fput(f);
        return (uint64)-EBADF;
    }

    /* Only owner or root may chmod */
    if (current_euid() != 0 && current_euid() != inode->uid) {
        vfs_fput(f);
        return (uint64)-EPERM;
    }

    vfs_ilock(inode);
    /* Preserve file type bits, update permission bits */
    inode->mode = (inode->mode & S_IFMT) | (mode & ~S_IFMT);
    inode->dirty = 1;
    vfs_iunlock(inode);

    vfs_fput(f);
    return 0;
}

/**
 * sys_vfs_fchmodat - change file mode bits relative to directory fd
 * fchmodat(int dirfd, const char *path, mode_t mode, int flags)
 */
uint64 sys_vfs_fchmodat(void) {
    /* dirfd ignored (AT_FDCWD assumed) — matches current *at() pattern */
    char path[MAXPATH];
    int mode, flags;
    argint(1, (int *)path); /* actually argstr */
    int n = argstr(1, path, MAXPATH);
    argint(2, &mode);
    argint(3, &flags);
    if (n < 0)
        return (uint64)-EFAULT;

    struct vfs_inode *inode = vfs_namei(path, strlen(path));
    if (IS_ERR(inode))
        return PTR_ERR(inode);
    if (inode == NULL)
        return (uint64)-ENOENT;

    if (current_euid() != 0 && current_euid() != inode->uid) {
        vfs_iput(inode);
        return (uint64)-EPERM;
    }

    vfs_ilock(inode);
    inode->mode = (inode->mode & S_IFMT) | (mode & ~S_IFMT);
    inode->dirty = 1;
    vfs_iunlock(inode);

    vfs_iput(inode);
    return 0;
}

/**
 * sys_vfs_fchown - change file ownership
 * fchown(int fd, uid_t owner, gid_t group)
 */
uint64 sys_vfs_fchown(void) {
    int fd, owner, group;
    argint(0, &fd);
    argint(1, &owner);
    argint(2, &group);

    struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, fd);
    if (f == NULL)
        return (uint64)-EBADF;

    struct vfs_inode *inode = vfs_inode_deref(&f->inode);
    if (inode == NULL) {
        vfs_fput(f);
        return (uint64)-EBADF;
    }

    /* Only root can change ownership */
    if (current_euid() != 0) {
        /* Non-root can only change group to one they belong to,
         * and only if they own the file */
        if (owner != -1 && (uint32)owner != inode->uid) {
            vfs_fput(f);
            return (uint64)-EPERM;
        }
        if (current_euid() != inode->uid) {
            vfs_fput(f);
            return (uint64)-EPERM;
        }
        if (group != -1 && !current_in_group((uint32)group)) {
            vfs_fput(f);
            return (uint64)-EPERM;
        }
    }

    vfs_ilock(inode);
    if (owner != -1)
        inode->uid = (uint32)owner;
    if (group != -1)
        inode->gid = (uint32)group;
    /* Clear setuid/setgid bits on chown (POSIX requirement) */
    if (owner != -1)
        inode->mode &= ~(S_ISUID | S_ISGID);
    inode->dirty = 1;
    vfs_iunlock(inode);

    vfs_fput(f);
    return 0;
}

/**
 * sys_vfs_fchownat - change file ownership relative to directory fd
 * fchownat(int dirfd, const char *path, uid_t owner, gid_t group, int flags)
 */
uint64 sys_vfs_fchownat(void) {
    char path[MAXPATH];
    int owner, group, flags;
    int n = argstr(1, path, MAXPATH);
    argint(2, &owner);
    argint(3, &group);
    argint(4, &flags);
    if (n < 0)
        return (uint64)-EFAULT;

    struct vfs_inode *inode = vfs_namei(path, strlen(path));
    if (IS_ERR(inode))
        return PTR_ERR(inode);
    if (inode == NULL)
        return (uint64)-ENOENT;

    if (current_euid() != 0) {
        if (owner != -1 && (uint32)owner != inode->uid) {
            vfs_iput(inode);
            return (uint64)-EPERM;
        }
        if (current_euid() != inode->uid) {
            vfs_iput(inode);
            return (uint64)-EPERM;
        }
        if (group != -1 && !current_in_group((uint32)group)) {
            vfs_iput(inode);
            return (uint64)-EPERM;
        }
    }

    vfs_ilock(inode);
    if (owner != -1)
        inode->uid = (uint32)owner;
    if (group != -1)
        inode->gid = (uint32)group;
    if (owner != -1)
        inode->mode &= ~(S_ISUID | S_ISGID);
    inode->dirty = 1;
    vfs_iunlock(inode);

    vfs_iput(inode);
    return 0;
}

/**
 * sys_umask - set file creation mask
 * umask(mode_t mask)
 * Returns the previous umask value.
 */
uint64 sys_umask(void) {
    int mask;
    argint(0, &mask);
    struct thread_group *tg = current->thread_group;
    mode_t old = tg->umask;
    tg->umask = (mode_t)(mask & 0777);
    return (uint64)old;
}
