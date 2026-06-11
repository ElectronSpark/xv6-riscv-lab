/*
 * ps2kbd.c — PS/2 keyboard driver for x86_64 QEMU.
 *
 * Handles IRQ 1, translates scan code set 1 to ASCII,
 * and exposes key events to userspace via /dev/kbd.
 */

#include <types.h>
#include <param.h>
#include <riscv.h>
#include <defs.h>
#include <errno.h>
#include <string.h>
#include <dev/cdev.h>
#include <dev/ps2kbd.h>
#include <dev/ps2mouse.h>
#include <trap.h>
#include <printf.h>
#include <proc/thread.h>
#include <lock/spinlock.h>
#include <proc/sched.h>
#include <mm/vm.h>
#include <vfs/poll.h>

#if defined(__x86_64__) || defined(__i386__)

/* ── I/O port helpers ────────────────────────────────────────────── */

static inline void kbd_outb(uint16 port, uint8 val)
{
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8 kbd_inb(uint16 port)
{
    uint8 val;
    asm volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static int ps2_controller_present(void)
{
    uint8 status = kbd_inb(PS2_STATUS_PORT);
    uint8 data = kbd_inb(PS2_DATA_PORT);

    /*
     * Hyper-V Gen2 does not expose an i8042 controller.  Reads from the
     * legacy PS/2 ports return 0xff, which would otherwise make the flush loop
     * below spin forever because the output-buffer bit is permanently set.
     */
    return !(status == 0xff && data == 0xff);
}

/* ── Scan code set 1 → ASCII tables ─────────────────────────────── */

/* Unshifted ASCII for scan codes 0x00–0x58 */
static const char scancode_to_ascii[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6',  /* 00-07 */
    '7', '8', '9', '0', '-', '=', '\b', '\t', /* 08-0F */
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',  /* 10-17 */
    'o', 'p', '[', ']', '\n', 0,   'a', 's',  /* 18-1F (1D=LCtrl) */
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',  /* 20-27 */
    '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v', /* 28-2F (2A=LShift) */
    'b', 'n', 'm', ',', '.', '/', 0,   '*',   /* 30-37 (36=RShift) */
    0,   ' ', 0,   0,   0,   0,   0,   0,     /* 38-3F (38=LAlt, 3A=Caps) */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 40-47 (F1-F8, then numpad) */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 48-4F */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 50-57 */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 58-5F */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 60-67 */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 68-6F */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 70-77 */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 78-7F */
};

/* Shifted ASCII */
static const char scancode_to_ascii_shift[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^',   /* 00-07 */
    '&', '*', '(', ')', '_', '+', '\b', '\t',  /* 08-0F */
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',  /* 10-17 */
    'O', 'P', '{', '}', '\n', 0,   'A', 'S',  /* 18-1F */
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',  /* 20-27 */
    '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',  /* 28-2F */
    'B', 'N', 'M', '<', '>', '?', 0,   '*',   /* 30-37 */
    0,   ' ', 0,   0,   0,   0,   0,   0,     /* 38-3F */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 40-47 */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 48-4F */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 50-57 */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 58-5F */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 60-67 */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 68-6F */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 70-77 */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 78-7F */
};

/* ── Event ring buffer ───────────────────────────────────────────── */

#define KBD_RING_SIZE 256

static struct {
    struct kbd_event ring[KBD_RING_SIZE];
    int head;
    int tail;
    spinlock_t lock;

    /* Modifier state tracked across interrupts */
    uint8  shift_l;
    uint8  shift_r;
    uint8  ctrl;
    uint8  alt;
    int    extended;   /* E0 prefix seen */

    /* Key-down bitmap for typematic suppression (128 codes = 16 bytes) */
    uint8  key_down[16];
} kbd_state;

static int ring_empty(void)
{
    return kbd_state.head == kbd_state.tail;
}

static void ring_push(struct kbd_event *ev)
{
    int next = (kbd_state.tail + 1) % KBD_RING_SIZE;
    if (next == kbd_state.head)
        kbd_state.head = (kbd_state.head + 1) % KBD_RING_SIZE;
    kbd_state.ring[kbd_state.tail] = *ev;
    kbd_state.tail = next;
}

static int ring_pop(struct kbd_event *ev)
{
    if (ring_empty())
        return -1;
    *ev = kbd_state.ring[kbd_state.head];
    kbd_state.head = (kbd_state.head + 1) % KBD_RING_SIZE;
    return 0;
}

/* ── IRQ handler ──────────────────────────────────────────────────── */

/* Process one scancode byte (called from kbd or mouse IRQ).
 * Caller must NOT hold kbd_state.lock. */
void ps2kbd_handle_byte(uint8 scancode)
{
    spin_lock(&kbd_state.lock);

    /* Handle E0 extended prefix */
    if (scancode == 0xE0) {
        kbd_state.extended = 1;
        spin_unlock(&kbd_state.lock);
        return;
    }

    int is_release = (scancode & 0x80) != 0;
    uint8 code = scancode & 0x7F;

    /* Suppress PS/2 typematic (auto-repeat) make codes.
     * Software key repeat is handled by the Wayland compositor
     * via wl_keyboard.repeat_info; hardware repeats only cause
     * duplicate characters in framebuffer-mode apps. */
    if (!is_release) {
        if (kbd_state.key_down[code / 8] & (1u << (code % 8))) {
            kbd_state.extended = 0;
            spin_unlock(&kbd_state.lock);
            return;   /* duplicate make code — typematic repeat, drop */
        }
        kbd_state.key_down[code / 8] |= (1u << (code % 8));
    } else {
        kbd_state.key_down[code / 8] &= ~(1u << (code % 8));
    }

    /* Track modifier state */
    if (code == 0x2A) kbd_state.shift_l = !is_release;
    if (code == 0x36) kbd_state.shift_r = !is_release;
    if (code == 0x1D) kbd_state.ctrl    = !is_release;
    if (code == 0x38) kbd_state.alt     = !is_release;

    /* Build event */
    struct kbd_event ev;
    ev.scancode = code;
    ev.pressed  = !is_release;
    ev.modifiers = 0;
    if (kbd_state.shift_l || kbd_state.shift_r) ev.modifiers |= KBD_MOD_SHIFT;
    if (kbd_state.ctrl)  ev.modifiers |= KBD_MOD_CTRL;
    if (kbd_state.alt)   ev.modifiers |= KBD_MOD_ALT;

    /* E0-prefixed keys: map to special key codes */
    if (kbd_state.extended) {
        ev.keycode = 0;
        switch (code) {
        case 0x48: ev.keycode = KBD_KEY_UP;     break;
        case 0x50: ev.keycode = KBD_KEY_DOWN;   break;
        case 0x4B: ev.keycode = KBD_KEY_LEFT;   break;
        case 0x4D: ev.keycode = KBD_KEY_RIGHT;  break;
        case 0x47: ev.keycode = KBD_KEY_HOME;   break;
        case 0x4F: ev.keycode = KBD_KEY_END;    break;
        case 0x49: ev.keycode = KBD_KEY_PGUP;   break;
        case 0x51: ev.keycode = KBD_KEY_PGDN;   break;
        case 0x52: ev.keycode = KBD_KEY_INSERT; break;
        case 0x53: ev.keycode = KBD_KEY_DELETE; break;
        }
    } else {
        /* Translate scancode to ASCII */
        if (code < 128) {
            if (ev.modifiers & KBD_MOD_SHIFT)
                ev.keycode = scancode_to_ascii_shift[code];
            else
                ev.keycode = scancode_to_ascii[code];

            /* Ctrl+letter → control character (^A=1 .. ^Z=26) */
            if ((ev.modifiers & KBD_MOD_CTRL) && ev.keycode >= 'a' && ev.keycode <= 'z')
                ev.keycode = ev.keycode - 'a' + 1;
            if ((ev.modifiers & KBD_MOD_CTRL) && ev.keycode >= 'A' && ev.keycode <= 'Z')
                ev.keycode = ev.keycode - 'A' + 1;
        } else {
            ev.keycode = 0;
        }
    }

    ring_push(&ev);
    wakeup_on_chan(&kbd_state.ring);

    kbd_state.extended = 0;
    spin_unlock(&kbd_state.lock);
}

/*
 * Drain the i8042 output buffer, dispatching each byte to the
 * appropriate device based on status bit 5 (1 = mouse, 0 = kbd).
 * The i8042 has one shared output buffer and will not raise any
 * further IRQ (kbd or mouse) until that buffer is empty, so a
 * single byte left undrained wedges *both* devices.
 */
static void ps2_drain_obf(void)
{
    /* Cap the drain count so a stuck/runaway controller cannot
     * monopolise the CPU.  Real PS/2 traffic is at most one
     * 4-byte mouse packet plus a few scancodes per IRQ. */
    for (int i = 0; i < 32; i++) {
        uint8 status = kbd_inb(PS2_STATUS_PORT);
        if (!(status & 0x01))
            return;
        uint8 byte = kbd_inb(PS2_DATA_PORT);
        if (status & 0x20)
            ps2mouse_handle_byte(byte);
        else
            ps2kbd_handle_byte(byte);
    }
}

static void kbd_irq_handler(int irq, void *data, device_t *dev)
{
    (void)irq; (void)data; (void)dev;
    ps2_drain_obf();
}

/* ── Character device operations ──────────────────────────────────── */

static int kbd_open(cdev_t *cdev)  { (void)cdev; return 0; }
static int kbd_release(cdev_t *cdev) { (void)cdev; return 0; }

static int kbd_read(cdev_t *cdev, bool user, void *buf, size_t count)
{
    (void)cdev;

    if (count < sizeof(struct kbd_event))
        return -EINVAL;

    struct kbd_event ev;

    spin_lock(&kbd_state.lock);
    if (ring_empty()) {
        spin_unlock(&kbd_state.lock);
        return -EAGAIN;  /* non-blocking */
    }
    ring_pop(&ev);
    spin_unlock(&kbd_state.lock);

    size_t copylen = sizeof(struct kbd_event);
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

static int kbd_write(cdev_t *cdev, bool user, const void *buf, size_t count)
{
    size_t off = 0;

    (void)cdev;
    if (buf == NULL)
        return -EFAULT;
    if (count == 0 || (count % sizeof(struct kbd_event)) != 0)
        return -EINVAL;

    while (off < count) {
        struct kbd_event ev;

        if (user) {
            if (either_copyin(&ev, 1, (uint64)buf + off, sizeof(ev)) < 0)
                return -EFAULT;
        } else {
            memcpy(&ev, (const char *)buf + off, sizeof(ev));
        }

        spin_lock(&kbd_state.lock);
        ring_push(&ev);
        spin_unlock(&kbd_state.lock);
        off += sizeof(ev);
    }

    wakeup_on_chan(&kbd_state.ring);
    return (int)count;
}

static int kbd_ioctl(cdev_t *cdev, uint64 cmd, void *arg)
{
    (void)cdev; (void)cmd; (void)arg;
    return -EINVAL;
}

static int kbd_poll(cdev_t *cdev, short events)
{
    (void)cdev;
    short revents = 0;
    if (events & (POLLIN | POLLRDNORM | POLLRDBAND)) {
        spin_lock(&kbd_state.lock);
        if (!ring_empty())
            revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
        spin_unlock(&kbd_state.lock);
    }
    return revents;
}

static cdev_t kbd_cdev = {
    .dev = {
        .major = KBD_MAJOR,
        .minor = KBD_MINOR,
        .devname = "kbd",
        .devmode = S_IFCHR | 0666,
    },
    .readable = 1,
    .writable = 1,
    .ops = {
        .read    = kbd_read,
        .write   = kbd_write,
        .open    = kbd_open,
        .release = kbd_release,
        .ioctl   = kbd_ioctl,
        .poll    = kbd_poll,
    },
};

/* ── Initialization ──────────────────────────────────────────────── */

void ps2kbd_init(void)
{
    spin_init(&kbd_state.lock, "kbd");
    kbd_state.head = kbd_state.tail = 0;
    kbd_state.shift_l = kbd_state.shift_r = 0;
    kbd_state.ctrl = kbd_state.alt = 0;
    kbd_state.extended = 0;

    int ret = cdev_register(&kbd_cdev);
    assert(ret == 0, "ps2kbd_init: cdev_register failed: %d", ret);

    if (!ps2_controller_present()) {
        printf("PS2 keyboard: i8042 controller not present; /dev/kbd is synthetic-only\n");
        return;
    }

    /* Enable keyboard port (port 1) — read config, set bit 0 for IRQ1 */
    kbd_outb(PS2_CMD_PORT, 0x20);  /* read config */
    for (int i = 0; i < 100000; i++)
        if (kbd_inb(PS2_STATUS_PORT) & 0x01) break;
    uint8 config = kbd_inb(PS2_DATA_PORT);

    config |= 0x01;   /* enable IRQ1 (keyboard interrupt) */
    config &= ~0x10;  /* enable keyboard clock */

    kbd_outb(PS2_CMD_PORT, 0x60);  /* write config */
    for (int i = 0; i < 100000; i++)
        if (!(kbd_inb(PS2_STATUS_PORT) & 0x02)) break;
    kbd_outb(PS2_DATA_PORT, config);

    /* Flush any pending data */
    for (int i = 0; i < 256 && (kbd_inb(PS2_STATUS_PORT) & 0x01); i++)
        (void)kbd_inb(PS2_DATA_PORT);

    /* Set PS/2 typematic to slowest rate (1000 ms delay, 2 cps).
     * Hardware repeat is mostly suppressed by the IRQ handler's
     * key_down bitmap, but this further reduces PS/2 repeat traffic. */
    for (int i = 0; i < 100000; i++)
        if (!(kbd_inb(PS2_STATUS_PORT) & 0x02)) break;
    kbd_outb(PS2_DATA_PORT, 0xF3);  /* Set Typematic Rate/Delay */
    for (int i = 0; i < 100000; i++)
        if (kbd_inb(PS2_STATUS_PORT) & 0x01) break;
    (void)kbd_inb(PS2_DATA_PORT);   /* consume ACK */
    for (int i = 0; i < 100000; i++)
        if (!(kbd_inb(PS2_STATUS_PORT) & 0x02)) break;
    kbd_outb(PS2_DATA_PORT, 0x7F);  /* delay=1000ms, rate=2cps */
    for (int i = 0; i < 100000; i++)
        if (kbd_inb(PS2_STATUS_PORT) & 0x01) break;
    (void)kbd_inb(PS2_DATA_PORT);   /* consume ACK */

    /* Register IRQ handler */
    static struct irq_desc kbd_irq_desc = {
        .handler = kbd_irq_handler,
        .data = NULL,
        .dev = &kbd_cdev.dev,
    };

    ret = register_irq_handler(PLIC_IRQ(KBD_IRQ), &kbd_irq_desc);
    if (ret != 0) {
        printf("PS2 kbd: failed to register IRQ handler: %d\n", ret);
        return;
    }

    /* Enable IRQ 1 in the I/O APIC */
    extern void plic_enable_irq(int irq);
    plic_enable_irq(KBD_IRQ);

    printf("PS2 keyboard: /dev/kbd registered (IRQ %d)\n", KBD_IRQ);
}

#else /* !x86_64 */

void ps2kbd_init(void) {}

#endif /* __x86_64__ */
