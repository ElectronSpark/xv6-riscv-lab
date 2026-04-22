/**
 * @file timerfd.c
 * @brief timerfd(2) implementation for xv6.
 *
 * A timerfd is a file descriptor that delivers timer expirations via
 * read(2).  Each read returns a uint64 count of expirations since the
 * last read.  poll() reports POLLIN when at least one expiration has
 * occurred.
 *
 * Syscalls:
 *   timerfd_create(clockid, flags) → fd
 *   timerfd_settime(fd, flags, new_value, old_value)
 *   timerfd_gettime(fd, curr_value)
 */

#include "types.h"
#include "defs.h"
#include "string.h"
#include "errno.h"
#include "proc/thread.h"
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
#include "timer/timer.h"
#include "proc/sched.h"
#include "proc/workqueue.h"

/* Flags from <sys/timerfd.h> — match musl */
#define TFD_NONBLOCK      O_NONBLOCK
#define TFD_CLOEXEC       O_CLOEXEC
#define TFD_TIMER_ABSTIME 1

/* Userspace-compatible itimerspec layout (matches musl struct itimerspec) */
struct k_timespec {
    int64 tv_sec;
    int64 tv_nsec;
};

struct k_itimerspec {
    struct k_timespec it_interval;
    struct k_timespec it_value;
};

struct timerfd_ctx {
    spinlock_t      lock;
    tq_t            rq;         /* readers waiting for expirations > 0 */
    struct timer_node timer;    /* kernel timer node */
    struct work_struct rearm_work; /* deferred re-arm for repeating timers */
    struct k_itimerspec setting; /* current timer setting */
    uint64          expirations; /* expiration counter */
    uint64          arm_time;   /* r_time() when timer was armed */
    int             clockid;
    int             flags;      /* TFD_TIMER_ABSTIME etc */
    bool            armed;
    bool            cancelled;  /* set on release to prevent re-arm */
    struct vfs_file *file;      /* back-pointer for kqueue notification */
};

static struct vfs_file_ops timerfd_file_ops;
static struct workqueue *timerfd_wq;

/* Convert k_timespec to milliseconds, clamping to avoid overflow */
static uint64 ts_to_ms(const struct k_timespec *ts)
{
    if (ts->tv_sec < 0 || ts->tv_nsec < 0)
        return 0;
    uint64 ms = (uint64)ts->tv_sec * 1000 + (uint64)ts->tv_nsec / 1000000;
    return ms;
}

/* Convert milliseconds to k_timespec */
static void ms_to_ts(uint64 ms, struct k_timespec *ts)
{
    ts->tv_sec  = (int64)(ms / 1000);
    ts->tv_nsec = (int64)((ms % 1000) * 1000000);
}

/* Check if a timespec is zero (disarm) */
static bool ts_is_zero(const struct k_timespec *ts)
{
    return ts->tv_sec == 0 && ts->tv_nsec == 0;
}

static void timerfd_timer_callback(struct timer_node *tn);

/* ── workqueue callback: re-arm repeating timer ─────────────────────── */
static void timerfd_rearm_work(struct work_struct *work)
{
    struct timerfd_ctx *ctx = (struct timerfd_ctx *)work->data;

    spin_lock(&ctx->lock);
    if (ctx->cancelled || !ctx->armed) {
        spin_unlock(&ctx->lock);
        return;
    }
    uint64 interval_ms = ts_to_ms(&ctx->setting.it_interval);
    if (interval_ms == 0) {
        spin_unlock(&ctx->lock);
        return;
    }
    ctx->arm_time = r_time();
    spin_unlock(&ctx->lock);

    /* Remove the old timer node first (it may still be in the tree if
     * retry_limit > 1), then re-arm with a fresh node. */
    sched_timer_done(&ctx->timer);
    sched_timer_set_cb(&ctx->timer, interval_ms,
                       timerfd_timer_callback, ctx);
}

/* ── timer callback (runs in timer-tick context with timer lock held) ─ */
static void timerfd_timer_callback(struct timer_node *tn)
{
    struct timerfd_ctx *ctx = tn->data;

    spin_lock(&ctx->lock);
    ctx->expirations++;

    bool need_rearm = !ts_is_zero(&ctx->setting.it_interval) && !ctx->cancelled;
    if (!need_rearm)
        ctx->armed = false;

    /* Wake readers */
    tq_wakeup_all(&ctx->rq, 0, 0);
    spin_unlock(&ctx->lock);

    /* Notify epoll/kqueue */
    if (ctx->file)
        vfs_file_knote_notify(ctx->file, EVFILT_READ, 0);

    /* Defer re-arm to workqueue (can't call sched_timer_set_cb here
     * because timer lock is already held) */
    if (need_rearm && timerfd_wq)
        queue_work(timerfd_wq, &ctx->rearm_work);
}

/* ── read ─────────────────────────────────────────────────────────────── */
static ssize_t timerfd_read(struct vfs_file *file, char *buf, size_t count,
                            bool user)
{
    struct timerfd_ctx *ctx = file->private_data;

    if (count < sizeof(uint64))
        return -EINVAL;

    spin_lock(&ctx->lock);
    while (ctx->expirations == 0) {
        if (file->f_flags & O_NONBLOCK) {
            spin_unlock(&ctx->lock);
            return -EAGAIN;
        }
        tq_wait_in_state(&ctx->rq, &ctx->lock, NULL,
                         THREAD_INTERRUPTIBLE);
        if (signal_pending(current) || killed(current)) {
            spin_unlock(&ctx->lock);
            return -EINTR;
        }
    }

    uint64 val = ctx->expirations;
    ctx->expirations = 0;
    spin_unlock(&ctx->lock);

    if (user) {
        if (vm_copyout(current->vm, (uint64)buf, (char *)&val,
                       sizeof(val)) < 0)
            return -EFAULT;
    } else {
        *(uint64 *)buf = val;
    }
    return sizeof(uint64);
}

/* ── poll ─────────────────────────────────────────────────────────────── */
static int timerfd_poll(struct vfs_file *file, short events)
{
    struct timerfd_ctx *ctx = file->private_data;
    short revents = 0;

    spin_lock(&ctx->lock);
    if (ctx->expirations > 0)
        revents |= (events & (POLLIN | POLLRDNORM));
    spin_unlock(&ctx->lock);

    return revents;
}

/* ── release ──────────────────────────────────────────────────────────── */
static int timerfd_release(struct vfs_inode *ip, struct vfs_file *file)
{
    struct timerfd_ctx *ctx = file->private_data;
    if (ctx) {
        spin_lock(&ctx->lock);
        ctx->cancelled = true;
        if (ctx->armed) {
            ctx->armed = false;
            spin_unlock(&ctx->lock);
            sched_timer_done(&ctx->timer);
        } else {
            spin_unlock(&ctx->lock);
        }
        tq_wakeup_all(&ctx->rq, -1, 0);
        kfree(ctx);
        file->private_data = NULL;
    }
    return 0;
}

static struct vfs_file_ops timerfd_file_ops = {
    .read    = timerfd_read,
    .poll    = timerfd_poll,
    .release = timerfd_release,
};

/* ── Helper: validate timerfd and get ctx ────────────────────────────── */
static struct timerfd_ctx *timerfd_get_ctx(int fd, struct vfs_file **fp_out)
{
    struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, fd);
    if (f == NULL)
        return NULL;

    /* Validate: must be a timerfd (file ops match) */
    if (f->ops != &timerfd_file_ops) {
        vfs_fput(f);
        return NULL;
    }

    struct timerfd_ctx *ctx = (struct timerfd_ctx *)f->private_data;
    if (ctx == NULL) {
        vfs_fput(f);
        return NULL;
    }

    if (fp_out)
        *fp_out = f;
    else
        vfs_fput(f);

    return ctx;
}

/* ── sys_timerfd_create ───────────────────────────────────────────────── */
uint64 sys_timerfd_create(void)
{
    int clockid, flags;
    argint(0, &clockid);
    argint(1, &flags);

    struct timerfd_ctx *ctx = (struct timerfd_ctx *)kalloc();
    if (ctx == NULL)
        return (uint64)-ENOMEM;

    memset(ctx, 0, sizeof(*ctx));
    spin_init(&ctx->lock, "timerfd");
    tq_init(&ctx->rq, "timerfd_rq", NULL);
    init_work_struct(&ctx->rearm_work, timerfd_rearm_work, (uint64)ctx);
    ctx->clockid = clockid;
    ctx->armed = false;
    ctx->cancelled = false;
    ctx->expirations = 0;

    int file_flags = O_RDWR;
    if (flags & TFD_NONBLOCK)
        file_flags |= O_NONBLOCK;

    int fd = vfs_custom_fd_alloc(&timerfd_file_ops, ctx, file_flags);
    if (fd < 0) {
        kfree(ctx);
        return (uint64)fd;
    }

    /* Stash file pointer for kqueue notifications */
    spin_lock(&current->fdtable->lock);
    struct vfs_file *f = current->fdtable->files[fd];
    ctx->file = f;
    if (flags & TFD_CLOEXEC)
        vfs_fdtable_set_fdflags(current->fdtable, fd, FD_CLOEXEC);
    spin_unlock(&current->fdtable->lock);

    return (uint64)fd;
}

/* ── sys_timerfd_settime ──────────────────────────────────────────────── */
uint64 sys_timerfd_settime(void)
{
    int fd, flags;
    uint64 new_addr, old_addr;

    argint(0, &fd);
    argint(1, &flags);
    argaddr(2, &new_addr);
    argaddr(3, &old_addr);

    struct vfs_file *fp;
    struct timerfd_ctx *ctx = timerfd_get_ctx(fd, &fp);
    if (ctx == NULL)
        return (uint64)-EBADF;

    /* Copy in the new itimerspec from userspace */
    struct k_itimerspec new_val;
    if (either_copyin(&new_val, 1, new_addr, sizeof(new_val)) < 0) {
        vfs_fput(fp);
        return (uint64)-EFAULT;
    }

    /* Validate timespec values */
    if (new_val.it_value.tv_nsec < 0 || new_val.it_value.tv_nsec >= 1000000000LL ||
        new_val.it_interval.tv_nsec < 0 || new_val.it_interval.tv_nsec >= 1000000000LL) {
        vfs_fput(fp);
        return (uint64)-EINVAL;
    }

    spin_lock(&ctx->lock);

    /* Return old value if requested */
    if (old_addr != 0) {
        struct k_itimerspec old_val;
        if (ctx->armed) {
            /* Compute remaining time */
            uint64 now = r_time();
            uint64 value_ms = ts_to_ms(&ctx->setting.it_value);
            uint64 elapsed_ms = RAWTICKS_TO_MS(now - ctx->arm_time);
            if (elapsed_ms < value_ms)
                ms_to_ts(value_ms - elapsed_ms, &old_val.it_value);
            else
                memset(&old_val.it_value, 0, sizeof(old_val.it_value));
            old_val.it_interval = ctx->setting.it_interval;
        } else {
            memset(&old_val, 0, sizeof(old_val));
        }
        spin_unlock(&ctx->lock);

        if (either_copyout(1, old_addr, &old_val, sizeof(old_val)) < 0) {
            vfs_fput(fp);
            return (uint64)-EFAULT;
        }

        spin_lock(&ctx->lock);
    }

    /* Disarm existing timer */
    if (ctx->armed) {
        ctx->armed = false;
        spin_unlock(&ctx->lock);
        sched_timer_done(&ctx->timer);
        spin_lock(&ctx->lock);
    }

    /* Reset expiration counter */
    ctx->expirations = 0;
    ctx->setting = new_val;

    /* Arm new timer if it_value is non-zero */
    if (!ts_is_zero(&new_val.it_value)) {
        uint64 delay_ms = ts_to_ms(&new_val.it_value);

        if (flags & TFD_TIMER_ABSTIME) {
            /* Absolute time: compute relative delay from now */
            uint64 now_ms = RAWTICKS_TO_MS(r_time());
            if (delay_ms > now_ms)
                delay_ms = delay_ms - now_ms;
            else
                delay_ms = 0; /* already past */
        }

        if (delay_ms == 0)
            delay_ms = 1; /* minimum 1ms to ensure timer fires */

        ctx->arm_time = r_time();
        ctx->armed = true;
        spin_unlock(&ctx->lock);
        sched_timer_set_cb(&ctx->timer, delay_ms,
                           timerfd_timer_callback, ctx);
        spin_lock(&ctx->lock);
    }

    spin_unlock(&ctx->lock);
    vfs_fput(fp);
    return 0;
}

/* ── sys_timerfd_gettime ──────────────────────────────────────────────── */
uint64 sys_timerfd_gettime(void)
{
    int fd;
    uint64 curr_addr;

    argint(0, &fd);
    argaddr(1, &curr_addr);

    struct vfs_file *fp;
    struct timerfd_ctx *ctx = timerfd_get_ctx(fd, &fp);
    if (ctx == NULL)
        return (uint64)-EBADF;

    struct k_itimerspec curr;
    spin_lock(&ctx->lock);
    if (ctx->armed) {
        uint64 now = r_time();
        uint64 value_ms = ts_to_ms(&ctx->setting.it_value);
        uint64 elapsed_ms = RAWTICKS_TO_MS(now - ctx->arm_time);
        if (elapsed_ms < value_ms)
            ms_to_ts(value_ms - elapsed_ms, &curr.it_value);
        else
            memset(&curr.it_value, 0, sizeof(curr.it_value));
        curr.it_interval = ctx->setting.it_interval;
    } else {
        memset(&curr, 0, sizeof(curr));
    }
    spin_unlock(&ctx->lock);

    vfs_fput(fp);

    if (either_copyout(1, curr_addr, &curr, sizeof(curr)) < 0)
        return (uint64)-EFAULT;

    return 0;
}

/* ── init ─────────────────────────────────────────────────────────────── */
void timerfd_init(void)
{
    timerfd_wq = workqueue_create("timerfd", 2);
}
