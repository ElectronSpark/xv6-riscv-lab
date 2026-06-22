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
#include <dev/ps2kbd.h>
#include <dev/evdev.h>
#include <trap.h>
#include <printf.h>
#include <proc/thread.h>
#include <lock/spinlock.h>
#include <proc/sched.h>
#include <mm/vm.h>
#include <kqueue.h>
#include <vfs/poll.h>
#include <cmdline.h>

#if defined(__x86_64__) || defined(__i386__)

static cdev_t mouse_cdev;

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

static int ps2_controller_present(void)
{
    uint8 status = ps2_inb(PS2_STATUS_PORT);
    uint8 data = ps2_inb(PS2_DATA_PORT);

    /*
     * Hyper-V Gen2 does not provide the legacy i8042.  The inactive I/O ports
     * read back as 0xff, so probing the mouse command stream would only create
     * bogus devices and leave later init code stuck on fake output-buffer bits.
     */
    return !(status == 0xff && data == 0xff);
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
#define VMMOUSE_REQUEST_ABS     0x53424152u  /* "RABS" — request absolute */
#define VMMOUSE_REQUEST_REL     0x4F525245u  /* "ERRO" — request relative */

static int vmmouse_available;  /* non-zero if VMware vmmouse works */
static int vmmouse_nodata;
static volatile uint64 dbg_vmmouse_status_err;
static volatile uint64 dbg_vmmouse_partial;
static volatile uint64 dbg_vmmouse_abs_reqs;
static volatile uint64 dbg_vmmouse_fallbacks;

#define VMMOUSE_NODATA_LIMIT 32

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

    /* Issue READ_ID — upstream QEMU's vmmouse_read_id pushes a 1-word
     * VERSION token onto the device data queue. We MUST drain that
     * single word before reading mouse packets, otherwise every
     * subsequent 4-word read is offset by one (giving the appearance
     * that buttons are stuck and X/Y are in the wrong registers). */
    vmware_cmd(VMCMD_ABSPTR_COMMAND, VMMOUSE_ENABLE, NULL, NULL, NULL, NULL);

    /* Check status — a working vmmouse returns version, not error */
    uint32 status;
    vmware_cmd(VMCMD_ABSPTR_STATUS, 0, &status, NULL, NULL, NULL);
    if ((status & 0xFFFF0000u) == 0xFFFF0000u) {
        /* Error or not available — disable and fall back */
        vmware_cmd(VMCMD_ABSPTR_COMMAND, VMMOUSE_DISABLE, NULL, NULL, NULL, NULL);
        return 0;
    }

    /* Drain the VERSION word that READ_ID pushed onto the queue. */
    if ((status & 0xFFFFu) >= 1) {
        uint32 dummy;
        vmware_cmd(VMCMD_ABSPTR_DATA, 1, &dummy, NULL, NULL, NULL);
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
    if ((status & 0xFFFF0000u) == 0xFFFF0000u) {
        dbg_vmmouse_status_err++;
        vmware_cmd(VMCMD_ABSPTR_COMMAND, VMMOUSE_DISABLE, NULL, NULL, NULL, NULL);
        vmware_cmd(VMCMD_ABSPTR_COMMAND, VMMOUSE_ENABLE, NULL, NULL, NULL, NULL);
        vmware_cmd(VMCMD_ABSPTR_COMMAND, VMMOUSE_REQUEST_ABS, NULL, NULL, NULL, NULL);
        dbg_vmmouse_abs_reqs++;
        return 0;  /* no data */
    }

    if (nwords < 4) {
        /*
         * The vmmouse queue can contain a short control token, commonly the
         * VERSION word produced by ENABLE/READ_ID.  If that token is left
         * queued, every future poll sees "not enough words" and absolute
         * pointer input appears frozen.  Drain short packets and re-request
         * absolute data so the next host motion can produce a full event.
         */
        if (nwords > 0) {
            uint32 dummy;
            dbg_vmmouse_partial++;
            vmware_cmd(VMCMD_ABSPTR_DATA, nwords, &dummy, NULL, NULL, NULL);
            vmware_cmd(VMCMD_ABSPTR_COMMAND, VMMOUSE_REQUEST_ABS, NULL, NULL, NULL, NULL);
            dbg_vmmouse_abs_reqs++;
        }
        return 0;  /* no data */
    }

    /* Correct register layout per upstream QEMU hw/i386/vmmouse.c
     * (verified against v9.0.x source): the device's data queue is
     * filled in vmmouse_mouse_event as [buttons, x, y, dz], and
     * vmmouse_set_data writes data[0..3] back to EAX..EDX. So:
     *   EAX = buttons word (VMware bit5=L, bit4=R, bit3=M; bit16 set
     *         in relative-mode packets)
     *   EBX = x  (0..0xFFFF in absolute mode)
     *   ECX = y  (0..0xFFFF in absolute mode)
     *   EDX = z  (scroll wheel delta) */
    uint32 buttons, x, y, z;
    vmware_cmd(VMCMD_ABSPTR_DATA, 4, &buttons, &x, &y, &z);

    {
        static int dbg = 0;
        if (dbg < 1) {
            dbg++;
            printf("[vmmouse] first sample buttons=0x%x x=0x%x y=0x%x z=0x%x\n",
                   buttons, x, y, z);
        }
    }

    /* VMware buttons word: bit5=left, bit4=right, bit3=middle.
     * Re-pack to xv6 convention (bit0=left, bit1=right, bit2=middle). */
    uint8 b = 0;
    if (buttons & 0x20) b |= 0x01;
    if (buttons & 0x10) b |= 0x02;
    if (buttons & 0x08) b |= 0x04;
    ev->buttons = b;
    ev->flags   = MOUSE_EVENT_F_ABSOLUTE;
    /* QEMU delivers absolute coords in 0..65535. The mouse_event ABI's
     * dx/dy fields are int16 so the upper half of the range reads as
     * negative; userspace (wlcomp) must zero-extend before scaling
     * — see ports/wayland/CMakeLists.txt for the zero-extend patch. */
    ev->dx      = (int16)(x & 0xFFFFu);
    ev->dy      = (int16)(y & 0xFFFFu);
    /* QEMU vmmouse reports wheel deltas with the opposite sign from the
     * PS/2/virtio convention used by /dev/mouse and wlcomp. */
    ev->dz      = (int8)(-(int8)(z & 0xFFu));
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

void mouse_input_push_event(const struct mouse_event *ev)
{
    spin_lock(&mouse_state.lock);
    ring_push((struct mouse_event *)ev);
    spin_unlock(&mouse_state.lock);
    evdev_pointer_event(ev);
    wakeup_on_chan(&mouse_state.ring);
    cdev_knote_notify(&mouse_cdev, EVFILT_READ, 0);
}

/* ── IRQ handler ──────────────────────────────────────────────────── */

/* Lightweight diagnostic counters (no printf in IRQ context).
 * Read out from mouse_open() so we know if IRQs ever fired before
 * the userspace consumer attached. */
static volatile uint64 dbg_mouse_irqs;
static volatile uint64 dbg_mouse_bytes;
static volatile uint64 dbg_mouse_outofsync;
static volatile uint64 dbg_mouse_packets;
static volatile uint64 dbg_mouse_overflow;
static volatile uint64 dbg_mouse_ringpush;
static volatile uint64 dbg_mouse_reads;
static volatile uint64 dbg_mouse_reads_ok;

static int ps2mouse_cmdline_enabled(const char *key)
{
    char value[8];

    if (cmdline_get_param(key, value, sizeof(value)) != 0)
        return 0;
    return value[0] != '0';
}

static int vmmouse_drain_locked(void)
{
    struct mouse_event ev;
    int got = 0;

    while (got < 32 && vmmouse_read(&ev)) {
        ring_push(&ev);
        evdev_pointer_event(&ev);
        dbg_mouse_ringpush++;
        dbg_mouse_packets++;
        got++;
    }
    if (got)
        vmmouse_nodata = 0;

    return got;
}

static void vmmouse_poll_thread(uint64 arg1, uint64 arg2)
{
    (void)arg1;
    (void)arg2;

    for (;;) {
        int got;

        spin_lock(&mouse_state.lock);
        got = vmmouse_drain_locked();
        spin_unlock(&mouse_state.lock);

        if (got) {
            wakeup_on_chan(&mouse_state.ring);
            cdev_knote_notify(&mouse_cdev, EVFILT_READ, 0);
        }

        sleep_ms(8);
    }
}

void ps2_diag_irq_seen(void)
{
    dbg_mouse_irqs++;
}

/*
 * Process a single byte that the i8042 has flagged as auxiliary
 * (mouse) data.  Caller must NOT hold mouse_state.lock and must
 * have already inspected the status byte to confirm bit 5 is set.
 */
void ps2mouse_handle_byte(uint8 byte)
{
    /* vmmouse mode: drain absolute X/Y + buttons from the backdoor and
     * discard the i8042 PS/2 wake-up packets entirely (their dx/dy is
     * just trigger noise — typically [0x08, 0x01, 0x00] — and would
     * otherwise be misinterpreted as relative motion).
     *
     * Pure PS/2 mode: assemble 3- or 4-byte packets and push events. */
    dbg_mouse_bytes++;
    spin_lock(&mouse_state.lock);

    if (vmmouse_available) {
        int got = vmmouse_drain_locked();
        if (got) {
            /* Discard the trigger byte; do NOT feed PS/2 packet assembly. */
            mouse_state.packet_idx = 0;
            spin_unlock(&mouse_state.lock);
            wakeup_on_chan(&mouse_state.ring);
            cdev_knote_notify(&mouse_cdev, EVFILT_READ, 0);
            return;
        }

        /*
         * QEMU can leave vmport/vmmouse advertised while the absolute-data
         * queue stops yielding packets.  If we keep discarding i8042 bytes in
         * that state, the pointer appears frozen forever.  After a short run
         * of empty vmmouse wakeups, fall back to the normal PS/2 packet path.
         */
        if (++vmmouse_nodata >= VMMOUSE_NODATA_LIMIT) {
            vmmouse_available = 0;
            vmmouse_nodata = 0;
            dbg_vmmouse_fallbacks++;
            printf("PS2 mouse: vmmouse no data, falling back to PS/2 relative mode\n");
        } else {
            mouse_state.packet_idx = 0;
            spin_unlock(&mouse_state.lock);
            return;
        }
    }

    /* Pure PS/2 path */
    if (mouse_state.packet_idx == 0 && !(byte & 0x08)) {
        dbg_mouse_outofsync++;
        spin_unlock(&mouse_state.lock);
        return;
    }

    mouse_state.packet[mouse_state.packet_idx++] = byte;

    if (mouse_state.packet_idx != mouse_state.packet_len) {
        spin_unlock(&mouse_state.lock);
        return;
    }

    dbg_mouse_packets++;
    uint8 flags = mouse_state.packet[0];
    int   dx    = (int)mouse_state.packet[1];
    int   dy    = (int)mouse_state.packet[2];
    if (flags & 0x10) dx |= 0xFFFFFF00;
    if (flags & 0x20) dy |= 0xFFFFFF00;

    if (flags & 0xC0) {
        dbg_mouse_overflow++;
        mouse_state.packet_idx = 0;
        spin_unlock(&mouse_state.lock);
        return;
    }

    struct mouse_event ev;
    ev.dx      = (int16)dx;
    ev.dy      = (int16)(-dy);
    ev.buttons = flags & 0x07;
    ev.flags   = 0;
    ev.dz      = intellimouse_enabled ? (int8)mouse_state.packet[3] : 0;
    ev.pad[0]  = 0;
    ring_push(&ev);
    dbg_mouse_ringpush++;

    mouse_state.packet_idx = 0;
    spin_unlock(&mouse_state.lock);
    evdev_pointer_event(&ev);
    wakeup_on_chan(&mouse_state.ring);
    cdev_knote_notify(&mouse_cdev, EVFILT_READ, 0);
}

/*
 * Drain the i8042 output buffer, dispatching each byte to the
 * appropriate device based on status bit 5.  See the matching
 * helper in ps2kbd.c for the rationale (single shared OB).
 */
static void ps2_drain_obf(void)
{
    for (int i = 0; i < 32; i++) {
        uint8 status = ps2_inb(PS2_STATUS_PORT);
        if (!(status & 0x01))
            return;
        uint8 byte = ps2_inb(PS2_DATA_PORT);
        if (status & 0x20)
            ps2mouse_handle_byte(byte);
        else
            ps2kbd_handle_byte(byte);
    }
}

static void ps2mouse_poll_thread(uint64 arg1, uint64 arg2)
{
    (void)arg1;
    (void)arg2;

    for (;;) {
        ps2_drain_obf();
        sleep_ms(2);
    }
}

static void mouse_irq_handler(int irq, void *data, device_t *dev)
{
    (void)irq; (void)data; (void)dev;
    /* Diagnostic counters; periodically dumped from /proc-style code
     * if needed.  Kept lightweight — no printf in IRQ context. */
    extern void ps2_diag_irq_seen(void);
    ps2_diag_irq_seen();
    ps2_drain_obf();
}

/* DEBUG kthread removed; counters retained for /proc-style introspection. */
extern void sleep_ms(uint64 ms);

/* ── Character device operations ──────────────────────────────────── */

static int mouse_open(cdev_t *cdev)  {
    (void)cdev;
    return 0;
}
static int mouse_release(cdev_t *cdev) { return 0; }

static int mouse_read(cdev_t *cdev, bool user, void *buf, size_t count)
{
    (void)cdev;
    dbg_mouse_reads++;

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
    dbg_mouse_reads_ok++;

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
    struct mouse_event ev;
    int trace;

    (void)cdev;
    if (count != sizeof(ev))
        return -EINVAL;

    if (user) {
        if (either_copyin((char *)&ev, 1, (uint64)buf, sizeof(ev)) < 0)
            return -EFAULT;
    } else {
        memcpy(&ev, buf, sizeof(ev));
    }

    trace = ps2mouse_cmdline_enabled("ps2mouse_write_trace");
    if (trace)
        printf("PS2 mouse: write begin flags=0x%x x=%u y=%u buttons=0x%x\n",
               ev.flags, (uint16)ev.dx, (uint16)ev.dy, ev.buttons);
    mouse_input_push_event(&ev);
    if (trace)
        printf("PS2 mouse: write end flags=0x%x x=%u y=%u buttons=0x%x\n",
               ev.flags, (uint16)ev.dx, (uint16)ev.dy, ev.buttons);
    return sizeof(ev);
}

static int mouse_poll(cdev_t *cdev, short events)
{
    (void)cdev;
    short revents = 0;
    if (events & (POLLIN | POLLRDNORM | POLLRDBAND)) {
        spin_lock(&mouse_state.lock);
        if (!ring_empty())
            revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
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
    .writable = 1,
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

    /* Register /dev/mouse before touching hardware.  Hyper-V Gen2 does not
     * expose i8042, but synthetic input drivers still need the shared mouse
     * event queue as their userspace ABI. */
    int ret = cdev_register(&mouse_cdev);
    assert(ret == 0, "ps2mouse_init: cdev_register failed: %d", ret);

    if (!ps2_controller_present()) {
        printf("PS2 mouse: i8042 controller not present; /dev/mouse is synthetic-only\n");
        return;
    }

    /* Enable the auxiliary (mouse) port on the PS/2 controller */
    ps2_send_cmd(PS2_CMD_ENABLE_PORT2);

    /* Read current config byte */
    ps2_send_cmd(PS2_CMD_READ_CONFIG);
    ps2_wait_output();
    uint8 config = ps2_inb(PS2_DATA_PORT);
    printf("PS2 mouse: config byte before = 0x%x\n", config);

    /* Enable IRQ12 (bit 1) and make sure port 2 clock is enabled (bit 5 = 0) */
    config |= 0x02;    /* enable auxiliary interrupt */
    config &= ~0x20;   /* enable auxiliary clock */

    ps2_send_cmd(PS2_CMD_WRITE_CONFIG);
    ps2_wait_input();
    ps2_outb(PS2_DATA_PORT, config);

    /* Read it back to confirm */
    ps2_send_cmd(PS2_CMD_READ_CONFIG);
    ps2_wait_output();
    uint8 cfg2 = ps2_inb(PS2_DATA_PORT);
    printf("PS2 mouse: config byte after  = 0x%x\n", cfg2);

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

    vmmouse_available = vmmouse_probe();
    if (vmmouse_available)
        printf("PS2 mouse: VMware absolute pointer enabled\n");
    else
        printf("PS2 mouse: using PS/2 relative mode\n");

    /* Register IRQ handler */
    static struct irq_desc mouse_irq_desc = {
        .handler = mouse_irq_handler,
        .data = NULL,
        .dev = &mouse_cdev.dev,
    };

    ret = register_irq_handler(PLIC_IRQ(MOUSE_IRQ), &mouse_irq_desc);
    if (ret != 0) {
        printf("PS2 mouse: failed to register IRQ handler: %d\n", ret);
        return;
    }

    /* Enable IRQ 12 in the I/O APIC */
    extern void plic_enable_irq(int irq);
    plic_enable_irq(MOUSE_IRQ);

    struct thread *poller = kthread_create("ps2mouse_poll",
                                           ps2mouse_poll_thread, 0, 0, 0);
    if (IS_ERR_OR_NULL(poller))
        printf("PS2 mouse: failed to start poll thread\n");

    if (vmmouse_available) {
        struct thread *vmpoller = kthread_create("vmmouse_poll",
                                                 vmmouse_poll_thread, 0, 0, 0);
        if (IS_ERR_OR_NULL(vmpoller))
            printf("PS2 mouse: failed to start vmmouse poll thread\n");
    }

    printf("PS2 mouse: /dev/mouse registered (IRQ %d)\n", MOUSE_IRQ);
}

#else /* !x86_64 */

void ps2mouse_init(void) {}

#endif /* __x86_64__ */
