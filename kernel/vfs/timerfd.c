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
#include "timer/goldfish_rtc.h"
#include "cmdline.h"

/* Flags from <sys/timerfd.h> — match musl */
#define TFD_NONBLOCK      O_NONBLOCK
#define TFD_CLOEXEC       O_CLOEXEC
#define TFD_TIMER_ABSTIME 1
#define TFD_TIMER_CANCEL_ON_SET 2

#define CLOCK_REALTIME       0
#define CLOCK_MONOTONIC      1
#define CLOCK_BOOTTIME       7
#define CLOCK_REALTIME_ALARM 8
#define CLOCK_BOOTTIME_ALARM 9

#define NSEC_PER_SEC  1000000000ULL
#define NSEC_PER_MSEC 1000000ULL

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
    uint64          next_expiration_ns; /* absolute time on ctx->clockid */
    uint64          interval_ns;
    int             clockid;
    int             flags;      /* TFD_TIMER_ABSTIME etc */
    bool            armed;
    bool            cancelled;  /* set on release to prevent re-arm */
    bool            rearm_pending; /* deferred repeating timer re-arm requested */
    bool            notify_pending;
    bool            work_pending; /* deferred work is queued or running */
    struct vfs_file *file;      /* back-pointer for kqueue notification */
    char            owner_name[16];
    pid_t           owner_pid;
};

static struct vfs_file_ops timerfd_file_ops;
static struct workqueue *timerfd_wq;

static int webkit_timerfd_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("webkit_timerfd_trace", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static bool webkit_timerfd_trace_ctx(struct timerfd_ctx *ctx)
{
    if (!webkit_timerfd_trace_enabled() || ctx == NULL)
        return false;

    return strncmp(ctx->owner_name, "MiniBrowser", 11) == 0 ||
           strncmp(ctx->owner_name, "WebKit", 6) == 0 ||
           strncmp(ctx->owner_name, "webkitgpusmoke", 14) == 0;
}

static bool webkit_timerfd_trace_current(void)
{
    if (!webkit_timerfd_trace_enabled() || current == NULL)
        return false;

    return strncmp(current->name, "MiniBrowser", 11) == 0 ||
           strncmp(current->name, "WebKit", 6) == 0 ||
           strncmp(current->name, "webkitgpusmoke", 14) == 0;
}

static bool timerfd_clock_supported(int clockid)
{
    return clockid == CLOCK_REALTIME ||
           clockid == CLOCK_MONOTONIC ||
           clockid == CLOCK_BOOTTIME ||
           clockid == CLOCK_REALTIME_ALARM ||
           clockid == CLOCK_BOOTTIME_ALARM;
}

static uint64 timerfd_now_ns(int clockid)
{
    if (clockid == CLOCK_REALTIME || clockid == CLOCK_REALTIME_ALARM)
        return goldfish_rtc_read_ns();

    uint64 freq = __timebase_frequency;
    if (freq == 0)
        return sched_timer_now_ms() * NSEC_PER_MSEC;

    uint64 ticks = r_time();
    return (ticks / freq) * NSEC_PER_SEC +
        ((ticks % freq) * NSEC_PER_SEC) / freq;
}

static int ts_to_ns(const struct k_timespec *ts, uint64 *ns)
{
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 ||
        ts->tv_nsec >= (int64)NSEC_PER_SEC)
        return -EINVAL;

    uint64 sec = (uint64)ts->tv_sec;
    if (sec > UINT64_MAX / NSEC_PER_SEC)
        return -EINVAL;
    uint64 base = sec * NSEC_PER_SEC;
    uint64 nsec = (uint64)ts->tv_nsec;
    if (base > UINT64_MAX - nsec)
        return -EINVAL;
    *ns = base + nsec;
    return 0;
}

static uint64 ns_to_ms_ceil(uint64 ns)
{
    if (ns == 0)
        return 0;
    return (ns + NSEC_PER_MSEC - 1) / NSEC_PER_MSEC;
}

static void ns_to_ts(uint64 ns, struct k_timespec *ts)
{
    ts->tv_sec  = (int64)(ns / NSEC_PER_SEC);
    ts->tv_nsec = (int64)(ns % NSEC_PER_SEC);
}

/* Check if a timespec is zero (disarm) */
static bool ts_is_zero(const struct k_timespec *ts)
{
    return ts->tv_sec == 0 && ts->tv_nsec == 0;
}

static void timerfd_timer_callback(struct timer_node *tn);

static void timerfd_queue_notification_locked(struct timerfd_ctx *ctx,
                                              bool *queue_deferred)
{
    ctx->notify_pending = true;
    if (!ctx->work_pending) {
        ctx->work_pending = true;
        *queue_deferred = true;
    }
}

/* ── workqueue callback: re-arm repeating timer ─────────────────────── */
static void timerfd_rearm_work(struct work_struct *work)
{
    struct timerfd_ctx *ctx = (struct timerfd_ctx *)work->data;

    for (;;) {
        struct vfs_file *notify_file = NULL;
        bool do_rearm = false;
        uint64 delay_ms = 0;

        spin_lock(&ctx->lock);
        if (ctx->cancelled) {
            ctx->rearm_pending = false;
            ctx->notify_pending = false;
            ctx->work_pending = false;
            bool free_ctx = ctx->file == NULL;
            spin_unlock(&ctx->lock);
            if (free_ctx)
                kfree(ctx);
            return;
        }

        if (ctx->notify_pending) {
            ctx->notify_pending = false;
            if (ctx->file)
                notify_file = vfs_fdup(ctx->file);
        }

        if (ctx->rearm_pending) {
            ctx->rearm_pending = false;
            if (ctx->armed && ctx->interval_ns != 0) {
                uint64 now_ns = timerfd_now_ns(ctx->clockid);
                if (ctx->next_expiration_ns <= now_ns)
                    ctx->next_expiration_ns = now_ns + ctx->interval_ns;
                delay_ms = ns_to_ms_ceil(ctx->next_expiration_ns - now_ns);
                do_rearm = true;
            }
        }

        if (!notify_file && !do_rearm) {
            ctx->work_pending = false;
            spin_unlock(&ctx->lock);
            return;
        }

        spin_unlock(&ctx->lock);

        if (notify_file) {
            vfs_file_knote_notify(notify_file, EVFILT_READ, 0);
            vfs_fput(notify_file);
        }

        if (do_rearm) {
            /* Remove the old timer node first (it may still be in the tree if
             * retry_limit > 1), then re-arm with a fresh node. */
            sched_timer_done(&ctx->timer);
            if (sched_timer_set_cb(&ctx->timer, delay_ms ? delay_ms : 1,
                                   timerfd_timer_callback, ctx) < 0) {
                spin_lock(&ctx->lock);
                ctx->armed = false;
                spin_unlock(&ctx->lock);
            }
        }

        spin_lock(&ctx->lock);
        if (!ctx->notify_pending && !ctx->rearm_pending) {
            ctx->work_pending = false;
            bool free_ctx = ctx->cancelled && ctx->file == NULL;
            spin_unlock(&ctx->lock);
            if (free_ctx)
                kfree(ctx);
            return;
        }
        spin_unlock(&ctx->lock);
    }
}

/* ── timer callback (runs in timer-tick context with timer lock held) ─ */
static void timerfd_timer_callback(struct timer_node *tn)
{
    struct timerfd_ctx *ctx = tn->data;

    spin_lock(&ctx->lock);
    uint64 now_ns = timerfd_now_ns(ctx->clockid);
    uint64 count = 1;
    if (ctx->interval_ns != 0 && now_ns > ctx->next_expiration_ns)
        count += (now_ns - ctx->next_expiration_ns) / ctx->interval_ns;
    ctx->expirations += count;
    if (ctx->interval_ns != 0)
        ctx->next_expiration_ns += count * ctx->interval_ns;

    bool need_rearm = ctx->interval_ns != 0 && !ctx->cancelled;
    bool queue_deferred = false;
    if (!need_rearm) {
        ctx->armed = false;
    } else {
        ctx->rearm_pending = true;
    }
    if (!ctx->cancelled) {
        timerfd_queue_notification_locked(ctx, &queue_deferred);
    }

    /* Wake readers */
    tq_wakeup_all(&ctx->rq, 0, 0);
    spin_unlock(&ctx->lock);

    if (webkit_timerfd_trace_ctx(ctx)) {
        printf("timerfd: expire owner=%s pid=%d clock=%d now_ns=%lu "
               "count=%lu expirations=%lu interval_ns=%lu next_ns=%lu "
               "rearm=%d\n",
               ctx->owner_name, ctx->owner_pid, ctx->clockid, now_ns, count,
               ctx->expirations, ctx->interval_ns, ctx->next_expiration_ns,
               need_rearm ? 1 : 0);
    }

    /* Defer kqueue notification and repeating timer re-arm out of timer
     * interrupt context.  kqueue locks may be held by the interrupted thread.
     */
    if (queue_deferred) {
        if (timerfd_wq == NULL || !queue_work(timerfd_wq, &ctx->rearm_work)) {
            spin_lock(&ctx->lock);
            ctx->work_pending = false;
            if (need_rearm)
                ctx->armed = false;
            spin_unlock(&ctx->lock);
        }
    }
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

    if (webkit_timerfd_trace_ctx(ctx)) {
        printf("timerfd: read owner=%s pid=%d reader=%s reader_pid=%d "
               "count=%lu\n",
               ctx->owner_name, ctx->owner_pid,
               current ? current->name : "(none)", current ? current->pid : -1,
               val);
    }

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
        revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
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
        ctx->file = NULL;
        if (ctx->armed) {
            ctx->armed = false;
            spin_unlock(&ctx->lock);
            sched_timer_done(&ctx->timer);
            spin_lock(&ctx->lock);
        } else {
        }
        tq_wakeup_all(&ctx->rq, -1, 0);
        bool free_ctx = !ctx->work_pending;
        spin_unlock(&ctx->lock);
        if (free_ctx)
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

    if (!timerfd_clock_supported(clockid))
        return (uint64)-EINVAL;
    if (flags & ~(TFD_NONBLOCK | TFD_CLOEXEC))
        return (uint64)-EINVAL;

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
    if (current != NULL) {
        memmove(ctx->owner_name, current->name, sizeof(ctx->owner_name));
        ctx->owner_name[sizeof(ctx->owner_name) - 1] = '\0';
        ctx->owner_pid = current->pid;
    }

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

    if (webkit_timerfd_trace_ctx(ctx)) {
        printf("timerfd: create owner=%s pid=%d fd=%d clock=%d flags=0x%x\n",
               ctx->owner_name, ctx->owner_pid, fd, clockid, flags);
    }

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

    if (flags & ~(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET)) {
        vfs_fput(fp);
        return (uint64)-EINVAL;
    }

    /* Copy in the new itimerspec from userspace */
    struct k_itimerspec new_val;
    if (either_copyin(&new_val, 1, new_addr, sizeof(new_val)) < 0) {
        vfs_fput(fp);
        return (uint64)-EFAULT;
    }

    uint64 value_ns, interval_ns;
    int ret = ts_to_ns(&new_val.it_value, &value_ns);
    if (ret != 0) {
        vfs_fput(fp);
        return (uint64)ret;
    }
    ret = ts_to_ns(&new_val.it_interval, &interval_ns);
    if (ret != 0) {
        vfs_fput(fp);
        return (uint64)ret;
    }

    bool notify_now = false;
    bool queue_deferred = false;
    bool trace = webkit_timerfd_trace_current();
    bool trace_armed = false;
    uint64 trace_now_ns = 0;
    uint64 trace_delay_ns = 0;
    uint64 trace_delay_ms = 0;
    uint64 trace_next_ns = 0;
    uint64 trace_expirations = 0;

    spin_lock(&ctx->lock);

    /* Return old value if requested */
    if (old_addr != 0) {
        struct k_itimerspec old_val;
        if (ctx->armed) {
            uint64 now_ns = timerfd_now_ns(ctx->clockid);
            if (ctx->next_expiration_ns > now_ns)
                ns_to_ts(ctx->next_expiration_ns - now_ns,
                         &old_val.it_value);
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
    ctx->interval_ns = interval_ns;

    /* Arm new timer if it_value is non-zero */
    if (!ts_is_zero(&new_val.it_value)) {
        uint64 now_ns = timerfd_now_ns(ctx->clockid);
        uint64 delay_ns;

        if (flags & TFD_TIMER_ABSTIME) {
            ctx->next_expiration_ns = value_ns;
            if (value_ns > now_ns) {
                delay_ns = value_ns - now_ns;
            } else {
                delay_ns = 0;
                uint64 count = 1;
                if (interval_ns != 0)
                    count += (now_ns - value_ns) / interval_ns;
                ctx->expirations = count;
                notify_now = true;
                tq_wakeup_all(&ctx->rq, 0, 0);
                if (interval_ns != 0)
                    ctx->next_expiration_ns = value_ns + count * interval_ns;
            }
        } else {
            if (UINT64_MAX - now_ns < value_ns) {
                spin_unlock(&ctx->lock);
                vfs_fput(fp);
                return (uint64)-EINVAL;
            }
            ctx->next_expiration_ns = now_ns + value_ns;
            delay_ns = value_ns;
        }

        if (notify_now)
            timerfd_queue_notification_locked(ctx, &queue_deferred);

        if (delay_ns != 0 || interval_ns != 0) {
            if (interval_ns != 0 && delay_ns == 0) {
                if (ctx->next_expiration_ns > now_ns)
                    delay_ns = ctx->next_expiration_ns - now_ns;
                else
                    delay_ns = interval_ns;
            }

            uint64 delay_ms = ns_to_ms_ceil(delay_ns);
            if (delay_ms == 0)
                delay_ms = 1; /* minimum 1ms to ensure timer fires */

            trace_now_ns = now_ns;
            trace_delay_ns = delay_ns;
            trace_delay_ms = delay_ms;
            trace_next_ns = ctx->next_expiration_ns;
            trace_expirations = ctx->expirations;
            trace_armed = true;

            ctx->armed = true;
            spin_unlock(&ctx->lock);
            int timer_ret = sched_timer_set_cb(&ctx->timer, delay_ms,
                                               timerfd_timer_callback, ctx);
            spin_lock(&ctx->lock);
            if (timer_ret < 0) {
                ctx->armed = false;
                spin_unlock(&ctx->lock);
                vfs_fput(fp);
                return (uint64)timer_ret;
            }
        } else {
            ctx->armed = false;
            trace_now_ns = timerfd_now_ns(ctx->clockid);
            trace_next_ns = ctx->next_expiration_ns;
            trace_expirations = ctx->expirations;
        }
    } else {
        trace_now_ns = timerfd_now_ns(ctx->clockid);
        trace_next_ns = ctx->next_expiration_ns;
        trace_expirations = ctx->expirations;
    }

    spin_unlock(&ctx->lock);
    if (trace) {
        printf("timerfd: settime proc=%s pid=%d fd=%d owner=%s owner_pid=%d "
               "clock=%d flags=0x%x value_ns=%lu interval_ns=%lu "
               "now_ns=%lu delay_ns=%lu delay_ms=%lu next_ns=%lu "
               "armed=%d expirations=%lu notify_now=%d\n",
               current ? current->name : "(none)",
               current ? current->pid : -1, fd, ctx->owner_name,
               ctx->owner_pid, ctx->clockid, flags, value_ns, interval_ns,
               trace_now_ns, trace_delay_ns, trace_delay_ms, trace_next_ns,
               trace_armed ? 1 : 0, trace_expirations, notify_now ? 1 : 0);
    }
    if (queue_deferred) {
        if (timerfd_wq == NULL || !queue_work(timerfd_wq, &ctx->rearm_work)) {
            vfs_file_knote_notify(fp, EVFILT_READ, 0);
            spin_lock(&ctx->lock);
            ctx->notify_pending = false;
            ctx->work_pending = false;
            spin_unlock(&ctx->lock);
        }
    }
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
        uint64 now_ns = timerfd_now_ns(ctx->clockid);
        if (ctx->next_expiration_ns > now_ns)
            ns_to_ts(ctx->next_expiration_ns - now_ns, &curr.it_value);
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
