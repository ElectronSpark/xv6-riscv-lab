#include <types.h>
#include <param.h>
#include <riscv.h>
#include <defs.h>
#include <errno.h>
#include <string.h>
#include <dev/cdev.h>
#include <dev/evdev.h>
#include <lock/spinlock.h>
#include <proc/sched.h>
#include <timer/timer.h>
#include <kqueue.h>
#include <kqueue_types.h>
#include <vfs/poll.h>
#include <vfs/file.h>
#include <mm/vm.h>
#include <cmdline.h>

#define EVDEV_MAJOR 13
#define EVDEV_KEYBOARD_MINOR 64
#define EVDEV_POINTER_MINOR 65

#define EV_VERSION 0x010001

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_MSC 0x04

#define SYN_REPORT 0

#define REL_X 0x00
#define REL_Y 0x01
#define REL_WHEEL 0x08

#define ABS_X 0x00
#define ABS_Y 0x01

#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112

#define INPUT_PROP_POINTER 0x00

#define KEY_ESC 1
#define KEY_LEFTCTRL 29
#define KEY_LEFTSHIFT 42
#define KEY_RIGHTSHIFT 54
#define KEY_LEFTALT 56
#define KEY_HOME 102
#define KEY_UP 103
#define KEY_PAGEUP 104
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_END 107
#define KEY_DOWN 108
#define KEY_PAGEDOWN 109
#define KEY_INSERT 110
#define KEY_DELETE 111

#define BUS_VIRTUAL 0x06

#define IOC_NRBITS 8
#define IOC_TYPEBITS 8
#define IOC_SIZEBITS 14
#define IOC_DIRBITS 2
#define IOC_NRSHIFT 0
#define IOC_TYPESHIFT (IOC_NRSHIFT + IOC_NRBITS)
#define IOC_SIZESHIFT (IOC_TYPESHIFT + IOC_TYPEBITS)
#define IOC_DIRSHIFT (IOC_SIZESHIFT + IOC_SIZEBITS)
#define IOC_NONE 0U
#define IOC_WRITE 1U
#define IOC_READ 2U
#define IOC(dir, type, nr, size) \
    (((dir) << IOC_DIRSHIFT) | ((type) << IOC_TYPESHIFT) | \
     ((nr) << IOC_NRSHIFT) | ((size) << IOC_SIZESHIFT))
#define IO(type, nr) IOC(IOC_NONE, (type), (nr), 0)
#define IOR(type, nr, size) IOC(IOC_READ, (type), (nr), sizeof(size))
#define IOW(type, nr, size) IOC(IOC_WRITE, (type), (nr), sizeof(size))

#define IOC_TYPE(cmd) (((cmd) >> IOC_TYPESHIFT) & ((1U << IOC_TYPEBITS) - 1))
#define IOC_NR(cmd) (((cmd) >> IOC_NRSHIFT) & ((1U << IOC_NRBITS) - 1))
#define IOC_SIZE(cmd) (((cmd) >> IOC_SIZESHIFT) & ((1U << IOC_SIZEBITS) - 1))

#define EVIOCGVERSION IOR('E', 0x01, int)
#define EVIOCGID IOR('E', 0x02, struct input_id)
#define EVIOCGREP IOR('E', 0x03, uint32[2])
#define EVIOCGPROP(len) IOC(IOC_READ, 'E', 0x09, len)
#define EVIOCGKEY(len) IOC(IOC_READ, 'E', 0x18, len)
#define EVIOCGLED(len) IOC(IOC_READ, 'E', 0x19, len)
#define EVIOCGSND(len) IOC(IOC_READ, 'E', 0x1a, len)
#define EVIOCGSW(len) IOC(IOC_READ, 'E', 0x1b, len)
#define EVIOCGABS(abs) IOR('E', 0x40 + (abs), struct input_absinfo)
#define EVIOCGRAB IOW('E', 0x90, int)
#define EVIOCSCLOCKID IOW('E', 0xa0, int)

#define LONG_BITS 64
#define BIT_WORD(bit) ((bit) / LONG_BITS)
#define BIT_MASK(bit) (1ULL << ((bit) % LONG_BITS))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

struct input_event {
    uint64 sec;
    uint64 usec;
    uint16 type;
    uint16 code;
    int32 value;
};

struct input_id {
    uint16 bustype;
    uint16 vendor;
    uint16 product;
    uint16 version;
};

struct input_absinfo {
    int32 value;
    int32 minimum;
    int32 maximum;
    int32 fuzz;
    int32 flat;
    int32 resolution;
};

struct evdev_state {
    cdev_t cdev;
    const char *name;
    const char *phys;
    int is_pointer;
    spinlock_t lock;
    struct evdev_client *clients;
    uint64 key_bits[12];
    uint64 key_down[12];
    uint64 prop_bits[1];
    uint64 rel_bits[1];
    uint64 abs_bits[1];
    uint16 last_buttons;
    int32 abs_x;
    int32 abs_y;
};

struct evdev_client {
    struct evdev_state *state;
    struct evdev_client *next;
    /* N5: back-pointer to the open file, protected by st->lock.  Knotes
     * from poll/epoll attach to the per-open FILE knote list (because
     * evdev_file_ops has .poll), so readiness must be delivered via
     * vfs_file_knote_notify on this file — cdev_knote_notify walks only
     * the cdev list, which stays empty for evdev fds. */
    struct vfs_file *file;
    struct input_event ring[256];
    int head;
    int tail;
};

#define EVDEV_NOTIFY_MAX_CLIENTS 8

static struct evdev_state evkbd;
static struct evdev_state evptr;
static int evdev_ready;
static int evdev_trace;

static uint64 evdev_monotonic_us(void)
{
    uint64 ticks = r_time();
    uint64 freq = __timebase_frequency ? __timebase_frequency : 10000000UL;

    /* Match CLOCK_MONOTONIC, which sys_clock_gettime() derives from the
     * same architecture timebase.  libinput compares input_event timestamps
     * with that clock and treats stale timestamps as an input-processing
     * backlog.  A synthetic per-record counter starts far behind boot time
     * and advances faster for multi-record pointer frames, producing exactly
     * that false backlog. */
    return (ticks / freq) * 1000000ULL +
           ((ticks % freq) * 1000000ULL) / freq;
}

static void set_bit(uint64 *bits, uint code)
{
    bits[BIT_WORD(code)] |= BIT_MASK(code);
}

static int client_ring_empty(struct evdev_client *client)
{
    return client->head == client->tail;
}

static void client_ring_push_locked(struct evdev_client *client,
                                    const struct input_event *src)
{
    int next = (client->tail + 1) % ARRAY_SIZE(client->ring);
    struct input_event *ev;

    if (next == client->head)
        client->head = (client->head + 1) % ARRAY_SIZE(client->ring);
    ev = &client->ring[client->tail];
    *ev = *src;
    client->tail = next;
}

static void ring_push_locked(struct evdev_state *st, uint16 type, uint16 code,
                             int32 value)
{
    struct input_event ev;
    struct evdev_client *client;

    uint64 now_us = evdev_monotonic_us();

    ev.sec = now_us / 1000000;
    ev.usec = now_us % 1000000;
    ev.type = type;
    ev.code = code;
    ev.value = value;

    for (client = st->clients; client != NULL; client = client->next)
        client_ring_push_locked(client, &ev);
    if (evdev_trace && st->is_pointer) {
        printf("evdev: push %s type=%u code=%u value=%d clients=%p\n",
               st->name, type, code, value, st->clients);
    }
}

static void notify(struct evdev_state *st)
{
    struct evdev_client *client;
    struct vfs_file *files[EVDEV_NOTIFY_MAX_CLIENTS];
    int nfiles = 0;

    spin_lock(&st->lock);
    for (client = st->clients; client != NULL; client = client->next) {
        wakeup_on_chan(&client->ring);
        if (client->file != NULL && nfiles < EVDEV_NOTIFY_MAX_CLIENTS)
            files[nfiles++] = vfs_fdup(client->file);
    }
    spin_unlock(&st->lock);
    cdev_knote_notify(&st->cdev, EVFILT_READ, 0);
    /* N5 fix: knotes for evdev fds live on the per-open FILE lists (see
     * struct evdev_client.file) — without this, poll/epoll waiters only
     * ever saw input via the rescan safety net, and a notify-backed
     * full wait (poll_notify_full_wait=1) froze input forever.  Notify
     * OUTSIDE st->lock: kqueue_wait re-checks levels via ops->poll
     * (which takes st->lock) while holding kq->lock. */
    for (int i = 0; i < nfiles; i++) {
        vfs_file_knote_notify(files[i], EVFILT_READ, 0);
        vfs_fput(files[i]);
    }
}

static uint16 linux_key_from_kbd_event(const struct kbd_event *ev)
{
    if (ev->keycode >= KBD_KEY_UP) {
        switch (ev->keycode) {
        case KBD_KEY_UP: return KEY_UP;
        case KBD_KEY_DOWN: return KEY_DOWN;
        case KBD_KEY_LEFT: return KEY_LEFT;
        case KBD_KEY_RIGHT: return KEY_RIGHT;
        case KBD_KEY_HOME: return KEY_HOME;
        case KBD_KEY_END: return KEY_END;
        case KBD_KEY_PGUP: return KEY_PAGEUP;
        case KBD_KEY_PGDN: return KEY_PAGEDOWN;
        case KBD_KEY_INSERT: return KEY_INSERT;
        case KBD_KEY_DELETE: return KEY_DELETE;
        default: return 0;
        }
    }
    if (ev->scancode != 0)
        return ev->scancode & 0x7f;
    if (ev->keycode == 27)
        return KEY_ESC;
    return 0;
}

void evdev_keyboard_event(const struct kbd_event *ev)
{
    uint16 key;

    if (!ev)
        return;
    if (!evdev_ready)
        return;
    key = linux_key_from_kbd_event(ev);
    if (key == 0 || key >= ARRAY_SIZE(evkbd.key_bits) * LONG_BITS)
        return;

    spin_lock(&evkbd.lock);
    if (ev->pressed)
        set_bit(evkbd.key_down, key);
    else
        evkbd.key_down[BIT_WORD(key)] &= ~BIT_MASK(key);
    ring_push_locked(&evkbd, EV_KEY, key, ev->pressed ? 1 : 0);
    ring_push_locked(&evkbd, EV_SYN, SYN_REPORT, 0);
    spin_unlock(&evkbd.lock);
    notify(&evkbd);
}

static void pointer_button(struct evdev_state *st, uint16 linux_button,
                           uint16 mask, uint16 buttons)
{
    uint16 old = st->last_buttons & mask;
    uint16 now = buttons & mask;

    if (old != now)
        ring_push_locked(st, EV_KEY, linux_button, now ? 1 : 0);
}

void evdev_pointer_event(const struct mouse_event *ev)
{
    if (!ev)
        return;
    if (!evdev_ready)
        return;

    spin_lock(&evptr.lock);
    pointer_button(&evptr, BTN_LEFT, 0x01, ev->buttons);
    pointer_button(&evptr, BTN_RIGHT, 0x02, ev->buttons);
    pointer_button(&evptr, BTN_MIDDLE, 0x04, ev->buttons);
    evptr.last_buttons = ev->buttons;

    if (ev->flags & MOUSE_EVENT_F_ABSOLUTE) {
        int32 abs_x = (int32)(uint16)ev->dx;
        int32 abs_y = (int32)(uint16)ev->dy;

        evptr.abs_x = abs_x;
        evptr.abs_y = abs_y;
        /* A Linux absolute tablet reports EV_ABS for an absolute packet; it
         * does not also synthesize an EV_REL delta.  Sending both makes
         * libinput apply the same move twice.  In Plasma that can move a
         * click from the launcher into a newly opened menu and activate an
         * unrelated application.  The shared device still advertises and
         * emits EV_REL for genuine PS/2 relative packets below. */
        ring_push_locked(&evptr, EV_ABS, ABS_X, evptr.abs_x);
        ring_push_locked(&evptr, EV_ABS, ABS_Y, evptr.abs_y);
    } else {
        if (ev->dx)
            ring_push_locked(&evptr, EV_REL, REL_X, ev->dx);
        if (ev->dy)
            ring_push_locked(&evptr, EV_REL, REL_Y, ev->dy);
    }
    if (ev->dz)
        ring_push_locked(&evptr, EV_REL, REL_WHEEL, -ev->dz);
    ring_push_locked(&evptr, EV_SYN, SYN_REPORT, 0);
    spin_unlock(&evptr.lock);
    notify(&evptr);
}

static ssize_t evdev_fops_read(struct vfs_file *file, char *buf, size_t count,
                               bool user)
{
    struct evdev_client *client = file->private_data;
    struct evdev_state *st;
    size_t copied = 0;

    if (client == NULL || client->state == NULL)
        return -ENODEV;
    if (count < sizeof(struct input_event))
        return -EINVAL;

    st = client->state;
    spin_lock(&st->lock);
    while (copied + sizeof(struct input_event) <= count &&
           !client_ring_empty(client)) {
        struct input_event ev = client->ring[client->head];

        client->head = (client->head + 1) % ARRAY_SIZE(client->ring);
        spin_unlock(&st->lock);
        if (user) {
            if (either_copyout(1, (uint64)buf + copied, &ev, sizeof(ev)) < 0)
                return copied ? (int)copied : -EFAULT;
        } else {
            memcpy((char *)buf + copied, &ev, sizeof(ev));
        }
        if (evdev_trace && st->is_pointer) {
            printf("evdev: read %s type=%u code=%u value=%d copied=%d\n",
                   st->name, ev.type, ev.code, ev.value,
                   (int)(copied + sizeof(ev)));
        }
        copied += sizeof(ev);
        spin_lock(&st->lock);
    }
    spin_unlock(&st->lock);

    if (copied == 0)
        return -EAGAIN;
    return (int)copied;
}

static ssize_t evdev_fops_write(struct vfs_file *file, const char *buf,
                                size_t count, bool user)
{
    (void)file;
    (void)user;
    (void)buf;
    (void)count;
    return -EINVAL;
}

static int evdev_fops_poll(struct vfs_file *file, short events)
{
    struct evdev_client *client = file->private_data;
    struct evdev_state *st;
    short revents = 0;

    if (client == NULL || client->state == NULL)
        return POLLERR;
    st = client->state;
    if (events & (POLLIN | POLLRDNORM | POLLRDBAND)) {
        spin_lock(&st->lock);
        if (!client_ring_empty(client))
            revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
        spin_unlock(&st->lock);
    }
    return revents;
}

static int copyout_truncated(uint64 user_ptr, const void *src, size_t src_len,
                             size_t max_len)
{
    size_t n = src_len;

    if (n > max_len)
        n = max_len;
    if (n == 0)
        return 0;
    if (either_copyout(1, user_ptr, (void *)src, n) < 0)
        return -EFAULT;
    return (int)n;
}

static int copyout_bits(uint64 user_ptr, const uint64 *bits,
                        size_t bits_bytes, size_t max_len)
{
    uint8 zeros[32];
    size_t n = bits_bytes;

    if (n > max_len)
        n = max_len;
    if (n > 0 && either_copyout(1, user_ptr, (void *)bits, n) < 0)
        return -EFAULT;
    if (max_len > n) {
        memset(zeros, 0, sizeof(zeros));
        size_t left = max_len - n;
        size_t off = n;
        while (left) {
            size_t chunk = left > sizeof(zeros) ? sizeof(zeros) : left;
            if (either_copyout(1, user_ptr + off, zeros, chunk) < 0)
                return -EFAULT;
            off += chunk;
            left -= chunk;
        }
    }
    return (int)max_len;
}

static int evdev_ioctl_state(struct evdev_state *st, uint64 cmd64, void *arg)
{
    uint32 cmd = (uint32)cmd64;
    uint64 user_ptr = (uint64)arg;
    uint32 nr = IOC_NR(cmd);
    uint32 size = IOC_SIZE(cmd);

    if (evdev_trace && st->is_pointer) {
        printf("evdev: ioctl %s cmd=0x%x nr=0x%x size=%u\n",
               st->name, cmd, nr, size);
    }

    if (IOC_TYPE(cmd) != 'E')
        return -ENOTTY;

    if (cmd == EVIOCGVERSION) {
        int version = EV_VERSION;
        return either_copyout(1, user_ptr, &version, sizeof(version)) < 0 ? -EFAULT : 0;
    }
    if (cmd == EVIOCGID) {
        struct input_id id;

        memset(&id, 0, sizeof(id));
        id.bustype = BUS_VIRTUAL;
        id.vendor = 0x5856;
        id.product = st->is_pointer ? 2 : 1;
        id.version = 1;
        return either_copyout(1, user_ptr, &id, sizeof(id)) < 0 ? -EFAULT : 0;
    }
    if (cmd == EVIOCGREP) {
        uint32 rep[2] = { 250, 33 };
        return either_copyout(1, user_ptr, rep, sizeof(rep)) < 0 ? -EFAULT : 0;
    }
    if (nr == 0x06)
        return copyout_truncated(user_ptr, st->name, strlen(st->name) + 1, size);
    if (nr == 0x07)
        return copyout_truncated(user_ptr, st->phys, strlen(st->phys) + 1, size);
    if (nr == 0x08)
        return copyout_truncated(user_ptr, "", 1, size);
    if (cmd == EVIOCGPROP(size))
        return copyout_bits(user_ptr, st->prop_bits, sizeof(st->prop_bits), size);
    if (cmd == EVIOCGKEY(size))
        return copyout_bits(user_ptr, st->key_down, sizeof(st->key_down), size);
    if (cmd == EVIOCGLED(size) || cmd == EVIOCGSND(size) ||
        cmd == EVIOCGSW(size))
        return copyout_bits(user_ptr, NULL, 0, size);
    if (nr >= 0x20 && nr < 0x40) {
        uint evtype = nr - 0x20;

        if (evtype == 0) {
            uint64 bits[1] = {0};
            set_bit(bits, EV_SYN);
            set_bit(bits, EV_KEY);
            if (st->is_pointer) {
                set_bit(bits, EV_REL);
                set_bit(bits, EV_ABS);
            } else {
                set_bit(bits, EV_MSC);
            }
            return copyout_bits(user_ptr, bits, sizeof(bits), size);
        }
        if (evtype == EV_KEY)
            return copyout_bits(user_ptr, st->key_bits, sizeof(st->key_bits), size);
        if (evtype == EV_REL)
            return copyout_bits(user_ptr, st->rel_bits, sizeof(st->rel_bits), size);
        if (evtype == EV_ABS)
            return copyout_bits(user_ptr, st->abs_bits, sizeof(st->abs_bits), size);
        return copyout_bits(user_ptr, NULL, 0, size);
    }
    if (nr >= 0x40 && nr < 0x80) {
        uint abs = nr - 0x40;
        struct input_absinfo ai;

        if (!st->is_pointer || (abs != ABS_X && abs != ABS_Y))
            return -EINVAL;
        memset(&ai, 0, sizeof(ai));
        ai.value = abs == ABS_X ? st->abs_x : st->abs_y;
        ai.minimum = 0;
        ai.maximum = 65535;
        ai.resolution = 16;
        return either_copyout(1, user_ptr, &ai, sizeof(ai)) < 0 ? -EFAULT : 0;
    }
    if (cmd == EVIOCGRAB || cmd == EVIOCSCLOCKID)
        return 0;

    if (evdev_trace && st->is_pointer) {
        printf("evdev: ioctl %s unhandled cmd=0x%x nr=0x%x size=%u\n",
               st->name, cmd, nr, size);
    }
    return -ENOTTY;
}

static int evdev_fops_ioctl(struct vfs_file *file, uint64 cmd64, void *arg)
{
    struct evdev_client *client = file->private_data;

    if (client == NULL || client->state == NULL)
        return -ENODEV;
    return evdev_ioctl_state(client->state, cmd64, arg);
}

static int evdev_fops_release(struct vfs_inode *inode, struct vfs_file *file)
{
    struct evdev_client *client = file->private_data;
    struct evdev_state *st;
    struct evdev_client **pp;

    (void)inode;
    if (client == NULL)
        return 0;
    st = client->state;
    if (st != NULL) {
        spin_lock(&st->lock);
        for (pp = &st->clients; *pp != NULL; pp = &(*pp)->next) {
            if (*pp == client) {
                *pp = client->next;
                break;
            }
        }
        spin_unlock(&st->lock);
        if (evdev_trace) {
            printf("evdev: release %s client=%p\n", st->name, client);
        }
    }
    file->private_data = NULL;
    kvfree(client);
    return 0;
}

static struct vfs_file_ops evdev_file_ops = {
    .read = evdev_fops_read,
    .write = evdev_fops_write,
    .release = evdev_fops_release,
    .ioctl = evdev_fops_ioctl,
    .poll = evdev_fops_poll,
};

static int evdev_open_file(cdev_t *cdev, struct vfs_file *file)
{
    struct evdev_state *st = container_of(cdev, struct evdev_state, cdev);
    struct evdev_client *client;

    client = kvmalloc(sizeof(*client));
    if (client == NULL)
        return -ENOMEM;
    memset(client, 0, sizeof(*client));
    client->state = st;
    client->file = file;

    spin_lock(&st->lock);
    client->next = st->clients;
    st->clients = client;
    spin_unlock(&st->lock);

    if (evdev_trace) {
        printf("evdev: open %s client=%p\n", st->name, client);
    }
    file->ops = &evdev_file_ops;
    file->private_data = client;
    return 0;
}

static int evdev_open(cdev_t *cdev)
{
    (void)cdev;
    return 0;
}

static int evdev_release(cdev_t *cdev)
{
    (void)cdev;
    return 0;
}

static int evdev_read(cdev_t *cdev, bool user, void *buf, size_t count)
{
    (void)cdev;
    (void)user;
    (void)buf;
    (void)count;
    return -EAGAIN;
}

static int evdev_write(cdev_t *cdev, bool user, const void *buf, size_t count)
{
    (void)cdev;
    (void)user;
    (void)buf;
    (void)count;
    return -EINVAL;
}

static int evdev_poll(cdev_t *cdev, short events)
{
    (void)cdev;
    (void)events;
    return 0;
}

static int evdev_ioctl(cdev_t *cdev, uint64 cmd64, void *arg)
{
    return evdev_ioctl_state(container_of(cdev, struct evdev_state, cdev),
                             cmd64, arg);
}

static void init_common(struct evdev_state *st, const char *name,
                        const char *phys, int is_pointer,
                        int minor, const char *devname)
{
    memset(st, 0, sizeof(*st));
    st->name = name;
    st->phys = phys;
    st->is_pointer = is_pointer;
    spin_init(&st->lock, (char *)name);
    st->cdev.dev.major = EVDEV_MAJOR;
    st->cdev.dev.minor = minor;
    st->cdev.dev.flags = DEV_FLAG_EXPLICIT_MINOR_ZERO;
    st->cdev.dev.devname = devname;
    st->cdev.dev.devmode = S_IFCHR | 0666;
    st->cdev.readable = 1;
    st->cdev.writable = 1;
    st->cdev.ops.read = evdev_read;
    st->cdev.ops.write = evdev_write;
    st->cdev.ops.open = evdev_open;
    st->cdev.ops.release = evdev_release;
    st->cdev.ops.open_file = evdev_open_file;
    st->cdev.ops.ioctl = evdev_ioctl;
    st->cdev.ops.poll = evdev_poll;
}

void evdev_init(void)
{
    char trace_opt[8];

    evdev_trace = cmdline_get_param("evdev_trace", trace_opt,
                                    sizeof(trace_opt)) == 0 &&
                  strcmp(trace_opt, "0") != 0;

    init_common(&evkbd, "xv6 PS/2 Keyboard", "xv6/input0", 0,
                EVDEV_KEYBOARD_MINOR, "input/event0");
    init_common(&evptr, "xv6 Virtio/PS2 Pointer", "xv6/input1", 1,
                EVDEV_POINTER_MINOR, "input/event1");

    for (uint key = 1; key <= 88; key++)
        set_bit(evkbd.key_bits, key);
    set_bit(evkbd.key_bits, KEY_LEFTCTRL);
    set_bit(evkbd.key_bits, KEY_LEFTSHIFT);
    set_bit(evkbd.key_bits, KEY_RIGHTSHIFT);
    set_bit(evkbd.key_bits, KEY_LEFTALT);
    set_bit(evkbd.key_bits, KEY_HOME);
    set_bit(evkbd.key_bits, KEY_UP);
    set_bit(evkbd.key_bits, KEY_PAGEUP);
    set_bit(evkbd.key_bits, KEY_LEFT);
    set_bit(evkbd.key_bits, KEY_RIGHT);
    set_bit(evkbd.key_bits, KEY_END);
    set_bit(evkbd.key_bits, KEY_DOWN);
    set_bit(evkbd.key_bits, KEY_PAGEDOWN);
    set_bit(evkbd.key_bits, KEY_INSERT);
    set_bit(evkbd.key_bits, KEY_DELETE);

    set_bit(evptr.key_bits, BTN_LEFT);
    set_bit(evptr.key_bits, BTN_RIGHT);
    set_bit(evptr.key_bits, BTN_MIDDLE);
    set_bit(evptr.prop_bits, INPUT_PROP_POINTER);
    set_bit(evptr.rel_bits, REL_X);
    set_bit(evptr.rel_bits, REL_Y);
    set_bit(evptr.rel_bits, REL_WHEEL);
    set_bit(evptr.abs_bits, ABS_X);
    set_bit(evptr.abs_bits, ABS_Y);
    evptr.abs_x = 32767;
    evptr.abs_y = 32767;

    int ret = cdev_register(&evkbd.cdev);
    assert(ret == 0, "evdev: keyboard register failed: %d", ret);
    ret = cdev_register(&evptr.cdev);
    assert(ret == 0, "evdev: pointer register failed: %d", ret);
    evdev_ready = 1;
    printf("evdev: registered /dev/input/event0 and /dev/input/event1\n");
}
