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
#include "devtmpfs.h"
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

/* Monotonically increasing PTY index for devtmpfs naming */
static int __pty_next_index = 0;

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

    /* Register a devtmpfs entry for this PTY slave (e.g. /dev/pts/0) */
    int idx;
    if (minor >= 0)
        idx = minor;
    else
        idx = __atomic_fetch_add(&__pty_next_index, 1, __ATOMIC_SEQ_CST);
    dev_t dev = mkdev(PTY_MAJOR, PTY_MINOR_BASE + idx);
    devtmpfs_create_node(name, S_IFCHR | 0620, dev);

    *slave_out = slave;
    return 0;
}

/*
 * pty_dealloc - clean up the devtmpfs entry for a PTY pair
 *
 * Call this when the PTY master is being closed and the slave should
 * no longer be accessible via /dev.  The tty struct itself is freed
 * separately via tty_unref().
 *
 * @slave: the slave tty (used for its name field)
 */
void pty_dealloc(struct tty *slave) {
    if (slave == NULL)
        return;

    /* Remove /dev/<name> where name is e.g. "pts/0" */
    if (slave->name[0] != '\0')
        devtmpfs_remove_node(slave->name);
}
