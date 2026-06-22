#include "types.h"
#include "defs.h"
#include "errno.h"
#include "string.h"
#include "list.h"
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "lock/rcu.h"
#include "signal.h"
#include "vfs/file.h"
#include "vfs/fcntl.h"
#include "vfs/poll.h"
#include "kqueue_types.h"
#include <mm/vm.h>

void argint(int n, int *ip);
void argaddr(int n, uint64 *ip);
int either_copyin(void *dst, int user_src, uint64 src, uint64 len);

struct pidfd_ctx {
    spinlock_t lock;
    list_node_t node;
    pid_t pid;
    uint64 pid_seq;
    struct vfs_file *file;
};

static spinlock_t pidfd_list_lock = SPINLOCK_INITIALIZED("pidfd_list");
static list_node_t pidfd_list = { .prev = &pidfd_list, .next = &pidfd_list };

static int pidfd_target_exited(pid_t pid, uint64 pid_seq)
{
    struct thread *p = NULL;
    int exited = 1;

    rcu_read_lock();
    if (get_pid_thread(pid, &p) == 0 && p != NULL &&
        p->pid_seq == pid_seq) {
        struct thread_group *tg = p->thread_group;
        exited = THREAD_ZOMBIE(p) &&
            (tg == NULL || !thread_is_group_leader(p) ||
             __atomic_load_n(&tg->live_threads, __ATOMIC_ACQUIRE) <= 0);
    }
    rcu_read_unlock();
    return exited;
}

static int pidfd_poll(struct vfs_file *file, short events)
{
    struct pidfd_ctx *ctx = file->private_data;

    if (ctx == NULL)
        return POLLERR | POLLHUP;
    if (pidfd_target_exited(ctx->pid, ctx->pid_seq))
        return (events & (POLLIN | POLLRDNORM)) | POLLHUP;
    return 0;
}

static int pidfd_release(struct vfs_inode *ip, struct vfs_file *file)
{
    struct pidfd_ctx *ctx = file->private_data;

    if (ctx == NULL)
        return 0;

    spin_lock(&pidfd_list_lock);
    if (!LIST_ENTRY_IS_DETACHED(&ctx->node))
        list_node_detach(ctx, node);
    spin_unlock(&pidfd_list_lock);

    file->private_data = NULL;
    kfree(ctx);
    return 0;
}

static ssize_t pidfd_readlink(struct vfs_file *file, char *buf,
                              size_t buflen)
{
    static const char target[] = "anon_inode:[pidfd]";
    size_t len = sizeof(target) - 1;

    (void)file;
    if (buflen != 0) {
        size_t copy = len < buflen - 1 ? len : buflen - 1;
        memmove(buf, target, copy);
        buf[copy] = '\0';
    }
    return (ssize_t)len;
}

static struct vfs_file_ops pidfd_file_ops = {
    .poll = pidfd_poll,
    .release = pidfd_release,
    .readlink = pidfd_readlink,
};

static void pidfd_close_installed_fd(int fd)
{
    struct vfs_file *file;

    spin_lock(&current->fdtable->lock);
    file = vfs_fdtable_dealloc_fd(current->fdtable, fd);
    spin_unlock(&current->fdtable->lock);

    if (file != NULL) {
        vfs_file_maybe_last_fd_close(file);
        vfs_fput(file);
    }
}

static int pidfd_install(pid_t pid, uint64 pid_seq, int file_flags,
                         int fd_flags)
{
    struct pidfd_ctx *ctx = kalloc();
    if (ctx == NULL)
        return -ENOMEM;
    memset(ctx, 0, sizeof(*ctx));
    spin_init(&ctx->lock, "pidfd");
    list_entry_init(&ctx->node);
    ctx->pid = pid;
    ctx->pid_seq = pid_seq;

    int fd = vfs_custom_fd_alloc(&pidfd_file_ops, ctx, file_flags);
    if (fd < 0) {
        kfree(ctx);
        return fd;
    }

    spin_lock(&current->fdtable->lock);
    ctx->file = current->fdtable->files[fd];
    if (fd_flags != 0)
        (void)vfs_fdtable_set_fdflags(current->fdtable, fd, fd_flags);
    spin_unlock(&current->fdtable->lock);

    spin_lock(&pidfd_list_lock);
    list_node_push(&pidfd_list, ctx, node);
    spin_unlock(&pidfd_list_lock);

    return fd;
}

int pidfd_file_get_target(struct vfs_file *file, pid_t *pid, uint64 *pid_seq)
{
    if (file == NULL || file->ops != &pidfd_file_ops ||
        file->private_data == NULL || pid == NULL || pid_seq == NULL)
        return -EBADF;
    struct pidfd_ctx *ctx = file->private_data;
    *pid = ctx->pid;
    *pid_seq = ctx->pid_seq;
    return 0;
}

void pidfd_notify_exit(struct thread *p)
{
    struct pidfd_ctx *ctx, *tmp;

    if (p == NULL)
        return;
    spin_lock(&pidfd_list_lock);
    list_foreach_node_safe(&pidfd_list, ctx, tmp, node) {
        if (ctx->pid == p->pid && ctx->pid_seq == p->pid_seq &&
            ctx->file != NULL) {
            vfs_file_knote_notify(ctx->file, EVFILT_READ, 0);
        }
    }
    spin_unlock(&pidfd_list_lock);
}

int pidfd_install_for_thread(struct thread *target, uint64 user_pidfd_addr)
{
    if (target == NULL || user_pidfd_addr == 0)
        return -EFAULT;

    int fd = pidfd_install(target->pid, target->pid_seq, O_RDWR, FD_CLOEXEC);
    if (fd < 0)
        return fd;

    if (vm_copyout(current->vm, user_pidfd_addr, &fd, sizeof(fd)) < 0) {
        pidfd_close_installed_fd(fd);
        return -EFAULT;
    }

    return 0;
}

uint64 sys_pidfd_open(void)
{
    int pid;
    uint64 flags;
    struct thread *target = NULL;
    pid_t target_pid;
    uint64 target_seq;

    argint(0, &pid);
    argaddr(1, &flags);

    if (pid <= 0)
        return (uint64)-EINVAL;
    if ((flags & ~((uint64)O_NONBLOCK)) != 0)
        return (uint64)-EINVAL;

    rcu_read_lock();
    if (get_pid_thread(pid, &target) != 0 || target == NULL ||
        target->pid != target->tgid || !thread_is_group_leader(target)) {
        rcu_read_unlock();
        return (uint64)-ESRCH;
    }
    target_pid = target->pid;
    target_seq = target->pid_seq;
    rcu_read_unlock();

    int file_flags = O_RDWR;
    if (flags & O_NONBLOCK)
        file_flags |= O_NONBLOCK;

    return (uint64)pidfd_install(target_pid, target_seq, file_flags, 0);
}

uint64 sys_pidfd_send_signal(void)
{
    int pidfd;
    int sig;
    uint64 siginfo_addr;
    uint64 flags;
    pid_t pid;
    uint64 pid_seq;

    argint(0, &pidfd);
    argint(1, &sig);
    argaddr(2, &siginfo_addr);
    argaddr(3, &flags);

    /*
     * Linux accepts PIDFD_SIGNAL_THREAD (bit 0) for pidfd_send_signal().
     * pidfd_open() only creates process pidfds here, so bit 0 has the same
     * target as flags==0.  Reject unknown higher bits.
     */
    if ((flags & ~1ULL) != 0)
        return (uint64)-EINVAL;
    if (SIGBAD(sig) && sig != 0)
        return (uint64)-EINVAL;

    struct vfs_file *file = vfs_fdtable_get_file(current->fdtable, pidfd);
    if (file == NULL)
        return (uint64)-EBADF;
    int ret = pidfd_file_get_target(file, &pid, &pid_seq);
    vfs_fput(file);
    if (ret < 0)
        return (uint64)ret;

    ksiginfo_t info = {0};
    info.signo = sig;
    info.sender = current;
    info.info.si_pid = thread_tgid(current);
    if (siginfo_addr != 0) {
        if (either_copyin(&info.info, 1, siginfo_addr,
                          sizeof(info.info)) < 0)
            return (uint64)-EFAULT;
        if (info.info.si_signo != sig)
            return (uint64)-EINVAL;
    }
    return (uint64)signal_send_pidfd(pid, pid_seq, &info);
}
