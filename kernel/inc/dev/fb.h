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
#define FBIOPUT_VSCREENINFO  0x4601   /* set resolution (WxH) at runtime */
#define FBIOGET_FSCREENINFO  0x4602

/* ── GPU acceleration ioctl commands ── */
#define FB_GPU_FILL_RECT     0x4610   /* fill rectangle with solid color */
#define FB_GPU_BLIT          0x4611   /* copy user buffer to screen rect */
#define FB_GPU_COPY_RECT     0x4612   /* screen-to-screen rectangle copy */
#define FB_GPU_GET_STATS     0x4613   /* query present/copy counters */
#define FB_GPU_BO_CREATE     0x4614   /* allocate/map a graphics buffer */
#define FB_GPU_BO_PRESENT    0x4615   /* present a mapped graphics buffer */
#define FB_GPU_BO_DESTROY    0x4616   /* destroy a graphics buffer handle */
#define FB_GPU_BO_IMPORT     0x4617   /* query/import a graphics buffer handle */

#define FB_GPU_BO_F_EXPORTABLE 0x1    /* return a stable kernel handle */

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

/* GPU fill rectangle command */
struct fb_gpu_fill {
    uint32 x;               /* destination x */
    uint32 y;               /* destination y */
    uint32 w;               /* width in pixels */
    uint32 h;               /* height in pixels */
    uint32 color;           /* ARGB8888 pixel value */
};

/* GPU blit command: copy user pixel data to a screen rectangle */
struct fb_gpu_blit {
    uint32   x;             /* destination x */
    uint32   y;             /* destination y */
    uint32   w;             /* width in pixels */
    uint32   h;             /* height in pixels */
    uint32   src_pitch;     /* source buffer pitch in bytes */
    uint64   pixels;        /* pointer to user pixel data (uint32[]) */
};

/* Allocate a graphics buffer and map it into the calling process.
 *
 * Userspace fills in width/height, then the kernel returns addr, pitch, and
 * size.  Release is normal munmap(addr, size).  This first-stage ABI is
 * intentionally process-local; later virtio-gpu/Mesa work can add exportable
 * handles/fences without changing the basic create/fill/present shape.
 */
struct fb_gpu_bo_create {
    uint32   width;          /* requested width in pixels */
    uint32   height;         /* requested height in pixels */
    uint32   flags;          /* FB_GPU_BO_F_* */
    uint32   pitch;          /* returned pitch in bytes */
    uint64   size;           /* returned mapping size in bytes */
    uint64   addr;           /* returned user virtual address */
    uint32   handle;         /* returned export/import handle */
    uint32   reserved;
};

/* Present a mapped graphics buffer to the framebuffer. */
struct fb_gpu_bo_present {
    uint32   x;              /* destination x */
    uint32   y;              /* destination y */
    uint32   w;              /* width in pixels */
    uint32   h;              /* height in pixels */
    uint32   src_pitch;      /* source pitch in bytes */
    uint64   pixels;         /* mapped buffer address */
    uint32   handle;         /* optional BO handle; overrides pixels/pitch */
    uint32   flags;          /* reserved, must be 0 */
};

struct fb_gpu_bo_destroy {
    uint32   handle;
    uint32   flags;          /* reserved, must be 0 */
};

struct fb_gpu_bo_import {
    uint32   handle;         /* existing exported handle */
    uint32   flags;          /* reserved, must be 0 */
    uint32   width;          /* returned width in pixels */
    uint32   height;         /* returned height in pixels */
    uint32   pitch;          /* returned pitch in bytes */
    uint32   reserved;
    uint64   size;           /* returned mapping size */
    uint64   addr;           /* creator-local mapping if visible */
};

/* GPU copy command: screen-to-screen rectangle copy */
struct fb_gpu_copy {
    uint32 src_x;           /* source x */
    uint32 src_y;           /* source y */
    uint32 dst_x;           /* destination x */
    uint32 dst_y;           /* destination y */
    uint32 w;               /* width in pixels */
    uint32 h;               /* height in pixels */
};

/* Framebuffer/GPU counters for compositor validation and tuning. */
struct fb_gpu_stats {
    uint64 full_blits;         /* blits covering the whole visible screen */
    uint64 partial_blits;      /* clipped or unclipped sub-rectangle blits */
    uint64 clipped_blits;      /* blit/fill/copy requests clipped to screen */
    uint64 rejected_blits;     /* invalid blit requests rejected with errno */
    uint64 fill_rects;         /* accepted fill-rect operations */
    uint64 copy_rects;         /* accepted screen-to-screen copies */
    uint64 blit_bytes;         /* user pixel bytes copied into the LFB */
    uint64 bo_allocs;          /* graphics buffer create requests */
    uint64 bo_bytes;           /* total graphics buffer bytes mapped */
    uint64 bo_presents;        /* graphics buffer present requests */
    uint64 bo_handles;         /* currently tracked graphics buffer handles */
    uint64 bo_imports;         /* graphics buffer handle import/query requests */
    uint64 virtio_commands;    /* virtio-gpu control commands completed */
    uint64 virtio_failures;    /* virtio-gpu commands rejected or failed */
    uint64 virtio_timeouts;    /* virtio-gpu commands timed out */
    uint64 virtio_resources;   /* currently tracked virtio-gpu resources */
    uint64 virtio_resource_bytes; /* bytes backing tracked resources */
    uint64 virtio_transfers;   /* transfer-to-host commands completed */
    uint64 virtio_flushes;     /* resource-flush commands completed */
    uint64 virtio_scanouts;    /* set-scanout commands completed */
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
void fb_panic_screen(const char *text);

#endif /* __KERNEL_DEV_FB_H */
