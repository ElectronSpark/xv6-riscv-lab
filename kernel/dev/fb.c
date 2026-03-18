/*
 * fb.c — Bochs VGA (BGA) framebuffer driver for x86_64 QEMU.
 *
 * Exposes /dev/fb0 as a character device.  Userspace can:
 *   - write()  pixel data (BGRA8888 format, row-major from top-left)
 *   - ioctl()  FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO for screen parameters
 *   - lseek()  is implicit via the write offset
 *
 * The Bochs Display Adapter uses VBE dispi I/O ports (0x01CE/0x01CF) for
 * mode setting and exposes the linear framebuffer through PCI BAR0.
 */

#include <types.h>
#include <param.h>
#include <riscv.h>
#include <defs.h>
#include <errno.h>
#include <string.h>
#include <dev/cdev.h>
#include <dev/fb.h>
#include <dev/pci.h>
#include <mm/vm.h>
#include <printf.h>
#include <lock/spinlock.h>

#if defined(__x86_64__) || defined(__i386__)

/* ── I/O port helpers ────────────────────────────────────────────── */

static inline void fb_outw(uint16 port, uint16 val)
{
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16 fb_inw(uint16 port)
{
    uint16 val;
    asm volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* ── BGA register access ─────────────────────────────────────────── */

static void bga_write_reg(uint16 index, uint16 value)
{
    fb_outw(VBE_DISPI_IOPORT_INDEX, index);
    fb_outw(VBE_DISPI_IOPORT_DATA, value);
}

static uint16 bga_read_reg(uint16 index)
{
    fb_outw(VBE_DISPI_IOPORT_INDEX, index);
    return fb_inw(VBE_DISPI_IOPORT_DATA);
}

/* ── Module state ─────────────────────────────────────────────────── */

static struct {
    int         detected;       /* non-zero if BGA found on PCI bus */
    uint64      fb_phys;        /* physical address of LFB (BAR0) */
    volatile uint8 *fb_virt;    /* kernel virtual address of LFB */
    uint32      xres;
    uint32      yres;
    uint32      bpp;
    uint32      pitch;          /* bytes per scanline */
    uint32      fb_size;        /* total framebuffer size in bytes */
    spinlock_t  lock;           /* serializes concurrent access */
} fb_state = {
    .lock = SPINLOCK_INITIALIZED("fb"),
};

/* ── BGA mode setting ─────────────────────────────────────────────── */

static void bga_set_mode(uint32 width, uint32 height, uint32 bpp)
{
    bga_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bga_write_reg(VBE_DISPI_INDEX_XRES, (uint16)width);
    bga_write_reg(VBE_DISPI_INDEX_YRES, (uint16)height);
    bga_write_reg(VBE_DISPI_INDEX_BPP,  (uint16)bpp);
    bga_write_reg(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16)width);
    bga_write_reg(VBE_DISPI_INDEX_VIRT_HEIGHT, (uint16)height);
    bga_write_reg(VBE_DISPI_INDEX_X_OFFSET, 0);
    bga_write_reg(VBE_DISPI_INDEX_Y_OFFSET, 0);
    bga_write_reg(VBE_DISPI_INDEX_ENABLE,
                  VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    fb_state.xres  = width;
    fb_state.yres  = height;
    fb_state.bpp   = bpp;
    fb_state.pitch = width * (bpp / 8);
    fb_state.fb_size = fb_state.pitch * height;
}

/* ── PCI discovery (called from pci_init) ─────────────────────────── */

void fb_pci_init(uint8 bus, uint8 dev, uint8 func)
{
    /* Guard against double-init on multi-core PCI scan */
    spin_lock(&fb_state.lock);
    if (fb_state.detected) {
        spin_unlock(&fb_state.lock);
        return;
    }
    spin_unlock(&fb_state.lock);

    printf("PCI: Bochs VGA detected at %d:%d:%d\n", bus, dev, func);

    /* Enable memory access */
    uint16 cmd = pci_config_read16(bus, dev, func, 0x04);
    cmd |= PCIE_CSCMD_MAE;
    pci_config_write16(bus, dev, func, 0x04, cmd);

    /* Read BAR0 — linear framebuffer base (memory BAR, mask low 4 bits) */
    uint32 bar0 = pci_config_read32(bus, dev, func, 0x10) & ~0xFU;

    fb_state.fb_phys = (uint64)bar0;
    fb_state.fb_virt = (volatile uint8 *)PA2VA(fb_state.fb_phys);

    printf("FB: BAR0 = 0x%lx (VA = 0x%lx)\n",
           fb_state.fb_phys, (uint64)fb_state.fb_virt);

    /* Verify BGA is present by reading the ID register */
    uint16 id = bga_read_reg(VBE_DISPI_INDEX_ID);
    printf("FB: BGA ID = 0x%x\n", id);

    /* Set default video mode */
    bga_set_mode(FB_DEFAULT_WIDTH, FB_DEFAULT_HEIGHT, FB_DEFAULT_BPP);

    printf("FB: mode set to %dx%dx%d (pitch=%d, size=%d)\n",
           fb_state.xres, fb_state.yres, fb_state.bpp,
           fb_state.pitch, fb_state.fb_size);

    /* Clear framebuffer to black */
    memset((void *)fb_state.fb_virt, 0, fb_state.fb_size);

    /* Publish as detected — must be last so readers see consistent state */
    __sync_synchronize();
    spin_lock(&fb_state.lock);
    fb_state.detected = 1;
    spin_unlock(&fb_state.lock);
}

int fb_detected(void)
{
    return fb_state.detected;
}

/* ── Character device operations ──────────────────────────────────── */

static int fb_open(cdev_t *cdev) { return 0; }
static int fb_release(cdev_t *cdev) { return 0; }

static int fb_write(cdev_t *cdev, bool user, const void *buf, size_t count)
{
    (void)cdev;

    if (!fb_state.detected)
        return -ENODEV;
    if (buf == NULL)
        return -EINVAL;
    if (count == 0)
        return 0;

    /* Clamp to framebuffer size (write always starts at offset 0) */
    if (count > fb_state.fb_size)
        count = fb_state.fb_size;

    if (user) {
        /*
         * Copy from userspace in chunks.  copyin can sleep so we must
         * NOT hold the spinlock across it.  Instead: copyin → lock →
         * memcpy to MMIO → unlock.
         */
        uint8 kbuf[512];
        size_t done = 0;
        while (done < count) {
            size_t chunk = count - done;
            if (chunk > sizeof(kbuf))
                chunk = sizeof(kbuf);
            if (either_copyin(kbuf, 1, (uint64)buf + done, chunk) < 0)
                return done ? (int)done : -EFAULT;
            spin_lock(&fb_state.lock);
            memcpy((void *)(fb_state.fb_virt + done), kbuf, chunk);
            spin_unlock(&fb_state.lock);
            done += chunk;
        }
        return done;
    } else {
        spin_lock(&fb_state.lock);
        memcpy((void *)fb_state.fb_virt, buf, count);
        spin_unlock(&fb_state.lock);
        return count;
    }
}

static int fb_read(cdev_t *cdev, bool user, void *buf, size_t count)
{
    (void)cdev;

    if (!fb_state.detected)
        return -ENODEV;
    if (buf == NULL)
        return -EINVAL;
    if (count == 0)
        return 0;

    if (count > fb_state.fb_size)
        count = fb_state.fb_size;

    if (user) {
        /*
         * Lock → memcpy from MMIO → unlock → copyout.
         * copyout may sleep, so it must be outside the lock.
         */
        uint8 kbuf[512];
        size_t done = 0;
        while (done < count) {
            size_t chunk = count - done;
            if (chunk > sizeof(kbuf))
                chunk = sizeof(kbuf);
            spin_lock(&fb_state.lock);
            memcpy(kbuf, (void *)(fb_state.fb_virt + done), chunk);
            spin_unlock(&fb_state.lock);
            if (either_copyout(1, (uint64)buf + done, kbuf, chunk) < 0)
                return done ? (int)done : -EFAULT;
            done += chunk;
        }
        return done;
    } else {
        spin_lock(&fb_state.lock);
        memcpy(buf, (void *)fb_state.fb_virt, count);
        spin_unlock(&fb_state.lock);
        return count;
    }
}

static int fb_ioctl(cdev_t *cdev, uint64 cmd, void *arg)
{
    (void)cdev;

    if (!fb_state.detected)
        return -ENODEV;

    switch (cmd) {
    case FBIOGET_VSCREENINFO: {
        struct fb_var_screeninfo info;
        spin_lock(&fb_state.lock);
        info.xres = fb_state.xres;
        info.yres = fb_state.yres;
        info.bits_per_pixel = fb_state.bpp;
        info.pitch = fb_state.pitch;
        spin_unlock(&fb_state.lock);
        if (either_copyout(1, (uint64)arg, (char *)&info, sizeof(info)) < 0)
            return -EFAULT;
        return 0;
    }
    case FBIOGET_FSCREENINFO: {
        struct fb_fix_screeninfo info;
        memset(&info, 0, sizeof(info));
        spin_lock(&fb_state.lock);
        strncpy(info.id, "BochsVGA", sizeof(info.id) - 1);
        info.smem_start = fb_state.fb_phys;
        info.smem_len = fb_state.fb_size;
        info.line_length = fb_state.pitch;
        spin_unlock(&fb_state.lock);
        if (either_copyout(1, (uint64)arg, (char *)&info, sizeof(info)) < 0)
            return -EFAULT;
        return 0;
    }
    default:
        return -EINVAL;
    }
}

static cdev_t fb_cdev = {
    .dev = {
        .major = FB_MAJOR,
        .minor = FB_MINOR,
        .devname = "fb0",
        .devmode = S_IFCHR | 0666,
    },
    .readable = 1,
    .writable = 1,
    .ops = {
        .read    = fb_read,
        .write   = fb_write,
        .open    = fb_open,
        .release = fb_release,
        .ioctl   = fb_ioctl,
        .poll    = NULL,
    },
};

void fbdevinit(void)
{
    if (!fb_state.detected) {
        printf("FB: no Bochs VGA detected, skipping /dev/fb0\n");
        return;
    }

    int ret = cdev_register(&fb_cdev);
    assert(ret == 0, "fbdevinit: failed to register fb cdev: %d", ret);
    printf("FB: registered /dev/fb0 (%dx%dx%d)\n",
           fb_state.xres, fb_state.yres, fb_state.bpp);
}

#else /* !x86_64 */

void fb_pci_init(uint8 bus, uint8 dev, uint8 func)
{
    (void)bus; (void)dev; (void)func;
}

int fb_detected(void) { return 0; }

void fbdevinit(void) {}

#endif /* __x86_64__ */
