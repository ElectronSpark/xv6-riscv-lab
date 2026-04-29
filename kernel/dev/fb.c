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
#include <proc/thread.h>
#include <printf.h>
#include <lock/spinlock.h>
#include <cmdline.h>

#if defined(__x86_64__) || defined(__i386__)

/* ── I/O port helpers ────────────────────────────────────────────── */

static inline void fb_outb(uint16 port, uint8 val)
{
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8 fb_inb(uint16 port)
{
    uint8 val;
    asm volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

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
    struct fb_gpu_stats stats;
    spinlock_t  lock;           /* serializes concurrent access */
} fb_state = {
    .lock = SPINLOCK_INITIALIZED("fb"),
};

static int fb_cmdline_enabled(const char *key)
{
    char buf[16];

    if (cmdline_get_param(key, buf, sizeof(buf)) != 0)
        return 0;
    return strcmp(buf, "1") == 0 ||
           strcmp(buf, "yes") == 0 ||
           strcmp(buf, "true") == 0 ||
           strcmp(buf, "on") == 0;
}

static int fb_blit_from_user(struct fb_gpu_blit cmd, int count_present)
{
    uint32 xres, yres;
    int clipped = 0;

    if (cmd.w == 0 || cmd.h == 0)
        return 0;
    if (cmd.w > 0xffffffffU / 4) {
        spin_lock(&fb_state.lock);
        fb_state.stats.rejected_blits++;
        spin_unlock(&fb_state.lock);
        return -EINVAL;
    }
    if (cmd.pixels == 0 || cmd.src_pitch == 0 ||
        (cmd.src_pitch & 3) != 0 ||
        cmd.src_pitch < cmd.w * 4) {
        spin_lock(&fb_state.lock);
        fb_state.stats.rejected_blits++;
        spin_unlock(&fb_state.lock);
        return -EINVAL;
    }

    spin_lock(&fb_state.lock);
    xres = fb_state.xres;
    yres = fb_state.yres;
    spin_unlock(&fb_state.lock);

    if (cmd.x >= xres || cmd.y >= yres)
        return 0;

    uint32 cw = cmd.w, ch = cmd.h;
    if ((uint64)cmd.x + cw > xres) {
        cw = xres - cmd.x;
        clipped = 1;
    }
    if ((uint64)cmd.y + ch > yres) {
        ch = yres - cmd.y;
        clipped = 1;
    }
    if (cw == 0 || ch == 0)
        return 0;

    uint8 kbuf[4096];
    uint32 max_px = sizeof(kbuf) / 4;
    volatile uint32 *fb = (volatile uint32 *)fb_state.fb_virt;
    uint32 stride = xres;

    spin_lock(&fb_state.lock);
    if (cmd.x == 0 && cmd.y == 0 && cw == xres && ch == yres)
        fb_state.stats.full_blits++;
    else
        fb_state.stats.partial_blits++;
    if (clipped)
        fb_state.stats.clipped_blits++;
    fb_state.stats.blit_bytes += (uint64)cw * ch * 4;
    if (count_present)
        fb_state.stats.bo_presents++;
    spin_unlock(&fb_state.lock);

    for (uint32 row = 0; row < ch; row++) {
        uint64 src_addr = cmd.pixels + (uint64)row * cmd.src_pitch;
        volatile uint32 *dst = fb + (cmd.y + row) * stride + cmd.x;
        uint32 remaining = cw;
        uint32 col = 0;

        while (remaining > 0) {
            uint32 chunk = remaining;
            if (chunk > max_px)
                chunk = max_px;
            if (either_copyin(kbuf, 1, src_addr + col * 4, chunk * 4) < 0)
                return -EFAULT;
            spin_lock(&fb_state.lock);
            uint32 *src = (uint32 *)kbuf;
            for (uint32 p = 0; p < chunk; p++)
                dst[col + p] = src[p];
            spin_unlock(&fb_state.lock);
            col += chunk;
            remaining -= chunk;
        }
    }
    virtio_gpu_present_fb_rect(fb_state.fb_virt, fb_state.pitch,
                               cmd.x, cmd.y, cw, ch);
    return 0;
}

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

    /*
     * Ensure VGA Attribute Controller has PAS (Palette Address Source)
     * bit set.  QEMU's vga_draw_graphic() checks ar_index & 0x20 and
     * blanks the display if PAS is clear.  BGA mode setting does NOT
     * touch the legacy VGA registers, so PAS may be left clear after
     * SeaBIOS → kernel handoff.
     */
    fb_inb(0x3DA);          /* Reset AR flip-flop */
    fb_outb(0x3C0, 0x20);   /* Set PAS bit, index 0 */

    /* Misc Output Register: enable RAM access, color I/O base (0x3D?) */
    fb_outb(0x3C2, 0x63);

    /* Sequencer register 01h, bit 5 = "Screen Off" — ensure it's clear */
    fb_outb(0x3C4, 0x01);
    uint8 seq1 = fb_inb(0x3C5);
    fb_outb(0x3C5, seq1 & ~0x20);

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

    /* Set video mode — check cmdline "video=WxH" first, else defaults */
    uint32 width = FB_DEFAULT_WIDTH;
    uint32 height = FB_DEFAULT_HEIGHT;
    {
        char vbuf[32];
        if (cmdline_get_param("video", vbuf, sizeof(vbuf)) == 0) {
            /* Parse "WIDTHxHEIGHT" */
            uint32 w = 0, h = 0;
            const char *p = vbuf;
            while (*p >= '0' && *p <= '9')
                w = w * 10 + (*p++ - '0');
            if (*p == 'x' || *p == 'X') {
                p++;
                while (*p >= '0' && *p <= '9')
                    h = h * 10 + (*p++ - '0');
            }
            if (w >= 640 && w <= 2560 && h >= 480 && h <= 1600) {
                width = w;
                height = h;
                printf("FB: using cmdline resolution %dx%d\n", w, h);
            } else {
                printf("FB: ignoring invalid video=%s, using default\n", vbuf);
            }
        }
    }
    bga_set_mode(width, height, FB_DEFAULT_BPP);

    /* Read back BGA registers to verify mode was actually set */
    {
        uint16 en  = bga_read_reg(VBE_DISPI_INDEX_ENABLE);
        uint16 rxr = bga_read_reg(VBE_DISPI_INDEX_XRES);
        uint16 ryr = bga_read_reg(VBE_DISPI_INDEX_YRES);
        uint16 rbp = bga_read_reg(VBE_DISPI_INDEX_BPP);
        printf("FB: BGA readback: enable=0x%x xres=%d yres=%d bpp=%d\n",
               en, rxr, ryr, rbp);
    }

    printf("FB: mode set to %dx%dx%d (pitch=%d, size=%d)\n",
           fb_state.xres, fb_state.yres, fb_state.bpp,
           fb_state.pitch, fb_state.fb_size);

    if (fb_cmdline_enabled("fbtest")) {
        /* Write a bright test pattern to verify LFB writes reach the VGA.
         * Top 16 scanlines: bright blue. Rest: black. */
        volatile uint32 *pixels = (volatile uint32 *)fb_state.fb_virt;
        uint32 npx = fb_state.xres * fb_state.yres;
        /* Clear to black first */
        for (uint32 i = 0; i < npx; i++)
            pixels[i] = 0x00000000;
        /* Bright blue bar at top */
        uint32 bar_rows = 16;
        for (uint32 y = 0; y < bar_rows && y < fb_state.yres; y++) {
            for (uint32 x = 0; x < fb_state.xres; x++) {
                pixels[y * fb_state.xres + x] = 0x000080FF; /* bright blue (BGRX) */
            }
        }
        /* Also write a bright white cross at center for visibility */
        uint32 cx = fb_state.xres / 2;
        uint32 cy = fb_state.yres / 2;
        for (uint32 i = 0; i < 40; i++) {
            if (cx - 20 + i < fb_state.xres)
                pixels[cy * fb_state.xres + cx - 20 + i] = 0x00FFFFFF;
            if (cy - 20 + i < fb_state.yres)
                pixels[(cy - 20 + i) * fb_state.xres + cx] = 0x00FFFFFF;
        }
        /* Read back a pixel to verify the write landed */
        uint32 readback = pixels[0];
        printf("FB: LFB test pattern written, readback pixel[0]=0x%x (expect 0x000080FF)\n",
               readback);
    } else {
        volatile uint32 *pixels = (volatile uint32 *)fb_state.fb_virt;
        uint32 npx = fb_state.xres * fb_state.yres;
        for (uint32 i = 0; i < npx; i++)
            pixels[i] = 0x00000000;
    }

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

/* ── GPU acceleration primitives ──────────────────────────────────── */

/*
 * gpu_fill_rect — fill a rectangle in the LFB with a solid color.
 * Writes directly to MMIO framebuffer memory.  Caller must hold fb_state.lock.
 */
static void gpu_fill_rect_locked(uint32 x, uint32 y, uint32 w, uint32 h,
                                 uint32 color)
{
    volatile uint32 *fb = (volatile uint32 *)fb_state.fb_virt;
    uint32 stride = fb_state.xres;  /* pixels per row */

    for (uint32 row = y; row < y + h; row++) {
        volatile uint32 *dst = fb + row * stride + x;
        for (uint32 col = 0; col < w; col++)
            dst[col] = color;
    }
}

/*
 * gpu_copy_rect — screen-to-screen rectangle copy.
 * Handles overlapping regions with correct copy direction.
 * Caller must hold fb_state.lock.
 */
static void gpu_copy_rect_locked(uint32 sx, uint32 sy,
                                 uint32 dx, uint32 dy,
                                 uint32 w, uint32 h)
{
    volatile uint32 *fb = (volatile uint32 *)fb_state.fb_virt;
    uint32 stride = fb_state.xres;

    if (dy < sy || (dy == sy && dx < sx)) {
        /* Copy top-to-bottom, left-to-right */
        for (uint32 row = 0; row < h; row++) {
            volatile uint32 *src = fb + (sy + row) * stride + sx;
            volatile uint32 *dst = fb + (dy + row) * stride + dx;
            for (uint32 col = 0; col < w; col++)
                dst[col] = src[col];
        }
    } else {
        /* Copy bottom-to-top, right-to-left (overlapping case) */
        for (uint32 row = h; row > 0; row--) {
            volatile uint32 *src = fb + (sy + row - 1) * stride + sx;
            volatile uint32 *dst = fb + (dy + row - 1) * stride + dx;
            for (uint32 col = w; col > 0; col--)
                dst[col - 1] = src[col - 1];
        }
    }
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
        uint8 kbuf[4096];
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
        virtio_gpu_present_fb_rect(fb_state.fb_virt, fb_state.pitch,
                                   0, 0, fb_state.xres, fb_state.yres);
        return done;
    } else {
        spin_lock(&fb_state.lock);
        memcpy((void *)fb_state.fb_virt, buf, count);
        spin_unlock(&fb_state.lock);
        virtio_gpu_present_fb_rect(fb_state.fb_virt, fb_state.pitch,
                                   0, 0, fb_state.xres, fb_state.yres);
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
        uint8 kbuf[4096];
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

    case FB_GPU_FILL_RECT: {
        struct fb_gpu_fill cmd;
        int clipped = 0;
        if (either_copyin((char *)&cmd, 1, (uint64)arg, sizeof(cmd)) < 0)
            return -EFAULT;
        if (cmd.w == 0 || cmd.h == 0)
            return 0;

        spin_lock(&fb_state.lock);
        /* Clip to screen bounds */
        if (cmd.x >= fb_state.xres || cmd.y >= fb_state.yres) {
            spin_unlock(&fb_state.lock);
            return 0;
        }
        if ((uint64)cmd.x + cmd.w > fb_state.xres) {
            cmd.w = fb_state.xres - cmd.x;
            clipped = 1;
        }
        if ((uint64)cmd.y + cmd.h > fb_state.yres) {
            cmd.h = fb_state.yres - cmd.y;
            clipped = 1;
        }

        gpu_fill_rect_locked(cmd.x, cmd.y, cmd.w, cmd.h, cmd.color);
        fb_state.stats.fill_rects++;
        if (clipped)
            fb_state.stats.clipped_blits++;
        spin_unlock(&fb_state.lock);
        virtio_gpu_present_fb_rect(fb_state.fb_virt, fb_state.pitch,
                                   cmd.x, cmd.y, cmd.w, cmd.h);
        return 0;
    }

    case FB_GPU_BLIT: {
        struct fb_gpu_blit cmd;

        if (either_copyin((char *)&cmd, 1, (uint64)arg, sizeof(cmd)) < 0)
            return -EFAULT;
        return fb_blit_from_user(cmd, 0);
    }

    case FB_GPU_BO_CREATE: {
        struct fb_gpu_bo_create req;
        uint64 size;
        uint64 addr;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.width == 0 || req.height == 0 ||
            req.width > 8192 || req.height > 8192 ||
            req.width > 0xffffffffU / 4)
            return -EINVAL;

        req.pitch = req.width * 4;
        if ((uint64)req.pitch > ((uint64)-1) / req.height)
            return -EINVAL;
        size = (uint64)req.pitch * req.height;
        if (size == 0 || size > 64ULL * 1024 * 1024)
            return -EINVAL;

        addr = vm_mmap(current->vm, 0, (size_t)size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if ((int64)addr < 0)
            return (int)addr;

        req.size = PGROUNDUP(size);
        req.addr = addr;
        spin_lock(&fb_state.lock);
        fb_state.stats.bo_allocs++;
        fb_state.stats.bo_bytes += req.size;
        spin_unlock(&fb_state.lock);

        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            (void)vm_munmap(current->vm, addr, (size_t)req.size);
            return -EFAULT;
        }
        return 0;
    }

    case FB_GPU_BO_PRESENT: {
        struct fb_gpu_bo_present req;
        struct fb_gpu_blit blit;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        blit.x = req.x;
        blit.y = req.y;
        blit.w = req.w;
        blit.h = req.h;
        blit.src_pitch = req.src_pitch;
        blit.pixels = req.pixels;
        return fb_blit_from_user(blit, 1);
    }

    case FB_GPU_COPY_RECT: {
        struct fb_gpu_copy cmd;
        int clipped = 0;
        if (either_copyin((char *)&cmd, 1, (uint64)arg, sizeof(cmd)) < 0)
            return -EFAULT;
        if (cmd.w == 0 || cmd.h == 0)
            return 0;

        spin_lock(&fb_state.lock);
        /* Clip source and destination to screen bounds */
        if (cmd.src_x >= fb_state.xres || cmd.src_y >= fb_state.yres ||
            cmd.dst_x >= fb_state.xres || cmd.dst_y >= fb_state.yres) {
            spin_unlock(&fb_state.lock);
            return 0;
        }
        uint32 w = cmd.w, h = cmd.h;
        if ((uint64)cmd.src_x + w > fb_state.xres) {
            w = fb_state.xres - cmd.src_x;
            clipped = 1;
        }
        if ((uint64)cmd.src_y + h > fb_state.yres) {
            h = fb_state.yres - cmd.src_y;
            clipped = 1;
        }
        if ((uint64)cmd.dst_x + w > fb_state.xres) {
            w = fb_state.xres - cmd.dst_x;
            clipped = 1;
        }
        if ((uint64)cmd.dst_y + h > fb_state.yres) {
            h = fb_state.yres - cmd.dst_y;
            clipped = 1;
        }

        gpu_copy_rect_locked(cmd.src_x, cmd.src_y,
                             cmd.dst_x, cmd.dst_y, w, h);
        fb_state.stats.copy_rects++;
        if (clipped)
            fb_state.stats.clipped_blits++;
        spin_unlock(&fb_state.lock);
        virtio_gpu_present_fb_rect(fb_state.fb_virt, fb_state.pitch,
                                   cmd.dst_x, cmd.dst_y, w, h);
        return 0;
    }

    case FB_GPU_GET_STATS: {
        struct fb_gpu_stats stats;
        spin_lock(&fb_state.lock);
        stats = fb_state.stats;
        spin_unlock(&fb_state.lock);
        virtio_gpu_get_fb_stats(&stats);
        if (either_copyout(1, (uint64)arg, (char *)&stats, sizeof(stats)) < 0)
            return -EFAULT;
        return 0;
    }

    case FBIOPUT_VSCREENINFO: {
        struct fb_var_screeninfo req;
        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;

        /* Validate requested resolution */
        if (req.xres < 640 || req.xres > 2560 ||
            req.yres < 480 || req.yres > 1600)
            return -EINVAL;

        spin_lock(&fb_state.lock);
        bga_set_mode(req.xres, req.yres, FB_DEFAULT_BPP);

        /* Clear framebuffer to black after mode change */
        volatile uint32 *pixels = (volatile uint32 *)fb_state.fb_virt;
        uint32 npx = fb_state.xres * fb_state.yres;
        for (uint32 i = 0; i < npx; i++)
            pixels[i] = 0x00000000;
        spin_unlock(&fb_state.lock);
        virtio_gpu_present_fb_rect(fb_state.fb_virt, fb_state.pitch,
                                   0, 0, fb_state.xres, fb_state.yres);

        printf("FB: resolution changed to %dx%d\n", fb_state.xres, fb_state.yres);
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

/* ── Panic screen: BSOD-style framebuffer overlay ─────────────────── */

/* Embedded 8x16 VGA bitmap font — printable ASCII 32–126 (95 glyphs) */
static const uint8 panic_font[95][16] = {
    /* 32 ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 33 '!' */ {0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 34 '"' */ {0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 35 '#' */ {0x00,0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00},
    /* 36 '$' */ {0x18,0x18,0x7C,0xC6,0xC2,0xC0,0x7C,0x06,0x06,0x86,0xC6,0x7C,0x18,0x18,0x00,0x00},
    /* 37 '%' */ {0x00,0x00,0x00,0x00,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC6,0x86,0x00,0x00,0x00,0x00},
    /* 38 '&' */ {0x00,0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    /* 39 ''' */ {0x00,0x30,0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 40 '(' */ {0x00,0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00},
    /* 41 ')' */ {0x00,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00},
    /* 42 '*' */ {0x00,0x00,0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 43 '+' */ {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 44 ',' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00},
    /* 45 '-' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 46 '.' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 47 '/' */ {0x00,0x00,0x00,0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00},
    /* 48 '0' */ {0x00,0x00,0x38,0x6C,0xC6,0xC6,0xD6,0xD6,0xC6,0xC6,0x6C,0x38,0x00,0x00,0x00,0x00},
    /* 49 '1' */ {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00},
    /* 50 '2' */ {0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0x00,0x00,0x00,0x00},
    /* 51 '3' */ {0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 52 '4' */ {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00},
    /* 53 '5' */ {0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 54 '6' */ {0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 55 '7' */ {0x00,0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00},
    /* 56 '8' */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 57 '9' */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00},
    /* 58 ':' */ {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    /* 59 ';' */ {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00},
    /* 60 '<' */ {0x00,0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00},
    /* 61 '=' */ {0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 62 '>' */ {0x00,0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00},
    /* 63 '?' */ {0x00,0x00,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 64 '@' */ {0x00,0x00,0x00,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0x7C,0x00,0x00,0x00,0x00},
    /* 65 'A' */ {0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 66 'B' */ {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00},
    /* 67 'C' */ {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00,0x00},
    /* 68 'D' */ {0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00},
    /* 69 'E' */ {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    /* 70 'F' */ {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    /* 71 'G' */ {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00,0x00,0x00,0x00},
    /* 72 'H' */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 73 'I' */ {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /* 74 'J' */ {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00},
    /* 75 'K' */ {0x00,0x00,0xE6,0x66,0x66,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    /* 76 'L' */ {0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    /* 77 'M' */ {0x00,0x00,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 78 'N' */ {0x00,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 79 'O' */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 80 'P' */ {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    /* 81 'Q' */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0x00,0x00},
    /* 82 'R' */ {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    /* 83 'S' */ {0x00,0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 84 'T' */ {0x00,0x00,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /* 85 'U' */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 86 'V' */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00,0x00,0x00,0x00},
    /* 87 'W' */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0xEE,0x6C,0x00,0x00,0x00,0x00},
    /* 88 'X' */ {0x00,0x00,0xC6,0xC6,0x6C,0x7C,0x38,0x38,0x7C,0x6C,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 89 'Y' */ {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x78,0x30,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00},
    /* 90 'Z' */ {0x00,0x00,0xFE,0xC6,0x86,0x0C,0x18,0x30,0x60,0xC2,0xC6,0xFE,0x00,0x00,0x00,0x00},
    /* 91 '[' */ {0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00,0x00},
    /* 92 '\' */ {0x00,0x00,0x00,0x80,0xC0,0xE0,0x70,0x38,0x1C,0x0E,0x06,0x02,0x00,0x00,0x00,0x00},
    /* 93 ']' */ {0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00,0x00},
    /* 94 '^' */ {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 95 '_' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00},
    /* 96 '`' */ {0x30,0x30,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 97 'a' */ {0x00,0x00,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    /* 98 'b' */ {0x00,0x00,0xE0,0x60,0x60,0x78,0x6C,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00},
    /* 99 'c' */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /*100 'd' */ {0x00,0x00,0x1C,0x0C,0x0C,0x3C,0x6C,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    /*101 'e' */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /*102 'f' */ {0x00,0x00,0x1C,0x36,0x32,0x30,0x78,0x30,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00},
    /*103 'g' */ {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0x00},
    /*104 'h' */ {0x00,0x00,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    /*105 'i' */ {0x00,0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /*106 'j' */ {0x00,0x00,0x06,0x06,0x00,0x0E,0x06,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0x00},
    /*107 'k' */ {0x00,0x00,0xE0,0x60,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00},
    /*108 'l' */ {0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /*109 'm' */ {0x00,0x00,0x00,0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0xD6,0xC6,0x00,0x00,0x00,0x00},
    /*110 'n' */ {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00},
    /*111 'o' */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /*112 'p' */ {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00},
    /*113 'q' */ {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0x00},
    /*114 'r' */ {0x00,0x00,0x00,0x00,0x00,0xDC,0x76,0x66,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    /*115 's' */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /*116 't' */ {0x00,0x00,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x30,0x36,0x1C,0x00,0x00,0x00,0x00},
    /*117 'u' */ {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    /*118 'v' */ {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00,0x00,0x00,0x00},
    /*119 'w' */ {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0x6C,0x00,0x00,0x00,0x00},
    /*120 'x' */ {0x00,0x00,0x00,0x00,0x00,0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00},
    /*121 'y' */ {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8,0x00},
    /*122 'z' */ {0x00,0x00,0x00,0x00,0x00,0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0x00,0x00,0x00,0x00},
    /*123 '{' */ {0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00},
    /*124 '|' */ {0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00},
    /*125 '}' */ {0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00},
    /*126 '~' */ {0x00,0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

/* Put one glyph at pixel (px, py) with given foreground color. */
static void panic_putchar(volatile uint32 *fb, uint32 stride,
                          uint32 xres, uint32 yres,
                          int px, int py, char ch, uint32 fg)
{
    if (ch < 32 || ch > 126)
        return;
    const uint8 *glyph = panic_font[ch - 32];
    for (int row = 0; row < 16; row++) {
        int y = py + row;
        if (y < 0 || (uint32)y >= yres)
            continue;
        uint8 bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            int x = px + col;
            if (x < 0 || (uint32)x >= xres)
                continue;
            if (bits & (0x80 >> col))
                fb[y * stride + x] = fg;
        }
    }
}

/* Render a NUL-terminated string, handling newlines. */
static void panic_puts(volatile uint32 *fb, uint32 stride,
                       uint32 xres, uint32 yres,
                       int *cx, int *cy, const char *s, uint32 fg)
{
    while (*s) {
        if (*s == '\n') {
            *cx = 32;   /* left margin */
            *cy += 18;  /* line height = 16px + 2px spacing */
        } else {
            panic_putchar(fb, stride, xres, yres, *cx, *cy, *s, fg);
            *cx += 8;
            if ((uint32)(*cx + 8) >= xres) {
                *cx = 32;
                *cy += 18;
            }
        }
        s++;
    }
}

/*
 * fb_panic_screen — render a BSOD-style panic screen.
 * Called from __panic_end() with the captured panic text.
 * Safe to call with no locks held (we're the only core alive).
 */
void fb_panic_screen(const char *text)
{
    if (!fb_state.detected || !fb_state.fb_virt)
        return;

    volatile uint32 *fb = (volatile uint32 *)fb_state.fb_virt;
    uint32 xres = fb_state.xres;
    uint32 yres = fb_state.yres;
    uint32 stride = xres;

    /* Fill background dark blue: BGRA(0xAA, 0x00, 0x00, 0x00) */
    uint32 bg = 0x00AA0000;
    uint32 npx = xres * yres;
    for (uint32 i = 0; i < npx; i++)
        fb[i] = bg;

    /* White foreground text */
    uint32 fg_white = 0x00FFFFFF;
    /* Yellow for the header */
    uint32 fg_yellow = 0x0000FFFF;

    int cx = 32, cy = 40;

    /* Header */
    panic_puts(fb, stride, xres, yres, &cx, &cy,
               "*** KERNEL PANIC ***", fg_yellow);
    cy += 18;

    /* Separator line */
    for (uint32 x = 32; x < xres - 32; x++)
        fb[cy * stride + x] = fg_yellow;
    cy += 10;
    cx = 32;

    /* Panic message text */
    if (text && text[0])
        panic_puts(fb, stride, xres, yres, &cx, &cy, text, fg_white);

    /* Footer */
    cy += 36;
    cx = 32;
    panic_puts(fb, stride, xres, yres, &cx, &cy,
               "System halted. Please restart the computer.", fg_yellow);
}

#else /* !x86_64 */

void fb_pci_init(uint8 bus, uint8 dev, uint8 func)
{
    (void)bus; (void)dev; (void)func;
}

int fb_detected(void) { return 0; }

void fbdevinit(void) {}

void fb_panic_screen(const char *text) { (void)text; }

#endif /* __x86_64__ */
