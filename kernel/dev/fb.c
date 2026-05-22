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
#include <dev/fdt.h>
#include <dev/pci.h>
#include <timer/timer.h>
#include <mm/page.h>
#include <mm/pgtable.h>
#include <mm/memlayout.h>
#include <mm/rmap.h>
#include <mm/vm.h>
#include <proc/thread.h>
#include <printf.h>
#include <lock/spinlock.h>
#include <cmdline.h>
#include <vfs/file.h>
#include <vfs/poll.h>
#include <vfs/vfs_types.h>
#include <uabi/drm.h>

#if defined(__x86_64__) || defined(__i386__)

#define FB_GPU_MAX_BOS 128
#define FB_GPU_MAX_KMS_FBS 32
#define FB_GPU_MAX_SYNCOBJS 128
#define FB_GPU_MAX_SYNCOBJ_STATES 128
#define FB_GPU_MAX_DXG_PRESENT_SOURCES 32
#define FB_GPU_DXG_MISSING_PRESENT_BIND "dxg-resource-scanout-bind"
#define FB_GPU_DXG_MISSING_PRESENT_HELPER \
    "host-display-helper/resource-scanout-bind"
#define FB_GPU_DXG_WSL_PRESENTHISTORYTOKEN_CMD 34U
#define FB_GPU_DXG_WSL_SETREDIRECTEDFLIPFENCEVALUE_CMD 35U
#define FB_GPU_DXG_WSL_BLT_CMD 38U
#define FB_GPU_D3D12_HEAP_ALIGN (64ULL * 1024ULL)
#define FB_GPU_ALIGN_UP(x, a) ((((uint64)(x)) + ((uint64)(a) - 1)) & ~((uint64)(a) - 1))

#define GPU_DRM_CRTC_ID                       1
#define GPU_DRM_ENCODER_ID                    2
#define GPU_DRM_CONNECTOR_ID                  3
#define GPU_DRM_PRIMARY_PLANE_ID              4

#define FB_TTM_PL_SYSTEM                      0x0001
#define FB_TTM_PL_TT                          0x0002
#define FB_TTM_PL_VRAM                        0x0004
#define FB_TTM_PL_STOLEN                      0x0008
#define FB_TTM_MEM_SYSTEM                     0
#define FB_TTM_MEM_TT                         1
#define FB_TTM_MEM_VRAM                       2
#define FB_TTM_MEM_STOLEN                     3

#define GPU_DRM_MMAP_HANDLE_SHIFT 32
#define GPU_DRM_MMAP_OFFSET(handle) ((uint64)(handle) << GPU_DRM_MMAP_HANDLE_SHIFT)
#define GPU_DRM_MMAP_HANDLE(offset) ((uint32)((offset) >> GPU_DRM_MMAP_HANDLE_SHIFT))
#define GPU_DRM_MMAP_PAGE(offset) \
    (((offset) & ((1ULL << GPU_DRM_MMAP_HANDLE_SHIFT) - 1)) >> PGSHIFT)

struct fb_gpu_bo_entry {
    int in_use;
    int dead;
    uint32 handle;
    uint32 refs;
    uint64 owner_id;
    pid_t owner_tgid;
    uint32 width;
    uint32 height;
    uint32 pitch;
    uint32 npages;
    uint64 size;
    uint64 last_fence;
    uint64 signaled_fence;
    uint32 ttm_placement;
    uint32 ttm_mem_type;
    uint32 ttm_pin_count;
    uint32 ttm_reservation_seq;
    uint64 ttm_dma_addr_base;
    page_t **pages;
};

struct fb_gpu_kms_fb_entry {
    int in_use;
    uint32 fb_id;
    uint32 bo_handle;
    uint64 owner_id;
    pid_t owner_tgid;
    uint32 width;
    uint32 height;
    uint32 pitch;
    uint32 pixel_format;
    uint64 modifier;
};

struct fb_gpu_syncobj_entry {
    int in_use;
    uint32 handle;
    uint64 owner_id;
    pid_t owner_tgid;
    uint32 state_index;
};

struct fb_gpu_syncobj_state_entry {
    int in_use;
    uint32 refs;
    int signaled;
    uint64 timeline_value;
};

struct fb_gpu_dxg_present_source_entry {
    int in_use;
    uint32 handle;
    uint64 owner_id;
    pid_t owner_tgid;
    int32 dxg_fd;
    int32 resource_fd;
    uint32 device;
    uint32 resource;
    uint32 allocation;
    uint32 allocation_count;
    uint32 width;
    uint32 height;
    uint32 pitch;
    uint32 format;
    uint64 modifier;
    uint32 adapter_luid_low;
    uint32 adapter_luid_high;
    uint32 provenance_flags;
    uint32 adapter_identity;
};

struct fb_gpu_fence_file {
    struct fb_gpu_bo_entry *bo;
    uint64 fence;
};

struct fb_gpu_virgl_fence_file {
    uint64 fence;
};

struct fb_gpu_syncobj_file {
    uint32 state_index;
};

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
    int         detected;       /* non-zero if a fbdev-compatible scanout exists */
    int         virtio_backed;  /* shadow framebuffer presented via virtio-gpu */
    int         firmware_backed; /* bootloader/firmware linear framebuffer */
    uint64      fb_phys;        /* physical address of LFB (BAR0), if any */
    volatile uint8 *fb_virt;    /* kernel virtual address of framebuffer */
    int         scanout_mappable; /* scanout can be mapped into userspace */
    uint32      xres;
    uint32      yres;
    uint32      bpp;
    uint32      pitch;          /* bytes per scanline */
    uint32      fb_size;        /* total framebuffer size in bytes */
    struct fb_gpu_stats stats;
    uint32      next_bo_handle;
    uint32      next_kms_fb_id;
    uint32      next_syncobj_handle;
    uint32      next_dxg_present_source;
    uint32      next_drm_magic;
    uint64      next_bo_fence;
    uint64      next_render_owner_id;
    uint64      drm_master_owner_id;
    struct fb_gpu_bo_entry bos[FB_GPU_MAX_BOS];
    struct fb_gpu_kms_fb_entry kms_fbs[FB_GPU_MAX_KMS_FBS];
    struct fb_gpu_syncobj_entry syncobjs[FB_GPU_MAX_SYNCOBJS];
    struct fb_gpu_syncobj_state_entry syncobj_states[FB_GPU_MAX_SYNCOBJ_STATES];
    uint32      current_kms_fb_id;
    struct fb_gpu_dxg_present_source_entry dxg_present_sources[FB_GPU_MAX_DXG_PRESENT_SOURCES];
    spinlock_t  lock;           /* serializes concurrent access */
} fb_state = {
    .next_bo_handle = 1,
    .next_kms_fb_id = 100,
    .next_syncobj_handle = 1,
    .next_dxg_present_source = 1,
    .next_drm_magic = 1,
    .next_bo_fence = 1,
    .next_render_owner_id = 1,
    .lock = SPINLOCK_INITIALIZED("fb"),
};

static int fb_kernel_range_has_pages(uint64 base, uint64 size);
static void gpu_backend_fill(struct fb_gpu_backend_info *info);
static int gpu_backend_query(uint64 arg);
static uint64 fb_note_display_complete_locked(void);

static uint32 fb_pack_rgb(uint8 r, uint8 g, uint8 b)
{
    if (fb_state.firmware_backed && platform.has_framebuffer) {
        return ((uint32)r << platform.framebuffer_red_pos) |
               ((uint32)g << platform.framebuffer_green_pos) |
               ((uint32)b << platform.framebuffer_blue_pos);
    }

    return ((uint32)r << 16) | ((uint32)g << 8) | b;
}

static void fb_fill_rect(uint32 x, uint32 y, uint32 w, uint32 h, uint32 color)
{
    if (fb_state.fb_virt == NULL || fb_state.bpp != 32 ||
        x >= fb_state.xres || y >= fb_state.yres)
        return;

    if (x + w > fb_state.xres)
        w = fb_state.xres - x;
    if (y + h > fb_state.yres)
        h = fb_state.yres - y;

    for (uint32 row = 0; row < h; row++) {
        volatile uint32 *dst = (volatile uint32 *)
            (fb_state.fb_virt + (uint64)(y + row) * fb_state.pitch +
             (uint64)x * sizeof(uint32));
        for (uint32 col = 0; col < w; col++)
            dst[col] = color;
    }
}

static void fb_draw_boot_glyph(uint32 x, uint32 y, uint32 scale,
                               const uint8 glyph[7], uint32 color)
{
    for (uint32 row = 0; row < 7; row++) {
        for (uint32 col = 0; col < 5; col++) {
            if (glyph[row] & (1U << (4 - col)))
                fb_fill_rect(x + col * scale, y + row * scale,
                             scale, scale, color);
        }
    }
}

static const uint8 *fb_boot_glyph_for(char ch)
{
    static const uint8 glyph_space[7] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8 glyph_6[7] = {
        0x0f, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e,
    };
    static const uint8 glyph_d[7] = {
        0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e,
    };
    static const uint8 glyph_e[7] = {
        0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f,
    };
    static const uint8 glyph_k[7] = {
        0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11,
    };
    static const uint8 glyph_o[7] = {
        0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e,
    };
    static const uint8 glyph_p[7] = {
        0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10,
    };
    static const uint8 glyph_s[7] = {
        0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e,
    };
    static const uint8 glyph_t[7] = {
        0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    };
    static const uint8 glyph_v[7] = {
        0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04,
    };
    static const uint8 glyph_x[7] = {
        0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11,
    };

    switch (ch) {
    case '6': return glyph_6;
    case 'D':
    case 'd': return glyph_d;
    case 'E':
    case 'e': return glyph_e;
    case 'K':
    case 'k': return glyph_k;
    case 'O':
    case 'o': return glyph_o;
    case 'P':
    case 'p': return glyph_p;
    case 'S':
    case 's': return glyph_s;
    case 'T':
    case 't': return glyph_t;
    case 'V':
    case 'v': return glyph_v;
    case 'X':
    case 'x': return glyph_x;
    default: return glyph_space;
    }
}

static uint32 fb_boot_text_width(const char *text, uint32 scale, uint32 gap)
{
    uint32 width = 0;

    for (const char *p = text; *p; p++) {
        if (p != text)
            width += gap;
        width += (*p == ' ') ? (3 * scale) : (5 * scale);
    }

    return width;
}

static void fb_draw_boot_text(uint32 x, uint32 y, uint32 scale,
                              const char *text, uint32 color)
{
    uint32 gap = scale;

    for (const char *p = text; *p; p++) {
        if (*p == ' ') {
            x += 3 * scale + gap;
            continue;
        }
        fb_draw_boot_glyph(x, y, scale, fb_boot_glyph_for(*p), color);
        x += 5 * scale + gap;
    }
}

static void fb_paint_boot_logo(void)
{
    const char *title = "XV6 Desktop";

    if (fb_state.fb_virt == NULL || fb_state.bpp != 32 ||
        fb_state.xres == 0 || fb_state.yres == 0)
        return;

    uint32 bg = fb_pack_rgb(10, 22, 38);
    for (uint32 y = 0; y < fb_state.yres; y++) {
        volatile uint32 *row = (volatile uint32 *)
            (fb_state.fb_virt + (uint64)y * fb_state.pitch);
        for (uint32 x = 0; x < fb_state.xres; x++)
            row[x] = bg;
    }

    uint32 scale = fb_state.xres / 80;
    if (scale < 8)
        scale = 8;
    if (scale > 18)
        scale = 18;

    uint32 gap = scale;
    uint32 logo_w = fb_boot_text_width(title, scale, gap);
    uint32 logo_h = 7 * scale;
    uint32 x = fb_state.xres > logo_w ? (fb_state.xres - logo_w) / 2 : 16;
    uint32 y = fb_state.yres > logo_h ? (fb_state.yres - logo_h) / 2 : 16;

    fb_fill_rect(x - scale, y - scale, logo_w + 2 * scale,
                 logo_h + 2 * scale, fb_pack_rgb(15, 35, 58));
    fb_draw_boot_text(x, y, scale, title, fb_pack_rgb(116, 198, 255));
    fb_fill_rect(x - scale, y + logo_h + scale / 2,
                 logo_w + 2 * scale, scale / 2 + 1,
                 fb_pack_rgb(86, 230, 170));
    __sync_synchronize();
}

static cdev_t gpu_cdev;

enum fb_gpu_drm_node_type {
    FB_GPU_DRM_NODE_LEGACY = 0,
    FB_GPU_DRM_NODE_PRIMARY,
    FB_GPU_DRM_NODE_RENDER,
};

struct fb_gpu_drm_device {
    const char *driver_name;
    const char *desc;
    uint32 driver_major;
    uint32 driver_minor;
    uint32 driver_patchlevel;
};

struct fb_gpu_drm_file {
    struct fb_gpu_drm_device *dev;
    enum fb_gpu_drm_node_type node_type;
    uint32 magic;
    uint64 client_caps;
    uint64 ioctl_count;
    int authenticated;
    int is_master;
};

static struct fb_gpu_drm_device fb_drm_device = {
    .driver_name = "xv6_gpu",
    .desc = "xv6 DRM compatibility GPU facade",
    .driver_major = 0,
    .driver_minor = 1,
    .driver_patchlevel = 0,
};

struct fb_gpu_render_owner;
static enum fb_gpu_drm_node_type gpu_drm_node_from_cdev(cdev_t *cdev);
static const char *gpu_drm_node_name(enum fb_gpu_drm_node_type type);
static int gpu_drm_is_primary_like(struct fb_gpu_render_owner *owner);
static struct pci_device_info *gpu_nouveau_device(void);

struct fb_gpu_render_owner {
    uint64 id;
    pid_t tgid;
    struct fb_gpu_drm_file drm;
    uint32 default_ctx_id;
    uint32 capset_id;
    int nouveau_channel;
    int nouveau_vm_initialized;
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

static int fb_gpu_trace_enabled(void)
{
    return fb_cmdline_enabled("webkit_gpu_trace");
}

static int fb_gpu_trace_process(void)
{
    return current != NULL &&
        (strncmp(current->name, "MiniBrowser", 11) == 0 ||
         strncmp(current->name, "WebKit", 6) == 0 ||
         strncmp(current->name, "wlcomp", 6) == 0);
}

static const char *fb_gpu_ioctl_name(uint64 cmd)
{
    switch (cmd) {
    case FBIOGET_VSCREENINFO: return "FBIOGET_VSCREENINFO";
    case FBIOGET_FSCREENINFO: return "FBIOGET_FSCREENINFO";
    case FBIOPUT_VSCREENINFO: return "FBIOPUT_VSCREENINFO";
    case FB_GPU_GET_STATS: return "FB_GPU_GET_STATS";
    case FB_GPU_BO_CREATE: return "FB_GPU_BO_CREATE";
    case FB_GPU_BO_DESTROY: return "FB_GPU_BO_DESTROY";
    case FB_GPU_BO_IMPORT: return "FB_GPU_BO_IMPORT";
    case FB_GPU_BO_EXPORT_FD: return "FB_GPU_BO_EXPORT_FD";
    case FB_GPU_BO_IMPORT_FD: return "FB_GPU_BO_IMPORT_FD";
    case FB_GPU_BO_INFO: return "FB_GPU_BO_INFO";
    case FB_GPU_DXG_PRESENT_SOURCE_REGISTER: return "FB_GPU_DXG_PRESENT_SOURCE_REGISTER";
    case FB_GPU_DXG_PRESENT_SOURCE_COMMIT: return "FB_GPU_DXG_PRESENT_SOURCE_COMMIT";
    case FB_GPU_DXG_PRESENT_SOURCE_QUERY: return "FB_GPU_DXG_PRESENT_SOURCE_QUERY";
    case FB_GPU_BO_FENCE: return "FB_GPU_BO_FENCE";
    case FB_GPU_FENCE_EXPORT_FD: return "FB_GPU_FENCE_EXPORT_FD";
    case FB_GPU_FENCE_QUERY: return "FB_GPU_FENCE_QUERY";
    case FB_GPU_VIRGL_CTX_CREATE: return "FB_GPU_VIRGL_CTX_CREATE";
    case FB_GPU_VIRGL_CTX_DESTROY: return "FB_GPU_VIRGL_CTX_DESTROY";
    case FB_GPU_VIRGL_SUBMIT: return "FB_GPU_VIRGL_SUBMIT";
    case FB_GPU_VIRGL_FENCE: return "FB_GPU_VIRGL_FENCE";
    case FB_GPU_VIRGL_FENCE_EXPORT_FD: return "FB_GPU_VIRGL_FENCE_EXPORT_FD";
    case FB_GPU_VIRGL_FENCE_QUERY_FD: return "FB_GPU_VIRGL_FENCE_QUERY_FD";
    case FB_GPU_VIRGL_GET_CAPS: return "FB_GPU_VIRGL_GET_CAPS";
    case FB_GPU_VIRGL_RESOURCE_CREATE: return "FB_GPU_VIRGL_RESOURCE_CREATE";
    case FB_GPU_VIRGL_RESOURCE_DESTROY: return "FB_GPU_VIRGL_RESOURCE_DESTROY";
    case FB_GPU_VIRGL_RESOURCE_EXPORT_FD: return "FB_GPU_VIRGL_RESOURCE_EXPORT_FD";
    case FB_GPU_VIRGL_TRANSFER_TO_HOST: return "FB_GPU_VIRGL_TRANSFER_TO_HOST";
    case FB_GPU_VIRGL_TRANSFER_FROM_HOST: return "FB_GPU_VIRGL_TRANSFER_FROM_HOST";
    case FB_GPU_SCANOUT_MAP: return "FB_GPU_SCANOUT_MAP";
    case FB_GPU_SCANOUT_FLUSH: return "FB_GPU_SCANOUT_FLUSH";
    case FB_GPU_DISPLAY_PROBE: return "FB_GPU_DISPLAY_PROBE";
    case FB_GPU_BACKEND_QUERY: return "FB_GPU_BACKEND_QUERY";
    case FB_GPU_DISPLAY_WAIT: return "FB_GPU_DISPLAY_WAIT";
    case DRM_IOCTL_VERSION: return "DRM_IOCTL_VERSION";
    case DRM_IOCTL_GET_UNIQUE: return "DRM_IOCTL_GET_UNIQUE";
    case DRM_IOCTL_GET_MAGIC: return "DRM_IOCTL_GET_MAGIC";
    case DRM_IOCTL_GET_CLIENT: return "DRM_IOCTL_GET_CLIENT";
    case DRM_IOCTL_GET_STATS: return "DRM_IOCTL_GET_STATS";
    case DRM_IOCTL_SET_VERSION: return "DRM_IOCTL_SET_VERSION";
    case DRM_IOCTL_GET_CAP: return "DRM_IOCTL_GET_CAP";
    case DRM_IOCTL_SET_CLIENT_CAP: return "DRM_IOCTL_SET_CLIENT_CAP";
    case DRM_IOCTL_AUTH_MAGIC: return "DRM_IOCTL_AUTH_MAGIC";
    case DRM_IOCTL_SET_MASTER: return "DRM_IOCTL_SET_MASTER";
    case DRM_IOCTL_DROP_MASTER: return "DRM_IOCTL_DROP_MASTER";
    case DRM_IOCTL_GEM_CLOSE: return "DRM_IOCTL_GEM_CLOSE";
    case DRM_IOCTL_PRIME_HANDLE_TO_FD: return "DRM_IOCTL_PRIME_HANDLE_TO_FD";
    case DRM_IOCTL_PRIME_FD_TO_HANDLE: return "DRM_IOCTL_PRIME_FD_TO_HANDLE";
    case DRM_IOCTL_WAIT_VBLANK: return "DRM_IOCTL_WAIT_VBLANK";
    case DRM_IOCTL_MODE_GETRESOURCES: return "DRM_IOCTL_MODE_GETRESOURCES";
    case DRM_IOCTL_MODE_GETCRTC: return "DRM_IOCTL_MODE_GETCRTC";
    case DRM_IOCTL_MODE_SETCRTC: return "DRM_IOCTL_MODE_SETCRTC";
    case DRM_IOCTL_MODE_GETENCODER: return "DRM_IOCTL_MODE_GETENCODER";
    case DRM_IOCTL_MODE_GETCONNECTOR: return "DRM_IOCTL_MODE_GETCONNECTOR";
    case DRM_IOCTL_MODE_GETPROPERTY: return "DRM_IOCTL_MODE_GETPROPERTY";
    case DRM_IOCTL_MODE_GETPROPBLOB: return "DRM_IOCTL_MODE_GETPROPBLOB";
    case DRM_IOCTL_MODE_RMFB: return "DRM_IOCTL_MODE_RMFB";
    case DRM_IOCTL_MODE_PAGE_FLIP: return "DRM_IOCTL_MODE_PAGE_FLIP";
    case DRM_IOCTL_MODE_CREATE_DUMB: return "DRM_IOCTL_MODE_CREATE_DUMB";
    case DRM_IOCTL_MODE_MAP_DUMB: return "DRM_IOCTL_MODE_MAP_DUMB";
    case DRM_IOCTL_MODE_DESTROY_DUMB: return "DRM_IOCTL_MODE_DESTROY_DUMB";
    case DRM_IOCTL_MODE_GETPLANERESOURCES: return "DRM_IOCTL_MODE_GETPLANERESOURCES";
    case DRM_IOCTL_MODE_GETPLANE: return "DRM_IOCTL_MODE_GETPLANE";
    case DRM_IOCTL_MODE_ADDFB2: return "DRM_IOCTL_MODE_ADDFB2";
    case DRM_IOCTL_MODE_ATOMIC: return "DRM_IOCTL_MODE_ATOMIC";
    case DRM_IOCTL_SYNCOBJ_CREATE: return "DRM_IOCTL_SYNCOBJ_CREATE";
    case DRM_IOCTL_SYNCOBJ_DESTROY: return "DRM_IOCTL_SYNCOBJ_DESTROY";
    case DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD: return "DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD";
    case DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE: return "DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE";
    case DRM_IOCTL_SYNCOBJ_WAIT: return "DRM_IOCTL_SYNCOBJ_WAIT";
    case DRM_IOCTL_SYNCOBJ_RESET: return "DRM_IOCTL_SYNCOBJ_RESET";
    case DRM_IOCTL_SYNCOBJ_SIGNAL: return "DRM_IOCTL_SYNCOBJ_SIGNAL";
    case DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT: return "DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT";
    case DRM_IOCTL_SYNCOBJ_QUERY: return "DRM_IOCTL_SYNCOBJ_QUERY";
    case DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL: return "DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL";
    case DRM_IOCTL_VIRTGPU_MAP: return "DRM_IOCTL_VIRTGPU_MAP";
    case DRM_IOCTL_VIRTGPU_EXECBUFFER: return "DRM_IOCTL_VIRTGPU_EXECBUFFER";
    case DRM_IOCTL_VIRTGPU_GETPARAM: return "DRM_IOCTL_VIRTGPU_GETPARAM";
    case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE: return "DRM_IOCTL_VIRTGPU_RESOURCE_CREATE";
    case DRM_IOCTL_VIRTGPU_RESOURCE_INFO: return "DRM_IOCTL_VIRTGPU_RESOURCE_INFO";
    case DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST: return "DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST";
    case DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST: return "DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST";
    case DRM_IOCTL_VIRTGPU_WAIT: return "DRM_IOCTL_VIRTGPU_WAIT";
    case DRM_IOCTL_VIRTGPU_GET_CAPS: return "DRM_IOCTL_VIRTGPU_GET_CAPS";
    case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB: return "DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB";
    case DRM_IOCTL_VIRTGPU_CONTEXT_INIT: return "DRM_IOCTL_VIRTGPU_CONTEXT_INIT";
    case DRM_IOCTL_NOUVEAU_GETPARAM: return "DRM_IOCTL_NOUVEAU_GETPARAM";
    case DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC: return "DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC";
    case DRM_IOCTL_NOUVEAU_CHANNEL_FREE: return "DRM_IOCTL_NOUVEAU_CHANNEL_FREE";
    case DRM_IOCTL_NOUVEAU_VM_INIT: return "DRM_IOCTL_NOUVEAU_VM_INIT";
    case DRM_IOCTL_NOUVEAU_VM_BIND: return "DRM_IOCTL_NOUVEAU_VM_BIND";
    case DRM_IOCTL_NOUVEAU_EXEC: return "DRM_IOCTL_NOUVEAU_EXEC";
    case DRM_IOCTL_NOUVEAU_GEM_NEW: return "DRM_IOCTL_NOUVEAU_GEM_NEW";
    case DRM_IOCTL_NOUVEAU_GEM_PUSHBUF: return "DRM_IOCTL_NOUVEAU_GEM_PUSHBUF";
    case DRM_IOCTL_NOUVEAU_GEM_CPU_PREP: return "DRM_IOCTL_NOUVEAU_GEM_CPU_PREP";
    case DRM_IOCTL_NOUVEAU_GEM_CPU_FINI: return "DRM_IOCTL_NOUVEAU_GEM_CPU_FINI";
    case DRM_IOCTL_NOUVEAU_GEM_INFO: return "DRM_IOCTL_NOUVEAU_GEM_INFO";
    default: return "?";
    }
}

static int fb_blit_from_user(struct fb_gpu_blit cmd, int count_present)
{
    uint32 xres, yres;
    uint32 pitch;
    bool virtio_backed;
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
    pitch = fb_state.pitch;
    virtio_backed = fb_state.virtio_backed;
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

    volatile uint8 *fb = fb_state.fb_virt;
    uint64 row_bytes = (uint64)cw * 4;

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

    /*
     * fb_virt is PA2VA-mapped (cached RAM, both Hyper-V synthvid and BGA),
     * so no volatile/lock/bounce-buffer is required to write pixels.  Copy
     * each row in a single either_copyin from user shadow buffer directly
     * into the framebuffer row.  This eliminates the per-pixel volatile
     * loop and per-chunk spin_lock that previously made non-virtio
     * (Hyper-V) blits cost hundreds of ms per frame.
     */
    (void)virtio_backed;
    for (uint32 row = 0; row < ch; row++) {
        uint64 src_addr = cmd.pixels + (uint64)row * cmd.src_pitch;
        void *dst = (void *)(fb + (uint64)(cmd.y + row) * pitch +
                             (uint64)cmd.x * 4);

        if (either_copyin(dst, 1, src_addr, row_bytes) < 0)
            return -EFAULT;
    }
    if (virtio_backed)
        virtio_gpu_present_fb_rect(fb_state.fb_virt, fb_state.pitch,
                                   cmd.x, cmd.y, cw, ch);
    hyperv_video_dirty(cmd.x, cmd.y, cw, ch);
    spin_lock(&fb_state.lock);
    fb_note_display_complete_locked();
    spin_unlock(&fb_state.lock);
    return 0;
}

static uint64 fb_bo_signal_present_locked(struct fb_gpu_bo_entry *bo)
{
    uint64 fence = fb_state.next_bo_fence++;

    if (fb_state.next_bo_fence == 0)
        fb_state.next_bo_fence = 1;
    bo->last_fence = fence;
    bo->signaled_fence = fence;
    fb_state.stats.bo_fences++;
    return fence;
}

static uint64 fb_note_display_complete_locked(void)
{
    uint64 seq = ++fb_state.stats.display_last_present;

    if (seq == 0)
        seq = fb_state.stats.display_last_present = 1;
    fb_state.stats.display_presents++;
    fb_state.stats.display_last_complete = seq;
    fb_state.stats.display_completions++;
    return seq;
}

static int fb_blit_from_bo(struct fb_gpu_bo_entry *bo,
                           struct fb_gpu_bo_present cmd,
                           uint64 *fence_out)
{
    uint32 xres, yres;
    uint32 pitch;
    bool virtio_backed;
    uint32 cw, ch;
    uint64 offset;
    uint64 last;
    int clipped = 0;

    if (bo == NULL)
        return -EINVAL;
    if (cmd.w == 0)
        cmd.w = bo->width;
    if (cmd.h == 0)
        cmd.h = bo->height;
    if (cmd.w == 0 || cmd.h == 0)
        return 0;
    if (cmd.w > 0xffffffffU / 4)
        goto reject;

    offset = cmd.pixels;
    last = offset + (uint64)(cmd.h - 1) * bo->pitch + (uint64)cmd.w * 4;
    if (offset >= bo->size || last > bo->size || last < offset)
        goto reject;

    spin_lock(&fb_state.lock);
    xres = fb_state.xres;
    yres = fb_state.yres;
    pitch = fb_state.pitch;
    virtio_backed = fb_state.virtio_backed;
    spin_unlock(&fb_state.lock);

    if (cmd.x >= xres || cmd.y >= yres)
        return 0;

    cw = cmd.w;
    ch = cmd.h;
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
    spin_lock(&fb_state.lock);
    if (cmd.x == 0 && cmd.y == 0 && cw == xres && ch == yres)
        fb_state.stats.full_blits++;
    else
        fb_state.stats.partial_blits++;
    if (clipped)
        fb_state.stats.clipped_blits++;
    fb_state.stats.blit_bytes += (uint64)cw * ch * 4;
    fb_state.stats.bo_presents++;
    spin_unlock(&fb_state.lock);

    for (uint32 row = 0; row < ch; row++) {
        uint64 src_off = offset + (uint64)row * bo->pitch;
        volatile uint8 *dst = fb_state.fb_virt +
                              ((uint64)(cmd.y + row) * pitch) +
                              (uint64)cmd.x * 4;
        uint32 remaining = cw * 4;
        uint32 copied = 0;

        while (remaining > 0) {
            uint32 page_idx = src_off / PGSIZE;
            uint32 page_off = src_off & (PGSIZE - 1);
            uint32 chunk = PGSIZE - page_off;
            uint64 pa;
            uint8 *src;

            if (page_idx >= bo->npages)
                goto reject;
            if (chunk > remaining)
                chunk = remaining;
            pa = __page_to_pa(bo->pages[page_idx]);
            src = (uint8 *)PA2VA(pa) + page_off;

            if (virtio_backed) {
                memcpy((void *)(dst + copied), src, chunk);
            } else {
                /*
                 * fb_virt is PA2VA-mapped cached RAM (Hyper-V synthvid and
                 * BGA both).  No volatile/lock required — plain memcpy is
                 * orders of magnitude faster than the per-pixel volatile
                 * loop and per-chunk spin_lock that previously stalled the
                 * Hyper-V compositor blit path.
                 */
                memcpy((void *)(dst + copied), src, chunk);
            }

            src_off += chunk;
            copied += chunk;
            remaining -= chunk;
        }
    }
    if (virtio_backed)
        virtio_gpu_present_fb_rect(fb_state.fb_virt, pitch,
                                   cmd.x, cmd.y, cw, ch);
    hyperv_video_dirty(cmd.x, cmd.y, cw, ch);
    spin_lock(&fb_state.lock);
    fb_note_display_complete_locked();
    if (fence_out)
        *fence_out = fb_bo_signal_present_locked(bo);
    spin_unlock(&fb_state.lock);
    return 0;

reject:
    spin_lock(&fb_state.lock);
    fb_state.stats.rejected_blits++;
    spin_unlock(&fb_state.lock);
    return -EINVAL;
}

static int fb_map_scanout_current(struct fb_gpu_scanout_map *req)
{
    vm_t *vm;
    vma_t *vma;
    uint64 addr;
    uint64 flags;
    uint64 pte_flags;
    uint64 fb_phys;
    uint64 fb_virt;
    uint64 map_size;
    uint32 xres, yres, pitch, fb_size;
    int mappable;
    int pfnmap;

    if (req == NULL || current == NULL || current->vm == NULL)
        return -EINVAL;

    spin_lock(&fb_state.lock);
    mappable = fb_state.detected && fb_state.scanout_mappable &&
               fb_state.fb_virt != NULL;
    fb_phys = fb_state.fb_phys;
    fb_virt = (uint64)(uintptr_t)fb_state.fb_virt;
    xres = fb_state.xres;
    yres = fb_state.yres;
    pitch = fb_state.pitch;
    fb_size = fb_state.fb_size;
    spin_unlock(&fb_state.lock);

    if (!mappable || fb_size == 0)
        return -ENODEV;

    map_size = PGROUNDUP((uint64)fb_size);
    if (map_size == 0)
        return -EINVAL;

    vm = current->vm;
    pfnmap = fb_phys != 0;
    flags = PROT_READ | PROT_WRITE | VMA_FLAG_USER |
            VMA_FLAG_DONTFORK | VMA_FLAG_DONTDUMP |
            (pfnmap ? VMA_FLAG_PFNMAP : 0);

    vm_wlock(vm);
    addr = vm_find_free_range(vm,
                              (size_t)(map_size + FB_GPU_D3D12_HEAP_ALIGN),
                              0);
    if (addr == 0) {
        printf("FB: scanout map failed no free user range size=0x%lx\n",
               map_size);
        vm_wunlock(vm);
        return -ENOMEM;
    }
    addr = FB_GPU_ALIGN_UP(addr, FB_GPU_D3D12_HEAP_ALIGN);

    vma = vma_alloc(vm, addr, map_size, flags);
    if (vma == NULL) {
        printf("FB: scanout map vma_alloc failed addr=0x%lx size=0x%lx flags=0x%lx\n",
               addr, map_size, flags);
        vm_wunlock(vm);
        return -ENOMEM;
    }
    if (!pfnmap && anon_vma_prepare(vma) != 0) {
        printf("FB: scanout map anon_vma_prepare failed addr=0x%lx size=0x%lx\n",
               addr, map_size);
        vma_free(vm, vma);
        vm_wunlock(vm);
        return -ENOMEM;
    }

    pte_flags = vma2pte_flags(flags);
    /*
     * Firmware framebuffers used by Hyper-V are MMIO-like PFN ranges.  The
     * kernel's direct map marks that range uncached, but this user mapping is
     * built by hand, so carry the same cache policy here.  Without this,
     * wlcomp can write through a cached alias while Hyper-V keeps scanning out
     * the old boot-logo contents.
     */
#ifdef PTE_PWT
    if (pfnmap)
        pte_flags |= PTE_PWT;
#endif
    for (uint64 off = 0; off < map_size; off += PGSIZE) {
        uint64 va = addr + off;
        uint64 pa;
        page_t *page;

        if (fb_phys != 0) {
            pa = fb_phys + off;
        } else {
            uint64 kva = fb_virt + off;
            pte_t *kpte = NULL;

            if (kva >= (uint64)PA2VA(KERNBASE) &&
                kva < (uint64)PA2VA(PHYSTOP)) {
                pa = VA2PA(kva);
            } else if (kernel_vm != NULL && kernel_vm->pagetable != NULL) {
                kpte = walk(kernel_vm->pagetable, kva, 0, NULL, NULL);
                if (kpte == NULL || !pte_present(kpte)) {
                    printf("FB: scanout map kernel walk failed kva=0x%lx off=0x%lx\n",
                           kva, off);
                    vma_free(vm, vma);
                    vm_wunlock(vm);
                    return -ENODEV;
                }
                pa = pte_pa(kpte) + (kva & (PGSIZE - 1));
            } else {
                vma_free(vm, vma);
                vm_wunlock(vm);
                return -ENODEV;
            }
        }

        if (!pfnmap) {
            page = __pa_to_page(pa);
            if (page == NULL) {
                printf("FB: scanout map unmanaged pa=0x%lx off=0x%lx\n",
                       pa, off);
                vma_free(vm, vma);
                vm_wunlock(vm);
                return -ENODEV;
            }
            if (page_ref_inc((void *)pa) <= 0) {
                printf("FB: scanout map page_ref_inc failed pa=0x%lx off=0x%lx\n",
                       pa, off);
                vma_free(vm, vma);
                vm_wunlock(vm);
                return -ENOMEM;
            }
        } else {
            page = NULL;
        }
        if (mappages(vm->pagetable, va, PGSIZE, pa, pte_flags) != 0) {
            printf("FB: scanout map mappages failed va=0x%lx pa=0x%lx off=0x%lx pte_flags=0x%lx pfnmap=%d\n",
                   va, pa, off, pte_flags, pfnmap);
            if (!pfnmap)
                page_ref_dec((void *)pa);
            vma_free(vm, vma);
            vm_wunlock(vm);
            return -ENOMEM;
        }
        if (!pfnmap)
            page_add_anon_rmap(page, vma, va);
    }
    vm_wunlock(vm);

    memset(req, 0, sizeof(*req));
    req->width = xres;
    req->height = yres;
    req->pitch = pitch;
    req->size = map_size;
    req->addr = addr;
    return 0;
}

static int fb_flush_scanout_rect(struct fb_gpu_scanout_flush req)
{
    uint32 xres, yres;
    uint32 w, h;
    int virtio_backed;

    spin_lock(&fb_state.lock);
    if (!fb_state.detected || !fb_state.scanout_mappable ||
        fb_state.fb_virt == NULL) {
        spin_unlock(&fb_state.lock);
        return -ENODEV;
    }
    virtio_backed = fb_state.virtio_backed;
    xres = fb_state.xres;
    yres = fb_state.yres;
    spin_unlock(&fb_state.lock);

    if (req.w == 0 || req.h == 0)
        return 0;
    if (req.x >= xres || req.y >= yres)
        return 0;

    w = req.w;
    h = req.h;
    if ((uint64)req.x + w > xres)
        w = xres - req.x;
    if ((uint64)req.y + h > yres)
        h = yres - req.y;
    if (w == 0 || h == 0)
        return 0;

    spin_lock(&fb_state.lock);
    if (req.x == 0 && req.y == 0 && w == xres && h == yres)
        fb_state.stats.full_blits++;
    else
        fb_state.stats.partial_blits++;
    fb_state.stats.blit_bytes += (uint64)w * h * 4;
    spin_unlock(&fb_state.lock);

    if (virtio_backed)
        virtio_gpu_present_fb_rect(fb_state.fb_virt, fb_state.pitch,
                                   req.x, req.y, w, h);
    hyperv_video_dirty(req.x, req.y, w, h);
    spin_lock(&fb_state.lock);
    fb_note_display_complete_locked();
    spin_unlock(&fb_state.lock);
    return 0;
}

static struct fb_gpu_bo_entry *fb_bo_lookup_locked(uint32 handle)
{
    if (handle == 0)
        return NULL;
    for (int i = 0; i < FB_GPU_MAX_BOS; i++) {
        if (fb_state.bos[i].in_use && !fb_state.bos[i].dead &&
            fb_state.bos[i].handle == handle)
            return &fb_state.bos[i];
    }
    return NULL;
}

static int fb_bo_owner_matches(const struct fb_gpu_bo_entry *bo,
                               uint64 owner_id, pid_t owner_tgid)
{
    if (bo == NULL)
        return 0;
    if (owner_id != 0)
        return bo->owner_id == owner_id;
    if (owner_tgid > 0 && bo->owner_id != 0)
        return bo->owner_tgid == owner_tgid;
    return 1;
}

static void fb_bo_release_pages(page_t **pages, uint32 npages)
{
    if (pages == NULL)
        return;
    for (uint32 i = 0; i < npages; i++) {
        if (pages[i] != NULL)
            page_ref_dec((void *)__page_to_pa(pages[i]));
    }
    kvfree(pages);
}

static uint32 fb_ttm_mem_type_for_placement(uint32 placement)
{
    if ((placement & FB_TTM_PL_VRAM) != 0)
        return FB_TTM_MEM_VRAM;
    if ((placement & FB_TTM_PL_TT) != 0)
        return FB_TTM_MEM_TT;
    if ((placement & FB_TTM_PL_STOLEN) != 0)
        return FB_TTM_MEM_STOLEN;
    return FB_TTM_MEM_SYSTEM;
}

static void fb_ttm_account_locked(uint32 mem_type, uint64 size, int add)
{
    uint64 *counter;

    switch (mem_type) {
    case FB_TTM_MEM_TT:
        counter = &fb_state.stats.ttm_tt_bytes;
        break;
    case FB_TTM_MEM_VRAM:
        counter = &fb_state.stats.ttm_vram_bytes;
        break;
    case FB_TTM_MEM_STOLEN:
        counter = &fb_state.stats.ttm_stolen_bytes;
        break;
    default:
        counter = &fb_state.stats.ttm_system_bytes;
        break;
    }
    if (add) {
        *counter += size;
    } else if (*counter >= size) {
        *counter -= size;
    } else {
        *counter = 0;
    }
}

static int fb_ttm_validate_placement_locked(struct fb_gpu_bo_entry *bo,
                                            uint32 placement)
{
    uint32 mem_type;

    if (bo == NULL || !bo->in_use ||
        (placement & ~(FB_TTM_PL_SYSTEM | FB_TTM_PL_TT |
                       FB_TTM_PL_VRAM | FB_TTM_PL_STOLEN)) != 0 ||
        placement == 0) {
        fb_state.stats.ttm_validate_failures++;
        return -EINVAL;
    }

    mem_type = fb_ttm_mem_type_for_placement(placement);
    if (mem_type != bo->ttm_mem_type) {
        fb_ttm_account_locked(bo->ttm_mem_type, bo->size, 0);
        fb_ttm_account_locked(mem_type, bo->size, 1);
        bo->ttm_mem_type = mem_type;
    }
    bo->ttm_placement = placement;
    bo->ttm_reservation_seq++;
    bo->ttm_dma_addr_base =
        bo->pages != NULL && bo->pages[0] != NULL ?
            __page_to_pa(bo->pages[0]) : 0;
    return 0;
}

static int fb_bo_set_ttm_placement(uint32 handle, uint64 owner_id,
                                   pid_t owner_tgid, uint32 placement)
{
    struct fb_gpu_bo_entry *bo;
    int ret;

    spin_lock(&fb_state.lock);
    bo = fb_bo_lookup_locked(handle);
    if (bo == NULL) {
        fb_state.stats.ttm_validate_failures++;
        spin_unlock(&fb_state.lock);
        return -ENOENT;
    }
    if (!fb_bo_owner_matches(bo, owner_id, owner_tgid)) {
        fb_state.stats.ttm_validate_failures++;
        spin_unlock(&fb_state.lock);
        return -EPERM;
    }
    ret = fb_ttm_validate_placement_locked(bo, placement);
    spin_unlock(&fb_state.lock);
    return ret;
}

static void fb_bo_put(struct fb_gpu_bo_entry *bo)
{
    page_t **pages = NULL;
    uint32 npages = 0;

    if (bo == NULL)
        return;

    spin_lock(&fb_state.lock);
    if (bo->refs > 0)
        bo->refs--;
    if (bo->dead && bo->refs == 0) {
        pages = bo->pages;
        npages = bo->npages;
        memset(bo, 0, sizeof(*bo));
    }
    spin_unlock(&fb_state.lock);

    fb_bo_release_pages(pages, npages);
}

static struct fb_gpu_bo_entry *fb_bo_get_owned(uint32 handle, uint64 owner_id,
                                               pid_t owner_tgid)
{
    struct fb_gpu_bo_entry *bo;

    spin_lock(&fb_state.lock);
    bo = fb_bo_lookup_locked(handle);
    if (bo != NULL && !fb_bo_owner_matches(bo, owner_id, owner_tgid))
        bo = NULL;
    if (bo != NULL)
        bo->refs++;
    spin_unlock(&fb_state.lock);
    return bo;
}

static struct fb_gpu_bo_entry *fb_bo_get(uint32 handle)
{
    return fb_bo_get_owned(handle, 0, 0);
}

static void *fb_bo_page_for_owner(uint32 handle, uint64 owner_id,
                                  pid_t owner_tgid, uint64 page_index)
{
    struct fb_gpu_bo_entry *bo;
    void *pa = NULL;

    spin_lock(&fb_state.lock);
    bo = fb_bo_lookup_locked(handle);
    if (bo != NULL && fb_bo_owner_matches(bo, owner_id, owner_tgid) &&
        page_index < bo->npages && bo->pages[page_index] != NULL) {
        pa = (void *)__page_to_pa(bo->pages[page_index]);
        if (page_ref_inc(pa) <= 0)
            pa = NULL;
    }
    spin_unlock(&fb_state.lock);
    return pa;
}

static int fb_bo_alloc_pages(uint32 npages, page_t ***pages_out)
{
    page_t **pages;

    if (npages == 0 || pages_out == NULL)
        return -EINVAL;
    pages = kvmalloc((size_t)npages * sizeof(*pages));
    if (pages == NULL)
        return -ENOMEM;
    memset(pages, 0, (size_t)npages * sizeof(*pages));

    for (uint32 i = 0; i < npages; i++) {
        pages[i] = __page_alloc(0, PAGE_TYPE_ANON);
        if (pages[i] == NULL) {
            fb_bo_release_pages(pages, npages);
            return -ENOMEM;
        }
        memset((void *)PA2VA(__page_to_pa(pages[i])), 0, PGSIZE);
    }

    *pages_out = pages;
    return 0;
}

static int fb_bo_clone_pages(struct fb_gpu_bo_entry *bo, page_t ***pages_out)
{
    page_t **pages;
    uint32 i;

    if (bo == NULL || bo->npages == 0 || bo->pages == NULL ||
        pages_out == NULL)
        return -EINVAL;

    pages = kvmalloc((size_t)bo->npages * sizeof(*pages));
    if (pages == NULL)
        return -ENOMEM;
    memset(pages, 0, (size_t)bo->npages * sizeof(*pages));

    for (i = 0; i < bo->npages; i++) {
        uint64 pa;

        if (bo->pages[i] == NULL)
            goto fail;
        pa = __page_to_pa(bo->pages[i]);
        if (page_ref_inc((void *)pa) <= 0)
            goto fail;
        pages[i] = bo->pages[i];
    }

    *pages_out = pages;
    return 0;

fail:
    while (i > 0) {
        i--;
        if (pages[i] != NULL)
            page_ref_dec((void *)__page_to_pa(pages[i]));
    }
    kvfree(pages);
    return -ENOMEM;
}

static int fb_bo_register(uint64 owner_id, pid_t owner_tgid,
                          uint32 width, uint32 height, uint32 pitch,
                          uint64 size, page_t **pages, uint32 npages,
                          uint32 *handle)
{
    spin_lock(&fb_state.lock);
    struct fb_gpu_bo_entry *bo = NULL;
    for (int i = 0; i < FB_GPU_MAX_BOS; i++) {
        if (!fb_state.bos[i].in_use) {
            bo = &fb_state.bos[i];
            break;
        }
    }
    if (bo == NULL) {
        fb_state.stats.rejected_blits++;
        spin_unlock(&fb_state.lock);
        return -ENOSPC;
    }

    uint32 next = fb_state.next_bo_handle++;
    if (fb_state.next_bo_handle == 0)
        fb_state.next_bo_handle = 1;
    memset(bo, 0, sizeof(*bo));
    bo->in_use = 1;
    bo->handle = next;
    bo->owner_id = owner_id;
    bo->owner_tgid = owner_tgid;
    bo->width = width;
    bo->height = height;
    bo->pitch = pitch;
    bo->npages = npages;
    bo->size = size;
    bo->pages = pages;
    bo->refs = 1;
    bo->ttm_placement = FB_TTM_PL_SYSTEM;
    bo->ttm_mem_type = FB_TTM_MEM_SYSTEM;
    bo->ttm_pin_count = 0;
    bo->ttm_reservation_seq = 1;
    bo->ttm_dma_addr_base =
        pages != NULL && npages != 0 && pages[0] != NULL ?
            __page_to_pa(pages[0]) : 0;
    fb_state.stats.bo_handles++;
    fb_state.stats.bo_live_bytes += size;
    fb_ttm_account_locked(bo->ttm_mem_type, size, 1);
    if (fb_state.stats.bo_handles > fb_state.stats.bo_peak_handles)
        fb_state.stats.bo_peak_handles = fb_state.stats.bo_handles;
    if (fb_state.stats.bo_live_bytes > fb_state.stats.bo_peak_bytes)
        fb_state.stats.bo_peak_bytes = fb_state.stats.bo_live_bytes;
    *handle = next;
    spin_unlock(&fb_state.lock);
    return 0;
}

static int fb_dxg_present_source_owner_matches(
    const struct fb_gpu_dxg_present_source_entry *source, uint64 owner_id,
    pid_t owner_tgid)
{
    if (source == NULL)
        return 0;
    if (owner_id != 0)
        return source->owner_id == owner_id;
    if (owner_tgid > 0 && source->owner_id != 0)
        return source->owner_tgid == owner_tgid;
    return 1;
}

static struct fb_gpu_dxg_present_source_entry *
fb_dxg_present_source_lookup_locked(uint32 handle)
{
    if (handle == 0)
        return NULL;
    for (int i = 0; i < FB_GPU_MAX_DXG_PRESENT_SOURCES; i++) {
        if (fb_state.dxg_present_sources[i].in_use &&
            fb_state.dxg_present_sources[i].handle == handle)
            return &fb_state.dxg_present_sources[i];
    }
    return NULL;
}

static void
fb_dxg_present_note_source_shape_locked(
    const struct fb_gpu_dxg_present_source_entry *source)
{
    if (source == NULL)
        return;
    fb_state.stats.dxg_present_last_width = source->width;
    fb_state.stats.dxg_present_last_height = source->height;
    fb_state.stats.dxg_present_last_pitch = source->pitch;
    fb_state.stats.dxg_present_last_format = source->format;
    fb_state.stats.dxg_present_last_allocation_count =
        source->allocation_count;
    fb_state.stats.dxg_present_last_dxg_fd =
        source->dxg_fd < 0 ? 0xffffffffULL : (uint64)(uint32)source->dxg_fd;
    fb_state.stats.dxg_present_last_resource_fd =
        source->resource_fd < 0 ?
        0xffffffffULL : (uint64)(uint32)source->resource_fd;
    fb_state.stats.dxg_present_last_provenance = source->provenance_flags;
    fb_state.stats.dxg_present_last_adapter_luid_low =
        source->adapter_luid_low;
    fb_state.stats.dxg_present_last_adapter_luid_high =
        source->adapter_luid_high;
    fb_state.stats.dxg_present_last_adapter_identity =
        source->adapter_identity;
}

static uint64 fb_dxg_present_required_metadata(uint32 flags)
{
    uint64 metadata = FB_GPU_DXG_PRESENT_META_DEVICE |
                      FB_GPU_DXG_PRESENT_META_RESOURCE |
                      FB_GPU_DXG_PRESENT_META_ALLOCATION |
                      FB_GPU_DXG_PRESENT_META_DIMENSIONS |
                      FB_GPU_DXG_PRESENT_META_FORMAT |
                      FB_GPU_DXG_PRESENT_META_MODIFIER |
                      FB_GPU_DXG_PRESENT_META_ADAPTER_LUID;

    if ((flags & FB_GPU_DXG_PRESENT_F_WAIT_SYNC) != 0)
        metadata |= FB_GPU_DXG_PRESENT_META_SYNC_OBJECT |
                    FB_GPU_DXG_PRESENT_META_FENCE_VALUE;
    return metadata;
}

static uint64 fb_dxg_present_fd_diag(int32 fd)
{
    return fd < 0 ? 0xffffffffULL : (uint64)(uint32)fd;
}

static void fb_dxg_present_hex32(char out[9], uint32 value)
{
    static const char hex[] = "0123456789abcdef";

    for (int i = 7; i >= 0; i--) {
        out[i] = hex[value & 0xfU];
        value >>= 4;
    }
    out[8] = '\0';
}

static uint32 fb_dxg_present_adapter_identity(uint32 luid_low,
                                              uint32 luid_high)
{
    if (luid_low == 0 && luid_high == 0)
        return FB_GPU_DXG_PRESENT_ADAPTER_UNKNOWN;
    return FB_GPU_DXG_PRESENT_ADAPTER_UNVERIFIED;
}

static uint32 fb_dxg_present_provenance_mask(void)
{
    return FB_GPU_DXG_PRESENT_PROV_DXG_FD |
           FB_GPU_DXG_PRESENT_PROV_RESOURCE_FD |
           FB_GPU_DXG_PRESENT_PROV_D3DKMT_HANDLES |
           FB_GPU_DXG_PRESENT_PROV_DIMENSIONS |
           FB_GPU_DXG_PRESENT_PROV_ADAPTER_LUID;
}

static void fb_dxg_present_sanitize_register_tail(
    struct fb_gpu_dxg_present_source_register *req)
{
    uint32 mask = fb_dxg_present_provenance_mask();

    if (req == NULL)
        return;
    /*
     * The fields after present_source are passive diagnostics.  If an older
     * caller uses the stable register prefix, the extra copyin bytes can be
     * unrelated stack data.  Treat unknown provenance bits or reserved tail
     * bytes as absent optional metadata, while keeping flags strictly checked
     * at their stable prefix offset.
     */
    if (req->reserved1 != 0 || (req->provenance_flags & ~mask) != 0) {
        req->adapter_luid_low = 0;
        req->adapter_luid_high = 0;
        req->provenance_flags = 0;
        req->reserved1 = 0;
    } else {
        req->provenance_flags &= mask;
    }
}

static uint32 fb_dxg_present_source_provenance(
    const struct fb_gpu_dxg_present_source_register *req)
{
    uint32 provenance = 0;

    if (req == NULL)
        return 0;
    if (req->dxg_fd >= 0)
        provenance |= FB_GPU_DXG_PRESENT_PROV_DXG_FD;
    if (req->resource_fd >= 0)
        provenance |= FB_GPU_DXG_PRESENT_PROV_RESOURCE_FD;
    if (req->device != 0 && req->resource != 0 && req->allocation != 0)
        provenance |= FB_GPU_DXG_PRESENT_PROV_D3DKMT_HANDLES;
    if (req->width != 0 && req->height != 0 && req->pitch != 0 &&
        req->format != 0)
        provenance |= FB_GPU_DXG_PRESENT_PROV_DIMENSIONS;
    if (req->adapter_luid_low != 0 || req->adapter_luid_high != 0)
        provenance |= FB_GPU_DXG_PRESENT_PROV_ADAPTER_LUID;
    provenance |= req->provenance_flags & fb_dxg_present_provenance_mask();
    return provenance;
}

static uint64 fb_dxg_present_helper_block_reason_locked(void)
{
    uint64 reason = FB_GPU_DXG_PRESENT_BLOCK_NO_TRANSPORT;

    if ((fb_state.stats.dxg_present_host_rejects &
         FB_GPU_DXG_PRESENT_REJECT_SYNTHVID_GPA_ONLY) != 0)
        reason |= FB_GPU_DXG_PRESENT_BLOCK_SYNTHVID_GPA_ONLY;
    if ((fb_state.stats.dxg_present_host_rejects &
         FB_GPU_DXG_PRESENT_REJECT_DXG_NO_DISPLAY_BIND) != 0)
        reason |= FB_GPU_DXG_PRESENT_BLOCK_DXG_NO_DISPLAY_BIND;
    if (fb_state.stats.dxg_present_last_adapter_identity !=
        FB_GPU_DXG_PRESENT_ADAPTER_MATCH)
        reason |= FB_GPU_DXG_PRESENT_BLOCK_LUID_UNVERIFIED;
    if (fb_state.stats.dxg_present_last_adapter_identity ==
        FB_GPU_DXG_PRESENT_ADAPTER_MISMATCH)
        reason |= FB_GPU_DXG_PRESENT_BLOCK_ADAPTER_MISMATCH;
    if (fb_state.stats.dxg_present_helper_transport_present == 0)
        reason |= FB_GPU_DXG_PRESENT_BLOCK_NO_TRANSPORT;
    if (fb_state.stats.dxg_present_helper_requires_completion != 0)
        reason |= FB_GPU_DXG_PRESENT_BLOCK_NO_COMPLETION;
    return reason;
}

static void fb_dxg_present_note_helper_contract_locked(
    const struct fb_gpu_dxg_present_source_entry *source, uint32 flags)
{
    uint64 lifetime = FB_GPU_DXG_PRESENT_LIFE_HANDLES_VALID |
                      FB_GPU_DXG_PRESENT_LIFE_HOST_COMPLETION |
                      FB_GPU_DXG_PRESENT_LIFE_NO_CPU_READBACK;

    if (source != NULL && source->in_use)
        lifetime |= FB_GPU_DXG_PRESENT_LIFE_SOURCE_REGISTERED;
    if ((flags & FB_GPU_DXG_PRESENT_F_WAIT_SYNC) != 0)
        lifetime |= FB_GPU_DXG_PRESENT_LIFE_SYNC_VALID;

    fb_state.stats.dxg_present_helper_contract_version = 1;
    fb_state.stats.dxg_present_helper_required_metadata =
        fb_dxg_present_required_metadata(flags);
    fb_state.stats.dxg_present_helper_transport =
        FB_GPU_DXG_PRESENT_HELPER_TRANSPORT_NONE;
    fb_state.stats.dxg_present_helper_transport_present = 0;
    fb_state.stats.dxg_present_helper_operation =
        FB_GPU_DXG_PRESENT_HELPER_OP_SCANOUT_BIND;
    fb_state.stats.dxg_present_helper_lifetime = lifetime;
    fb_state.stats.dxg_present_helper_source_live =
        source != NULL && source->in_use ? 1 : 0;
    fb_state.stats.dxg_present_helper_requires_completion = 1;
    fb_state.stats.dxg_present_selected_lane =
        FB_GPU_DXG_PRESENT_LANE_HELPER_SCANOUT_BIND;
    fb_state.stats.dxg_present_helper_block_reason =
        fb_dxg_present_helper_block_reason_locked();
}

static void
fb_dxg_present_note_host_lanes_locked(void)
{
    struct hyperv_video_status video;
    struct hyperv_dxg_status dxg;
    uint64 candidates = 0;
    uint64 rejects = 0;
    uint64 synthvid_state = 0;
    uint64 dxg_state = 0;

    /*
     * Linux hyperv_drm/hyperv_fb present through SYNTHVID_VRAM_LOCATION
     * followed by SYNTHVID_DIRT: the host consumes a guest physical VRAM
     * address, not a D3DKMT resource or allocation handle.  WSL dxgkrnl also
     * clears display_supported/indirect_display_device for QAI adapter type,
     * and exposes no D3DKMT display-bind ioctl.  A real native handoff needs a
     * new host ABI that binds {device, resource, allocation, sync/fence} to
     * scanout, or a documented DXG command with equivalent semantics.  The WSL
     * VMBus command enum names present-history, redirected-flip-fence, and BLT
     * commands, but this kernel has no source-backed packet/result contract
     * that converts a runtime D3D12 resource into synthvid scanout.
     */
    if (hyperv_video_get_status(&video) == 0 && video.present) {
        candidates |= FB_GPU_DXG_PRESENT_HOST_SYNTHVID;
        if (video.present)
            synthvid_state |= 0x1;
        if (video.gpadl_ok)
            synthvid_state |= 0x2;
        if (video.open_ok)
            synthvid_state |= 0x4;
        if (video.initialized)
            synthvid_state |= 0x8;
        if (video.dirt_needed)
            synthvid_state |= 0x10;
        fb_state.stats.dxg_present_synthvid_vram_gpa = video.vram_gpa;
        rejects |= FB_GPU_DXG_PRESENT_REJECT_SYNTHVID_GPA_ONLY;
    }
    if (hyperv_dxg_get_status(&dxg) == 0 &&
        (dxg.global_present || dxg.vgpu_present)) {
        candidates |= FB_GPU_DXG_PRESENT_HOST_DXG;
        if (dxg.global_present)
            dxg_state |= 0x1;
        if (dxg.global_open_ok)
            dxg_state |= 0x2;
        if (dxg.vgpu_present)
            dxg_state |= 0x4;
        if (dxg.vgpu_open_ok)
            dxg_state |= 0x8;
        if (hyperv_dxg_d3dkmt_ready())
            dxg_state |= 0x10;
        rejects |= FB_GPU_DXG_PRESENT_REJECT_DXG_NO_DISPLAY_BIND;
    }
    fb_state.stats.dxg_present_host_candidates = candidates;
    fb_state.stats.dxg_present_host_rejects = rejects;
    fb_state.stats.dxg_present_synthvid_state = synthvid_state;
    fb_state.stats.dxg_present_dxg_state = dxg_state;
    fb_state.stats.dxg_present_helper_block_reason =
        fb_dxg_present_helper_block_reason_locked();
}

static int fb_dxg_present_register(uint64 owner_id, pid_t owner_tgid,
                                   struct fb_gpu_dxg_present_source_register *req)
{
    struct fb_gpu_dxg_present_source_entry *source = NULL;
    int print_flags_reject = 0;
    uint32 print_flags = 0;
    uint32 print_device = 0;
    uint32 print_resource = 0;
    uint32 print_allocation = 0;
    uint32 print_width = 0;
    uint32 print_height = 0;
    uint32 print_pitch = 0;
    uint32 print_format = 0;
    uint64 print_dxg_fd = 0xffffffffULL;
    uint64 print_resource_fd = 0xffffffffULL;
    uint64 print_rejects = 0;
    int ret = 0;

    if (req == NULL)
        return -EINVAL;
    fb_dxg_present_sanitize_register_tail(req);
    spin_lock(&fb_state.lock);
    fb_state.stats.dxg_present_register_attempts++;
    fb_state.stats.dxg_present_last_ret = 0;
    fb_state.stats.dxg_present_last_device = req->device;
    fb_state.stats.dxg_present_last_resource = req->resource;
    fb_state.stats.dxg_present_last_allocation = req->allocation;
    fb_state.stats.dxg_present_last_sync = 0;
    fb_state.stats.dxg_present_last_flags = req->flags;
    fb_state.stats.dxg_present_last_fence_value = 0;
    fb_state.stats.dxg_present_last_width = req->width;
    fb_state.stats.dxg_present_last_height = req->height;
    fb_state.stats.dxg_present_last_pitch = req->pitch;
    fb_state.stats.dxg_present_last_format = req->format;
    fb_state.stats.dxg_present_last_allocation_count = req->allocation_count;
    fb_state.stats.dxg_present_last_dxg_fd =
        fb_dxg_present_fd_diag(req->dxg_fd);
    fb_state.stats.dxg_present_last_resource_fd =
        fb_dxg_present_fd_diag(req->resource_fd);
    fb_state.stats.dxg_present_last_provenance =
        fb_dxg_present_source_provenance(req);
    fb_state.stats.dxg_present_last_adapter_luid_low =
        req->adapter_luid_low;
    fb_state.stats.dxg_present_last_adapter_luid_high =
        req->adapter_luid_high;
    fb_state.stats.dxg_present_last_adapter_identity =
        fb_dxg_present_adapter_identity(req->adapter_luid_low,
                                        req->adapter_luid_high);
    fb_state.stats.dxg_present_selected_lane =
        FB_GPU_DXG_PRESENT_LANE_HELPER_SCANOUT_BIND;
    fb_state.stats.dxg_present_display_target_kind =
        FB_GPU_DXG_DISPLAY_TARGET_NONE;
    fb_state.stats.dxg_present_requires_host_protocol = 0;
    fb_state.stats.dxg_present_missing_host_abi =
        FB_GPU_DXG_PRESENT_MISSING_NONE;
    fb_state.stats.dxg_present_host_candidates = 0;
    fb_state.stats.dxg_present_host_rejects = 0;
    fb_state.stats.dxg_present_synthvid_state = 0;
    fb_state.stats.dxg_present_synthvid_vram_gpa = 0;
    fb_state.stats.dxg_present_dxg_state = 0;
    fb_state.stats.dxg_present_helper_block_reason =
        FB_GPU_DXG_PRESENT_BLOCK_NO_TRANSPORT |
        FB_GPU_DXG_PRESENT_BLOCK_LUID_UNVERIFIED;
    fb_dxg_present_note_helper_contract_locked(NULL, 0);

    if (req->flags != 0 || req->dxg_fd < 0 || req->resource_fd < -1 ||
        req->device == 0 || req->resource == 0 || req->allocation == 0 ||
        req->allocation_count == 0 || req->allocation_count > 1024 ||
        req->width == 0 || req->height == 0 ||
        req->width > 16384 || req->height > 16384 ||
        req->width > 0xffffffffU / 4U ||
        req->format == 0 || req->pitch == 0 ||
        req->pitch < req->width * 4U) {
        if (req->flags != 0) {
            print_flags_reject = 1;
            print_flags = req->flags;
            print_device = req->device;
            print_resource = req->resource;
            print_allocation = req->allocation;
            print_width = req->width;
            print_height = req->height;
            print_pitch = req->pitch;
            print_format = req->format;
            print_dxg_fd = fb_dxg_present_fd_diag(req->dxg_fd);
            print_resource_fd = fb_dxg_present_fd_diag(req->resource_fd);
        }
        ret = -EINVAL;
        goto out;
    }

    for (int i = 0; i < FB_GPU_MAX_DXG_PRESENT_SOURCES; i++) {
        if (!fb_state.dxg_present_sources[i].in_use) {
            source = &fb_state.dxg_present_sources[i];
            break;
        }
    }
    if (source == NULL) {
        ret = -ENOSPC;
        goto out;
    }

    req->present_source = fb_state.next_dxg_present_source++;
    if (fb_state.next_dxg_present_source == 0)
        fb_state.next_dxg_present_source = 1;
    memset(source, 0, sizeof(*source));
    source->in_use = 1;
    source->handle = req->present_source;
    source->owner_id = owner_id;
    source->owner_tgid = owner_tgid;
    source->dxg_fd = req->dxg_fd;
    source->resource_fd = req->resource_fd;
    source->device = req->device;
    source->resource = req->resource;
    source->allocation = req->allocation;
    source->allocation_count = req->allocation_count;
    source->width = req->width;
    source->height = req->height;
    source->pitch = req->pitch;
    source->format = req->format;
    source->modifier = req->modifier;
    source->adapter_luid_low = req->adapter_luid_low;
    source->adapter_luid_high = req->adapter_luid_high;
    source->provenance_flags = fb_dxg_present_source_provenance(req);
    source->adapter_identity =
        fb_dxg_present_adapter_identity(req->adapter_luid_low,
                                        req->adapter_luid_high);
    fb_state.stats.dxg_present_register_successes++;
    fb_state.stats.dxg_present_last_source = req->present_source;
    fb_dxg_present_note_source_shape_locked(source);
    fb_dxg_present_note_helper_contract_locked(source, 0);

out:
    if (ret != 0) {
        fb_state.stats.dxg_present_register_rejects++;
        fb_state.stats.dxg_present_last_ret = (uint64)(-ret);
        print_rejects = fb_state.stats.dxg_present_register_rejects;
    }
    spin_unlock(&fb_state.lock);
    if (print_flags_reject && print_rejects <= 8) {
        printf("fb: dxg present-source register rejected: "
               "reserved flags nonzero flags:0x%x errno:%d "
               "dxg_fd:0x%lx resource_fd:0x%lx dev:0x%x res:0x%x "
               "alloc:0x%x %ux%u pitch:%u fmt:0x%x "
               "abi_hint:register-prefix-append-only\n",
               print_flags, EINVAL, print_dxg_fd, print_resource_fd,
               print_device, print_resource, print_allocation, print_width,
               print_height, print_pitch, print_format);
    }
    return ret;
}

static void fb_dxg_present_source_unregister(uint32 handle)
{
    spin_lock(&fb_state.lock);
    struct fb_gpu_dxg_present_source_entry *source =
        fb_dxg_present_source_lookup_locked(handle);
    if (source != NULL)
        memset(source, 0, sizeof(*source));
    spin_unlock(&fb_state.lock);
}

static int fb_dxg_present_commit(uint64 owner_id, pid_t owner_tgid,
                                 const struct fb_gpu_dxg_present_source_commit *req)
{
    struct fb_gpu_dxg_present_source_entry *source;
    int print_missing = 0;
    uint32 print_source = 0;
    uint32 print_device = 0;
    uint32 print_resource = 0;
    uint32 print_allocation = 0;
    uint32 print_allocation_count = 0;
    uint32 print_sync = 0;
    uint64 print_fence = 0;
    uint32 print_width = 0;
    uint32 print_height = 0;
    uint32 print_pitch = 0;
    uint32 print_format = 0;
    uint64 print_candidates = 0;
    uint64 print_rejects = 0;
    uint64 print_synthvid_state = 0;
    uint64 print_synthvid_vram = 0;
    uint64 print_dxg_state = 0;
    uint64 print_helper_meta = 0;
    uint64 print_helper_lifetime = 0;
    uint64 print_helper_block = 0;
    uint64 print_provenance = 0;
    uint64 print_no_source = 0;
    uint64 print_bad_flags = 0;
    uint64 print_no_transport = 0;
    uint64 print_no_completion = 0;
    uint64 print_resource_fd_unverified = 0;
    uint64 print_adapter_mismatch = 0;
    uint64 print_commit_rejects = 0;
    uint32 print_lane = 0;
    uint32 print_adapter_identity = 0;
    uint64 print_luid_low = 0;
    uint64 print_luid_high = 0;
    char print_luid_low_hex[9];
    char print_luid_high_hex[9];
    uint64 print_dxg_fd = 0xffffffffULL;
    uint64 print_resource_fd = 0xffffffffULL;
    int ret = -EOPNOTSUPP;

    if (req == NULL)
        return -EINVAL;
    fb_dxg_present_hex32(print_luid_low_hex, 0);
    fb_dxg_present_hex32(print_luid_high_hex, 0);
    spin_lock(&fb_state.lock);
    fb_state.stats.dxg_present_commit_attempts++;
    fb_state.stats.dxg_present_last_source = req->present_source;
    fb_state.stats.dxg_present_last_sync = req->sync_object;
    fb_state.stats.dxg_present_last_flags = req->flags;
    fb_state.stats.dxg_present_last_fence_value = req->fence_value;
    fb_dxg_present_note_helper_contract_locked(NULL, req->flags);

    source = fb_dxg_present_source_lookup_locked(req->present_source);
    if (source == NULL ||
        !fb_dxg_present_source_owner_matches(source, owner_id, owner_tgid)) {
        fb_state.stats.dxg_present_commit_no_source++;
        fb_state.stats.dxg_present_helper_block_reason |=
            FB_GPU_DXG_PRESENT_BLOCK_NO_REGISTERED_SOURCE;
        ret = -ENOENT;
        goto out;
    }
    fb_state.stats.dxg_present_last_device = source->device;
    fb_state.stats.dxg_present_last_resource = source->resource;
    fb_state.stats.dxg_present_last_allocation = source->allocation;
    fb_dxg_present_note_source_shape_locked(source);
    fb_dxg_present_note_helper_contract_locked(source, req->flags);

    if ((req->flags & ~FB_GPU_DXG_PRESENT_F_WAIT_SYNC) != 0 ||
        ((req->flags & FB_GPU_DXG_PRESENT_F_WAIT_SYNC) != 0 &&
         req->sync_object == 0) ||
        ((req->flags & FB_GPU_DXG_PRESENT_F_WAIT_SYNC) == 0 &&
         req->sync_object != 0)) {
        fb_state.stats.dxg_present_commit_bad_flags++;
        ret = -EINVAL;
        goto out;
    }

    fb_state.stats.dxg_present_host_handoff_missing++;
    fb_state.stats.dxg_present_display_target_kind =
        FB_GPU_DXG_DISPLAY_TARGET_NONE;
    fb_state.stats.dxg_present_requires_host_protocol = 1;
    fb_state.stats.dxg_present_missing_host_abi =
        FB_GPU_DXG_PRESENT_MISSING_SCANOUT_BIND;
    fb_dxg_present_note_host_lanes_locked();
    if (source->resource_fd < 0) {
        fb_state.stats.dxg_present_commit_resource_fd_unverified++;
        fb_state.stats.dxg_present_helper_block_reason |=
            FB_GPU_DXG_PRESENT_BLOCK_RESOURCE_FD_UNVERIFIED;
    }
    if (source->adapter_identity == FB_GPU_DXG_PRESENT_ADAPTER_MISMATCH) {
        fb_state.stats.dxg_present_commit_adapter_mismatch++;
        fb_state.stats.dxg_present_helper_block_reason |=
            FB_GPU_DXG_PRESENT_BLOCK_ADAPTER_MISMATCH;
    }
    if (fb_state.stats.dxg_present_helper_transport_present == 0)
        fb_state.stats.dxg_present_commit_no_transport++;
    if (fb_state.stats.dxg_present_helper_requires_completion != 0)
        fb_state.stats.dxg_present_commit_no_completion++;
    if (fb_state.stats.dxg_present_host_handoff_missing <= 8) {
        print_missing = 1;
        print_source = source->handle;
        print_device = source->device;
        print_resource = source->resource;
        print_allocation = source->allocation;
        print_allocation_count = source->allocation_count;
        print_sync = req->sync_object;
        print_fence = req->fence_value;
        print_width = source->width;
        print_height = source->height;
        print_pitch = source->pitch;
        print_format = source->format;
        print_candidates = fb_state.stats.dxg_present_host_candidates;
        print_rejects = fb_state.stats.dxg_present_host_rejects;
        print_synthvid_state = fb_state.stats.dxg_present_synthvid_state;
        print_synthvid_vram = fb_state.stats.dxg_present_synthvid_vram_gpa;
        print_dxg_state = fb_state.stats.dxg_present_dxg_state;
        print_helper_meta =
            fb_state.stats.dxg_present_helper_required_metadata;
        print_helper_lifetime =
            fb_state.stats.dxg_present_helper_lifetime;
        print_helper_block =
            fb_state.stats.dxg_present_helper_block_reason;
        print_provenance = source->provenance_flags;
        print_no_source = fb_state.stats.dxg_present_commit_no_source;
        print_bad_flags = fb_state.stats.dxg_present_commit_bad_flags;
        print_no_transport =
            fb_state.stats.dxg_present_commit_no_transport;
        print_no_completion =
            fb_state.stats.dxg_present_commit_no_completion;
        print_resource_fd_unverified =
            fb_state.stats.dxg_present_commit_resource_fd_unverified;
        print_adapter_mismatch =
            fb_state.stats.dxg_present_commit_adapter_mismatch;
        print_lane = (uint32)fb_state.stats.dxg_present_selected_lane;
        print_adapter_identity = source->adapter_identity;
        print_luid_low = (uint64)source->adapter_luid_low;
        print_luid_high = (uint64)source->adapter_luid_high;
        print_dxg_fd = fb_dxg_present_fd_diag(source->dxg_fd);
        print_resource_fd = fb_dxg_present_fd_diag(source->resource_fd);
    }

out:
    fb_state.stats.dxg_present_commit_rejects++;
    fb_state.stats.dxg_present_last_ret = (uint64)(-ret);
    print_commit_rejects = fb_state.stats.dxg_present_commit_rejects;
    print_helper_block = fb_state.stats.dxg_present_helper_block_reason;
    print_no_source = fb_state.stats.dxg_present_commit_no_source;
    print_bad_flags = fb_state.stats.dxg_present_commit_bad_flags;
    spin_unlock(&fb_state.lock);
    fb_dxg_present_hex32(print_luid_high_hex,
                         (uint32)(print_luid_high & 0xffffffffULL));
    fb_dxg_present_hex32(print_luid_low_hex,
                         (uint32)(print_luid_low & 0xffffffffULL));
    if (!print_missing && print_commit_rejects <= 8) {
        printf("fb: dxg present-source commit rejected: "
               "source:%u flags:0x%x sync:0x%x fence:%lu errno:%d "
               "block_reason:0x%lx no_source:%lu bad_flags:%lu "
               "helper_transport_present:0 no_completion:1 "
               "present_id:0 completed:0\n",
               req->present_source, req->flags, req->sync_object,
               req->fence_value, -ret, print_helper_block,
               print_no_source, print_bad_flags);
    }
    if (print_missing) {
        printf("fb: dxg present-source commit blocked: missing "
               "host ABI=%s helper=%s source:%u "
               "dev:0x%x res:0x%x alloc:0x%x allocs:%u "
               "sync:0x%x fence:%lu %ux%u pitch:%u fmt:0x%x "
               "candidates:0x%lx rejects:0x%lx synthvid:0x%lx "
               "vram:0x%lx dxg:0x%lx "
               "dependency:bind-runtime-d3d12-resource-to-host-display "
               "helper_contract:v1 helper_transport:%u "
               "helper_op:%u vmbus_offer:0 hvsock_service:0 "
               "required_meta:0x%lx lifetime:0x%lx "
               "block_reason:0x%lx lane:%u provenance:0x%lx "
               "block_bits:no_transport=%u,synthvid_gpa=%u,dxg_no_bind=%u,"
               "luid_unverified=%u,no_source=%u,resource_fd_unverified=%u,"
               "adapter_mismatch=%u,no_completion=%u "
               "counters:no_source=%lu,bad_flags=%lu,no_transport=%lu,"
               "no_completion=%lu,resource_fd_unverified=%lu,"
               "adapter_mismatch=%lu "
               "dxg_fd:0x%lx resource_fd:0x%lx luid:%s:%s "
               "adapter_identity:%u "
               "synthvid:gpa-dirty-only dxg_display_bind:0 "
               "candidate_cmds:presenthistory=%u,redirected_flip_fence=%u,blt=%u "
               "present_id:0 completed:0\n",
               FB_GPU_DXG_MISSING_PRESENT_BIND,
               FB_GPU_DXG_MISSING_PRESENT_HELPER,
               print_source, print_device, print_resource,
               print_allocation, print_allocation_count, print_sync,
               print_fence, print_width, print_height, print_pitch,
               print_format, print_candidates, print_rejects,
               print_synthvid_state, print_synthvid_vram, print_dxg_state,
               FB_GPU_DXG_PRESENT_HELPER_TRANSPORT_NONE,
               FB_GPU_DXG_PRESENT_HELPER_OP_SCANOUT_BIND,
               print_helper_meta, print_helper_lifetime,
               print_helper_block, print_lane, print_provenance,
               (print_helper_block &
                FB_GPU_DXG_PRESENT_BLOCK_NO_TRANSPORT) != 0,
               (print_helper_block &
                FB_GPU_DXG_PRESENT_BLOCK_SYNTHVID_GPA_ONLY) != 0,
               (print_helper_block &
                FB_GPU_DXG_PRESENT_BLOCK_DXG_NO_DISPLAY_BIND) != 0,
               (print_helper_block &
                FB_GPU_DXG_PRESENT_BLOCK_LUID_UNVERIFIED) != 0,
               (print_helper_block &
                FB_GPU_DXG_PRESENT_BLOCK_NO_REGISTERED_SOURCE) != 0,
               (print_helper_block &
                FB_GPU_DXG_PRESENT_BLOCK_RESOURCE_FD_UNVERIFIED) != 0,
               (print_helper_block &
                FB_GPU_DXG_PRESENT_BLOCK_ADAPTER_MISMATCH) != 0,
               (print_helper_block &
                FB_GPU_DXG_PRESENT_BLOCK_NO_COMPLETION) != 0,
               print_no_source, print_bad_flags, print_no_transport,
               print_no_completion, print_resource_fd_unverified,
               print_adapter_mismatch,
               print_dxg_fd, print_resource_fd, print_luid_high_hex,
               print_luid_low_hex, print_adapter_identity,
               FB_GPU_DXG_WSL_PRESENTHISTORYTOKEN_CMD,
               FB_GPU_DXG_WSL_SETREDIRECTEDFLIPFENCEVALUE_CMD,
               FB_GPU_DXG_WSL_BLT_CMD);
    }
    return ret;
}

static int fb_dxg_present_query(uint64 owner_id, pid_t owner_tgid,
                                struct fb_gpu_dxg_present_source_query *req)
{
    struct fb_gpu_dxg_present_source_entry *source;
    uint32 source_handle;
    uint64 helper_meta;
    uint64 helper_lifetime;

    if (req == NULL)
        return -EINVAL;

    spin_lock(&fb_state.lock);
    fb_state.stats.dxg_present_query_attempts++;
    fb_state.stats.dxg_present_query_rejects++;
    fb_state.stats.dxg_present_last_ret = EOPNOTSUPP;

    source_handle = req->present_source != 0 ?
        req->present_source : (uint32)fb_state.stats.dxg_present_last_source;
    source = fb_dxg_present_source_lookup_locked(source_handle);
    if (source != NULL &&
        !fb_dxg_present_source_owner_matches(source, owner_id, owner_tgid))
        source = NULL;

    if (source != NULL) {
        fb_state.stats.dxg_present_last_source = source->handle;
        fb_state.stats.dxg_present_last_device = source->device;
        fb_state.stats.dxg_present_last_resource = source->resource;
        fb_state.stats.dxg_present_last_allocation = source->allocation;
        fb_dxg_present_note_source_shape_locked(source);
    }
    fb_dxg_present_note_helper_contract_locked(
        source, (uint32)fb_state.stats.dxg_present_last_flags);
    helper_meta = fb_state.stats.dxg_present_helper_required_metadata;
    helper_lifetime = fb_state.stats.dxg_present_helper_lifetime;

    memset(req, 0, sizeof(*req));
    req->present_source = source_handle;
    req->display_target_kind =
        (uint32)fb_state.stats.dxg_present_display_target_kind;
    req->source_live = source != NULL ? 1 : 0;
    req->present_id = 0;
    req->completed = 0;
    req->host_handoff_missing =
        fb_state.stats.dxg_present_host_handoff_missing;
    req->requires_host_protocol = 1;
    req->missing_host_abi = FB_GPU_DXG_PRESENT_MISSING_SCANOUT_BIND;
    req->helper_contract_version =
        fb_state.stats.dxg_present_helper_contract_version;
    req->helper_required_metadata = helper_meta;
    req->helper_transport = fb_state.stats.dxg_present_helper_transport;
    req->helper_transport_present =
        fb_state.stats.dxg_present_helper_transport_present;
    req->helper_operation = fb_state.stats.dxg_present_helper_operation;
    req->helper_lifetime = helper_lifetime;
    req->helper_requires_completion =
        fb_state.stats.dxg_present_helper_requires_completion;
    if (source != NULL) {
        req->device = source->device;
        req->resource = source->resource;
        req->allocation = source->allocation;
        req->allocation_count = source->allocation_count;
    } else {
        req->device = (uint32)fb_state.stats.dxg_present_last_device;
        req->resource = (uint32)fb_state.stats.dxg_present_last_resource;
        req->allocation = (uint32)fb_state.stats.dxg_present_last_allocation;
        req->allocation_count =
            (uint32)fb_state.stats.dxg_present_last_allocation_count;
    }
    req->sync_object = (uint32)fb_state.stats.dxg_present_last_sync;
    req->last_flags = (uint32)fb_state.stats.dxg_present_last_flags;
    req->fence_value = fb_state.stats.dxg_present_last_fence_value;
    req->last_ret = EOPNOTSUPP;
    if (source != NULL) {
        req->dxg_fd = source->dxg_fd;
        req->resource_fd = source->resource_fd;
        req->provenance_flags = source->provenance_flags;
        req->adapter_luid_low = source->adapter_luid_low;
        req->adapter_luid_high = source->adapter_luid_high;
        req->adapter_identity = source->adapter_identity;
    } else {
        req->dxg_fd = (int32)fb_state.stats.dxg_present_last_dxg_fd;
        req->resource_fd =
            (int32)fb_state.stats.dxg_present_last_resource_fd;
        req->provenance_flags =
            (uint32)fb_state.stats.dxg_present_last_provenance;
        req->adapter_luid_low =
            (uint32)fb_state.stats.dxg_present_last_adapter_luid_low;
        req->adapter_luid_high =
            (uint32)fb_state.stats.dxg_present_last_adapter_luid_high;
        req->adapter_identity =
            (uint32)fb_state.stats.dxg_present_last_adapter_identity;
    }
    req->selected_lane = (uint32)fb_state.stats.dxg_present_selected_lane;
    req->helper_block_reason =
        (uint32)fb_state.stats.dxg_present_helper_block_reason;
    req->host_candidates = fb_state.stats.dxg_present_host_candidates;
    req->host_rejects = fb_state.stats.dxg_present_host_rejects;
    spin_unlock(&fb_state.lock);

    return -EOPNOTSUPP;
}

static int fb_bo_destroy(uint32 handle);

static int fb_bo_fops_release(struct vfs_inode *inode, struct vfs_file *file)
{
    (void)inode;
    struct fb_gpu_bo_entry *bo =
        file ? (struct fb_gpu_bo_entry *)file->private_data : NULL;

    if (file != NULL)
        file->private_data = NULL;
    fb_bo_put(bo);
    spin_lock(&fb_state.lock);
    if (fb_state.stats.bo_fd_live > 0)
        fb_state.stats.bo_fd_live--;
    spin_unlock(&fb_state.lock);
    return 0;
}

static struct vfs_file_ops fb_bo_file_ops = {
    .release = fb_bo_fops_release,
};

static int fb_fence_fops_release(struct vfs_inode *inode,
                                 struct vfs_file *file)
{
    (void)inode;
    struct fb_gpu_fence_file *fence =
        file ? (struct fb_gpu_fence_file *)file->private_data : NULL;

    if (file != NULL)
        file->private_data = NULL;
    if (fence != NULL) {
        fb_bo_put(fence->bo);
        kvfree(fence);
    }
    spin_lock(&fb_state.lock);
    if (fb_state.stats.fence_fd_live > 0)
        fb_state.stats.fence_fd_live--;
    spin_unlock(&fb_state.lock);
    return 0;
}

static int fb_fence_fops_poll(struct vfs_file *file, short events)
{
    struct fb_gpu_fence_file *fence =
        file ? (struct fb_gpu_fence_file *)file->private_data : NULL;
    short revents = 0;

    if (fence == NULL || fence->bo == NULL)
        return POLLERR | POLLHUP;

    spin_lock(&fb_state.lock);
    if (fence->bo->signaled_fence >= fence->fence)
        revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
    fb_state.stats.fence_fd_polls++;
    if (revents & (POLLIN | POLLRDNORM | POLLRDBAND))
        fb_state.stats.fence_fd_poll_ready++;
    spin_unlock(&fb_state.lock);

    return revents;
}

static struct vfs_file_ops fb_fence_file_ops = {
    .poll = fb_fence_fops_poll,
    .release = fb_fence_fops_release,
};

static int fb_virgl_fence_fops_release(struct vfs_inode *inode,
                                       struct vfs_file *file)
{
    struct fb_gpu_virgl_fence_file *fence =
        file ? (struct fb_gpu_virgl_fence_file *)file->private_data : NULL;

    (void)inode;
    if (file != NULL)
        file->private_data = NULL;
    if (fence != NULL)
        kvfree(fence);
    spin_lock(&fb_state.lock);
    if (fb_state.stats.fence_fd_live > 0)
        fb_state.stats.fence_fd_live--;
    spin_unlock(&fb_state.lock);
    return 0;
}

static int fb_virgl_fence_fops_poll(struct vfs_file *file, short events)
{
    struct fb_gpu_virgl_fence_file *fence =
        file ? (struct fb_gpu_virgl_fence_file *)file->private_data : NULL;
    uint64 signaled = 0;
    short revents = 0;

    if (fence == NULL)
        return POLLERR | POLLHUP;
    if (virtio_gpu_user_fence(0, 0, &signaled) != 0)
        return POLLERR | POLLHUP;

    spin_lock(&fb_state.lock);
    fb_state.stats.fence_fd_polls++;
    if (signaled >= fence->fence) {
        revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
        if (revents & (POLLIN | POLLRDNORM | POLLRDBAND))
            fb_state.stats.fence_fd_poll_ready++;
    }
    spin_unlock(&fb_state.lock);
    return revents;
}

static struct vfs_file_ops fb_virgl_fence_file_ops = {
    .poll = fb_virgl_fence_fops_poll,
    .release = fb_virgl_fence_fops_release,
};

static void gpu_syncobj_state_put_locked(uint32 state_index);

static int fb_syncobj_fops_release(struct vfs_inode *inode,
                                   struct vfs_file *file)
{
    struct fb_gpu_syncobj_file *sync_file =
        file ? (struct fb_gpu_syncobj_file *)file->private_data : NULL;

    (void)inode;
    if (file != NULL)
        file->private_data = NULL;
    if (sync_file != NULL) {
        spin_lock(&fb_state.lock);
        gpu_syncobj_state_put_locked(sync_file->state_index);
        spin_unlock(&fb_state.lock);
        kvfree(sync_file);
    }
    return 0;
}

static struct vfs_file_ops fb_syncobj_file_ops = {
    .release = fb_syncobj_fops_release,
};

static int fb_bo_map_current(struct fb_gpu_bo_entry *bo, uint64 *addr_out)
{
    vm_t *vm;
    vma_t *vma;
    uint64 addr;
    uint64 flags;
    uint64 pte_flags;

    if (bo == NULL || addr_out == NULL || current == NULL ||
        current->vm == NULL)
        return -EINVAL;

    vm = current->vm;
    flags = PROT_READ | PROT_WRITE | VMA_FLAG_USER;

    vm_wlock(vm);
    addr = vm_find_free_range(vm, (size_t)(bo->size + FB_GPU_D3D12_HEAP_ALIGN), 0);
    if (addr == 0) {
        vm_wunlock(vm);
        return -ENOMEM;
    }
    addr = FB_GPU_ALIGN_UP(addr, FB_GPU_D3D12_HEAP_ALIGN);

    vma = vma_alloc(vm, addr, bo->size, flags);
    if (vma == NULL) {
        vm_wunlock(vm);
        return -ENOMEM;
    }
    if (anon_vma_prepare(vma) != 0) {
        vma_free(vm, vma);
        vm_wunlock(vm);
        return -ENOMEM;
    }

    pte_flags = vma2pte_flags(flags);
    for (uint32 i = 0; i < bo->npages; i++) {
        uint64 va = addr + (uint64)i * PGSIZE;
        uint64 pa = __page_to_pa(bo->pages[i]);

        if (page_ref_inc((void *)pa) <= 0) {
            vma_free(vm, vma);
            vm_wunlock(vm);
            return -ENOMEM;
        }
        if (mappages(vm->pagetable, va, PGSIZE, pa, pte_flags) != 0) {
            page_ref_dec((void *)pa);
            vma_free(vm, vma);
            vm_wunlock(vm);
            return -ENOMEM;
        }
        page_add_anon_rmap(bo->pages[i], vma, va);
    }

    vm_wunlock(vm);
    *addr_out = addr;
    return 0;
}

void fb_gpu_destroy_owner(pid_t owner_tgid)
{
    uint32 handles[FB_GPU_MAX_BOS];
    int n = 0;

    if (owner_tgid <= 0)
        return;

    spin_lock(&fb_state.lock);
    for (int i = 0; i < FB_GPU_MAX_BOS; i++) {
        struct fb_gpu_bo_entry *bo = &fb_state.bos[i];

        if (!bo->in_use || bo->owner_tgid != owner_tgid)
            continue;
        handles[n++] = bo->handle;
    }
    spin_unlock(&fb_state.lock);

    for (int i = 0; i < n; i++)
        (void)fb_bo_destroy(handles[i]);
}

void fb_gpu_destroy_render_owner(uint64 owner_id)
{
    uint32 handles[FB_GPU_MAX_BOS];
    int n = 0;

    if (owner_id == 0)
        return;

    spin_lock(&fb_state.lock);
    for (int i = 0; i < FB_GPU_MAX_BOS; i++) {
        struct fb_gpu_bo_entry *bo = &fb_state.bos[i];

        if (!bo->in_use || bo->owner_id != owner_id)
            continue;
        handles[n++] = bo->handle;
    }
    spin_unlock(&fb_state.lock);

    for (int i = 0; i < n; i++)
        (void)fb_bo_destroy(handles[i]);
}

static int fb_bo_destroy(uint32 handle)
{
    struct fb_gpu_bo_entry *bo;

    spin_lock(&fb_state.lock);
    bo = fb_bo_lookup_locked(handle);
    if (bo == NULL) {
        spin_unlock(&fb_state.lock);
        return -ENOENT;
    }
    bo->in_use = 0;
    bo->dead = 1;
    fb_ttm_account_locked(bo->ttm_mem_type, bo->size, 0);
    if (bo->ttm_pin_count != 0) {
        if (fb_state.stats.ttm_pinned_bytes >= bo->size)
            fb_state.stats.ttm_pinned_bytes -= bo->size;
        else
            fb_state.stats.ttm_pinned_bytes = 0;
        bo->ttm_pin_count = 0;
    }
    if (fb_state.stats.bo_handles > 0)
        fb_state.stats.bo_handles--;
    if (fb_state.stats.bo_live_bytes >= bo->size)
        fb_state.stats.bo_live_bytes -= bo->size;
    else
        fb_state.stats.bo_live_bytes = 0;
    spin_unlock(&fb_state.lock);

    fb_bo_put(bo);
    return 0;
}

static int fb_bo_destroy_owned(uint32 handle, uint64 owner_id,
                               pid_t owner_tgid)
{
    struct fb_gpu_bo_entry *bo;

    spin_lock(&fb_state.lock);
    bo = fb_bo_lookup_locked(handle);
    if (bo == NULL) {
        spin_unlock(&fb_state.lock);
        return -ENOENT;
    }
    if (!fb_bo_owner_matches(bo, owner_id, owner_tgid)) {
        spin_unlock(&fb_state.lock);
        return -EPERM;
    }
    spin_unlock(&fb_state.lock);
    return fb_bo_destroy(handle);
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
        fb_paint_boot_logo();
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

void fb_get_resolution(uint32 *xres, uint32 *yres)
{
    spin_lock(&fb_state.lock);
    if (xres)
        *xres = fb_state.xres;
    if (yres)
        *yres = fb_state.yres;
    spin_unlock(&fb_state.lock);
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
static int fb_release(cdev_t *cdev)
{
    (void)cdev;
    return 0;
}

static int gpu_open(cdev_t *cdev)
{
    enum fb_gpu_drm_node_type type = gpu_drm_node_from_cdev(cdev);

    spin_lock(&fb_state.lock);
    fb_state.stats.gpu_opens++;
    fb_state.stats.gpu_live_opens++;
    if (type == FB_GPU_DRM_NODE_PRIMARY) {
        fb_state.stats.drm_primary_opens++;
        fb_state.stats.drm_primary_live++;
    } else if (type == FB_GPU_DRM_NODE_RENDER) {
        fb_state.stats.drm_render_opens++;
        fb_state.stats.drm_render_live++;
    }
    spin_unlock(&fb_state.lock);
    return 0;
}

static int gpu_release_node(enum fb_gpu_drm_node_type type)
{
    spin_lock(&fb_state.lock);
    if (fb_state.stats.gpu_live_opens > 0)
        fb_state.stats.gpu_live_opens--;
    if (type == FB_GPU_DRM_NODE_PRIMARY &&
        fb_state.stats.drm_primary_live > 0) {
        fb_state.stats.drm_primary_live--;
    } else if (type == FB_GPU_DRM_NODE_RENDER &&
               fb_state.stats.drm_render_live > 0) {
        fb_state.stats.drm_render_live--;
    }
    spin_unlock(&fb_state.lock);
    return 0;
}

static int gpu_release(cdev_t *cdev)
{
    return gpu_release_node(gpu_drm_node_from_cdev(cdev));
}

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
        hyperv_video_dirty(0, 0, fb_state.xres, fb_state.yres);
        return done;
    } else {
        spin_lock(&fb_state.lock);
        memcpy((void *)fb_state.fb_virt, buf, count);
        spin_unlock(&fb_state.lock);
        virtio_gpu_present_fb_rect(fb_state.fb_virt, fb_state.pitch,
                                   0, 0, fb_state.xres, fb_state.yres);
        hyperv_video_dirty(0, 0, fb_state.xres, fb_state.yres);
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

static int fb_ioctl_for_owner(cdev_t *cdev, uint64 cmd, void *arg,
                              uint64 owner_id, pid_t owner_tgid)
{
    (void)cdev;
    if (owner_tgid == 0 && current != NULL)
        owner_tgid = current->tgid;

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
        strncpy(info.id, fb_state.virtio_backed ? "VirtioGPU" :
                (fb_state.firmware_backed ? "FirmwareFB" : "BochsVGA"),
                sizeof(info.id) - 1);
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
        hyperv_video_dirty(cmd.x, cmd.y, cmd.w, cmd.h);
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
        uint32 handle;
        uint32 npages;
        page_t **pages;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if ((req.flags & ~FB_GPU_BO_F_EXPORTABLE) != 0 ||
            req.width == 0 || req.height == 0 ||
            req.width > 8192 || req.height > 8192 ||
            req.width > 0xffffffffU / 4)
            return -EINVAL;

        req.pitch = req.width * 4;
        if ((uint64)req.pitch > ((uint64)-1) / req.height)
            return -EINVAL;
        size = (uint64)req.pitch * req.height;
        if (size == 0 || size > 64ULL * 1024 * 1024)
            return -EINVAL;

        req.size = FB_GPU_ALIGN_UP(size, FB_GPU_D3D12_HEAP_ALIGN);
        npages = req.size / PGSIZE;
        ret = fb_bo_alloc_pages(npages, &pages);
        if (ret != 0)
            return ret;

        ret = fb_bo_register(owner_id, owner_tgid, req.width, req.height,
                             req.pitch, req.size, pages, npages, &handle);
        if (ret != 0) {
            fb_bo_release_pages(pages, npages);
            return ret;
        }

        struct fb_gpu_bo_entry *bo = fb_bo_get(handle);
        if (bo == NULL) {
            (void)fb_bo_destroy(handle);
            return -ENOENT;
        }
        ret = fb_bo_map_current(bo, &addr);
        fb_bo_put(bo);
        if (ret != 0) {
            (void)fb_bo_destroy(handle);
            return ret;
        }

        req.addr = addr;
        req.handle = handle;
        req.reserved = 0;
        spin_lock(&fb_state.lock);
        fb_state.stats.bo_allocs++;
        fb_state.stats.bo_bytes += req.size;
        spin_unlock(&fb_state.lock);

        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            (void)fb_bo_destroy(handle);
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
        if (req.flags != 0)
            return -EINVAL;

        if (req.handle != 0) {
            struct fb_gpu_bo_entry *bo =
                fb_bo_get_owned(req.handle, owner_id, owner_tgid);
            int ret;

            if (bo == NULL) {
                spin_lock(&fb_state.lock);
                fb_state.stats.rejected_blits++;
                spin_unlock(&fb_state.lock);
                return -ENOENT;
            }
            ret = fb_blit_from_bo(bo, req, &req.fence);
            fb_bo_put(bo);
            if (ret == 0 &&
                either_copyout(1, (uint64)arg, (char *)&req,
                               sizeof(req)) < 0)
                return -EFAULT;
            return ret;
        }

        req.fence = 0;
        blit.x = req.x;
        blit.y = req.y;
        blit.w = req.w;
        blit.h = req.h;
        blit.src_pitch = req.src_pitch;
        blit.pixels = req.pixels;
        int ret = fb_blit_from_user(blit, 1);
        if (ret == 0 &&
            either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        return ret;
    }

    case FB_GPU_SCANOUT_MAP: {
        struct fb_gpu_scanout_map req;
        int ret;

        memset(&req, 0, sizeof(req));
        ret = fb_map_scanout_current(&req);
        if (ret != 0)
            return ret;
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            (void)vm_munmap(current->vm, req.addr, (size_t)req.size);
            return -EFAULT;
        }
        return 0;
    }

    case FB_GPU_SCANOUT_FLUSH: {
        struct fb_gpu_scanout_flush req;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        return fb_flush_scanout_rect(req);
    }

    case FB_GPU_DISPLAY_PROBE: {
        struct fb_gpu_display_probe req;
        uint32 host_w = 0, host_h = 0;
        uint32 edid_w = 0, edid_h = 0, edid_refresh = 0;

        memset(&req, 0, sizeof(req));
        spin_lock(&fb_state.lock);
        req.current_width = fb_state.xres;
        req.current_height = fb_state.yres;
        req.current_pitch = fb_state.pitch;
        spin_unlock(&fb_state.lock);
        req.current_refresh_millihz = 60000;
        req.host_refresh_millihz = 60000;
        if (virtio_gpu_probe_scanout(&host_w, &host_h) == 0 &&
            host_w != 0 && host_h != 0) {
            req.host_width = host_w;
            req.host_height = host_h;
            req.flags |= FB_GPU_DISPLAY_F_HOST_SCANOUT;
            if (host_w > 4096 || host_h > 4096 ||
                (req.current_width != 0 && host_w >= req.current_width * 2) ||
                (req.current_height != 0 && host_h >= req.current_height * 2))
                req.flags |= FB_GPU_DISPLAY_F_HOST_SCALED;
        }
        if (virtio_gpu_probe_edid_mode(&edid_w, &edid_h, &edid_refresh) == 0 &&
            edid_w != 0 && edid_h != 0) {
            req.preferred_width = edid_w;
            req.preferred_height = edid_h;
            req.preferred_refresh_millihz = edid_refresh;
            req.flags |= FB_GPU_DISPLAY_F_EDID;
            if (edid_w == req.current_width && edid_h == req.current_height &&
                edid_refresh != 0)
                req.current_refresh_millihz = edid_refresh;
        }
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }

    case FB_GPU_DISPLAY_WAIT: {
        struct fb_gpu_display_wait req;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if ((req.flags & ~FB_GPU_DISPLAY_WAIT_F_WAIT) != 0)
            return -EINVAL;

        spin_lock(&fb_state.lock);
        req.presented = fb_state.stats.display_last_present;
        req.completed = fb_state.stats.display_last_complete;
        spin_unlock(&fb_state.lock);
        req.refresh_millihz = 60000;

        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        if ((req.flags & FB_GPU_DISPLAY_WAIT_F_WAIT) &&
            req.wait_for != 0 && req.completed < req.wait_for)
            return -EAGAIN;
        return 0;
    }

    case FB_GPU_BO_DESTROY: {
        struct fb_gpu_bo_destroy req;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.handle == 0)
            return -EINVAL;
        return fb_bo_destroy_owned(req.handle, owner_id, owner_tgid);
    }

    case FB_GPU_BO_IMPORT: {
        struct fb_gpu_bo_import req;
        struct fb_gpu_bo_entry *bo;
        uint64 addr;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.handle == 0)
            return -EINVAL;

        bo = fb_bo_get_owned(req.handle, owner_id, owner_tgid);
        if (bo == NULL) {
            spin_lock(&fb_state.lock);
            fb_state.stats.rejected_blits++;
            spin_unlock(&fb_state.lock);
            return -ENOENT;
        }

        ret = fb_bo_map_current(bo, &addr);
        if (ret != 0) {
            fb_bo_put(bo);
            return ret;
        }

        req.width = bo->width;
        req.height = bo->height;
        req.pitch = bo->pitch;
        req.reserved = 0;
        req.size = bo->size;
        req.addr = addr;
        fb_bo_put(bo);

        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            (void)vm_munmap(current->vm, addr, (size_t)req.size);
            return -EFAULT;
        }

        spin_lock(&fb_state.lock);
        fb_state.stats.bo_imports++;
        spin_unlock(&fb_state.lock);
        return 0;
    }

    case FB_GPU_BO_EXPORT_FD: {
        struct fb_gpu_bo_export_fd req;
        struct fb_gpu_bo_entry *bo;
        int fd;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.handle == 0)
            return -EINVAL;

        bo = fb_bo_get_owned(req.handle, owner_id, owner_tgid);
        if (bo == NULL) {
            spin_lock(&fb_state.lock);
            fb_state.stats.rejected_blits++;
            spin_unlock(&fb_state.lock);
            return -ENOENT;
        }

        fd = vfs_custom_fd_alloc(&fb_bo_file_ops, bo, 0);
        if (fd < 0) {
            fb_bo_put(bo);
            return fd;
        }

        req.fd = fd;
        req.reserved = 0;
        spin_lock(&fb_state.lock);
        fb_state.stats.bo_fd_exports++;
        fb_state.stats.bo_fd_live++;
        if (fb_state.stats.bo_fd_live > fb_state.stats.bo_fd_peak)
            fb_state.stats.bo_fd_peak = fb_state.stats.bo_fd_live;
        spin_unlock(&fb_state.lock);
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }

    case FB_GPU_BO_IMPORT_FD: {
        struct fb_gpu_bo_import_fd req;
        struct vfs_file *file;
        struct fb_gpu_bo_entry *bo;
        page_t **pages;
        uint32 handle;
        uint64 addr;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.fd < 0)
            return -EINVAL;

        file = vfs_fdtable_get_file(current->fdtable, req.fd);
        if (file == NULL)
            return -EBADF;
        if (file->ops != &fb_bo_file_ops || file->private_data == NULL) {
            vfs_fput(file);
            return -EINVAL;
        }
        bo = (struct fb_gpu_bo_entry *)file->private_data;

        ret = fb_bo_clone_pages(bo, &pages);
        if (ret != 0) {
            vfs_fput(file);
            return ret;
        }
        ret = fb_bo_register(owner_id, owner_tgid, bo->width, bo->height,
                             bo->pitch, bo->size, pages, bo->npages, &handle);
        if (ret != 0) {
            fb_bo_release_pages(pages, bo->npages);
            vfs_fput(file);
            return ret;
        }

        bo = fb_bo_get(handle);
        if (bo == NULL) {
            (void)fb_bo_destroy(handle);
            vfs_fput(file);
            return -ENOENT;
        }
        ret = fb_bo_map_current(bo, &addr);
        if (ret != 0) {
            fb_bo_put(bo);
            (void)fb_bo_destroy(handle);
            vfs_fput(file);
            return ret;
        }

        req.width = bo->width;
        req.height = bo->height;
        req.pitch = bo->pitch;
        req.handle = handle;
        req.size = bo->size;
        req.addr = addr;
        fb_bo_put(bo);
        vfs_fput(file);

        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            (void)vm_munmap(current->vm, addr, (size_t)req.size);
            (void)fb_bo_destroy(handle);
            return -EFAULT;
        }

        spin_lock(&fb_state.lock);
        fb_state.stats.bo_fd_imports++;
        spin_unlock(&fb_state.lock);
        return 0;
    }

    case FB_GPU_BO_INFO: {
        struct fb_gpu_bo_info req;
        struct fb_gpu_bo_entry *bo;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.handle == 0)
            return -EINVAL;

        bo = fb_bo_get_owned(req.handle, owner_id, owner_tgid);
        if (bo == NULL)
            return -ENOENT;

        req.width = bo->width;
        req.height = bo->height;
        req.pitch = bo->pitch;
        req.format = FB_GPU_BO_FORMAT_XRGB8888;
        req.modifier = FB_GPU_BO_MOD_LINEAR;
        req.size = bo->size;
        req.addr_align = FB_GPU_D3D12_HEAP_ALIGN;
        req.size_align = FB_GPU_D3D12_HEAP_ALIGN;
        req.page_size = PGSIZE;
        req.reserved = 0;
        req.mmap_offset = GPU_DRM_MMAP_OFFSET(req.handle);
        fb_bo_put(bo);

        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }

    case FB_GPU_DXG_PRESENT_SOURCE_REGISTER: {
        struct fb_gpu_dxg_present_source_register req;
        int ret;

        spin_lock(&fb_state.lock);
        fb_state.stats.dxg_present_register_ioctl_entries++;
        spin_unlock(&fb_state.lock);
        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0) {
            spin_lock(&fb_state.lock);
            fb_state.stats.dxg_present_register_copyin_failures++;
            fb_state.stats.dxg_present_last_ret = EFAULT;
            spin_unlock(&fb_state.lock);
            return -EFAULT;
        }
        ret = fb_dxg_present_register(owner_id, owner_tgid, &req);
        if (ret != 0)
            return ret;
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            fb_dxg_present_source_unregister(req.present_source);
            return -EFAULT;
        }
        return 0;
    }

    case FB_GPU_DXG_PRESENT_SOURCE_COMMIT: {
        struct fb_gpu_dxg_present_source_commit req;

        spin_lock(&fb_state.lock);
        fb_state.stats.dxg_present_commit_ioctl_entries++;
        spin_unlock(&fb_state.lock);
        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0) {
            spin_lock(&fb_state.lock);
            fb_state.stats.dxg_present_commit_copyin_failures++;
            fb_state.stats.dxg_present_last_ret = EFAULT;
            spin_unlock(&fb_state.lock);
            return -EFAULT;
        }
        return fb_dxg_present_commit(owner_id, owner_tgid, &req);
    }

    case FB_GPU_DXG_PRESENT_SOURCE_QUERY: {
        struct fb_gpu_dxg_present_source_query req;
        int ret;

        spin_lock(&fb_state.lock);
        fb_state.stats.dxg_present_query_ioctl_entries++;
        spin_unlock(&fb_state.lock);
        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0) {
            spin_lock(&fb_state.lock);
            fb_state.stats.dxg_present_query_copyin_failures++;
            fb_state.stats.dxg_present_last_ret = EFAULT;
            spin_unlock(&fb_state.lock);
            return -EFAULT;
        }
        ret = fb_dxg_present_query(owner_id, owner_tgid, &req);
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            spin_lock(&fb_state.lock);
            fb_state.stats.dxg_present_query_copyout_failures++;
            fb_state.stats.dxg_present_last_ret = EFAULT;
            spin_unlock(&fb_state.lock);
            return -EFAULT;
        }
        return ret;
    }

    case FB_GPU_BO_FENCE: {
        struct fb_gpu_bo_fence req;
        struct fb_gpu_bo_entry *bo;
        uint64 target;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if ((req.flags & ~FB_GPU_BO_FENCE_WAIT) != 0 || req.handle == 0)
            return -EINVAL;

        bo = fb_bo_get_owned(req.handle, owner_id, owner_tgid);
        if (bo == NULL) {
            spin_lock(&fb_state.lock);
            fb_state.stats.rejected_blits++;
            spin_unlock(&fb_state.lock);
            return -ENOENT;
        }

        spin_lock(&fb_state.lock);
        target = req.wait_for ? req.wait_for : bo->last_fence;
        req.last_present = bo->last_fence;
        req.signaled = bo->signaled_fence;
        fb_state.stats.bo_fence_waits++;
        spin_unlock(&fb_state.lock);

        fb_bo_put(bo);
        if ((req.flags & FB_GPU_BO_FENCE_WAIT) && target > req.signaled)
            return -EAGAIN;
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }

    case FB_GPU_FENCE_EXPORT_FD: {
        struct fb_gpu_fence_export_fd req;
        struct fb_gpu_bo_entry *bo;
        struct fb_gpu_fence_file *fence_file;
        uint64 target;
        int fd;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.handle == 0)
            return -EINVAL;

        bo = fb_bo_get_owned(req.handle, owner_id, owner_tgid);
        if (bo == NULL)
            return -ENOENT;

        spin_lock(&fb_state.lock);
        target = req.fence ? req.fence : bo->last_fence;
        req.fence = target;
        req.signaled = bo->signaled_fence;
        spin_unlock(&fb_state.lock);
        if (target == 0) {
            fb_bo_put(bo);
            return -EINVAL;
        }

        fence_file = kvmalloc(sizeof(*fence_file));
        if (fence_file == NULL) {
            fb_bo_put(bo);
            return -ENOMEM;
        }
        fence_file->bo = bo;
        fence_file->fence = target;

        fd = vfs_custom_fd_alloc(&fb_fence_file_ops, fence_file, 0);
        if (fd < 0) {
            fb_bo_put(bo);
            kvfree(fence_file);
            return fd;
        }

        req.fd = fd;
        req.reserved = 0;
        spin_lock(&fb_state.lock);
        fb_state.stats.fence_fd_exports++;
        fb_state.stats.fence_fd_live++;
        if (fb_state.stats.fence_fd_live > fb_state.stats.fence_fd_peak)
            fb_state.stats.fence_fd_peak = fb_state.stats.fence_fd_live;
        spin_unlock(&fb_state.lock);
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }

    case FB_GPU_FENCE_QUERY: {
        struct fb_gpu_fence_query req;
        struct fb_gpu_fence_file *fence_file;
        struct vfs_file *file;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if ((req.flags & ~FB_GPU_FENCE_WAIT) != 0 || req.fd < 0)
            return -EINVAL;

        file = vfs_fdtable_get_file(current->fdtable, req.fd);
        if (file == NULL)
            return -EBADF;
        if (file->ops != &fb_fence_file_ops || file->private_data == NULL) {
            vfs_fput(file);
            return -EINVAL;
        }

        fence_file = (struct fb_gpu_fence_file *)file->private_data;
        spin_lock(&fb_state.lock);
        req.fence = fence_file->fence;
        req.signaled = fence_file->bo->signaled_fence;
        fb_state.stats.fence_fd_queries++;
        spin_unlock(&fb_state.lock);
        vfs_fput(file);

        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        if ((req.flags & FB_GPU_FENCE_WAIT) && req.signaled < req.fence)
            return -EAGAIN;
        return 0;
    }

    case FB_GPU_VIRGL_CTX_CREATE: {
        struct fb_gpu_virgl_ctx req;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0)
            return -EINVAL;
        req.debug_name[sizeof(req.debug_name) - 1] = 0;

        ret = virtio_gpu_user_context_create(owner_id, owner_tgid,
                                             0, req.debug_name, &req.ctx_id);
        if (ret != 0)
            return ret;
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            (void)virtio_gpu_user_context_destroy(owner_id, owner_tgid,
                                                  req.ctx_id);
            return -EFAULT;
        }
        return 0;
    }

    case FB_GPU_VIRGL_CTX_DESTROY: {
        struct fb_gpu_virgl_ctx req;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.ctx_id == 0)
            return -EINVAL;
        return virtio_gpu_user_context_destroy(owner_id, owner_tgid,
                                               req.ctx_id);
    }

    case FB_GPU_VIRGL_SUBMIT: {
        struct fb_gpu_virgl_submit req;
        uint32 *cmds;
        uint32 alloc_len = PGSIZE;
        int order = 0;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if ((req.flags & ~(FB_GPU_VIRGL_SUBMIT_ASYNC |
                           FB_GPU_VIRGL_SUBMIT_FORCE_FAIL)) != 0 ||
            req.ctx_id == 0 || req.cmd == 0 ||
            req.cmd_size == 0 || req.cmd_size > PGSIZE * 64 ||
            (req.cmd_size & (sizeof(uint32) - 1)) != 0)
            return -EINVAL;

        while (alloc_len < req.cmd_size) {
            if (order >= PAGE_BUDDY_MAX_ORDER)
                return -ENOMEM;
            order++;
            alloc_len <<= 1;
        }

        cmds = page_alloc(order, PAGE_TYPE_ANON);
        if (cmds == NULL)
            return -ENOMEM;
        if (either_copyin((char *)cmds, 1, req.cmd, req.cmd_size) < 0) {
            page_free(cmds, order);
            return -EFAULT;
        }

        ret = virtio_gpu_user_submit(owner_id, owner_tgid, req.ctx_id,
                                     req.flags, cmds,
                                     req.cmd_size / sizeof(uint32),
                                     &req.fence, &req.signaled);
        page_free(cmds, order);
        if (ret != 0)
            return ret;
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }

    case FB_GPU_VIRGL_FENCE: {
        struct fb_gpu_virgl_fence req;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if ((req.flags & ~FB_GPU_VIRGL_FENCE_WAIT) != 0)
            return -EINVAL;

        ret = virtio_gpu_user_fence(req.wait_for,
                                    (req.flags & FB_GPU_VIRGL_FENCE_WAIT) != 0,
                                    &req.signaled);
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        return ret;
    }

    case FB_GPU_VIRGL_FENCE_EXPORT_FD: {
        struct fb_gpu_virgl_fence_export_fd req;
        struct fb_gpu_virgl_fence_file *fence_file;
        uint64 signaled = 0;
        uint64 target;
        int fd;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0)
            return -EINVAL;
        if (virtio_gpu_user_fence(0, 0, &signaled) != 0)
            return -ENODEV;
        target = req.fence ? req.fence : signaled;
        if (target == 0)
            return -EINVAL;

        fence_file = kvmalloc(sizeof(*fence_file));
        if (fence_file == NULL)
            return -ENOMEM;
        fence_file->fence = target;
        fd = vfs_custom_fd_alloc(&fb_virgl_fence_file_ops, fence_file, 0);
        if (fd < 0) {
            kvfree(fence_file);
            return fd;
        }

        req.fd = fd;
        req.fence = target;
        req.signaled = signaled;
        spin_lock(&fb_state.lock);
        fb_state.stats.fence_fd_exports++;
        fb_state.stats.fence_fd_live++;
        if (fb_state.stats.fence_fd_live > fb_state.stats.fence_fd_peak)
            fb_state.stats.fence_fd_peak = fb_state.stats.fence_fd_live;
        spin_unlock(&fb_state.lock);
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }

    case FB_GPU_VIRGL_FENCE_QUERY_FD: {
        struct fb_gpu_virgl_fence_query_fd req;
        struct fb_gpu_virgl_fence_file *fence_file;
        struct vfs_file *file;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if ((req.flags & ~FB_GPU_VIRGL_FENCE_WAIT) != 0 || req.fd < 0)
            return -EINVAL;

        file = vfs_fdtable_get_file(current->fdtable, req.fd);
        if (file == NULL)
            return -EBADF;
        if (file->ops != &fb_virgl_fence_file_ops ||
            file->private_data == NULL) {
            vfs_fput(file);
            return -EINVAL;
        }
        fence_file = (struct fb_gpu_virgl_fence_file *)file->private_data;
        req.fence = fence_file->fence;
        vfs_fput(file);

        ret = virtio_gpu_user_fence(req.fence,
                                    (req.flags & FB_GPU_VIRGL_FENCE_WAIT) != 0,
                                    &req.signaled);
        if (ret != 0) {
            if (either_copyout(1, (uint64)arg, (char *)&req,
                               sizeof(req)) < 0)
                return -EFAULT;
            return ret;
        }

        spin_lock(&fb_state.lock);
        fb_state.stats.fence_fd_queries++;
        spin_unlock(&fb_state.lock);
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }

    case FB_GPU_VIRGL_GET_CAPS: {
        struct fb_gpu_virgl_caps req;
        void *caps = NULL;
        uint32 capset_size = 0;
        uint32 requested_size;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0)
            return -EINVAL;
        requested_size = req.size;
        if (req.data != 0) {
            if (requested_size == 0 || requested_size > PGSIZE)
                return -EINVAL;
            caps = kalloc();
            if (caps == NULL)
                return -ENOMEM;
        }

        ret = virtio_gpu_user_get_caps(caps, req.data ? req.size : 0,
                                       &req.capset_id,
                                       &req.capset_version,
                                       &capset_size);
        req.size = capset_size;
        if (ret == 0 && req.data != 0) {
            uint32 copy_size = capset_size;
            if (copy_size > requested_size)
                copy_size = requested_size;
            if (copy_size != 0 &&
                either_copyout(1, req.data, (char *)caps, copy_size) < 0)
                ret = -EFAULT;
        }
        if (caps != NULL)
            kfree(caps);
        if (ret != 0)
            return ret;
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }

    case FB_GPU_VIRGL_RESOURCE_CREATE: {
        struct fb_gpu_virgl_resource_create req;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        req.resource_id = 0;
        req.addr = 0;
        ret = virtio_gpu_user_resource_create(owner_id, owner_tgid, &req);
        if (ret != 0)
            return ret;
        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0) {
            (void)virtio_gpu_user_resource_destroy(owner_id, owner_tgid,
                                                   req.resource_id);
            if (req.addr != 0 && req.size != 0)
                (void)vm_munmap(current->vm, req.addr, (size_t)req.size);
            return -EFAULT;
        }
        return 0;
    }

    case FB_GPU_VIRGL_RESOURCE_DESTROY: {
        struct fb_gpu_virgl_resource_destroy req;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.resource_id == 0)
            return -EINVAL;
        return virtio_gpu_user_resource_destroy(owner_id, owner_tgid,
                                                req.resource_id);
    }

    case FB_GPU_VIRGL_RESOURCE_EXPORT_FD: {
        struct fb_gpu_virgl_resource_export_fd req;
        struct fb_gpu_bo_entry *bo;
        page_t **pages = NULL;
        uint32 width = 0;
        uint32 height = 0;
        uint32 pitch = 0;
        uint32 npages = 0;
        uint32 handle = 0;
        uint64 size = 0;
        int fd;
        int ret;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.flags != 0 || req.resource_id == 0)
            return -EINVAL;

        ret = virtio_gpu_user_resource_export_pages(owner_id, owner_tgid,
                                                    req.resource_id, &width,
                                                    &height, &pitch, &size,
                                                    &pages, &npages);
        if (ret != 0)
            return ret;

        ret = fb_bo_register(owner_id, owner_tgid, width, height, pitch, size,
                             pages, npages, &handle);
        if (ret != 0) {
            fb_bo_release_pages(pages, npages);
            return ret;
        }

        bo = fb_bo_get(handle);
        if (bo == NULL) {
            (void)fb_bo_destroy(handle);
            return -ENOENT;
        }
        fd = vfs_custom_fd_alloc(&fb_bo_file_ops, bo, 0);
        if (fd < 0) {
            fb_bo_put(bo);
            (void)fb_bo_destroy(handle);
            return fd;
        }

        req.fd = fd;
        req.handle = handle;
        req.width = width;
        req.height = height;
        req.pitch = pitch;
        req.reserved = 0;
        req.size = size;
        spin_lock(&fb_state.lock);
        fb_state.stats.bo_fd_exports++;
        fb_state.stats.bo_fd_live++;
        if (fb_state.stats.bo_fd_live > fb_state.stats.bo_fd_peak)
            fb_state.stats.bo_fd_peak = fb_state.stats.bo_fd_live;
        spin_unlock(&fb_state.lock);

        /*
         * The fd owns the exported BO capability. Drop the transient handle
         * immediately so closing the fd releases the shared page references.
         */
        (void)fb_bo_destroy(handle);

        if (either_copyout(1, (uint64)arg, (char *)&req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }

    case FB_GPU_VIRGL_TRANSFER_TO_HOST:
    case FB_GPU_VIRGL_TRANSFER_FROM_HOST: {
        struct fb_gpu_virgl_transfer req;

        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        return virtio_gpu_user_transfer(owner_id, owner_tgid, &req,
            cmd == FB_GPU_VIRGL_TRANSFER_FROM_HOST);
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
        struct fb_gpu_backend_info backend;

        spin_lock(&fb_state.lock);
        stats = fb_state.stats;
        spin_unlock(&fb_state.lock);
        virtio_gpu_get_fb_stats(&stats);
        gpu_backend_fill(&backend);
        stats.gpu_backend = backend.backend;
        stats.gpu_backend_flags = backend.flags;
        stats.dxg_global_open = backend.dxg_global_open;
        stats.dxg_vgpu_open = backend.dxg_vgpu_open;
        stats.dxg_d3dkmt = backend.dxg_d3dkmt;
        stats.dxg_global_rx = backend.dxg_global_rx;
        stats.dxg_vgpu_rx = backend.dxg_vgpu_rx;
        if (either_copyout(1, (uint64)arg, (char *)&stats, sizeof(stats)) < 0)
            return -EFAULT;
        return 0;
    }

    case FB_GPU_BACKEND_QUERY:
        return gpu_backend_query((uint64)arg);

    case FBIOPUT_VSCREENINFO: {
        struct fb_var_screeninfo req;
        if (either_copyin((char *)&req, 1, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;

        /* Validate requested resolution */
        if (req.xres < 640 || req.xres > 2560 ||
            req.yres < 480 || req.yres > 1600)
            return -EINVAL;

        if (fb_state.virtio_backed) {
            int ret = virtio_gpu_resize_scanout(req.xres, req.yres);
            if (ret == 0)
                printf("FB: virtio resolution changed to %dx%d\n",
                       req.xres, req.yres);
            return ret;
        }

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

static int fb_ioctl(cdev_t *cdev, uint64 cmd, void *arg)
{
    return fb_ioctl_for_owner(cdev, cmd, arg, 0,
                              current ? current->tgid : 0);
}

static int gpu_ioctl(cdev_t *cdev, uint64 cmd, void *arg)
{
    int trace = fb_gpu_trace_enabled() && fb_gpu_trace_process();
    int ret;

    if (trace)
        printf("webkit-gpu: enter pid=%d name=%s dev=cdev cmd=0x%lx(%s)\n",
               current ? current->pid : -1,
               current ? current->name : "?", cmd,
               fb_gpu_ioctl_name(cmd));

    switch (cmd) {
    case FB_GPU_GET_STATS:
    case FB_GPU_BO_CREATE:
    case FB_GPU_BO_DESTROY:
    case FB_GPU_BO_IMPORT:
    case FB_GPU_BO_EXPORT_FD:
    case FB_GPU_BO_IMPORT_FD:
    case FB_GPU_BO_INFO:
    case FB_GPU_DXG_PRESENT_SOURCE_REGISTER:
    case FB_GPU_DXG_PRESENT_SOURCE_COMMIT:
    case FB_GPU_DXG_PRESENT_SOURCE_QUERY:
    case FB_GPU_BO_FENCE:
    case FB_GPU_FENCE_EXPORT_FD:
    case FB_GPU_FENCE_QUERY:
    case FB_GPU_VIRGL_CTX_CREATE:
    case FB_GPU_VIRGL_CTX_DESTROY:
    case FB_GPU_VIRGL_SUBMIT:
    case FB_GPU_VIRGL_FENCE:
    case FB_GPU_VIRGL_FENCE_EXPORT_FD:
    case FB_GPU_VIRGL_FENCE_QUERY_FD:
    case FB_GPU_VIRGL_GET_CAPS:
    case FB_GPU_VIRGL_RESOURCE_CREATE:
    case FB_GPU_VIRGL_RESOURCE_DESTROY:
    case FB_GPU_VIRGL_RESOURCE_EXPORT_FD:
    case FB_GPU_VIRGL_TRANSFER_TO_HOST:
    case FB_GPU_VIRGL_TRANSFER_FROM_HOST:
    case FB_GPU_DISPLAY_PROBE:
    case FB_GPU_BACKEND_QUERY:
        break;
    default:
        return -EINVAL;
    }

    spin_lock(&fb_state.lock);
    fb_state.stats.gpu_ioctls++;
    spin_unlock(&fb_state.lock);
    ret = fb_ioctl(cdev, cmd, arg);
    if (trace)
        printf("webkit-gpu: exit pid=%d name=%s dev=cdev cmd=0x%lx(%s) ret=%d\n",
               current ? current->pid : -1,
               current ? current->name : "?", cmd,
               fb_gpu_ioctl_name(cmd), ret);
    return ret;
}

static uint64 gpu_alloc_render_owner_id(void)
{
    uint64 id;

    spin_lock(&fb_state.lock);
    id = fb_state.next_render_owner_id++;
    if (fb_state.next_render_owner_id == 0)
        fb_state.next_render_owner_id = 1;
    spin_unlock(&fb_state.lock);
    return id;
}

static uint32 gpu_alloc_drm_magic(void)
{
    uint32 magic;

    spin_lock(&fb_state.lock);
    magic = fb_state.next_drm_magic++;
    if (fb_state.next_drm_magic == 0)
        fb_state.next_drm_magic = 1;
    if (magic == 0)
        magic = 1;
    spin_unlock(&fb_state.lock);
    return magic;
}

static enum fb_gpu_drm_node_type gpu_drm_node_from_cdev(cdev_t *cdev)
{
    if (cdev == NULL)
        return FB_GPU_DRM_NODE_LEGACY;
    if (cdev->dev.devname != NULL) {
        if (strcmp(cdev->dev.devname, "dri/card0") == 0)
            return FB_GPU_DRM_NODE_PRIMARY;
        if (strcmp(cdev->dev.devname, "dri/renderD128") == 0)
            return FB_GPU_DRM_NODE_RENDER;
    }
    if (cdev->dev.major == DRM_PRIMARY_MAJOR &&
        cdev->dev.minor == DRM_PRIMARY_MINOR)
        return FB_GPU_DRM_NODE_PRIMARY;
    if (cdev->dev.major == DRM_RENDER_MAJOR &&
        cdev->dev.minor == DRM_RENDER_MINOR)
        return FB_GPU_DRM_NODE_RENDER;
    return FB_GPU_DRM_NODE_LEGACY;
}

static const char *gpu_drm_node_name(enum fb_gpu_drm_node_type type)
{
    switch (type) {
    case FB_GPU_DRM_NODE_PRIMARY:
        return "primary";
    case FB_GPU_DRM_NODE_RENDER:
        return "render";
    default:
        return "legacy";
    }
}

static int gpu_drm_is_primary_like(struct fb_gpu_render_owner *owner)
{
    return owner != NULL &&
        (owner->drm.node_type == FB_GPU_DRM_NODE_PRIMARY ||
         owner->drm.node_type == FB_GPU_DRM_NODE_LEGACY);
}

static cdev_t gpu_render_cdev;

static int gpu_copyout_string(uint64 dst, uint64 *len, const char *src)
{
    uint64 actual = strlen(src);
    uint64 n, cap;

    if (len == NULL)
        return -EINVAL;
    cap = *len;
    n = cap;
    *len = actual;
    if (dst == 0 || n == 0)
        return 0;
    if (n > actual)
        n = actual;
    if (either_copyout(1, dst, (void *)src, n) < 0)
        return -EFAULT;
    if (cap > n) {
        char nul = 0;
        if (either_copyout(1, dst + n, &nul, 1) < 0)
            return -EFAULT;
    }
    return 0;
}

static int gpu_user_debug_name(uint64 user_ptr, char name[64])
{
    uint32 i;

    if (name == NULL)
        return -EINVAL;
    if (user_ptr == 0) {
        memcpy(name, "xv6-drm", sizeof("xv6-drm"));
        return 0;
    }

    for (i = 0; i < 63; i++) {
        if (either_copyin(&name[i], 1, user_ptr + i, 1) < 0)
            return -EFAULT;
        if (name[i] == 0)
            return 0;
    }
    name[63] = 0;
    return 0;
}

static void gpu_drm_mode_append_uint(char *buf, uint32 *pos, uint32 value)
{
    char tmp[10];
    uint32 n = 0;

    if (buf == NULL || pos == NULL || *pos >= 31)
        return;
    if (value == 0) {
        buf[(*pos)++] = '0';
        return;
    }
    while (value != 0 && n < sizeof(tmp)) {
        tmp[n++] = '0' + (value % 10);
        value /= 10;
    }
    while (n != 0 && *pos < 31)
        buf[(*pos)++] = tmp[--n];
}

static void gpu_drm_get_mode_size(uint32 *width, uint32 *height)
{
    uint32 w, h;

    spin_lock(&fb_state.lock);
    w = fb_state.xres;
    h = fb_state.yres;
    spin_unlock(&fb_state.lock);
    if (w < 640)
        w = FB_DEFAULT_WIDTH;
    if (h < 480)
        h = FB_DEFAULT_HEIGHT;
    if (width)
        *width = w;
    if (height)
        *height = h;
}

static void gpu_drm_fill_mode(struct drm_mode_modeinfo_compat *mode)
{
    uint32 w, h, pos = 0;
    uint32 hblank, vblank;

    memset(mode, 0, sizeof(*mode));
    gpu_drm_get_mode_size(&w, &h);

    hblank = w / 5;
    if (hblank < 160)
        hblank = 160;
    vblank = h / 20;
    if (vblank < 30)
        vblank = 30;

    mode->hdisplay = (uint16)w;
    mode->hsync_start = (uint16)(w + hblank / 3);
    mode->hsync_end = (uint16)(w + (2 * hblank) / 3);
    mode->htotal = (uint16)(w + hblank);
    mode->vdisplay = (uint16)h;
    mode->vsync_start = (uint16)(h + vblank / 3);
    mode->vsync_end = (uint16)(h + (2 * vblank) / 3);
    mode->vtotal = (uint16)(h + vblank);
    mode->vrefresh = 60;
    mode->clock = (uint32)(((uint64)mode->htotal * mode->vtotal *
                            mode->vrefresh) / 1000);
    if (mode->clock == 0)
        mode->clock = 40000;
    mode->flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
    mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

    gpu_drm_mode_append_uint(mode->name, &pos, w);
    if (pos < 31)
        mode->name[pos++] = 'x';
    gpu_drm_mode_append_uint(mode->name, &pos, h);
    mode->name[pos < 32 ? pos : 31] = 0;
}

static int gpu_drm_copyout_u32_array(uint64 ptr, uint32 capacity,
                                     const uint32 *ids, uint32 count)
{
    uint32 n;

    if (ptr == 0 || capacity == 0 || ids == NULL || count == 0)
        return 0;
    n = capacity < count ? capacity : count;
    if (either_copyout(1, ptr, (void *)ids, (uint64)n * sizeof(uint32)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_copyout_mode_array(uint64 ptr, uint32 capacity,
                                      const struct drm_mode_modeinfo_compat *mode)
{
    if (ptr == 0 || capacity == 0 || mode == NULL)
        return 0;
    if (either_copyout(1, ptr, (void *)mode, sizeof(*mode)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_current_capset(uint32 *capset_id)
{
    uint32 id = 0;
    int ret;

    ret = virtio_gpu_user_get_caps(NULL, 0, &id, NULL, NULL);
    if (ret != 0)
        return ret;
    if (id == 0)
        return -ENODEV;
    if (capset_id != NULL)
        *capset_id = id;
    return 0;
}

static void gpu_backend_copy_string(char *dst, size_t dst_size,
                                    const char *src)
{
    size_t i;

    if (dst == NULL || dst_size == 0)
        return;
    for (i = 0; i + 1 < dst_size && src != NULL && src[i] != 0; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static void gpu_backend_fill(struct fb_gpu_backend_info *info)
{
    struct hyperv_dxg_status dxg;

    memset(info, 0, sizeof(*info));
    info->backend = FB_GPU_BACKEND_DUMB;
    info->flags = FB_GPU_BACKEND_F_RENDER_NODE | FB_GPU_BACKEND_F_DUMB_BO;
    gpu_backend_copy_string(info->name, sizeof(info->name), "dumb");
    gpu_backend_copy_string(info->renderer, sizeof(info->renderer),
                            "xv6 software/dumb-buffer render node");

    if (virtio_gpu_has_virgl()) {
        uint32 capset_id = 0, capset_version = 0, capset_size = 0;

        (void)virtio_gpu_user_get_caps(NULL, 0, &capset_id,
                                       &capset_version, &capset_size);
        info->backend = FB_GPU_BACKEND_VIRGL;
        info->flags |= FB_GPU_BACKEND_F_VIRGL_OPENGL |
                       FB_GPU_BACKEND_F_OPENGL_SUBMIT;
        info->capset_id = capset_id;
        info->capset_version = capset_version;
        info->capset_size = capset_size;
        gpu_backend_copy_string(info->name, sizeof(info->name), "virgl");
        gpu_backend_copy_string(info->renderer, sizeof(info->renderer),
                                "OpenGL via virtio-gpu virgl");
        return;
    }

    if (hyperv_dxg_get_status(&dxg) == 0 &&
        (dxg.global_present || dxg.vgpu_present)) {
        info->backend = FB_GPU_BACKEND_HYPERV_DXG;
        if (dxg.global_open_ok && dxg.vgpu_open_ok)
            info->flags |= FB_GPU_BACKEND_F_DXG_TRANSPORT;
        info->dxg_global_open = dxg.global_open_ok != 0;
        info->dxg_vgpu_open = dxg.vgpu_open_ok != 0;
        info->dxg_d3dkmt = hyperv_dxg_d3dkmt_ready() != 0;
        if (info->dxg_d3dkmt)
            info->flags |= FB_GPU_BACKEND_F_D3DKMT;
        info->dxg_global_status = dxg.global_open_status;
        info->dxg_vgpu_status = dxg.vgpu_open_status;
        info->dxg_global_rx = dxg.global_rx_packets;
        info->dxg_vgpu_rx = dxg.vgpu_rx_packets;
        gpu_backend_copy_string(info->name, sizeof(info->name),
                                "hyperv-dxg");
        gpu_backend_copy_string(info->renderer, sizeof(info->renderer),
                                info->dxg_d3dkmt ?
                                "Hyper-V GPU-PV D3DKMT/DXCore; OpenGL-submit gated" :
                                "Hyper-V GPU-PV DXG transport; D3DKMT pending");
    }
}

static int gpu_backend_query(uint64 arg)
{
    struct fb_gpu_backend_info info;

    gpu_backend_fill(&info);
    if (either_copyout(1, arg, &info, sizeof(info)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_owner_create_context(struct fb_gpu_render_owner *owner,
                                    const char *name, uint32 capset_id)
{
    int ret;
    uint32 current_capset = 0;

    if (owner == NULL)
        return -EBADF;
    ret = gpu_drm_current_capset(&current_capset);
    if (ret != 0)
        return ret;
    if (capset_id == 0)
        capset_id = current_capset;
    ret = virtio_gpu_user_get_caps_for(capset_id, 0, NULL, 0, NULL, NULL,
                                       NULL);
    if (ret != 0)
        return ret;
    if (owner->capset_id != 0 && owner->capset_id != capset_id)
        return -EINVAL;
    if (owner->default_ctx_id != 0)
        return 0;
    ret = virtio_gpu_user_context_create(owner->id, owner->tgid, capset_id,
                                         name ? name : "xv6-drm",
                                         &owner->default_ctx_id);
    if (ret == 0)
        owner->capset_id = capset_id;
    return ret;
}

static int gpu_owner_ensure_context(struct fb_gpu_render_owner *owner)
{
    return gpu_owner_create_context(owner, "xv6-drm", 0);
}

static int gpu_drm_version(uint64 arg)
{
    struct drm_version_compat req;
    struct fb_gpu_backend_info backend;
    const char *driver;
    const char *desc;
    int ret;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    gpu_backend_fill(&backend);
    if (backend.backend == FB_GPU_BACKEND_VIRGL) {
        driver = "virtio_gpu";
        desc = backend.renderer[0] ? backend.renderer : "virtio GPU";
    } else if (gpu_nouveau_device() != NULL) {
        driver = "nouveau";
        desc = "xv6 native Nouveau DRM compatibility facade";
    } else {
        driver = fb_drm_device.driver_name;
        desc = backend.renderer[0] ? backend.renderer : fb_drm_device.desc;
    }
    req.version_major = (int)fb_drm_device.driver_major;
    req.version_minor = (int)fb_drm_device.driver_minor;
    req.version_patchlevel = (int)fb_drm_device.driver_patchlevel;
    ret = gpu_copyout_string(req.name, &req.name_len, driver);
    if (ret != 0)
        return ret;
    ret = gpu_copyout_string(req.date, &req.date_len, "20260502");
    if (ret != 0)
        return ret;
    ret = gpu_copyout_string(req.desc, &req.desc_len, desc);
    if (ret != 0)
        return ret;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_get_unique(uint64 arg)
{
    struct drm_unique_compat req;
    struct fb_gpu_backend_info backend;
    struct pci_device_info *nvdev;
    char pci_unique[32];

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    gpu_backend_fill(&backend);
    nvdev = gpu_nouveau_device();
    if (nvdev != NULL) {
        memset(pci_unique, 0, sizeof(pci_unique));
        pci_unique[0] = 'p';
        pci_unique[1] = 'c';
        pci_unique[2] = 'i';
        pci_unique[3] = ':';
        pci_unique[4] = '0';
        pci_unique[5] = '0';
        pci_unique[6] = '0';
        pci_unique[7] = '0';
        pci_unique[8] = ':';
        pci_unique[9] = "0123456789abcdef"[(nvdev->bus >> 4) & 0xf];
        pci_unique[10] = "0123456789abcdef"[nvdev->bus & 0xf];
        pci_unique[11] = ':';
        pci_unique[12] = "0123456789abcdef"[(nvdev->dev >> 4) & 0xf];
        pci_unique[13] = "0123456789abcdef"[nvdev->dev & 0xf];
        pci_unique[14] = '.';
        pci_unique[15] = '0' + (nvdev->func & 0x7);
        if (gpu_copyout_string(req.unique, &req.unique_len, pci_unique) != 0)
            return -EFAULT;
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }
    if (gpu_copyout_string(req.unique, &req.unique_len,
                           backend.backend == FB_GPU_BACKEND_HYPERV_DXG ?
                               "vmbus:hyperv-dxg" : "pci:0000:00:04.0") != 0)
        return -EFAULT;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_get_cap(uint64 arg)
{
    struct drm_get_cap_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    switch (req.capability) {
    case DRM_CAP_DUMB_BUFFER:
    case DRM_CAP_VBLANK_HIGH_CRTC:
    case DRM_CAP_TIMESTAMP_MONOTONIC:
        req.value = 1;
        break;
    case DRM_CAP_DUMB_PREFERRED_DEPTH:
        req.value = 32;
        break;
    case DRM_CAP_DUMB_PREFER_SHADOW:
        req.value = 0;
        break;
    case DRM_CAP_PRIME:
        req.value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;
        break;
    case DRM_CAP_CURSOR_WIDTH:
    case DRM_CAP_CURSOR_HEIGHT:
        req.value = 64;
        break;
    case DRM_CAP_ASYNC_PAGE_FLIP:
    case DRM_CAP_ADDFB2_MODIFIERS:
    case DRM_CAP_PAGE_FLIP_TARGET:
    case DRM_CAP_CRTC_IN_VBLANK_EVENT:
    case DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP:
        req.value = 0;
        break;
    case DRM_CAP_SYNCOBJ:
    case DRM_CAP_SYNCOBJ_TIMELINE:
        req.value = 1;
        break;
    default:
        return -EINVAL;
    }
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_get_magic(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_auth_compat req;

    if (!gpu_drm_is_primary_like(owner))
        return -EOPNOTSUPP;
    req.magic = owner->drm.magic;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_auth_magic(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_auth_compat req;

    if (!gpu_drm_is_primary_like(owner))
        return -EOPNOTSUPP;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.magic == 0 || req.magic != owner->drm.magic)
        return -EINVAL;
    owner->drm.authenticated = 1;
    spin_lock(&fb_state.lock);
    fb_state.stats.drm_auths++;
    spin_unlock(&fb_state.lock);
    return 0;
}

static int gpu_drm_get_client(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_client_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.idx != 0)
        return -EINVAL;
    req.auth = owner ? owner->drm.authenticated : 0;
    req.pid = owner ? (uint64)owner->tgid : 0;
    req.uid = 0;
    req.magic = owner ? owner->drm.magic : 1;
    spin_lock(&fb_state.lock);
    req.iocs = owner ? owner->drm.ioctl_count : fb_state.stats.drm_ioctls;
    spin_unlock(&fb_state.lock);
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_set_client_cap(struct fb_gpu_render_owner *owner,
                                  uint64 arg)
{
    struct drm_set_client_cap_compat req;
    uint64 bit;

    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    switch (req.capability) {
    case DRM_CLIENT_CAP_STEREO_3D:
    case DRM_CLIENT_CAP_UNIVERSAL_PLANES:
    case DRM_CLIENT_CAP_ATOMIC:
    case DRM_CLIENT_CAP_ASPECT_RATIO:
    case DRM_CLIENT_CAP_WRITEBACK_CONNECTORS:
        break;
    default:
        return -EINVAL;
    }
    if (req.value > 1)
        return -EINVAL;
    bit = 1ULL << req.capability;
    if (req.value)
        owner->drm.client_caps |= bit;
    else
        owner->drm.client_caps &= ~bit;
    return 0;
}

static int gpu_drm_set_master(struct fb_gpu_render_owner *owner)
{
    if (!gpu_drm_is_primary_like(owner))
        return -EOPNOTSUPP;

    spin_lock(&fb_state.lock);
    if (fb_state.drm_master_owner_id != 0 &&
        fb_state.drm_master_owner_id != owner->id) {
        spin_unlock(&fb_state.lock);
        return -EBUSY;
    }
    fb_state.drm_master_owner_id = owner->id;
    fb_state.stats.drm_master_sets++;
    owner->drm.is_master = 1;
    owner->drm.authenticated = 1;
    spin_unlock(&fb_state.lock);
    return 0;
}

static int gpu_drm_drop_master(struct fb_gpu_render_owner *owner)
{
    if (!gpu_drm_is_primary_like(owner))
        return -EOPNOTSUPP;

    spin_lock(&fb_state.lock);
    if (fb_state.drm_master_owner_id != owner->id || !owner->drm.is_master) {
        spin_unlock(&fb_state.lock);
        return -EINVAL;
    }
    fb_state.drm_master_owner_id = 0;
    fb_state.stats.drm_master_drops++;
    owner->drm.is_master = 0;
    spin_unlock(&fb_state.lock);
    return 0;
}

static int gpu_drm_get_stats(uint64 arg)
{
    struct drm_stats_compat req;

    memset(&req, 0, sizeof(req));
    req.count = 6;
    spin_lock(&fb_state.lock);
    req.data[0].value = fb_state.stats.drm_ioctls;
    req.data[1].value = fb_state.stats.bo_allocs;
    req.data[2].value = fb_state.stats.bo_bytes / 1024;
    req.data[3].value = fb_state.stats.fence_fd_polls;
    req.data[4].value = fb_state.stats.drm_unknown_ioctls;
    req.data[5].value = fb_state.stats.drm_master_sets;
    spin_unlock(&fb_state.lock);
    req.data[0].type = 3; /* _DRM_STAT_IOCTLS */
    req.data[1].type = 8; /* _DRM_STAT_COUNT */
    req.data[2].type = 7; /* _DRM_STAT_BYTE */
    req.data[3].type = 8; /* _DRM_STAT_COUNT */
    req.data[4].type = 8; /* _DRM_STAT_COUNT */
    req.data[5].type = 8; /* _DRM_STAT_COUNT */
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_set_version(uint64 arg)
{
    struct drm_set_version_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    req.drm_di_major = 1;
    if (req.drm_di_minor < 4)
        req.drm_di_minor = 4;
    req.drm_dd_major = 0;
    req.drm_dd_minor = 1;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_wait_vblank(uint64 arg)
{
    union drm_wait_vblank_compat req;
    uint64 ticks = r_time();

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    req.reply.sequence = req.request.sequence + 1;
    req.reply.tval_sec = (int64)(ticks / 10000000ULL);
    req.reply.tval_usec = (int64)((ticks % 10000000ULL) / 10ULL);
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_mode_getresources(uint64 arg)
{
    struct drm_mode_card_res_compat req;
    uint32 ids[1];
    uint32 w, h;
    int ret;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    gpu_drm_get_mode_size(&w, &h);

    ids[0] = GPU_DRM_CRTC_ID;
    ret = gpu_drm_copyout_u32_array(req.crtc_id_ptr, req.count_crtcs, ids, 1);
    if (ret != 0)
        return ret;
    ids[0] = GPU_DRM_CONNECTOR_ID;
    ret = gpu_drm_copyout_u32_array(req.connector_id_ptr,
                                    req.count_connectors, ids, 1);
    if (ret != 0)
        return ret;
    ids[0] = GPU_DRM_ENCODER_ID;
    ret = gpu_drm_copyout_u32_array(req.encoder_id_ptr,
                                    req.count_encoders, ids, 1);
    if (ret != 0)
        return ret;

    req.count_fbs = 0;
    req.count_crtcs = 1;
    req.count_connectors = 1;
    req.count_encoders = 1;
    req.min_width = 1;
    req.max_width = w > 8192 ? w : 8192;
    req.min_height = 1;
    req.max_height = h > 8192 ? h : 8192;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_mode_getcrtc(uint64 arg)
{
    struct drm_mode_crtc_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.crtc_id != GPU_DRM_CRTC_ID)
        return -ENOENT;
    req.fb_id = 0;
    req.x = 0;
    req.y = 0;
    req.gamma_size = 0;
    req.mode_valid = 1;
    gpu_drm_fill_mode(&req.mode);
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_mode_getencoder(uint64 arg)
{
    struct drm_mode_get_encoder_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.encoder_id != GPU_DRM_ENCODER_ID)
        return -ENOENT;
    req.encoder_type = DRM_MODE_ENCODER_VIRTUAL;
    req.crtc_id = GPU_DRM_CRTC_ID;
    req.possible_crtcs = 1;
    req.possible_clones = 1;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_mode_getconnector(uint64 arg)
{
    struct drm_mode_get_connector_compat req;
    struct drm_mode_modeinfo_compat mode;
    uint32 encoder = GPU_DRM_ENCODER_ID;
    int ret;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.connector_id != GPU_DRM_CONNECTOR_ID)
        return -ENOENT;
    gpu_drm_fill_mode(&mode);
    ret = gpu_drm_copyout_u32_array(req.encoders_ptr, req.count_encoders,
                                    &encoder, 1);
    if (ret != 0)
        return ret;
    ret = gpu_drm_copyout_mode_array(req.modes_ptr, req.count_modes, &mode);
    if (ret != 0)
        return ret;

    req.count_modes = 1;
    req.count_props = 0;
    req.count_encoders = 1;
    req.encoder_id = GPU_DRM_ENCODER_ID;
    req.connector_type = DRM_MODE_CONNECTOR_VIRTUAL;
    req.connector_type_id = 1;
    req.connection = DRM_MODE_CONNECTED;
    req.mm_width = mode.hdisplay / 4;
    req.mm_height = mode.vdisplay / 4;
    req.subpixel = DRM_MODE_SUBPIXEL_UNKNOWN;
    req.pad = 0;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_mode_getproperty(uint64 arg)
{
    struct drm_mode_get_property_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    req.flags = 0;
    req.count_values = 0;
    req.count_enum_blobs = 0;
    memset(req.name, 0, sizeof(req.name));
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return -EINVAL;
}

static int gpu_drm_mode_getblob(uint64 arg)
{
    struct drm_mode_get_blob_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    req.length = 0;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return -EINVAL;
}

static int gpu_drm_mode_getplaneresources(uint64 arg)
{
    struct drm_mode_get_plane_res_compat req;
    uint32 plane_id = GPU_DRM_PRIMARY_PLANE_ID;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.plane_id_ptr != 0 && req.count_planes != 0 &&
        either_copyout(1, req.plane_id_ptr, &plane_id,
                       sizeof(plane_id)) < 0)
        return -EFAULT;
    req.count_planes = 1;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_mode_getplane(uint64 arg)
{
    struct drm_mode_get_plane_compat req;
    uint32 formats[2] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };
    uint32 copy_count;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.plane_id != GPU_DRM_PRIMARY_PLANE_ID)
        return -ENOENT;
    copy_count = req.count_format_types;
    if (copy_count > 2)
        copy_count = 2;
    if (req.format_type_ptr != 0 && copy_count != 0 &&
        either_copyout(1, req.format_type_ptr, formats,
                       copy_count * sizeof(formats[0])) < 0)
        return -EFAULT;
    req.crtc_id = GPU_DRM_CRTC_ID;
    req.fb_id = fb_state.current_kms_fb_id;
    req.possible_crtcs = 1;
    req.gamma_size = 0;
    req.count_format_types = 2;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static struct fb_gpu_kms_fb_entry *gpu_kms_fb_lookup_locked(uint32 fb_id)
{
    for (uint32 i = 0; i < FB_GPU_MAX_KMS_FBS; i++) {
        if (fb_state.kms_fbs[i].in_use && fb_state.kms_fbs[i].fb_id == fb_id)
            return &fb_state.kms_fbs[i];
    }
    return NULL;
}

static int gpu_kms_fb_owner_matches(const struct fb_gpu_kms_fb_entry *fb,
                                    struct fb_gpu_render_owner *owner)
{
    if (fb == NULL || owner == NULL)
        return 0;
    return fb->owner_id == owner->id && fb->owner_tgid == owner->tgid;
}

static void gpu_kms_unpin_bo_locked(uint32 handle, uint64 owner_id,
                                    pid_t owner_tgid)
{
    struct fb_gpu_bo_entry *bo = fb_bo_lookup_locked(handle);

    if (bo == NULL || !fb_bo_owner_matches(bo, owner_id, owner_tgid) ||
        bo->ttm_pin_count == 0)
        return;
    bo->ttm_pin_count--;
    if (bo->ttm_pin_count == 0) {
        if (fb_state.stats.ttm_pinned_bytes >= bo->size)
            fb_state.stats.ttm_pinned_bytes -= bo->size;
        else
            fb_state.stats.ttm_pinned_bytes = 0;
    }
}

static void gpu_kms_destroy_owner_fbs(struct fb_gpu_render_owner *owner)
{
    if (owner == NULL)
        return;

    spin_lock(&fb_state.lock);
    for (uint32 i = 0; i < FB_GPU_MAX_KMS_FBS; i++) {
        struct fb_gpu_kms_fb_entry *fb = &fb_state.kms_fbs[i];

        if (!gpu_kms_fb_owner_matches(fb, owner))
            continue;
        if (fb_state.current_kms_fb_id == fb->fb_id)
            fb_state.current_kms_fb_id = 0;
        gpu_kms_unpin_bo_locked(fb->bo_handle, owner->id, owner->tgid);
        memset(fb, 0, sizeof(*fb));
        if (fb_state.stats.kms_framebuffers > 0)
            fb_state.stats.kms_framebuffers--;
    }
    spin_unlock(&fb_state.lock);
}

static int gpu_drm_mode_addfb2(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_mode_fb_cmd2_compat req;
    struct fb_gpu_bo_entry *bo;
    uint64 min_size;
    uint32 fb_id = 0;
    int ret = 0;

    if (!gpu_drm_is_primary_like(owner))
        return -EOPNOTSUPP;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.width == 0 || req.height == 0 || req.handles[0] == 0 ||
        req.pitches[0] == 0)
        return -EINVAL;
    if ((req.flags & ~(DRM_MODE_FB_MODIFIERS)) != 0)
        return -EINVAL;
    if (req.pixel_format != DRM_FORMAT_XRGB8888 &&
        req.pixel_format != DRM_FORMAT_ARGB8888)
        return -EINVAL;
    for (uint32 i = 1; i < 4; i++) {
        if (req.handles[i] != 0 || req.pitches[i] != 0 ||
            req.offsets[i] != 0 || req.modifier[i] != 0)
            return -EINVAL;
    }
    if ((req.flags & DRM_MODE_FB_MODIFIERS) &&
        req.modifier[0] != DRM_FORMAT_MOD_LINEAR)
        return -EINVAL;
    if (req.pitches[0] < req.width * 4)
        return -EINVAL;
    min_size = (uint64)req.pitches[0] * req.height + req.offsets[0];
    if (min_size < req.offsets[0])
        return -EINVAL;

    bo = fb_bo_get_owned(req.handles[0], owner->id, owner->tgid);
    if (bo == NULL)
        return -ENOENT;
    if (bo->size < min_size) {
        fb_bo_put(bo);
        return -EINVAL;
    }

    spin_lock(&fb_state.lock);
    for (uint32 i = 0; i < FB_GPU_MAX_KMS_FBS; i++) {
        struct fb_gpu_kms_fb_entry *fb = &fb_state.kms_fbs[i];

        if (fb->in_use)
            continue;
        fb_id = fb_state.next_kms_fb_id++;
        if (fb_state.next_kms_fb_id == 0)
            fb_state.next_kms_fb_id = 100;
        memset(fb, 0, sizeof(*fb));
        fb->in_use = 1;
        fb->fb_id = fb_id;
        fb->bo_handle = req.handles[0];
        fb->owner_id = owner->id;
        fb->owner_tgid = owner->tgid;
        fb->width = req.width;
        fb->height = req.height;
        fb->pitch = req.pitches[0];
        fb->pixel_format = req.pixel_format;
        fb->modifier = (req.flags & DRM_MODE_FB_MODIFIERS) ?
            req.modifier[0] : DRM_FORMAT_MOD_LINEAR;
        if (bo->ttm_pin_count == 0)
            fb_state.stats.ttm_pinned_bytes += bo->size;
        bo->ttm_pin_count++;
        fb_state.stats.kms_framebuffers++;
        break;
    }
    spin_unlock(&fb_state.lock);
    fb_bo_put(bo);
    if (fb_id == 0)
        return -ENOSPC;

    req.fb_id = fb_id;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        ret = -EFAULT;
    return ret;
}

static int gpu_drm_mode_rmfb(struct fb_gpu_render_owner *owner, uint64 arg)
{
    uint32 fb_id;
    int ret = -ENOENT;

    if (!gpu_drm_is_primary_like(owner))
        return -EOPNOTSUPP;
    if (either_copyin(&fb_id, 1, arg, sizeof(fb_id)) < 0)
        return -EFAULT;

    spin_lock(&fb_state.lock);
    for (uint32 i = 0; i < FB_GPU_MAX_KMS_FBS; i++) {
        struct fb_gpu_kms_fb_entry *fb = &fb_state.kms_fbs[i];

        if (fb->fb_id != fb_id || !gpu_kms_fb_owner_matches(fb, owner))
            continue;
        if (fb_state.current_kms_fb_id == fb_id)
            fb_state.current_kms_fb_id = 0;
        gpu_kms_unpin_bo_locked(fb->bo_handle, owner->id, owner->tgid);
        memset(fb, 0, sizeof(*fb));
        if (fb_state.stats.kms_framebuffers > 0)
            fb_state.stats.kms_framebuffers--;
        ret = 0;
        break;
    }
    spin_unlock(&fb_state.lock);
    return ret;
}

static int gpu_drm_mode_setcrtc(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_mode_crtc_compat req;
    int ret = 0;

    if (!gpu_drm_is_primary_like(owner))
        return -EOPNOTSUPP;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.crtc_id != GPU_DRM_CRTC_ID)
        return -ENOENT;
    if (req.fb_id != 0) {
        spin_lock(&fb_state.lock);
        ret = gpu_kms_fb_owner_matches(
            gpu_kms_fb_lookup_locked(req.fb_id), owner) ? 0 : -ENOENT;
        if (ret == 0)
            fb_state.current_kms_fb_id = req.fb_id;
        spin_unlock(&fb_state.lock);
    }
    return ret;
}

static int gpu_drm_mode_page_flip(struct fb_gpu_render_owner *owner,
                                  uint64 arg)
{
    struct drm_mode_crtc_page_flip_compat req;
    int ret;

    if (!gpu_drm_is_primary_like(owner))
        return -EOPNOTSUPP;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.crtc_id != GPU_DRM_CRTC_ID || req.reserved != 0 ||
        (req.flags & ~DRM_MODE_PAGE_FLIP_FLAGS) != 0 ||
        ((req.flags & DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE) &&
         (req.flags & DRM_MODE_PAGE_FLIP_TARGET_RELATIVE)))
        return -EINVAL;

    spin_lock(&fb_state.lock);
    ret = gpu_kms_fb_owner_matches(
        gpu_kms_fb_lookup_locked(req.fb_id), owner) ? 0 : -ENOENT;
    if (ret == 0) {
        fb_state.current_kms_fb_id = req.fb_id;
        fb_state.stats.kms_page_flips++;
    }
    spin_unlock(&fb_state.lock);
    return ret;
}

static int gpu_drm_mode_atomic(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_mode_atomic_compat req;

    if (!gpu_drm_is_primary_like(owner))
        return -EOPNOTSUPP;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if ((req.flags & ~DRM_MODE_ATOMIC_FLAGS) != 0 || req.reserved != 0)
        return -EINVAL;
    if (req.count_objs != 0)
        return -EOPNOTSUPP;
    spin_lock(&fb_state.lock);
    fb_state.stats.kms_atomic_commits++;
    spin_unlock(&fb_state.lock);
    return 0;
}

static struct fb_gpu_syncobj_entry *
gpu_syncobj_lookup_locked(uint32 handle, struct fb_gpu_render_owner *owner)
{
    if (handle == 0 || owner == NULL)
        return NULL;
    for (uint32 i = 0; i < FB_GPU_MAX_SYNCOBJS; i++) {
        struct fb_gpu_syncobj_entry *obj = &fb_state.syncobjs[i];

        if (obj->in_use && obj->handle == handle &&
            obj->owner_id == owner->id && obj->owner_tgid == owner->tgid)
            return obj;
    }
    return NULL;
}

static struct fb_gpu_syncobj_state_entry *
gpu_syncobj_state_locked(uint32 state_index)
{
    if (state_index == 0 || state_index > FB_GPU_MAX_SYNCOBJ_STATES)
        return NULL;
    if (!fb_state.syncobj_states[state_index - 1].in_use)
        return NULL;
    return &fb_state.syncobj_states[state_index - 1];
}

static void gpu_syncobj_state_put_locked(uint32 state_index)
{
    struct fb_gpu_syncobj_state_entry *state =
        gpu_syncobj_state_locked(state_index);

    if (state == NULL)
        return;
    if (state->refs > 0)
        state->refs--;
    if (state->refs == 0)
        memset(state, 0, sizeof(*state));
}

static int gpu_syncobj_state_get_locked(uint32 state_index)
{
    struct fb_gpu_syncobj_state_entry *state =
        gpu_syncobj_state_locked(state_index);

    if (state == NULL)
        return -ENOENT;
    state->refs++;
    return 0;
}

static int gpu_syncobj_alloc_state_locked(int signaled,
                                          uint32 *state_index)
{
    if (state_index == NULL)
        return -EINVAL;
    for (uint32 i = 0; i < FB_GPU_MAX_SYNCOBJ_STATES; i++) {
        struct fb_gpu_syncobj_state_entry *state =
            &fb_state.syncobj_states[i];

        if (state->in_use)
            continue;
        memset(state, 0, sizeof(*state));
        state->in_use = 1;
        state->signaled = signaled;
        state->timeline_value = signaled ? 1 : 0;
        *state_index = i + 1;
        return 0;
    }
    return -ENOSPC;
}

static int gpu_syncobj_alloc_handle_locked(struct fb_gpu_render_owner *owner,
                                           uint32 state_index,
                                           uint32 *handle_out)
{
    struct fb_gpu_syncobj_state_entry *state;

    if (owner == NULL || handle_out == NULL)
        return -EINVAL;
    state = gpu_syncobj_state_locked(state_index);
    if (state == NULL)
        return -ENOENT;

    for (uint32 i = 0; i < FB_GPU_MAX_SYNCOBJS; i++) {
        struct fb_gpu_syncobj_entry *obj = &fb_state.syncobjs[i];
        uint32 handle;

        if (obj->in_use)
            continue;
        handle = fb_state.next_syncobj_handle++;
        if (fb_state.next_syncobj_handle == 0)
            fb_state.next_syncobj_handle = 1;
        memset(obj, 0, sizeof(*obj));
        obj->in_use = 1;
        obj->handle = handle;
        obj->owner_id = owner->id;
        obj->owner_tgid = owner->tgid;
        obj->state_index = state_index;
        state->refs++;
        fb_state.stats.syncobj_created++;
        fb_state.stats.syncobj_live++;
        *handle_out = handle;
        return 0;
    }
    return -ENOSPC;
}

static void gpu_syncobj_destroy_handle_locked(struct fb_gpu_syncobj_entry *obj)
{
    uint32 state_index;

    if (obj == NULL || !obj->in_use)
        return;
    state_index = obj->state_index;
    memset(obj, 0, sizeof(*obj));
    if (fb_state.stats.syncobj_live > 0)
        fb_state.stats.syncobj_live--;
    gpu_syncobj_state_put_locked(state_index);
}

static int gpu_syncobj_copy_handle(uint64 base, uint32 index, uint32 *handle)
{
    if (base == 0 || handle == NULL)
        return -EINVAL;
    if (either_copyin(handle, 1, base + (uint64)index * sizeof(uint32),
                      sizeof(*handle)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_syncobj_copy_point(uint64 base, uint32 index, uint64 *point)
{
    if (base == 0 || point == NULL)
        return -EINVAL;
    if (either_copyin(point, 1, base + (uint64)index * sizeof(uint64),
                      sizeof(*point)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_syncobj_write_point(uint64 base, uint32 index, uint64 point)
{
    if (base == 0)
        return -EINVAL;
    if (either_copyout(1, base + (uint64)index * sizeof(uint64),
                       &point, sizeof(point)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_syncobj_create(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_syncobj_create_compat req;
    uint32 handle = 0;
    uint32 state_index = 0;
    int ret;

    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if ((req.flags & ~DRM_SYNCOBJ_CREATE_SIGNALED) != 0)
        return -EINVAL;

    spin_lock(&fb_state.lock);
    ret = gpu_syncobj_alloc_state_locked(
        (req.flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0, &state_index);
    if (ret == 0)
        ret = gpu_syncobj_alloc_handle_locked(owner, state_index, &handle);
    if (ret != 0 && state_index != 0)
        gpu_syncobj_state_put_locked(state_index);
    spin_unlock(&fb_state.lock);
    if (ret != 0)
        return ret;
    req.handle = handle;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_syncobj_destroy(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_syncobj_destroy_compat req;
    struct fb_gpu_syncobj_entry *obj;
    int ret = -ENOENT;

    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;

    spin_lock(&fb_state.lock);
    obj = gpu_syncobj_lookup_locked(req.handle, owner);
    if (obj != NULL) {
        gpu_syncobj_destroy_handle_locked(obj);
        ret = 0;
    }
    spin_unlock(&fb_state.lock);
    return ret;
}

static int gpu_syncobj_handle_to_fd(struct fb_gpu_render_owner *owner,
                                    uint64 arg)
{
    struct drm_syncobj_handle_compat req;
    struct fb_gpu_syncobj_file *sync_file;
    struct fb_gpu_syncobj_entry *obj;
    int fd;
    int ret;

    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.flags != 0 || req.pad != 0)
        return -EINVAL;

    sync_file = kvmalloc(sizeof(*sync_file));
    if (sync_file == NULL)
        return -ENOMEM;
    memset(sync_file, 0, sizeof(*sync_file));

    spin_lock(&fb_state.lock);
    obj = gpu_syncobj_lookup_locked(req.handle, owner);
    if (obj == NULL) {
        spin_unlock(&fb_state.lock);
        kvfree(sync_file);
        return -ENOENT;
    }
    ret = gpu_syncobj_state_get_locked(obj->state_index);
    if (ret == 0)
        sync_file->state_index = obj->state_index;
    spin_unlock(&fb_state.lock);
    if (ret != 0) {
        kvfree(sync_file);
        return ret;
    }

    fd = vfs_custom_fd_alloc(&fb_syncobj_file_ops, sync_file, 0);
    if (fd < 0) {
        spin_lock(&fb_state.lock);
        gpu_syncobj_state_put_locked(sync_file->state_index);
        spin_unlock(&fb_state.lock);
        kvfree(sync_file);
        return fd;
    }
    req.fd = fd;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_syncobj_fd_to_handle(struct fb_gpu_render_owner *owner,
                                    uint64 arg)
{
    struct drm_syncobj_handle_compat req;
    struct fb_gpu_syncobj_file *sync_file;
    struct vfs_file *file;
    uint32 handle = 0;
    int ret;

    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.flags != 0 || req.pad != 0 || req.fd < 0)
        return -EINVAL;

    file = vfs_fdtable_get_file(current->fdtable, req.fd);
    if (file == NULL)
        return -EBADF;
    if (file->ops != &fb_syncobj_file_ops || file->private_data == NULL) {
        vfs_fput(file);
        return -EINVAL;
    }
    sync_file = (struct fb_gpu_syncobj_file *)file->private_data;

    spin_lock(&fb_state.lock);
    ret = gpu_syncobj_alloc_handle_locked(owner, sync_file->state_index,
                                          &handle);
    spin_unlock(&fb_state.lock);
    vfs_fput(file);
    if (ret != 0)
        return ret;

    req.handle = handle;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_syncobj_array_signal_reset(struct fb_gpu_render_owner *owner,
                                          uint64 arg, int signal,
                                          int timeline)
{
    struct drm_syncobj_array_compat arr;
    struct drm_syncobj_timeline_array_compat tl;
    uint32 count;
    uint64 handles_ptr;
    uint64 points_ptr = 0;

    if (owner == NULL)
        return -EBADF;
    if (timeline) {
        if (either_copyin(&tl, 1, arg, sizeof(tl)) < 0)
            return -EFAULT;
        if (tl.flags != 0)
            return -EINVAL;
        count = tl.count_handles;
        handles_ptr = tl.handles;
        points_ptr = tl.points;
    } else {
        if (either_copyin(&arr, 1, arg, sizeof(arr)) < 0)
            return -EFAULT;
        if (arr.pad != 0)
            return -EINVAL;
        count = arr.count_handles;
        handles_ptr = arr.handles;
    }
    if (count == 0 || count > 64)
        return -EINVAL;

    for (uint32 i = 0; i < count; i++) {
        uint32 handle;
        uint64 point = 1;
        int ret = gpu_syncobj_copy_handle(handles_ptr, i, &handle);
        if (ret != 0)
            return ret;
        if (timeline) {
            ret = gpu_syncobj_copy_point(points_ptr, i, &point);
            if (ret != 0)
                return ret;
        }
        struct fb_gpu_syncobj_entry *obj;
        struct fb_gpu_syncobj_state_entry *state;

        spin_lock(&fb_state.lock);
        obj = gpu_syncobj_lookup_locked(handle, owner);
        state = obj != NULL ? gpu_syncobj_state_locked(obj->state_index) :
            NULL;
        if (obj == NULL || state == NULL) {
            spin_unlock(&fb_state.lock);
            return -ENOENT;
        }
        if (signal) {
            state->signaled = 1;
            if (state->timeline_value < point)
                state->timeline_value = point;
            fb_state.stats.syncobj_signals++;
        } else {
            state->signaled = 0;
        }
        spin_unlock(&fb_state.lock);
    }
    return 0;
}

static int gpu_syncobj_wait_common(struct fb_gpu_render_owner *owner,
                                   uint64 arg, int timeline)
{
    struct drm_syncobj_wait_compat wait;
    struct drm_syncobj_timeline_wait_compat twait;
    uint32 count;
    uint64 handles_ptr;
    uint64 points_ptr = 0;
    int first = -1;
    uint32 flags;

    if (owner == NULL)
        return -EBADF;
    if (timeline) {
        if (either_copyin(&twait, 1, arg, sizeof(twait)) < 0)
            return -EFAULT;
        count = twait.count_handles;
        handles_ptr = twait.handles;
        points_ptr = twait.points;
        flags = twait.flags;
    } else {
        if (either_copyin(&wait, 1, arg, sizeof(wait)) < 0)
            return -EFAULT;
        count = wait.count_handles;
        handles_ptr = wait.handles;
        flags = wait.flags;
    }
    if (count == 0 || count > 64 ||
        (flags & ~(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
                   DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
                   DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE |
                   DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE)) != 0)
        return -EINVAL;

    for (uint32 i = 0; i < count; i++) {
        uint32 handle;
        uint64 point = 1;
        int ready;
        int ret = gpu_syncobj_copy_handle(handles_ptr, i, &handle);
        if (ret != 0)
            return ret;
        if (timeline) {
            ret = gpu_syncobj_copy_point(points_ptr, i, &point);
            if (ret != 0)
                return ret;
        }
        struct fb_gpu_syncobj_entry *obj;
        struct fb_gpu_syncobj_state_entry *state;

        spin_lock(&fb_state.lock);
        obj = gpu_syncobj_lookup_locked(handle, owner);
        state = obj != NULL ? gpu_syncobj_state_locked(obj->state_index) :
            NULL;
        if (obj == NULL || state == NULL) {
            spin_unlock(&fb_state.lock);
            return -ENOENT;
        }
        ready = state->signaled && state->timeline_value >= point;
        spin_unlock(&fb_state.lock);
        if (ready && first < 0)
            first = (int)i;
        if (!ready && (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL))
            return -ETIME;
    }
    if (first < 0)
        return -ETIME;
    spin_lock(&fb_state.lock);
    fb_state.stats.syncobj_waits++;
    spin_unlock(&fb_state.lock);
    if (timeline) {
        twait.first_signaled = (uint32)first;
        if (either_copyout(1, arg, &twait, sizeof(twait)) < 0)
            return -EFAULT;
    } else {
        wait.first_signaled = (uint32)first;
        if (either_copyout(1, arg, &wait, sizeof(wait)) < 0)
            return -EFAULT;
    }
    return 0;
}

static int gpu_syncobj_query(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_syncobj_timeline_array_compat req;

    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.count_handles == 0 || req.count_handles > 64 ||
        (req.flags & ~DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) != 0)
        return -EINVAL;
    for (uint32 i = 0; i < req.count_handles; i++) {
        uint32 handle;
        uint64 point;
        int ret = gpu_syncobj_copy_handle(req.handles, i, &handle);
        if (ret != 0)
            return ret;
        struct fb_gpu_syncobj_entry *obj;
        struct fb_gpu_syncobj_state_entry *state;

        spin_lock(&fb_state.lock);
        obj = gpu_syncobj_lookup_locked(handle, owner);
        state = obj != NULL ? gpu_syncobj_state_locked(obj->state_index) :
            NULL;
        if (obj == NULL || state == NULL) {
            spin_unlock(&fb_state.lock);
            return -ENOENT;
        }
        point = state->timeline_value;
        spin_unlock(&fb_state.lock);
        ret = gpu_syncobj_write_point(req.points, i, point);
        if (ret != 0)
            return ret;
    }
    return 0;
}

static void gpu_syncobj_destroy_owner(struct fb_gpu_render_owner *owner)
{
    if (owner == NULL)
        return;
    spin_lock(&fb_state.lock);
    for (uint32 i = 0; i < FB_GPU_MAX_SYNCOBJS; i++) {
        struct fb_gpu_syncobj_entry *obj = &fb_state.syncobjs[i];
        if (!obj->in_use || obj->owner_id != owner->id ||
            obj->owner_tgid != owner->tgid)
            continue;
        gpu_syncobj_destroy_handle_locked(obj);
    }
    spin_unlock(&fb_state.lock);
}

static int gpu_drm_create_dumb(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_mode_create_dumb_compat req;
    uint64 size;
    uint32 npages;
    page_t **pages;
    uint32 handle;
    int ret;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.width == 0 || req.height == 0 || req.bpp == 0 ||
        req.bpp > 64 || req.flags != 0 || req.width > 8192 ||
        req.height > 8192)
        return -EINVAL;

    req.pitch = req.width * ((req.bpp + 7) / 8);
    if (req.pitch == 0 || (uint64)req.pitch > ((uint64)-1) / req.height)
        return -EINVAL;
    size = (uint64)req.pitch * req.height;
    if (size == 0 || size > 256ULL * 1024 * 1024)
        return -EINVAL;
    req.size = PGROUNDUP(size);
    npages = req.size / PGSIZE;

    ret = fb_bo_alloc_pages(npages, &pages);
    if (ret != 0)
        return ret;

    ret = fb_bo_register(owner->id, owner->tgid, req.width, req.height,
                         req.pitch, req.size, pages, npages, &handle);
    if (ret != 0) {
        fb_bo_release_pages(pages, npages);
        return ret;
    }
    req.handle = handle;
    spin_lock(&fb_state.lock);
    fb_state.stats.bo_allocs++;
    fb_state.stats.bo_bytes += req.size;
    spin_unlock(&fb_state.lock);

    if (either_copyout(1, arg, &req, sizeof(req)) < 0) {
        (void)fb_bo_destroy_owned(req.handle, owner->id, owner->tgid);
        return -EFAULT;
    }
    return 0;
}

static int gpu_drm_prime_handle_to_fd(struct fb_gpu_render_owner *owner,
                                      uint64 arg)
{
    struct drm_prime_handle_compat req;
    struct fb_gpu_bo_entry *bo;
    int fd;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    bo = fb_bo_get_owned(req.handle, owner->id, owner->tgid);
    if (bo == NULL)
        return -ENOENT;
    fd = vfs_custom_fd_alloc(&fb_bo_file_ops, bo, 0);
    if (fd < 0) {
        fb_bo_put(bo);
        return fd;
    }
    req.fd = fd;
    spin_lock(&fb_state.lock);
    fb_state.stats.bo_fd_exports++;
    fb_state.stats.bo_fd_live++;
    if (fb_state.stats.bo_fd_live > fb_state.stats.bo_fd_peak)
        fb_state.stats.bo_fd_peak = fb_state.stats.bo_fd_live;
    spin_unlock(&fb_state.lock);
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_prime_fd_to_handle(struct fb_gpu_render_owner *owner,
                                      uint64 arg)
{
    struct drm_prime_handle_compat req;
    struct vfs_file *file;
    struct fb_gpu_bo_entry *bo;
    page_t **pages;
    uint32 handle;
    int ret;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    file = vfs_fdtable_get_file(current->fdtable, req.fd);
    if (file == NULL)
        return -EBADF;
    if (file->ops != &fb_bo_file_ops || file->private_data == NULL) {
        vfs_fput(file);
        return -EINVAL;
    }
    bo = (struct fb_gpu_bo_entry *)file->private_data;
    ret = fb_bo_clone_pages(bo, &pages);
    if (ret != 0) {
        vfs_fput(file);
        return ret;
    }
    ret = fb_bo_register(owner->id, owner->tgid, bo->width, bo->height,
                         bo->pitch, bo->size, pages, bo->npages, &handle);
    if (ret != 0) {
        fb_bo_release_pages(pages, bo->npages);
        vfs_fput(file);
        return ret;
    }
    vfs_fput(file);
    req.handle = handle;
    spin_lock(&fb_state.lock);
    fb_state.stats.bo_fd_imports++;
    spin_unlock(&fb_state.lock);
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_virtgpu_resource_create(struct fb_gpu_render_owner *owner,
                                           uint64 arg, int blob)
{
    struct fb_gpu_virgl_resource_create create;
    uint32 out_handle = 0;
    uint64 out_size = 0;
    uint32 out_stride = 0;
    int ret;

    ret = gpu_owner_ensure_context(owner);
    if (ret != 0)
        return ret;

    memset(&create, 0, sizeof(create));
    create.ctx_id = owner->default_ctx_id;
    if (blob) {
        struct drm_virtgpu_resource_create_blob_compat req;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.size == 0 ||
            (req.blob_mem != VIRTGPU_BLOB_MEM_GUEST &&
             req.blob_mem != VIRTGPU_BLOB_MEM_HOST3D &&
             req.blob_mem != VIRTGPU_BLOB_MEM_HOST3D_GUEST))
            return -EINVAL;
        create.width = req.size > 4096 ? 4096 : (uint32)req.size;
        create.height = (uint32)((req.size + create.width - 1) / create.width);
        create.depth = 1;
        create.array_size = 1;
        create.format = 1; /* B8G8R8A8_UNORM */
        create.bind = 0;
        create.size = req.size;
        ret = virtio_gpu_user_resource_create(owner->id, owner->tgid, &create);
        if (ret != 0)
            return ret;
        if (create.addr != 0 && create.size != 0)
            (void)vm_munmap(current->vm, create.addr, (size_t)create.size);
        req.bo_handle = create.resource_id;
        req.res_handle = create.resource_id;
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    } else {
        struct drm_virtgpu_resource_create_compat req;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        create.target = req.target;
        create.format = req.format;
        create.bind = req.bind;
        create.width = req.width;
        create.height = req.height;
        create.depth = req.depth;
        create.array_size = req.array_size;
        create.last_level = req.last_level;
        create.nr_samples = req.nr_samples;
        create.flags = req.flags;
        create.size = req.size;
        ret = virtio_gpu_user_resource_create(owner->id, owner->tgid, &create);
        if (ret != 0)
            return ret;
        if (create.addr != 0 && create.size != 0)
            (void)vm_munmap(current->vm, create.addr, (size_t)create.size);
        out_handle = create.resource_id;
        out_size = create.size;
        out_stride = req.stride ? req.stride : create.width * 4;
        req.bo_handle = out_handle;
        req.res_handle = out_handle;
        req.size = (uint32)out_size;
        req.stride = out_stride;
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }
}

static int gpu_drm_virtgpu_transfer(struct fb_gpu_render_owner *owner,
                                    uint64 arg, int from_host)
{
    struct drm_virtgpu_3d_transfer_compat req;
    struct fb_gpu_virgl_transfer transfer;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    memset(&transfer, 0, sizeof(transfer));
    transfer.resource_id = req.bo_handle;
    transfer.x = req.box.x;
    transfer.y = req.box.y;
    transfer.z = req.box.z;
    transfer.w = req.box.w;
    transfer.h = req.box.h;
    transfer.d = req.box.d ? req.box.d : 1;
    transfer.level = req.level;
    transfer.offset = req.offset;
    transfer.stride = req.stride;
    transfer.layer_stride = req.layer_stride;
    return virtio_gpu_user_transfer(owner->id, owner->tgid, &transfer,
                                    from_host);
}

static struct pci_device_info *gpu_nouveau_device(void)
{
    struct hyperv_dxg_status dxg;

    if (hyperv_dxg_get_status(&dxg) == 0 &&
        (dxg.global_present || dxg.vgpu_present))
        return NULL;
    return pci_get_nvidia_gpu(0);
}

static int gpu_nouveau_require_device(void)
{
    return gpu_nouveau_device() != NULL ? 0 : -ENODEV;
}

static void gpu_nouveau_stat_inc(uint64 *counter)
{
    spin_lock(&fb_state.lock);
    (*counter)++;
    spin_unlock(&fb_state.lock);
}

static uint64 gpu_nouveau_bar_size(uint32 bar)
{
    if (bar == 0)
        return 0;
    if ((bar & 0x1) != 0)
        return 0;
    return 256ULL * 1024ULL * 1024ULL;
}

static int gpu_nouveau_getparam(uint64 arg)
{
    struct drm_nouveau_getparam_compat req;
    struct pci_device_info *dev = gpu_nouveau_device();

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (dev == NULL) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return -ENODEV;
    }

    switch (req.param) {
    case NOUVEAU_GETPARAM_PCI_VENDOR:
        req.value = dev->vendor_id;
        break;
    case NOUVEAU_GETPARAM_PCI_DEVICE:
        req.value = dev->device_id;
        break;
    case NOUVEAU_GETPARAM_BUS_TYPE:
        req.value = 2;
        break;
    case NOUVEAU_GETPARAM_FB_SIZE:
    case NOUVEAU_GETPARAM_VRAM_BAR_SIZE:
        req.value = gpu_nouveau_bar_size(dev->bar[1]);
        break;
    case NOUVEAU_GETPARAM_AGP_SIZE:
        req.value = 0;
        break;
    case NOUVEAU_GETPARAM_CHIPSET_ID:
        req.value = dev->device_id & 0xff;
        break;
    case NOUVEAU_GETPARAM_VM_VRAM_BASE:
        req.value = 0;
        break;
    case NOUVEAU_GETPARAM_GRAPH_UNITS:
        req.value = 1;
        break;
    case NOUVEAU_GETPARAM_PTIMER_TIME:
        req.value = get_jiffs() * 1000000ULL;
        break;
    case NOUVEAU_GETPARAM_HAS_BO_USAGE:
        req.value = 1;
        break;
    case NOUVEAU_GETPARAM_HAS_PAGEFLIP:
        req.value = 0;
        break;
    case NOUVEAU_GETPARAM_EXEC_PUSH_MAX:
        req.value = 0;
        break;
    case NOUVEAU_GETPARAM_VRAM_USED:
        req.value = 0;
        break;
    case NOUVEAU_GETPARAM_HAS_VMA_TILEMODE:
        req.value = 0;
        break;
    default:
        return -EINVAL;
    }

    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_getparams);
    return 0;
}

static int gpu_nouveau_channel_alloc(struct fb_gpu_render_owner *owner,
                                     uint64 arg)
{
    struct drm_nouveau_channel_alloc_compat req;
    int ret = gpu_nouveau_require_device();

    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return ret;
    }
    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (owner->nouveau_channel != 0)
        return -EBUSY;
    if (req.nr_subchan > 8)
        return -EINVAL;

    owner->nouveau_channel = 1;
    req.channel = 0;
    req.pushbuf_domains = NOUVEAU_GEM_DOMAIN_GART |
                          NOUVEAU_GEM_DOMAIN_MAPPABLE |
                          NOUVEAU_GEM_DOMAIN_COHERENT;
    req.notifier_handle = 0;
    req.nr_subchan = 0;
    memset(req.subchan, 0, sizeof(req.subchan));
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_channel_allocs);
    return 0;
}

static int gpu_nouveau_channel_free(struct fb_gpu_render_owner *owner,
                                    uint64 arg)
{
    struct drm_nouveau_channel_free_compat req;
    int ret = gpu_nouveau_require_device();

    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return ret;
    }
    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.channel != 0 || owner->nouveau_channel == 0)
        return -EINVAL;
    owner->nouveau_channel = 0;
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_channel_frees);
    return 0;
}

static int gpu_nouveau_gem_new(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_nouveau_gem_new_compat req;
    uint64 size;
    uint32 npages;
    uint32 handle;
    page_t **pages;
    int ret = gpu_nouveau_require_device();

    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return ret;
    }
    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.info.handle != 0 || req.info.size == 0 ||
        req.info.size > 64ULL * 1024ULL * 1024ULL ||
        (req.info.domain & ~NOUVEAU_GEM_VALID_DOMAINS) != 0)
        return -EINVAL;
    if (req.align != 0 && (req.align & (req.align - 1)) != 0)
        return -EINVAL;

    size = FB_GPU_ALIGN_UP(req.info.size, PGSIZE);
    npages = size / PGSIZE;
    ret = fb_bo_alloc_pages(npages, &pages);
    if (ret != 0)
        return ret;
    ret = fb_bo_register(owner->id, owner->tgid, (uint32)(size / 4), 1,
                         (uint32)size, size, pages, npages, &handle);
    if (ret != 0) {
        fb_bo_release_pages(pages, npages);
        return ret;
    }

    req.info.handle = handle;
    if (req.info.domain == 0)
        req.info.domain = NOUVEAU_GEM_DOMAIN_GART |
                          NOUVEAU_GEM_DOMAIN_MAPPABLE |
                          NOUVEAU_GEM_DOMAIN_COHERENT;
    if ((req.info.domain & NOUVEAU_GEM_DOMAIN_VRAM) != 0) {
        ret = fb_bo_set_ttm_placement(handle, owner->id, owner->tgid,
                                      FB_TTM_PL_VRAM);
    } else if ((req.info.domain & NOUVEAU_GEM_DOMAIN_GART) != 0) {
        ret = fb_bo_set_ttm_placement(handle, owner->id, owner->tgid,
                                      FB_TTM_PL_TT);
    } else {
        ret = fb_bo_set_ttm_placement(handle, owner->id, owner->tgid,
                                      FB_TTM_PL_SYSTEM);
    }
    if (ret != 0) {
        (void)fb_bo_destroy_owned(handle, owner->id, owner->tgid);
        return ret;
    }
    req.info.size = size;
    req.info.offset = 0;
    req.info.map_handle = GPU_DRM_MMAP_OFFSET(handle);
    if (either_copyout(1, arg, &req, sizeof(req)) < 0) {
        (void)fb_bo_destroy_owned(handle, owner->id, owner->tgid);
        return -EFAULT;
    }
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_gem_news);
    return 0;
}

static int gpu_nouveau_gem_info(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_nouveau_gem_info_compat req;
    struct fb_gpu_bo_entry *bo;
    int ret = gpu_nouveau_require_device();

    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return ret;
    }
    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    bo = fb_bo_get_owned(req.handle, owner->id, owner->tgid);
    if (bo == NULL)
        return -ENOENT;
    if (bo->ttm_mem_type == FB_TTM_MEM_VRAM)
        req.domain = NOUVEAU_GEM_DOMAIN_VRAM;
    else if (bo->ttm_mem_type == FB_TTM_MEM_TT)
        req.domain = NOUVEAU_GEM_DOMAIN_GART;
    else
        req.domain = NOUVEAU_GEM_DOMAIN_CPU;
    req.domain |= NOUVEAU_GEM_DOMAIN_MAPPABLE |
                  NOUVEAU_GEM_DOMAIN_COHERENT;
    req.size = bo->size;
    req.offset = 0;
    req.map_handle = GPU_DRM_MMAP_OFFSET(req.handle);
    req.tile_mode = 0;
    req.tile_flags = 0;
    fb_bo_put(bo);
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_gem_infos);
    return 0;
}

static int gpu_nouveau_cpu_prep(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_nouveau_gem_cpu_prep_compat req;
    struct fb_gpu_bo_entry *bo;
    int ret = gpu_nouveau_require_device();

    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return ret;
    }
    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if ((req.flags & ~(NOUVEAU_GEM_CPU_PREP_NOWAIT |
                       NOUVEAU_GEM_CPU_PREP_WRITE)) != 0)
        return -EINVAL;
    bo = fb_bo_get_owned(req.handle, owner->id, owner->tgid);
    if (bo == NULL)
        return -ENOENT;
    fb_bo_put(bo);
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_cpu_preps);
    return 0;
}

static int gpu_nouveau_cpu_fini(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_nouveau_gem_cpu_fini_compat req;
    struct fb_gpu_bo_entry *bo;
    int ret = gpu_nouveau_require_device();

    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return ret;
    }
    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    bo = fb_bo_get_owned(req.handle, owner->id, owner->tgid);
    if (bo == NULL)
        return -ENOENT;
    fb_bo_put(bo);
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_cpu_finis);
    return 0;
}

static int gpu_nouveau_vm_init(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_nouveau_vm_init_compat req;
    int ret = gpu_nouveau_require_device();

    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return ret;
    }
    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (owner->nouveau_channel != 0)
        return -ENOSYS;
    owner->nouveau_vm_initialized = 1;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_vm_inits);
    return 0;
}

static int gpu_nouveau_pushbuf(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_nouveau_gem_pushbuf_compat req;
    int ret = gpu_nouveau_require_device();

    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return ret;
    }
    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.nr_buffers > NOUVEAU_GEM_MAX_BUFFERS ||
        req.nr_relocs > NOUVEAU_GEM_MAX_RELOCS ||
        req.nr_push > NOUVEAU_GEM_MAX_PUSH)
        return -EINVAL;
    if (req.channel != 0 || owner->nouveau_channel == 0)
        return -EINVAL;
    if (req.nr_buffers == 0 && req.nr_relocs == 0 && req.nr_push == 0) {
        req.vram_available = 0;
        req.gart_available = 256ULL * 1024ULL * 1024ULL;
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_pushbuf_noops);
        return 0;
    }
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_unsupported);
    return -EOPNOTSUPP;
}

static int gpu_nouveau_bind_or_exec(struct fb_gpu_render_owner *owner,
                                    uint64 arg, int exec)
{
    struct drm_nouveau_vm_bind_compat bind_req;
    struct drm_nouveau_exec_compat exec_req;
    int ret = gpu_nouveau_require_device();

    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_ioctl_entries);
    if (ret != 0) {
        gpu_nouveau_stat_inc(&fb_state.stats.nouveau_fail_closed);
        return ret;
    }
    if (owner == NULL)
        return -EBADF;
    if (exec) {
        if (either_copyin(&exec_req, 1, arg, sizeof(exec_req)) < 0)
            return -EFAULT;
        if (exec_req.channel != 0 || owner->nouveau_channel == 0)
            return -EINVAL;
        if (exec_req.push_count == 0 && exec_req.wait_count == 0 &&
            exec_req.sig_count == 0) {
            gpu_nouveau_stat_inc(&fb_state.stats.nouveau_exec_noops);
            return 0;
        }
    } else {
        if (either_copyin(&bind_req, 1, arg, sizeof(bind_req)) < 0)
            return -EFAULT;
        if (!owner->nouveau_vm_initialized)
            return -EINVAL;
        if (bind_req.op_count == 0 && bind_req.wait_count == 0 &&
            bind_req.sig_count == 0 && bind_req.flags == 0) {
            gpu_nouveau_stat_inc(&fb_state.stats.nouveau_vm_bind_noops);
            return 0;
        }
    }
    gpu_nouveau_stat_inc(&fb_state.stats.nouveau_unsupported);
    return -EOPNOTSUPP;
}

static int gpu_drm_ioctl(struct fb_gpu_render_owner *owner, uint64 cmd,
                         uint64 arg)
{
    if (owner == NULL)
        return -EBADF;

    owner->drm.ioctl_count++;
    if (owner->drm.node_type == FB_GPU_DRM_NODE_PRIMARY ||
        owner->drm.node_type == FB_GPU_DRM_NODE_RENDER) {
        spin_lock(&fb_state.lock);
        fb_state.stats.drm_ioctls++;
        spin_unlock(&fb_state.lock);
    }

    switch (cmd) {
    case DRM_IOCTL_VERSION:
        return gpu_drm_version(arg);
    case DRM_IOCTL_GET_UNIQUE:
        return gpu_drm_get_unique(arg);
    case DRM_IOCTL_GET_MAGIC:
        return gpu_drm_get_magic(owner, arg);
    case DRM_IOCTL_AUTH_MAGIC:
        return gpu_drm_auth_magic(owner, arg);
    case DRM_IOCTL_GET_CLIENT:
        return gpu_drm_get_client(owner, arg);
    case DRM_IOCTL_GET_STATS:
        return gpu_drm_get_stats(arg);
    case DRM_IOCTL_SET_VERSION:
        return gpu_drm_set_version(arg);
    case DRM_IOCTL_GET_CAP:
        return gpu_drm_get_cap(arg);
    case DRM_IOCTL_SET_CLIENT_CAP:
        return gpu_drm_set_client_cap(owner, arg);
    case DRM_IOCTL_SET_MASTER:
        return gpu_drm_set_master(owner);
    case DRM_IOCTL_DROP_MASTER:
        return gpu_drm_drop_master(owner);
    case DRM_IOCTL_GEM_CLOSE: {
        struct drm_gem_close_compat req;
        int ret;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        ret = fb_bo_destroy_owned(req.handle, owner->id, owner->tgid);
        if (ret == -ENOENT)
            ret = virtio_gpu_user_resource_destroy(owner->id, owner->tgid,
                                                   req.handle);
        return ret;
    }
    case DRM_IOCTL_PRIME_HANDLE_TO_FD:
        return gpu_drm_prime_handle_to_fd(owner, arg);
    case DRM_IOCTL_PRIME_FD_TO_HANDLE:
        return gpu_drm_prime_fd_to_handle(owner, arg);
    case DRM_IOCTL_WAIT_VBLANK:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_wait_vblank(arg);
    case DRM_IOCTL_MODE_GETRESOURCES:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getresources(arg);
    case DRM_IOCTL_MODE_GETCRTC:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getcrtc(arg);
    case DRM_IOCTL_MODE_SETCRTC:
        return gpu_drm_mode_setcrtc(owner, arg);
    case DRM_IOCTL_MODE_GETENCODER:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getencoder(arg);
    case DRM_IOCTL_MODE_GETCONNECTOR:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getconnector(arg);
    case DRM_IOCTL_MODE_GETPROPERTY:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getproperty(arg);
    case DRM_IOCTL_MODE_GETPROPBLOB:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getblob(arg);
    case DRM_IOCTL_MODE_GETPLANERESOURCES:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getplaneresources(arg);
    case DRM_IOCTL_MODE_GETPLANE:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getplane(arg);
    case DRM_IOCTL_MODE_ADDFB2:
        return gpu_drm_mode_addfb2(owner, arg);
    case DRM_IOCTL_MODE_RMFB:
        return gpu_drm_mode_rmfb(owner, arg);
    case DRM_IOCTL_MODE_PAGE_FLIP:
        return gpu_drm_mode_page_flip(owner, arg);
    case DRM_IOCTL_MODE_ATOMIC:
        return gpu_drm_mode_atomic(owner, arg);
    case DRM_IOCTL_SYNCOBJ_CREATE:
        return gpu_syncobj_create(owner, arg);
    case DRM_IOCTL_SYNCOBJ_DESTROY:
        return gpu_syncobj_destroy(owner, arg);
    case DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD:
        return gpu_syncobj_handle_to_fd(owner, arg);
    case DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE:
        return gpu_syncobj_fd_to_handle(owner, arg);
    case DRM_IOCTL_SYNCOBJ_WAIT:
        return gpu_syncobj_wait_common(owner, arg, 0);
    case DRM_IOCTL_SYNCOBJ_RESET:
        return gpu_syncobj_array_signal_reset(owner, arg, 0, 0);
    case DRM_IOCTL_SYNCOBJ_SIGNAL:
        return gpu_syncobj_array_signal_reset(owner, arg, 1, 0);
    case DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT:
        return gpu_syncobj_wait_common(owner, arg, 1);
    case DRM_IOCTL_SYNCOBJ_QUERY:
        return gpu_syncobj_query(owner, arg);
    case DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL:
        return gpu_syncobj_array_signal_reset(owner, arg, 1, 1);
    case DRM_IOCTL_MODE_CREATE_DUMB:
        return gpu_drm_create_dumb(owner, arg);
    case DRM_IOCTL_MODE_MAP_DUMB:
    {
        struct drm_mode_map_dumb_compat req;
        struct fb_gpu_bo_entry *bo;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.handle == 0)
            return -EINVAL;
        bo = fb_bo_get_owned(req.handle, owner->id, owner->tgid);
        if (bo == NULL)
            return -ENOENT;
        req.offset = GPU_DRM_MMAP_OFFSET(req.handle);
        fb_bo_put(bo);
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_MAP: {
        struct drm_virtgpu_map_compat req;
        uint64 size = 0;
        int ret;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.handle == 0)
            return -EINVAL;
        ret = virtio_gpu_user_resource_info(owner->id, owner->tgid,
                                            req.handle, NULL, NULL, NULL,
                                            &size);
        if (ret != 0) {
            struct fb_gpu_bo_entry *bo =
                fb_bo_get_owned(req.handle, owner->id, owner->tgid);
            if (bo == NULL)
                return ret;
            fb_bo_put(bo);
        }
        req.offset = GPU_DRM_MMAP_OFFSET(req.handle);
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }
    case DRM_IOCTL_MODE_DESTROY_DUMB: {
        struct drm_mode_destroy_dumb_compat req;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        return fb_bo_destroy_owned(req.handle, owner->id, owner->tgid);
    }
    case DRM_IOCTL_VIRTGPU_GETPARAM: {
        struct drm_virtgpu_getparam_compat req;
        uint64 value = 0;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        switch (req.param) {
        case VIRTGPU_PARAM_3D_FEATURES:
        case VIRTGPU_PARAM_CAPSET_QUERY_FIX:
        case VIRTGPU_PARAM_CONTEXT_INIT:
        case VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME:
            value = virtio_gpu_has_virgl() ? 1 : 0;
            break;
        case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs:
            if (virtio_gpu_user_capset_ids(&value) != 0)
                value = 0;
            break;
        case VIRTGPU_PARAM_RESOURCE_BLOB:
        case VIRTGPU_PARAM_HOST_VISIBLE:
            value = 0;
            break;
        default:
            value = 0;
            break;
        }
        if (req.value == 0)
            return -EFAULT;
        if (either_copyout(1, req.value, &value, sizeof(value)) < 0)
            return -EFAULT;
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_CONTEXT_INIT: {
        struct drm_virtgpu_context_init_compat req;
        struct drm_virtgpu_context_set_param_compat param;
        char debug_name[64];
        uint32 capset_id = 0;
        uint32 current_capset = 0;
        uint32 i;
        int ret;

        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.pad != 0 || req.num_params > 16)
            return -EINVAL;

        ret = gpu_drm_current_capset(&current_capset);
        if (ret != 0)
            return ret;
        capset_id = current_capset;
        memcpy(debug_name, "xv6-drm", sizeof("xv6-drm"));

        if (req.num_params != 0 && req.ctx_set_params == 0)
            return -EFAULT;
        for (i = 0; i < req.num_params; i++) {
            uint64 user_param = req.ctx_set_params +
                (uint64)i * sizeof(param);
            if (either_copyin(&param, 1, user_param, sizeof(param)) < 0)
                return -EFAULT;

            switch (param.param) {
            case VIRTGPU_CONTEXT_PARAM_CAPSET_ID:
                if (param.value == 0)
                    return -EINVAL;
                ret = virtio_gpu_user_get_caps_for((uint32)param.value, 0,
                                                   NULL, 0, NULL, NULL, NULL);
                if (ret != 0)
                    return -EINVAL;
                capset_id = (uint32)param.value;
                break;
            case VIRTGPU_CONTEXT_PARAM_DEBUG_NAME:
                ret = gpu_user_debug_name(param.value, debug_name);
                if (ret != 0)
                    return ret;
                break;
            case VIRTGPU_CONTEXT_PARAM_NUM_RINGS:
                if (param.value > 1)
                    return -EINVAL;
                break;
            case VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK:
                if (param.value > 1)
                    return -EINVAL;
                break;
            default:
                return -EINVAL;
            }
        }
        return gpu_owner_create_context(owner, debug_name, capset_id);
    }
    case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE:
        return gpu_drm_virtgpu_resource_create(owner, arg, 0);
    case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB:
        return gpu_drm_virtgpu_resource_create(owner, arg, 1);
    case DRM_IOCTL_VIRTGPU_RESOURCE_INFO: {
        struct drm_virtgpu_resource_info_compat req;
        uint64 size = 0;
        int ret;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        ret = virtio_gpu_user_resource_info(owner->id, owner->tgid,
                                            req.bo_handle, NULL, NULL, NULL,
                                            &size);
        if (ret != 0)
            return ret;
        req.res_handle = req.bo_handle;
        req.size = (uint32)size;
        req.blob_mem = 0;
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST:
        return gpu_drm_virtgpu_transfer(owner, arg, 1);
    case DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST:
        return gpu_drm_virtgpu_transfer(owner, arg, 0);
    case DRM_IOCTL_VIRTGPU_WAIT: {
        struct drm_virtgpu_3d_wait_compat req;
        uint64 signaled = 0;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        return virtio_gpu_user_fence(0, !(req.flags & VIRTGPU_WAIT_NOWAIT),
                                     &signaled);
    }
    case DRM_IOCTL_VIRTGPU_GET_CAPS: {
        struct drm_virtgpu_get_caps_compat req;
        uint32 capset_id = 0, capset_ver = 0, capset_size = 0;
        uint32 copy_size;
        void *caps = NULL;
        int ret;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;

        ret = virtio_gpu_user_get_caps_for(req.cap_set_id, req.cap_set_ver,
                                           NULL, 0, &capset_id, &capset_ver,
                                           &capset_size);
        if (ret == -ENODEV) {
            capset_id = 0;
            capset_ver = 0;
            capset_size = 0;
            ret = 0;
        }
        if (ret != 0)
            return ret;

        if (req.addr != 0 && req.size != 0) {
            caps = kalloc();
            if (caps == NULL)
                return -ENOMEM;
        }
        if (caps != NULL)
            ret = virtio_gpu_user_get_caps_for(capset_id, capset_ver, caps,
                                               req.size, NULL, NULL, NULL);
        copy_size = capset_size;
        if (copy_size > req.size)
            copy_size = req.size;
        if (ret == 0 && caps != NULL && copy_size != 0 &&
            either_copyout(1, req.addr, caps, copy_size) < 0)
            ret = -EFAULT;
        if (caps != NULL)
            kfree(caps);
        if (ret != 0)
            return ret;
        req.cap_set_id = capset_id;
        req.cap_set_ver = capset_ver;
        req.size = capset_size;
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_EXECBUFFER: {
        struct drm_virtgpu_execbuffer_compat req;
        uint32 *cmds;
        uint64 fence = 0, signaled = 0;
        int ret;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        ret = gpu_owner_ensure_context(owner);
        if (ret != 0)
            return ret;
        if (req.command == 0 || req.size == 0 || req.size > PGSIZE * 64 ||
            (req.size & 3) != 0)
            return -EINVAL;
        cmds = kvmalloc(req.size);
        if (cmds == NULL)
            return -ENOMEM;
        if (either_copyin(cmds, 1, req.command, req.size) < 0) {
            kvfree(cmds);
            return -EFAULT;
        }
        ret = virtio_gpu_user_submit(owner->id, owner->tgid,
                                     owner->default_ctx_id, 0, cmds,
                                     req.size / sizeof(uint32), &fence,
                                     &signaled);
        kvfree(cmds);
        return ret;
    }
    case DRM_IOCTL_NOUVEAU_GETPARAM:
        return gpu_nouveau_getparam(arg);
    case DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC:
        return gpu_nouveau_channel_alloc(owner, arg);
    case DRM_IOCTL_NOUVEAU_CHANNEL_FREE:
        return gpu_nouveau_channel_free(owner, arg);
    case DRM_IOCTL_NOUVEAU_GEM_NEW:
        return gpu_nouveau_gem_new(owner, arg);
    case DRM_IOCTL_NOUVEAU_GEM_INFO:
        return gpu_nouveau_gem_info(owner, arg);
    case DRM_IOCTL_NOUVEAU_GEM_CPU_PREP:
        return gpu_nouveau_cpu_prep(owner, arg);
    case DRM_IOCTL_NOUVEAU_GEM_CPU_FINI:
        return gpu_nouveau_cpu_fini(owner, arg);
    case DRM_IOCTL_NOUVEAU_GEM_PUSHBUF:
        return gpu_nouveau_pushbuf(owner, arg);
    case DRM_IOCTL_NOUVEAU_VM_INIT:
        return gpu_nouveau_vm_init(owner, arg);
    case DRM_IOCTL_NOUVEAU_VM_BIND:
        return gpu_nouveau_bind_or_exec(owner, arg, 0);
    case DRM_IOCTL_NOUVEAU_EXEC:
        return gpu_nouveau_bind_or_exec(owner, arg, 1);
    default:
        spin_lock(&fb_state.lock);
        fb_state.stats.drm_unknown_ioctls++;
        spin_unlock(&fb_state.lock);
        printf("DRM: unknown ioctl node=%s owner=%lu tgid=%d cmd=0x%lx\n",
               gpu_drm_node_name(owner->drm.node_type),
               owner->id, owner->tgid, cmd);
        return -EINVAL;
    }
}

static int gpu_fops_release(struct vfs_inode *inode, struct vfs_file *file)
{
    struct fb_gpu_render_owner *owner =
        file ? (struct fb_gpu_render_owner *)file->private_data : NULL;

    (void)inode;
    if (file != NULL)
        file->private_data = NULL;
    if (owner != NULL) {
        spin_lock(&fb_state.lock);
        if (fb_state.drm_master_owner_id == owner->id)
            fb_state.drm_master_owner_id = 0;
        spin_unlock(&fb_state.lock);
        gpu_kms_destroy_owner_fbs(owner);
        gpu_syncobj_destroy_owner(owner);
        fb_gpu_destroy_render_owner(owner->id);
        virtio_gpu_user_destroy_render_owner(owner->id);
        (void)gpu_release_node(owner->drm.node_type);
        kvfree(owner);
        return 0;
    }
    return gpu_release(&gpu_cdev);
}

static int gpu_fops_ioctl(struct vfs_file *file, uint64 cmd, void *arg)
{
    struct fb_gpu_render_owner *owner =
        file ? (struct fb_gpu_render_owner *)file->private_data : NULL;
    int trace = fb_gpu_trace_enabled() && fb_gpu_trace_process();
    int ret;

    if (owner == NULL)
        return -EBADF;
    if (trace)
        printf("webkit-gpu: enter pid=%d name=%s owner=%lu:%d cmd=0x%lx(%s)\n",
               current ? current->pid : -1,
               current ? current->name : "?", owner->id, owner->tgid, cmd,
               fb_gpu_ioctl_name(cmd));
    switch (cmd) {
    case FB_GPU_GET_STATS:
    case FB_GPU_BO_CREATE:
    case FB_GPU_BO_DESTROY:
    case FB_GPU_BO_IMPORT:
    case FB_GPU_BO_EXPORT_FD:
    case FB_GPU_BO_IMPORT_FD:
    case FB_GPU_BO_INFO:
    case FB_GPU_DXG_PRESENT_SOURCE_REGISTER:
    case FB_GPU_DXG_PRESENT_SOURCE_COMMIT:
    case FB_GPU_DXG_PRESENT_SOURCE_QUERY:
    case FB_GPU_BO_FENCE:
    case FB_GPU_FENCE_EXPORT_FD:
    case FB_GPU_FENCE_QUERY:
    case FB_GPU_VIRGL_CTX_CREATE:
    case FB_GPU_VIRGL_CTX_DESTROY:
    case FB_GPU_VIRGL_SUBMIT:
    case FB_GPU_VIRGL_FENCE:
    case FB_GPU_VIRGL_FENCE_EXPORT_FD:
    case FB_GPU_VIRGL_FENCE_QUERY_FD:
    case FB_GPU_VIRGL_GET_CAPS:
    case FB_GPU_VIRGL_RESOURCE_CREATE:
    case FB_GPU_VIRGL_RESOURCE_DESTROY:
    case FB_GPU_VIRGL_RESOURCE_EXPORT_FD:
    case FB_GPU_VIRGL_TRANSFER_TO_HOST:
    case FB_GPU_VIRGL_TRANSFER_FROM_HOST:
    case FB_GPU_DISPLAY_PROBE:
    case FB_GPU_BACKEND_QUERY:
        break;
    default:
        ret = gpu_drm_ioctl(owner, cmd, (uint64)arg);
        if (trace)
            printf("webkit-gpu: exit pid=%d name=%s owner=%lu:%d cmd=0x%lx(%s) ret=%d\n",
                   current ? current->pid : -1,
                   current ? current->name : "?", owner->id, owner->tgid, cmd,
                   fb_gpu_ioctl_name(cmd), ret);
        return ret;
    }

    spin_lock(&fb_state.lock);
    fb_state.stats.gpu_ioctls++;
    spin_unlock(&fb_state.lock);
    ret = fb_ioctl_for_owner(&gpu_cdev, cmd, arg, owner->id, owner->tgid);
    if (trace)
        printf("webkit-gpu: exit pid=%d name=%s owner=%lu:%d cmd=0x%lx(%s) ret=%d\n",
               current ? current->pid : -1,
               current ? current->name : "?", owner->id, owner->tgid, cmd,
               fb_gpu_ioctl_name(cmd), ret);
    return ret;
}

static void *gpu_fops_fault(struct vfs_file *file, struct vma *vma, uint64 va)
{
    struct fb_gpu_render_owner *owner =
        file ? (struct fb_gpu_render_owner *)file->private_data : NULL;
    uint64 off;
    uint32 handle;
    uint64 page_index;
    void *pa;

    if (owner == NULL || vma == NULL || va < vma->start)
        return NULL;

    off = vma->pgoff + (va - vma->start);
    handle = GPU_DRM_MMAP_HANDLE(off);
    page_index = GPU_DRM_MMAP_PAGE(off);
    if (handle == 0)
        return NULL;

    pa = fb_bo_page_for_owner(handle, owner->id, owner->tgid, page_index);
    if (pa != NULL)
        return pa;
    return virtio_gpu_user_resource_page(owner->id, owner->tgid, handle,
                                         page_index);
}

static struct vfs_file_ops gpu_file_ops = {
    .release = gpu_fops_release,
    .ioctl   = gpu_fops_ioctl,
    .fault   = gpu_fops_fault,
};

static int gpu_open_file(cdev_t *cdev, struct vfs_file *file)
{
    struct fb_gpu_render_owner *owner;
    int ret = gpu_open(cdev);
    if (ret != 0)
        return ret;
    owner = kvmalloc(sizeof(*owner));
    if (owner == NULL) {
        (void)gpu_release(cdev);
        return -ENOMEM;
    }
    memset(owner, 0, sizeof(*owner));
    owner->id = gpu_alloc_render_owner_id();
    owner->tgid = current ? current->tgid : 0;
    owner->drm.dev = &fb_drm_device;
    owner->drm.node_type = gpu_drm_node_from_cdev(cdev);
    owner->drm.magic = gpu_alloc_drm_magic();
    owner->drm.authenticated =
        owner->drm.node_type != FB_GPU_DRM_NODE_PRIMARY;
    file->ops = &gpu_file_ops;
    file->private_data = owner;
    printf("DRM: open node=%s owner=%lu tgid=%d magic=%u\n",
           gpu_drm_node_name(owner->drm.node_type), owner->id, owner->tgid,
           owner->drm.magic);
    return 0;
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

static cdev_t gpu_cdev = {
    .dev = {
        .major = GPU_MAJOR,
        .minor = GPU_MINOR,
        .devname = "gpu0",
        .devmode = S_IFCHR | 0666,
    },
    .readable = 1,
    .writable = 1,
    .ops = {
        .read    = NULL,
        .write   = NULL,
        .open    = gpu_open,
        .release = gpu_release,
        .ioctl   = gpu_ioctl,
        .poll    = NULL,
        .open_file = gpu_open_file,
    },
};

static cdev_t gpu_primary_cdev = {
    .dev = {
        .major = DRM_PRIMARY_MAJOR,
        .minor = DRM_PRIMARY_MINOR,
        .devname = "dri/card0",
        .devmode = S_IFCHR | 0666,
    },
    .readable = 1,
    .writable = 1,
    .ops = {
        .read    = NULL,
        .write   = NULL,
        .open    = gpu_open,
        .release = gpu_release,
        .ioctl   = gpu_ioctl,
        .poll    = NULL,
        .open_file = gpu_open_file,
    },
};

static cdev_t gpu_render_cdev = {
    .dev = {
        .major = DRM_RENDER_MAJOR,
        .minor = DRM_RENDER_MINOR,
        .devname = "dri/renderD128",
        .devmode = S_IFCHR | 0666,
    },
    .readable = 1,
    .writable = 1,
    .ops = {
        .read    = NULL,
        .write   = NULL,
        .open    = gpu_open,
        .release = gpu_release,
        .ioctl   = gpu_ioctl,
        .poll    = NULL,
        .open_file = gpu_open_file,
    },
};

static bool fb_cdev_registered;
static bool gpu_cdev_registered;
static bool gpu_primary_cdev_registered;
static bool gpu_render_cdev_registered;

static int fb_register_cdev(void)
{
    int ret;

    if (fb_cdev_registered)
        return 0;
    ret = cdev_register(&fb_cdev);
    if (ret == 0)
        fb_cdev_registered = true;
    return ret;
}

static int gpu_register_base_devices(void)
{
    int ret;

    if (!gpu_cdev_registered) {
        ret = cdev_register(&gpu_cdev);
        if (ret != 0)
            return ret;
        gpu_cdev_registered = true;
        printf("GPU: registered /dev/gpu0 (render facade)\n");
    }

    if (!gpu_primary_cdev_registered) {
        ret = cdev_register(&gpu_primary_cdev);
        if (ret != 0)
            return ret;
        gpu_primary_cdev_registered = true;
        printf("GPU: registered /dev/dri/card0 (DRM primary facade)\n");
    }

    return 0;
}

int fb_init_virtio_gpu_scanout(uint32 width, uint32 height)
{
    uint64 size;
    void *buf;
    int ret;

    if (width < 640 || width > 2560 || height < 400 || height > 1600)
        return -EINVAL;

    spin_lock(&fb_state.lock);
    if (fb_state.detected && !fb_state.firmware_backed) {
        spin_unlock(&fb_state.lock);
        return 0;
    }
    spin_unlock(&fb_state.lock);

    size = (uint64)width * height * 4;
    if (size == 0 || size > 64ULL * 1024 * 1024)
        return -EINVAL;

    buf = kvmalloc((size_t)size);
    if (buf == NULL)
        return -ENOMEM;
    memset(buf, 0, (size_t)size);

    spin_lock(&fb_state.lock);
    if (fb_state.detected && !fb_state.firmware_backed) {
        spin_unlock(&fb_state.lock);
        kvfree(buf);
        return 0;
    }
    fb_state.virtio_backed = 1;
    fb_state.firmware_backed = 0;
    fb_state.fb_phys = 0;
    fb_state.fb_virt = (volatile uint8 *)buf;
    fb_state.scanout_mappable = 0;
    fb_state.xres = width;
    fb_state.yres = height;
    fb_state.bpp = 32;
    fb_state.pitch = width * 4;
    fb_state.fb_size = (uint32)size;
    __sync_synchronize();
    fb_state.detected = 1;
    spin_unlock(&fb_state.lock);

    ret = fb_register_cdev();
    if (ret != 0)
        return ret;

    printf("FB: registered /dev/fb0 (virtio-gpu shadow %dx%dx32)\n",
           width, height);
    virtio_gpu_present_fb_rect(fb_state.fb_virt, fb_state.pitch,
                               0, 0, width, height);
    return 0;
}

int fb_init_virtio_gpu_scanout_backing(uint32 width, uint32 height,
                                       void *backing, uint32 backing_size,
                                       uint32 pitch)
{
    uint64 size;
    int ret;

    if (width < 640 || width > 2560 || height < 400 || height > 1600 ||
        backing == NULL || pitch < width * 4 || (pitch & 3) != 0)
        return -EINVAL;

    size = (uint64)pitch * height;
    if (size == 0 || size > backing_size || size > 64ULL * 1024 * 1024)
        return -EINVAL;

    spin_lock(&fb_state.lock);
    if (fb_state.detected && !fb_state.firmware_backed) {
        spin_unlock(&fb_state.lock);
        return 0;
    }

    fb_state.virtio_backed = 1;
    fb_state.firmware_backed = 0;
    fb_state.fb_phys = 0;
    fb_state.fb_virt = (volatile uint8 *)backing;
    fb_state.scanout_mappable =
        fb_kernel_range_has_pages((uint64)(uintptr_t)backing, size);
    fb_state.xres = width;
    fb_state.yres = height;
    fb_state.bpp = 32;
    fb_state.pitch = pitch;
    fb_state.fb_size = (uint32)size;
    __sync_synchronize();
    fb_state.detected = 1;
    spin_unlock(&fb_state.lock);

    ret = fb_register_cdev();
    if (ret != 0)
        return ret;

    printf("FB: registered /dev/fb0 (virtio-gpu direct %dx%dx32 pitch=%u)\n",
           width, height, pitch);
    virtio_gpu_present_fb_rect(fb_state.fb_virt, fb_state.pitch,
                               0, 0, width, height);
    return 0;
}

int fb_replace_virtio_gpu_scanout_backing(uint32 width, uint32 height,
                                          void *backing, uint32 backing_size,
                                          uint32 pitch)
{
    uint64 size;

    if (width < 640 || width > 2560 || height < 400 || height > 1600 ||
        backing == NULL || pitch < width * 4 || (pitch & 3) != 0)
        return -EINVAL;

    size = (uint64)pitch * height;
    if (size == 0 || size > backing_size || size > 64ULL * 1024 * 1024)
        return -EINVAL;

    spin_lock(&fb_state.lock);
    if (!fb_state.detected || !fb_state.virtio_backed) {
        spin_unlock(&fb_state.lock);
        return -ENODEV;
    }

    fb_state.firmware_backed = 0;
    fb_state.fb_phys = 0;
    fb_state.fb_virt = (volatile uint8 *)backing;
    fb_state.scanout_mappable =
        fb_kernel_range_has_pages((uint64)(uintptr_t)backing, size);
    fb_state.xres = width;
    fb_state.yres = height;
    fb_state.bpp = 32;
    fb_state.pitch = pitch;
    fb_state.fb_size = (uint32)size;
    __sync_synchronize();
    spin_unlock(&fb_state.lock);

    printf("FB: updated /dev/fb0 (virtio-gpu direct %dx%dx32 pitch=%u)\n",
           width, height, pitch);
    return 0;
}

int fb_gpu_register_render_node(void)
{
    int ret;
    struct fb_gpu_backend_info backend;

    ret = gpu_register_base_devices();
    if (ret != 0)
        return ret;

    if (!gpu_render_cdev_registered) {
        ret = cdev_register(&gpu_render_cdev);
        if (ret != 0)
            return ret;
        gpu_render_cdev_registered = true;
        gpu_backend_fill(&backend);
        printf("GPU: registered /dev/dri/renderD128 (%s render node: %s)\n",
               backend.name, backend.renderer);
    }

    return 0;
}

static int fb_kernel_range_has_pages(uint64 base, uint64 size)
{
    uint64 end;

    if (size == 0)
        return 0;
    end = base + size;
    if (end < base)
        return 0;
    for (uint64 addr = base; addr < end; addr += PGSIZE) {
        uint64 pa;

        if (addr >= (uint64)PA2VA(KERNBASE) &&
            addr < (uint64)PA2VA(PHYSTOP)) {
            pa = VA2PA(addr);
        } else if (kernel_vm != NULL && kernel_vm->pagetable != NULL) {
            pte_t *pte = walk(kernel_vm->pagetable, addr, 0, NULL, NULL);

            if (pte == NULL || !pte_present(pte))
                return 0;
            pa = pte_pa(pte) + (addr & (PGSIZE - 1));
        } else {
            return 0;
        }
        if (__pa_to_page(pa) == NULL)
            return 0;
    }
    return 1;
}

static int fb_init_firmware_framebuffer(void)
{
    uint64 size;

    if (!platform.has_framebuffer)
        return -ENODEV;
    if (platform.framebuffer_bpp != 32 ||
        platform.framebuffer_width < 320 ||
        platform.framebuffer_height < 200 ||
        platform.framebuffer_pitch < platform.framebuffer_width * 4)
        return -EINVAL;

    size = (uint64)platform.framebuffer_pitch * platform.framebuffer_height;
    if (size == 0 || size > platform.framebuffer_size ||
        size > 0xffffffffULL)
        return -EINVAL;

    spin_lock(&fb_state.lock);
    if (fb_state.detected) {
        spin_unlock(&fb_state.lock);
        return 0;
    }

    fb_state.virtio_backed = 0;
    fb_state.firmware_backed = 1;
    fb_state.fb_phys = platform.framebuffer_base;
    fb_state.fb_virt = (volatile uint8 *)PA2VA(platform.framebuffer_base);
    /*
     * Firmware framebuffers, including Hyper-V synthvid, are often MMIO-like
     * PFN ranges outside the managed page allocator.  They can still be mapped
     * as PFNMAP VMAs; requiring struct page coverage rejects the fast scanout
     * path before the mapper can do the right thing.
     */
    fb_state.scanout_mappable = fb_state.fb_phys != 0;
    fb_state.xres = platform.framebuffer_width;
    fb_state.yres = platform.framebuffer_height;
    fb_state.bpp = platform.framebuffer_bpp;
    fb_state.pitch = platform.framebuffer_pitch;
    fb_state.fb_size = (uint32)size;
    fb_paint_boot_logo();
    fb_state.detected = 1;
    spin_unlock(&fb_state.lock);

    printf("FB: registered firmware framebuffer 0x%lx %ux%ux%u pitch=%u mappable=%d\n",
           fb_state.fb_phys, fb_state.xres, fb_state.yres,
           fb_state.bpp, fb_state.pitch, fb_state.scanout_mappable);
    return 0;
}

void fbdevinit(void)
{
    if (!fb_state.detected)
        fb_init_firmware_framebuffer();

    if (!fb_state.detected) {
        printf("FB: no Bochs VGA detected, skipping /dev/fb0\n");
    } else {
        if (!fb_cdev_registered) {
            int ret = fb_register_cdev();
            assert(ret == 0, "fbdevinit: failed to register fb cdev: %d", ret);
        }

        printf("FB: registered /dev/fb0 (%dx%dx%d)\n",
               fb_state.xres, fb_state.yres, fb_state.bpp);
    }

    int ret = fb_gpu_register_render_node();
    if (ret != 0)
        assert(ret == 0, "fbdevinit: failed to register gpu devices: %d", ret);
    if (!virtio_gpu_has_virgl())
        printf("GPU: virgl unavailable; exposing dumb-buffer DRM only\n");
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

void fb_get_resolution(uint32 *xres, uint32 *yres)
{
    if (xres)
        *xres = 0;
    if (yres)
        *yres = 0;
}

void fbdevinit(void) {}

int fb_gpu_register_render_node(void) { return -ENODEV; }

void fb_panic_screen(const char *text) { (void)text; }

#endif /* __x86_64__ */
