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
    struct pty_pair *pair = container_of(cdev, struct pty_pair, slave_cdev);
    if (pair->slave == NULL)
        return -ENXIO;
    return tty_open(pair->slave);
}

static int pts_cdev_release(cdev_t *cdev) {
    struct pty_pair *pair = container_of(cdev, struct pty_pair, slave_cdev);

    /*
     * This callback fires when the device kobject refcount reaches 0,
     * i.e. after device_unregister + all user cdev_put calls.
     * It is safe to tear down the pair and free everything here.
     */

    /* Close the slave tty (drops the "register-open" tty ref) */
    if (pair->slave != NULL)
        tty_close(pair->slave);

    /* Drop the initial allocation tty ref (from pty_alloc/tty_alloc) */
    if (pair->slave != NULL) {
        tty_unref(pair->slave);
        pair->slave = NULL;
    }

    /* Clear pty_table slot */
    spin_lock(&ptmx_lock);
    if (pty_table[pair->index] == pair)
        pty_table[pair->index] = NULL;
    spin_unlock(&ptmx_lock);

    /* Free the pair (slave_cdev is embedded, so this is the last use) */
    kmm_free(pair);
    return 0;
}

static int pts_cdev_read(cdev_t *cdev, bool user, void *buf, size_t count) {
    struct pty_pair *pair = container_of(cdev, struct pty_pair, slave_cdev);
    if (pair->slave == NULL)
        return -ENXIO;
    return tty_read(pair->slave, (char *)buf, count, user);
}

static int pts_cdev_write(cdev_t *cdev, bool user, const void *buf,
                          size_t count) {
    struct pty_pair *pair = container_of(cdev, struct pty_pair, slave_cdev);
    if (pair->slave == NULL)
        return -ENXIO;
    return tty_write(pair->slave, (const char *)buf, count, user);
}

static int pts_cdev_ioctl(cdev_t *cdev, uint64 cmd, void *arg) {
    struct pty_pair *pair = container_of(cdev, struct pty_pair, slave_cdev);
    if (pair->slave == NULL)
        return -ENXIO;
    return tty_ioctl(pair->slave, cmd, arg);
}

static int pts_cdev_poll(cdev_t *cdev, short events) {
    struct pty_pair *pair = container_of(cdev, struct pty_pair, slave_cdev);
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

    /* Remove /dev/pts/N from devtmpfs so no NEW opens succeed */
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

    pair->master_open = 0;
    file->private_data = NULL;

    /*
     * Unregister the slave cdev.  This drops the initial kobject ref.
     * If user processes still have open fds to /dev/pts/N, the kobject
     * refcount stays > 0 and pts_cdev_release runs later (when the
     * last user closes the slave fd).  If no user fds remain, the
     * kobject reaches 0 synchronously and pts_cdev_release fires
     * HERE — freeing pair.  After this call, pair may be INVALID.
     */
    cdev_unregister(&pair->slave_cdev);
    /* pair is potentially freed — do NOT touch it after this point */

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
    int dev_minor = idx + 1; /* device framework rejects minor 0 */
    int ret = pty_alloc(&slave, name, dev_minor);
    if (ret != 0) {
        kmm_free(pair);
        return ret;
    }
    pair->slave = slave;

    /* Set up the slave cdev at (PTS_MAJOR, dev_minor) */
    memset(&pair->slave_cdev, 0, sizeof(pair->slave_cdev));
    pair->slave_cdev.dev.major = PTS_MAJOR;
    pair->slave_cdev.dev.minor = dev_minor;
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
