#ifndef __KERNEL_DEV_FB_H
#define __KERNEL_DEV_FB_H

#include <types.h>

/*
 * Framebuffer device (/dev/fb0) — Bochs VGA (BGA) driver for QEMU x86_64.
 *
 * Provides a linear framebuffer at a fixed resolution.  Userspace writes
 * pixel data via write() or queries screen info via ioctl().
 */

/* ── ioctl commands (Linux fbdev compatible subset) ── */
#define FBIOGET_VSCREENINFO  0x4600
#define FBIOGET_FSCREENINFO  0x4602

/* Variable screen info (returned by FBIOGET_VSCREENINFO) */
struct fb_var_screeninfo {
    uint32 xres;            /* visible resolution */
    uint32 yres;
    uint32 bits_per_pixel;  /* 32 for BGRA8888 */
    uint32 pitch;           /* bytes per scanline */
};

/* Fixed screen info (returned by FBIOGET_FSCREENINFO) */
struct fb_fix_screeninfo {
    char   id[16];          /* identification string */
    uint64 smem_start;      /* physical address of framebuffer */
    uint32 smem_len;        /* length of framebuffer mem */
    uint32 line_length;     /* bytes per scanline */
};

/* ── Bochs VGA (BGA) register interface ── */
#define VBE_DISPI_IOPORT_INDEX    0x01CE
#define VBE_DISPI_IOPORT_DATA     0x01CF

#define VBE_DISPI_INDEX_ID        0x0
#define VBE_DISPI_INDEX_XRES      0x1
#define VBE_DISPI_INDEX_YRES      0x2
#define VBE_DISPI_INDEX_BPP       0x3
#define VBE_DISPI_INDEX_ENABLE    0x4
#define VBE_DISPI_INDEX_BANK      0x5
#define VBE_DISPI_INDEX_VIRT_WIDTH  0x6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 0x7
#define VBE_DISPI_INDEX_X_OFFSET  0x8
#define VBE_DISPI_INDEX_Y_OFFSET  0x9

#define VBE_DISPI_DISABLED        0x00
#define VBE_DISPI_ENABLED         0x01
#define VBE_DISPI_LFB_ENABLED     0x40

/* PCI identifiers for Bochs VGA */
#define PCI_VENDOR_BOCHS          0x1234
#define PCI_DEVICE_BOCHS_VGA      0x1111

/* Default resolution */
#define FB_DEFAULT_WIDTH   1024
#define FB_DEFAULT_HEIGHT   768
#define FB_DEFAULT_BPP       32

/* Device numbers */
#define FB_MAJOR  29
#define FB_MINOR   0

/* Kernel API */
void fbdevinit(void);
int  fb_detected(void);
void fb_pci_init(uint8 bus, uint8 dev, uint8 func);

#endif /* __KERNEL_DEV_FB_H */
