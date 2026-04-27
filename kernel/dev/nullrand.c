#include <types.h>
#include <param.h>
#include <riscv.h>
#include <defs.h>
#include <errno.h>
#include <string.h>
#include <dev/cdev.h>
#include <proc/thread.h>
#include <mm/vm.h>

static int null_open(cdev_t *cdev) { return 0; }

static int null_release(cdev_t *cdev) { return 0; }

static int null_read(cdev_t *cdev, bool user, void *buf, size_t count) {
    (void)cdev;
    (void)user;
    (void)buf;
    (void)count;
    return 0;
}

static int null_write(cdev_t *cdev, bool user, const void *buf, size_t count) {
    (void)cdev;
    (void)user;
    (void)buf;
    return count;
}

static uint64 __rand_state = 0x9e3779b97f4a7c15ULL;

static uint64 __xorshift64star(void) {
    uint64 x = __atomic_load_n(&__rand_state, __ATOMIC_RELAXED);
    x ^= r_time();
    x ^= (uint64)current;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    x *= 2685821657736338717ULL;
    __atomic_store_n(&__rand_state, x, __ATOMIC_RELAXED);
    return x;
}

void random_fill_bytes(void *buf, size_t count) {
    if (buf == NULL || count == 0) {
        return;
    }

    uint8 *dst = (uint8 *)buf;
    size_t i = 0;
    while (i < count) {
        uint64 r = __xorshift64star();
        for (int b = 0; b < 8 && i < count; b++, i++) {
            dst[i] = (uint8)(r & 0xFF);
            r >>= 8;
        }
    }
}

static int random_open(cdev_t *cdev) { return 0; }

static int random_release(cdev_t *cdev) { return 0; }

static int random_read(cdev_t *cdev, bool user, void *buf, size_t count) {
    (void)cdev;
    if (buf == NULL) {
        return -EINVAL;
    }
    if (count == 0) {
        return 0;
    }

    if (!user) {
        random_fill_bytes(buf, count);
        return count;
    }

    uint8 kbuf[64];
    size_t done = 0;
    while (done < count) {
        size_t chunk = count - done;
        if (chunk > sizeof(kbuf))
            chunk = sizeof(kbuf);

        random_fill_bytes(kbuf, chunk);

        if (either_copyout(1, (uint64)buf + done, kbuf, chunk) < 0) {
            return done ? (int)done : -EFAULT;
        }
        done += chunk;
    }

    return done;
}

static int random_write(cdev_t *cdev, bool user, const void *buf,
                        size_t count) {
    (void)cdev;
    (void)user;
    (void)buf;
    return count;
}

static cdev_t null_cdev = {
    .dev =
        {
            .major = NULL_MAJOR,
            .minor = NULL_MINOR,
            .devname = "null",
            .devmode = S_IFCHR | 0666,
        },
    .readable = 1,
    .writable = 1,
    .ops =
        {
            .read = null_read,
            .write = null_write,
            .open = null_open,
            .release = null_release,
            .ioctl = NULL,
            .poll = NULL, /* /dev/null is always ready — handled by fallback */
        },
};

static cdev_t random_cdev = {
    .dev =
        {
            .major = RANDOM_MAJOR,
            .minor = RANDOM_MINOR,
            .devname = "random",
            .devmode = S_IFCHR | 0666,
        },
    .readable = 1,
    .writable = 1,
    .ops =
        {
            .read = random_read,
            .write = random_write,
            .open = random_open,
            .release = random_release,
            .ioctl = NULL,
            .poll = NULL, /* /dev/random is always ready — handled by fallback */
        },
};

static cdev_t urandom_cdev = {
    .dev =
        {
            .major = URANDOM_MAJOR,
            .minor = URANDOM_MINOR,
            .devname = "urandom",
            .devmode = S_IFCHR | 0666,
        },
    .readable = 1,
    .writable = 1,
    .ops =
        {
            .read = random_read,
            .write = random_write,
            .open = random_open,
            .release = random_release,
            .ioctl = NULL,
            .poll = NULL, /* /dev/urandom is always ready — handled by fallback */
        },
};

/* ------------------------------------------------------------------ */
/*  /dev/zero — reads return zeroes, writes are discarded             */
/* ------------------------------------------------------------------ */

static int zero_open(cdev_t *cdev) { return 0; }

static int zero_release(cdev_t *cdev) { return 0; }

static int zero_read(cdev_t *cdev, bool user, void *buf, size_t count) {
    (void)cdev;
    if (buf == NULL)
        return -EINVAL;
    if (count == 0)
        return 0;

    if (!user) {
        memset(buf, 0, count);
        return count;
    }

    /* Zero out in chunks to userspace */
    static const char zeros[64] = {0};
    size_t done = 0;
    while (done < count) {
        size_t chunk = count - done;
        if (chunk > sizeof(zeros))
            chunk = sizeof(zeros);
        if (either_copyout(1, (uint64)buf + done, (char *)zeros, chunk) < 0)
            return done ? (int)done : -EFAULT;
        done += chunk;
    }
    return done;
}

static int zero_write(cdev_t *cdev, bool user, const void *buf, size_t count) {
    (void)cdev;
    (void)user;
    (void)buf;
    return count;
}

static cdev_t zero_cdev = {
    .dev =
        {
            .major = ZERO_MAJOR,
            .minor = ZERO_MINOR,
            .devname = "zero",
            .devmode = S_IFCHR | 0666,
        },
    .readable = 1,
    .writable = 1,
    .ops =
        {
            .read = zero_read,
            .write = zero_write,
            .open = zero_open,
            .release = zero_release,
            .ioctl = NULL,
            .poll = NULL, /* /dev/zero is always ready — handled by fallback */
        },
};

void nullranddevinit(void) {
    int ret = cdev_register(&null_cdev);
    assert(ret == 0, "nullranddevinit: failed to register null cdev: %d", ret);

    ret = cdev_register(&random_cdev);
    assert(ret == 0, "nullranddevinit: failed to register random cdev: %d",
           ret);

    ret = cdev_register(&urandom_cdev);
    assert(ret == 0, "nullranddevinit: failed to register urandom cdev: %d",
           ret);

    ret = cdev_register(&zero_cdev);
    assert(ret == 0, "nullranddevinit: failed to register zero cdev: %d", ret);
}
