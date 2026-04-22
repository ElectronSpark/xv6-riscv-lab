/*
 * tty_dev.c - /dev/tty character device
 *
 * Provides a character device node at /dev/tty (major 5, minor 0) that
 * forwards read/write/ioctl operations to the calling process's
 * controlling terminal.  If the process has no controlling terminal,
 * operations return -ENXIO.
 *
 * This is analogous to Linux's /dev/tty; in this kernel, minor 1 is used
 * because minor 0 is reserved by the device table allocator.
 */

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "errno.h"
#include "defs.h"
#include "dev/cdev.h"
#include "tty/tty.h"
#include "tty/session.h"
#include "proc/thread.h"
#include "smp/percpu.h"

/* Major/minor for /dev/tty */
#define TTY_DEV_MAJOR 5
#define TTY_DEV_MINOR 1

/*
 * Get the current process's controlling terminal.
 * Returns NULL if the process has no session or no controlling tty.
 */
static struct tty *__get_ctrl_tty(void) {
    struct thread *t = current;
    if (t == NULL || t->session == NULL)
        return NULL;
    return session_get_ctrl_tty(t->session);
}

static int ttydev_open(cdev_t *cdev) {
    (void)cdev;
    struct tty *tty = __get_ctrl_tty();
    if (tty == NULL)
        return -ENXIO;
    return tty_open(tty);
}

static int ttydev_release(cdev_t *cdev) {
    (void)cdev;
    struct tty *tty = __get_ctrl_tty();
    if (tty == NULL)
        return 0;
    tty_close(tty);
    return 0;
}

static int ttydev_read(cdev_t *cdev, bool user, void *buf, size_t count) {
    (void)cdev;
    struct tty *tty = __get_ctrl_tty();
    if (tty == NULL)
        return -ENXIO;
    return tty_read(tty, (char *)buf, count, user);
}

static int ttydev_write(cdev_t *cdev, bool user, const void *buf,
                        size_t count) {
    (void)cdev;
    struct tty *tty = __get_ctrl_tty();
    if (tty == NULL)
        return -ENXIO;
    return tty_write(tty, (const char *)buf, count, user);
}

static int ttydev_ioctl(cdev_t *cdev, uint64 cmd, void *arg) {
    (void)cdev;
    struct tty *tty = __get_ctrl_tty();
    if (tty == NULL)
        return -ENXIO;
    return tty_ioctl(tty, cmd, arg);
}

static int ttydev_poll(cdev_t *cdev, short events) {
    (void)cdev;
    struct tty *tty = __get_ctrl_tty();
    if (tty == NULL)
        return 0;
    return tty_poll(tty, events);
}

static cdev_t tty_cdev = {
    .dev =
        {
            .major = TTY_DEV_MAJOR,
            .minor = TTY_DEV_MINOR,
            .devname = "tty",
            .devmode = S_IFCHR | 0666,
        },
    .readable = 1,
    .writable = 1,
    .ops =
        {
            .read = ttydev_read,
            .write = ttydev_write,
            .open = ttydev_open,
            .release = ttydev_release,
            .ioctl = ttydev_ioctl,
            .poll = ttydev_poll,
        },
};

void ttydevinit(void) {
    int ret = cdev_register(&tty_cdev);
    assert(ret == 0, "ttydevinit: cdev_register failed: %d", ret);
}
