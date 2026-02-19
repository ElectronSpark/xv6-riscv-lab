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
#include "vfs/vfs_types.h"
#include "vfs/file.h"
#include "vfs/poll.h"
#include "vfs/pipe.h"
#include "tty/tty.h"
#include "tty/termios.h"
#include "devtmpfs.h"
#include "vfs/stat.h"

/* ---- Constants ---- */

#define PTMX_MAJOR    5
#define PTMX_MINOR    2
#define PTS_MAJOR     136   /* Same as PTY_MAJOR in param.h */
#define MAX_PTYS      64

/* ---- Per-PTY state ---- */

struct pty_pair {
    struct tty  *slave;         /* The slave tty (allocated by pty_alloc) */
    cdev_t       slave_cdev;    /* Registered cdev for /dev/pts/N */
    int          index;         /* PTY index (N in /dev/pts/N) */
    int          refcount;      /* Open master + slave fds */
    int          master_open;   /* Master side still alive? */
    int          cdev_live;     /* Slave cdev still registered? */
    spinlock_t   lock;          /* Protects refcount / flags */
};

static spinlock_t ptmx_lock = SPINLOCK_INITIALIZED("ptmx");
static struct pty_pair *pty_table[MAX_PTYS];
static slab_cache_t pty_pair_cache;

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

    slab_free(pair);
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
    if (pair == NULL || pair->slave == NULL)
        return -EIO;
    return tty_write(pair->slave, (const char *)buf, count, user);
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

static int pts_fops_release(struct vfs_inode *inode, struct vfs_file *file)
{
    (void)inode;
    struct pty_pair *pair = (struct pty_pair *)file->private_data;
    if (pair == NULL)
        return 0;

    file->private_data = NULL;

    /* Drop the tty-level "open" ref taken in pts_open_file */
    tty_close(pair->slave);

    /* Drop the pair ref — may destroy */
    if (pty_pair_put(pair))
        pty_pair_destroy(pair);

    return 0;
}

static struct vfs_file_ops pts_slave_file_ops = {
    .read    = pts_fops_read,
    .write   = pts_fops_write,
    .release = pts_fops_release,
    .ioctl   = pts_fops_ioctl,
    .poll    = pts_fops_poll,
};

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
    if (events & POLLIN) {
        struct pipe *outp = pair->slave->output_pipe;
        if (outp != NULL) {
            uint nw = smp_load_acquire(&outp->nwrite);
            uint nr = smp_load_acquire(&outp->nread);
            if ((nw - nr) > 0)
                revents |= POLLIN;
        }
    }

    /* Master is always writable (slave input pipe has space) */
    if (events & POLLOUT)
        revents |= POLLOUT;

    return revents;
}

static struct vfs_file_ops ptmx_master_file_ops = {
    .read    = ptmx_fops_read,
    .write   = ptmx_fops_write,
    .release = ptmx_fops_release,
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
    struct pty_pair *pair = slab_alloc(&pty_pair_cache);
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
        slab_free(pair);
        return ret;
    }
    pair->slave = slave;

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
        slab_free(pair);
        return ret;
    }
    pair->cdev_live = 1;

    /* Record in global table */
    spin_lock(&ptmx_lock);
    pty_table[idx] = pair;
    spin_unlock(&ptmx_lock);

    /* Install master file ops on the opened file */
    file->ops = &ptmx_master_file_ops;
    file->private_data = pair;

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
/*  Initialization                                                    */
/* ================================================================== */

void ptmxinit(void) {
    int ret = slab_cache_init(&pty_pair_cache, "pty_pair",
                              sizeof(struct pty_pair), SLAB_FLAG_STATIC);
    assert(ret == 0, "ptmxinit: slab_cache_init failed: %d", ret);

    ret = cdev_register(&ptmx_cdev);
    assert(ret == 0, "ptmxinit: cdev_register failed: %d", ret);
    printf("ptmx: /dev/ptmx registered (major %d, minor %d)\n",
           PTMX_MAJOR, PTMX_MINOR);
}
