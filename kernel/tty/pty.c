/*
 * pty.c - Pseudo-terminal (PTY) implementation
 *
 * A PTY pair consists of a master and a slave.  Data written to the
 * master appears as input on the slave (passes through the slave's
 * line discipline), and data written to the slave appears as output
 * readable from the master.
 *
 * The master is just two pipes; the slave is a full struct tty that
 * uses those pipes.  pty_alloc() creates the pair.
 *
 * Implementation
 * ──────────────
 *   master-write  ──►  slave tty_input()  (line discipline)
 *   master-read   ◄──  slave output pipe
 */

#include "types.h"
#include "param.h"
#include "errno.h"
#include "string.h"
#include "printf.h"
#include "lock/spinlock.h"
#include "mm/slab.h"
#include "mm/vm.h"
#include "vfs/pipe.h"
#include "vfs/stat.h"
#include "defs.h"
#include "tty/tty.h"

/* ------------------------------------------------------------------ */
/*  PTY slave ops                                                     */
/* ------------------------------------------------------------------ */

static int pty_slave_open(struct tty *tty) {
    (void)tty;
    return 0;
}

static void pty_slave_close(struct tty *tty) { (void)tty; }

static ssize_t pty_slave_write(struct tty *tty, const char *buf, size_t nr) {
    /* Slave write → push into output pipe (master can read it) */
    return pipe_write(tty->output_pipe, buf, nr, 0);
}

static ssize_t pty_slave_read(struct tty *tty, char *buf, size_t nr) {
    /* Slave read ← pull from input pipe (fed by master write) */
    return pipe_read(tty->input_pipe, buf, nr, 0);
}

static struct tty_ops pty_slave_ops = {
    .open = pty_slave_open,
    .close = pty_slave_close,
    .read = pty_slave_read,
    .write = pty_slave_write,
};

/* ------------------------------------------------------------------ */
/*  PTY master                                                        */
/* ------------------------------------------------------------------ */

/*
 * pty_master_write - data from the master goes through the slave's
 *                    line-discipline via tty_input().
 */
ssize_t pty_master_write(struct tty *slave, const char *buf, size_t count,
                         bool user) {
    char kbuf[64];
    size_t written = 0;

    while (written < count) {
        size_t batch = count - written;
        if (batch > sizeof(kbuf))
            batch = sizeof(kbuf);

        if (user) {
            if (either_copyin(kbuf, 1, (uint64)buf + written, batch) < 0)
                return (ssize_t)(written ? written : -EFAULT);
        } else {
            memmove(kbuf, buf + written, batch);
        }

        /* Feed through the slave's line discipline */
        ssize_t ret = tty_input(slave, kbuf, batch);
        if (ret < 0)
            return (ssize_t)(written ? written : ret);

        written += (size_t)ret;
    }

    return (ssize_t)written;
}

/*
 * pty_master_read - read data that the slave has written
 *                   (i.e., the slave's output pipe).
 */
ssize_t pty_master_read(struct tty *slave, char *buf, size_t count, bool user) {
    return pipe_read(slave->output_pipe, buf, count, user);
}

/* ------------------------------------------------------------------ */
/*  Allocation                                                        */
/* ------------------------------------------------------------------ */

/*
 * pty_alloc - create a PTY master/slave pair
 *
 * @slave_out: on success, receives a pointer to the slave tty
 * @name:      base name for the slave (e.g. "pts/0")
 * @minor:     minor device number for /dev/pts/N (if < 0, auto-assign)
 *
 * Returns 0 on success, negative errno on failure.
 *
 * The caller interacts with the master side through
 * pty_master_read / pty_master_write, passing the slave tty pointer.
 */
int pty_alloc(struct tty **slave_out, const char *name, int minor) {
    struct tty *slave = tty_alloc(name, &pty_slave_ops);
    if (IS_ERR(slave))
        return PTR_ERR(slave);

    /* devtmpfs node is now created automatically by device_register()
     * when the slave cdev is registered in ptmx_open_file(). */
    (void)minor;

    *slave_out = slave;
    return 0;
}

/*
 * pty_dealloc - clean up for a PTY pair
 *
 * The devtmpfs node is now removed automatically by device_unregister()
 * when the slave cdev is unregistered (via dev->devname).
 * This function is retained for any future non-devtmpfs cleanup.
 *
 * @slave: the slave tty (used for its name field)
 */
void pty_dealloc(struct tty *slave) {
    if (slave == NULL)
        return;
    /* devtmpfs removal is handled by cdev_unregister → device_unregister */
}
