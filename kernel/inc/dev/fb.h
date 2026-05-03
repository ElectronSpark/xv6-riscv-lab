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
#define FB_GPU_BO_FENCE      0x4618   /* query/wait BO present fences */
#define FB_GPU_VIRGL_CTX_CREATE  0x4619 /* create a virtio-gpu 3D context */
#define FB_GPU_VIRGL_CTX_DESTROY 0x461A /* destroy a virtio-gpu 3D context */
#define FB_GPU_VIRGL_SUBMIT      0x461B /* submit virgl command dwords */
#define FB_GPU_VIRGL_FENCE       0x461C /* query/wait virtio-gpu fences */
#define FB_GPU_VIRGL_GET_CAPS    0x461D /* query selected virgl capset payload */
#define FB_GPU_VIRGL_RESOURCE_CREATE 0x461E /* create/map a virgl resource */
#define FB_GPU_VIRGL_RESOURCE_DESTROY 0x461F /* destroy a virgl resource */
#define FB_GPU_VIRGL_TRANSFER_TO_HOST 0x4620 /* upload mapped resource backing */
#define FB_GPU_VIRGL_TRANSFER_FROM_HOST 0x4621 /* download resource backing */
#define FB_GPU_BO_EXPORT_FD  0x4622   /* export a BO as a file descriptor */
#define FB_GPU_BO_IMPORT_FD  0x4623   /* map a BO from an exported fd */
#define FB_GPU_FENCE_EXPORT_FD 0x4624 /* export a BO fence as an fd */
#define FB_GPU_FENCE_QUERY     0x4625 /* query/wait an exported fence fd */
#define FB_GPU_VIRGL_FENCE_EXPORT_FD 0x4626 /* export a virgl fence as an fd */
#define FB_GPU_VIRGL_FENCE_QUERY_FD  0x4627 /* query/wait a virgl fence fd */
#define FB_GPU_VIRGL_RESOURCE_EXPORT_FD 0x4628 /* export a virgl resource as a BO fd */

#define FB_GPU_BO_F_EXPORTABLE 0x1    /* return a stable kernel handle */
#define FB_GPU_BO_FENCE_WAIT 0x1      /* wait_for must be signaled */
#define FB_GPU_FENCE_WAIT 0x1         /* wait for fence fd to signal */
#define FB_GPU_VIRGL_FENCE_WAIT 0x1   /* wait_for must be signaled */
#define FB_GPU_VIRGL_SUBMIT_FORCE_FAIL 0x80000000u /* test-only context fault */

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

/* Allocate a kernel-tracked graphics buffer and map it into the caller.
 *
 * Userspace fills in width/height, then the kernel returns addr, pitch, and
 * size.  Release the mapping with normal munmap(addr, size).  If
 * FB_GPU_BO_F_EXPORTABLE is set, handle can be imported by another process;
 * each import creates a caller-local mapping of the same BO pages.
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

/* Present a mapped graphics buffer to the framebuffer.
 *
 * With handle != 0, pixels is a byte offset inside the BO.  With handle == 0,
 * pixels is a userspace pointer and src_pitch must be supplied by the caller.
 */
struct fb_gpu_bo_present {
    uint32   x;              /* destination x */
    uint32   y;              /* destination y */
    uint32   w;              /* width in pixels */
    uint32   h;              /* height in pixels */
    uint32   src_pitch;      /* source pitch in bytes */
    uint64   pixels;         /* mapped buffer address */
    uint32   handle;         /* optional BO handle; overrides pixels/pitch */
    uint32   flags;          /* reserved, must be 0 */
    uint64   fence;          /* returned completed fence for handle presents */
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
    uint64   addr;           /* returned caller-local mapping */
};

struct fb_gpu_bo_export_fd {
    uint32   handle;         /* existing exported handle */
    uint32   flags;          /* reserved, must be 0 */
    int32    fd;             /* returned fd-like BO capability */
    uint32   reserved;
};

struct fb_gpu_bo_import_fd {
    int32    fd;             /* BO capability returned by EXPORT_FD */
    uint32   flags;          /* reserved, must be 0 */
    uint32   width;          /* returned width in pixels */
    uint32   height;         /* returned height in pixels */
    uint32   pitch;          /* returned pitch in bytes */
    uint32   handle;         /* returned caller-local imported BO handle */
    uint64   size;           /* returned mapping size */
    uint64   addr;           /* returned caller-local mapping */
};

struct fb_gpu_bo_fence {
    uint32   handle;         /* existing exported handle */
    uint32   flags;          /* FB_GPU_BO_FENCE_* */
    uint64   wait_for;       /* 0 means the BO's latest present fence */
    uint64   signaled;       /* returned latest completed fence */
    uint64   last_present;   /* returned latest issued present fence */
};

struct fb_gpu_fence_export_fd {
    uint32   handle;         /* BO handle whose fence is exported */
    uint32   flags;          /* reserved, must be 0 */
    uint64   fence;          /* input target fence; 0 means latest */
    int32    fd;             /* returned fence capability fd */
    uint32   reserved;
    uint64   signaled;       /* returned latest completed fence */
};

struct fb_gpu_fence_query {
    int32    fd;             /* fence capability returned by EXPORT_FD */
    uint32   flags;          /* FB_GPU_FENCE_* */
    uint64   fence;          /* returned target fence */
    uint64   signaled;       /* returned latest completed fence */
};

struct fb_gpu_virgl_ctx {
    uint32   ctx_id;          /* returned/input context id */
    uint32   flags;           /* reserved, must be 0 */
    char     debug_name[64];  /* optional create-time debug name */
};

struct fb_gpu_virgl_submit {
    uint32   ctx_id;          /* context returned by CTX_CREATE */
    uint32   flags;           /* FB_GPU_VIRGL_SUBMIT_* */
    uint32   cmd_size;        /* command bytes at cmd, max 256 KiB */
    uint32   reserved;
    uint64   cmd;             /* user pointer to uint32 command dwords */
    uint64   fence;           /* returned submitted fence id */
    uint64   signaled;        /* returned latest completed fence id */
};

struct fb_gpu_virgl_fence {
    uint32   flags;           /* FB_GPU_VIRGL_FENCE_* */
    uint32   reserved;
    uint64   wait_for;        /* 0 queries latest completed fence */
    uint64   signaled;        /* returned latest completed fence id */
};

struct fb_gpu_virgl_fence_export_fd {
    uint32   flags;           /* reserved, must be 0 */
    int32    fd;              /* returned fence capability fd */
    uint64   fence;           /* input target fence; 0 means latest completed */
    uint64   signaled;        /* returned latest completed fence id */
};

struct fb_gpu_virgl_fence_query_fd {
    int32    fd;              /* fence capability returned by EXPORT_FD */
    uint32   flags;           /* FB_GPU_VIRGL_FENCE_WAIT */
    uint64   fence;           /* returned target fence */
    uint64   signaled;        /* returned latest completed fence id */
};

struct fb_gpu_virgl_caps {
    uint32   flags;           /* reserved, must be 0 */
    uint32   capset_id;       /* returned selected capset id */
    uint32   capset_version;  /* returned selected capset version */
    uint32   size;            /* input buffer size, returned payload size */
    uint64   data;            /* optional user buffer for capset payload */
};

struct fb_gpu_virgl_resource_create {
    uint32   ctx_id;          /* optional context to attach the resource to */
    uint32   flags;           /* virtio resource flags */
    uint32   resource_id;     /* returned virtio resource id */
    uint32   target;          /* virgl/VirtIO texture target */
    uint32   format;          /* virgl/VirtIO format */
    uint32   bind;            /* virgl bind flags */
    uint32   width;
    uint32   height;
    uint32   depth;
    uint32   array_size;
    uint32   last_level;
    uint32   nr_samples;
    uint64   size;            /* backing size; 0 means width*height*4 */
    uint64   addr;            /* returned caller-local mapping */
};

struct fb_gpu_virgl_resource_destroy {
    uint32   resource_id;
    uint32   flags;           /* reserved, must be 0 */
};

struct fb_gpu_virgl_resource_export_fd {
    uint32   resource_id;
    uint32   flags;           /* reserved, must be 0 */
    int32    fd;              /* returned BO capability fd */
    uint32   handle;          /* returned transient BO handle */
    uint32   width;           /* returned width in pixels */
    uint32   height;          /* returned height in pixels */
    uint32   pitch;           /* returned pitch in bytes */
    uint32   reserved;
    uint64   size;            /* returned mapping size */
};

struct fb_gpu_virgl_transfer {
    uint32   resource_id;
    uint32   flags;           /* reserved, must be 0 */
    uint32   x;
    uint32   y;
    uint32   z;
    uint32   w;
    uint32   h;
    uint32   d;
    uint64   offset;
    uint32   level;
    uint32   stride;
    uint32   layer_stride;
    uint32   padding;
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
    uint64 bo_live_bytes;      /* bytes currently pinned by tracked BOs */
    uint64 bo_peak_handles;    /* high-water mark of tracked BO handles */
    uint64 bo_peak_bytes;      /* high-water mark of tracked BO bytes */
    uint64 bo_imports;         /* graphics buffer handle import/query requests */
    uint64 bo_fd_exports;      /* graphics buffer fd capability exports */
    uint64 bo_fd_imports;      /* graphics buffer fd capability imports */
    uint64 bo_fd_live;         /* currently open BO capability fds */
    uint64 bo_fd_peak;         /* high-water mark of open BO fds */
    uint64 bo_fences;          /* completed graphics buffer present fences */
    uint64 bo_fence_waits;     /* graphics buffer fence wait/query requests */
    uint64 fence_fd_exports;   /* BO fence fd capability exports */
    uint64 fence_fd_queries;   /* BO fence fd query/wait requests */
    uint64 fence_fd_live;      /* currently open fence capability fds */
    uint64 fence_fd_peak;      /* high-water mark of open fence fds */
    uint64 fence_fd_polls;     /* fence capability poll readiness checks */
    uint64 fence_fd_poll_ready; /* poll checks that reported signaled fences */
    uint64 gpu_opens;          /* /dev/gpu0 opens */
    uint64 gpu_live_opens;     /* currently open /dev/gpu0 handles */
    uint64 gpu_ioctls;         /* /dev/gpu0 render-device ioctl calls */
    uint64 virtio_commands;    /* virtio-gpu control commands completed */
    uint64 virtio_failures;    /* virtio-gpu commands rejected or failed */
    uint64 virtio_timeouts;    /* virtio-gpu commands timed out */
    uint64 virtio_resources;   /* currently tracked virtio-gpu resources */
    uint64 virtio_resource_bytes; /* bytes backing tracked resources */
    uint64 virtio_transfers;   /* transfer-to-host commands completed */
    uint64 virtio_flushes;     /* resource-flush commands completed */
    uint64 virtio_scanouts;    /* set-scanout commands completed */
    uint64 virtio_capsets;     /* advertised virtio-gpu capsets */
    uint64 virtio_virgl;       /* nonzero when a virgl capset is present */
    uint64 virtio_virgl_version; /* max version for the selected virgl capset */
    uint64 virtio_virgl_size;  /* max capset payload size */
    uint64 virtio_contexts;    /* completed 3D context create/destroy commands */
    uint64 virtio_context_failed; /* currently failed user 3D contexts */
    uint64 virtio_context_failures; /* user 3D contexts marked failed */
    uint64 virtio_submits;     /* completed 3D command submissions */
    uint64 virtio_fences;      /* completed virtio-gpu fence submissions */
    uint64 virtio_last_fence;  /* last completed virtio-gpu fence id */
    uint64 virtio_irq_completions; /* queue completions observed by IRQ */
    uint64 virtio_poll_fallbacks;  /* queue waits that fell back to polling */
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
#define GPU_MAJOR 30
#define GPU_MINOR  0
#define DRM_PRIMARY_MAJOR 226
#define DRM_PRIMARY_MINOR   0
#define DRM_RENDER_MAJOR 226
#define DRM_RENDER_MINOR 128

/* Kernel API */
void fbdevinit(void);
int  fb_gpu_register_virgl_render_node(void);
int  fb_detected(void);
void fb_pci_init(uint8 bus, uint8 dev, uint8 func);
void fb_get_resolution(uint32 *xres, uint32 *yres);
void fb_gpu_destroy_owner(pid_t owner_tgid);
void fb_panic_screen(const char *text);

#endif /* __KERNEL_DEV_FB_H */
