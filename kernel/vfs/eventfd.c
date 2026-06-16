/**
 * @file eventfd.c
 * @brief eventfd(2) implementation for xv6.
 *
 * An eventfd is a file descriptor that maintains a uint64 counter.
 *  - write(fd, &val, 8) adds val to the counter.
 *  - read(fd, &buf, 8) returns the counter value and resets it to 0
 *    (or, with EFD_SEMAPHORE, returns 1 and decrements by 1).
 *  - poll() reports POLLIN when counter > 0.
 *
 * Used by libcurl as a lightweight self-pipe mechanism for wakeups.
 */

#include "types.h"
#include "defs.h"
#include "string.h"
#include "errno.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "vfs/vfs_types.h"
#include "vfs/file.h"
#include "vfs/fcntl.h"
#include "vfs/poll.h"
#include "mm/vm.h"
#include "printf.h"
#include "lock/spinlock.h"
#include "proc/tq.h"
#include "kqueue_types.h"
#include "kqueue.h"
#include "signal.h"

/* eventfd flags (match musl <sys/eventfd.h>) */
#define EFD_SEMAPHORE 1
#define EFD_CLOEXEC   O_CLOEXEC
#define EFD_NONBLOCK  O_NONBLOCK

#define EVENTFD_MAX   ((uint64)~0ULL - 1)

struct eventfd_ctx {
    spinlock_t  lock;
    tq_t        rq;       /* readers waiting for counter > 0 */
    tq_t        wq;       /* writers waiting for space */
    uint64      count;
    int         flags;    /* EFD_SEMAPHORE */
    struct vfs_file *file; /* back-pointer for kqueue notification */
};

/* ── read ─────────────────────────────────────────────────────────────── */
static ssize_t eventfd_read(struct vfs_file *file, char *buf, size_t count,
                            bool user)
{
    struct eventfd_ctx *ctx = file->private_data;

    if (count < sizeof(uint64))
        return -EINVAL;

    spin_lock(&ctx->lock);
    while (ctx->count == 0) {
        int wait_ret;

        if (file->f_flags & O_NONBLOCK) {
            spin_unlock(&ctx->lock);
            return -EAGAIN;
        }
        wait_ret = tq_wait_in_state(&ctx->rq, &ctx->lock, NULL,
                                    THREAD_INTERRUPTIBLE);
        if (wait_ret < 0 || signal_pending(current) || killed(current)) {
            spin_unlock(&ctx->lock);
            return -EINTR;
        }
    }

    uint64 val;
    if (ctx->flags & EFD_SEMAPHORE) {
        val = 1;
        ctx->count--;
    } else {
        val = ctx->count;
        ctx->count = 0;
    }
    /* Wake writers that might be waiting for space */
    tq_wakeup_all(&ctx->wq, 0, 0);
    spin_unlock(&ctx->lock);

    /* Notify epoll/kqueue if counter became zero */
    if (ctx->file)
        vfs_file_knote_notify(ctx->file, EVFILT_WRITE, 0);

    if (user) {
        if (vm_copyout(current->vm, (uint64)buf, (char *)&val,
                       sizeof(val)) < 0)
            return -EFAULT;
    } else {
        *(uint64 *)buf = val;
    }
    return sizeof(uint64);
}

/* ── write ────────────────────────────────────────────────────────────── */
static ssize_t eventfd_write(struct vfs_file *file, const char *buf,
                             size_t count, bool user)
{
    struct eventfd_ctx *ctx = file->private_data;
    uint64 val;

    if (count != sizeof(uint64))
        return -EINVAL;

    if (user) {
        if (vm_copyin(current->vm, (char *)&val, (uint64)buf,
                      sizeof(val)) < 0)
            return -EFAULT;
    } else {
        val = *(const uint64 *)buf;
    }

    if (val == (uint64)~0ULL)
        return -EINVAL;

    spin_lock(&ctx->lock);
    while (EVENTFD_MAX - ctx->count < val) {
        if (file->f_flags & O_NONBLOCK) {
            spin_unlock(&ctx->lock);
            return -EAGAIN;
        }
        tq_wait_in_state(&ctx->wq, &ctx->lock, NULL,
                         THREAD_INTERRUPTIBLE);
        if (signal_pending(current) || killed(current)) {
            spin_unlock(&ctx->lock);
            return -EINTR;
        }
    }
    ctx->count += val;

    /* Linux accepts a zero write, but it does not make the fd readable. */
    if (val != 0)
        tq_wakeup_all(&ctx->rq, 0, 0);
    spin_unlock(&ctx->lock);

    /* Notify epoll/kqueue that the fd is now readable */
    if (val != 0 && ctx->file)
        vfs_file_knote_notify(ctx->file, EVFILT_READ, 0);

    return sizeof(uint64);
}

/* ── poll ─────────────────────────────────────────────────────────────── */
static int eventfd_poll(struct vfs_file *file, short events)
{
    struct eventfd_ctx *ctx = file->private_data;
    short revents = 0;

    hyperv_input_intr();
    spin_lock(&ctx->lock);
    if (ctx->count > 0)
        revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
    if (ctx->count < EVENTFD_MAX)
        revents |= (events & (POLLOUT | POLLWRNORM | POLLWRBAND));
    spin_unlock(&ctx->lock);

    return revents;
}

/* ── release ──────────────────────────────────────────────────────────── */
static int eventfd_release(struct vfs_inode *ip, struct vfs_file *file)
{
    struct eventfd_ctx *ctx = file->private_data;
    if (ctx) {
        spin_lock(&ctx->lock);
        tq_wakeup_all(&ctx->rq, -1, 0);
        tq_wakeup_all(&ctx->wq, -1, 0);
        spin_unlock(&ctx->lock);
        kfree(ctx);
        file->private_data = NULL;
    }
    return 0;
}

static struct vfs_file_ops eventfd_file_ops = {
    .read    = eventfd_read,
    .write   = eventfd_write,
    .poll    = eventfd_poll,
    .release = eventfd_release,
};

int eventfd_file_is_eventfd(struct vfs_file *file)
{
    return file != NULL && file->ops == &eventfd_file_ops &&
           file->private_data != NULL;
}

int eventfd_signal_file(struct vfs_file *file, uint64 value)
{
    struct eventfd_ctx *ctx;

    if (!eventfd_file_is_eventfd(file))
        return -EINVAL;
    if (value == 0)
        return 0;
    if (value == (uint64)~0ULL)
        return -EINVAL;

    ctx = file->private_data;
    spin_lock(&ctx->lock);
    if (EVENTFD_MAX - ctx->count < value) {
        spin_unlock(&ctx->lock);
        return -EAGAIN;
    }
    ctx->count += value;
    tq_wakeup_all(&ctx->rq, 0, 0);
    spin_unlock(&ctx->lock);

    if (ctx->file)
        vfs_file_knote_notify(ctx->file, EVFILT_READ, 0);
    return 0;
}

/* ── sys_eventfd2 ─────────────────────────────────────────────────────── */
uint64 sys_eventfd2(void)
{
    uint initval;
    int flags;
    argint(0, (int *)&initval);
    argint(1, &flags);

    if (flags & ~(EFD_SEMAPHORE | EFD_CLOEXEC | EFD_NONBLOCK))
        return (uint64)-EINVAL;

    struct eventfd_ctx *ctx = (struct eventfd_ctx *)kalloc();
    if (ctx == NULL)
        return (uint64)-ENOMEM;

    memset(ctx, 0, sizeof(*ctx));
    spin_init(&ctx->lock, "eventfd");
    tq_init(&ctx->rq, "eventfd_rq", NULL);
    tq_init(&ctx->wq, "eventfd_wq", NULL);
    ctx->count = initval;
    ctx->flags = flags & EFD_SEMAPHORE;

    int file_flags = O_RDWR;
    if (flags & EFD_NONBLOCK)
        file_flags |= O_NONBLOCK;

    int fd = vfs_custom_fd_alloc(&eventfd_file_ops, ctx, file_flags);
    if (fd < 0) {
        kfree(ctx);
        return (uint64)fd;
    }

    /* Stash file pointer for kqueue notifications */
    spin_lock(&current->fdtable->lock);
    struct vfs_file *f = current->fdtable->files[fd];
    ctx->file = f;
    if (flags & EFD_CLOEXEC)
        vfs_fdtable_set_fdflags(current->fdtable, fd, FD_CLOEXEC);
    spin_unlock(&current->fdtable->lock);

    return (uint64)fd;
}

uint64 sys_eventfd(void)
{
    /*
     * eventfd(initval) is the legacy two-argument-free ABI.  eventfd2's
     * second argument lives in RSI on x86_64, so do not tail-call it.
     */
    uint initval;
    argint(0, (int *)&initval);

    struct eventfd_ctx *ctx = (struct eventfd_ctx *)kalloc();
    if (ctx == NULL)
        return (uint64)-ENOMEM;

    memset(ctx, 0, sizeof(*ctx));
    spin_init(&ctx->lock, "eventfd");
    tq_init(&ctx->rq, "eventfd_rq", NULL);
    tq_init(&ctx->wq, "eventfd_wq", NULL);
    ctx->count = initval;
    ctx->flags = 0;

    int fd = vfs_custom_fd_alloc(&eventfd_file_ops, ctx, O_RDWR);
    if (fd < 0) {
        kfree(ctx);
        return (uint64)fd;
    }

    spin_lock(&current->fdtable->lock);
    ctx->file = current->fdtable->files[fd];
    spin_unlock(&current->fdtable->lock);
    return (uint64)fd;
}
