/*
 * ptmx.c — /dev/ptmx and /dev/pts/N   (VFS-integrated lifecycle)
 *
 * Every open fd — master or slave — holds one reference on the shared
 * pty_pair.  When the last fd is closed the tty and pair are freed.
 *
 *   /dev/ptmx  (major 5, minor 2)
 *     Opening allocates a new PTY pair.  The returned fd is the master
 *     side.  The slave index is obtained via TIOCGPTN ioctl; the slave
 *     device appears at /dev/pts/<index>.
 *
 *   /dev/pts/N (major 136, minor N+1)
 *     A character device whose open_file callback installs vfs_file_ops
 *     that forward to the PTY slave tty.  Each open fd holds a pair ref.
 *
 * Lifecycle:
 *
 *   ptmx open   → pty_pair_alloc(), pair->refcount = 1
 *   pts  open   → pair->refcount++
 *   any  close  → pair->refcount--; if 0 → pty_pair_destroy()
 *   master close → also: tty_hangup, remove devtmpfs, cdev_unregister
 */

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "errno.h"
#include "string.h"
#include "printf.h"
#include "defs.h"
#include "lock/spinlock.h"
#include "mm/slab.h"
#include "mm/vm.h"
#include "dev/cdev.h"
#include "kqueue.h"
#include "kqueue_types.h"
#include "proc/thread.h"
#include "vfs/vfs_types.h"
#include "vfs/file.h"
#include "vfs/fs.h"
#include "vfs/poll.h"
#include "vfs/pipe.h"
#include "vfs/fcntl.h"
#include "tty/tty.h"
#include "tty/termios.h"
#include "tty/session.h"
#include "devtmpfs.h"
#include "vfs/stat.h"

int snprintf(char *buf, size_t size, const char *fmt, ...);

/* ---- Constants ---- */

#define PTMX_MAJOR    5
#define PTMX_MINOR    2
#define PTS_MAJOR     136   /* Same as PTY_MAJOR in param.h */
#define MAX_PTYS      64

/* ---- Per-PTY state ---- */

struct pty_pair {
    struct tty  *slave;         /* The slave tty (allocated by pty_alloc) */
    struct vfs_file *master_file; /* Master fd file for readiness wakeups */
    cdev_t       slave_cdev;    /* Registered cdev for /dev/pts/N */
    int          index;         /* PTY index (N in /dev/pts/N) */
    int          refcount;      /* Open master + slave fds */
    int          master_open;   /* Master side still alive? */
    int          slave_count;   /* Number of open slave fds */
    int          cdev_live;     /* Slave cdev still registered? */
    int          pts_node_stat_valid; /* /dev/pts/N stat snapshot available */
    struct stat  pts_node_stat;  /* stat identity of the devtmpfs slave node */
    spinlock_t   lock;          /* Protects refcount / flags */
};

static spinlock_t ptmx_lock = SPINLOCK_INITIALIZED("ptmx");
static struct pty_pair *pty_table[MAX_PTYS];

/* ---- refcount helpers ---- */

/*
 * Drop one reference.  Returns true if this was the LAST reference
 * and the caller must call pty_pair_destroy().
 */
static int pty_pair_put(struct pty_pair *pair)
{
    spin_lock(&pair->lock);
    int last = (--pair->refcount <= 0);
    spin_unlock(&pair->lock);
    return last;
}

static void pty_pair_destroy(struct pty_pair *pair)
{
    /* Clear table slot so the index can be reused */
    spin_lock(&ptmx_lock);
    if (pty_table[pair->index] == pair)
        pty_table[pair->index] = NULL;
    spin_unlock(&ptmx_lock);

    /* Free the tty (closes pipe ends, releases slab) */
    if (pair->slave) {
        tty_unref(pair->slave);
        pair->slave = NULL;
    }

    kvfree(pair);
}

/* Build the devtmpfs name "pts/<idx>" into buf (must be >= 16 bytes) */
static void pts_name(char *buf, int idx)
{
    const char *prefix = "pts/";
    int n = 0;
    while (*prefix)
        buf[n++] = *prefix++;
    char tmp[8];
    int ti = 0;
    int v = idx;
    if (v == 0) {
        tmp[ti++] = '0';
    } else {
        while (v > 0) { tmp[ti++] = '0' + (v % 10); v /= 10; }
    }
    for (int i = ti - 1; i >= 0; i--)
        buf[n++] = tmp[i];
    buf[n] = '\0';
}

static int vfs_inode_stat_snapshot(struct vfs_inode *inode, struct stat *st)
{
    if (inode == NULL || st == NULL)
        return -EINVAL;

    if (inode->ops && inode->ops->getattr) {
        memset(st, 0, sizeof(*st));
        return inode->ops->getattr(inode, st);
    }

    vfs_ilock(inode);
    memset(st, 0, sizeof(*st));
    st->st_dev = inode->sb ? (uint64)inode->sb : 0;
    st->st_ino = inode->ino;
    st->st_mode = inode->mode;
    st->st_nlink = inode->n_links;
    st->st_uid = inode->uid;
    st->st_gid = inode->gid;
    if (S_ISBLK(inode->mode))
        st->st_rdev = inode->bdev;
    else if (S_ISCHR(inode->mode))
        st->st_rdev = inode->cdev;
    st->st_size = inode->size;
    st->st_blksize = 4096;
    st->st_blocks = (inode->size + 511) >> 9;
    st->st_atime_sec = inode->atime;
    st->st_mtime_sec = inode->mtime;
    st->st_ctime_sec = inode->ctime;
    vfs_iunlock(inode);
    return 0;
}

static void pts_snapshot_node_stat(struct pty_pair *pair)
{
    char path[48];
    int n;

    if (pair == NULL || pair->slave == NULL)
        return;

    n = snprintf(path, sizeof(path), "/dev/%s", pair->slave->name);
    if (n < 0 || (size_t)n >= sizeof(path))
        return;

    struct vfs_inode *inode = vfs_namei(path, (size_t)n);
    if (IS_ERR_OR_NULL(inode))
        return;

    struct stat st;
    int ret = vfs_inode_stat_snapshot(inode, &st);
    vfs_iput(inode);
    if (ret != 0)
        return;

    pair->pts_node_stat = st;
    pair->pts_node_stat_valid = 1;
}

/* ================================================================== */
/*  /dev/pts/N — slave vfs_file_ops (installed by open_file callback) */
/* ================================================================== */

static ssize_t pts_fops_read(struct vfs_file *file, char *buf,
                             size_t count, bool user)
{
    struct pty_pair *pair = (struct pty_pair *)file->private_data;
    if (pair == NULL || pair->slave == NULL)
        return -EIO;
    return tty_read(pair->slave, buf, count, user);
}

static ssize_t pts_fops_write(struct vfs_file *file, const char *buf,
                              size_t count, bool user)
{
    struct pty_pair *pair = (struct pty_pair *)file->private_data;
    struct vfs_file *master_file = NULL;
    ssize_t ret;

    if (pair == NULL || pair->slave == NULL)
        return -EIO;
    ret = tty_write(pair->slave, (const char *)buf, count, user);

    /*
     * Slave writes make the master fd readable.  Event loops such as
     * Konsole's epoll backend wait on /dev/ptmx itself, not on the internal
     * output pipe, so propagate the readiness edge to the master file knotes.
     */
    if (ret > 0) {
        spin_lock(&pair->lock);
        master_file = vfs_fdup(pair->master_file);
        spin_unlock(&pair->lock);
        if (master_file != NULL) {
            vfs_file_knote_notify(master_file, EVFILT_READ, ret);
            vfs_fput(master_file);
        }
    }

    return ret;
}

static int pts_fops_ioctl(struct vfs_file *file, uint64 cmd, void *arg)
{
    struct pty_pair *pair = (struct pty_pair *)file->private_data;
    if (pair == NULL || pair->slave == NULL)
        return -EIO;
    return tty_ioctl(pair->slave, cmd, arg);
}

static int pts_fops_poll(struct vfs_file *file, short events)
{
    struct pty_pair *pair = (struct pty_pair *)file->private_data;
    if (pair == NULL || pair->slave == NULL)
        return 0;
    return tty_poll(pair->slave, events);
}

static int pts_fops_stat(struct vfs_file *file, struct stat *st)
{
    struct pty_pair *pair = (struct pty_pair *)file->private_data;

    if (file == NULL || st == NULL)
        return -EINVAL;
    if (pair == NULL || pair->slave == NULL)
        return -EIO;

    memset(st, 0, sizeof(*st));
    if (pair->pts_node_stat_valid) {
        *st = pair->pts_node_stat;
    } else {
        st->st_dev = mkdev(PTS_MAJOR, 0);
        st->st_ino = (uint64)pair->index + 1;
        st->st_nlink = 1;
        st->st_mode = S_IFCHR | 0620;
        st->st_uid = 0;
        st->st_gid = 0;
        st->st_rdev = mkdev(PTS_MAJOR, pair->index + 1);
        st->st_blksize = 4096;
    }
    return 0;
}

static ssize_t pts_fops_readlink(struct vfs_file *file, char *buf,
                                 size_t buflen)
{
    struct pty_pair *pair = (struct pty_pair *)file->private_data;

    if (file == NULL || buf == NULL)
        return -EINVAL;
    if (pair == NULL || pair->slave == NULL)
        return -EIO;
    return snprintf(buf, buflen, "/dev/pts/%d", pair->index);
}

static int pts_fops_release(struct vfs_inode *inode, struct vfs_file *file)
{
    (void)inode;
    struct pty_pair *pair = (struct pty_pair *)file->private_data;
    if (pair == NULL)
        return 0;

    file->private_data = NULL;

    /* Drop the tty-level "open" ref taken in pts_open_file */
    tty_close(pair->slave);

    /*
     * Closing a slave fd must not destructively hang up the PTY while the
     * master remains open.  Programs often open the slave temporarily for
     * setup, close it, and later hand a dup'd slave to exec'd children.  The
     * destructive hangup belongs to master close or controlling-session
     * hangup, both of which call tty_hangup().
     */
    spin_lock(&pair->lock);
    pair->slave_count--;
    spin_unlock(&pair->lock);

    /* Drop the pair ref — may destroy */
    if (pty_pair_put(pair))
        pty_pair_destroy(pair);

    return 0;
}

static struct vfs_file_ops pts_slave_file_ops = {
    .read    = pts_fops_read,
    .write   = pts_fops_write,
    .release = pts_fops_release,
    .stat    = pts_fops_stat,
    .readlink = pts_fops_readlink,
    .ioctl   = pts_fops_ioctl,
    .poll    = pts_fops_poll,
};

static int pts_install_peer_fd(struct pty_pair *pair, int flags)
{
    int file_flags = (flags & O_ACCMODE) == 0 ?
        ((flags & ~O_ACCMODE) | O_RDWR) : flags;
    int fd;
    int ret;

    file_flags &= (O_ACCMODE | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
    file_flags &= ~O_NOCTTY;

    spin_lock(&pair->lock);
    if (!pair->master_open) {
        spin_unlock(&pair->lock);
        return -ENXIO;
    }
    pair->refcount++;
    pair->slave_count++;
    spin_unlock(&pair->lock);

    ret = tty_open(pair->slave);
    if (ret != 0) {
        if (pty_pair_put(pair))
            pty_pair_destroy(pair);
        return ret;
    }

    fd = vfs_custom_fd_alloc(&pts_slave_file_ops, pair, file_flags);
    if (fd < 0) {
        tty_close(pair->slave);
        spin_lock(&pair->lock);
        pair->slave_count--;
        spin_unlock(&pair->lock);
        if (pty_pair_put(pair))
            pty_pair_destroy(pair);
        return fd;
    }

    if (file_flags & O_CLOEXEC) {
        spin_lock(&current->fdtable->lock);
        (void)vfs_fdtable_set_fdflags(current->fdtable, fd, FD_CLOEXEC);
        spin_unlock(&current->fdtable->lock);
    }

    return fd;
}

static void pts_maybe_set_controlling_tty(struct pty_pair *pair,
                                          int open_flags)
{
    if (pair == NULL || pair->slave == NULL || current == NULL)
        return;
    if (open_flags & O_NOCTTY)
        return;
    if (current->session == NULL)
        return;
    if (session_get_ctrl_tty(current->session) != NULL)
        return;
    if (current->session->sid != thread_tgid(current))
        return;

    session_set_ctrl_tty(current->session, pair->slave);
}

/* ================================================================== */
/*  /dev/pts/N — cdev (open_file installs vfs_file_ops above)         */
/* ================================================================== */

/*
 * pts_open_file is called by __vfs_open_cdev.  It installs the slave
 * file ops on the vfs_file so that the VFS manages the fd lifecycle.
 * The cdev kobject ref is released by __vfs_open_cdev immediately
 * after this returns (because we set file->ops).
 */
static int pts_open_file(cdev_t *cdev, struct vfs_file *file)
{
    struct pty_pair *pair = container_of(cdev, struct pty_pair, slave_cdev);

    /* Reject open after master closed (device is being torn down) */
    spin_lock(&pair->lock);
    if (!pair->master_open) {
        spin_unlock(&pair->lock);
        return -ENXIO;
    }
    pair->refcount++;
    pair->slave_count++;
    spin_unlock(&pair->lock);

    /* tty-level open (bumps tty refcount) */
    int ret = tty_open(pair->slave);
    if (ret != 0) {
        if (pty_pair_put(pair))
            pty_pair_destroy(pair);
        return ret;
    }

    file->ops = &pts_slave_file_ops;
    file->private_data = pair;
    pts_maybe_set_controlling_tty(pair, file->f_flags);
    return 0;
}

/* cdev open/release are no-ops; lifecycle is via vfs_file_ops */
static int pts_cdev_open(cdev_t *cdev)    { (void)cdev; return 0; }
static int pts_cdev_release(cdev_t *cdev) { (void)cdev; return 0; }

/* ================================================================== */
/*  PTY master — vfs_file_ops (installed by ptmx open_file)           */
/* ================================================================== */

static ssize_t ptmx_fops_read(struct vfs_file *file, char *buf,
                               size_t count, bool user) {
    struct pty_pair *pair = (struct pty_pair *)file->private_data;
    if (pair == NULL || pair->slave == NULL)
        return -ENXIO;

    /* Honour O_NONBLOCK on the master fd: temporarily set the pipe's
     * non-blocking read flag so pipe_read returns -EAGAIN instead of
     * sleeping when no data is available. */
    struct pipe *outp = pair->slave->output_pipe;
    if (outp && (file->f_flags & O_NONBLOCK)) {
        int old = pipe_get_flags(outp);
        pipe_set_flags(outp, old | (1 << PIPE_FLAGS_NONBLOCK_RD));
        ssize_t ret = pty_master_read(pair->slave, buf, count, user);
        pipe_set_flags(outp, old);
        return ret;
    }

    return pty_master_read(pair->slave, buf, count, user);
}

static ssize_t ptmx_fops_write(struct vfs_file *file, const char *buf,
                                size_t count, bool user) {
    struct pty_pair *pair = (struct pty_pair *)file->private_data;
    if (pair == NULL || pair->slave == NULL)
        return -ENXIO;
    return pty_master_write(pair->slave, buf, count, user);
}

static int ptmx_fops_release(struct vfs_inode *inode, struct vfs_file *file) {
    (void)inode;
    struct pty_pair *pair = (struct pty_pair *)file->private_data;
    if (pair == NULL)
        return 0;

    file->private_data = NULL;

    /* ---- master-specific teardown ---- */

    spin_lock(&pair->lock);
    pair->master_open = 0;
    pair->master_file = NULL;
    int do_unregister = pair->cdev_live;
    pair->cdev_live = 0;
    spin_unlock(&pair->lock);

    /* Hang up the slave tty so any blocked readers/writers unblock */
    if (pair->slave != NULL)
        tty_hangup(pair->slave);

    /* Unregister the slave cdev.  device_unregister() will also
     * remove /dev/pts/N from devtmpfs via dev->devname.  Since all
     * slave fds use open_file
     * (no file holds a kobject ref), the kobject drops to 0 immediately
     * and pts_cdev_release fires (which is a no-op).  Existing slave
     * fds continue to work because they use vfs_file_ops directly. */
    if (do_unregister)
        cdev_unregister(&pair->slave_cdev);

    /* Drop the pair ref — may destroy */
    if (pty_pair_put(pair))
        pty_pair_destroy(pair);

    return 0;
}

static int ptmx_fops_ioctl(struct vfs_file *file, uint64 cmd, void *arg) {
    struct pty_pair *pair = (struct pty_pair *)file->private_data;
    if (pair == NULL)
        return -ENXIO;

    switch (cmd) {
    case TIOCGPTN: {
        /* Return the slave PTY index (what N in /dev/pts/N) */
        int *idxp = (int *)arg;
        *idxp = pair->index;
        return 0;
    }
    case TIOCSPTLCK:
        /* PTY slave locking is not implemented; always succeed (no-op) */
        return 0;
    case TIOCGPTPEER:
        return pts_install_peer_fd(pair, (int)(uint64)arg);
    default:
        /* Forward termios / winsize ioctls to the slave tty */
        if (pair->slave != NULL)
            return tty_ioctl(pair->slave, cmd, arg);
        return -ENOTTY;
    }
}

static int ptmx_fops_poll(struct vfs_file *file, short events) {
    struct pty_pair *pair = (struct pty_pair *)file->private_data;
    if (pair == NULL || pair->slave == NULL)
        return 0;

    short revents = 0;

    /* Master is readable when the slave's output pipe has data */
    if (events & (POLLIN | POLLRDNORM | POLLRDBAND | POLLRDHUP)) {
        struct pipe *outp = pair->slave->output_pipe;
        if (outp != NULL) {
            uint nw = smp_load_acquire(&outp->nwrite);
            uint nr = smp_load_acquire(&outp->nread);
            if ((nw - nr) > 0)
                revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
            /* Slave side hung up: report hangup so the master detects EOF */
            if (!PIPE_WRITABLE(outp)) {
                revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
                revents |= POLLHUP;
                if (events & POLLRDHUP)
                    revents |= POLLRDHUP;
            }
        }
    }

    /* Master is always writable (slave input pipe has space) */
    if (events & (POLLOUT | POLLWRNORM | POLLWRBAND))
        revents |= (events & (POLLOUT | POLLWRNORM | POLLWRBAND));

    return revents;
}

static int ptmx_fops_stat(struct vfs_file *file, struct stat *st)
{
    struct pty_pair *pair = (struct pty_pair *)file->private_data;

    if (file == NULL || st == NULL)
        return -EINVAL;
    if (pair == NULL)
        return -ENXIO;

    memset(st, 0, sizeof(*st));
    st->st_dev = mkdev(PTMX_MAJOR, PTMX_MINOR);
    st->st_ino = (uint64)pair->index + 1;
    st->st_nlink = 1;
    st->st_mode = S_IFCHR | 0666;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_rdev = mkdev(PTMX_MAJOR, PTMX_MINOR);
    st->st_blksize = 4096;
    return 0;
}

static ssize_t ptmx_fops_readlink(struct vfs_file *file, char *buf,
                                  size_t buflen)
{
    (void)file;
    if (buf == NULL)
        return -EINVAL;
    return snprintf(buf, buflen, "/dev/ptmx");
}

static struct vfs_file_ops ptmx_master_file_ops = {
    .read    = ptmx_fops_read,
    .write   = ptmx_fops_write,
    .release = ptmx_fops_release,
    .stat    = ptmx_fops_stat,
    .readlink = ptmx_fops_readlink,
    .ioctl   = ptmx_fops_ioctl,
    .poll    = ptmx_fops_poll,
};

/* ================================================================== */
/*  /dev/ptmx — character device (open_file allocates a PTY pair)     */
/* ================================================================== */

static int ptmx_open_file(cdev_t *cdev, struct vfs_file *file) {
    (void)cdev;

    /* Allocate a PTY index — reuse freed slots */
    spin_lock(&ptmx_lock);
    int idx = -1;
    for (int i = 0; i < MAX_PTYS; i++) {
        if (pty_table[i] == NULL) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        spin_unlock(&ptmx_lock);
        return -ENOSPC;
    }
    /* Reserve the slot temporarily (set non-NULL so concurrent allocs skip it) */
    pty_table[idx] = (struct pty_pair *)1;
    spin_unlock(&ptmx_lock);

    /* Allocate the pair structure */
    struct pty_pair *pair = kvmalloc(sizeof(*pair));
    if (pair == NULL) {
        spin_lock(&ptmx_lock);
        pty_table[idx] = NULL;
        spin_unlock(&ptmx_lock);
        return -ENOMEM;
    }
    memset(pair, 0, sizeof(*pair));
    spin_init(&pair->lock, "pty_pair");

    pair->index       = idx;
    pair->refcount    = 1;          /* master fd */
    pair->master_open = 1;
    pair->cdev_live   = 0;

    /* Build slave name "pts/N" */
    char name[32];
    pts_name(name, idx);

    /* Allocate the slave tty */
    struct tty *slave = NULL;
    int dev_minor = idx + 1; /* device framework rejects minor 0 */
    int ret = pty_alloc(&slave, name, dev_minor);
    if (ret != 0) {
        spin_lock(&ptmx_lock);
        pty_table[idx] = NULL;
        spin_unlock(&ptmx_lock);
        kvfree(pair);
        return ret;
    }
    pair->slave = slave;

    /* Back-pointer so pty_slave_hangup can reach the pair */
    slave->driver_data = pair;

    /* Set up the slave cdev with open_file so slave fds get file ops */
    memset(&pair->slave_cdev, 0, sizeof(pair->slave_cdev));
    pair->slave_cdev.dev.major = PTS_MAJOR;
    pair->slave_cdev.dev.minor = dev_minor;
    pair->slave_cdev.readable = 1;
    pair->slave_cdev.writable = 1;
    pair->slave_cdev.ops.open      = pts_cdev_open;
    pair->slave_cdev.ops.release   = pts_cdev_release;
    pair->slave_cdev.ops.open_file = pts_open_file;

    /* devname/devmode so device_register() auto-creates the devtmpfs node */
    pair->slave_cdev.dev.devname = pair->slave->name;  /* e.g. "pts/0" */
    pair->slave_cdev.dev.devmode = S_IFCHR | 0620;

    ret = cdev_register(&pair->slave_cdev);
    if (ret != 0) {
        printf("ptmx: failed to register pts/%d cdev: %d\n", idx, ret);
        tty_unref(slave);
        spin_lock(&ptmx_lock);
        pty_table[idx] = NULL;
        spin_unlock(&ptmx_lock);
        kvfree(pair);
        return ret;
    }
    pair->cdev_live = 1;
    pts_snapshot_node_stat(pair);

    /* Record in global table */
    spin_lock(&ptmx_lock);
    pty_table[idx] = pair;
    spin_unlock(&ptmx_lock);

    /* Install master file ops on the opened file */
    file->ops = &ptmx_master_file_ops;
    file->private_data = pair;
    pair->master_file = file;

    return 0;
}

/* The ptmx cdev open/release are no-ops — open_file does the real work */
static int ptmx_cdev_open(cdev_t *cdev) { (void)cdev; return 0; }
static int ptmx_cdev_release(cdev_t *cdev) { (void)cdev; return 0; }

static cdev_t ptmx_cdev = {
    .dev = {
        .major = PTMX_MAJOR,
        .minor = PTMX_MINOR,
        .devname = "ptmx",
        .devmode = S_IFCHR | 0666,
    },
    .readable = 1,
    .writable = 1,
    .ops = {
        .open      = ptmx_cdev_open,
        .release   = ptmx_cdev_release,
        .open_file = ptmx_open_file,
    },
};

/* ================================================================== */
/*  Session-hangup cleanup (remove /dev/pts/N from devtmpfs)          */
/* ================================================================== */

/*
 * pty_hangup_cleanup — called from exit() OUTSIDE pid_wlock after
 * session_hangup has severed the pipe link.
 *
 * Unregisters the slave cdev so /dev/pts/N disappears immediately
 * when the session leader exits, instead of waiting for the master
 * fd to be closed by sshd-session.  Existing file descriptors keep
 * working (they use vfs_file_ops directly, not the cdev).
 */
void pty_hangup_cleanup(struct tty *tty) {
    if (tty == NULL)
        return;
    struct pty_pair *pair = (struct pty_pair *)tty->driver_data;
    if (pair == NULL)
        return;

    spin_lock(&pair->lock);
    int do_unregister = pair->cdev_live;
    pair->cdev_live = 0;
    spin_unlock(&pair->lock);

    if (do_unregister)
        cdev_unregister(&pair->slave_cdev);
}

/* ================================================================== */
/*  Initialization                                                    */
/* ================================================================== */

void ptmxinit(void) {
    int ret = cdev_register(&ptmx_cdev);
    assert(ret == 0, "ptmxinit: cdev_register failed: %d", ret);
    printf("ptmx: /dev/ptmx registered (major %d, minor %d)\n",
           PTMX_MAJOR, PTMX_MINOR);
}
