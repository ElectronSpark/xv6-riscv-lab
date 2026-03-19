/*
 * ps2mouse.c — PS/2 mouse driver for x86_64 QEMU.
 *
 * Handles IRQ 12, reads 3-byte PS/2 mouse packets, and exposes
 * them to userspace via /dev/mouse as struct mouse_event reads.
 */

#include <types.h>
#include <param.h>
#include <riscv.h>
#include <defs.h>
#include <errno.h>
#include <string.h>
#include <dev/cdev.h>
#include <dev/ps2mouse.h>
#include <trap.h>
#include <printf.h>
#include <proc/thread.h>
#include <lock/spinlock.h>
#include <proc/sched.h>
#include <mm/vm.h>

#if defined(__x86_64__) || defined(__i386__)

/* ── I/O port helpers ────────────────────────────────────────────── */

static inline void ps2_outb(uint16 port, uint8 val)
{
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8 ps2_inb(uint16 port)
{
    uint8 val;
    asm volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* ── PS/2 controller helpers ─────────────────────────────────────── */

static void ps2_wait_input(void)
{
    for (int i = 0; i < 100000; i++)
        if (!(ps2_inb(PS2_STATUS_PORT) & 0x02))
            return;
}

static void ps2_wait_output(void)
{
    for (int i = 0; i < 100000; i++)
        if (ps2_inb(PS2_STATUS_PORT) & 0x01)
            return;
}

static void ps2_send_cmd(uint8 cmd)
{
    ps2_wait_input();
    ps2_outb(PS2_CMD_PORT, cmd);
}

static void ps2_send_mouse_cmd(uint8 cmd)
{
    ps2_send_cmd(PS2_CMD_WRITE_PORT2);  /* route next byte to mouse */
    ps2_wait_input();
    ps2_outb(PS2_DATA_PORT, cmd);
    ps2_wait_output();                  /* wait for ACK (0xFA) */
    (void)ps2_inb(PS2_DATA_PORT);      /* discard ACK */
}

/* ── Event ring buffer ───────────────────────────────────────────── */

#define MOUSE_RING_SIZE 128

static struct {
    struct mouse_event ring[MOUSE_RING_SIZE];
    int head;
    int tail;
    spinlock_t lock;

    /* PS/2 3-byte packet assembly */
    uint8 packet[3];
    int   packet_idx;
} mouse_state;

static int ring_empty(void)
{
    return mouse_state.head == mouse_state.tail;
}

static void ring_push(struct mouse_event *ev)
{
    int next = (mouse_state.tail + 1) % MOUSE_RING_SIZE;
    if (next == mouse_state.head)
        mouse_state.head = (mouse_state.head + 1) % MOUSE_RING_SIZE; /* drop oldest */
    mouse_state.ring[mouse_state.tail] = *ev;
    mouse_state.tail = next;
}

static int ring_pop(struct mouse_event *ev)
{
    if (ring_empty())
        return -1;
    *ev = mouse_state.ring[mouse_state.head];
    mouse_state.head = (mouse_state.head + 1) % MOUSE_RING_SIZE;
    return 0;
}

/* ── IRQ handler ──────────────────────────────────────────────────── */

static void mouse_irq_handler(int irq, void *data, device_t *dev)
{
    (void)irq; (void)data; (void)dev;

    uint8 status = ps2_inb(PS2_STATUS_PORT);
    if (!(status & 0x01))
        return;
    /* Bit 5 of status: data is from mouse (auxiliary port) */
    if (!(status & 0x20))
        return;

    uint8 byte = ps2_inb(PS2_DATA_PORT);

    spin_lock(&mouse_state.lock);

    /* First byte must have bit 3 set (always-1 bit in PS/2 packet) */
    if (mouse_state.packet_idx == 0 && !(byte & 0x08)) {
        spin_unlock(&mouse_state.lock);
        return;  /* out of sync, skip */
    }

    mouse_state.packet[mouse_state.packet_idx++] = byte;

    if (mouse_state.packet_idx == 3) {
        /* Complete packet — decode */
        uint8 flags = mouse_state.packet[0];
        int dx = (int)mouse_state.packet[1];
        int dy = (int)mouse_state.packet[2];

        /* Sign-extend using flags bits 4 and 5 */
        if (flags & 0x10) dx |= 0xFFFFFF00;
        if (flags & 0x20) dy |= 0xFFFFFF00;

        /* Discard if overflow bits are set */
        if (!(flags & 0xC0)) {
            struct mouse_event ev;
            ev.dx = (int16)dx;
            ev.dy = (int16)(-dy);  /* PS/2 Y is inverted */
            ev.buttons = flags & 0x07;
            ev.pad[0] = ev.pad[1] = ev.pad[2] = 0;
            ring_push(&ev);
            wakeup_on_chan(&mouse_state.ring);
        }

        mouse_state.packet_idx = 0;
    }

    spin_unlock(&mouse_state.lock);
}

/* ── Character device operations ──────────────────────────────────── */

static int mouse_open(cdev_t *cdev)  { return 0; }
static int mouse_release(cdev_t *cdev) { return 0; }

static int mouse_read(cdev_t *cdev, bool user, void *buf, size_t count)
{
    (void)cdev;

    if (count < sizeof(struct mouse_event))
        return -EINVAL;

    struct mouse_event ev;

    spin_lock(&mouse_state.lock);
    if (ring_empty()) {
        spin_unlock(&mouse_state.lock);
        return -EAGAIN;  /* non-blocking: no events available */
    }
    ring_pop(&ev);
    spin_unlock(&mouse_state.lock);

    size_t copylen = sizeof(struct mouse_event);
    if (count < copylen)
        copylen = count;

    if (user) {
        if (either_copyout(1, (uint64)buf, (char *)&ev, copylen) < 0)
            return -EFAULT;
    } else {
        memcpy(buf, &ev, copylen);
    }

    return copylen;
}

static int mouse_write(cdev_t *cdev, bool user, const void *buf, size_t count)
{
    (void)cdev; (void)user; (void)buf; (void)count;
    return -ENOSYS;
}

static int mouse_poll(cdev_t *cdev, short events)
{
    (void)cdev;
    short revents = 0;
    if (events & 0x01) {  /* POLLIN */
        spin_lock(&mouse_state.lock);
        if (!ring_empty())
            revents |= 0x01;
        spin_unlock(&mouse_state.lock);
    }
    return revents;
}

static cdev_t mouse_cdev = {
    .dev = {
        .major = MOUSE_MAJOR,
        .minor = MOUSE_MINOR,
        .devname = "mouse",
        .devmode = S_IFCHR | 0666,
    },
    .readable = 1,
    .writable = 0,
    .ops = {
        .read    = mouse_read,
        .write   = mouse_write,
        .open    = mouse_open,
        .release = mouse_release,
        .ioctl   = NULL,
        .poll    = mouse_poll,
    },
};

/* ── Initialization ──────────────────────────────────────────────── */

void ps2mouse_init(void)
{
    spin_init(&mouse_state.lock, "mouse");
    mouse_state.head = mouse_state.tail = 0;
    mouse_state.packet_idx = 0;

    /* Enable the auxiliary (mouse) port on the PS/2 controller */
    ps2_send_cmd(PS2_CMD_ENABLE_PORT2);

    /* Read current config byte */
    ps2_send_cmd(PS2_CMD_READ_CONFIG);
    ps2_wait_output();
    uint8 config = ps2_inb(PS2_DATA_PORT);

    /* Enable IRQ12 (bit 1) and make sure port 2 clock is enabled (bit 5 = 0) */
    config |= 0x02;    /* enable auxiliary interrupt */
    config &= ~0x20;   /* enable auxiliary clock */

    ps2_send_cmd(PS2_CMD_WRITE_CONFIG);
    ps2_wait_input();
    ps2_outb(PS2_DATA_PORT, config);

    /* Reset mouse */
    ps2_send_mouse_cmd(MOUSE_CMD_RESET);
    /* Wait for self-test result (0xAA) and device ID (0x00) */
    ps2_wait_output();
    (void)ps2_inb(PS2_DATA_PORT);  /* 0xAA */
    ps2_wait_output();
    (void)ps2_inb(PS2_DATA_PORT);  /* 0x00 */

    /* Set defaults and enable data reporting */
    ps2_send_mouse_cmd(MOUSE_CMD_SET_DEFAULTS);
    ps2_send_mouse_cmd(MOUSE_CMD_ENABLE);

    /* Register IRQ handler */
    static struct irq_desc mouse_irq_desc = {
        .handler = mouse_irq_handler,
        .data = NULL,
        .dev = &mouse_cdev.dev,
    };

    int ret = register_irq_handler(PLIC_IRQ(MOUSE_IRQ), &mouse_irq_desc);
    if (ret != 0) {
        printf("PS2 mouse: failed to register IRQ handler: %d\n", ret);
        return;
    }

    /* Enable IRQ 12 in the I/O APIC */
    extern void plic_enable_irq(int irq);
    plic_enable_irq(MOUSE_IRQ);

    /* Register character device */
    ret = cdev_register(&mouse_cdev);
    assert(ret == 0, "ps2mouse_init: cdev_register failed: %d", ret);

    printf("PS2 mouse: /dev/mouse registered (IRQ %d)\n", MOUSE_IRQ);
}

#else /* !x86_64 */

void ps2mouse_init(void) {}

#endif /* __x86_64__ */
