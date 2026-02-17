/*
 * ptmx.c - /dev/ptmx and /dev/pts/N character devices
 *
 * Implements the Linux-style PTY multiplexer interface:
 *
 *   /dev/ptmx  (major 5, minor 2)
 *     Opening this device allocates a new PTY pair.  The returned fd is
 *     the master side.  The slave index can be obtained via TIOCGPTN
 *     ioctl, and the corresponding slave is at /dev/pts/<index>.
 *
 *   /dev/pts/N (major 136, minor N)
 *     Each slave is a character device that wraps a struct tty with
 *     the PTY slave ops.  Opening /dev/pts/N gives the slave side
 *     of PTY pair N.
 *
 * The ptmx cdev uses the open_file callback to transform the opened
 * vfs_file into a master PTY file descriptor with custom file ops.
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
    struct tty *slave;      /* The slave tty (allocated by pty_alloc) */
    cdev_t      slave_cdev; /* Registered cdev for /dev/pts/N */
    int         index;      /* PTY index (= minor number) */
    int         master_open;/* Whether a master fd is still open */
};

static spinlock_t ptmx_lock = SPINLOCK_INITIALIZED("ptmx");
static struct pty_pair *pty_table[MAX_PTYS]; /* indexed by minor */

/* ================================================================== */
/*  /dev/pts/N — slave character device                               */
/* ================================================================== */

static int pts_cdev_open(cdev_t *cdev) {
    struct pty_pair *pair = (struct pty_pair *)cdev;
    if (pair->slave == NULL)
        return -ENXIO;
    return tty_open(pair->slave);
}

static int pts_cdev_release(cdev_t *cdev) {
    struct pty_pair *pair = (struct pty_pair *)cdev;
    if (pair->slave != NULL)
        tty_close(pair->slave);
    return 0;
}

static int pts_cdev_read(cdev_t *cdev, bool user, void *buf, size_t count) {
    struct pty_pair *pair = (struct pty_pair *)cdev;
    if (pair->slave == NULL)
        return -ENXIO;
    return tty_read(pair->slave, (char *)buf, count, user);
}

static int pts_cdev_write(cdev_t *cdev, bool user, const void *buf,
                          size_t count) {
    struct pty_pair *pair = (struct pty_pair *)cdev;
    if (pair->slave == NULL)
        return -ENXIO;
    return tty_write(pair->slave, (const char *)buf, count, user);
}

static int pts_cdev_ioctl(cdev_t *cdev, uint64 cmd, void *arg) {
    struct pty_pair *pair = (struct pty_pair *)cdev;
    if (pair->slave == NULL)
        return -ENXIO;
    return tty_ioctl(pair->slave, cmd, arg);
}

static int pts_cdev_poll(cdev_t *cdev, short events) {
    struct pty_pair *pair = (struct pty_pair *)cdev;
    if (pair->slave == NULL)
        return 0;
    return tty_poll(pair->slave, events);
}

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

    int idx = pair->index;

    /* Hang up the slave tty so any blocked readers/writers unblock */
    if (pair->slave != NULL)
        tty_hangup(pair->slave);

    /* Unregister the slave cdev */
    cdev_unregister(&pair->slave_cdev);

    /* Remove /dev/pts/N from devtmpfs */
    {
        char name[32];
        char tmp[16];
        int ti = 0;
        int v = idx;
        if (v == 0) {
            tmp[ti++] = '0';
        } else {
            while (v > 0) {
                tmp[ti++] = '0' + (v % 10);
                v /= 10;
            }
        }
        const char *prefix = "pts/";
        int nlen = 0;
        int pi = 0;
        while (prefix[pi])
            name[nlen++] = prefix[pi++];
        for (int i = ti - 1; i >= 0; i--)
            name[nlen++] = tmp[i];
        name[nlen] = '\0';
        devtmpfs_remove_node(name);
    }

    /* Release the slave tty */
    if (pair->slave != NULL)
        tty_unref(pair->slave);

    /* Clear table slot and free */
    spin_lock(&ptmx_lock);
    pty_table[idx] = NULL;
    pair->master_open = 0;
    spin_unlock(&ptmx_lock);

    kmm_free(pair);
    file->private_data = NULL;

    return 0;
}

static int ptmx_fops_ioctl(struct vfs_file *file, uint64 cmd, void *arg) {
    struct pty_pair *pair = (struct pty_pair *)file->private_data;
    if (pair == NULL)
        return -ENXIO;

    switch (cmd) {
    case TIOCGPTN: {
        /* Return the slave PTY index (what N in /dev/pts/N) */
        int idx = pair->index;
        if (either_copyout(1, (uint64)arg, (char *)&idx, sizeof(idx)) < 0)
            return -EFAULT;
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
    /* Simplified: master always reports writable, and readable is
     * approximated by slave output pipe readability.  A proper
     * implementation would check pipe data counts. */
    short revents = 0;
    if (events & POLLIN)
        revents |= POLLIN;  /* optimistic; read will block if no data */
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
    struct pty_pair *pair = kmm_alloc(sizeof(*pair));
    if (pair == NULL)
        return -ENOMEM;
    memset(pair, 0, sizeof(*pair));

    pair->index = idx;
    pair->master_open = 1;

    /* Build slave name "pts/N" */
    char name[32];
    int nlen = 0;
    /* Manual int-to-string for the index */
    {
        char tmp[16];
        int ti = 0;
        int v = idx;
        if (v == 0) {
            tmp[ti++] = '0';
        } else {
            while (v > 0) {
                tmp[ti++] = '0' + (v % 10);
                v /= 10;
            }
        }
        const char *prefix = "pts/";
        int pi = 0;
        while (prefix[pi]) {
            name[nlen++] = prefix[pi++];
        }
        for (int i = ti - 1; i >= 0; i--) {
            name[nlen++] = tmp[i];
        }
        name[nlen] = '\0';
    }

    /* Allocate the slave tty */
    struct tty *slave = NULL;
    int ret = pty_alloc(&slave, name);
    if (ret != 0) {
        kmm_free(pair);
        return ret;
    }
    pair->slave = slave;

    /* Set up the slave cdev at (PTS_MAJOR, idx) */
    memset(&pair->slave_cdev, 0, sizeof(pair->slave_cdev));
    pair->slave_cdev.dev.major = PTS_MAJOR;
    pair->slave_cdev.dev.minor = idx;
    pair->slave_cdev.readable = 1;
    pair->slave_cdev.writable = 1;
    pair->slave_cdev.ops.open    = pts_cdev_open;
    pair->slave_cdev.ops.release = pts_cdev_release;
    pair->slave_cdev.ops.read    = pts_cdev_read;
    pair->slave_cdev.ops.write   = pts_cdev_write;
    pair->slave_cdev.ops.ioctl   = pts_cdev_ioctl;
    pair->slave_cdev.ops.poll    = pts_cdev_poll;

    ret = cdev_register(&pair->slave_cdev);
    if (ret != 0) {
        printf("ptmx: failed to register pts/%d cdev: %d\n", idx, ret);
        tty_unref(slave);
        kmm_free(pair);
        return ret;
    }

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
    int ret = cdev_register(&ptmx_cdev);
    assert(ret == 0, "ptmxinit: cdev_register failed: %d", ret);
    printf("ptmx: /dev/ptmx registered (major %d, minor %d)\n",
           PTMX_MAJOR, PTMX_MINOR);
}
