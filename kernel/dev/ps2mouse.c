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

/* ── VMware vmmouse (absolute pointer) ───────────────────────────── */

#define VMWARE_MAGIC            0x564D5868u  /* "VMXh" */
#define VMWARE_PORT             0x5658u

#define VMCMD_GETVERSION        10
#define VMCMD_ABSPTR_DATA       39
#define VMCMD_ABSPTR_STATUS     40
#define VMCMD_ABSPTR_COMMAND    41

#define VMMOUSE_ENABLE          0x45414552u  /* "REAE" */
#define VMMOUSE_DISABLE         0x000000F5u
#define VMMOUSE_REQUEST_ABS     0x53414C41u  /* "ALAS" */

static int vmmouse_available;  /* non-zero if VMware vmmouse works */
static int vmmouse_nodata;    /* consecutive IRQs with no vmmouse data */
#define VMMOUSE_NODATA_LIMIT 64  /* disable after this many failures */

static inline void vmware_cmd(uint32 cmd, uint32 param,
                              uint32 *a, uint32 *b, uint32 *c, uint32 *d)
{
    uint32 ra, rb, rc, rd;
    asm volatile("inl %%dx, %%eax"
        : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd)
        : "a"(VMWARE_MAGIC), "b"(param), "c"(cmd), "d"(VMWARE_PORT)
        : "memory");
    if (a) *a = ra;
    if (b) *b = rb;
    if (c) *c = rc;
    if (d) *d = rd;
}

static int vmmouse_probe(void)
{
    /* Check VMware backdoor: GETVERSION should return magic in EBX */
    uint32 ver, magic;
    vmware_cmd(VMCMD_GETVERSION, 0, &ver, &magic, NULL, NULL);
    if (magic != VMWARE_MAGIC)
        return 0;

    /* Enable absolute pointer */
    vmware_cmd(VMCMD_ABSPTR_COMMAND, VMMOUSE_ENABLE, NULL, NULL, NULL, NULL);

    /* Check status — a working vmmouse returns version, not error */
    uint32 status;
    vmware_cmd(VMCMD_ABSPTR_STATUS, 0, &status, NULL, NULL, NULL);
    if ((status & 0xFFFF0000u) == 0xFFFF0000u) {
        /* Error or not available — disable and fall back */
        vmware_cmd(VMCMD_ABSPTR_COMMAND, VMMOUSE_DISABLE, NULL, NULL, NULL, NULL);
        return 0;
    }

    /* Request absolute mode */
    vmware_cmd(VMCMD_ABSPTR_COMMAND, VMMOUSE_REQUEST_ABS, NULL, NULL, NULL, NULL);

    /* Verify we're still OK */
    vmware_cmd(VMCMD_ABSPTR_STATUS, 0, &status, NULL, NULL, NULL);
    if ((status & 0xFFFF0000u) == 0xFFFF0000u) {
        vmware_cmd(VMCMD_ABSPTR_COMMAND, VMMOUSE_DISABLE, NULL, NULL, NULL, NULL);
        return 0;
    }

    return 1;
}

/* Read one absolute-mode event. Returns 1 if data was available, 0 otherwise. */
static int vmmouse_read(struct mouse_event *ev)
{
    uint32 status;
    vmware_cmd(VMCMD_ABSPTR_STATUS, 0, &status, NULL, NULL, NULL);

    uint32 nwords = status & 0xFFFFu;
    if (nwords < 4)
        return 0;  /* no data */

    uint32 s, x, y, z;
    vmware_cmd(VMCMD_ABSPTR_DATA, 4, &s, &x, &y, &z);

    ev->buttons = (uint8)(s & 0x07);
    ev->flags   = MOUSE_EVENT_F_ABSOLUTE;
    ev->dx      = (int16)(x & 0xFFFF);
    ev->dy      = (int16)(y & 0xFFFF);
    ev->dz      = 0;
    ev->pad[0]  = 0;
    return 1;
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

/* ── IntelliMouse (scroll wheel) state ────────────────────────────── */

static int intellimouse_enabled;  /* 1 = 4-byte packets with Z axis */

/* ── Event ring buffer ───────────────────────────────────────────── */

#define MOUSE_RING_SIZE 128

static struct {
    struct mouse_event ring[MOUSE_RING_SIZE];
    int head;
    int tail;
    spinlock_t lock;

    /* PS/2 packet assembly (3 or 4 bytes depending on IntelliMouse) */
    uint8 packet[4];
    int   packet_idx;
    int   packet_len;  /* 3 (standard) or 4 (IntelliMouse) */
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

    /* Try VMware vmmouse for absolute coordinates first */
    if (vmmouse_available) {
        struct mouse_event ev;
        int got = 0;
        while (vmmouse_read(&ev)) {
            ring_push(&ev);
            got++;
        }
        if (got) {
            /* vmmouse provided data — discard PS/2 trigger byte */
            vmmouse_nodata = 0;
            mouse_state.packet_idx = 0;
            spin_unlock(&mouse_state.lock);
            wakeup_on_chan(&mouse_state.ring);
            return;
        }
        /* No vmmouse data — maybe QEMU didn't actually create the
         * vmmouse device.  After enough failures, give up and fall
         * back to PS/2 permanently. */
        if (++vmmouse_nodata >= VMMOUSE_NODATA_LIMIT) {
            vmmouse_available = 0;
            printf("PS2 mouse: vmmouse not responding, falling back to PS/2\n");
        }
        /* Fall through to normal PS/2 processing for this byte */
    }

    /* Standard PS/2 relative mouse */

    /* First byte must have bit 3 set (always-1 bit in PS/2 packet) */
    if (mouse_state.packet_idx == 0 && !(byte & 0x08)) {
        spin_unlock(&mouse_state.lock);
        return;  /* out of sync, skip */
    }

    mouse_state.packet[mouse_state.packet_idx++] = byte;

    if (mouse_state.packet_idx == mouse_state.packet_len) {
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
            ev.flags = 0;
            ev.dz = 0;
            ev.pad[0] = 0;

            /* IntelliMouse 4th byte: scroll wheel delta */
            if (intellimouse_enabled)
                ev.dz = (int8)mouse_state.packet[3];

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
    mouse_state.packet_len = 3;  /* default: standard 3-byte PS/2 */
    intellimouse_enabled = 0;

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

    /* ── IntelliMouse detection ──────────────────────────────────── *
     * Send the magic sample-rate sequence 200, 100, 80.             *
     * If the mouse supports scroll wheel it switches to mouse ID 3  *
     * and sends 4-byte packets with a Z-axis byte.                  */
    ps2_send_mouse_cmd(MOUSE_CMD_SET_SAMPLE);
    ps2_send_mouse_cmd(200);
    ps2_send_mouse_cmd(MOUSE_CMD_SET_SAMPLE);
    ps2_send_mouse_cmd(100);
    ps2_send_mouse_cmd(MOUSE_CMD_SET_SAMPLE);
    ps2_send_mouse_cmd(80);

    /* Read mouse ID: send GET_DEVICE_ID (0xF2) */
    ps2_send_cmd(PS2_CMD_WRITE_PORT2);
    ps2_wait_input();
    ps2_outb(PS2_DATA_PORT, 0xF2);
    ps2_wait_output();
    (void)ps2_inb(PS2_DATA_PORT);  /* ACK */
    ps2_wait_output();
    uint8 mouse_id = ps2_inb(PS2_DATA_PORT);

    if (mouse_id == 3) {
        intellimouse_enabled = 1;
        mouse_state.packet_len = 4;
        printf("PS2 mouse: IntelliMouse detected (scroll wheel enabled)\n");
    }

    ps2_send_mouse_cmd(MOUSE_CMD_ENABLE);

    /* VMware vmmouse disabled — QEMU's vmport backdoor port responds
     * to probes (GETVERSION works, ABSPTR_COMMAND succeeds) but the
     * returned coordinates are always (0,0) unless a real '-device vmmouse'
     * is linked to the i8042 controller.  Since that requires QEMU machine
     * glue that we can't set up on all versions, stick with PS/2 relative.
     * The USB-tablet device in QEMU eliminates host grab without needing
     * abs coords inside the guest. */
    vmmouse_available = 0;
    (void)vmmouse_probe;  /* suppress unused warning */

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
