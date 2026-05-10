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
#include <mm/page.h>
#include <mm/pgtable.h>
#include <mm/rmap.h>
#include <mm/vm.h>
#include <proc/thread.h>
#include <printf.h>
#include <lock/spinlock.h>
#include <cmdline.h>
#include <vfs/file.h>
#include <vfs/poll.h>
#include <vfs/vfs_types.h>

#if defined(__x86_64__) || defined(__i386__)

#define FB_GPU_MAX_BOS 128

#define DRM_IOCTL_VERSION                      0xc0406400UL
#define DRM_IOCTL_GET_UNIQUE                   0xc0106401UL
#define DRM_IOCTL_GET_MAGIC                    0x80046402UL
#define DRM_IOCTL_GET_CLIENT                   0xc0286405UL
#define DRM_IOCTL_GET_STATS                    0x80f86406UL
#define DRM_IOCTL_SET_VERSION                  0xc0106407UL
#define DRM_IOCTL_GET_CAP                      0xc010640cUL
#define DRM_IOCTL_SET_CLIENT_CAP               0x4010640dUL
#define DRM_IOCTL_AUTH_MAGIC                   0x40046411UL
#define DRM_IOCTL_SET_MASTER                   0x0000641eUL
#define DRM_IOCTL_DROP_MASTER                  0x0000641fUL
#define DRM_IOCTL_GEM_CLOSE                    0x40086409UL
#define DRM_IOCTL_PRIME_HANDLE_TO_FD           0xc00c642dUL
#define DRM_IOCTL_PRIME_FD_TO_HANDLE           0xc00c642eUL
#define DRM_IOCTL_WAIT_VBLANK                  0xc018643aUL
#define DRM_IOCTL_MODE_GETRESOURCES            0xc04064a0UL
#define DRM_IOCTL_MODE_GETCRTC                 0xc06864a1UL
#define DRM_IOCTL_MODE_GETENCODER              0xc01464a6UL
#define DRM_IOCTL_MODE_GETCONNECTOR            0xc05064a7UL
#define DRM_IOCTL_MODE_GETPROPERTY             0xc04064aaUL
#define DRM_IOCTL_MODE_GETPROPBLOB             0xc01064acUL
#define DRM_IOCTL_MODE_CREATE_DUMB             0xc02064b2UL
#define DRM_IOCTL_MODE_MAP_DUMB                0xc01064b3UL
#define DRM_IOCTL_MODE_DESTROY_DUMB            0xc00464b4UL
#define DRM_IOCTL_MODE_GETPLANERESOURCES       0xc01064b5UL
#define DRM_IOCTL_VIRTGPU_MAP                  0xc0106441UL
#define DRM_IOCTL_VIRTGPU_EXECBUFFER           0xc0406442UL
#define DRM_IOCTL_VIRTGPU_GETPARAM             0xc0106443UL
#define DRM_IOCTL_VIRTGPU_RESOURCE_CREATE      0xc0386444UL
#define DRM_IOCTL_VIRTGPU_RESOURCE_INFO        0xc0106445UL
#define DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST   0xc02c6446UL
#define DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST     0xc02c6447UL
#define DRM_IOCTL_VIRTGPU_WAIT                 0xc0086448UL
#define DRM_IOCTL_VIRTGPU_GET_CAPS             0xc0186449UL
#define DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB 0xc030644aUL
#define DRM_IOCTL_VIRTGPU_CONTEXT_INIT         0xc010644bUL

#define DRM_CAP_DUMB_BUFFER           0x1
#define DRM_CAP_VBLANK_HIGH_CRTC      0x2
#define DRM_CAP_DUMB_PREFERRED_DEPTH  0x3
#define DRM_CAP_DUMB_PREFER_SHADOW    0x4
#define DRM_CAP_PRIME                 0x5
#define DRM_PRIME_CAP_IMPORT          0x1
#define DRM_PRIME_CAP_EXPORT          0x2
#define DRM_CAP_TIMESTAMP_MONOTONIC   0x6
#define DRM_CAP_ASYNC_PAGE_FLIP       0x7
#define DRM_CAP_CURSOR_WIDTH          0x8
#define DRM_CAP_CURSOR_HEIGHT         0x9
#define DRM_CAP_ADDFB2_MODIFIERS      0x10
#define DRM_CAP_PAGE_FLIP_TARGET      0x11
#define DRM_CAP_CRTC_IN_VBLANK_EVENT  0x12
#define DRM_CAP_SYNCOBJ               0x13
#define DRM_CAP_SYNCOBJ_TIMELINE      0x14
#define DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP 0x15

#define VIRTGPU_PARAM_3D_FEATURES             1
#define VIRTGPU_PARAM_CAPSET_QUERY_FIX        2
#define VIRTGPU_PARAM_RESOURCE_BLOB           3
#define VIRTGPU_PARAM_HOST_VISIBLE            4
#define VIRTGPU_PARAM_CONTEXT_INIT            6
#define VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs    7
#define VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME     8
#define VIRTGPU_WAIT_NOWAIT                   1
#define VIRTGPU_BLOB_MEM_GUEST                0x0001
#define VIRTGPU_BLOB_MEM_HOST3D               0x0002
#define VIRTGPU_BLOB_MEM_HOST3D_GUEST         0x0003
#define VIRTGPU_BLOB_FLAG_USE_MAPPABLE        0x0001
#define VIRTGPU_BLOB_FLAG_USE_SHAREABLE       0x0002
#define VIRTGPU_CONTEXT_PARAM_CAPSET_ID       0x0001
#define VIRTGPU_CONTEXT_PARAM_NUM_RINGS       0x0002
#define VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK 0x0003
#define VIRTGPU_CONTEXT_PARAM_DEBUG_NAME      0x0004

#define DRM_MODE_TYPE_PREFERRED               (1 << 3)
#define DRM_MODE_TYPE_DRIVER                  (1 << 6)
#define DRM_MODE_FLAG_NHSYNC                  (1 << 1)
#define DRM_MODE_FLAG_NVSYNC                  (1 << 3)
#define DRM_MODE_ENCODER_VIRTUAL              5
#define DRM_MODE_CONNECTOR_VIRTUAL            15
#define DRM_MODE_CONNECTED                    1
#define DRM_MODE_SUBPIXEL_UNKNOWN             1
#define GPU_DRM_CRTC_ID                       1
#define GPU_DRM_ENCODER_ID                    2
#define GPU_DRM_CONNECTOR_ID                  3

#define GPU_DRM_MMAP_HANDLE_SHIFT 32
#define GPU_DRM_MMAP_OFFSET(handle) ((uint64)(handle) << GPU_DRM_MMAP_HANDLE_SHIFT)
#define GPU_DRM_MMAP_HANDLE(offset) ((uint32)((offset) >> GPU_DRM_MMAP_HANDLE_SHIFT))
#define GPU_DRM_MMAP_PAGE(offset) \
    (((offset) & ((1ULL << GPU_DRM_MMAP_HANDLE_SHIFT) - 1)) >> PGSHIFT)

struct drm_version_compat {
    int version_major;
    int version_minor;
    int version_patchlevel;
    uint64 name_len;
    uint64 name;
    uint64 date_len;
    uint64 date;
    uint64 desc_len;
    uint64 desc;
};

struct drm_unique_compat {
    uint64 unique_len;
    uint64 unique;
};

struct drm_auth_compat {
    uint32 magic;
};

struct drm_client_compat {
    int idx;
    int auth;
    uint64 pid;
    uint64 uid;
    uint64 magic;
    uint64 iocs;
};

struct drm_stats_compat {
    uint64 count;
    struct {
        uint64 value;
        uint32 type;
    } data[15];
};

struct drm_set_version_compat {
    int drm_di_major;
    int drm_di_minor;
    int drm_dd_major;
    int drm_dd_minor;
};

struct drm_get_cap_compat {
    uint64 capability;
    uint64 value;
};

struct drm_set_client_cap_compat {
    uint64 capability;
    uint64 value;
};

struct drm_gem_close_compat {
    uint32 handle;
    uint32 pad;
};

struct drm_prime_handle_compat {
    uint32 handle;
    uint32 flags;
    int32 fd;
};

union drm_wait_vblank_compat {
    struct {
        uint32 type;
        uint32 sequence;
        uint64 signal;
    } request;
    struct {
        uint32 type;
        uint32 sequence;
        int64 tval_sec;
        int64 tval_usec;
    } reply;
};

struct drm_mode_modeinfo_compat {
    uint32 clock;
    uint16 hdisplay;
    uint16 hsync_start;
    uint16 hsync_end;
    uint16 htotal;
    uint16 hskew;
    uint16 vdisplay;
    uint16 vsync_start;
    uint16 vsync_end;
    uint16 vtotal;
    uint16 vscan;
    uint32 vrefresh;
    uint32 flags;
    uint32 type;
    char name[32];
};

struct drm_mode_card_res_compat {
    uint64 fb_id_ptr;
    uint64 crtc_id_ptr;
    uint64 connector_id_ptr;
    uint64 encoder_id_ptr;
    uint32 count_fbs;
    uint32 count_crtcs;
    uint32 count_connectors;
    uint32 count_encoders;
    uint32 min_width;
    uint32 max_width;
    uint32 min_height;
    uint32 max_height;
};

struct drm_mode_crtc_compat {
    uint64 set_connectors_ptr;
    uint32 count_connectors;
    uint32 crtc_id;
    uint32 fb_id;
    uint32 x;
    uint32 y;
    uint32 gamma_size;
    uint32 mode_valid;
    struct drm_mode_modeinfo_compat mode;
};

struct drm_mode_get_encoder_compat {
    uint32 encoder_id;
    uint32 encoder_type;
    uint32 crtc_id;
    uint32 possible_crtcs;
    uint32 possible_clones;
};

struct drm_mode_get_connector_compat {
    uint64 encoders_ptr;
    uint64 modes_ptr;
    uint64 props_ptr;
    uint64 prop_values_ptr;
    uint32 count_modes;
    uint32 count_props;
    uint32 count_encoders;
    uint32 encoder_id;
    uint32 connector_id;
    uint32 connector_type;
    uint32 connector_type_id;
    uint32 connection;
    uint32 mm_width;
    uint32 mm_height;
    uint32 subpixel;
    uint32 pad;
};

struct drm_mode_get_property_compat {
    uint64 values_ptr;
    uint64 enum_blob_ptr;
    uint32 prop_id;
    uint32 flags;
    char name[32];
    uint32 count_values;
    uint32 count_enum_blobs;
};

struct drm_mode_get_blob_compat {
    uint32 blob_id;
    uint32 length;
    uint64 data;
};

struct drm_mode_get_plane_res_compat {
    uint64 plane_id_ptr;
    uint32 count_planes;
};

struct drm_mode_create_dumb_compat {
    uint32 height;
    uint32 width;
    uint32 bpp;
    uint32 flags;
    uint32 handle;
    uint32 pitch;
    uint64 size;
};

struct drm_mode_map_dumb_compat {
    uint32 handle;
    uint32 pad;
    uint64 offset;
};

struct drm_mode_destroy_dumb_compat {
    uint32 handle;
};

struct drm_virtgpu_map_compat {
    uint64 offset;
    uint32 handle;
    uint32 pad;
};

struct drm_virtgpu_getparam_compat {
    uint64 param;
    uint64 value;
};

struct drm_virtgpu_resource_create_compat {
    uint32 target;
    uint32 format;
    uint32 bind;
    uint32 width;
    uint32 height;
    uint32 depth;
    uint32 array_size;
    uint32 last_level;
    uint32 nr_samples;
    uint32 flags;
    uint32 bo_handle;
    uint32 res_handle;
    uint32 size;
    uint32 stride;
};

struct drm_virtgpu_resource_info_compat {
    uint32 bo_handle;
    uint32 res_handle;
    uint32 size;
    uint32 blob_mem;
};

struct drm_virtgpu_3d_box_compat {
    uint32 x, y, z, w, h, d;
};

struct drm_virtgpu_3d_transfer_compat {
    uint32 bo_handle;
    struct drm_virtgpu_3d_box_compat box;
    uint32 level;
    uint32 offset;
    uint32 stride;
    uint32 layer_stride;
};

struct drm_virtgpu_3d_wait_compat {
    uint32 handle;
    uint32 flags;
};

struct drm_virtgpu_get_caps_compat {
    uint32 cap_set_id;
    uint32 cap_set_ver;
    uint64 addr;
    uint32 size;
    uint32 pad;
};

struct drm_virtgpu_resource_create_blob_compat {
    uint32 blob_mem;
    uint32 blob_flags;
    uint32 bo_handle;
    uint32 res_handle;
    uint64 size;
    uint32 pad;
    uint32 cmd_size;
    uint64 cmd;
    uint64 blob_id;
};

struct drm_virtgpu_context_set_param_compat {
    uint64 param;
    uint64 value;
};

struct drm_virtgpu_context_init_compat {
    uint32 num_params;
    uint32 pad;
    uint64 ctx_set_params;
};

struct drm_virtgpu_execbuffer_compat {
    uint32 flags;
    uint32 size;
    uint64 command;
    uint64 bo_handles;
    uint32 num_bo_handles;
    int32 fence_fd;
    uint32 ring_idx;
    uint32 syncobj_stride;
    uint32 num_in_syncobjs;
    uint32 num_out_syncobjs;
    uint64 in_syncobjs;
    uint64 out_syncobjs;
};

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
    page_t **pages;
};

struct fb_gpu_fence_file {
    struct fb_gpu_bo_entry *bo;
    uint64 fence;
};

struct fb_gpu_virgl_fence_file {
    uint64 fence;
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
    uint64      fb_phys;        /* physical address of LFB (BAR0), if any */
    volatile uint8 *fb_virt;    /* kernel virtual address of framebuffer */
    uint32      xres;
    uint32      yres;
    uint32      bpp;
    uint32      pitch;          /* bytes per scanline */
    uint32      fb_size;        /* total framebuffer size in bytes */
    struct fb_gpu_stats stats;
    uint32      next_bo_handle;
    uint64      next_bo_fence;
    uint64      next_render_owner_id;
    struct fb_gpu_bo_entry bos[FB_GPU_MAX_BOS];
    spinlock_t  lock;           /* serializes concurrent access */
} fb_state = {
    .next_bo_handle = 1,
    .next_bo_fence = 1,
    .next_render_owner_id = 1,
    .lock = SPINLOCK_INITIALIZED("fb"),
};

static cdev_t gpu_cdev;

struct fb_gpu_render_owner {
    uint64 id;
    pid_t tgid;
    uint32 default_ctx_id;
    uint32 capset_id;
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
    case DRM_IOCTL_MODE_GETENCODER: return "DRM_IOCTL_MODE_GETENCODER";
    case DRM_IOCTL_MODE_GETCONNECTOR: return "DRM_IOCTL_MODE_GETCONNECTOR";
    case DRM_IOCTL_MODE_GETPROPERTY: return "DRM_IOCTL_MODE_GETPROPERTY";
    case DRM_IOCTL_MODE_GETPROPBLOB: return "DRM_IOCTL_MODE_GETPROPBLOB";
    case DRM_IOCTL_MODE_CREATE_DUMB: return "DRM_IOCTL_MODE_CREATE_DUMB";
    case DRM_IOCTL_MODE_MAP_DUMB: return "DRM_IOCTL_MODE_MAP_DUMB";
    case DRM_IOCTL_MODE_DESTROY_DUMB: return "DRM_IOCTL_MODE_DESTROY_DUMB";
    case DRM_IOCTL_MODE_GETPLANERESOURCES: return "DRM_IOCTL_MODE_GETPLANERESOURCES";
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

    uint8 kbuf[4096];
    uint32 max_px = sizeof(kbuf) / 4;
    volatile uint8 *fb = fb_state.fb_virt;

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
        volatile uint8 *dst = fb + (uint64)(cmd.y + row) * pitch +
                              (uint64)cmd.x * 4;
        uint32 remaining = cw;
        uint32 col = 0;

        while (remaining > 0) {
            uint32 chunk = remaining;
            if (chunk > max_px)
                chunk = max_px;
            if (either_copyin(kbuf, 1, src_addr + col * 4, chunk * 4) < 0)
                return -EFAULT;
            if (virtio_backed) {
                memcpy((void *)(dst + (uint64)col * 4), kbuf, chunk * 4);
            } else {
                spin_lock(&fb_state.lock);
                volatile uint32 *dst32 =
                    (volatile uint32 *)(dst + (uint64)col * 4);
                uint32 *src32 = (uint32 *)kbuf;
                for (uint32 p = 0; p < chunk; p++)
                    dst32[p] = src32[p];
                spin_unlock(&fb_state.lock);
            }
            col += chunk;
            remaining -= chunk;
        }
    }
    virtio_gpu_present_fb_rect(fb_state.fb_virt, fb_state.pitch,
                               cmd.x, cmd.y, cw, ch);
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

static int fb_blit_from_bo(struct fb_gpu_bo_entry *bo,
                           struct fb_gpu_bo_present cmd,
                           uint64 *fence_out)
{
    uint32 xres, yres;
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
                              ((uint64)(cmd.y + row) * fb_state.pitch) +
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
            } else if ((((uint64)(uintptr_t)(dst + copied) |
                         (uint64)(uintptr_t)src | chunk) & 3) == 0) {
                spin_lock(&fb_state.lock);
                volatile uint32 *d32 = (volatile uint32 *)(dst + copied);
                uint32 *s32 = (uint32 *)src;
                uint32 words = chunk / sizeof(uint32);

                for (uint32 i = 0; i < words; i++)
                    d32[i] = s32[i];
                spin_unlock(&fb_state.lock);
            } else {
                spin_lock(&fb_state.lock);
                volatile uint8 *d8 = dst + copied;

                for (uint32 i = 0; i < chunk; i++)
                    d8[i] = src[i];
                spin_unlock(&fb_state.lock);
            }

            src_off += chunk;
            copied += chunk;
            remaining -= chunk;
        }
    }

    virtio_gpu_present_fb_rect(fb_state.fb_virt, fb_state.pitch,
                               cmd.x, cmd.y, cw, ch);
    spin_lock(&fb_state.lock);
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
    fb_state.stats.bo_handles++;
    fb_state.stats.bo_live_bytes += size;
    if (fb_state.stats.bo_handles > fb_state.stats.bo_peak_handles)
        fb_state.stats.bo_peak_handles = fb_state.stats.bo_handles;
    if (fb_state.stats.bo_live_bytes > fb_state.stats.bo_peak_bytes)
        fb_state.stats.bo_peak_bytes = fb_state.stats.bo_live_bytes;
    *handle = next;
    spin_unlock(&fb_state.lock);
    return 0;
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
    addr = vm_find_free_range(vm, (size_t)bo->size, 0);
    if (addr == 0) {
        vm_wunlock(vm);
        return -ENOMEM;
    }

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
    (void)cdev;
    spin_lock(&fb_state.lock);
    fb_state.stats.gpu_opens++;
    fb_state.stats.gpu_live_opens++;
    spin_unlock(&fb_state.lock);
    return 0;
}

static int gpu_release(cdev_t *cdev)
{
    (void)cdev;
    spin_lock(&fb_state.lock);
    if (fb_state.stats.gpu_live_opens > 0)
        fb_state.stats.gpu_live_opens--;
    spin_unlock(&fb_state.lock);
    return 0;
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
        strncpy(info.id, fb_state.virtio_backed ? "VirtioGPU" : "BochsVGA",
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

        req.size = PGROUNDUP(size);
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
        if ((req.flags & ~FB_GPU_VIRGL_SUBMIT_FORCE_FAIL) != 0 ||
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

        if (fb_state.virtio_backed)
            return -EOPNOTSUPP;

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
    int has_virgl;
    int ret;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    has_virgl = virtio_gpu_has_virgl();
    req.version_major = 0;
    req.version_minor = 1;
    req.version_patchlevel = 0;
    ret = gpu_copyout_string(req.name, &req.name_len,
                             has_virgl ? "virtio_gpu" : "xv6_gpu");
    if (ret != 0)
        return ret;
    ret = gpu_copyout_string(req.date, &req.date_len, "20260502");
    if (ret != 0)
        return ret;
    ret = gpu_copyout_string(req.desc, &req.desc_len,
                             has_virgl ? "xv6 virtio-gpu virgl render node" :
                                         "xv6 dumb-buffer render node");
    if (ret != 0)
        return ret;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_get_unique(uint64 arg)
{
    struct drm_unique_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (gpu_copyout_string(req.unique, &req.unique_len,
                           "pci:0000:00:04.0") != 0)
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
    case DRM_CAP_SYNCOBJ:
    case DRM_CAP_SYNCOBJ_TIMELINE:
        req.value = 0;
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

    req.magic = owner && owner->id ? (uint32)owner->id : 1;
    if (req.magic == 0)
        req.magic = 1;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_auth_magic(uint64 arg)
{
    struct drm_auth_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    (void)req;
    return 0;
}

static int gpu_drm_get_client(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_client_compat req;

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    if (req.idx != 0)
        return -EINVAL;
    req.auth = 1;
    req.pid = owner ? (uint64)owner->tgid : 0;
    req.uid = 0;
    req.magic = owner ? owner->id : 1;
    spin_lock(&fb_state.lock);
    req.iocs = fb_state.stats.gpu_ioctls;
    spin_unlock(&fb_state.lock);
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
}

static int gpu_drm_get_stats(uint64 arg)
{
    struct drm_stats_compat req;

    memset(&req, 0, sizeof(req));
    req.count = 4;
    spin_lock(&fb_state.lock);
    req.data[0].value = fb_state.stats.gpu_ioctls;
    req.data[1].value = fb_state.stats.bo_allocs;
    req.data[2].value = fb_state.stats.bo_bytes / 1024;
    req.data[3].value = fb_state.stats.fence_fd_polls;
    spin_unlock(&fb_state.lock);
    req.data[0].type = 3; /* _DRM_STAT_IOCTLS */
    req.data[1].type = 8; /* _DRM_STAT_COUNT */
    req.data[2].type = 7; /* _DRM_STAT_BYTE */
    req.data[3].type = 8; /* _DRM_STAT_COUNT */
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

    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    req.count_planes = 0;
    if (either_copyout(1, arg, &req, sizeof(req)) < 0)
        return -EFAULT;
    return 0;
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

static int gpu_drm_ioctl(struct fb_gpu_render_owner *owner, uint64 cmd,
                         uint64 arg)
{
    if (owner == NULL)
        return -EBADF;

    switch (cmd) {
    case DRM_IOCTL_VERSION:
        return gpu_drm_version(arg);
    case DRM_IOCTL_GET_UNIQUE:
        return gpu_drm_get_unique(arg);
    case DRM_IOCTL_GET_MAGIC:
        return gpu_drm_get_magic(owner, arg);
    case DRM_IOCTL_AUTH_MAGIC:
        return gpu_drm_auth_magic(arg);
    case DRM_IOCTL_GET_CLIENT:
        return gpu_drm_get_client(owner, arg);
    case DRM_IOCTL_GET_STATS:
        return gpu_drm_get_stats(arg);
    case DRM_IOCTL_SET_VERSION:
        return gpu_drm_set_version(arg);
    case DRM_IOCTL_GET_CAP:
        return gpu_drm_get_cap(arg);
    case DRM_IOCTL_SET_CLIENT_CAP:
        return 0;
    case DRM_IOCTL_SET_MASTER:
    case DRM_IOCTL_DROP_MASTER:
        return 0;
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
        return gpu_drm_wait_vblank(arg);
    case DRM_IOCTL_MODE_GETRESOURCES:
        return gpu_drm_mode_getresources(arg);
    case DRM_IOCTL_MODE_GETCRTC:
        return gpu_drm_mode_getcrtc(arg);
    case DRM_IOCTL_MODE_GETENCODER:
        return gpu_drm_mode_getencoder(arg);
    case DRM_IOCTL_MODE_GETCONNECTOR:
        return gpu_drm_mode_getconnector(arg);
    case DRM_IOCTL_MODE_GETPROPERTY:
        return gpu_drm_mode_getproperty(arg);
    case DRM_IOCTL_MODE_GETPROPBLOB:
        return gpu_drm_mode_getblob(arg);
    case DRM_IOCTL_MODE_GETPLANERESOURCES:
        return gpu_drm_mode_getplaneresources(arg);
    case DRM_IOCTL_MODE_CREATE_DUMB:
        return gpu_drm_create_dumb(owner, arg);
    case DRM_IOCTL_MODE_MAP_DUMB:
    {
        struct drm_mode_map_dumb_compat req;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        req.offset = GPU_DRM_MMAP_OFFSET(req.handle);
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_MAP: {
        struct drm_virtgpu_map_compat req;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
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
    default:
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
        fb_gpu_destroy_render_owner(owner->id);
        virtio_gpu_user_destroy_render_owner(owner->id);
        kvfree(owner);
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
    owner->id = gpu_alloc_render_owner_id();
    owner->tgid = current ? current->tgid : 0;
    file->ops = &gpu_file_ops;
    file->private_data = owner;
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
    if (fb_state.detected) {
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
    if (fb_state.detected) {
        spin_unlock(&fb_state.lock);
        kvfree(buf);
        return 0;
    }
    fb_state.virtio_backed = 1;
    fb_state.fb_phys = 0;
    fb_state.fb_virt = (volatile uint8 *)buf;
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
    if (fb_state.detected) {
        spin_unlock(&fb_state.lock);
        return 0;
    }

    fb_state.virtio_backed = 1;
    fb_state.fb_phys = 0;
    fb_state.fb_virt = (volatile uint8 *)backing;
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

int fb_gpu_register_virgl_render_node(void)
{
    int ret;

    ret = gpu_register_base_devices();
    if (ret != 0)
        return ret;

    if (!virtio_gpu_has_virgl())
        return gpu_render_cdev_registered ? 0 : -ENODEV;

    if (!gpu_render_cdev_registered) {
        ret = cdev_register(&gpu_render_cdev);
        if (ret != 0)
            return ret;
        gpu_render_cdev_registered = true;
        printf("GPU: registered /dev/dri/renderD128 (DRM render facade)\n");
    }

    return 0;
}

void fbdevinit(void)
{
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

    int ret = fb_gpu_register_virgl_render_node();
    if (ret != 0 && ret != -ENODEV)
        assert(ret == 0, "fbdevinit: failed to register gpu devices: %d", ret);
    if (!virtio_gpu_has_virgl())
        printf("GPU: no virgl render node advertised\n");
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

int fb_gpu_register_virgl_render_node(void) { return -ENODEV; }

void fb_panic_screen(const char *text) { (void)text; }

#endif /* __x86_64__ */
