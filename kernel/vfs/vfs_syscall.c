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
#include "vfs/file_lock.h"
#include "vfs/pipe.h"
#include "vfs/unix_socket.h"
#include "vfs/fcntl.h"
#include "vfs/stat.h"
#include "vfs/xv6fs/ondisk.h" // for DIRSIZ
#include "proc/cred.h"
#include <mm/vm.h>
#include <mm/pcache.h>
#include "printf.h"
#include "timer/goldfish_rtc.h"
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
#include "arch_thread.h"
#include "kqueue.h"
#include "kqueue_types.h"
#include "kstats.h"
#include "cmdline.h"

#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE 1
#endif
#ifndef FALLOC_FL_PUNCH_HOLE
#define FALLOC_FL_PUNCH_HOLE 2
#endif
#ifndef AT_FDCWD
#define AT_FDCWD -100
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif
#ifndef SYNC_FILE_RANGE_WAIT_BEFORE
#define SYNC_FILE_RANGE_WAIT_BEFORE 1
#endif
#ifndef SYNC_FILE_RANGE_WRITE
#define SYNC_FILE_RANGE_WRITE 2
#endif
#ifndef SYNC_FILE_RANGE_WAIT_AFTER
#define SYNC_FILE_RANGE_WAIT_AFTER 4
#endif

#define LINUX_IOCTL_NCCS 19

struct linux_ioctl_termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_line;
    cc_t c_cc[LINUX_IOCTL_NCCS];
};

_Static_assert(sizeof(struct linux_ioctl_termios) == 36,
               "x86_64 TCGETS termios ABI size must match Linux");

static void termios_to_linux_ioctl(const struct termios *src,
                                   struct linux_ioctl_termios *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->c_iflag = src->c_iflag;
    dst->c_oflag = src->c_oflag;
    dst->c_cflag = src->c_cflag;
    dst->c_lflag = src->c_lflag;
    dst->c_line = src->c_line;
    for (int i = 0; i < LINUX_IOCTL_NCCS; i++)
        dst->c_cc[i] = src->c_cc[i];
}

static void linux_ioctl_to_termios(const struct linux_ioctl_termios *src,
                                   struct termios *dst)
{
    dst->c_iflag = src->c_iflag;
    dst->c_oflag = src->c_oflag;
    dst->c_cflag = src->c_cflag;
    dst->c_lflag = src->c_lflag;
    dst->c_line = src->c_line;
    for (int i = 0; i < LINUX_IOCTL_NCCS; i++)
        dst->c_cc[i] = src->c_cc[i];
}

// Forward declaration for syscall argument helpers
void argint(int n, int *ip);
void argint64(int n, int64 *ip);
void argaddr(int n, uint64 *ip);
int argstr(int n, char *buf, int max);
uint64 sys_vfs_unlinkat(void);

static int webkit_vfs_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("webkit_vfs_trace", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static int webkit_vfs_trace_process(void)
{
    return current != NULL &&
        (strncmp(current->name, "MiniBrowser", 11) == 0 ||
         strncmp(current->name, "WebKit", 6) == 0);
}

static int webkit_readlink_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("webkit_readlink_trace", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static int webkit_poll_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("webkit_poll_trace", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static int webkit_poll_summary_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("webkit_poll_summary", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static int timespec_to_timeout_ms_ceil(int64 tv_sec, int64 tv_nsec,
                                       int *timeout_ms)
{
    if (tv_sec < 0 || tv_nsec < 0 || tv_nsec >= 1000000000LL)
        return -EINVAL;

    if (tv_sec > 0x7fffffffLL / 1000) {
        *timeout_ms = 0x7fffffff;
        return 0;
    }

    int64 ms = tv_sec * 1000;
    if (tv_nsec != 0)
        ms += (tv_nsec + 999999LL) / 1000000LL;
    if (ms > 0x7fffffffLL)
        ms = 0x7fffffffLL;

    *timeout_ms = (int)ms;
    return 0;
}

#define WEBKIT_VFS_TRACE(fmt, ...)                                            \
    do {                                                                      \
        if (webkit_vfs_trace_enabled() && webkit_vfs_trace_process())         \
            printf("webkit-vfs: " fmt, ##__VA_ARGS__);                       \
    } while (0)

#define WEBKIT_READLINK_TRACE(fmt, ...)                                       \
    do {                                                                      \
        if ((webkit_vfs_trace_enabled() || webkit_readlink_trace_enabled()) && \
            webkit_vfs_trace_process())                                       \
            printf("webkit-readlink: " fmt, ##__VA_ARGS__);                  \
    } while (0)

#define SYSCALL_PROFILE_BEGIN(call_ctr)                                     \
    uint64 __sys_start = r_time();                                          \
    __atomic_add_fetch(&(call_ctr), 1, __ATOMIC_RELAXED)

#define SYSCALL_PROFILE_RETURN(ret_expr, tick_ctr)                          \
    do {                                                                    \
        uint64 __sys_ret = (uint64)(ret_expr);                              \
        __atomic_add_fetch(&(tick_ctr), r_time() - __sys_start,             \
                           __ATOMIC_RELAXED);                               \
        return __sys_ret;                                                   \
    } while (0)

static int __vfs_argpath(int narg, char **path_out, int *len_out) {
    uint64 addr;
    char *path = kvmalloc(VFS_USER_PATH_MAX);
    if (path == NULL)
        return -ENOMEM;

    argaddr(narg, &addr);
    int ret = vm_copyinstr(current->vm, path, addr, VFS_USER_PATH_MAX);
    if (ret < 0) {
        kvfree(path);
        return ret;
    }

    int len = strlen(path);
    *path_out = path;
    if (len_out)
        *len_out = len;
    return 0;
}

static char *__vfs_alloc_pathbuf(void) {
    return kvmalloc(VFS_USER_PATH_MAX);
}

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
 * __vfs_resolve_dirfd - Convert a dirfd to a starting directory inode.
 *
 * For AT_FDCWD (-100), returns NULL (meaning "use cwd" in vfs_namei_at).
 * For a real fd, validates it's a directory and returns its inode with
 * an extra reference held. Caller must vfs_iput() the returned inode.
 *
 * @dirfd: Directory file descriptor or AT_FDCWD
 * @dir_out: On success, set to the directory inode (or NULL for cwd)
 *
 * Returns: 0 on success, negative errno on failure
 */
static int __vfs_resolve_dirfd(int dirfd, struct vfs_inode **dir_out) {
    *dir_out = NULL;
    if (dirfd == -100) /* AT_FDCWD */
        return 0;

    struct vfs_file *f = __vfs_argfd(dirfd);
    if (f == NULL)
        return -EBADF;

    struct vfs_inode *ip = f->inode.inode;
    if (ip == NULL || !S_ISDIR(ip->mode)) {
        vfs_fput(f);
        return -ENOTDIR;
    }

    vfs_idup(ip);
    vfs_fput(f);
    *dir_out = ip;
    return 0;
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

static void __vfs_finish_close_file(struct vfs_file *f)
{
    if (f == NULL)
        return;

    vfs_file_maybe_last_fd_close(f);

    /*
     * Keep internal close paths in step with close(2): AF_UNIX peers need
     * hangup/read-EOF publication before the descriptor disappears from
     * Linux-style bulk close helpers such as close_range().
     */
    if ((f->ops == &unix_socket_file_ops ||
         (f->ops != NULL && f->ops->early_release_on_close)) &&
        f->ops->release != NULL &&
        f->private_data != NULL &&
        __atomic_load_n(&f->ref_count, __ATOMIC_ACQUIRE) == 1) {
        f->ops->release(vfs_inode_deref(&f->inode), f);
    }

    vfs_file_lock_release(f, current->tgid);
    __vfs_fput_call_rcu(f);
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
    if (f != NULL) {
        __vfs_finish_close_file(f);
    }
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
        vfs_file_lock_release(old_newfd, current->tgid);
        vfs_file_maybe_last_fd_close(old_newfd);
        __vfs_fput_call_rcu(old_newfd);
    }
    vfs_fput(f);
    return ret;
}

uint64 sys_vfs_read(void) {
    SYSCALL_PROFILE_BEGIN(g_sys_read_calls);
    int fd, n;
    uint64 p;

    argint(0, &fd);
    argaddr(1, &p);
    argint(2, &n);

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL) {
        SYSCALL_PROFILE_RETURN(-EBADF, g_sys_read_ticks);
    }

    loff_t pos_before = f->f_pos;
    const char *kind = "custom";
    uint32 mode = 0;
    uint64 ino = 0;
    uint32 cdev = 0;
    const char *fs_name = "(none)";
    struct vfs_inode *inode = vfs_inode_deref(&f->inode);
    if (inode != NULL) {
        mode = inode->mode;
        ino = inode->ino;
        cdev = inode->cdev;
        if (S_ISREG(mode))
            kind = "reg";
        else if (S_ISDIR(mode))
            kind = "dir";
        else if (S_ISCHR(mode))
            kind = "chr";
        else if (S_ISBLK(mode))
            kind = "blk";
        else if (S_ISLNK(mode))
            kind = "lnk";
        else if (S_ISSOCK(mode))
            kind = "sock";
        else if (S_ISFIFO(mode))
            kind = "fifo";
        if (inode->sb != NULL && inode->sb->fs_type != NULL &&
            inode->sb->fs_type->name != NULL)
            fs_name = inode->sb->fs_type->name;
    }

    if (webkit_vfs_trace_enabled() && webkit_vfs_trace_process()) {
        if (inode == NULL) {
            printf("webkit-vfs: read-enter pid=%d name=%s fd=%d kind=%s "
                   "count=%d state=%p ops=%p private=%p flags=0x%x\n",
                   current->pid, current->name, fd, kind, n,
                   (void *)f->f_pos, f->ops, f->private_data, f->f_flags);
        } else {
            printf("webkit-vfs: read-enter pid=%d name=%s fd=%d kind=%s "
                   "count=%d pos=%lld mode=0x%x ino=%lu fs=%s cdev=%u:%u "
                   "ops=%p private=%p flags=0x%x\n",
                   current->pid, current->name, fd, kind, n, pos_before, mode,
                   ino, fs_name, major(cdev), minor(cdev), f->ops,
                   f->private_data, f->f_flags);
        }
    }

    ssize_t ret = vfs_fileread(f, (void *)p, n, true);
    if (webkit_vfs_trace_enabled() && webkit_vfs_trace_process()) {
        if (inode == NULL) {
            printf("webkit-vfs: read-exit pid=%d name=%s fd=%d kind=%s "
                   "count=%d ret=%ld state=%p ops=%p private=%p flags=0x%x\n",
                   current->pid, current->name, fd, kind, n, ret,
                   (void *)f->f_pos, f->ops, f->private_data, f->f_flags);
        } else {
            printf("webkit-vfs: read-exit pid=%d name=%s fd=%d kind=%s "
                   "count=%d ret=%ld pos=%lld->%lld mode=0x%x ino=%lu "
                   "fs=%s cdev=%u:%u ops=%p private=%p flags=0x%x\n",
                   current->pid, current->name, fd, kind, n, ret, pos_before,
                   f->f_pos, mode, ino, fs_name, major(cdev), minor(cdev),
                   f->ops, f->private_data, f->f_flags);
        }
    }
    vfs_fput(f);
    if (ret > 0)
        ACCT_ADD(current->thread_group, fs_bytes_read, (uint64)ret);
    SYSCALL_PROFILE_RETURN(ret, g_sys_read_ticks);
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

    __vfs_finish_close_file(f);
    ACCT_INC(current->thread_group, fs_closes);
    return 0;
}

uint64 sys_vfs_fstat(void) {
    SYSCALL_PROFILE_BEGIN(g_sys_fstat_calls);
    int fd;
    uint64 st;

    argint(0, &fd);
    argaddr(1, &st);

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL) {
        SYSCALL_PROFILE_RETURN(-EBADF, g_sys_fstat_ticks);
    }

    struct stat kst;
    int ret = vfs_filestat(f, &kst);
    if (ret != 0) {
        vfs_fput(f); // remove the reference from __vfs_argfd
        SYSCALL_PROFILE_RETURN(ret, g_sys_fstat_ticks);
    }

    if (vm_copyout(current->vm, st, (char *)&kst, sizeof(kst)) < 0) {
        vfs_fput(f); // remove the reference from __vfs_argfd
        SYSCALL_PROFILE_RETURN(-EFAULT, g_sys_fstat_ticks);
    }

    vfs_fput(f); // remove the reference from __vfs_argfd
    SYSCALL_PROFILE_RETURN(0, g_sys_fstat_ticks);
}

uint64 sys_vfs_lseek(void) {
    SYSCALL_PROFILE_BEGIN(g_sys_lseek_calls);
    int fd, whence;
    int64 offset;
    argint(0, &fd);
    argint64(1, &offset);
    argint(2, &whence);

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL) {
        SYSCALL_PROFILE_RETURN(-EBADF, g_sys_lseek_ticks);
    }

    loff_t ret = vfs_filelseek(f, offset, whence);
    vfs_fput(f);
    SYSCALL_PROFILE_RETURN(ret, g_sys_lseek_ticks);
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

static int __fallocate_zero_range(struct vfs_file *f, loff_t offset,
                                  loff_t len, int keep_size) {
    static const char zeros[512];
    struct vfs_inode *inode = vfs_inode_deref(&f->inode);
    loff_t saved_pos;
    loff_t old_size;
    loff_t end = offset + len;
    loff_t pos = offset;
    int ret = 0;

    if (inode == NULL)
        return -EINVAL;
    if (f->ops == NULL || f->ops->write == NULL)
        return -EOPNOTSUPP;

    mutex_lock(&f->lock);
    saved_pos = f->f_pos;
    old_size = inode->size;

    while (pos < end) {
        size_t chunk = sizeof(zeros);
        if ((loff_t)chunk > end - pos)
            chunk = (size_t)(end - pos);

        f->f_pos = pos;
        ssize_t written = f->ops->write(f, zeros, chunk, false);
        if (written < 0) {
            ret = (int)written;
            break;
        }
        if (written == 0) {
            ret = -ENOSPC;
            break;
        }

        pos += written;
    }

    f->f_pos = saved_pos;
    mutex_unlock(&f->lock);

    if (keep_size && inode->size != old_size) {
        int tr = truncate(f, old_size);
        if (ret == 0)
            ret = tr;
    }

    return ret;
}

uint64 sys_fallocate(void) {
    int fd, mode;
    int64 offset, len;

    argint(0, &fd);
    argint(1, &mode);
    argint64(2, &offset);
    argint64(3, &len);

    if (offset < 0 || len <= 0)
        return -EINVAL;
    int64 max_off = (int64)(((uint64)-1) >> 1);
    if (offset > max_off - len)
        return -EFBIG;

    const int supported = FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE;
    if (mode & ~supported)
        return -EOPNOTSUPP;
    if ((mode & FALLOC_FL_PUNCH_HOLE) &&
        !(mode & FALLOC_FL_KEEP_SIZE))
        return -EOPNOTSUPP;

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        return -EBADF;

    struct vfs_inode *inode = vfs_inode_deref(&f->inode);
    if (inode == NULL || !S_ISREG(inode->mode)) {
        vfs_fput(f);
        return inode != NULL && S_ISDIR(inode->mode) ? -EISDIR : -EINVAL;
    }

    if ((f->f_flags & O_ACCMODE) == O_RDONLY) {
        vfs_fput(f);
        return -EBADF;
    }

    loff_t end = offset + len;
    int keep_size = (mode & FALLOC_FL_KEEP_SIZE) != 0;
    int ret = 0;

    if (f->f_is_memfd) {
        if (f->f_seals & F_SEAL_WRITE) {
            vfs_fput(f);
            return -EPERM;
        }
        if (!keep_size && end > inode->size &&
            (f->f_seals & F_SEAL_GROW)) {
            vfs_fput(f);
            return -EPERM;
        }
    }

    if (mode & FALLOC_FL_PUNCH_HOLE) {
        loff_t size = inode->size;
        if (offset < size) {
            loff_t punch_end = end < size ? end : size;
            ret = __fallocate_zero_range(f, offset, punch_end - offset, 1);
        }
        vfs_fput(f);
        return ret;
    }

    if (keep_size) {
        loff_t size = inode->size;
        if (offset < size) {
            loff_t alloc_end = end < size ? end : size;
            ret = __fallocate_zero_range(f, offset, alloc_end - offset, 1);
        }
        vfs_fput(f);
        return ret;
    }

    ret = __fallocate_zero_range(f, offset, len, 0);
    vfs_fput(f);
    return ret;
}

uint64 sys_vfs_fcntl(void) {
    int fd, cmd, arg = 0;
    uint64 uarg = 0;
    argint(0, &fd);
    argint(1, &cmd);

    if (fd < 0 || fd >= NOFILE) {
        return -EBADF;
    }

    if (cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW ||
        cmd == F_OFD_GETLK || cmd == F_OFD_SETLK || cmd == F_OFD_SETLKW ||
        cmd == F_SETOWN_EX || cmd == F_GETOWN_EX) {
        argaddr(2, &uarg);
    } else {
        argint(2, &arg);
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
    switch (normalized_cmd) {
    case F_ADD_SEALS:
        if (!f->f_is_memfd) {
            ret = -EINVAL;
            break;
        }
        if (!f->f_allow_sealing || (f->f_seals & F_SEAL_SEAL)) {
            ret = -EPERM;
            break;
        }
        if (arg & ~(F_SEAL_SEAL | F_SEAL_SHRINK |
                    F_SEAL_GROW | F_SEAL_WRITE)) {
            ret = -EINVAL;
            break;
        }
        f->f_seals |= (uint32)arg;
        ret = 0;
        break;
    case F_GET_SEALS:
        if (!f->f_is_memfd) {
            ret = -EINVAL;
            break;
        }
        ret = (int)f->f_seals;
        break;
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
    case F_OFD_GETLK:
    case F_OFD_SETLK:
    case F_OFD_SETLKW: {
        struct flock fl = {0};
        if (uarg == 0) {
            ret = -EINVAL;
            break;
        }
        if (vm_copyin(current->vm, &fl, uarg, sizeof(fl)) < 0) {
            ret = -EFAULT;
            break;
        }
        pid_t lock_owner =
            (normalized_cmd == F_OFD_GETLK || normalized_cmd == F_OFD_SETLK ||
             normalized_cmd == F_OFD_SETLKW)
                ? f->f_ofd_lock_owner
                : current->tgid;
        ret = vfs_file_lock_ctl(f, lock_owner, normalized_cmd, &fl);
        if (ret == 0 &&
            (normalized_cmd == F_GETLK || normalized_cmd == F_OFD_GETLK) &&
            vm_copyout(current->vm, uarg, &fl, sizeof(fl)) < 0) {
            ret = -EFAULT;
        }
        break;
    }
    case F_GETFL:
        ret = f->f_flags & ~O_CLOEXEC;
        break;
    case F_GETOWN:
        ret = f->f_owner;
        break;
    case F_SETOWN:
        f->f_owner = arg;
        f->f_owner_type = arg < 0 ? F_OWNER_PGRP : F_OWNER_PID;
        ret = 0;
        break;
    case F_GETSIG:
        ret = f->f_owner_signal;
        break;
    case F_SETSIG:
        if (arg < 0 || arg >= NSIG) {
            ret = -EINVAL;
            break;
        }
        f->f_owner_signal = arg;
        ret = 0;
        break;
    case F_SETOWN_EX: {
        struct f_owner_ex owner;
        if (uarg == 0) {
            ret = -EINVAL;
            break;
        }
        if (vm_copyin(current->vm, &owner, uarg, sizeof(owner)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (owner.type != F_OWNER_TID && owner.type != F_OWNER_PID &&
            owner.type != F_OWNER_PGRP) {
            ret = -EINVAL;
            break;
        }
        f->f_owner = owner.pid;
        f->f_owner_type = owner.type;
        ret = 0;
        break;
    }
    case F_GETOWN_EX: {
        struct f_owner_ex owner = {
            .type = f->f_owner_type,
            .pid = f->f_owner,
        };
        if (uarg == 0) {
            ret = -EINVAL;
            break;
        }
        if (vm_copyout(current->vm, uarg, &owner, sizeof(owner)) < 0) {
            ret = -EFAULT;
            break;
        }
        ret = 0;
        break;
    }
    case F_GETLEASE:
        ret = F_UNLCK;
        break;
    case F_SETLEASE:
        if (arg != F_RDLCK && arg != F_WRLCK && arg != F_UNLCK) {
            ret = -EINVAL;
            break;
        }
        ret = arg == F_UNLCK ? 0 : -EAGAIN;
        break;
    case F_NOTIFY: {
        uint32 supported = DN_ACCESS | DN_MODIFY | DN_CREATE | DN_DELETE |
                           DN_RENAME | DN_ATTRIB | DN_MULTISHOT;
        if (arg == 0) {
            ret = 0;
            break;
        }
        if ((uint32)arg & ~supported) {
            ret = -EINVAL;
            break;
        }
        struct vfs_inode *inode = vfs_inode_deref(&f->inode);
        if (inode == NULL || !S_ISDIR(inode->mode)) {
            ret = -ENOTDIR;
            break;
        }
        ret = 0;
        break;
    }
    case F_SETFL: {
        int old_flags = f->f_flags;
        int new_flags = (f->f_flags & O_ACCMODE) | (arg & ~(O_ACCMODE | O_CLOEXEC));
        f->f_flags = new_flags;
        /* Propagate O_NONBLOCK to pipe internal flags.
         * writer_lock protects read_file/write_file pointers and
         * prevents pipe_close() from freeing the pipe mid-access. */
        if (f->f_kind == VFS_FILE_KIND_PIPE && f->pipe != NULL) {
            struct pipe *pi = f->pipe;
            spin_lock(&pi->writer_lock);
            int pflags = pipe_get_flags(pi);
            if (new_flags & O_NONBLOCK) {
                if (f == pi->read_file)
                    pflags |= (1 << PIPE_FLAGS_NONBLOCK_RD);
                if (f == pi->write_file)
                    pflags |= (1 << PIPE_FLAGS_NONBLOCK_WR);
            } else {
                if (f == pi->read_file)
                    pflags &= ~(1 << PIPE_FLAGS_NONBLOCK_RD);
                if (f == pi->write_file)
                    pflags &= ~(1 << PIPE_FLAGS_NONBLOCK_WR);
            }
            pipe_set_flags(pi, pflags);
            spin_unlock(&pi->writer_lock);
        }
        if (f->ops != NULL && f->ops->set_flags != NULL) {
            ret = f->ops->set_flags(f, old_flags, new_flags);
            break;
        }
        ret = 0;
        break;
    }
    case F_GETPIPE_SZ:
        if (f->f_kind != VFS_FILE_KIND_PIPE || f->pipe == NULL) {
            ret = -EBADF;
            break;
        }
        ret = PIPESIZE;
        break;
    case F_SETPIPE_SZ:
        if (f->f_kind != VFS_FILE_KIND_PIPE || f->pipe == NULL) {
            ret = -EBADF;
            break;
        }
        if (arg < 0) {
            ret = -EINVAL;
            break;
        }
        if (arg > PIPESIZE) {
            ret = -EPERM;
            break;
        }
        ret = PIPESIZE;
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
    if (S_ISBLK(inode->mode))
        kst->st_rdev = inode->bdev;
    else if (S_ISCHR(inode->mode))
        kst->st_rdev = inode->cdev;
    kst->st_size = inode->size;
    kst->st_blksize = 4096;
    kst->st_blocks = (inode->size + 511) / 512;
    kst->st_atime_sec = inode->atime;
    kst->st_mtime_sec = inode->mtime;
    kst->st_ctime_sec = inode->ctime;
    vfs_iunlock(inode);
    return 0;
}

uint64 sys_vfs_stat(void) {
    char *path;
    uint64 st_addr;
    int n;
    int ret = __vfs_argpath(0, &path, &n);
    argaddr(1, &st_addr);
    if (ret < 0)
        return ret;

    /* vfs_namei now follows all symlinks automatically */
    struct vfs_inode *inode = vfs_namei(path, n);
    kvfree(path);
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

uint64 sys_vfs_lstat(void) {
    char *path;
    char *name = __vfs_alloc_pathbuf();
    uint64 st_addr;
    int n;
    int ret;

    if (name == NULL)
        return -ENOMEM;

    ret = __vfs_argpath(0, &path, &n);
    argaddr(1, &st_addr);
    if (ret < 0) {
        kvfree(name);
        return ret;
    }

    bool root_path = true;
    for (int i = 0; i < n; i++) {
        if (path[i] != '/') {
            root_path = false;
            break;
        }
    }
    if (root_path) {
        struct vfs_inode *inode = vfs_namei(path, n);
        kvfree(path);
        kvfree(name);
        if (IS_ERR(inode))
            return PTR_ERR(inode);
        if (inode == NULL)
            return -ENOENT;

        struct stat kst;
        ret = __vfs_inode_stat(inode, &kst);
        vfs_iput(inode);
        if (ret != 0)
            return ret;
        if (either_copyout(1, st_addr, &kst, sizeof(kst)) < 0)
            return -EFAULT;
        return 0;
    }

    struct vfs_inode *parent = vfs_nameiparent(path, n, name, VFS_USER_PATH_MAX);
    kvfree(path);
    if (IS_ERR(parent)) {
        kvfree(name);
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        kvfree(name);
        return -ENOENT;
    }

    struct vfs_dentry dentry = {.sb = parent->sb, .parent = parent};
    ret = vfs_ilookup(parent, &dentry, name, strlen(name));
    if (ret != 0) {
        vfs_iput(parent);
        kvfree(name);
        return ret;
    }
    kvfree(name);

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
    char *path;
    int mode;
    int n;
    int ret = __vfs_argpath(0, &path, &n);
    argint(1, &mode);
    if (ret < 0)
        return ret;

    struct vfs_inode *inode = vfs_namei(path, n);
    kvfree(path);
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
    char name[MAXPATH];
    uint64 buf_addr;
    int bufsz;

    int n = argstr(0, path, MAXPATH);
    argaddr(1, &buf_addr);
    argint(2, &bufsz);

    if (n < 0) {
        WEBKIT_READLINK_TRACE("readlink path=<fault> ret=%d\n", -EFAULT);
        return -EFAULT;
    }
    if (bufsz <= 0) {
        WEBKIT_READLINK_TRACE("readlink path=%s bufsz=%d ret=%d\n", path,
                              bufsz, -EINVAL);
        return -EINVAL;
    }

    struct vfs_inode *parent = vfs_nameiparent(path, n, name, MAXPATH);
    if (IS_ERR(parent)) {
        int err = PTR_ERR(parent);
        WEBKIT_READLINK_TRACE("readlink path=%s parent ret=%d\n", path, err);
        return err;
    }
    if (parent == NULL) {
        WEBKIT_READLINK_TRACE("readlink path=%s parent ret=%d\n", path,
                              -ENOENT);
        return -ENOENT;
    }

    struct vfs_dentry dentry = {.sb = parent->sb, .parent = parent};
    int ret = vfs_ilookup(parent, &dentry, name, strlen(name));
    if (ret != 0) {
        vfs_iput(parent);
        WEBKIT_READLINK_TRACE("readlink path=%s lookup name=%s ret=%d\n",
                              path, name, ret);
        return ret;
    }

    struct vfs_inode *inode = vfs_get_dentry_inode(&dentry);
    vfs_release_dentry(&dentry);
    vfs_iput(parent);
    if (IS_ERR(inode)) {
        int err = PTR_ERR(inode);
        WEBKIT_READLINK_TRACE("readlink path=%s inode ret=%d\n", path, err);
        return err;
    }
    if (inode == NULL) {
        WEBKIT_READLINK_TRACE("readlink path=%s inode ret=%d\n", path,
                              -ENOENT);
        return -ENOENT;
    }

    char *kbuf = kvmalloc(bufsz);
    if (kbuf == NULL) {
        vfs_iput(inode);
        WEBKIT_READLINK_TRACE("readlink path=%s bufsz=%d ret=%d\n", path,
                              bufsz, -ENOMEM);
        return -ENOMEM;
    }

    ssize_t len = vfs_readlink(inode, kbuf, bufsz);
    vfs_iput(inode);

    if (len < 0) {
        kvfree(kbuf);
        WEBKIT_READLINK_TRACE("readlink path=%s bufsz=%d ret=%ld\n", path,
                              bufsz, (long)len);
        return len;
    }

    if (either_copyout(1, buf_addr, kbuf, len) < 0) {
        kvfree(kbuf);
        WEBKIT_READLINK_TRACE("readlink path=%s len=%ld ret=%d\n", path,
                              (long)len, -EFAULT);
        return -EFAULT;
    }
    if (len >= 0 && len < bufsz)
        kbuf[len] = '\0';
    else if (bufsz > 0)
        kbuf[bufsz - 1] = '\0';
    WEBKIT_READLINK_TRACE("readlink path=%s len=%ld target=%s\n", path,
                          (long)len, kbuf);
    kvfree(kbuf);
    return len;
}

uint64 sys_vfs_rename(void) {
    char oldpath[MAXPATH], newpath[MAXPATH];
    char oldname[MAXPATH], newname[MAXPATH];
    int n1 = argstr(0, oldpath, MAXPATH);
    int n2 = argstr(1, newpath, MAXPATH);
    if (n1 < 0 || n2 < 0) {
        return -EFAULT;
    }

    struct vfs_inode *old_parent =
        vfs_nameiparent(oldpath, n1, oldname, MAXPATH);
    if (IS_ERR(old_parent)) {
        return PTR_ERR(old_parent);
    }
    if (old_parent == NULL) {
        return -ENOENT;
    }

    struct vfs_inode *new_parent =
        vfs_nameiparent(newpath, n2, newname, MAXPATH);
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
    SYSCALL_PROFILE_BEGIN(g_sys_open_calls);
    char *path;
    char *name;
    int omode;
    int mode = 0;
    int n;

    argint(1, &omode);
    argint(2, &mode);
    if (omode & O_PATH)
        omode &= (O_PATH | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    int path_ret = __vfs_argpath(0, &path, &n);
    if (path_ret < 0) {
        SYSCALL_PROFILE_RETURN(path_ret, g_sys_open_ticks);
    }
    name = __vfs_alloc_pathbuf();
    if (name == NULL) {
        kvfree(path);
        SYSCALL_PROFILE_RETURN(-ENOMEM, g_sys_open_ticks);
    }

#define SYS_VFS_OPEN_RETURN(ret_expr)                                        \
    do {                                                                     \
        kvfree(path);                                                        \
        kvfree(name);                                                        \
        SYSCALL_PROFILE_RETURN(ret_expr, g_sys_open_ticks);                  \
    } while (0)

    if (n < 0) {
        SYSCALL_PROFILE_RETURN(-EFAULT, g_sys_open_ticks);
    }

    struct vfs_inode *inode = NULL;
    int ret = 0;

    if ((omode & O_TMPFILE) == O_TMPFILE) {
        inode = vfs_namei(path, strlen(path));
        if (IS_ERR(inode))
            SYS_VFS_OPEN_RETURN(PTR_ERR(inode));
        if (inode == NULL)
            SYS_VFS_OPEN_RETURN(-ENOENT);
        int is_dir = S_ISDIR(inode->mode);
        vfs_iput(inode);
        if (!is_dir)
            SYS_VFS_OPEN_RETURN(-ENOTDIR);
        if ((omode & (O_WRONLY | O_RDWR)) == 0)
            SYS_VFS_OPEN_RETURN(-EINVAL);
        SYS_VFS_OPEN_RETURN(-EOPNOTSUPP);
    }

    if (omode & O_CREAT) {
        bool root_path = n > 0;
        for (int i = 0; root_path && i < n; i++)
            root_path = path[i] == '/';
        if (root_path)
            SYS_VFS_OPEN_RETURN(-EISDIR);

        // Create file if it doesn't exist
        struct vfs_inode *parent = vfs_nameiparent(path, n, name, VFS_USER_PATH_MAX);
        if (IS_ERR(parent)) {
            SYS_VFS_OPEN_RETURN(PTR_ERR(parent));
        }
        if (parent == NULL) {
            SYS_VFS_OPEN_RETURN(-ENOENT);
        }

        size_t name_len = strlen(name);
        if (name_len == 0) {
            vfs_iput(parent);
            SYS_VFS_OPEN_RETURN(-EISDIR);
        }

        // Try to create (apply umask to default mode)
        mode_t create_mode = mode == 0 ? 0666 : (mode_t)mode;
        inode = vfs_create(parent, create_mode & ~current_umask(), name,
                           name_len);
        vfs_iput(parent);

        if (IS_ERR(inode)) {
            if (PTR_ERR(inode) == -EEXIST && !(omode & O_EXCL)) {
                // File exists, try to open it
                inode = vfs_namei(path, n);
                // O_CREAT on an existing directory is not allowed
                if (!IS_ERR_OR_NULL(inode) && S_ISDIR(inode->mode)) {
                    vfs_iput(inode);
                    SYS_VFS_OPEN_RETURN(-EISDIR);
                }
            } else {
                SYS_VFS_OPEN_RETURN(PTR_ERR(inode));
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
            struct vfs_inode *parent =
                vfs_nameiparent(path, strlen(path), name, VFS_USER_PATH_MAX);
            if (IS_ERR(parent))
                SYS_VFS_OPEN_RETURN(PTR_ERR(parent));
            if (parent == NULL)
                SYS_VFS_OPEN_RETURN(-ENOENT);

            struct vfs_dentry dentry = {.sb = parent->sb, .parent = parent};
            int lr = vfs_ilookup(parent, &dentry, name, strlen(name));
            if (lr != 0) {
                vfs_iput(parent);
                SYS_VFS_OPEN_RETURN(lr);
            }
            inode = vfs_get_dentry_inode(&dentry);
            vfs_release_dentry(&dentry);
            vfs_iput(parent);
            if (IS_ERR(inode))
                SYS_VFS_OPEN_RETURN(PTR_ERR(inode));
            if (inode == NULL)
                SYS_VFS_OPEN_RETURN(-ENOENT);
            /* O_NOFOLLOW fails symlinks except Linux O_PATH, which opens the link itself. */
            if (S_ISLNK(inode->mode) && !(omode & O_PATH)) {
                vfs_iput(inode);
                SYS_VFS_OPEN_RETURN(-ELOOP);
            }
        } else {
            inode = vfs_namei(path, strlen(path));
            if (IS_ERR(inode)) {
                SYS_VFS_OPEN_RETURN(PTR_ERR(inode));
            }
            if (inode == NULL)
                SYS_VFS_OPEN_RETURN(-ENOENT);
        }
    }

    if (IS_ERR(inode)) {
        SYS_VFS_OPEN_RETURN(PTR_ERR(inode));
    }
    if (inode == NULL) {
        SYS_VFS_OPEN_RETURN(-ENOENT);
    }

    // Check if trying to write to a directory
    if (S_ISDIR(inode->mode) && (omode & O_WRONLY || omode & O_RDWR)) {
        vfs_iput(inode);
        SYS_VFS_OPEN_RETURN(-EISDIR);
    }
    if ((omode & O_DIRECTORY) && !S_ISDIR(inode->mode)) {
        vfs_iput(inode);
        SYS_VFS_OPEN_RETURN(-ENOTDIR);
    }

    // Check for O_TRUNC before releasing inode reference
    int should_truncate =
        !(omode & O_PATH) && (omode & O_TRUNC) && S_ISREG(inode->mode);

    struct vfs_file *f = vfs_fileopen(inode, omode);
    vfs_iput(inode); // Release local inode reference (file holds its own ref)

    if (IS_ERR(f)) {
        SYS_VFS_OPEN_RETURN(PTR_ERR(f));
    }

    // Handle O_TRUNC - truncate the file to zero length
    if (should_truncate) {
        ret = vfs_itruncate(vfs_inode_deref(&f->inode), 0);
        if (ret != 0) {
            vfs_fput(f);
            SYS_VFS_OPEN_RETURN(ret);
        }
    }

    spin_lock(&current->fdtable->lock);
    int fd = __vfs_fdalloc(f);
    if (fd >= 0 && (omode & O_CLOEXEC)) {
        (void)vfs_fdtable_set_fdflags(current->fdtable, fd, FD_CLOEXEC);
    }
    spin_unlock(&current->fdtable->lock);

    if (fd >= 0 && webkit_vfs_trace_enabled() && webkit_vfs_trace_process()) {
        const char *fs_name = "(none)";
        struct vfs_inode *opened_inode = vfs_inode_deref(&f->inode);
        if (opened_inode != NULL && opened_inode->sb != NULL &&
            opened_inode->sb->fs_type != NULL &&
            opened_inode->sb->fs_type->name != NULL)
            fs_name = opened_inode->sb->fs_type->name;
        printf("webkit-vfs: open pid=%d name=%s fd=%d path=%s flags=0x%x "
               "mode=0x%x ino=%lu fs=%s size=%lld\n",
               current->pid, current->name, fd, path, omode,
               opened_inode != NULL ? opened_inode->mode : 0,
               opened_inode != NULL ? opened_inode->ino : 0, fs_name,
               opened_inode != NULL ? opened_inode->size : 0);
    }

    // When success, the refcount of f will be increased by fdtable, thus we do
    // not put f here. When failure, we need to put f anyway.
    vfs_fput(f);
    if (fd >= 0)
        ACCT_INC(current->thread_group, fs_opens);
    SYS_VFS_OPEN_RETURN(fd);
#undef SYS_VFS_OPEN_RETURN
}

uint64 sys_vfs_mkdir(void) {
    char *path;
    char *name = __vfs_alloc_pathbuf();
    int mode;
    int n;

    if (name == NULL)
        return -ENOMEM;
    int ret = __vfs_argpath(0, &path, &n);
    argint(1, &mode);
    if (ret < 0) {
        kvfree(name);
        return ret;
    }

    struct vfs_inode *parent =
        vfs_nameiparent_at(NULL, path, n, name, VFS_USER_PATH_MAX);
    kvfree(path);
    if (IS_ERR(parent)) {
        kvfree(name);
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        kvfree(name);
        return -ENOENT;
    }

    size_t name_len = strlen(name);

    struct vfs_inode *dir =
        vfs_mkdir(parent, (mode_t)mode & ~current_umask(), name, name_len);
    kvfree(name);
    vfs_iput(parent);

    if (IS_ERR(dir)) {
        return PTR_ERR(dir);
    }

    vfs_iput(dir);
    ACCT_INC(current->thread_group, fs_creates);
    return 0;
}

/**
 * sys_vfs_mknod - Create a special file
 * Args: a0=path, a1=mode, a2=dev
 *
 * musl packs major/minor into a single dev_t via makedev().
 * The kernel's mkdev() uses (major << 20 | minor); user-space must
 * encode dev_t with the same layout.
 */
uint64 sys_vfs_mknod(void) {
    if (!capable())
        return (uint64)-EPERM;

    char *path;
    char *name = __vfs_alloc_pathbuf();
    int mode;
    uint64 dev;
    int n;

    if (name == NULL)
        return -ENOMEM;
    int ret = __vfs_argpath(0, &path, &n);
    if (ret < 0) {
        kvfree(name);
        return ret;
    }
    argint(1, &mode);
    argaddr(2, &dev);

    struct vfs_inode *parent = vfs_nameiparent(path, n, name, VFS_USER_PATH_MAX);
    kvfree(path);
    if (IS_ERR(parent)) {
        kvfree(name);
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        kvfree(name);
        return -ENOENT;
    }

    size_t name_len = strlen(name);

    struct vfs_inode *node =
        vfs_mknod(parent, (mode_t)mode, (dev_t)dev, name, name_len);
    kvfree(name);
    vfs_iput(parent);

    if (IS_ERR(node)) {
        return PTR_ERR(node);
    }

    vfs_iput(node);
    return 0;
}

uint64 sys_vfs_unlink(void) {
    char *path;
    char *name = __vfs_alloc_pathbuf();
    int n;

    if (name == NULL)
        return -ENOMEM;
    int ret = __vfs_argpath(0, &path, &n);
    if (ret < 0) {
        kvfree(name);
        return ret;
    }

    struct vfs_inode *parent = vfs_nameiparent(path, n, name, VFS_USER_PATH_MAX);
    kvfree(path);
    if (IS_ERR(parent)) {
        kvfree(name);
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        kvfree(name);
        return -ENOENT;
    }

    size_t name_len = strlen(name);

    ret = vfs_unlink(parent, name, name_len);
    kvfree(name);
    vfs_iput(parent);
    if (ret == 0)
        ACCT_INC(current->thread_group, fs_deletes);
    return ret;
}

uint64 sys_vfs_link(void) {
    char old[MAXPATH], new[MAXPATH];
    char name[MAXPATH];
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
    struct vfs_inode *parent = vfs_nameiparent(new, n2, name, MAXPATH);
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

uint64 sys_vfs_symlink(void) {
    char target[MAXPATH], linkpath[MAXPATH];
    char name[MAXPATH];
    int n1, n2;

    if ((n1 = argstr(0, target, MAXPATH)) < 0 ||
        (n2 = argstr(1, linkpath, MAXPATH)) < 0) {
        return -EFAULT;
    }

    struct vfs_inode *parent = vfs_nameiparent(linkpath, n2, name, MAXPATH);
    if (IS_ERR(parent)) {
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        return -ENOENT;
    }

    size_t name_len = strlen(name);

    struct vfs_inode *sym =
        vfs_symlink(parent, 0777, name, name_len, target, strlen(target));
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
    struct vfs_inode *cwd = vfs_curdir();
    if (IS_ERR(cwd)) {
        return PTR_ERR(cwd);
    }
    if (cwd == NULL) {
        return -ENOENT;
    }
    struct vfs_inode *root = vfs_curroot();
    if (IS_ERR(root)) {
        vfs_iput(cwd);
        return PTR_ERR(root);
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
            vfs_iput(root);
            vfs_iput(cwd);
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
        vfs_iput(root);
        vfs_iput(cwd);
        return -ERANGE;
    }

    if (vm_copyout(p->vm, buf_addr, path, pathlen + 1) < 0) {
        vfs_iput(root);
        vfs_iput(cwd);
        return -EFAULT;
    }

    vfs_iput(root);
    vfs_iput(cwd);
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

// Linux getdents64-compatible dirent structure.
struct linux_dirent64 {
    uint64 d_ino;    // Inode number
    int64 d_off;     // Offset to next structure
    uint16 d_reclen; // Size of this dirent
    uint8 d_type;    // File type
    char d_name[];   // Filename (null-terminated)
};

// Legacy Linux getdents(2) record.  d_type is stored in the last byte of the
// record, after the nul-terminated name and any alignment padding.
struct linux_dirent_compat {
    uint64 d_ino;
    uint64 d_off;
    uint16 d_reclen;
    char d_name[];
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

static size_t linux_dirent_reclen(size_t name_len, bool compat)
{
    size_t base = compat ? sizeof(struct linux_dirent_compat) :
                           sizeof(struct linux_dirent64);
    size_t extra = compat ? 2 : 1; // nul + trailing d_type, or just nul
    return (base + name_len + extra + 7) & ~7;
}

static void linux_dirent_fill(void *dst, uint64 ino, int64 off, uint16 reclen,
                              uint8 dtype, const char *name, size_t name_len,
                              bool compat)
{
    if (compat) {
        struct linux_dirent_compat *de = dst;
        de->d_ino = ino;
        de->d_off = off;
        de->d_reclen = reclen;
        memmove(de->d_name, name, name_len);
        de->d_name[name_len] = '\0';
        ((uint8 *)dst)[reclen - 1] = dtype;
        return;
    }

    struct linux_dirent64 *de = dst;
    de->d_ino = ino;
    de->d_off = off;
    de->d_reclen = reclen;
    de->d_type = dtype;
    memmove(de->d_name, name, name_len);
    de->d_name[name_len] = '\0';
}

static uint64 sys_getdents_common(bool compat) {
    SYSCALL_PROFILE_BEGIN(g_sys_getdents_calls);
    int fd;
    uint64 dirp;
    int count;

    argint(0, &fd);
    argaddr(1, &dirp);
    argint(2, &count);

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL) {
        SYSCALL_PROFILE_RETURN(-EBADF, g_sys_getdents_ticks);
    }

    struct vfs_inode *inode = vfs_inode_deref(&f->inode);
    if (inode == NULL || !S_ISDIR(inode->mode)) {
        vfs_fput(f); // remove the reference from __vfs_argfd
        SYSCALL_PROFILE_RETURN(-ENOTDIR, g_sys_getdents_ticks);
    }

    // Allocate kernel buffer
    char *kbuf = kvmalloc(count);
    if (kbuf == NULL) {
        vfs_fput(f); // remove the reference from __vfs_argfd
        SYSCALL_PROFILE_RETURN(-ENOMEM, g_sys_getdents_ticks);
    }

    int bytes_written = 0;

    /*
     * Fast path: use getdents_fill to let the driver fill the buffer
     * directly under a single lock, avoiding per-entry VFS overhead.
     */
    if (!compat && inode->ops != NULL && inode->ops->getdents_fill != NULL) {
        int ret;

        /* Synthesize "." if at start */
        if (f->dir_iter.index == VFS_DITER_INDEX_START) {
            size_t reclen = linux_dirent_reclen(1, false);
            if ((int)reclen <= count) {
                linux_dirent_fill(kbuf, inode->ino, 0, (uint16)reclen,
                                  DT_DIR, ".", 1, false);
                bytes_written = reclen;
            }
            f->dir_iter.index = 1;
        }

        /* Delegate remaining entries to driver */
        if (bytes_written < count) {
            vfs_superblock_rlock(inode->sb);
            vfs_ilock(inode);

            ret = inode->ops->getdents_fill(inode, &f->dir_iter,
                                            kbuf + bytes_written,
                                            count - bytes_written);
            if (ret > 0) {
                bytes_written += ret;
            } else if (ret == 0 && f->dir_iter.index > 1) {
                /* No more entries — mark end */
                f->dir_iter.index = VFS_DITER_INDEX_END;
            }

            vfs_iunlock(inode);
            vfs_superblock_unlock(inode->sb);

            if (ret < 0) {
                kvfree(kbuf);
                vfs_fput(f);
                SYSCALL_PROFILE_RETURN(ret, g_sys_getdents_ticks);
            }
        }

        if (bytes_written > 0) {
            if (vm_copyout(current->vm, dirp, kbuf, bytes_written) < 0) {
                kvfree(kbuf);
                vfs_fput(f);
                SYSCALL_PROFILE_RETURN(-EFAULT, g_sys_getdents_ticks);
            }
        }

        kvfree(kbuf);
        vfs_fput(f);
        SYSCALL_PROFILE_RETURN(bytes_written, g_sys_getdents_ticks);
    }

    /* Slow path: per-entry vfs_dir_iter (fallback for drivers without
     * getdents_fill). */
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
            SYSCALL_PROFILE_RETURN(ret, g_sys_getdents_ticks);
        }

        if (dentry.name == NULL) {
            // End of directory
            break;
        }

        // Calculate record length (must be 8-byte aligned)
        size_t name_len = dentry.name_len;
        size_t reclen = linux_dirent_reclen(name_len, compat);

        if (bytes_written + (int)reclen > count) {
            // Not enough space, restore iterator state for next call
            f->dir_iter.cookies = saved_cookies;
            f->dir_iter.index = saved_index;
            vfs_release_dentry(&dentry);
            break;
        }

        // Get d_type — use ext4 dir entry type if available, else look up inode
        uint8 d_type = DT_UNKNOWN;
        if (dentry.d_type != 0) {
            /* Filesystem provided d_type directly (ext4 dir entry).  Map
             * EXT4_DE_* → DT_* using a static table matching Linux's
             * ext4_filetype_table[]. */
            static const uint8 ext4_de_to_dt[] = {
                [0] = DT_UNKNOWN, /* EXT4_DE_UNKNOWN */
                [1] = DT_REG,     /* EXT4_DE_REG_FILE */
                [2] = DT_DIR,     /* EXT4_DE_DIR */
                [3] = DT_CHR,     /* EXT4_DE_CHRDEV */
                [4] = DT_BLK,     /* EXT4_DE_BLKDEV */
                [5] = DT_FIFO,    /* EXT4_DE_FIFO */
                [6] = DT_SOCK,    /* EXT4_DE_SOCK */
                [7] = DT_LNK,     /* EXT4_DE_SYMLINK */
            };
            if (dentry.d_type < sizeof(ext4_de_to_dt))
                d_type = ext4_de_to_dt[dentry.d_type];
        }
        if (d_type == DT_UNKNOWN) {
            struct vfs_inode *child = vfs_get_dentry_inode(&dentry);
            if (!IS_ERR_OR_NULL(child)) {
                d_type = __mode_to_dtype(child->mode);
                vfs_iput(child);
            }
        }

        linux_dirent_fill(kbuf + bytes_written, dentry.ino, f->dir_iter.index,
                          (uint16)reclen, d_type, dentry.name, name_len,
                          compat);

        bytes_written += reclen;
        vfs_release_dentry(&dentry);
        memset(&dentry, 0, sizeof(dentry));
    }

    // Copy to user space
    if (bytes_written > 0) {
        if (vm_copyout(current->vm, dirp, kbuf, bytes_written) < 0) {
            kvfree(kbuf);
            vfs_fput(f); // remove the reference from __vfs_argfd
            SYSCALL_PROFILE_RETURN(-EFAULT, g_sys_getdents_ticks);
        }
    }

    kvfree(kbuf);
    vfs_fput(f); // remove the reference from __vfs_argfd
    SYSCALL_PROFILE_RETURN(bytes_written, g_sys_getdents_ticks);
}

uint64 sys_getdents(void) {
    return sys_getdents_common(false);
}

uint64 sys_getdents_compat(void) {
    return sys_getdents_common(true);
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
                   const char *source, int source_len,
                   unsigned long flags, const char *data) {
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
    int ret = vfs_mount(fstype, target_dir, source_inode, (int)flags, data);

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

/**
 * vfs_remount_path - Remount a filesystem with new flags
 * @target: target mount point path
 * @target_len: length of target path
 * @flags: new mount flags (MS_RDONLY, etc.)
 * @data: filesystem-specific data string (may be NULL)
 *
 * Returns 0 on success, negative errno on failure.
 */
int vfs_remount_path(const char *target, int target_len,
                     unsigned long flags, const char *data) {
    struct vfs_inode *mounted_root = vfs_namei(target, target_len);
    if (IS_ERR(mounted_root)) {
        return PTR_ERR(mounted_root);
    }
    if (mounted_root == NULL) {
        return -ENOENT;
    }

    if (!vfs_inode_is_local_root(mounted_root)) {
        vfs_iput(mounted_root);
        return -EINVAL;
    }

    struct vfs_superblock *sb = mounted_root->sb;
    if (sb == NULL) {
        vfs_iput(mounted_root);
        return -EINVAL;
    }

    vfs_mount_lock();
    vfs_superblock_wlock(sb);
    vfs_ilock(mounted_root);

    int ret = vfs_remount(mounted_root, (int)flags, data);

    vfs_iunlock(mounted_root);
    vfs_superblock_unlock(sb);
    vfs_mount_unlock();

    vfs_iput(mounted_root);
    return ret;
}

/**
 * vfs_move_mount_path - Move a mount from one location to another
 * @old_target: current mount point path
 * @old_len: length of old target path
 * @new_target: new mount point path
 * @new_len: length of new target path
 *
 * Returns 0 on success, negative errno on failure.
 */
int vfs_move_mount_path(const char *old_target, int old_len,
                        const char *new_target, int new_len) {
    /* Resolve old target — following mounts gives us the mounted root */
    struct vfs_inode *mounted_root = vfs_namei(old_target, old_len);
    if (IS_ERR(mounted_root)) {
        return PTR_ERR(mounted_root);
    }
    if (mounted_root == NULL) {
        return -ENOENT;
    }
    if (!vfs_inode_is_local_root(mounted_root)) {
        vfs_iput(mounted_root);
        return -EINVAL;
    }

    struct vfs_superblock *child_sb = mounted_root->sb;
    if (child_sb == NULL || child_sb->mountpoint == NULL) {
        vfs_iput(mounted_root);
        return -EINVAL;
    }

    struct vfs_inode *old_mountpoint = child_sb->mountpoint;
    if (!old_mountpoint->mount) {
        vfs_iput(mounted_root);
        return -EINVAL;
    }

    /* Resolve new target */
    struct vfs_inode *new_dir = vfs_namei(new_target, new_len);
    if (IS_ERR(new_dir)) {
        vfs_iput(mounted_root);
        return PTR_ERR(new_dir);
    }
    if (new_dir == NULL) {
        vfs_iput(mounted_root);
        return -ENOENT;
    }
    if (!S_ISDIR(new_dir->mode)) {
        vfs_iput(new_dir);
        vfs_iput(mounted_root);
        return -ENOTDIR;
    }

    /*
     * Lock order: mount mutex → parent superblocks → child superblock
     *             → inode locks
     *
     * If old and new directories are on the same superblock we only take
     * one superblock lock; otherwise we take the old parent's first,
     * then the new parent's.
     */
    vfs_mount_lock();

    struct vfs_superblock *old_parent_sb = old_mountpoint->sb;
    struct vfs_superblock *new_parent_sb = new_dir->sb;

    if (old_parent_sb == new_parent_sb) {
        vfs_superblock_wlock(old_parent_sb);
    } else {
        /* Consistent ordering by superblock pointer address */
        if ((uint64)old_parent_sb < (uint64)new_parent_sb) {
            vfs_superblock_wlock(old_parent_sb);
            vfs_superblock_wlock(new_parent_sb);
        } else {
            vfs_superblock_wlock(new_parent_sb);
            vfs_superblock_wlock(old_parent_sb);
        }
    }

    vfs_superblock_wlock(child_sb);
    vfs_ilock(old_mountpoint);
    vfs_ilock(new_dir);

    int ret = vfs_move_mount(old_mountpoint, new_dir);

    if (ret == 0) {
        /* Success: old_mountpoint is cleared, new_dir is now the mountpoint.
         * Unlock everything.  vfs_iput on old_mountpoint releases the extra
         * ref that __vfs_turn_mountpoint originally took. */
        vfs_iunlock(new_dir);
        vfs_iunlock(old_mountpoint);
        vfs_superblock_unlock(child_sb);
        if (old_parent_sb != new_parent_sb) {
            vfs_superblock_unlock(new_parent_sb);
        }
        vfs_superblock_unlock(old_parent_sb);
    } else {
        vfs_iunlock(new_dir);
        vfs_iunlock(old_mountpoint);
        vfs_superblock_unlock(child_sb);
        if (old_parent_sb != new_parent_sb) {
            vfs_superblock_unlock(new_parent_sb);
        }
        vfs_superblock_unlock(old_parent_sb);
    }

    vfs_mount_unlock();
    vfs_iput(new_dir);
    vfs_iput(mounted_root);
    return ret;
}

uint64 sys_mount(void) {
    if (!capable())
        return (uint64)-EPERM;

    char source[MAXPATH];
    char target[MAXPATH];
    char fstype[32];
    int n1, n2;
    uint64 flags = 0;
    uint64 data_addr = 0;

    if ((n1 = argstr(0, source, MAXPATH)) < 0 ||
        (n2 = argstr(1, target, MAXPATH)) < 0 || argstr(2, fstype, 32) < 0) {
        return -EFAULT;
    }

    /* arg3 = flags (unsigned long), arg4 = data pointer (may be 0/NULL) */
    argaddr(3, &flags);
    argaddr(4, &data_addr);
    (void)data_addr; /* data is not yet used — reserved for future use */

    int ret;

    if (flags & MS_MOVE) {
        /* MS_MOVE: source is the old mountpoint, target is the new one */
        ret = vfs_move_mount_path(source, n1, target, n2);
    } else if (flags & MS_REMOUNT) {
        /* MS_REMOUNT: change options on an existing mount */
        ret = vfs_remount_path(target, n2, flags & ~MS_REMOUNT, NULL);
    } else {
        /* Normal mount */
        ret = vfs_mount_path(fstype, target, n2, source, n1, flags, NULL);
    }

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
        struct linux_ioctl_termios lt;
        ret = vfs_ioctl(f, cmd, &kt);
        if (ret == 0) {
            termios_to_linux_ioctl(&kt, &lt);
            if (either_copyout(1, arg, &lt, sizeof(lt)) < 0)
                ret = -EFAULT;
        }
        break;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        struct termios kt;
        struct linux_ioctl_termios lt;
        ret = vfs_ioctl(f, TCGETS, &kt);
        if (ret != 0) {
            break;
        }
        if (either_copyin(&lt, 1, arg, sizeof(lt)) < 0) {
            ret = -EFAULT;
        } else {
            linux_ioctl_to_termios(&lt, &kt);
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

static void webkit_poll_trace_fd(const char *phase, const struct pollfd_k *pfd,
                                 struct vfs_file *f)
{
    if (!webkit_poll_trace_enabled() || !webkit_vfs_trace_process())
        return;

    const char *kind = "none";
    if (f == NULL) {
        kind = "badfd";
    } else if (f->ops != NULL && f->ops->poll != NULL) {
        kind = "fileops-poll";
    } else if (f->f_kind == VFS_FILE_KIND_LEGACY_SOCKET && f->sock != NULL) {
        kind = "legacy-sock";
    } else if (f->inode.inode == NULL) {
        kind = "custom";
    } else if (S_ISCHR(f->inode.inode->mode)) {
        kind = "char";
    } else {
        kind = "regular";
    }

    printf("webkit-poll: %s pid=%d name=%s fd=%d events=0x%x "
           "revents=0x%x kind=%s flags=0x%x ops=%p poll=%p inode=%p\n",
           phase, current->pid, current->name, pfd->fd,
           (uint)pfd->events, (uint)pfd->revents, kind,
           f != NULL ? (uint)f->f_flags : 0,
           f != NULL ? f->ops : NULL,
           (f != NULL && f->ops != NULL) ? f->ops->poll : NULL,
	           f != NULL ? f->inode.inode : NULL);
}

static const char *webkit_poll_fd_kind(struct vfs_file *f)
{
    if (f == NULL)
        return "badfd";
    if (f->ops != NULL && f->ops->poll != NULL)
        return "fileops-poll";
    if (f->f_kind == VFS_FILE_KIND_LEGACY_SOCKET && f->sock != NULL)
        return "legacy-sock";
    if (f->inode.inode == NULL)
        return "custom";
    if (S_ISCHR(f->inode.inode->mode))
        return "char";
    return "regular";
}

/*
 * __vfs_poll_always_ready - report requested events as ready based on
 * file access mode.  Used as fallback when no specific poll callback
 * is available (regular files, block devices, /dev/null, etc.).
 */
static inline short __vfs_poll_always_ready(short events, int f_flags) {
    short revents = 0;
    if ((events & (POLLIN | POLLRDNORM | POLLRDBAND)) &&
        ((f_flags & O_ACCMODE) != O_WRONLY))
        revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
    if ((events & (POLLOUT | POLLWRNORM | POLLWRBAND)) &&
        ((f_flags & O_ACCMODE) != O_RDONLY))
        revents |= (events & (POLLOUT | POLLWRNORM | POLLWRBAND));
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
            webkit_poll_trace_fd("scan", pfd, NULL);
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
             * Inode-less custom files (lwIP sockets, eventfd, timerfd,
             * kqueue, etc.) must use their file_ops poll callback when one
             * exists.  Treating them as always-ready breaks poll()/ppoll()
             * for network sockets because readiness no longer reflects the
             * underlying netconn state.
             *
             * Legacy sockets created via vfs_sockalloc() still have
             * f->ops == NULL and use sockpoll() on their rxq.
             */
            if (f->ops != NULL && f->ops->poll != NULL) {
                pfd->revents = f->ops->poll(f, pfd->events);
            } else if (f->f_kind == VFS_FILE_KIND_LEGACY_SOCKET &&
                       f->sock != NULL) {
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
            cdev_t *cdev = f->f_kind == VFS_FILE_KIND_CDEV ? f->cdev : NULL;
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
        webkit_poll_trace_fd("scan", pfd, f);
        vfs_fput(f);

        if (pfd->revents != 0)
            ready++;
    }

    return ready;
}

static void webkit_poll_summary(const char *phase, int nfds, int timeout_ms,
                                int ready, struct pollfd_k *pfds)
{
    if (!webkit_poll_summary_enabled() || !webkit_vfs_trace_process())
        return;

    static _Atomic uint64 seq;
    uint64 n = __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED);
    if (n > 128 && (n & 0xff) != 0)
        return;

    int in = 0, out = 0, err = 0, zero = 0;
    int sample_fd[6];
    short sample_events[6];
    short sample_revents[6];
    const char *sample_kind[6];
    void *sample_poll[6];
    int nsample = 0;

    for (int i = 0; i < nfds; i++) {
        short r = pfds[i].revents;
        int take_sample = 0;
        if (r == 0) {
            zero++;
            take_sample = ready == 0 && nsample < (int)NELEM(sample_fd);
        } else {
            if (r & (POLLIN | POLLRDNORM | POLLRDBAND))
                in++;
            if (r & (POLLOUT | POLLWRNORM | POLLWRBAND))
                out++;
            if (r & (POLLERR | POLLHUP | POLLNVAL | POLLRDHUP))
                err++;
            take_sample = nsample < (int)NELEM(sample_fd);
        }

        if (take_sample) {
            const char *kind = "neg";
            void *poll = NULL;
            struct vfs_file *f = NULL;
            if (pfds[i].fd >= 0) {
                f = __vfs_argfd(pfds[i].fd);
                kind = webkit_poll_fd_kind(f);
                if (f != NULL && f->ops != NULL)
                    poll = f->ops->poll;
            }
            sample_fd[nsample] = pfds[i].fd;
            sample_events[nsample] = pfds[i].events;
            sample_revents[nsample] = r;
            sample_kind[nsample] = kind;
            sample_poll[nsample] = poll;
            nsample++;
            if (f != NULL)
                vfs_fput(f);
        }
    }

    printf("webkit-poll-summary: seq=%lu phase=%s pid=%d name=%s nfds=%d "
           "timeout=%d ready=%d in=%d out=%d err=%d zero=%d",
           n, phase, current->pid, current->name, nfds, timeout_ms, ready,
           in, out, err, zero);
    for (int i = 0; i < nsample; i++) {
        printf(" sample%d=fd%d/e%x/r%x/%s/%p", i, sample_fd[i],
               (uint)sample_events[i], (uint)sample_revents[i],
               sample_kind[i], sample_poll[i]);
    }
    printf("\n");
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
    webkit_poll_summary("initial", nfds, timeout_ms, ready, pfds);
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

    struct kqueue *kq = kqueue_alloc_private();
    if (kq == NULL) {
        /* If private kqueue allocation fails, fall back to a single scan */
        goto copyout;
    }

    /*
     * Register EVFILT_READ and/or EVFILT_WRITE for each polled fd.
     * Knotes stay registered (no EV_ONESHOT) so they can re-trigger
     * across loop iterations.  kqueue_close cleans them up.
     * Store the pollfd index in udata for result mapping.
     */
    int max_changes = nfds * 2; /* worst case: READ+WRITE per fd */
    struct kevent *changes = kvmalloc(max_changes * sizeof(struct kevent));
    if (changes == NULL) {
        kqueue_close_private(kq);
        goto copyout;
    }
    int nchanges = 0;

    for (int i = 0; i < nfds; i++) {
        if (pfds[i].fd < 0)
            continue;
        if (pfds[i].events & (POLLIN | POLLRDNORM | POLLRDBAND | POLLRDHUP)) {
            changes[nchanges].ident = pfds[i].fd;
            changes[nchanges].filter = EVFILT_READ;
            changes[nchanges].flags = EV_ADD;
            changes[nchanges].fflags = 0;
            changes[nchanges].data = 0;
            changes[nchanges].udata = (uint64)i;
            nchanges++;
        }
        if (pfds[i].events & (POLLOUT | POLLWRNORM | POLLWRBAND)) {
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
        kqueue_close_private(kq);
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
        webkit_poll_summary("wait", nfds, timeout_ms, ready, pfds);
        if (ready > 0)
            break;

        if (signal_pending(current)) {
            ready = -EINTR;
            break;
        }
    }

    #undef POLL_RESCAN_MS

    kqueue_close_private(kq);
    kvfree(changes);
    kvfree(events);

    if (ready == -EINTR) {
        kvfree(pfds);
        return -EINTR;
    }

copyout:
    webkit_poll_summary("copyout", nfds, timeout_ms, ready, pfds);
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
 *   a3 = sigset_t*
 *   a4 = sigsetsize
 */
uint64 sys_vfs_ppoll(void) {
    uint64 fds_addr;
    int nfds;
    uint64 tmo_p, sigmask_addr;
    int sigsetsize;

    argaddr(0, &fds_addr);
    argint(1, &nfds);
    argaddr(2, &tmo_p);
    argaddr(3, &sigmask_addr);
    argint(4, &sigsetsize);

    int timeout_ms;
    if (tmo_p == 0) {
        /* NULL timespec → infinite wait */
        timeout_ms = -1;
    } else {
        struct { int64 tv_sec; int64 tv_nsec; } ts;
        if (either_copyin(&ts, 1, tmo_p, sizeof(ts)) < 0)
            return (uint64)-EFAULT;
        int ret = timespec_to_timeout_ms_ceil(ts.tv_sec, ts.tv_nsec,
                                              &timeout_ms);
        if (ret < 0)
            return (uint64)ret;
    }

    sigset_t newmask, oldmask;
    int use_mask = 0;
    if (sigmask_addr != 0) {
        if (sigsetsize != (int)sizeof(sigset_t))
            return (uint64)-EINVAL;
        if (either_copyin(&newmask, 1, sigmask_addr, sizeof(newmask)) < 0)
            return (uint64)-EFAULT;
        sigmask_swap(&newmask, &oldmask);
        use_mask = 1;
    }

    uint64 ret = __vfs_poll_impl(fds_addr, nfds, timeout_ms);
    if (use_mask)
        sigmask_swap(&oldmask, NULL);
    return ret;
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

    sigset_t newmask, oldmask;
    int use_mask = 0;
    if (sig_addr != 0) {
        struct {
            uint64 ss;
            uint64 ss_len;
        } sig_data;

        if (either_copyin(&sig_data, 1, sig_addr, sizeof(sig_data)) < 0)
            return (uint64)-EFAULT;
        if (sig_data.ss != 0) {
            if (sig_data.ss_len != sizeof(sigset_t))
                return (uint64)-EINVAL;
            if (either_copyin(&newmask, 1, sig_data.ss,
                              sizeof(newmask)) < 0)
                return (uint64)-EFAULT;
            sigmask_swap(&newmask, &oldmask);
            use_mask = 1;
        }
    }

    /* Parse timeout: NULL means block indefinitely */
    int timeout_ms = -1;
    if (timeout_addr != 0) {
        int64 ts[2]; /* { tv_sec, tv_nsec } */
        if (either_copyin(ts, 1, timeout_addr, sizeof(ts)) < 0) {
            if (use_mask)
                sigmask_swap(&oldmask, NULL);
            return -EFAULT;
        }
        int ret = timespec_to_timeout_ms_ceil(ts[0], ts[1], &timeout_ms);
        if (ret < 0) {
            if (use_mask)
                sigmask_swap(&oldmask, NULL);
            return ret;
        }
    }

    /* No fds to watch — just sleep */
    if (nfds == 0) {
        if (timeout_ms == 0) {
            if (use_mask)
                sigmask_swap(&oldmask, NULL);
            return 0;
        }
        uint64 start = get_jiffs();
        uint64 ret = 0;
        while (timeout_ms < 0 || (int)(get_jiffs() - start) < timeout_ms) {
            sleep_ms(1);
            if (signal_pending(current)) {
                ret = (uint64)-EINTR;
                break;
            }
        }
        if (use_mask)
            sigmask_swap(&oldmask, NULL);
        return ret;
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
        if (!rfds) {
            if (use_mask)
                sigmask_swap(&oldmask, NULL);
            return -ENOMEM;
        }
        memset(rfds, 0, alloc_bytes);
        if (either_copyin(rfds, 1, readfds_addr, set_bytes) < 0) {
            kvfree(rfds);
            if (use_mask)
                sigmask_swap(&oldmask, NULL);
            return -EFAULT;
        }
    }
    if (writefds_addr) {
        wfds = kvmalloc(alloc_bytes);
        if (!wfds) {
            if (rfds) kvfree(rfds);
            if (use_mask)
                sigmask_swap(&oldmask, NULL);
            return -ENOMEM;
        }
        memset(wfds, 0, alloc_bytes);
        if (either_copyin(wfds, 1, writefds_addr, set_bytes) < 0) {
            if (rfds) kvfree(rfds);
            kvfree(wfds);
            if (use_mask)
                sigmask_swap(&oldmask, NULL);
            return -EFAULT;
        }
    }
    if (exceptfds_addr) {
        efds = kvmalloc(alloc_bytes);
        if (!efds) {
            if (rfds) kvfree(rfds);
            if (wfds) kvfree(wfds);
            if (use_mask)
                sigmask_swap(&oldmask, NULL);
            return -ENOMEM;
        }
        memset(efds, 0, alloc_bytes);
        if (either_copyin(efds, 1, exceptfds_addr, set_bytes) < 0) {
            if (rfds) kvfree(rfds);
            if (wfds) kvfree(wfds);
            kvfree(efds);
            if (use_mask)
                sigmask_swap(&oldmask, NULL);
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
        if (use_mask)
            sigmask_swap(&oldmask, NULL);
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
        if (use_mask)
            sigmask_swap(&oldmask, NULL);
        return -ENOMEM;
    }

    for (int fd = 0; fd < nfds; fd++) {
        int word = fd >> 6;
        uint64 bit = 1ULL << (fd & 63);
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
            int word = fd >> 6;
            uint64 bit = 1ULL << (fd & 63);
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
    if (use_mask)
        sigmask_swap(&oldmask, NULL);

    return ready;
}

uint64 sys_select(void)
{
    /*
     * Linux select(2) is pselect6 without the signal-mask argument.  Raw
     * callers provide only five registers, so this wrapper must ignore R9
     * instead of routing directly to sys_pselect6().
     */
    int nfds;
    uint64 readfds_addr, writefds_addr, exceptfds_addr;
    uint64 timeout_addr;

    argint(0, &nfds);
    argaddr(1, &readfds_addr);
    argaddr(2, &writefds_addr);
    argaddr(3, &exceptfds_addr);
    argaddr(4, &timeout_addr);

    if (nfds < 0 || nfds > NOFILE)
        return -EINVAL;

    int timeout_ms = -1;
    if (timeout_addr != 0) {
        struct {
            int64 tv_sec;
            int64 tv_usec;
        } tv;
        if (either_copyin(&tv, 1, timeout_addr, sizeof(tv)) < 0)
            return -EFAULT;
        if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000)
            return -EINVAL;
        if (tv.tv_sec > 0x7fffffff / 1000)
            timeout_ms = 0x7fffffff;
        else {
            timeout_ms = (int)(tv.tv_sec * 1000);
            timeout_ms += (int)((tv.tv_usec + 999) / 1000);
        }
    }

    if (nfds == 0) {
        if (timeout_ms > 0)
            sleep_ms_interruptible(timeout_ms);
        return signal_pending(current) ? (uint64)-EINTR : 0;
    }

    int set_bytes = ((nfds + 7) / 8);
    int set_words = (nfds + 63) / 64;
    int alloc_bytes = set_words * 8;
    uint64 *rfds = NULL, *wfds = NULL, *efds = NULL;
    struct pollfd_k *pfds = NULL;
    int *fd_map = NULL;
    int ready = -ENOMEM;

    if (readfds_addr) {
        rfds = kvmalloc(alloc_bytes);
        if (!rfds)
            goto out;
        memset(rfds, 0, alloc_bytes);
        if (either_copyin(rfds, 1, readfds_addr, set_bytes) < 0) {
            ready = -EFAULT;
            goto out;
        }
    }
    if (writefds_addr) {
        wfds = kvmalloc(alloc_bytes);
        if (!wfds)
            goto out;
        memset(wfds, 0, alloc_bytes);
        if (either_copyin(wfds, 1, writefds_addr, set_bytes) < 0) {
            ready = -EFAULT;
            goto out;
        }
    }
    if (exceptfds_addr) {
        efds = kvmalloc(alloc_bytes);
        if (!efds)
            goto out;
        memset(efds, 0, alloc_bytes);
        if (either_copyin(efds, 1, exceptfds_addr, set_bytes) < 0) {
            ready = -EFAULT;
            goto out;
        }
    }

    pfds = kvmalloc(nfds * sizeof(struct pollfd_k));
    fd_map = kvmalloc(nfds * sizeof(int));
    if (!pfds || !fd_map)
        goto out;

    int npfds = 0;
    for (int fd = 0; fd < nfds; fd++) {
        int word = fd >> 6;
        uint64 bit = 1ULL << (fd & 63);
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
            fd_map[npfds++] = fd;
        }
    }

    ready = npfds == 0 ? 0 : __vfs_poll_scan(pfds, npfds);
    if (ready == 0 && timeout_ms != 0) {
        uint64 poll_start = get_jiffs();
        for (;;) {
            int sleep_chunk = 10;
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

    if (ready >= 0) {
        if (rfds) memset(rfds, 0, alloc_bytes);
        if (wfds) memset(wfds, 0, alloc_bytes);
        if (efds) memset(efds, 0, alloc_bytes);
        int count = 0;
        for (int i = 0; i < npfds; i++) {
            if (pfds[i].revents == 0)
                continue;
            int fd = fd_map[i];
            int word = fd >> 6;
            uint64 bit = 1ULL << (fd & 63);
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
        if (rfds && either_copyout(1, readfds_addr, rfds, set_bytes) < 0)
            ready = -EFAULT;
        if (ready >= 0 && wfds && either_copyout(1, writefds_addr, wfds, set_bytes) < 0)
            ready = -EFAULT;
        if (ready >= 0 && efds && either_copyout(1, exceptfds_addr, efds, set_bytes) < 0)
            ready = -EFAULT;
    }

out:
    if (fd_map) kvfree(fd_map);
    if (pfds) kvfree(pfds);
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
    SYSCALL_PROFILE_BEGIN(g_sys_openat_calls);
    int dirfd;
    argint(0, &dirfd);

    struct vfs_inode *start_dir = NULL;
    int err = __vfs_resolve_dirfd(dirfd, &start_dir);
    if (err)
        SYSCALL_PROFILE_RETURN(err, g_sys_openat_ticks);

    // path is in a1, flags in a2, mode in a3
    char *path;
    char *name;
    int omode;
    int mode;
    int n;

    int path_ret = __vfs_argpath(1, &path, &n);
    if (path_ret < 0) {
        if (start_dir) vfs_iput(start_dir);
        SYSCALL_PROFILE_RETURN(path_ret, g_sys_openat_ticks);
    }
    name = __vfs_alloc_pathbuf();
    if (name == NULL) {
        if (start_dir) vfs_iput(start_dir);
        kvfree(path);
        SYSCALL_PROFILE_RETURN(-ENOMEM, g_sys_openat_ticks);
    }
    argint(2, &omode);
    argint(3, &mode);
    if (omode & O_PATH)
        omode &= (O_PATH | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);

#define SYS_VFS_OPENAT_RETURN(ret_expr)                                      \
    do {                                                                     \
        if (start_dir) vfs_iput(start_dir);                                  \
        kvfree(path);                                                        \
        kvfree(name);                                                        \
        SYSCALL_PROFILE_RETURN(ret_expr, g_sys_openat_ticks);                \
    } while (0)

    struct vfs_inode *inode = NULL;

    if ((omode & O_TMPFILE) == O_TMPFILE) {
        inode = vfs_namei_at(start_dir, path, n);
        if (IS_ERR(inode))
            SYS_VFS_OPENAT_RETURN(PTR_ERR(inode));
        if (inode == NULL)
            SYS_VFS_OPENAT_RETURN(-ENOENT);
        int is_dir = S_ISDIR(inode->mode);
        vfs_iput(inode);
        if (!is_dir)
            SYS_VFS_OPENAT_RETURN(-ENOTDIR);
        if ((omode & (O_WRONLY | O_RDWR)) == 0)
            SYS_VFS_OPENAT_RETURN(-EINVAL);
        SYS_VFS_OPENAT_RETURN(-EOPNOTSUPP);
    }

    if (omode & O_CREAT) {
        bool root_path = n > 0;
        for (int i = 0; root_path && i < n; i++)
            root_path = path[i] == '/';
        if (root_path)
            SYS_VFS_OPENAT_RETURN(-EISDIR);

        struct vfs_inode *parent =
            vfs_nameiparent_at(start_dir, path, n, name, VFS_USER_PATH_MAX);
        if (IS_ERR(parent)) {
            SYS_VFS_OPENAT_RETURN(PTR_ERR(parent));
        }
        if (parent == NULL) {
            SYS_VFS_OPENAT_RETURN(-ENOENT);
        }

        size_t name_len = strlen(name);
        if (name_len == 0) {
            vfs_iput(parent);
            SYS_VFS_OPENAT_RETURN(-EISDIR);
        }
        inode = vfs_create(parent, (mode_t)mode & ~current_umask(),
                           name, name_len);
        vfs_iput(parent);

        if (IS_ERR(inode)) {
            if (PTR_ERR(inode) == -EEXIST && !(omode & O_EXCL)) {
                inode = vfs_namei_at(start_dir, path, n);
                if (!IS_ERR_OR_NULL(inode) && S_ISDIR(inode->mode)) {
                    vfs_iput(inode);
                    SYS_VFS_OPENAT_RETURN(-EISDIR);
                }
            } else {
                SYS_VFS_OPENAT_RETURN(PTR_ERR(inode));
            }
        }
    } else {
        if (omode & O_NOFOLLOW) {
            struct vfs_inode *parent =
                vfs_nameiparent_at(start_dir, path, n, name, VFS_USER_PATH_MAX);
            if (IS_ERR(parent))
                SYS_VFS_OPENAT_RETURN(PTR_ERR(parent));
            if (parent == NULL)
                SYS_VFS_OPENAT_RETURN(-ENOENT);

            struct vfs_dentry dentry = {.sb = parent->sb, .parent = parent};
            int lr = vfs_ilookup(parent, &dentry, name, strlen(name));
            if (lr != 0) {
                vfs_iput(parent);
                SYS_VFS_OPENAT_RETURN(lr);
            }
            inode = vfs_get_dentry_inode(&dentry);
            vfs_release_dentry(&dentry);
            vfs_iput(parent);
            if (IS_ERR(inode))
                SYS_VFS_OPENAT_RETURN(PTR_ERR(inode));
            if (inode == NULL)
                SYS_VFS_OPENAT_RETURN(-ENOENT);
            if (S_ISLNK(inode->mode) && !(omode & O_PATH)) {
                vfs_iput(inode);
                SYS_VFS_OPENAT_RETURN(-ELOOP);
            }
        } else {
            inode = vfs_namei_at(start_dir, path, n);
            if (IS_ERR(inode))
                SYS_VFS_OPENAT_RETURN(PTR_ERR(inode));
            if (inode == NULL)
                SYS_VFS_OPENAT_RETURN(-ENOENT);
        }
    }

    if (start_dir) {
        vfs_iput(start_dir);
        start_dir = NULL;
    }

    if (IS_ERR(inode))
        SYS_VFS_OPENAT_RETURN(PTR_ERR(inode));
    if (inode == NULL)
        SYS_VFS_OPENAT_RETURN(-ENOENT);

    if (S_ISDIR(inode->mode) && (omode & (O_WRONLY | O_RDWR))) {
        vfs_iput(inode);
        SYS_VFS_OPENAT_RETURN(-EISDIR);
    }
    if ((omode & O_DIRECTORY) && !S_ISDIR(inode->mode)) {
        vfs_iput(inode);
        SYS_VFS_OPENAT_RETURN(-ENOTDIR);
    }

    struct vfs_file *f = vfs_fileopen(inode, omode);
    vfs_iput(inode);
    if (IS_ERR(f))
        SYS_VFS_OPENAT_RETURN(PTR_ERR(f));
    if (f == NULL)
        SYS_VFS_OPENAT_RETURN(-ENOMEM);

    if (!(omode & O_PATH) && (omode & O_TRUNC)) {
        truncate(f, 0);
    }

    spin_lock(&current->fdtable->lock);
    n = __vfs_fdalloc(f);
    if (n >= 0 && (omode & O_CLOEXEC)) {
        vfs_fdtable_set_fdflags(current->fdtable, n, FD_CLOEXEC);
    }
    spin_unlock(&current->fdtable->lock);

    if (n >= 0 && webkit_vfs_trace_enabled() && webkit_vfs_trace_process()) {
        const char *fs_name = "(none)";
        struct vfs_inode *opened_inode = vfs_inode_deref(&f->inode);
        if (opened_inode != NULL && opened_inode->sb != NULL &&
            opened_inode->sb->fs_type != NULL &&
            opened_inode->sb->fs_type->name != NULL)
            fs_name = opened_inode->sb->fs_type->name;
        printf("webkit-vfs: openat pid=%d name=%s fd=%d dirfd=%d path=%s "
               "flags=0x%x mode=0x%x ino=%lu fs=%s size=%lld\n",
               current->pid, current->name, n, dirfd, path, omode,
               opened_inode != NULL ? opened_inode->mode : 0,
               opened_inode != NULL ? opened_inode->ino : 0, fs_name,
               opened_inode != NULL ? opened_inode->size : 0);
    }

    vfs_fput(f);

    if (n < 0)
        SYS_VFS_OPENAT_RETURN(n);

    if (n >= 0)
        ACCT_INC(current->thread_group, fs_opens);
    SYS_VFS_OPENAT_RETURN(n);
#undef SYS_VFS_OPENAT_RETURN
}

uint64 sys_vfs_openat2(void) {
    struct linux_open_how {
        uint64 flags;
        uint64 mode;
        uint64 resolve;
    } how;
    int dirfd;
    uint64 how_addr;
    uint64 size;

    argint(0, &dirfd);
    argaddr(2, &how_addr);
    argaddr(3, &size);

    if (how_addr == 0)
        return (uint64)-EFAULT;
    if (size < sizeof(uint64) * 2)
        return (uint64)-EINVAL;
    memset(&how, 0, sizeof(how));
    uint64 copy_size = size;
    if (copy_size > sizeof(how))
        copy_size = sizeof(how);
    if (either_copyin(&how, 1, how_addr, copy_size) < 0)
        return (uint64)-EFAULT;
    if (how.resolve != 0)
        return (uint64)-EINVAL;
    if ((how.flags & O_CREAT) == 0 && how.mode != 0)
        return (uint64)-EINVAL;

    uint64 saved_rdx = current->trapframe->trapframe.rdx;
    uint64 saved_r10 = current->trapframe->trapframe.r10;
    arch_tf_set_arg2(current->trapframe, how.flags);
    arch_tf_set_arg3(current->trapframe, how.mode);
    uint64 ret = sys_vfs_openat();
    current->trapframe->trapframe.rdx = saved_rdx;
    current->trapframe->trapframe.r10 = saved_r10;
    return ret;
}

uint64 sys_vfs_close_range(void) {
    uint first;
    uint last;
    int flags;

    argint(0, (int *)&first);
    argint(1, (int *)&last);
    argint(2, &flags);

    const int CLOSE_RANGE_UNSHARE = 1 << 1;
    const int CLOSE_RANGE_CLOEXEC = 1 << 2;

    if (flags & ~(CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC))
        return (uint64)-EINVAL;
    if (first > last)
        return (uint64)-EINVAL;
    if (first >= NOFILE)
        return 0;
    if (last >= NOFILE)
        last = NOFILE - 1;

    if (flags & CLOSE_RANGE_UNSHARE) {
        struct vfs_fdtable *old = current->fdtable;
        struct vfs_fdtable *new = vfs_fdtable_clone(old, 0);
        if (IS_ERR_OR_NULL(new))
            return IS_ERR(new) ? (uint64)PTR_ERR(new) : (uint64)-ENOMEM;
        current->fdtable = new;
        vfs_fdtable_put(old);
    }

    if (flags & CLOSE_RANGE_CLOEXEC) {
        spin_lock(&current->fdtable->lock);
        for (uint fd = first; fd <= last; fd++)
            (void)vfs_fdtable_set_fdflags(current->fdtable, (int)fd,
                                          FD_CLOEXEC);
        spin_unlock(&current->fdtable->lock);
        return 0;
    }

    for (uint fd = first; fd <= last; fd++)
        __vfs_close_fd((int)fd);
    return 0;
}

uint64 sys_vfs_creat(void) {
    uint64 path_addr;
    int mode;
    argaddr(0, &path_addr);
    argint(1, &mode);

    uint64 saved_rdi = current->trapframe->trapframe.rdi;
    uint64 saved_rsi = current->trapframe->trapframe.rsi;
    uint64 saved_rdx = current->trapframe->trapframe.rdx;
    uint64 saved_r10 = current->trapframe->trapframe.r10;
    arch_tf_set_arg0(current->trapframe, AT_FDCWD);
    arch_tf_set_arg1(current->trapframe, path_addr);
    arch_tf_set_arg2(current->trapframe, O_CREAT | O_WRONLY | O_TRUNC);
    arch_tf_set_arg3(current->trapframe, mode);
    uint64 ret = sys_vfs_openat();
    current->trapframe->trapframe.rdi = saved_rdi;
    current->trapframe->trapframe.rsi = saved_rsi;
    current->trapframe->trapframe.rdx = saved_rdx;
    current->trapframe->trapframe.r10 = saved_r10;
    return ret;
}

uint64 sys_vfs_truncate(void) {
    char *path;
    int n;
    int64 length;
    argint64(1, &length);
    if (length < 0)
        return (uint64)-EINVAL;
    int ret = __vfs_argpath(0, &path, &n);
    if (ret < 0)
        return (uint64)ret;
    struct vfs_inode *inode = vfs_namei(path, n);
    kvfree(path);
    if (IS_ERR(inode))
        return (uint64)PTR_ERR(inode);
    if (inode == NULL)
        return (uint64)-ENOENT;
    if (!S_ISREG(inode->mode)) {
        vfs_iput(inode);
        return (uint64)-EINVAL;
    }
    ret = vfs_itruncate(inode, length);
    vfs_iput(inode);
    return (uint64)ret;
}

uint64 sys_vfs_rmdir(void) {
    uint64 path_addr;
    argaddr(0, &path_addr);

    uint64 saved_rdi = current->trapframe->trapframe.rdi;
    uint64 saved_rsi = current->trapframe->trapframe.rsi;
    uint64 saved_rdx = current->trapframe->trapframe.rdx;
    arch_tf_set_arg0(current->trapframe, AT_FDCWD);
    arch_tf_set_arg1(current->trapframe, path_addr);
    arch_tf_set_arg2(current->trapframe, AT_REMOVEDIR);
    uint64 ret = sys_vfs_unlinkat();
    current->trapframe->trapframe.rdi = saved_rdi;
    current->trapframe->trapframe.rsi = saved_rsi;
    current->trapframe->trapframe.rdx = saved_rdx;
    return ret;
}

uint64 sys_vfs_fchdir(void) {
    int fd;
    argint(0, &fd);
    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        return (uint64)-EBADF;
    struct vfs_inode *inode = vfs_inode_deref(&f->inode);
    if (inode == NULL || !S_ISDIR(inode->mode)) {
        vfs_fput(f);
        return (uint64)-ENOTDIR;
    }
    struct vfs_inode_ref new_cwd_ref;
    int ret = vfs_inode_get_ref(inode, &new_cwd_ref);
    if (ret != 0) {
        vfs_fput(f);
        return (uint64)ret;
    }
    vfs_struct_lock(current->fs);
    struct vfs_inode_ref old_cwd = current->fs->cwd;
    current->fs->cwd = new_cwd_ref;
    vfs_struct_unlock(current->fs);
    vfs_inode_put_ref(&old_cwd);
    vfs_fput(f);
    return 0;
}

/* Forward declaration — defined below after pread64/pwrite64 */
static int
__preadwritev_copyin(uint64 iov_addr, int iovcnt,
                     struct kernel_iovec stack_iovs[UIO_FASTIOV],
                     struct kernel_iovec **piov,
                     struct kernel_iovec **pheap,
                     struct iov_iter *iter);

/*
 * writev(fd, iov, iovcnt) — scatter-gather write.
 *
 * Uses the unified vfs_filewritev() path which dispatches to the
 * native .writev callback if available, or falls back to per-segment
 * .write / cdev_write.
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

    struct kernel_iovec stack_iovs[UIO_FASTIOV];
    struct kernel_iovec *iov, *heap_iov;
    struct iov_iter iter;
    int err = __preadwritev_copyin(iov_addr, iovcnt, stack_iovs,
                                   &iov, &heap_iov, &iter);
    if (err) {
        vfs_fput(f);
        return err;
    }

    ssize_t ret = vfs_filewritev(f, &iter, true);

    vfs_fput(f);
    if (heap_iov) uio_iovec_free_ex(heap_iov, iovcnt);
    if (ret > 0)
        ACCT_ADD(current->thread_group, fs_bytes_written, (uint64)ret);
    return ret;
}

/*
 * readv(fd, iov, iovcnt) — scatter-gather read.
 *
 * Uses the unified vfs_filereadv() path which dispatches to the
 * native .readv callback if available, or falls back to per-segment
 * .read / cdev_read.
 */
uint64 sys_vfs_readv(void) {
    SYSCALL_PROFILE_BEGIN(g_sys_readv_calls);
    int fd, iovcnt;
    uint64 iov_addr;

    argint(0, &fd);
    argaddr(1, &iov_addr);
    argint(2, &iovcnt);

    if (iovcnt <= 0 || iovcnt > UIO_MAXIOV)
        SYSCALL_PROFILE_RETURN(-EINVAL, g_sys_readv_ticks);

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        SYSCALL_PROFILE_RETURN(-EBADF, g_sys_readv_ticks);

    struct kernel_iovec stack_iovs[UIO_FASTIOV];
    struct kernel_iovec *iov, *heap_iov;
    struct iov_iter iter;
    int err = __preadwritev_copyin(iov_addr, iovcnt, stack_iovs,
                                   &iov, &heap_iov, &iter);
    if (err) {
        vfs_fput(f);
        SYSCALL_PROFILE_RETURN(err, g_sys_readv_ticks);
    }

    ssize_t ret = vfs_filereadv(f, &iter, true);

    vfs_fput(f);
    if (heap_iov) uio_iovec_free_ex(heap_iov, iovcnt);
    if (ret > 0)
        ACCT_ADD(current->thread_group, fs_bytes_read, (uint64)ret);
    SYSCALL_PROFILE_RETURN(ret, g_sys_readv_ticks);
}

/*
 * pread64(fd, buf, count, offset) — read at position without changing file offset.
 *
 * Atomically: save f_pos, seek to offset, read, restore f_pos — all under
 * the file lock so concurrent read/write/lseek can't interleave.
 */
uint64 sys_vfs_pread64(void) {
    SYSCALL_PROFILE_BEGIN(g_sys_pread64_calls);
    int fd, count;
    uint64 buf_addr;
    int64 offset;

    argint(0, &fd);
    argaddr(1, &buf_addr);
    argint(2, &count);
    argint64(3, &offset);

    if (offset < 0)
        SYSCALL_PROFILE_RETURN(-EINVAL, g_sys_pread64_ticks);

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        SYSCALL_PROFILE_RETURN(-EBADF, g_sys_pread64_ticks);

    struct vfs_inode *inode = vfs_inode_deref(&f->inode);
    if (inode == NULL || !S_ISREG(inode->mode)) {
        vfs_fput(f);
        SYSCALL_PROFILE_RETURN(-ESPIPE, g_sys_pread64_ticks);
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
    SYSCALL_PROFILE_RETURN(ret, g_sys_pread64_ticks);
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
 * Helper: copy user-space iovec array into kernel_iovec, validate, and
 * build an iov_iter.  Shared by preadv/pwritev/preadv2/pwritev2.
 *
 * On success returns 0 and fills *iter, *piov (heap allocation or NULL),
 * and *pstack (stack array — caller must keep in scope).
 * On failure returns a negative errno.
 */
static int
__preadwritev_copyin(uint64 iov_addr, int iovcnt,
                     struct kernel_iovec stack_iovs[UIO_FASTIOV],
                     struct kernel_iovec **piov,
                     struct kernel_iovec **pheap,
                     struct iov_iter *iter)
{
    if (iovcnt <= 0 || iovcnt > UIO_MAXIOV)
        return -EINVAL;

    struct kernel_iovec *iov = stack_iovs;
    struct kernel_iovec *heap_iov = NULL;

    if (iovcnt > UIO_FASTIOV) {
        heap_iov = uio_iovec_alloc(iovcnt);
        if (heap_iov == NULL)
            return -ENOMEM;
        iov = heap_iov;
    }

    if (either_copyin(iov, 1, iov_addr,
                      iovcnt * sizeof(struct kernel_iovec)) < 0) {
        if (heap_iov) uio_iovec_free_ex(heap_iov, iovcnt);
        return -EFAULT;
    }

    size_t total_len = iov_iter_total_len(iov, iovcnt);
    if (total_len == (size_t)-1) {
        if (heap_iov) uio_iovec_free_ex(heap_iov, iovcnt);
        return -EINVAL;
    }

    iov_iter_init(iter, iov, iovcnt, total_len);
    *piov = iov;
    *pheap = heap_iov;
    return 0;
}

/*
 * preadv(fd, iov, iovcnt, offset) — scatter-gather read at position.
 *
 * Like pread64 but with an iovec array.  Does not change file offset.
 */
uint64 sys_vfs_preadv(void) {
    int fd, iovcnt;
    uint64 iov_addr;
    int64 offset;

    argint(0, &fd);
    argaddr(1, &iov_addr);
    argint(2, &iovcnt);
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

    struct kernel_iovec stack_iovs[UIO_FASTIOV];
    struct kernel_iovec *iov, *heap_iov;
    struct iov_iter iter;
    int err = __preadwritev_copyin(iov_addr, iovcnt, stack_iovs,
                                   &iov, &heap_iov, &iter);
    if (err) {
        vfs_fput(f);
        return err;
    }

    mutex_lock(&f->lock);
    loff_t saved = f->f_pos;
    f->f_pos = offset;

    ssize_t ret;
    if (f->ops == NULL)
        ret = -EOPNOTSUPP;
    else if (f->ops->readv)
        ret = f->ops->readv(f, &iter, true);
    else if (f->ops->read) {
        /* Generic fallback: per-segment read */
        ret = 0;
        for (int i = 0; i < iovcnt && iter.count > 0; i++) {
            if (iov[i].iov_len == 0)
                continue;
            ssize_t n = f->ops->read(f, (void *)iov[i].iov_base,
                                     iov[i].iov_len, true);
            if (n < 0) {
                if (ret == 0) ret = n;
                break;
            }
            ret += n;
            if ((uint64)n < iov[i].iov_len)
                break;
        }
    } else
        ret = -EOPNOTSUPP;

    f->f_pos = saved;
    mutex_unlock(&f->lock);

    if (heap_iov) uio_iovec_free_ex(heap_iov, iovcnt);
    if (ret > 0)
        ACCT_ADD(current->thread_group, fs_bytes_read, (uint64)ret);
    vfs_fput(f);
    return ret;
}

/*
 * pwritev(fd, iov, iovcnt, offset) — scatter-gather write at position.
 *
 * Like pwrite64 but with an iovec array.  Does not change file offset.
 */
uint64 sys_vfs_pwritev(void) {
    int fd, iovcnt;
    uint64 iov_addr;
    int64 offset;

    argint(0, &fd);
    argaddr(1, &iov_addr);
    argint(2, &iovcnt);
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

    struct kernel_iovec stack_iovs[UIO_FASTIOV];
    struct kernel_iovec *iov, *heap_iov;
    struct iov_iter iter;
    int err = __preadwritev_copyin(iov_addr, iovcnt, stack_iovs,
                                   &iov, &heap_iov, &iter);
    if (err) {
        vfs_fput(f);
        return err;
    }

    mutex_lock(&f->lock);
    loff_t saved = f->f_pos;
    f->f_pos = offset;

    ssize_t ret;
    if (f->ops == NULL)
        ret = -EOPNOTSUPP;
    else if (f->ops->writev)
        ret = f->ops->writev(f, &iter, true);
    else if (f->ops->write) {
        /* Generic fallback: per-segment write */
        ret = 0;
        for (int i = 0; i < iovcnt && iter.count > 0; i++) {
            if (iov[i].iov_len == 0)
                continue;
            ssize_t n = f->ops->write(f, (const void *)iov[i].iov_base,
                                      iov[i].iov_len, true);
            if (n < 0) {
                if (ret == 0) ret = n;
                break;
            }
            ret += n;
            if ((uint64)n < iov[i].iov_len)
                break;
        }
    } else
        ret = -EOPNOTSUPP;

    f->f_pos = saved;
    mutex_unlock(&f->lock);

    if (heap_iov) uio_iovec_free_ex(heap_iov, iovcnt);
    if (ret > 0)
        ACCT_ADD(current->thread_group, fs_bytes_written, (uint64)ret);
    vfs_fput(f);
    return ret;
}

/*
 * preadv2(fd, iov, iovcnt, offset, flags) — scatter-gather read at position
 * with flags.
 *
 * When offset == -1, uses the current file position (like readv).
 * Flags: RWF_HIPRI, RWF_DSYNC, RWF_SYNC, RWF_NOWAIT, RWF_APPEND.
 * RWF_NOWAIT causes -EAGAIN when a page is not already cached.
 */
uint64 sys_vfs_preadv2(void) {
    int fd, iovcnt, flags;
    uint64 iov_addr;
    int64 offset;

    argint(0, &fd);
    argaddr(1, &iov_addr);
    argint(2, &iovcnt);
    argint64(3, &offset);
    argint(4, &flags);

    /* Reject unknown flags */
    if (flags & ~RWF_SUPPORTED)
        return -EOPNOTSUPP;

    if (offset < -1)
        return -EINVAL;

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        return -EBADF;

    /* offset == -1 means "use current file position" (like readv) */
    if (offset == -1) {
        struct kernel_iovec stack_iovs[UIO_FASTIOV];
        struct kernel_iovec *iov, *heap_iov;
        struct iov_iter iter;
        int err = __preadwritev_copyin(iov_addr, iovcnt, stack_iovs,
                                       &iov, &heap_iov, &iter);
        if (err) {
            vfs_fput(f);
            return err;
        }

        iter.flags = (unsigned int)flags;
        ssize_t ret = vfs_filereadv(f, &iter, true);

        if (heap_iov) uio_iovec_free_ex(heap_iov, iovcnt);
        if (ret > 0)
            ACCT_ADD(current->thread_group, fs_bytes_read, (uint64)ret);
        vfs_fput(f);
        return ret;
    }

    /* Positional read — must be regular file */
    struct vfs_inode *inode = vfs_inode_deref(&f->inode);
    if (inode == NULL || !S_ISREG(inode->mode)) {
        vfs_fput(f);
        return -ESPIPE;
    }

    struct kernel_iovec stack_iovs[UIO_FASTIOV];
    struct kernel_iovec *iov, *heap_iov;
    struct iov_iter iter;
    int err = __preadwritev_copyin(iov_addr, iovcnt, stack_iovs,
                                   &iov, &heap_iov, &iter);
    if (err) {
        vfs_fput(f);
        return err;
    }

    iter.flags = (unsigned int)flags;

    mutex_lock(&f->lock);
    loff_t saved = f->f_pos;
    f->f_pos = offset;

    ssize_t ret;
    if (f->ops == NULL)
        ret = -EOPNOTSUPP;
    else if (f->ops->readv)
        ret = f->ops->readv(f, &iter, true);
    else if (f->ops->read) {
        ret = 0;
        for (int i = 0; i < iovcnt && iter.count > 0; i++) {
            if (iov[i].iov_len == 0)
                continue;
            ssize_t n = f->ops->read(f, (void *)iov[i].iov_base,
                                     iov[i].iov_len, true);
            if (n < 0) {
                if (ret == 0) ret = n;
                break;
            }
            ret += n;
            if ((uint64)n < iov[i].iov_len)
                break;
        }
    } else
        ret = -EOPNOTSUPP;

    f->f_pos = saved;
    mutex_unlock(&f->lock);

    if (heap_iov) uio_iovec_free_ex(heap_iov, iovcnt);
    if (ret > 0)
        ACCT_ADD(current->thread_group, fs_bytes_read, (uint64)ret);
    vfs_fput(f);
    return ret;
}

/*
 * pwritev2(fd, iov, iovcnt, offset, flags) — scatter-gather write at position
 * with flags.
 *
 * When offset == -1, uses the current file position (like writev).
 * Flags: RWF_HIPRI, RWF_DSYNC, RWF_SYNC, RWF_NOWAIT, RWF_APPEND.
 * RWF_NOWAIT causes -EAGAIN when a page is not already cached.
 * RWF_APPEND could later seek to end before writing.
 */
uint64 sys_vfs_pwritev2(void) {
    int fd, iovcnt, flags;
    uint64 iov_addr;
    int64 offset;

    argint(0, &fd);
    argaddr(1, &iov_addr);
    argint(2, &iovcnt);
    argint64(3, &offset);
    argint(4, &flags);

    /* Reject unknown flags */
    if (flags & ~RWF_SUPPORTED)
        return -EOPNOTSUPP;

    if (offset < -1)
        return -EINVAL;

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        return -EBADF;

    /* offset == -1 means "use current file position" (like writev) */
    if (offset == -1) {
        struct kernel_iovec stack_iovs[UIO_FASTIOV];
        struct kernel_iovec *iov, *heap_iov;
        struct iov_iter iter;
        int err = __preadwritev_copyin(iov_addr, iovcnt, stack_iovs,
                                       &iov, &heap_iov, &iter);
        if (err) {
            vfs_fput(f);
            return err;
        }

        iter.flags = (unsigned int)flags;
        ssize_t ret = vfs_filewritev(f, &iter, true);

        if (heap_iov) uio_iovec_free_ex(heap_iov, iovcnt);
        if (ret > 0)
            ACCT_ADD(current->thread_group, fs_bytes_written, (uint64)ret);
        vfs_fput(f);
        return ret;
    }

    /* Positional write — must be regular file */
    struct vfs_inode *inode = vfs_inode_deref(&f->inode);
    if (inode == NULL || !S_ISREG(inode->mode)) {
        vfs_fput(f);
        return -ESPIPE;
    }

    struct kernel_iovec stack_iovs[UIO_FASTIOV];
    struct kernel_iovec *iov, *heap_iov;
    struct iov_iter iter;
    int err = __preadwritev_copyin(iov_addr, iovcnt, stack_iovs,
                                   &iov, &heap_iov, &iter);
    if (err) {
        vfs_fput(f);
        return err;
    }

    iter.flags = (unsigned int)flags;

    mutex_lock(&f->lock);
    loff_t saved = f->f_pos;
    f->f_pos = offset;

    ssize_t ret;
    if (f->ops == NULL)
        ret = -EOPNOTSUPP;
    else if (f->ops->writev)
        ret = f->ops->writev(f, &iter, true);
    else if (f->ops->write) {
        ret = 0;
        for (int i = 0; i < iovcnt && iter.count > 0; i++) {
            if (iov[i].iov_len == 0)
                continue;
            ssize_t n = f->ops->write(f, (const void *)iov[i].iov_base,
                                      iov[i].iov_len, true);
            if (n < 0) {
                if (ret == 0) ret = n;
                break;
            }
            ret += n;
            if ((uint64)n < iov[i].iov_len)
                break;
        }
    } else
        ret = -EOPNOTSUPP;

    f->f_pos = saved;
    mutex_unlock(&f->lock);

    if (heap_iov) uio_iovec_free_ex(heap_iov, iovcnt);
    if (ret > 0)
        ACCT_ADD(current->thread_group, fs_bytes_written, (uint64)ret);
    vfs_fput(f);
    return ret;
}

/*
 * fstatat(dirfd, path, statbuf, flags) — stat relative to directory fd.
 */
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef AT_NO_AUTOMOUNT
#define AT_NO_AUTOMOUNT 0x800
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

uint64 sys_vfs_fstatat(void) {
    SYSCALL_PROFILE_BEGIN(g_sys_fstatat_calls);
    int dirfd, flags;
    uint64 stat_addr;
    char *path;
    int path_len;

    argint(0, &dirfd);
    int path_ret = __vfs_argpath(1, &path, &path_len);
    if (path_ret < 0)
        SYSCALL_PROFILE_RETURN(path_ret, g_sys_fstatat_ticks);
    argaddr(2, &stat_addr);
    argint(3, &flags);

    const int allowed_flags = AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT |
                              AT_EMPTY_PATH;
    if (flags & ~allowed_flags) {
        kvfree(path);
        SYSCALL_PROFILE_RETURN(-EINVAL, g_sys_fstatat_ticks);
    }

    if ((flags & AT_EMPTY_PATH) && path_len == 0) {
        struct vfs_file *f = __vfs_argfd(dirfd);
        if (f == NULL) {
            kvfree(path);
            SYSCALL_PROFILE_RETURN(-EBADF, g_sys_fstatat_ticks);
        }

        struct stat st;
        int ret = vfs_filestat(f, &st);
        vfs_fput(f);
        kvfree(path);
        if (ret != 0)
            SYSCALL_PROFILE_RETURN(ret, g_sys_fstatat_ticks);

        if (either_copyout(1, stat_addr, &st, sizeof(st)) < 0)
            SYSCALL_PROFILE_RETURN(-EFAULT, g_sys_fstatat_ticks);
        SYSCALL_PROFILE_RETURN(0, g_sys_fstatat_ticks);
    }

    struct vfs_inode *start_dir = NULL;
    int err = __vfs_resolve_dirfd(dirfd, &start_dir);
    if (err) {
        kvfree(path);
        SYSCALL_PROFILE_RETURN(err, g_sys_fstatat_ticks);
    }

    struct vfs_inode *inode = NULL;

    if (flags & AT_SYMLINK_NOFOLLOW) {
        char *name = __vfs_alloc_pathbuf();
        if (name == NULL) {
            if (start_dir) vfs_iput(start_dir);
            kvfree(path);
            SYSCALL_PROFILE_RETURN(-ENOMEM, g_sys_fstatat_ticks);
        }
        struct vfs_inode *parent = vfs_nameiparent_at(start_dir, path, path_len, name, VFS_USER_PATH_MAX);
        if (start_dir) vfs_iput(start_dir);
        if (IS_ERR(parent)) {
            kvfree(name);
            kvfree(path);
            SYSCALL_PROFILE_RETURN(PTR_ERR(parent), g_sys_fstatat_ticks);
        }
        if (parent == NULL) {
            kvfree(name);
            kvfree(path);
            SYSCALL_PROFILE_RETURN(-ENOENT, g_sys_fstatat_ticks);
        }

        struct vfs_dentry dentry = {.sb = parent->sb, .parent = parent};
        int ret = vfs_ilookup(parent, &dentry, name, strlen(name));
        kvfree(name);
        if (ret != 0) {
            vfs_iput(parent);
            kvfree(path);
            SYSCALL_PROFILE_RETURN(ret, g_sys_fstatat_ticks);
        }
        inode = vfs_get_dentry_inode(&dentry);
        vfs_release_dentry(&dentry);
        vfs_iput(parent);
    } else {
        /* vfs_namei follows all symlinks automatically */
        inode = vfs_namei_at(start_dir, path, path_len);
        if (start_dir) vfs_iput(start_dir);
    }
    kvfree(path);

    if (IS_ERR(inode))
        SYSCALL_PROFILE_RETURN(PTR_ERR(inode), g_sys_fstatat_ticks);
    if (inode == NULL)
        SYSCALL_PROFILE_RETURN(-ENOENT, g_sys_fstatat_ticks);

    struct stat st;
    int ret = __vfs_inode_stat(inode, &st);
    vfs_iput(inode);
    if (ret != 0)
        SYSCALL_PROFILE_RETURN(ret, g_sys_fstatat_ticks);

    // Copy the 128-byte struct stat directly to userspace.
    // Kernel struct stat layout matches musl's riscv64 layout exactly.
    if (either_copyout(1, stat_addr, &st, sizeof(st)) < 0)
        SYSCALL_PROFILE_RETURN(-EFAULT, g_sys_fstatat_ticks);

    SYSCALL_PROFILE_RETURN(0, g_sys_fstatat_ticks);
}

struct statx_timestamp {
    int64 tv_sec;
    uint32 tv_nsec;
    int32 __reserved;
};

struct statx {
    uint32 stx_mask;
    uint32 stx_blksize;
    uint64 stx_attributes;
    uint32 stx_nlink;
    uint32 stx_uid;
    uint32 stx_gid;
    uint16 stx_mode;
    uint16 __spare0[1];
    uint64 stx_ino;
    uint64 stx_size;
    uint64 stx_blocks;
    uint64 stx_attributes_mask;
    struct statx_timestamp stx_atime;
    struct statx_timestamp stx_btime;
    struct statx_timestamp stx_ctime;
    struct statx_timestamp stx_mtime;
    uint32 stx_rdev_major;
    uint32 stx_rdev_minor;
    uint32 stx_dev_major;
    uint32 stx_dev_minor;
    uint64 stx_mnt_id;
    uint32 stx_dio_mem_align;
    uint32 stx_dio_offset_align;
    uint64 __spare3[12];
};

#define STATX_TYPE        0x00000001U
#define STATX_MODE        0x00000002U
#define STATX_NLINK       0x00000004U
#define STATX_UID         0x00000008U
#define STATX_GID         0x00000010U
#define STATX_ATIME       0x00000020U
#define STATX_MTIME       0x00000040U
#define STATX_CTIME       0x00000080U
#define STATX_INO         0x00000100U
#define STATX_SIZE        0x00000200U
#define STATX_BLOCKS      0x00000400U
#define STATX_BASIC_STATS 0x000007ffU
#define STATX_BTIME       0x00000800U

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH     0x1000
#endif

static void stat_to_statx(const struct stat *st, struct statx *stx)
{
    memset(stx, 0, sizeof(*stx));

    stx->stx_mask = STATX_BASIC_STATS;
    stx->stx_blksize = st->st_blksize > 0 ? (uint32)st->st_blksize : 4096;
    stx->stx_nlink = (uint32)st->st_nlink;
    stx->stx_uid = st->st_uid;
    stx->stx_gid = st->st_gid;
    stx->stx_mode = (uint16)st->st_mode;
    stx->stx_ino = st->st_ino;
    stx->stx_size = st->st_size;
    stx->stx_blocks = st->st_blocks;

    stx->stx_atime.tv_sec = st->st_atime_sec;
    stx->stx_atime.tv_nsec = (uint32)st->st_atime_nsec;
    stx->stx_mtime.tv_sec = st->st_mtime_sec;
    stx->stx_mtime.tv_nsec = (uint32)st->st_mtime_nsec;
    stx->stx_ctime.tv_sec = st->st_ctime_sec;
    stx->stx_ctime.tv_nsec = (uint32)st->st_ctime_nsec;

    stx->stx_rdev_major = major(st->st_rdev);
    stx->stx_rdev_minor = minor(st->st_rdev);
    stx->stx_dev_major = major(st->st_dev);
    stx->stx_dev_minor = minor(st->st_dev);
}

uint64 sys_statx(void)
{
    int dirfd;
    int flags;
    int mask;
    uint64 statx_addr;
    char *path;
    int path_len;
    struct stat st;
    int ret;

    argint(0, &dirfd);
    ret = __vfs_argpath(1, &path, &path_len);
    if (ret < 0)
        return (uint64)ret;
    argint(2, &flags);
    argint(3, &mask);
    argaddr(4, &statx_addr);

    (void)mask;

    if ((flags & AT_EMPTY_PATH) && path[0] == '\0') {
        struct vfs_file *f = __vfs_argfd(dirfd);
        if (f == NULL) {
            kvfree(path);
            return (uint64)-EBADF;
        }

        ret = vfs_filestat(f, &st);
        vfs_fput(f);
        if (ret < 0) {
            kvfree(path);
            return (uint64)ret;
        }
    } else {
        struct vfs_inode *start_dir = NULL;

        ret = __vfs_resolve_dirfd(dirfd, &start_dir);
        if (ret < 0) {
            kvfree(path);
            return (uint64)ret;
        }

        struct vfs_inode *inode = NULL;

        if (flags & AT_SYMLINK_NOFOLLOW) {
            char *name = __vfs_alloc_pathbuf();
            if (name == NULL) {
                if (start_dir) vfs_iput(start_dir);
                kvfree(path);
                return (uint64)-ENOMEM;
            }
            struct vfs_inode *parent = vfs_nameiparent_at(start_dir, path, path_len, name, VFS_USER_PATH_MAX);
            if (start_dir) vfs_iput(start_dir);
            if (IS_ERR(parent)) {
                kvfree(name);
                kvfree(path);
                return (uint64)PTR_ERR(parent);
            }
            if (parent == NULL) {
                kvfree(name);
                kvfree(path);
                return (uint64)-ENOENT;
            }

            struct vfs_dentry dentry = { .sb = parent->sb, .parent = parent };
            ret = vfs_ilookup(parent, &dentry, name, strlen(name));
            kvfree(name);
            if (ret == 0)
                inode = vfs_get_dentry_inode(&dentry);
            vfs_release_dentry(&dentry);
            vfs_iput(parent);
            if (ret != 0) {
                kvfree(path);
                return (uint64)ret;
            }
        } else {
            inode = vfs_namei_at(start_dir, path, path_len);
            if (start_dir) vfs_iput(start_dir);
        }
        kvfree(path);
        path = NULL;

        if (IS_ERR(inode))
            return (uint64)PTR_ERR(inode);
        if (inode == NULL)
            return (uint64)-ENOENT;

        ret = __vfs_inode_stat(inode, &st);
        vfs_iput(inode);
        if (ret < 0)
            return (uint64)ret;
    }
    if (path)
        kvfree(path);

    struct statx stx;
    stat_to_statx(&st, &stx);

    if (either_copyout(1, statx_addr, &stx, sizeof(stx)) < 0)
        return (uint64)-EFAULT;

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
        spin_lock(&current->fdtable->lock);
        vfs_fdtable_set_fdflags(current->fdtable, fd0, FD_CLOEXEC);
        vfs_fdtable_set_fdflags(current->fdtable, fd1, FD_CLOEXEC);
        spin_unlock(&current->fdtable->lock);
    }

    return 0;
}

/* ===========================================================================
 * Linux-compatible *at() syscall variants for musl libc
 *
 * These accept a dirfd as the first argument. AT_FDCWD (-100) means "use
 * current working directory." A real fd must refer to a directory.
 * ===========================================================================
 */

#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif

/**
 * sys_vfs_mkdirat - Create a directory relative to dirfd
 * Args: a0=dirfd, a1=path, a2=mode
 */
uint64 sys_vfs_mkdirat(void) {
    int dirfd;
    char *path;
    char *name;
    int mode;

    argint(0, &dirfd);

    struct vfs_inode *start_dir = NULL;
    int err = __vfs_resolve_dirfd(dirfd, &start_dir);
    if (err)
        return err;

    int n;
    int path_ret = __vfs_argpath(1, &path, &n);
    argint(2, &mode);
    if (path_ret < 0) {
        if (start_dir) vfs_iput(start_dir);
        return path_ret;
    }
    name = __vfs_alloc_pathbuf();
    if (name == NULL) {
        if (start_dir) vfs_iput(start_dir);
        kvfree(path);
        return -ENOMEM;
    }

    struct vfs_inode *parent =
        vfs_nameiparent_at(start_dir, path, n, name, VFS_USER_PATH_MAX);
    if (start_dir) vfs_iput(start_dir);
    kvfree(path);
    if (IS_ERR(parent)) {
        kvfree(name);
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        kvfree(name);
        return -ENOENT;
    }

    struct vfs_inode *dir = vfs_mkdir(parent, (mode_t)mode, name, strlen(name));
    kvfree(name);
    vfs_iput(parent);
    if (IS_ERR(dir))
        return PTR_ERR(dir);
    vfs_iput(dir);
    ACCT_INC(current->thread_group, fs_creates);
    return 0;
}

/**
 * sys_vfs_mknodat - Create a special file relative to dirfd
 * Args: a0=dirfd, a1=path, a2=mode, a3=dev
 *
 * musl packs major/minor into a single dev_t. xv6 also uses mkdev().
 */
uint64 sys_vfs_mknodat(void) {
    if (!capable())
        return (uint64)-EPERM;

    int dirfd;
    char *path;
    char *name;
    int mode;
    uint64 dev;

    argint(0, &dirfd);

    struct vfs_inode *start_dir = NULL;
    int err = __vfs_resolve_dirfd(dirfd, &start_dir);
    if (err)
        return err;

    int n;
    int path_ret = __vfs_argpath(1, &path, &n);
    argint(2, &mode);
    argaddr(3, &dev);
    if (path_ret < 0) {
        if (start_dir) vfs_iput(start_dir);
        return path_ret;
    }
    name = __vfs_alloc_pathbuf();
    if (name == NULL) {
        if (start_dir) vfs_iput(start_dir);
        kvfree(path);
        return -ENOMEM;
    }

    struct vfs_inode *parent =
        vfs_nameiparent_at(start_dir, path, n, name, VFS_USER_PATH_MAX);
    if (start_dir) vfs_iput(start_dir);
    kvfree(path);
    if (IS_ERR(parent)) {
        kvfree(name);
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        kvfree(name);
        return -ENOENT;
    }

    struct vfs_inode *node =
        vfs_mknod(parent, (mode_t)mode, (dev_t)dev, name, strlen(name));
    kvfree(name);
    vfs_iput(parent);
    if (IS_ERR(node))
        return PTR_ERR(node);
    vfs_iput(node);
    return 0;
}

/**
 * sys_vfs_unlinkat - Remove a file or directory relative to dirfd
 * Args: a0=dirfd, a1=path, a2=flags (AT_REMOVEDIR for rmdir)
 */
uint64 sys_vfs_unlinkat(void) {
    int dirfd, flags;
    char *path;
    char *name;

    argint(0, &dirfd);

    struct vfs_inode *start_dir = NULL;
    int err = __vfs_resolve_dirfd(dirfd, &start_dir);
    if (err)
        return err;

    int n;
    int path_ret = __vfs_argpath(1, &path, &n);
    argint(2, &flags);
    if (path_ret < 0) {
        if (start_dir) vfs_iput(start_dir);
        return path_ret;
    }
    name = __vfs_alloc_pathbuf();
    if (name == NULL) {
        if (start_dir) vfs_iput(start_dir);
        kvfree(path);
        return -ENOMEM;
    }

    struct vfs_inode *parent =
        vfs_nameiparent_at(start_dir, path, n, name, VFS_USER_PATH_MAX);
    if (start_dir) vfs_iput(start_dir);
    kvfree(path);
    if (IS_ERR(parent)) {
        kvfree(name);
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        kvfree(name);
        return -ENOENT;
    }

    (void)flags;  /* AT_REMOVEDIR handled by vfs_unlink */
    int ret = vfs_unlink(parent, name, strlen(name));
    kvfree(name);
    vfs_iput(parent);
    if (ret == 0)
        ACCT_INC(current->thread_group, fs_deletes);
    return ret;
}

/**
 * sys_vfs_linkat - Create a hard link relative to dirfds
 * Args: a0=olddirfd, a1=oldpath, a2=newdirfd, a3=newpath, a4=flags
 */
uint64 sys_vfs_linkat(void) {
    int olddirfd, newdirfd, flags;
    char old[MAXPATH], new[MAXPATH];
    char name[MAXPATH];

    argint(0, &olddirfd);
    argint(2, &newdirfd);
    argint(4, &flags);

    struct vfs_inode *old_start = NULL, *new_start = NULL;
    int err = __vfs_resolve_dirfd(olddirfd, &old_start);
    if (err)
        return err;
    err = __vfs_resolve_dirfd(newdirfd, &new_start);
    if (err) {
        if (old_start) vfs_iput(old_start);
        return err;
    }

    int n1 = argstr(1, old, MAXPATH);
    int n2 = argstr(3, new, MAXPATH);
    if (n1 < 0 || n2 < 0) {
        if (old_start) vfs_iput(old_start);
        if (new_start) vfs_iput(new_start);
        return -EFAULT;
    }

    struct vfs_inode *src = vfs_namei_at(old_start, old, n1);
    if (old_start) vfs_iput(old_start);
    if (IS_ERR(src)) {
        if (new_start) vfs_iput(new_start);
        return PTR_ERR(src);
    }
    if (src == NULL) {
        if (new_start) vfs_iput(new_start);
        return -ENOENT;
    }
    if (S_ISDIR(src->mode)) {
        vfs_iput(src);
        if (new_start) vfs_iput(new_start);
        return -EPERM;
    }

    struct vfs_inode *parent = vfs_nameiparent_at(new_start, new, n2, name, MAXPATH);
    if (new_start) vfs_iput(new_start);
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
 * sys_vfs_symlinkat - Create a symbolic link relative to dirfd
 * Args: a0=target, a1=newdirfd, a2=linkpath
 *
 * Note: Linux symlinkat has (target, newdirfd, linkpath) ordering.
 */
uint64 sys_vfs_symlinkat(void) {
    int newdirfd;
    char target[MAXPATH], linkpath[MAXPATH];
    char name[MAXPATH];

    argint(1, &newdirfd);

    struct vfs_inode *start_dir = NULL;
    int err = __vfs_resolve_dirfd(newdirfd, &start_dir);
    if (err)
        return err;

    int n1 = argstr(0, target, MAXPATH);
    int n2 = argstr(2, linkpath, MAXPATH);
    if (n1 < 0 || n2 < 0) {
        if (start_dir) vfs_iput(start_dir);
        return -EFAULT;
    }

    struct vfs_inode *parent = vfs_nameiparent_at(start_dir, linkpath, n2, name, MAXPATH);
    if (start_dir) vfs_iput(start_dir);
    if (IS_ERR(parent))
        return PTR_ERR(parent);
    if (parent == NULL)
        return -ENOENT;

    struct vfs_inode *sym =
        vfs_symlink(parent, 0777, name, strlen(name), target, strlen(target));
    vfs_iput(parent);
    if (IS_ERR(sym))
        return PTR_ERR(sym);
    vfs_iput(sym);
    ACCT_INC(current->thread_group, fs_links);
    return 0;
}

/**
 * sys_vfs_readlinkat - Read a symbolic link relative to dirfd
 * Args: a0=dirfd, a1=path, a2=buf, a3=bufsiz
 */
uint64 sys_vfs_readlinkat(void) {
    SYSCALL_PROFILE_BEGIN(g_sys_readlinkat_calls);
    int dirfd;
    char *path;
    char *name;
    uint64 buf_addr;
    int bufsz;

    argint(0, &dirfd);

    struct vfs_inode *start_dir = NULL;
    int err = __vfs_resolve_dirfd(dirfd, &start_dir);
    if (err)
        SYSCALL_PROFILE_RETURN(err, g_sys_readlinkat_ticks);

    int n;
    int path_ret = __vfs_argpath(1, &path, &n);
    argaddr(2, &buf_addr);
    argint(3, &bufsz);
    if (path_ret < 0) {
        if (start_dir) vfs_iput(start_dir);
        WEBKIT_READLINK_TRACE("readlinkat dirfd=%d path=<fault> ret=%d\n",
                              dirfd, path_ret);
        SYSCALL_PROFILE_RETURN(path_ret, g_sys_readlinkat_ticks);
    }
    name = __vfs_alloc_pathbuf();
    if (name == NULL) {
        if (start_dir) vfs_iput(start_dir);
        kvfree(path);
        SYSCALL_PROFILE_RETURN(-ENOMEM, g_sys_readlinkat_ticks);
    }
#define SYS_VFS_READLINKAT_RETURN(ret_expr)                                  \
    do {                                                                     \
        kvfree(path);                                                        \
        kvfree(name);                                                        \
        SYSCALL_PROFILE_RETURN(ret_expr, g_sys_readlinkat_ticks);            \
    } while (0)

    if (bufsz <= 0) {
        if (start_dir) vfs_iput(start_dir);
        WEBKIT_READLINK_TRACE("readlinkat dirfd=%d path=%s bufsz=%d ret=%d\n",
                              dirfd, path, bufsz, -EINVAL);
        SYS_VFS_READLINKAT_RETURN(-EINVAL);
    }

    struct vfs_inode *parent =
        vfs_nameiparent_at(start_dir, path, n, name, VFS_USER_PATH_MAX);
    if (start_dir) vfs_iput(start_dir);
    if (IS_ERR(parent)) {
        int err = PTR_ERR(parent);
        WEBKIT_READLINK_TRACE("readlinkat dirfd=%d path=%s parent ret=%d\n",
                              dirfd, path, err);
        SYS_VFS_READLINKAT_RETURN(err);
    }
    if (parent == NULL) {
        WEBKIT_READLINK_TRACE("readlinkat dirfd=%d path=%s parent ret=%d\n",
                              dirfd, path, -ENOENT);
        SYS_VFS_READLINKAT_RETURN(-ENOENT);
    }

    struct vfs_dentry dentry = {.sb = parent->sb, .parent = parent};
    int ret = vfs_ilookup(parent, &dentry, name, strlen(name));
    if (ret != 0) {
        vfs_iput(parent);
        WEBKIT_READLINK_TRACE(
            "readlinkat dirfd=%d path=%s lookup name=%s ret=%d\n",
            dirfd, path, name, ret);
        SYS_VFS_READLINKAT_RETURN(ret);
    }

    struct vfs_inode *inode = vfs_get_dentry_inode(&dentry);
    vfs_release_dentry(&dentry);
    vfs_iput(parent);
    if (IS_ERR(inode)) {
        int err = PTR_ERR(inode);
        WEBKIT_READLINK_TRACE("readlinkat dirfd=%d path=%s inode ret=%d\n",
                              dirfd, path, err);
        SYS_VFS_READLINKAT_RETURN(err);
    }
    if (inode == NULL) {
        WEBKIT_READLINK_TRACE("readlinkat dirfd=%d path=%s inode ret=%d\n",
                              dirfd, path, -ENOENT);
        SYS_VFS_READLINKAT_RETURN(-ENOENT);
    }

    char *kbuf = kvmalloc(bufsz);
    if (kbuf == NULL) {
        vfs_iput(inode);
        WEBKIT_READLINK_TRACE("readlinkat dirfd=%d path=%s bufsz=%d ret=%d\n",
                              dirfd, path, bufsz, -ENOMEM);
        SYS_VFS_READLINKAT_RETURN(-ENOMEM);
    }

    ssize_t len = vfs_readlink(inode, kbuf, bufsz);
    vfs_iput(inode);
    if (len < 0) {
        kvfree(kbuf);
        WEBKIT_READLINK_TRACE(
            "readlinkat dirfd=%d path=%s bufsz=%d ret=%ld\n",
            dirfd, path, bufsz, (long)len);
        SYS_VFS_READLINKAT_RETURN(len);
    }

    if (either_copyout(1, buf_addr, kbuf, len) < 0) {
        kvfree(kbuf);
        WEBKIT_READLINK_TRACE(
            "readlinkat dirfd=%d path=%s len=%ld ret=%d\n",
            dirfd, path, (long)len, -EFAULT);
        SYS_VFS_READLINKAT_RETURN(-EFAULT);
    }
    if (len >= 0 && len < bufsz)
        kbuf[len] = '\0';
    else if (bufsz > 0)
        kbuf[bufsz - 1] = '\0';
    WEBKIT_READLINK_TRACE(
        "readlinkat dirfd=%d path=%s len=%ld target=%s\n",
        dirfd, path, (long)len, kbuf);
    kvfree(kbuf);
    SYS_VFS_READLINKAT_RETURN(len);
#undef SYS_VFS_READLINKAT_RETURN
}

/**
 * sys_vfs_renameat - Rename a file relative to dirfds
 * Args: a0=olddirfd, a1=oldpath, a2=newdirfd, a3=newpath
 */
uint64 sys_vfs_renameat(void) {
    int olddirfd, newdirfd;
    char *oldpath, *newpath;
    char *oldname, *newname;

    argint(0, &olddirfd);
    argint(2, &newdirfd);

    struct vfs_inode *old_start = NULL, *new_start = NULL;
    int err = __vfs_resolve_dirfd(olddirfd, &old_start);
    if (err)
        return err;
    err = __vfs_resolve_dirfd(newdirfd, &new_start);
    if (err) {
        if (old_start) vfs_iput(old_start);
        return err;
    }

    int n1, n2;
    int ret1 = __vfs_argpath(1, &oldpath, &n1);
    int ret2 = __vfs_argpath(3, &newpath, &n2);
    if (ret1 < 0 || ret2 < 0) {
        if (old_start) vfs_iput(old_start);
        if (new_start) vfs_iput(new_start);
        if (ret1 >= 0) kvfree(oldpath);
        if (ret2 >= 0) kvfree(newpath);
        return ret1 < 0 ? ret1 : ret2;
    }
    oldname = __vfs_alloc_pathbuf();
    newname = __vfs_alloc_pathbuf();
    if (oldname == NULL || newname == NULL) {
        if (old_start) vfs_iput(old_start);
        if (new_start) vfs_iput(new_start);
        kvfree(oldpath);
        kvfree(newpath);
        kvfree(oldname);
        kvfree(newname);
        return -ENOMEM;
    }

    struct vfs_inode *old_parent =
        vfs_nameiparent_at(old_start, oldpath, n1, oldname, VFS_USER_PATH_MAX);
    if (old_start) vfs_iput(old_start);
    kvfree(oldpath);
    if (IS_ERR(old_parent)) {
        if (new_start) vfs_iput(new_start);
        int ret = PTR_ERR(old_parent);
        kvfree(newpath);
        kvfree(oldname);
        kvfree(newname);
        return ret;
    }
    if (old_parent == NULL) {
        if (new_start) vfs_iput(new_start);
        kvfree(newpath);
        kvfree(oldname);
        kvfree(newname);
        return -ENOENT;
    }

    struct vfs_inode *new_parent =
        vfs_nameiparent_at(new_start, newpath, n2, newname, VFS_USER_PATH_MAX);
    if (new_start) vfs_iput(new_start);
    kvfree(newpath);
    if (IS_ERR(new_parent)) {
        vfs_iput(old_parent);
        int ret = PTR_ERR(new_parent);
        kvfree(oldname);
        kvfree(newname);
        return ret;
    }
    if (new_parent == NULL) {
        vfs_iput(old_parent);
        kvfree(oldname);
        kvfree(newname);
        return -ENOENT;
    }

    struct vfs_dentry old_dentry = {.sb = old_parent->sb, .parent = old_parent};
    int ret = vfs_ilookup(old_parent, &old_dentry, oldname, strlen(oldname));
    if (ret != 0) {
        vfs_iput(old_parent);
        vfs_iput(new_parent);
        kvfree(oldname);
        kvfree(newname);
        return ret;
    }

    ret = vfs_move(old_parent, &old_dentry, new_parent, newname, strlen(newname));
    kvfree(oldname);
    kvfree(newname);
    vfs_release_dentry(&old_dentry);
    vfs_iput(old_parent);
    vfs_iput(new_parent);
    if (ret == 0)
        ACCT_INC(current->thread_group, fs_renames);
    return ret;
}

/**
 * sys_vfs_faccessat - Check file accessibility relative to dirfd
 * Args: a0=dirfd, a1=path, a2=mode, a3=flags
 */
uint64 sys_vfs_faccessat(void) {
    SYSCALL_PROFILE_BEGIN(g_sys_faccessat_calls);
    int dirfd, mode, flags;
    char *path;

    argint(0, &dirfd);

    struct vfs_inode *start_dir = NULL;
    int err = __vfs_resolve_dirfd(dirfd, &start_dir);
    if (err)
        SYSCALL_PROFILE_RETURN(err, g_sys_faccessat_ticks);

    int n;
    int path_ret = __vfs_argpath(1, &path, &n);
    argint(2, &mode);
    argint(3, &flags);
    if (path_ret < 0) {
        if (start_dir) vfs_iput(start_dir);
        SYSCALL_PROFILE_RETURN(path_ret, g_sys_faccessat_ticks);
    }

    struct vfs_inode *inode = vfs_namei_at(start_dir, path, n);
    if (start_dir) vfs_iput(start_dir);
    kvfree(path);
    if (IS_ERR(inode))
        SYSCALL_PROFILE_RETURN(PTR_ERR(inode), g_sys_faccessat_ticks);
    if (inode == NULL)
        SYSCALL_PROFILE_RETURN(-ENOENT, g_sys_faccessat_ticks);

    if (mode != 0) {
        mode_t perm = inode->mode;
        if ((mode & 4) && !(perm & (S_IRUSR | S_IRGRP | S_IROTH))) {
            vfs_iput(inode);
            SYSCALL_PROFILE_RETURN(-EACCES, g_sys_faccessat_ticks);
        }
        if ((mode & 2) && !(perm & (S_IWUSR | S_IWGRP | S_IWOTH))) {
            vfs_iput(inode);
            SYSCALL_PROFILE_RETURN(-EACCES, g_sys_faccessat_ticks);
        }
        if ((mode & 1) && !(perm & (S_IXUSR | S_IXGRP | S_IXOTH))) {
            vfs_iput(inode);
            SYSCALL_PROFILE_RETURN(-EACCES, g_sys_faccessat_ticks);
        }
    }

    vfs_iput(inode);
    SYSCALL_PROFILE_RETURN(0, g_sys_faccessat_ticks);
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
    if (ret >= 0 && (flags & O_CLOEXEC))
        vfs_fdtable_set_fdflags(current->fdtable, ret, FD_CLOEXEC);
    spin_unlock(&current->fdtable->lock);

    if (old_newfd) {
        vfs_file_maybe_last_fd_close(old_newfd);
        __vfs_fput_call_rcu(old_newfd);
    }
    vfs_fput(f);

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

uint64 sys_vfs_copy_file_range(void)
{
    int in_fd, out_fd;
    uint64 off_in_addr, off_out_addr;
    int64 len;
    int flags;
    argint(0, &in_fd);
    argaddr(1, &off_in_addr);
    argint(2, &out_fd);
    argaddr(3, &off_out_addr);
    argint64(4, &len);
    argint(5, &flags);

    if (flags != 0)
        return (uint64)-EINVAL;
    if (len < 0)
        return (uint64)-EINVAL;
    if (len == 0)
        return 0;

    struct vfs_file *in_f = __vfs_argfd(in_fd);
    if (in_f == NULL)
        return (uint64)-EBADF;
    struct vfs_file *out_f = __vfs_argfd(out_fd);
    if (out_f == NULL) {
        vfs_fput(in_f);
        return (uint64)-EBADF;
    }

    bool use_in_off = off_in_addr != 0;
    bool use_out_off = off_out_addr != 0;
    loff_t off_in = 0;
    loff_t off_out = 0;
    if (use_in_off &&
        either_copyin(&off_in, 1, off_in_addr, sizeof(off_in)) < 0) {
        vfs_fput(in_f);
        vfs_fput(out_f);
        return (uint64)-EFAULT;
    }
    if (use_out_off &&
        either_copyin(&off_out, 1, off_out_addr, sizeof(off_out)) < 0) {
        vfs_fput(in_f);
        vfs_fput(out_f);
        return (uint64)-EFAULT;
    }
    if ((use_in_off && off_in < 0) || (use_out_off && off_out < 0)) {
        vfs_fput(in_f);
        vfs_fput(out_f);
        return (uint64)-EINVAL;
    }

    char buf[4096];
    ssize_t total = 0;
    while (total < len) {
        size_t chunk = (size_t)(len - total);
        if (chunk > sizeof(buf))
            chunk = sizeof(buf);

        loff_t saved_in = 0;
        if (use_in_off) {
            mutex_lock(&in_f->lock);
            saved_in = in_f->f_pos;
            in_f->f_pos = off_in;
            mutex_unlock(&in_f->lock);
        }
        ssize_t nr = vfs_fileread(in_f, buf, chunk, false);
        if (use_in_off) {
            mutex_lock(&in_f->lock);
            off_in = in_f->f_pos;
            in_f->f_pos = saved_in;
            mutex_unlock(&in_f->lock);
        }
        if (nr <= 0) {
            if (nr < 0 && total == 0)
                total = nr;
            break;
        }

        loff_t saved_out = 0;
        if (use_out_off) {
            mutex_lock(&out_f->lock);
            saved_out = out_f->f_pos;
            out_f->f_pos = off_out;
            mutex_unlock(&out_f->lock);
        }
        ssize_t nw = vfs_filewrite(out_f, buf, (size_t)nr, false);
        if (use_out_off) {
            mutex_lock(&out_f->lock);
            off_out = out_f->f_pos;
            out_f->f_pos = saved_out;
            mutex_unlock(&out_f->lock);
        }
        if (nw <= 0) {
            if (nw < 0 && total == 0)
                total = nw;
            break;
        }
        total += nw;
        if (nw < nr)
            break;
    }

    if (total > 0) {
        if (use_in_off)
            either_copyout(1, off_in_addr, &off_in, sizeof(off_in));
        if (use_out_off)
            either_copyout(1, off_out_addr, &off_out, sizeof(off_out));
    }

    vfs_fput(in_f);
    vfs_fput(out_f);
    return (uint64)total;
}

uint64 sys_vfs_xattr_not_supported(void)
{
    return (uint64)-ENOTSUP;
}

/* -------------------------------------------------------------------------- */
/* signalfd compatibility                                                      */

#define SFD_CLOEXEC  O_CLOEXEC
#define SFD_NONBLOCK O_NONBLOCK

struct signalfd_ctx {
    sigset_t mask;
};

static ssize_t signalfd_read(struct vfs_file *file, char *buf, size_t count,
                             bool user)
{
    (void)file;
    (void)buf;
    (void)user;
    if (count < 128)
        return -EINVAL;
    return -EAGAIN;
}

static int signalfd_poll(struct vfs_file *file, short events)
{
    (void)file;
    (void)events;
    return 0;
}

static int signalfd_release(struct vfs_inode *ip, struct vfs_file *file)
{
    (void)ip;
    if (file->private_data != NULL) {
        kfree(file->private_data);
        file->private_data = NULL;
    }
    return 0;
}

static struct vfs_file_ops signalfd_file_ops = {
    .read = signalfd_read,
    .poll = signalfd_poll,
    .release = signalfd_release,
};

static uint64 signalfd_create_fd(sigset_t mask, int flags)
{
    if (flags & ~(SFD_CLOEXEC | SFD_NONBLOCK))
        return (uint64)-EINVAL;

    struct signalfd_ctx *ctx = kalloc();
    if (ctx == NULL)
        return (uint64)-ENOMEM;
    memset(ctx, 0, sizeof(*ctx));
    ctx->mask = mask;

    int file_flags = O_RDONLY;
    if (flags & SFD_NONBLOCK)
        file_flags |= O_NONBLOCK;

    int fd = vfs_custom_fd_alloc(&signalfd_file_ops, ctx, file_flags);
    if (fd < 0) {
        kfree(ctx);
        return (uint64)fd;
    }
    if (flags & SFD_CLOEXEC) {
        spin_lock(&current->fdtable->lock);
        (void)vfs_fdtable_set_fdflags(current->fdtable, fd, FD_CLOEXEC);
        spin_unlock(&current->fdtable->lock);
    }
    return (uint64)fd;
}

static uint64 signalfd_common(int with_flags)
{
    int fd;
    uint64 mask_addr;
    int sigsetsize;
    int flags = 0;
    sigset_t mask = 0;

    argint(0, &fd);
    argaddr(1, &mask_addr);
    argint(2, &sigsetsize);
    if (with_flags)
        argint(3, &flags);

    if (sigsetsize != (int)sizeof(sigset_t))
        return (uint64)-EINVAL;
    if (mask_addr == 0)
        return (uint64)-EFAULT;
    if (either_copyin(&mask, 1, mask_addr, sizeof(mask)) < 0)
        return (uint64)-EFAULT;

    if (fd == -1)
        return signalfd_create_fd(mask, flags);

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        return (uint64)-EBADF;
    if (f->ops != &signalfd_file_ops || f->private_data == NULL) {
        vfs_fput(f);
        return (uint64)-EINVAL;
    }
    if (flags != 0) {
        vfs_fput(f);
        return (uint64)-EINVAL;
    }
    ((struct signalfd_ctx *)f->private_data)->mask = mask;
    vfs_fput(f);
    return (uint64)fd;
}

uint64 sys_signalfd(void)
{
    return signalfd_common(0);
}

uint64 sys_signalfd4(void)
{
    return signalfd_common(1);
}

/* -------------------------------------------------------------------------- */
/* inotify compatibility                                                       */

#define IN_CLOEXEC  O_CLOEXEC
#define IN_NONBLOCK O_NONBLOCK

struct inotify_ctx {
    int next_wd;
    bool active[256];
};

static ssize_t inotify_read(struct vfs_file *file, char *buf, size_t count,
                            bool user)
{
    (void)file;
    (void)buf;
    (void)count;
    (void)user;
    return -EAGAIN;
}

static int inotify_poll(struct vfs_file *file, short events)
{
    (void)file;
    (void)events;
    return 0;
}

static int inotify_release(struct vfs_inode *ip, struct vfs_file *file)
{
    (void)ip;
    if (file->private_data != NULL) {
        kfree(file->private_data);
        file->private_data = NULL;
    }
    return 0;
}

static struct vfs_file_ops inotify_file_ops = {
    .read = inotify_read,
    .poll = inotify_poll,
    .release = inotify_release,
};

static uint64 inotify_create_fd(int flags)
{
    if (flags & ~(IN_CLOEXEC | IN_NONBLOCK))
        return (uint64)-EINVAL;

    struct inotify_ctx *ctx = kalloc();
    if (ctx == NULL)
        return (uint64)-ENOMEM;
    memset(ctx, 0, sizeof(*ctx));
    ctx->next_wd = 1;

    int file_flags = O_RDONLY;
    if (flags & IN_NONBLOCK)
        file_flags |= O_NONBLOCK;

    int fd = vfs_custom_fd_alloc(&inotify_file_ops, ctx, file_flags);
    if (fd < 0) {
        kfree(ctx);
        return (uint64)fd;
    }
    if (flags & IN_CLOEXEC) {
        spin_lock(&current->fdtable->lock);
        (void)vfs_fdtable_set_fdflags(current->fdtable, fd, FD_CLOEXEC);
        spin_unlock(&current->fdtable->lock);
    }
    return (uint64)fd;
}

uint64 sys_inotify_init(void)
{
    return inotify_create_fd(0);
}

uint64 sys_inotify_init1(void)
{
    int flags;
    argint(0, &flags);
    return inotify_create_fd(flags);
}

uint64 sys_inotify_add_watch(void)
{
    int fd;
    uint64 path_addr;
    uint32 mask;
    argint(0, &fd);
    argaddr(1, &path_addr);
    argint(2, (int *)&mask);

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        return (uint64)-EBADF;
    if (f->ops != &inotify_file_ops || f->private_data == NULL) {
        vfs_fput(f);
        return (uint64)-EINVAL;
    }

    char path[MAXPATH];
    if (vm_copyinstr(current->vm, path, path_addr, MAXPATH) < 0) {
        vfs_fput(f);
        return (uint64)-EFAULT;
    }
    struct vfs_inode *inode = vfs_namei(path, strlen(path));
    if (IS_ERR_OR_NULL(inode)) {
        vfs_fput(f);
        return (uint64)(IS_ERR(inode) ? PTR_ERR(inode) : -ENOENT);
    }
    vfs_iput(inode);

    struct inotify_ctx *ctx = f->private_data;
    int wd = ctx->next_wd++;
    if (wd <= 0 || wd >= (int)(sizeof(ctx->active) / sizeof(ctx->active[0]))) {
        vfs_fput(f);
        return (uint64)-ENOSPC;
    }
    ctx->active[wd] = true;
    (void)mask;
    vfs_fput(f);
    return (uint64)wd;
}

uint64 sys_inotify_rm_watch(void)
{
    int fd;
    int wd;
    argint(0, &fd);
    argint(1, &wd);

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        return (uint64)-EBADF;
    if (f->ops != &inotify_file_ops || f->private_data == NULL) {
        vfs_fput(f);
        return (uint64)-EINVAL;
    }

    struct inotify_ctx *ctx = f->private_data;
    if (wd <= 0 || wd >= (int)(sizeof(ctx->active) / sizeof(ctx->active[0])) ||
        !ctx->active[wd]) {
        vfs_fput(f);
        return (uint64)-EINVAL;
    }
    ctx->active[wd] = false;
    vfs_fput(f);
    return 0;
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
static uint64 vfs_chmodat_path(int dirfd, const char *path, int mode, int flags) {
    (void)flags;
    struct vfs_inode *start_dir = NULL;
    int err = __vfs_resolve_dirfd(dirfd, &start_dir);
    if (err)
        return err;

    struct vfs_inode *inode = vfs_namei_at(start_dir, path, strlen(path));
    if (start_dir) vfs_iput(start_dir);
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

uint64 sys_vfs_fchmodat(void) {
    int dirfd;
    char path[MAXPATH];
    int mode, flags;

    argint(0, &dirfd);
    int n = argstr(1, path, MAXPATH);
    argint(2, &mode);
    argint(3, &flags);
    if (n < 0)
        return (uint64)-EFAULT;

    return vfs_chmodat_path(dirfd, path, mode, flags);
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
static uint64 vfs_chownat_path(int dirfd, const char *path, int owner,
                               int group, int flags) {
    (void)flags;
    struct vfs_inode *start_dir = NULL;
    int err = __vfs_resolve_dirfd(dirfd, &start_dir);
    if (err)
        return err;

    struct vfs_inode *inode = vfs_namei_at(start_dir, path, strlen(path));
    if (start_dir) vfs_iput(start_dir);
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

uint64 sys_vfs_fchownat(void) {
    int dirfd;
    char path[MAXPATH];
    int owner, group, flags;

    argint(0, &dirfd);
    int n = argstr(1, path, MAXPATH);
    argint(2, &owner);
    argint(3, &group);
    argint(4, &flags);
    if (n < 0)
        return (uint64)-EFAULT;

    return vfs_chownat_path(dirfd, path, owner, group, flags);
}

uint64 sys_vfs_chmod(void) {
    char path[MAXPATH];
    int mode;
    int n = argstr(0, path, MAXPATH);
    argint(1, &mode);
    if (n < 0)
        return (uint64)-EFAULT;
    return vfs_chmodat_path(AT_FDCWD, path, mode, 0);
}

uint64 sys_vfs_chown(void) {
    char path[MAXPATH];
    int owner, group;
    int n = argstr(0, path, MAXPATH);
    argint(1, &owner);
    argint(2, &group);
    if (n < 0)
        return (uint64)-EFAULT;
    return vfs_chownat_path(AT_FDCWD, path, owner, group, 0);
}

uint64 sys_vfs_lchown(void) {
    char path[MAXPATH];
    int owner, group;
    int n = argstr(0, path, MAXPATH);
    argint(1, &owner);
    argint(2, &group);
    if (n < 0)
        return (uint64)-EFAULT;
    return vfs_chownat_path(AT_FDCWD, path, owner, group, AT_SYMLINK_NOFOLLOW);
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

/* ================================================================== */
/*  fsync / fdatasync                                                 */
/* ================================================================== */

/**
 * sys_vfs_fsync - synchronize a file's state with storage
 * fsync(int fd) → 0 / -errno
 */
uint64 sys_vfs_fsync(void) {
    int fd;
    argint(0, &fd);

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        return (uint64)-EBADF;

    int ret = 0;
    if (f->ops && f->ops->fsync)
        ret = f->ops->fsync(f, 0, (loff_t)-1);
    vfs_fput(f);
    return (uint64)ret;
}

/**
 * sys_vfs_fdatasync - synchronize a file's data (not metadata)
 * fdatasync(int fd) → 0 / -errno
 *
 * In xv6, fdatasync is identical to fsync — we don't distinguish
 * data-only sync from full metadata sync.
 */
uint64 sys_vfs_fdatasync(void) {
    int fd;
    argint(0, &fd);

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        return (uint64)-EBADF;

    int ret = 0;
    if (f->ops && f->ops->fsync)
        ret = f->ops->fsync(f, 0, (loff_t)-1);
    vfs_fput(f);
    return (uint64)ret;
}

uint64 sys_vfs_readahead(void) {
    int fd;
    int64 offset;
    uint64 count;
    argint(0, &fd);
    argint64(1, &offset);
    argaddr(2, &count);

    if (offset < 0)
        return (uint64)-EINVAL;

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        return (uint64)-EBADF;
    if (f->f_flags & O_PATH) {
        vfs_fput(f);
        return (uint64)-EBADF;
    }

    (void)count;
    vfs_fput(f);
    return 0;
}

uint64 sys_vfs_sync_file_range(void) {
    int fd;
    int64 offset;
    int64 nbytes;
    uint flags;
    argint(0, &fd);
    argint64(1, &offset);
    argint64(2, &nbytes);
    argint(3, (int *)&flags);

    const uint supported = SYNC_FILE_RANGE_WAIT_BEFORE |
                           SYNC_FILE_RANGE_WRITE |
                           SYNC_FILE_RANGE_WAIT_AFTER;
    if (offset < 0 || nbytes < 0)
        return (uint64)-EINVAL;
    if (flags & ~supported)
        return (uint64)-EINVAL;

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        return (uint64)-EBADF;
    if (f->f_flags & O_PATH) {
        vfs_fput(f);
        return (uint64)-EBADF;
    }

    int ret = 0;
    if ((flags & SYNC_FILE_RANGE_WRITE) && f->ops && f->ops->fsync)
        ret = f->ops->fsync(f, offset, nbytes);
    vfs_fput(f);
    return (uint64)ret;
}

uint64 sys_vfs_syncfs(void) {
    int fd;
    argint(0, &fd);

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        return (uint64)-EBADF;
    vfs_fput(f);
    return 0;
}

/* ================================================================== */
/*  fadvise64                                                         */
/* ================================================================== */

/**
 * sys_fadvise64 - provide file access advice to the kernel
 * fadvise64(int fd, int64 offset, int64 len, int advice) → 0 / -errno
 *
 * Currently only POSIX_FADV_DONTNEED is implemented (drop page cache).
 * Other advice values are accepted but ignored (per POSIX).
 */
uint64 sys_fadvise64(void) {
    int fd, advice;
    int64 offset, len;
    argint(0, &fd);
    argint64(1, &offset);
    argint64(2, &len);
    argint(3, &advice);

    (void)offset;
    (void)len;

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        return (uint64)-EBADF;

    int ret = 0;
    if (advice == POSIX_FADV_DONTNEED) {
        struct vfs_inode *inode = vfs_inode_deref(&f->inode);
        if (inode != NULL) {
            ret = pcache_evict_all(&inode->i_data);
        }
    }

    vfs_fput(f);
    return (uint64)ret;
}

/* ================================================================== */
/*  utimensat                                                         */
/* ================================================================== */

#define AT_FDCWD          (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define UTIME_NOW  ((1L << 30) - 1L)
#define UTIME_OMIT ((1L << 30) - 2L)

struct k_utimespec {
    int64 tv_sec;
    int64 tv_nsec;
};

struct k_timeval_abi {
    int64 tv_sec;
    int64 tv_usec;
};

struct k_utimbuf_abi {
    int64 actime;
    int64 modtime;
};

uint64 sys_futimesat(void);

uint64 sys_utimes(void) {
    uint64 path_addr;
    uint64 times_addr;
    argaddr(0, &path_addr);
    argaddr(1, &times_addr);

    uint64 saved_rdi = current->trapframe->trapframe.rdi;
    uint64 saved_rsi = current->trapframe->trapframe.rsi;
    uint64 saved_rdx = current->trapframe->trapframe.rdx;
    arch_tf_set_arg0(current->trapframe, AT_FDCWD);
    arch_tf_set_arg1(current->trapframe, path_addr);
    arch_tf_set_arg2(current->trapframe, times_addr);
    uint64 ret = sys_futimesat();
    current->trapframe->trapframe.rdi = saved_rdi;
    current->trapframe->trapframe.rsi = saved_rsi;
    current->trapframe->trapframe.rdx = saved_rdx;
    return ret;
}

uint64 sys_utime(void) {
    uint64 path_addr;
    uint64 times_addr;
    argaddr(0, &path_addr);
    argaddr(1, &times_addr);

    uint64 now_ns = goldfish_rtc_read_ns();
    uint64 atime = now_ns;
    uint64 mtime = now_ns;
    if (times_addr != 0) {
        struct k_utimbuf_abi ut;
        if (either_copyin(&ut, 1, times_addr, sizeof(ut)) < 0)
            return (uint64)-EFAULT;
        if (ut.actime < 0 || ut.modtime < 0)
            return (uint64)-EINVAL;
        atime = (uint64)ut.actime * 1000000000ULL;
        mtime = (uint64)ut.modtime * 1000000000ULL;
    }

    char path[MAXPATH];
    if (vm_copyinstr(current->vm, path, path_addr, MAXPATH) < 0)
        return (uint64)-EFAULT;
    size_t path_len = strlen(path);
    struct vfs_inode *inode = vfs_namei(path, (size_t)path_len);
    if (IS_ERR_OR_NULL(inode))
        return (uint64)(IS_ERR(inode) ? PTR_ERR(inode) : -ENOENT);

    vfs_ilock(inode);
    inode->atime = atime;
    inode->mtime = mtime;
    inode->ctime = now_ns;
    inode->dirty = 1;
    vfs_iunlock(inode);
    vfs_iput(inode);
    return 0;
}

uint64 sys_futimesat(void) {
    int dirfd;
    uint64 times_addr;
    char path[MAXPATH];
    argint(0, &dirfd);
    int path_len = argstr(1, path, MAXPATH);
    argaddr(2, &times_addr);

    struct vfs_inode *inode = NULL;
    struct vfs_inode *start_dir = NULL;
    int err = __vfs_resolve_dirfd(dirfd, &start_dir);
    if (err)
        return (uint64)err;
    if (path_len < 0) {
        if (start_dir) vfs_iput(start_dir);
        return (uint64)-ENOENT;
    }
    inode = vfs_namei_at(start_dir, path, (size_t)path_len);
    if (start_dir) vfs_iput(start_dir);
    if (IS_ERR_OR_NULL(inode))
        return (uint64)(IS_ERR(inode) ? PTR_ERR(inode) : -ENOENT);

    uint64 now_ns = goldfish_rtc_read_ns();
    uint64 atime = now_ns;
    uint64 mtime = now_ns;
    if (times_addr != 0) {
        struct k_timeval_abi tv[2];
        if (either_copyin(tv, 1, times_addr, sizeof(tv)) < 0) {
            vfs_iput(inode);
            return (uint64)-EFAULT;
        }
        if (tv[0].tv_sec < 0 || tv[0].tv_usec < 0 || tv[0].tv_usec >= 1000000 ||
            tv[1].tv_sec < 0 || tv[1].tv_usec < 0 || tv[1].tv_usec >= 1000000) {
            vfs_iput(inode);
            return (uint64)-EINVAL;
        }
        atime = (uint64)tv[0].tv_sec * 1000000000ULL +
                (uint64)tv[0].tv_usec * 1000ULL;
        mtime = (uint64)tv[1].tv_sec * 1000000000ULL +
                (uint64)tv[1].tv_usec * 1000ULL;
    }

    vfs_ilock(inode);
    inode->atime = atime;
    inode->mtime = mtime;
    inode->ctime = now_ns;
    inode->dirty = 1;
    vfs_iunlock(inode);
    vfs_iput(inode);
    return 0;
}

/**
 * sys_utimensat - change file timestamps with nanosecond precision
 * utimensat(int dirfd, const char *pathname, const struct timespec times[2],
 *           int flags) → 0 / -errno
 */
uint64 sys_utimensat(void) {
    int dirfd, flags;
    uint64 times_addr;
    char path[MAXPATH];

    argint(0, &dirfd);
    int path_len = argstr(1, path, MAXPATH);
    argaddr(2, &times_addr);
    argint(3, &flags);

    struct vfs_inode *inode = NULL;

    if (path_len < 0 && dirfd != AT_FDCWD) {
        /* utimensat(fd, NULL, ...) — operate on the fd itself */
        struct vfs_file *f = __vfs_argfd(dirfd);
        if (f == NULL)
            return (uint64)-EBADF;
        inode = f->inode.inode;
        if (inode == NULL) {
            vfs_fput(f);
            return (uint64)-EBADF;
        }
        vfs_idup(inode);
        vfs_fput(f);
    } else {
        struct vfs_inode *start_dir = NULL;
        int err = __vfs_resolve_dirfd(dirfd, &start_dir);
        if (err)
            return err;
        if (path_len < 0) {
            if (start_dir) vfs_iput(start_dir);
            return (uint64)-ENOENT;
        }

        if (flags & AT_SYMLINK_NOFOLLOW) {
            /* Don't follow symlinks — use lstat-style lookup.
             * vfs_namei follows symlinks, so we use it without
             * AT_SYMLINK_NOFOLLOW for now (limitation). */
        }
        inode = vfs_namei_at(start_dir, path, (size_t)path_len);
        if (start_dir) vfs_iput(start_dir);
        if (IS_ERR_OR_NULL(inode))
            return (uint64)(IS_ERR(inode) ? PTR_ERR(inode) : -ENOENT);
    }

    /* Parse times[2] from user space */
    struct k_utimespec times[2];
    uint64 now_ns = goldfish_rtc_read_ns();

    if (times_addr == 0) {
        /* NULL times → set both atime and mtime to current time */
        times[0].tv_nsec = UTIME_NOW;
        times[1].tv_nsec = UTIME_NOW;
    } else {
        if (either_copyin(times, 1, times_addr, sizeof(times)) < 0) {
            vfs_iput(inode);
            return (uint64)-EFAULT;
        }
    }

    vfs_ilock(inode);

    if (times[0].tv_nsec == UTIME_NOW) {
        inode->atime = now_ns;
    } else if (times[0].tv_nsec != UTIME_OMIT) {
        inode->atime = (uint64)times[0].tv_sec * 1000000000ULL +
                       (uint64)times[0].tv_nsec;
    }

    if (times[1].tv_nsec == UTIME_NOW) {
        inode->mtime = now_ns;
    } else if (times[1].tv_nsec != UTIME_OMIT) {
        inode->mtime = (uint64)times[1].tv_sec * 1000000000ULL +
                       (uint64)times[1].tv_nsec;
    }

    inode->ctime = now_ns;
    inode->dirty = 1;

    vfs_iunlock(inode);
    vfs_iput(inode);
    return 0;
}

/* ================================================================== */
/*  memfd_create                                                      */
/* ================================================================== */

#define MFD_CLOEXEC       0x0001U
#define MFD_ALLOW_SEALING 0x0002U

/**
 * sys_memfd_create - create an anonymous file in memory
 * memfd_create(const char *name, unsigned int flags) → fd / -errno
 *
 * Creates an anonymous tmpfs-backed file descriptor.  The file exists
 * only in memory and has no directory entry.
 */
uint64 sys_memfd_create(void) {
    char name[250];
    int flags;

    argstr(0, name, sizeof(name));
    argint(1, &flags);

    if (flags & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING))
        return (uint64)-EINVAL;

    /*
     * We need a real tmpfs inode so that the tmpfs file_ops (read/write/
     * llseek/fault) have something to operate on.  Locate the tmpfs
     * superblock that backs /tmp, allocate an inode on it, mark it as a
     * regular file with embedded storage, and open it normally through
     * vfs_fileopen().  Because the inode is never linked into any
     * directory it is effectively anonymous — it will be freed when the
     * last fd referring to it is closed.
     */
    struct vfs_inode *tmp_dir = vfs_namei("/tmp", 4);
    if (IS_ERR_OR_NULL(tmp_dir))
        return (uint64)-ENOENT;

    struct vfs_superblock *sb = tmp_dir->sb;
    vfs_iput(tmp_dir);

    /* Allocate a fresh tmpfs inode (requires sb write lock). */
    vfs_superblock_wlock(sb);
    struct vfs_inode *inode = vfs_alloc_inode(sb);
    vfs_superblock_unlock(sb);

    if (IS_ERR_OR_NULL(inode))
        return (uint64)(IS_ERR(inode) ? PTR_ERR(inode) : -ENOMEM);

    /* Initialise the inode as a regular file with embedded storage,
     * mirroring what __tmpfs_make_regfile() does inside tmpfs. */
    {
        /* tmpfs_private.h is internal, but we only touch the base
         * vfs_inode fields plus the bool that lives right after it. */
        inode->size   = 0;
        inode->mode   = S_IFREG | 0600;
        inode->n_links = 0;  /* anonymous — no directory entry */

        /* The embedded flag is the first field after vfs_inode in
         * struct tmpfs_inode.  Set it to true so small writes go to the
         * inline buffer instead of the (uninitialised) pcache. */
        bool *embedded = (bool *)((char *)inode + sizeof(struct vfs_inode));
        *embedded = true;
    }

    vfs_iunlock(inode); /* vfs_alloc_inode returns it locked */

    /* Open the inode through the normal path — this takes a ref,
     * calls tmpfs_open() which sets file->ops = &tmpfs_file_ops,
     * and installs the inode reference in the file struct. */
    int f_flags = O_RDWR;
    if (flags & MFD_CLOEXEC)
        f_flags |= O_CLOEXEC;

    struct vfs_file *file = vfs_fileopen(inode, f_flags);
    vfs_iput(inode); /* drop our ref; vfs_fileopen took its own */

    if (IS_ERR(file))
        return (uint64)PTR_ERR(file);

    file->f_is_memfd = true;
    file->f_allow_sealing = (flags & MFD_ALLOW_SEALING) != 0;
    if (!file->f_allow_sealing)
        file->f_seals = F_SEAL_SEAL;

    /* Install the open file into the fd table. */
    spin_lock(&current->fdtable->lock);
    int fd = vfs_fdtable_alloc_fd(current->fdtable, file);
    spin_unlock(&current->fdtable->lock);
    vfs_fput(file); /* drop alloc ref; fdtable now owns it */

    if (fd < 0)
        return (uint64)-EMFILE;

    if (flags & MFD_CLOEXEC) {
        spin_lock(&current->fdtable->lock);
        vfs_fdtable_set_fdflags(current->fdtable, fd, FD_CLOEXEC);
        spin_unlock(&current->fdtable->lock);
    }

    return (uint64)fd;
}

/*
 * sys_flock — BSD file locking via flock(2).
 *
 * Translates flock() operations into POSIX fcntl() file locks
 * on the entire file (start=0, len=0).
 */
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

uint64 sys_flock(void) {
    int fd, operation;
    argint(0, &fd);
    argint(1, &operation);

    int type;
    int op_base = operation & ~LOCK_NB;
    switch (op_base) {
    case LOCK_SH:
        type = F_RDLCK;
        break;
    case LOCK_EX:
        type = F_WRLCK;
        break;
    case LOCK_UN:
        type = F_UNLCK;
        break;
    default:
        return -EINVAL;
    }

    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL)
        return -EBADF;

    int cmd = (operation & LOCK_NB) ? F_SETLK : F_SETLKW;
    struct flock fl = {
        .l_type = type,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0, /* entire file */
    };

    int ret = vfs_file_lock_ctl(f, current->tgid, cmd, &fl);
    vfs_fput(f);
    return ret;
}

/*
 * sys_fstatfs / sys_statfs — filesystem stat stubs.
 *
 * Fontconfig (and other libraries) call fstatfs() to detect remote or
 * FAT filesystems.  We return a minimal ext4-like result so the caller
 * treats the filesystem as a normal local fs.
 */
struct kstatfs {
    unsigned long f_type;
    unsigned long f_bsize;
    uint64        f_blocks;
    uint64        f_bfree;
    uint64        f_bavail;
    uint64        f_files;
    uint64        f_ffree;
    struct { int val[2]; } f_fsid;
    unsigned long f_namelen;
    unsigned long f_frsize;
    unsigned long f_flags;
    unsigned long f_spare[4];
};

static int do_statfs(struct kstatfs *kbuf)
{
    memset(kbuf, 0, sizeof(*kbuf));
    kbuf->f_type    = 0xEF53;  /* EXT4_SUPER_MAGIC */
    kbuf->f_bsize   = 4096;
    kbuf->f_frsize  = 4096;
    kbuf->f_namelen = 255;
    kbuf->f_blocks  = 65536;
    kbuf->f_bfree   = 32768;
    kbuf->f_bavail  = 32768;
    kbuf->f_files   = 16384;
    kbuf->f_ffree   = 8192;
    return 0;
}

uint64 sys_fstatfs(void)
{
    int fd;
    uint64 buf_addr;
    argint(0, &fd);
    argaddr(1, &buf_addr);

    struct vfs_file *f = __vfs_argfd(fd);
    if (!f)
        return -EBADF;
    vfs_fput(f);

    struct kstatfs kbuf;
    do_statfs(&kbuf);

    if (vm_copyout(current->vm, buf_addr,
                (char *)&kbuf, sizeof(kbuf)) < 0)
        return -EFAULT;
    return 0;
}

uint64 sys_statfs(void)
{
    uint64 buf_addr;
    char path[MAXPATH];
    if (argstr(0, path, MAXPATH) < 0)
        return -EFAULT;
    argaddr(1, &buf_addr);

    struct kstatfs kbuf;
    do_statfs(&kbuf);

    if (vm_copyout(current->vm, buf_addr,
                (char *)&kbuf, sizeof(kbuf)) < 0)
        return -EFAULT;
    return 0;
}
