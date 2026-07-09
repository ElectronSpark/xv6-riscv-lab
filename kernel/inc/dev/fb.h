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
#define FB_GPU_SCANOUT_MAP 0x4629   /* map scanout backing into caller */
#define FB_GPU_SCANOUT_FLUSH 0x462A /* flush a dirty scanout rectangle */
#define FB_GPU_DISPLAY_PROBE 0x462B /* query current, host, and EDID modes */
#define FB_GPU_BACKEND_QUERY 0x462C /* query active render backend */
#define FB_GPU_DISPLAY_WAIT  0x462D /* query/wait display present completion */
#define FB_GPU_BO_INFO       0x462E /* query read-only BO metadata */
#define FB_GPU_DXG_PRESENT_SOURCE_REGISTER 0x462F /* declare opened DXG source */
#define FB_GPU_DXG_PRESENT_SOURCE_COMMIT   0x4630 /* present registered DXG source */
#define FB_GPU_DXG_PRESENT_SOURCE_QUERY    0x4631 /* query fail-closed DXG source */
#define FB_GPU_TTM_VALIDATE  0x4632 /* compat: validate sysmem placement metadata */
#define FB_GPU_DXG_PRESENT_BIND_CONTRACT_QUERY 0x4633 /* query future DXG display bind */
#define FB_GPU_VIRGL_RESOURCE_ATTACH 0x4634 /* attach/import a virgl resource into a context */
#define FB_GPU_SCANOUT_READ 0x4635 /* diagnostic readback of current scanout */
#define FB_GPU_BO_COPY       0x4636 /* copy one virgl-backed BO into another */
#define FB_GPU_PAGE_FLIP     0x4637 /* bind full-screen virgl BO as scanout */
#define FB_GPU_SET_CURSOR    0x4638 /* upload hardware cursor image + hotspot */
#define FB_GPU_MOVE_CURSOR   0x4639 /* move/show/hide the hardware cursor */
#define FB_GPU_TEST_DMABUF_EXPORT_FD 0x463A /* test-only generic dma-buf exporter */

/* Hardware cursor plane (virtio-gpu cursor queue) ── */
#define FB_GPU_CURSOR_MAX_DIM 64    /* virtio-gpu cursor resources are 64x64 */
#define FB_GPU_CURSOR_F_VISIBLE 0x1 /* MOVE_CURSOR: show (clear = hide) */


#define FB_GPU_DXG_DISPLAY_TARGET_NONE          0
/*
 * Display targets reported by FB_GPU_DXG_PRESENT_SOURCE_QUERY.
 * NONE means no native display handoff has completed.  The nonzero values are
 * reserved for built-in GPU-P/DDA lanes only; they must not be reported for
 * CPU/readback, DRI software, or external host-helper paths.
 */
#define FB_GPU_DXG_DISPLAY_TARGET_SYNTHVID_VRAM_D3D12_EXISTING_SYSMEM 1
#define FB_GPU_DXG_DISPLAY_TARGET_RUNTIME_D3D12_RESOURCE              2
#define FB_GPU_DXG_DISPLAY_TARGET_DDA_NOUVEAU_SCANOUT                 3
#define FB_GPU_DXG_PRESENT_MISSING_NONE         0
#define FB_GPU_DXG_PRESENT_MISSING_SCANOUT_BIND 1

#define FB_GPU_DXG_PRESENT_HOST_SYNTHVID        0x1
#define FB_GPU_DXG_PRESENT_HOST_DXG             0x2
#define FB_GPU_DXG_PRESENT_HOST_DDA_NOUVEAU     0x4
#define FB_GPU_DXG_STATE_GLOBAL_PRESENT         0x01
#define FB_GPU_DXG_STATE_GLOBAL_OPEN            0x02
#define FB_GPU_DXG_STATE_VGPU_PRESENT           0x04
#define FB_GPU_DXG_STATE_VGPU_OPEN              0x08
#define FB_GPU_DXG_STATE_D3DKMT_READY           0x10
#define FB_GPU_DXG_STATE_PARAVIRTUALIZED        0x20
#define FB_GPU_DXG_STATE_NO_DISPLAY             0x40
#define FB_GPU_DXG_STATE_NO_SOURCES             0x80
#define FB_GPU_DXG_PRESENT_REJECT_SYNTHVID_GPA_ONLY 0x1
#define FB_GPU_DXG_PRESENT_REJECT_DXG_NO_DISPLAY_BIND 0x2
#define FB_GPU_DXG_PRESENT_REJECT_DDA_ABSENT    0x4
#define FB_GPU_DXG_PRESENT_REJECT_DDA_NO_IMPORT_PATH 0x8
#define FB_GPU_DXG_PRESENT_REJECT_WSL_ENUM_ONLY 0x10

#define FB_GPU_DXG_PRESENT_LANE_NONE            0
#define FB_GPU_DXG_PRESENT_LANE_GPUP_DXG_SCANOUT_BIND 1
#define FB_GPU_DXG_PRESENT_LANE_HELPER_SCANOUT_BIND \
    FB_GPU_DXG_PRESENT_LANE_GPUP_DXG_SCANOUT_BIND
#define FB_GPU_DXG_PRESENT_LANE_DDA_NOUVEAU     2
#define FB_GPU_DXG_PRESENT_BLOCK_NO_TRANSPORT   0x0001
#define FB_GPU_DXG_PRESENT_BLOCK_SYNTHVID_GPA_ONLY 0x0002
#define FB_GPU_DXG_PRESENT_BLOCK_DXG_NO_DISPLAY_BIND 0x0004
#define FB_GPU_DXG_PRESENT_BLOCK_LUID_UNVERIFIED 0x0008
#define FB_GPU_DXG_PRESENT_BLOCK_NO_REGISTERED_SOURCE 0x0010
#define FB_GPU_DXG_PRESENT_BLOCK_RESOURCE_FD_UNVERIFIED 0x0020
#define FB_GPU_DXG_PRESENT_BLOCK_ADAPTER_MISMATCH 0x0040
#define FB_GPU_DXG_PRESENT_BLOCK_NO_COMPLETION 0x0080
#define FB_GPU_DXG_PRESENT_BLOCK_DDA_NO_IMPORT_PATH 0x0100
#define FB_GPU_DXG_PRESENT_BLOCK_WSL_ENUM_ONLY 0x0200
#define FB_GPU_DXG_PRESENT_BLOCK_ALL \
    (FB_GPU_DXG_PRESENT_BLOCK_NO_TRANSPORT | \
     FB_GPU_DXG_PRESENT_BLOCK_SYNTHVID_GPA_ONLY | \
     FB_GPU_DXG_PRESENT_BLOCK_DXG_NO_DISPLAY_BIND | \
     FB_GPU_DXG_PRESENT_BLOCK_LUID_UNVERIFIED | \
     FB_GPU_DXG_PRESENT_BLOCK_NO_REGISTERED_SOURCE | \
     FB_GPU_DXG_PRESENT_BLOCK_RESOURCE_FD_UNVERIFIED | \
     FB_GPU_DXG_PRESENT_BLOCK_ADAPTER_MISMATCH | \
     FB_GPU_DXG_PRESENT_BLOCK_NO_COMPLETION | \
     FB_GPU_DXG_PRESENT_BLOCK_DDA_NO_IMPORT_PATH | \
     FB_GPU_DXG_PRESENT_BLOCK_WSL_ENUM_ONLY)

#define FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_ENUM_ONLY        0x01
#define FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_NO_LINUX_IOCTL   0x02
#define FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_NO_PACKET_SENDER 0x04
#define FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_NO_RESOURCE_BIND 0x08
#define FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_NO_COMPLETION    0x10
#define FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_NO_PRESENT_ID    0x20
#define FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_NO_COMPLETED_ID  0x40
#define FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_ALL \
    (FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_ENUM_ONLY | \
     FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_NO_LINUX_IOCTL | \
     FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_NO_PACKET_SENDER | \
     FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_NO_RESOURCE_BIND | \
     FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_NO_COMPLETION | \
     FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_NO_PRESENT_ID | \
     FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_NO_COMPLETED_ID)

#define FB_GPU_DXG_PRESENT_PROV_DXG_FD          0x0001
#define FB_GPU_DXG_PRESENT_PROV_RESOURCE_FD     0x0002
#define FB_GPU_DXG_PRESENT_PROV_D3DKMT_HANDLES  0x0004
#define FB_GPU_DXG_PRESENT_PROV_DIMENSIONS      0x0008
#define FB_GPU_DXG_PRESENT_PROV_ADAPTER_LUID    0x0010

#define FB_GPU_DXG_PRESENT_ADAPTER_UNKNOWN      0
#define FB_GPU_DXG_PRESENT_ADAPTER_UNVERIFIED   1
#define FB_GPU_DXG_PRESENT_ADAPTER_MATCH        2
#define FB_GPU_DXG_PRESENT_ADAPTER_MISMATCH     3

#define FB_GPU_DXG_PRESENT_GPUP_DDA_TRANSPORT_NONE   0
#define FB_GPU_DXG_PRESENT_GPUP_DDA_TRANSPORT_VMBUS  1
#define FB_GPU_DXG_PRESENT_GPUP_DDA_TRANSPORT_HVSOCK 2
#define FB_GPU_DXG_PRESENT_GPUP_DDA_OP_SCANOUT_BIND  1
#define FB_GPU_DXG_DISPLAY_BIND_SOURCE_NONE          0
#define FB_GPU_DXG_DISPLAY_BIND_SOURCE_NON_WSL_DXGKRNL_EXTENSION 1
#define FB_GPU_DXG_DISPLAY_BIND_SOURCE_DDA_NOUVEAU_NATIVE_DISPLAY 2
#define FB_GPU_DXG_PRESENT_COMPLETION_NONE           0
#define FB_GPU_DXG_PRESENT_COMPLETION_DXG_SYNC_FILE  1
#define FB_GPU_DXG_PRESENT_COMPLETION_DXG_FENCE      2
#define FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY        3
#define FB_GPU_DXG_PRESENT_HELPER_TRANSPORT_NONE \
    FB_GPU_DXG_PRESENT_GPUP_DDA_TRANSPORT_NONE
#define FB_GPU_DXG_PRESENT_HELPER_TRANSPORT_VMBUS \
    FB_GPU_DXG_PRESENT_GPUP_DDA_TRANSPORT_VMBUS
#define FB_GPU_DXG_PRESENT_HELPER_TRANSPORT_HVSOCK \
    FB_GPU_DXG_PRESENT_GPUP_DDA_TRANSPORT_HVSOCK
#define FB_GPU_DXG_PRESENT_HELPER_OP_SCANOUT_BIND \
    FB_GPU_DXG_PRESENT_GPUP_DDA_OP_SCANOUT_BIND

#define FB_GPU_DXG_PRESENT_META_DEVICE       0x0001
#define FB_GPU_DXG_PRESENT_META_RESOURCE     0x0002
#define FB_GPU_DXG_PRESENT_META_ALLOCATION   0x0004
#define FB_GPU_DXG_PRESENT_META_DIMENSIONS   0x0008
#define FB_GPU_DXG_PRESENT_META_FORMAT       0x0010
#define FB_GPU_DXG_PRESENT_META_MODIFIER     0x0020
#define FB_GPU_DXG_PRESENT_META_SYNC_OBJECT  0x0040
#define FB_GPU_DXG_PRESENT_META_FENCE_VALUE  0x0080
#define FB_GPU_DXG_PRESENT_META_ADAPTER_LUID 0x0100

#define FB_GPU_DXG_PRESENT_LIFE_SOURCE_REGISTERED 0x0001
#define FB_GPU_DXG_PRESENT_LIFE_HANDLES_VALID     0x0002
#define FB_GPU_DXG_PRESENT_LIFE_SYNC_VALID        0x0004
#define FB_GPU_DXG_PRESENT_LIFE_HOST_COMPLETION   0x0008
#define FB_GPU_DXG_PRESENT_LIFE_NO_CPU_READBACK   0x0010

#define FB_GPU_DISPLAY_F_HOST_SCANOUT 0x1 /* host/raw virtio scanout is valid */
#define FB_GPU_DISPLAY_F_EDID         0x2 /* preferred_* came from EDID */
#define FB_GPU_DISPLAY_F_HOST_SCALED  0x4 /* host/raw scanout looks scaled */

#define FB_GPU_BACKEND_DUMB       0
#define FB_GPU_BACKEND_VIRGL      1
#define FB_GPU_BACKEND_HYPERV_DXG 2

#define FB_GPU_BACKEND_F_RENDER_NODE    0x0001
#define FB_GPU_BACKEND_F_DUMB_BO        0x0002
#define FB_GPU_BACKEND_F_VIRGL_OPENGL   0x0004
#define FB_GPU_BACKEND_F_DXG_TRANSPORT  0x0008
#define FB_GPU_BACKEND_F_D3DKMT         0x0010
#define FB_GPU_BACKEND_F_OPENGL_SUBMIT  0x0020
#define FB_GPU_BACKEND_F_DXG_SHARED_RESOURCE 0x0040
#define FB_GPU_BACKEND_F_DXG_SAME_ADAPTER    0x0080
#define FB_GPU_BACKEND_F_DXG_NO_READBACK     0x0100
#define FB_GPU_BACKEND_F_DDA_NOUVEAU         0x0200
/*
 * Real GPU compute (D3D12/D3DKMT submit + hardware monitored-fence) is reachable
 * on this backend. This is NOT the same promise as FB_GPU_BACKEND_F_OPENGL_SUBMIT,
 * which specifically guarantees a *presented* OpenGL frame. GPU_COMPUTE only means
 * the runtime D3DKMT path can submit a command buffer that completes on the host
 * GPU and signals a real fence (proven in-guest by user/programs/d3d12probe).
 */
#define FB_GPU_BACKEND_F_GPU_COMPUTE         0x0400

#define FB_GPU_KMS_PRESENT_LANE_NONE         0
#define FB_GPU_KMS_PRESENT_LANE_DUMB         1
#define FB_GPU_KMS_PRESENT_LANE_SYNTHVID     2
#define FB_GPU_KMS_PRESENT_LANE_NOUVEAU_HW   3

#define FB_GPU_KMS_PRESENT_REJECT_NO_NATIVE_DISPLAY   0x0001
#define FB_GPU_KMS_PRESENT_REJECT_NO_NOUVEAU_DISPLAY  0x0002
#define FB_GPU_KMS_PRESENT_REJECT_NO_DISPLAY_CREATE   0x0004
#define FB_GPU_KMS_PRESENT_REJECT_NO_HEADS            0x0008
#define FB_GPU_KMS_PRESENT_REJECT_NO_CONNECTORS       0x0010
#define FB_GPU_KMS_PRESENT_REJECT_NO_VBLANK           0x0020
#define FB_GPU_KMS_PRESENT_REJECT_NO_HW_COMPLETION    0x0040
#define FB_GPU_KMS_PRESENT_REJECT_NO_ATOMIC_PAGEFLIP_BACKEND 0x0080
#define FB_GPU_KMS_PRESENT_REJECT_ALL \
    (FB_GPU_KMS_PRESENT_REJECT_NO_NATIVE_DISPLAY | \
     FB_GPU_KMS_PRESENT_REJECT_NO_NOUVEAU_DISPLAY | \
     FB_GPU_KMS_PRESENT_REJECT_NO_DISPLAY_CREATE | \
     FB_GPU_KMS_PRESENT_REJECT_NO_HEADS | \
     FB_GPU_KMS_PRESENT_REJECT_NO_CONNECTORS | \
     FB_GPU_KMS_PRESENT_REJECT_NO_VBLANK | \
     FB_GPU_KMS_PRESENT_REJECT_NO_HW_COMPLETION | \
     FB_GPU_KMS_PRESENT_REJECT_NO_ATOMIC_PAGEFLIP_BACKEND)

#define FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_NONE 0
#define FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ  1

#define FB_GPU_NOUVEAU_GETPARAM_SOURCE_NONE      0
#define FB_GPU_NOUVEAU_GETPARAM_SOURCE_DDA_PCI   1
#define FB_GPU_NOUVEAU_GETPARAM_SOURCE_SYNTHETIC 2
#define FB_GPU_NOUVEAU_GETPARAM_SOURCE_DRIVER_CAP 3

#define FB_GPU_RESV_SHARED_SLOTS 8
#define FB_GPU_RESV_ATTACH_NONE          0
#define FB_GPU_RESV_ATTACH_DMABUF_EXPORT 1
#define FB_GPU_RESV_ATTACH_DMABUF_IMPORT 2
#define FB_GPU_RESV_ATTACH_PRIME_EXPORT  3
#define FB_GPU_RESV_ATTACH_PRIME_IMPORT  4
#define FB_GPU_RESV_ATTACH_KMS_PIN       5
#define FB_GPU_RESV_ATTACH_KMS_UNPIN     6
#define FB_GPU_RESV_ATTACH_SYNCOBJ_SIGNAL 7
#define FB_GPU_RESV_ATTACH_SYNCOBJ_WAIT   8
#define FB_GPU_RESV_ATTACH_SYNC_FILE_EXPORT 9
#define FB_GPU_RESV_ATTACH_SYNC_FILE_IMPORT 10

#define FB_GPU_BO_F_EXPORTABLE 0x1    /* return a stable kernel handle */
#define FB_GPU_BO_PRESENT_F_VIRGL_COPY 0x1 /* try virgl resource copy */
#define FB_GPU_BO_PRESENT_F_VIRGL_SCANOUT 0x2 /* bind BO resource as scanout */
#define FB_GPU_BO_PRESENT_F_READBACK_FALLBACK 0x80000000u
#define FB_GPU_PAGE_FLIP_F_SCANOUT_REBIND 0x1 /* output: SET_SCANOUT issued */
#define FB_GPU_PAGE_FLIP_F_SCANOUT_CACHED 0x2 /* output: cached scanout set */
#define FB_GPU_BO_FENCE_WAIT 0x1      /* wait_for must be signaled */
#define FB_GPU_FENCE_WAIT 0x1         /* wait for fence fd to signal */
#define FB_GPU_VIRGL_FENCE_WAIT 0x1   /* wait_for must be signaled */
#define FB_GPU_VIRGL_SUBMIT_ASYNC 0x1 /* return once the command is queued */
#define FB_GPU_VIRGL_SUBMIT_ALLOW_IMPORTED_RESOURCES 0x40000000u /* kernel DRM execbuffer adapter */
#define FB_GPU_VIRGL_SUBMIT_FORCE_FAIL 0x80000000u /* test-only context fault */
#define FB_GPU_DISPLAY_WAIT_F_WAIT 0x1 /* wait_for must be complete */
#define FB_GPU_DXG_PRESENT_F_WAIT_SYNC 0x1 /* wait for sync_object/fence_value */

#define FB_GPU_BO_FORMAT_XRGB8888 0x34325258u /* DRM_FORMAT_XRGB8888 */
#define FB_GPU_BO_FORMAT_ARGB8888 0x34325241u /* DRM_FORMAT_ARGB8888 */
#define FB_GPU_BO_FORMAT_NV12     0x3231564eu /* DRM_FORMAT_NV12 */
#define FB_GPU_BO_MOD_LINEAR      0ULL        /* DRM_FORMAT_MOD_LINEAR */

#define FB_GPU_DMABUF_TAG_NONE      0u
#define FB_GPU_DMABUF_TAG_FB_BO     1u
#define FB_GPU_DMABUF_TAG_DRM_PRIME 2u

#define FB_GPU_DMABUF_POLL_WAKE_UNKNOWN           0u
#define FB_GPU_DMABUF_POLL_WAKE_SHARED_ATTACH     1u
#define FB_GPU_DMABUF_POLL_WAKE_EXCLUSIVE_RELEASE 2u
#define FB_GPU_DMABUF_POLL_WAKE_PRESENT_SIGNAL    3u
#define FB_GPU_DMABUF_POLL_WAKE_EXCLUSIVE_ACQUIRE 4u

/*
 * FB_GPU_TTM_* names are frozen private xv6 ABI compatibility labels.
 * They expose sysmem/shmem placement metadata, not a Linux TTM allocator.
 */
#define FB_GPU_TTM_PL_SYSTEM 0x0001u
#define FB_GPU_TTM_PL_TT     0x0002u
#define FB_GPU_TTM_PL_VRAM   0x0004u
#define FB_GPU_TTM_PL_STOLEN 0x0008u

#define FB_GPU_TTM_F_SET_PLACEMENT 0x0001u
#define FB_GPU_TTM_F_PIN           0x0002u
#define FB_GPU_TTM_F_UNPIN         0x0004u
#define FB_GPU_TTM_F_FORCE_EVICT   0x0008u
#define FB_GPU_TTM_F_RESERVE       0x0010u
#define FB_GPU_TTM_F_UNRESERVE     0x0020u
#define FB_GPU_TTM_F_WW_VALIDATE   0x0040u

/* Linux fbdev screen info ABI.  Keep these layouts in sync with linux/fb.h. */
struct fb_bitfield {
    uint32 offset;
    uint32 length;
    uint32 msb_right;
};

#define FB_TYPE_PACKED_PIXELS 0
#define FB_VISUAL_TRUECOLOR   2
#define FB_ACCEL_NONE         0
#define FB_ACTIVATE_NOW       0
#define FB_VMODE_NONINTERLACED 0

/* Variable screen info (returned by FBIOGET_VSCREENINFO) */
struct fb_var_screeninfo {
    uint32 xres;
    uint32 yres;
    uint32 xres_virtual;
    uint32 yres_virtual;
    uint32 xoffset;
    uint32 yoffset;
    uint32 bits_per_pixel;
    uint32 grayscale;
    struct fb_bitfield red;
    struct fb_bitfield green;
    struct fb_bitfield blue;
    struct fb_bitfield transp;
    uint32 nonstd;
    uint32 activate;
    uint32 height;
    uint32 width;
    uint32 accel_flags;
    uint32 pixclock;
    uint32 left_margin;
    uint32 right_margin;
    uint32 upper_margin;
    uint32 lower_margin;
    uint32 hsync_len;
    uint32 vsync_len;
    uint32 sync;
    uint32 vmode;
    uint32 rotate;
    uint32 colorspace;
    uint32 reserved[4];
};

/* Fixed screen info (returned by FBIOGET_FSCREENINFO) */
struct fb_fix_screeninfo {
    char   id[16];
    uint64 smem_start;
    uint32 smem_len;
    uint32 type;
    uint32 type_aux;
    uint32 visual;
    uint16 xpanstep;
    uint16 ypanstep;
    uint16 ywrapstep;
    uint32 line_length;
    uint64 mmio_start;
    uint32 mmio_len;
    uint32 accel;
    uint16 capabilities;
    uint16 reserved[2];
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
    uint32   flags;          /* FB_GPU_BO_PRESENT_F_* */
    uint64   fence;          /* returned completed fence for handle presents */
};

struct fb_gpu_bo_copy {
    uint32   src_handle;
    uint32   dst_handle;
    uint32   src_x;
    uint32   src_y;
    uint32   dst_x;
    uint32   dst_y;
    uint32   w;
    uint32   h;
    uint32   flags;
    uint32   reserved;
    uint64   fence;
};

struct fb_gpu_page_flip {
    uint32   handle;         /* full-screen virgl-backed BO handle */
    uint32   flags;          /* input zero; output FB_GPU_PAGE_FLIP_F_* */
    uint64   fence;          /* returned completed display fence */
};

struct fb_gpu_scanout_map {
    uint32   width;          /* returned width in pixels */
    uint32   height;         /* returned height in pixels */
    uint32   pitch;          /* returned pitch in bytes */
    uint32   reserved;
    uint64   size;           /* returned mapping size in bytes */
    uint64   addr;           /* returned user virtual address */
};

struct fb_gpu_scanout_flush {
    uint32   x;
    uint32   y;
    uint32   w;
    uint32   h;
};

struct fb_gpu_cursor_image {
    uint32   width;          /* <= FB_GPU_CURSOR_MAX_DIM */
    uint32   height;         /* <= FB_GPU_CURSOR_MAX_DIM */
    uint32   hot_x;          /* hotspot within the image */
    uint32   hot_y;
    uint32   flags;          /* reserved, must be zero */
    uint32   reserved;
    uint64   pixels;         /* user ptr to width*height BGRA (0xAARRGGBB) */
};

struct fb_gpu_cursor_move {
    int32    x;              /* cursor hotspot position in scanout pixels */
    int32    y;
    uint32   flags;          /* FB_GPU_CURSOR_F_VISIBLE */
    uint32   reserved;
};

struct fb_gpu_scanout_read {
    uint32   x;
    uint32   y;
    uint32   w;
    uint32   h;
    uint32   pitch;          /* destination pitch in bytes */
    uint32   flags;
    uint64   pixels;         /* userspace BGRA/XRGB destination */
    uint32   screen_width;   /* returned current scanout width */
    uint32   screen_height;  /* returned current scanout height */
    uint32   screen_pitch;   /* returned current scanout pitch */
    uint32   reserved;
};

struct fb_gpu_display_probe {
    uint32   current_width;
    uint32   current_height;
    uint32   current_pitch;
    uint32   host_width;
    uint32   host_height;
    uint32   flags;
    uint32   current_refresh_millihz;
    uint32   host_refresh_millihz;
    uint32   preferred_width;
    uint32   preferred_height;
    uint32   preferred_refresh_millihz;
    uint32   reserved;
};

struct fb_gpu_backend_info {
    uint32 backend;          /* FB_GPU_BACKEND_* */
    uint32 flags;            /* FB_GPU_BACKEND_F_* */
    uint32 capset_id;        /* virgl capset id, if any */
    uint32 capset_version;   /* virgl capset version, if any */
    uint32 capset_size;      /* virgl capset payload bytes, if any */
    uint32 dxg_global_open;  /* Hyper-V DXG global channel open */
    uint32 dxg_vgpu_open;    /* Hyper-V DXG vGPU channel open */
    uint32 dxg_d3dkmt;       /* D3DKMT ioctl layer available */
    uint32 dxg_global_status;
    uint32 dxg_vgpu_status;
    uint32 dxg_global_rx;
    uint32 dxg_vgpu_rx;
    char   name[32];
    char   renderer[64];
};

struct fb_gpu_display_wait {
    uint32   flags;          /* FB_GPU_DISPLAY_WAIT_F_* */
    uint32   refresh_millihz; /* current display refresh estimate */
    uint64   wait_for;       /* 0 means query only */
    uint64   presented;      /* latest issued display-present sequence */
    uint64   completed;      /* latest completed display-present sequence */
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
    uint32   format;         /* returned DRM fourcc */
    uint32   plane_count;    /* returned dma-buf plane count */
    uint64   modifier;       /* returned DRM format modifier */
    uint32   offsets[4];     /* returned plane offsets */
    uint32   strides[4];     /* returned plane strides */
    uint64   implicit_fence; /* returned latest implicit fence */
    uint64   explicit_fence; /* returned latest explicit fence */
};

struct fb_gpu_bo_info {
    uint32   handle;         /* existing BO handle */
    uint32   flags;          /* reserved, must be 0 */
    uint32   width;          /* returned width in pixels */
    uint32   height;         /* returned height in pixels */
    uint32   pitch;          /* returned pitch in bytes */
    uint32   format;         /* FB_GPU_BO_FORMAT_* */
    uint64   modifier;       /* FB_GPU_BO_MOD_* */
    uint64   size;           /* returned mapping size */
    uint64   addr_align;     /* required CPU VA alignment for create/import */
    uint64   size_align;     /* allocation size granularity */
    uint32   page_size;      /* backing page size */
    uint32   reserved;
    uint64   mmap_offset;    /* render-node mmap offset for this handle */
    uint32   plane_count;    /* returned dma-buf plane count */
    uint32   metadata_flags; /* reserved for metadata validity flags */
    uint32   offsets[4];     /* returned plane offsets */
    uint32   strides[4];     /* returned plane strides */
    uint64   implicit_fence; /* returned latest implicit fence */
    uint64   explicit_fence; /* returned latest explicit fence */
    uint32   virtio_resource_id; /* returned backing virgl resource, if any */
    uint32   reserved1;
    uint64   virtio_resource_owner_id;
    int32    virtio_resource_owner_tgid;
    uint32   reserved2;
};

struct fb_gpu_ttm_validate {
    uint32 handle;
    uint32 flags;
    uint32 placement;
    uint32 mem_type;
    uint32 pin_count;
    uint32 tt_populated;
    uint32 sg_nents;
    uint32 peer_handle;     /* optional second BO for ww-acquire validation */
    uint64 size;
    uint64 dma_addr_base;
    uint64 sg_total_len;
    uint64 sg_dma_addr_first;
    uint64 sg_dma_addr_last;
    uint64 reservation_seq;
    uint64 lru_seq;
    uint64 move_count;
    uint64 manager_bytes[4];
    uint64 evictions;
    uint64 metadata_only_moves;
    uint64 real_copy_moves;
    uint64 move_bytes;
    uint64 native_accel_credit;
    uint64 manager_moves[4];
    uint64 cpu_copy_fallback_moves[4];
    uint64 metadata_noop_moves[4];
    uint64 unsupported_hw_copy_moves[4];
    uint64 real_copy_moves_by_domain[4];
    uint64 resv_count;
    uint64 resv_seq;
    uint64 resv_exclusive_fence;
    uint64 resv_waits;
    uint64 resv_conflicts;
    uint64 resv_shared_slots;
    uint64 resv_shared_count;
    uint64 resv_latest_shared_fence;
    uint64 resv_wait_wakeups;
    uint64 resv_stale_fence_rejects;
    uint64 resv_attach_syncobj_signal;
    uint64 resv_attach_syncobj_wait;
    uint64 resv_attach_sync_file_export;
    uint64 resv_attach_sync_file_import;
    uint64 resv_evict_pinned_rejects;
    uint64 resv_evict_busy_rejects;
    uint64 syncobj_wait_queued;
    uint64 syncobj_wait_wakeups;
    uint64 syncobj_timeout_waits;
    uint64 syncobj_stale_wait_rejects;
};

/*
 * Future Hyper-V DXG display handoff ABI.
 *
 * These structs describe an already-opened same-adapter D3DKMT resource and
 * allocation.  They intentionally do not create a D3D12 object from an fb BO;
 * userspace must first import/open the shared DXG resource through /dev/dxg.
 */
struct fb_gpu_dxg_present_source_register {
    int32    dxg_fd;          /* /dev/dxg fd that owns the D3DKMT handles */
    int32    resource_fd;     /* optional anon_inode:dxgresource provenance */
    uint32   device;          /* opened D3DKMT device handle */
    uint32   resource;        /* opened D3DKMT resource handle */
    uint32   allocation;      /* allocation selected for display */
    uint32   allocation_count; /* opened resource allocation count */
    uint32   width;
    uint32   height;
    uint32   pitch;
    uint32   format;         /* FB_GPU_BO_FORMAT_* / DRM fourcc */
    uint64   modifier;       /* FB_GPU_BO_MOD_* */
    uint64   reserved0;
    uint32   flags;          /* reserved, must be 0 */
    uint32   present_source;  /* returned display-local source handle */
    /*
     * Optional diagnostics tail.  Keep the stable register prefix above
     * append-only so older fail-closed callers do not have their reserved
     * flags/present_source fields reinterpreted as provenance metadata.
     */
    uint32   adapter_luid_low; /* optional DXG adapter LUID identity */
    uint32   adapter_luid_high;
    uint32   provenance_flags; /* optional FB_GPU_DXG_PRESENT_PROV_* hints */
    uint32   reserved1;
};

struct fb_gpu_dxg_present_source_commit {
    uint32   present_source;  /* handle returned by DXG_PRESENT_SOURCE_REGISTER */
    uint32   flags;           /* FB_GPU_DXG_PRESENT_F_* */
    uint32   sync_object;     /* optional opened D3DKMT sync object */
    uint32   reserved;
    uint64   fence_value;     /* wait target when WAIT_SYNC is set */
    uint64   present_id;      /* returned display-present sequence */
    uint64   completed;       /* returned completed display sequence */
};

struct fb_gpu_dxg_present_source_query {
    uint32   present_source;  /* optional source handle to inspect */
    uint32   flags;           /* reserved, must be 0 */
    uint32   display_target_kind; /* FB_GPU_DXG_DISPLAY_TARGET_* */
    uint32   source_live;     /* nonzero when present_source is registered */
    uint64   present_id;      /* 0 until GPU-P/DDA display completion exists */
    uint64   completed;       /* 0 until GPU-P/DDA display completion exists */
    uint64   host_handoff_missing; /* commits blocked before GPU-P/DDA bind */
    uint64   requires_host_protocol; /* nonzero while fail-closed */
    uint64   missing_host_abi; /* FB_GPU_DXG_PRESENT_MISSING_* */
    uint64   helper_contract_version; /* legacy name: GPU-P/DDA contract */
    uint64   helper_required_metadata; /* legacy name: required metadata */
    uint64   helper_transport; /* legacy name: GPU-P/DDA transport */
    uint64   helper_transport_present; /* legacy name: transport exists */
    uint64   helper_operation; /* legacy name: GPU-P/DDA operation */
    uint64   helper_lifetime; /* legacy name: GPU-P/DDA lifetime */
    uint64   helper_requires_completion; /* present_id/completed required */
    uint32   device;
    uint32   resource;
    uint32   allocation;
    uint32   allocation_count;
    uint32   sync_object;
    uint32   last_flags;
    uint64   fence_value;
    uint64   last_ret;
    int32    dxg_fd;
    int32    resource_fd;
    uint32   provenance_flags;
    uint32   selected_lane;
    uint32   adapter_luid_low;
    uint32   adapter_luid_high;
    uint32   adapter_identity;
    uint32   helper_block_reason; /* legacy name: GPU-P/DDA block reason */
    uint64   host_candidates;
    uint64   host_rejects;
    uint32   resource_fd_kind; /* 2 means WSL-style dxgresource fd */
    uint32   resource_fd_sealed; /* sealed metadata was visible at admission */
    uint32   resource_fd_matches_handles; /* fd metadata matches D3DKMT handles */
    uint32   resource_fd_shared_records_valid;
    uint64   resource_fd_generation; /* sealed shared-resource generation */
    uint64   source_generation; /* registered source generation */
    uint64   resource_generation; /* display-bind resource generation */
    uint64   display_bind_status; /* last display-bind errno/status */
    uint64   display_bind_block_reason; /* FB_GPU_DXG_PRESENT_BLOCK_* */
    uint64   display_bind_completion_source; /* FB_GPU_DXG_PRESENT_COMPLETION_* */
    uint64   display_bind_dirty_sequence; /* display dirty seq, if any */
    uint64   display_bind_dirty_rects; /* display dirty rect count, if any */
    uint32   display_bind_host_abi_present; /* provider reported host ABI */
    uint32   display_bind_sender_present; /* provider reported sender */
    uint32   display_bind_completion_present; /* provider reported completion */
    uint32   display_bind_provider_no_host_abi; /* provider lacks ABI */
    uint32   display_bind_provider_no_sender; /* provider lacks sender */
    uint32   display_bind_provider_no_completion; /* provider lacks done */
    uint32   display_bind_provider_pin_revalidated; /* provider checked pin */
    uint32   display_bind_transport_source; /* FB_GPU_DXG_DISPLAY_BIND_SOURCE_* */
    uint32   display_bind_host_saw_packet; /* host observed bind packet */
    uint32   display_bind_wsl_presenthistory_completion_credit; /* must be 0 */
    uint32   reserved2;
};

/*
 * Contract for a future GPU-P/DDA resource scanout bind.
 *
 * FB_GPU_DXG_PRESENT_BIND_CONTRACT_QUERY fills this structure and returns
 * -EOPNOTSUPP until a real GPU-P/DDA display lane exists.  The current kernel
 * has no documented GPU-P present packet that binds a D3DKMT resource to the
 * Windows display path, DDA/Nouveau is a separate native PCI path, and
 * synthvid only accepts a guest physical VRAM address plus dirty rectangles.
 * A real GPU-P or DDA lane must consume the same-adapter handles below and
 * return display-correlated present/completion sequence numbers before
 * FB_GPU_DXG_PRESENT_SOURCE_COMMIT can report success.
 */
struct fb_gpu_dxg_present_host_bind_contract {
    uint32 version;       /* set to 1 */
    uint32 transport;     /* FB_GPU_DXG_PRESENT_GPUP_DDA_TRANSPORT_* */
    uint32 operation;     /* FB_GPU_DXG_PRESENT_GPUP_DDA_OP_* */
    uint32 flags;         /* FB_GPU_DXG_PRESENT_F_* */
    uint32 present_source; /* optional registered source to bind */
    uint32 source_live;    /* returned: source still belongs to this open */
    uint32 device;
    uint32 resource;
    uint32 allocation;
    uint32 allocation_count;
    uint32 sync_object;
    uint32 width;
    uint32 height;
    uint32 pitch;
    uint32 format;
    uint32 adapter_luid_low;
    uint32 adapter_luid_high;
    uint32 reserved;
    uint64 modifier;
    uint64 fence_value;
    uint64 present_id;
    uint64 completed;
    uint64 provenance_flags;
    uint64 selected_lane;
    uint64 helper_block_reason; /* legacy name: GPU-P/DDA block reason */
    uint64 required_metadata; /* FB_GPU_DXG_PRESENT_META_* */
    uint64 lifetime;      /* FB_GPU_DXG_PRESENT_LIFE_* */
    uint64 host_candidates; /* FB_GPU_DXG_PRESENT_HOST_* */
    uint64 host_rejects;  /* FB_GPU_DXG_PRESENT_REJECT_* */
    uint64 source_generation; /* stable token for this source registration */
    uint64 resource_generation; /* stable token for resource metadata */
    uint64 completion_source; /* FB_GPU_DXG_PRESENT_COMPLETION_* */
    int32 dxg_fd;
    int32 resource_fd;
    uint32 adapter_identity;
    uint32 resource_fd_kind;
    uint32 resource_fd_sealed;
    uint32 resource_fd_matches_handles;
    uint32 resource_fd_shared_records_valid;
    uint32 reserved2;
    uint64 resource_fd_generation;
    uint64 provider_status; /* last provider errno/status */
    uint64 provider_block_reason; /* FB_GPU_DXG_PRESENT_BLOCK_* */
    uint64 dirty_sequence; /* display dirty seq, if any */
    uint64 dirty_rects; /* display dirty rect count, if any */
    uint32 host_abi_present; /* provider reported host ABI */
    uint32 sender_present; /* provider reported sender */
    uint32 completion_present; /* provider reported completion */
    uint32 provider_no_host_abi; /* provider lacks ABI */
    uint32 provider_no_sender; /* provider lacks sender */
    uint32 provider_no_completion; /* provider lacks done */
    uint32 provider_pin_revalidated; /* provider checked pin */
    uint32 transport_source; /* FB_GPU_DXG_DISPLAY_BIND_SOURCE_* */
    uint32 host_saw_packet; /* host observed bind packet */
    uint32 wsl_presenthistory_completion_credit; /* must be 0 */
    uint32 reserved3;
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
    uint32   resource_count;  /* optional referenced resource id count */
    uint64   cmd;             /* user pointer to uint32 command dwords */
    uint64   fence;           /* returned submitted fence id */
    uint64   signaled;        /* returned latest completed fence id */
    uint64   resources;       /* optional user pointer to uint32 resource ids */
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

struct fb_gpu_virgl_blob_create {
    uint32   ctx_id;
    uint32   resource_id;     /* returned virtio resource id */
    uint32   blob_mem;        /* VIRTGPU_BLOB_MEM_* */
    uint32   blob_flags;      /* VIRTGPU_BLOB_FLAG_* */
    uint64   blob_id;
    uint64   size;
    uint64   addr;            /* returned caller-local mapping */
};

struct fb_gpu_virgl_resource_destroy {
    uint32   resource_id;
    uint32   flags;           /* reserved, must be 0 */
};

struct fb_gpu_virgl_resource_attach {
    uint32   ctx_id;           /* target context */
    uint32   resource_id;      /* existing virgl resource */
    uint32   handle;           /* caller-owned imported BO handle, or 0 */
    uint32   flags;            /* reserved, must be 0 */
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
    uint64 dmabuf_exports;     /* dma-buf-shaped BO/PRIME fd exports */
    uint64 dmabuf_imports;     /* dma-buf-shaped BO/PRIME fd imports */
    uint64 dmabuf_attachments; /* importer attachments created */
    uint64 dmabuf_live_attachments; /* imported BO handles still attached */
    uint64 dmabuf_peak_attachments; /* high-water mark of live attachments */
    uint64 dmabuf_live;        /* currently open dma-buf-shaped fds */
    uint64 dmabuf_peak;        /* high-water mark of open dma-buf fds */
    uint64 dmabuf_releases;    /* dma-buf-shaped fd releases */
    uint64 dmabuf_import_attempts; /* BO/PRIME fd-to-handle attempts */
    uint64 dmabuf_local_imports; /* accepted local fb dma-buf imports */
    uint64 dmabuf_foreign_import_attempts; /* valid but non-local fds */
    uint64 dmabuf_foreign_import_rejects; /* foreign fds rejected */
    uint64 dmabuf_local_only_import_path; /* only fb_dmabuf_file_ops accepted */
    uint64 dmabuf_d3d12_foreign_resource_imports; /* D3D12 foreign imports */
    uint64 dmabuf_nouveau_scanout_bind_imports; /* Nouveau scanout binds */
    uint64 dmabuf_native_present_credit; /* native present credit from import */
    uint64 dmabuf_bad_fd_rejects; /* invalid/closed fd import rejects */
    uint64 dmabuf_foreign_fd_rejects; /* non-dma-buf fd import rejects */
    uint64 dmabuf_resv_snapshots; /* exported/imported reservation snapshots */
    uint64 dmabuf_last_exporter_tag; /* FB_GPU_DMABUF_TAG_* */
    uint64 dmabuf_last_importer_tag; /* FB_GPU_DMABUF_TAG_* */
    uint64 dmabuf_last_ttm_resv_seq; /* last copied GEM reservation seq */
    uint64 dmabuf_last_ttm_resv_exclusive_fence; /* last exclusive fence */
    uint64 dmabuf_poll_semantics; /* nonzero when dma-buf fds implement poll */
    uint64 dmabuf_poll_attempts; /* dma-buf fd poll readiness checks */
    uint64 dmabuf_poll_ready; /* poll checks that reported ready fences */
    uint64 dmabuf_poll_not_ready; /* poll checks blocked by pending fences */
    uint64 dmabuf_poll_errors; /* poll checks rejected bad dma-buf state */
    uint64 dmabuf_poll_last_target_fence; /* last dma-buf poll fence target */
    uint64 dmabuf_poll_last_signaled_fence; /* last dma-buf poll signaled fence */
    uint64 dmabuf_poll_read_ready; /* read-side poll readiness successes */
    uint64 dmabuf_poll_write_ready; /* write-side poll readiness successes */
    uint64 dmabuf_poll_pending; /* requested poll events blocked by fences */
    uint64 dmabuf_poll_last_read_fence; /* last exclusive/write fence */
    uint64 dmabuf_poll_last_write_fence; /* last all-fences write target */
    uint64 dmabuf_poll_real_fence_ready; /* nonzero fence satisfied by poll */
    uint64 dmabuf_poll_callbacks_armed; /* pending poll callback diagnostics */
    uint64 dmabuf_poll_callbacks_fired; /* reservation wakeups that hit polls */
    uint64 dmabuf_poll_pending_to_ready; /* armed pending poll later readied */
    uint64 dmabuf_poll_last_callback_source; /* FB_GPU_DMABUF_POLL_WAKE_* */
    uint64 dmabuf_poll_last_callback_target_fence; /* target armed at wake */
    uint64 dmabuf_poll_last_callback_wakeup_seq; /* reservation wake seq */
    uint64 dmabuf_shared_fence_semantics; /* shared-fence slot capacity */
    uint64 dmabuf_wait_queue_semantics; /* nonzero when waits use wakeups */
    uint64 dmabuf_last_ttm_resv_shared_fence; /* last shared fence copied */
    uint64 dmabuf_last_ttm_resv_shared_count; /* shared slots used */
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
    uint64 drm_primary_opens;  /* /dev/dri/card0 opens */
    uint64 drm_render_opens;   /* /dev/dri/renderD128 opens */
    uint64 drm_primary_live;   /* currently open primary DRM files */
    uint64 drm_render_live;    /* currently open render DRM files */
    uint64 drm_ioctls;         /* DRM ioctl calls through card/render nodes */
    uint64 drm_unknown_ioctls; /* unknown DRM ioctl commands rejected */
    uint64 drm_auths;          /* primary-node auth ioctls accepted */
    uint64 drm_master_sets;    /* primary-node SET_MASTER successes */
    uint64 drm_master_drops;   /* primary-node DROP_MASTER successes */
    uint64 drm_events_queued;  /* KMS events queued on DRM files */
    uint64 drm_events_read;    /* KMS events delivered by DRM read() */
    uint64 drm_event_queue_depth; /* current queued KMS events across files */
    uint64 drm_event_queue_high_water; /* aggregate queue depth high-water */
    uint64 drm_event_file_high_water; /* max depth reached by one DRM file */
    uint64 drm_event_queue_overflows; /* event queue full rejections */
    uint64 drm_event_queue_dropped; /* events not queued because FIFO was full */
    uint64 drm_event_close_stale; /* unread events discarded on file close */
    uint64 kms_vblank_sequence; /* synthetic/display CRTC vblank sequence */
    uint64 kms_vblank_timestamp_ns; /* timestamp for last vblank sample */
    uint64 kms_vblank_last_tick; /* internal 100ns clock sample */
    uint64 kms_vblank_samples; /* vblank samples returned to DRM callers */
    uint64 kms_vblank_page_flip_events; /* page-flip events using vblank source */
    uint64 kms_vblank_synthetic; /* current vblank source is synthetic */
    uint64 kms_vblank_display_correlated; /* source is display-correlated */
    uint64 kms_vblank_source_synthetic; /* last source was synthetic timer */
    uint64 kms_vblank_source_software_display; /* last source was fb completion */
    uint64 kms_vblank_source_nouveau_hw; /* last source was Nouveau HW */
    uint64 kms_page_flip_events_software_blit; /* event after software present */
    uint64 kms_page_flip_events_native_hw; /* event after native HW present */
    uint64 kms_page_flip_target_rejects; /* target flips rejected until real */
    uint64 kms_page_flip_async_rejects; /* async flips rejected until real */
    uint64 kms_page_flip_invalid_noevent_rejects; /* invalid flips no-event */
    uint64 kms_crtc_queue_sequence_rejects; /* queue-sequence unsupported */
    uint64 kms_crtc_queue_sequence_bad_flags; /* invalid sequence flags */
    uint64 kms_crtc_queue_sequence_noevent_rejects; /* rejected before event */
    uint64 drm_file_legacy_opens; /* DRM-core file opens via /dev/gpu0 */
    uint64 drm_file_primary_opens; /* DRM-core file opens via /dev/dri/card0 */
    uint64 drm_file_render_opens; /* DRM-core file opens via renderD128 */
    uint64 drm_file_legacy_closes; /* DRM-core file closes via /dev/gpu0 */
    uint64 drm_file_primary_closes; /* DRM-core file closes via card0 */
    uint64 drm_file_render_closes; /* DRM-core file closes via renderD128 */
    uint64 drm_file_legacy_live; /* live DRM-core legacy files */
    uint64 drm_file_primary_live; /* live DRM-core primary files */
    uint64 drm_file_render_live; /* live DRM-core render files */
    uint64 drm_file_close_generation; /* increments once per DRM file close */
    uint64 drm_file_stale_gem_handles; /* GEM/BO handles reaped on close */
    uint64 drm_file_stale_kms_fbs; /* KMS FB IDs reaped on close */
    uint64 drm_file_stale_syncobjs; /* syncobjs reaped on close */
    uint64 drm_file_stale_events; /* unread DRM events reaped on close */
    uint64 drm_minor_model_version; /* Linux-shaped minor diag version */
    uint64 drm_minor_primary_index; /* primary minor index, card0 => 0 */
    uint64 drm_minor_render_index; /* render minor index, renderD128 => 128 */
    uint64 drm_minor_control_index; /* control minor index, if present */
    uint64 drm_minor_primary_registered; /* primary node exists */
    uint64 drm_minor_render_registered; /* render node exists */
    uint64 drm_minor_control_registered; /* control node exists */
    uint64 drm_minor_static_nodes; /* statically registered DRM nodes */
    uint64 drm_minor_dynamic_nodes; /* dynamically allocated DRM nodes */
    uint64 drm_minor_generation; /* increments when DRM minor model changes */
    uint64 drm_lease_ioctl_attempts; /* recognized DRM lease ioctl attempts */
    uint64 drm_lease_create_rejects; /* CREATE_LEASE fail-closed rejects */
    uint64 drm_lease_create_object_rejects; /* object-list lease rejects */
    uint64 drm_lease_create_empty_rejects; /* empty-object lease rejects */
    uint64 drm_lease_list_rejects; /* LIST_LESSEES fail-closed rejects */
    uint64 drm_lease_get_rejects; /* GET_LEASE fail-closed rejects */
    uint64 drm_lease_revoke_rejects; /* REVOKE_LEASE fail-closed rejects */
    uint64 drm_lease_render_rejects; /* lease ioctls rejected on render node */
    uint64 drm_lease_fds_created; /* lease fd creations, should stay zero */
    uint64 drm_lease_active; /* active leases, should stay zero */
    uint64 nouveau_ioctl_entries; /* Nouveau private ioctl calls entered */
    uint64 nouveau_fail_closed; /* Nouveau ioctls rejected because absent */
    uint64 nouveau_getparams;  /* Nouveau GETPARAM calls accepted */
    uint64 nouveau_getparam_dda_facts; /* GETPARAM facts sourced from DDA PCI */
    uint64 nouveau_getparam_synthetic_facts; /* synthetic/non-DDA GETPARAM facts */
    uint64 nouveau_getparam_driver_caps; /* local driver capability answers */
    uint64 nouveau_getparam_fail_closed; /* GETPARAM rejected because absent */
    uint64 nouveau_getparam_last_source; /* FB_GPU_NOUVEAU_GETPARAM_SOURCE_* */
    uint64 nouveau_channel_allocs; /* Nouveau channel alloc successes */
    uint64 nouveau_channel_frees; /* Nouveau channel free successes */
    uint64 nouveau_channel_active; /* currently active Nouveau channels */
    uint64 nouveau_notifier_allocs; /* legacy notifier objects allocated */
    uint64 nouveau_grobj_allocs; /* legacy graphics objects allocated */
    uint64 nouveau_gpuobj_frees; /* legacy notifier/GROBJ object frees */
    uint64 nouveau_object_rejects; /* invalid/unsupported object requests */
    uint64 nouveau_close_object_reclaims; /* objects reclaimed at fd close */
    uint64 nouveau_nvif_ioctls; /* NVIF ioctl attempts */
    uint64 nouveau_nvif_sclass_queries; /* NVIF SCLASS queries */
    uint64 nouveau_nvif_sclass_count; /* last advertised NVIF class count */
    uint64 nouveau_nvif_new_rejects; /* NVIF NEW class rejects */
    uint64 nouveau_nvif_del_rejects; /* NVIF DEL rejects */
    uint64 nouveau_nvif_unsupported; /* unsupported NVIF operations */
    uint64 nouveau_gem_news;   /* Nouveau GEM_NEW successes */
    uint64 nouveau_gem_infos;  /* Nouveau GEM_INFO successes */
    uint64 nouveau_cpu_preps;  /* Nouveau GEM_CPU_PREP successes */
    uint64 nouveau_cpu_finis;  /* Nouveau GEM_CPU_FINI successes */
    uint64 nouveau_vm_inits;   /* Nouveau VM_INIT successes */
    uint64 nouveau_vm_bind_noops; /* zero-op Nouveau VM_BIND successes */
    uint64 nouveau_pushbuf_noops; /* zero-push Nouveau GEM_PUSHBUF successes */
    uint64 nouveau_exec_noops; /* zero-push Nouveau EXEC successes */
    uint64 nouveau_nonempty_pushbuf_rejects; /* non-empty pushbuf fail-closed */
    uint64 nouveau_nonempty_exec_rejects; /* non-empty EXEC fail-closed */
    uint64 nouveau_nonempty_vm_bind_rejects; /* non-empty VM_BIND fail-closed */
    uint64 nouveau_unsupported; /* recognized but not implemented Nouveau ops */
    uint64 nouveau_pci_registered; /* Nouveau PCI driver registrations */
    uint64 nouveau_pci_probes; /* Nouveau PCI probe attempts */
    uint64 nouveau_pci_probe_failures; /* Nouveau PCI probe failures */
    uint64 nouveau_pci_probe_reject_dxg_present; /* GPU-P/DXG-only device */
    uint64 nouveau_pci_probe_reject_class; /* candidate was not display class */
    uint64 nouveau_pci_probe_reject_no_bars; /* no usable DDA BAR aperture */
    uint64 nouveau_pci_probe_enable_failures; /* pci_enable_device failures */
    uint64 nouveau_pci_probe_accepts; /* accepted DDA/Nouveau PCI functions */
    uint64 nouveau_pci_removes; /* clean Nouveau PCI remove callbacks */
    uint64 nouveau_pci_suspends; /* Nouveau PCI suspend callbacks */
    uint64 nouveau_pci_resumes; /* Nouveau PCI resume callbacks */
    uint64 nouveau_pci_enable_count; /* PCI core enable refcount */
    uint64 nouveau_pci_master_enabled; /* PCI bus mastering state */
    uint64 nouveau_pci_irq_vectors; /* allocated PCI IRQ vector count */
    uint64 nouveau_pci_runtime_suspended; /* PCI runtime suspend state */
    uint64 nouveau_pci_suspend_count; /* PCI core runtime suspend count */
    uint64 nouveau_pci_resume_count; /* PCI core runtime resume count */
    uint64 nouveau_pci_runtime_pm_balanced; /* not suspended with equal counts */
    uint64 nouveau_pci_remove_runtime_suspended; /* remove while suspended */
    uint64 nouveau_pci_remove_calls; /* PCI core remove callbacks entered */
    uint64 nouveau_pci_remove_runtime_resume_attempts; /* resume-before-remove */
    uint64 nouveau_pci_remove_runtime_resume_successes; /* resumed before remove */
    uint64 nouveau_pci_remove_runtime_barriers; /* remove PM barriers */
    uint64 nouveau_pci_remove_active_before_callback; /* remove saw active dev */
    uint64 nouveau_pci_hot_remove_events; /* PCI hot-remove events */
    uint64 nouveau_pci_removed; /* PCI device marked removed */
    uint64 nouveau_pci_bar_iounmaps; /* BAR mappings torn down */
    uint64 nouveau_pci_irq_unregisters; /* IRQ handler unregisters */
    uint64 nouveau_pci_irq_vectors_freed; /* IRQ vectors freed */
    uint64 nouveau_pci_bus_master_clears; /* bus master cleared on teardown */
    uint64 nouveau_pci_device_disables; /* PCI device disables on teardown */
    uint64 nouveau_pci_drvdata_cleared; /* drvdata cleared on teardown */
    uint64 nouveau_pci_bar0_len; /* BAR0 aperture length from PCI core */
    uint64 nouveau_pci_bar1_len; /* BAR1 aperture length from PCI core */
    uint64 nouveau_pci_irq; /* selected legacy IRQ vector, if any */
    uint64 nouveau_pci_irq_pin; /* PCI interrupt pin, if any */
    uint64 nouveau_pci_msi_cap; /* PCI MSI capability offset, if present */
    uint64 nouveau_pci_msix_cap; /* PCI MSI-X capability offset, if present */
    uint64 nouveau_pci_dma_mask_configured; /* streaming DMA mask set */
    uint64 nouveau_pci_dma_mask_requested_bits; /* requested streaming bits */
    uint64 nouveau_pci_dma_mask_bits; /* streaming DMA address bits */
    uint64 nouveau_pci_dma_mask_effective_bits; /* effective streaming bits */
    uint64 nouveau_pci_dma_mask_fallback_32; /* 64-bit streaming fallback */
    uint64 nouveau_pci_coherent_dma_mask_configured; /* coherent mask set */
    uint64 nouveau_pci_coherent_dma_mask_requested_bits; /* requested coherent */
    uint64 nouveau_pci_coherent_dma_mask_bits; /* coherent DMA bits */
    uint64 nouveau_pci_coherent_dma_mask_effective_bits; /* effective coherent */
    uint64 nouveau_pci_coherent_dma_mask_fallback_32; /* 64-bit coherent fallback */
    uint64 nouveau_pci_bar0_claimed; /* BAR0 region claim state */
    uint64 nouveau_pci_bar1_claimed; /* BAR1 region claim state */
    uint64 nouveau_pci_bar_claim_failures; /* BAR claim failures */
    uint64 nouveau_pci_bar_releases; /* BAR regions released */
    uint64 nouveau_pci_resource_claims; /* PCI resource claims */
    uint64 nouveau_pci_resource_releases; /* PCI resource releases */
    uint64 nouveau_pci_resource_iomaps; /* claimed BAR iomaps */
    uint64 nouveau_pci_resource_owner_mismatches; /* busy owner mismatch */
    uint64 nouveau_pci_unclaimed_iomaps; /* rejected iomap-before-claim */
    uint64 nouveau_pci_unclaimed_releases; /* rejected release-before-claim */
    uint64 nouveau_pci_irq_request_failures; /* IRQ vector request failures */
    uint64 nouveau_pci_irq_mode; /* PCI_IRQ_* granted by PCI core */
    uint64 nouveau_pci_msi_requested; /* MSI/MSI-X requested when present */
    uint64 nouveau_pci_msi_fail_closed; /* MSI/MSI-X unsupported path hit */
    uint64 nouveau_pci_irq_alloc_requests; /* PCI IRQ allocation attempts */
    uint64 nouveau_pci_irq_alloc_failures; /* PCI IRQ allocation failures */
    uint64 nouveau_pci_msi_program_attempts; /* MSI programming attempts */
    uint64 nouveau_pci_msi_program_unsupported; /* MSI unsupported results */
    uint64 nouveau_pci_msix_program_attempts; /* MSI-X programming attempts */
    uint64 nouveau_pci_msix_program_unsupported; /* MSI-X unsupported results */
    uint64 nouveau_pci_legacy_irq_requests; /* legacy IRQ allocation attempts */
    uint64 nouveau_pci_legacy_irq_grants; /* legacy IRQ vectors granted */
    uint64 nouveau_pci_irq_vector_valid; /* selected vector is usable */
    uint64 nouveau_pci_irq_handler_registered; /* driver installed handler */
    uint64 nouveau_pci_irq_delivery_enabled; /* interrupts can be delivered */
    uint64 nouveau_pci_irq_delivery_claimed; /* driver claims live delivery */
    uint64 nouveau_pci_legacy_irq_fallback; /* legacy vector fallback used */
    uint64 nouveau_pci_irq_handler_invocations; /* handler entry count */
    uint64 nouveau_pci_irq_cause_reads; /* device cause reads */
    uint64 nouveau_pci_irq_cause_valid; /* nonzero device causes */
    uint64 nouveau_pci_irq_cause_acks; /* device cause acknowledgements */
    uint64 nouveau_pci_irq_spurious; /* handler entries without cause */
    uint64 nouveau_pci_dma_map_api_present; /* PCI DMA map API available */
    uint64 nouveau_pci_dma_map_attempts; /* Nouveau DMA map attempts */
    uint64 nouveau_pci_dma_map_successes; /* Nouveau DMA map successes */
    uint64 nouveau_pci_dma_map_failures; /* Nouveau DMA map failures */
    uint64 nouveau_pci_dma_unmaps; /* Nouveau DMA unmap calls */
    uint64 nouveau_pci_dma_map_last_size; /* last DMA mapping size */
    uint64 nouveau_pci_dma_map_last_addr; /* last DMA bus address */
    uint64 nouveau_pci_dma_map_last_ret; /* last DMA map errno */
    uint64 nouveau_pci_native_present_credit; /* native scanout credit */
    /*
     * DDA device-presence facts (GPU plan section 1.2).  These record the
     * REAL identity of the NVIDIA function that the Nouveau PCI probe accepted
     * over the Hyper-V vPCI bus.  They are zero when no physical 0x10DE BAR-
     * backed display function is assigned to the guest, in which case the
     * presence matrix reports fail-closed.  vendor/device/class/bar values are
     * copied verbatim from the probed pci_device_info, never synthesized.
     */
    uint64 nouveau_dda_present; /* 1 == real NVIDIA DDA function accepted */
    uint64 nouveau_dda_vendor_id; /* real PCI vendor id (expect 0x10DE) */
    uint64 nouveau_dda_device_id; /* real PCI device id of assigned GPU */
    uint64 nouveau_dda_class_code; /* real 24-bit PCI class code */
    uint64 nouveau_dda_bar_count; /* count of usable (nonzero-len) BARs */
    uint64 nouveau_native_display_ready; /* real DDA/Nouveau display ready */
    uint64 nouveau_dda_native_display_present; /* HW present path exists */
    uint64 nouveau_display_probe_attempts; /* DDA display readiness probes */
    uint64 nouveau_display_create_attempts; /* display object create attempts */
    uint64 nouveau_display_create_successes; /* real display create successes */
    uint64 nouveau_display_create_fail_closed; /* display create not wired */
    uint64 nouveau_display_create_fail_reason; /* FB_GPU_KMS_PRESENT_REJECT_* */
    uint64 nouveau_display_head_probe_attempts; /* head discovery probes */
    uint64 nouveau_display_heads; /* detected DDA/Nouveau display heads */
    uint64 nouveau_display_connector_probe_attempts; /* connector probes */
    uint64 nouveau_display_connectors; /* detected display connectors */
    uint64 nouveau_display_nonvirtual_connectors; /* non-virtual connectors */
    uint64 nouveau_display_vblank_supported; /* HW vblank source exists */
    uint64 nouveau_display_vblank_irq_supported; /* HW vblank IRQ capable */
    uint64 nouveau_display_vblank_source; /* FB_GPU_NOUVEAU_DISPLAY_VBLANK_* */
    uint64 nouveau_display_vblank_irqs; /* HW vblank IRQs observed */
    uint64 nouveau_display_page_flip_completion_ready; /* HW flip complete gate */
    uint64 nouveau_display_page_flip_completions; /* HW flip completions */
    uint64 nouveau_display_atomic_pageflip_backend_missing; /* no KMS HW backend */
    uint64 nouveau_display_engine_object_created; /* nvif/nvkm display object */
    uint64 nouveau_display_mode_config_ready; /* DRM mode_config initialized */
    uint64 nouveau_display_crtc_count; /* DRM CRTC/head objects registered */
    uint64 nouveau_display_encoder_count; /* DRM encoder/outp objects */
    uint64 nouveau_display_primary_plane_count; /* primary plane objects */
    uint64 nouveau_display_primary_plane_linear_required; /* LINEAR scanout gate */
    uint64 nouveau_display_primary_plane_nonlinear_modifiers; /* blocked mods */
    uint64 nouveau_display_outp_mask_seen; /* NVIF/NVKM outp mask observed */
    uint64 nouveau_display_conn_mask_seen; /* connector mask observed */
    uint64 nouveau_display_head_mask_seen; /* head mask observed */
    uint64 nouveau_display_nvif_head_ctor_successes; /* nvif_head objects */
    uint64 nouveau_display_vblank_event_registered; /* nvif vblank event */
    uint64 nouveau_display_hpd_event_registered; /* connector HPD event */
    uint64 nouveau_display_dp_irq_event_registered; /* DP IRQ event */
    uint64 nouveau_display_atomic_commit_tail_ready; /* HW commit tail exists */
    uint64 nouveau_display_page_flip_event_source; /* FB_GPU_NOUVEAU_DISPLAY_VBLANK_* */
    uint64 nouveau_native_display_reject_reasons; /* KMS present reject bits */
    uint64 kms_present_last_lane; /* FB_GPU_KMS_PRESENT_LANE_* */
    uint64 kms_present_dumb; /* KMS presents through dumb/software lane */
    uint64 kms_present_synthvid; /* KMS presents through Hyper-V synthvid */
    uint64 kms_present_nouveau_hw; /* KMS presents through Nouveau HW */
    uint64 kms_present_rejects; /* native-present lane rejects */
    uint64 kms_present_reject_reasons; /* FB_GPU_KMS_PRESENT_REJECT_* */
    uint64 kms_present_reject_no_native_display; /* no native display ready */
    uint64 kms_present_reject_no_nouveau_display; /* no Nouveau display */
    uint64 kms_present_reject_no_display_create; /* no display object */
    uint64 kms_present_reject_no_heads; /* no HW heads */
    uint64 kms_present_reject_no_connectors; /* no HW connectors */
    uint64 kms_present_reject_no_vblank; /* no HW vblank */
    uint64 kms_present_reject_no_hw_completion; /* no flip completion */
    uint64 kms_present_reject_no_atomic_pageflip_backend; /* no HW backend */
    uint64 kms_framebuffers;   /* currently registered KMS framebuffer IDs */
    uint64 kms_page_flips;     /* accepted KMS page flips */
    uint64 kms_atomic_commits; /* accepted/tested atomic commits */
    uint64 kms_atomic_in_fence_accepted; /* accepted atomic IN_FENCE_FD fds */
    uint64 kms_atomic_in_fence_rejected; /* rejected atomic IN_FENCE_FD fds */
    uint64 kms_atomic_in_fence_fd_refs; /* prepared stable fd references */
    uint64 kms_atomic_in_fence_fd_ref_puts; /* released prepared references */
    uint64 kms_atomic_in_fence_duplicate_rejects; /* duplicate prop rejects */
    uint64 kms_atomic_in_fence_test_only_validated; /* TEST_ONLY validated */
    uint64 kms_atomic_in_fence_test_only_waits; /* TEST_ONLY wait regressions */
    uint64 kms_atomic_in_fence_sync_file_pending_waits; /* live sync_file */
    uint64 kms_atomic_in_fence_sync_file_pending_wakeups; /* pending became ready */
    uint64 kms_atomic_out_fence_prepared; /* real commits prepared fd */
    uint64 kms_atomic_out_fence_cleanup_closes; /* prepared fd cleanup closes */
    uint64 kms_atomic_out_fence_test_only_placeholders; /* TEST_ONLY wrote -1 */
    uint64 kms_atomic_out_fence_fd_exports; /* atomic OUT_FENCE_PTR fd exports */
    uint64 kms_atomic_out_fence_display_correlated; /* native/display fences */
    uint64 kms_atomic_out_fence_software_scanout_correlated; /* sw scanout */
    uint64 kms_atomic_nonblock_rejects; /* nonblocking atomic commits rejected */
    uint64 ttm_system_bytes;   /* BO bytes in system placement */
    uint64 ttm_tt_bytes;       /* BO bytes in GART/TT placement */
    uint64 ttm_vram_bytes;     /* BO bytes in VRAM placement */
    uint64 ttm_stolen_bytes;   /* BO bytes in stolen/scanout placement */
    uint64 ttm_pinned_bytes;   /* BO bytes with nonzero pin count */
    uint64 ttm_validate_failures; /* rejected placement/validation requests */
    uint64 ttm_metadata_only_moves; /* placement moves without backing copy */
    uint64 ttm_real_copy_moves; /* placement moves with copied backing pages */
    uint64 ttm_move_bytes;     /* bytes covered by placement moves */
    uint64 ttm_native_accel_credit; /* native accel credit granted by TTM */
    uint64 ttm_cpu_copy_fallback_moves[4]; /* dst-domain CPU-copy fallback */
    uint64 ttm_metadata_noop_moves[4]; /* dst-domain same-placement no-ops */
    uint64 ttm_unsupported_hw_copy_moves[4]; /* dst-domain HW-copy gaps */
    uint64 ttm_real_copy_moves_by_domain[4]; /* dst-domain copied moves */
    uint64 ttm_resv_acquires;  /* dma_resv-like exclusive reservations */
    uint64 ttm_resv_releases;  /* reservation drops */
    uint64 ttm_resv_waits;     /* waits/conflict probes observed */
    uint64 ttm_resv_conflicts; /* conflicting reservation attempts */
    uint64 ttm_resv_exclusive_fences; /* reservation fence generations */
    uint64 ttm_resv_shared_slots; /* fixed dma_resv-like shared slots per BO */
    uint64 ttm_resv_shared_used; /* last observed shared slot count */
    uint64 ttm_resv_shared_fences; /* shared reservation fences attached */
    uint64 ttm_resv_shared_replaced; /* ring slots overwritten */
    uint64 ttm_resv_wait_queued; /* reservation waits that slept on a channel */
    uint64 ttm_resv_wait_wakeups; /* waiters woken by reservation/fence change */
    uint64 ttm_resv_stale_fence_rejects; /* stale snapshots not trusted */
    uint64 ttm_resv_attach_prime_export; /* PRIME export fence attachments */
    uint64 ttm_resv_attach_prime_import; /* PRIME import fence attachments */
    uint64 ttm_resv_attach_dmabuf_export; /* fb BO fd export attachments */
    uint64 ttm_resv_attach_dmabuf_import; /* fb BO fd import attachments */
    uint64 ttm_resv_attach_kms_pin; /* KMS framebuffer pin attachments */
    uint64 ttm_resv_attach_kms_unpin; /* KMS framebuffer unpin attachments */
    uint64 ttm_resv_attach_syncobj_signal; /* syncobj signal attachments */
    uint64 ttm_resv_attach_syncobj_wait; /* syncobj wait reservation probes */
    uint64 ttm_resv_attach_sync_file_export; /* sync-file export attachments */
    uint64 ttm_resv_attach_sync_file_import; /* sync-file import attachments */
    uint64 ttm_resv_last_attach_point; /* FB_GPU_RESV_ATTACH_* */
    uint64 ttm_resv_last_shared_fence; /* last shared reservation fence */
    uint64 ttm_resv_evict_pinned_rejects; /* pinned BO eviction rejects */
    uint64 ttm_resv_evict_busy_rejects; /* busy reservation eviction rejects */
    uint64 ttm_resv_ww_contexts; /* ww_acquire_ctx-shaped reservations */
    uint64 ttm_resv_ww_ordered_acquires; /* ordered object acquisitions */
    uint64 ttm_resv_ww_deadlock_retries; /* reversed order backoffs */
    uint64 ttm_resv_ww_wound_backoffs; /* wound/wait retry decisions */
    uint64 ttm_resv_ww_multi_object; /* validated multi-object sequences */
    uint64 ttm_resv_ww_release_balance; /* paired release sequences */
    uint64 ttm_resv_ww_max_acquired; /* max objects held by one ctx */
    uint64 ttm_resv_ww_validate_failures; /* malformed ww validation */
    uint64 syncobj_created;    /* DRM syncobjs created */
    uint64 syncobj_live;       /* currently live DRM syncobjs */
    uint64 syncobj_signals;    /* binary/timeline signal operations */
    uint64 syncobj_waits;      /* binary/timeline wait operations */
    uint64 syncobj_resv_attach; /* syncobj/sync-file reservation attaches */
    uint64 syncobj_sync_file_exports; /* syncobj handle-to-fd exports */
    uint64 syncobj_sync_file_imports; /* syncobj fd-to-handle imports */
    uint64 syncobj_wait_queued; /* syncobj waits that slept on a channel */
    uint64 syncobj_wait_wakeups; /* waiters woken by syncobj signal/reset */
    uint64 syncobj_wait_callbacks_armed; /* wait callbacks armed */
    uint64 syncobj_wait_callbacks_fired; /* wait callbacks fired by signal */
    uint64 syncobj_wait_callbacks_cancelled; /* timed/interrupted wait cancels */
    uint64 syncobj_wait_callback_late_fires; /* inconsistent wait callback */
    uint64 syncobj_pending_transfers; /* transfer copied an unsignaled point */
    uint64 syncobj_pending_transfer_wakeups; /* pending transfer became ready */
    uint64 syncobj_timeout_waits; /* waits using finite timeout wakeups */
    uint64 syncobj_stale_wait_rejects; /* waits rejected as stale/unsubmitted */
    uint64 sync_file_pending_exports; /* pending sync_file fd exports */
    uint64 sync_file_pending_imports; /* pending sync_file imports */
    uint64 sync_file_pending_poll_not_ready; /* pending poll miss */
    uint64 sync_file_pending_poll_ready; /* pending poll became ready */
    uint64 sync_file_pending_wakeups; /* proxy fence completions observed */
    uint64 sync_file_pending_import_rejects; /* invalid pending imports */
    uint64 sync_file_pending_callbacks_armed; /* pending poll callback armed */
    uint64 sync_file_pending_callbacks_fired; /* source signal fired callback */
    uint64 sync_file_pending_callbacks_cancelled; /* close cancelled callback */
    uint64 sync_file_pending_callback_late_fires; /* fired after cancellation */
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
    uint64 virtio_async_posted; /* async virtio-gpu commands posted */
    uint64 virtio_async_posted_submit_3d; /* async 3D submits posted */
    uint64 virtio_async_posted_flush; /* async resource flushes posted */
    uint64 virtio_async_posted_transfer; /* async transfer-to-host posted */
    uint64 virtio_async_retired; /* async commands retired from used ring */
    uint64 virtio_async_pending; /* async commands still in flight */
    uint64 virtio_async_depth; /* configured async ring depth */
    uint64 virtio_async_make_room_calls; /* async slot admission attempts */
    uint64 virtio_async_make_room_submit_3d_calls; /* 3D admission attempts */
    uint64 virtio_async_make_room_flush_calls; /* flush admission attempts */
    uint64 virtio_async_make_room_transfer_calls; /* transfer admission attempts */
    uint64 virtio_async_make_room_stalls; /* admissions that waited for room */
    uint64 virtio_async_make_room_submit_3d_stalls; /* 3D admissions stalled */
    uint64 virtio_async_make_room_flush_stalls; /* flush admissions stalled */
    uint64 virtio_async_make_room_transfer_stalls; /* transfer admissions stalled */
    uint64 virtio_async_wait_progress_calls; /* blocking async wait calls */
    uint64 virtio_async_make_room_wait_ticks; /* total room-wait time */
    uint64 virtio_async_make_room_last_wait_us; /* last room-wait duration */
    uint64 virtio_async_make_room_max_wait_us; /* max room-wait duration */
    uint64 virgl_bo_presents;  /* BO presents whose dmabuf came from virgl */
    uint64 virgl_bo_present_pixels; /* virgl-backed BO present pixels */
    uint64 virgl_bo_present_last_resource; /* last presented virgl resource */
    uint64 bo_present_copy_ticks; /* CPU BO-to-scanout copy time */
    uint64 bo_present_virtio_ticks; /* virtio transfer/flush wait time */
    uint64 bo_present_total_ticks; /* total BO present hot-path time */
    uint64 bo_present_last_copy_us; /* last BO copy duration */
    uint64 bo_present_last_virtio_us; /* last virtio scanout duration */
    uint64 bo_present_last_total_us; /* last BO present duration */
    uint64 gpu_backend;       /* FB_GPU_BACKEND_* currently selected */
    uint64 gpu_backend_flags; /* FB_GPU_BACKEND_F_* currently selected */
    uint64 dxg_global_open;   /* Hyper-V DXG global transport opened */
    uint64 dxg_vgpu_open;     /* Hyper-V DXG vGPU transport opened */
    uint64 dxg_d3dkmt;        /* nonzero when D3DKMT ioctls are implemented */
    uint64 dxg_global_rx;     /* packets drained from global DXG channel */
    uint64 dxg_vgpu_rx;       /* packets drained from vGPU DXG channel */
    uint64 dxg_present_register_ioctl_entries; /* register ioctl entered */
    uint64 dxg_present_commit_ioctl_entries; /* commit ioctl entered */
    uint64 dxg_present_query_ioctl_entries; /* query ioctl entered */
    uint64 dxg_present_register_copyin_failures; /* register copyin faults */
    uint64 dxg_present_commit_copyin_failures; /* commit copyin faults */
    uint64 dxg_present_commit_copyout_failures; /* commit copyout faults */
    uint64 dxg_present_query_copyin_failures; /* query copyin faults */
    uint64 dxg_present_query_copyout_failures; /* query copyout faults */
    uint64 dxg_present_register_attempts; /* DXG source register attempts */
    uint64 dxg_present_register_successes; /* accepted inert DXG sources */
    uint64 dxg_present_register_rejects; /* rejected DXG source registers */
    uint64 dxg_present_commit_attempts; /* DXG source commit attempts */
    uint64 dxg_present_commit_rejects; /* rejected DXG source commits */
    uint64 dxg_present_query_attempts; /* DXG source query attempts */
    uint64 dxg_present_query_rejects; /* fail-closed DXG source queries */
    uint64 dxg_present_host_handoff_missing; /* commits blocked before host */
    uint64 dxg_present_last_source; /* last registered/committed source */
    uint64 dxg_present_last_ret;    /* last DXG present-source errno */
    uint64 dxg_present_last_device; /* last D3DKMT device handle */
    uint64 dxg_present_last_resource; /* last D3DKMT resource handle */
    uint64 dxg_present_last_allocation; /* last D3DKMT allocation handle */
    uint64 dxg_present_last_sync;   /* last D3DKMT sync object handle */
    uint64 dxg_present_last_flags;  /* last register/commit flags */
    uint64 dxg_present_last_fence_value; /* last requested wait fence */
    uint64 dxg_present_last_width;  /* last source width */
    uint64 dxg_present_last_height; /* last source height */
    uint64 dxg_present_last_pitch;  /* last source pitch */
    uint64 dxg_present_last_format; /* last source format */
    uint64 dxg_present_last_allocation_count; /* source allocation count */
    uint64 dxg_present_last_dxg_fd; /* registered /dev/dxg fd, or 0xffffffff */
    uint64 dxg_present_last_resource_fd; /* dxgresource fd, or 0xffffffff */
    uint64 dxg_present_last_provenance; /* FB_GPU_DXG_PRESENT_PROV_* */
    uint64 dxg_present_last_adapter_luid_low; /* optional source LUID low */
    uint64 dxg_present_last_adapter_luid_high; /* optional source LUID high */
    uint64 dxg_present_last_adapter_identity; /* FB_GPU_DXG_PRESENT_ADAPTER_* */
    uint64 dxg_present_selected_lane; /* FB_GPU_DXG_PRESENT_LANE_* */
    uint64 dxg_present_helper_block_reason; /* legacy name: GPU-P/DDA block reason */
    uint64 dxg_present_display_target_kind; /* FB_GPU_DXG_DISPLAY_TARGET_* */
    uint64 dxg_present_requires_host_protocol; /* no kernel/host bind ABI */
    uint64 dxg_present_missing_host_abi; /* FB_GPU_DXG_PRESENT_MISSING_* */
    uint64 dxg_present_host_candidates; /* FB_GPU_DXG_PRESENT_HOST_* */
    uint64 dxg_present_host_rejects; /* FB_GPU_DXG_PRESENT_REJECT_* */
    uint64 dxg_present_synthvid_state; /* present/open/init/dirt bitfield */
    uint64 dxg_present_synthvid_vram_gpa; /* current synthvid VRAM GPA */
    uint64 dxg_present_dxg_state; /* global/vgpu/d3dkmt readiness bitfield */
    uint64 dxg_present_dxg_adapter_type_raw; /* host QAI adapter type */
    uint64 dxg_present_dxg_adapter_type_wsl; /* WSL-shaped adapter type */
    uint64 dxg_present_dxg_adapter_type_rewrites; /* QAI rewrites observed */
    uint64 dxg_present_dxg_adapter_sources; /* adapter source count */
    uint64 dxg_present_dxg_adapter_render_supported; /* WSL adapter render bit */
    uint64 dxg_present_dxg_adapter_display_supported; /* WSL adapter display bit */
    uint64 dxg_present_dxg_adapter_paravirtualized; /* WSL adapter GPU-PV bit */
    uint64 dxg_present_dxg_adapter_compute_only; /* WSL adapter compute-only bit */
    uint64 dxg_present_dxg_adapter_sources_known; /* enum adapter source count valid */
    uint64 dxg_present_dxg_enum_adapter_count; /* enum adapters returned */
    uint64 dxg_present_dxg_enum_adapter_handle; /* last local enum adapter */
    uint64 dxg_present_dxg_enum_adapter_luid_low; /* enum adapter LUID low */
    uint64 dxg_present_dxg_enum_adapter_luid_high; /* enum adapter LUID high */
    uint64 dxg_present_dxg_user_luid_low; /* current DXG user LUID low */
    uint64 dxg_present_dxg_user_luid_high; /* current DXG user LUID high */
    uint64 dxg_present_dda_nouveau_present; /* DDA/Nouveau PCI candidate seen */
    uint64 dxg_present_dda_nouveau_import_path_present; /* D3D12 import lane exists */
    uint64 dxg_present_dda_nouveau_scanout_bind_present; /* scanout bind lane exists */
    uint64 dxg_present_helper_contract_version; /* legacy name: GPU-P/DDA contract */
    uint64 dxg_present_helper_required_metadata; /* FB_GPU_DXG_PRESENT_META_* */
    uint64 dxg_present_helper_transport; /* legacy name: GPU-P/DDA transport */
    uint64 dxg_present_helper_transport_present; /* nonzero once GPU-P/DDA lane exists */
    uint64 dxg_present_helper_operation; /* FB_GPU_DXG_PRESENT_HELPER_OP_* */
    uint64 dxg_present_helper_lifetime; /* FB_GPU_DXG_PRESENT_LIFE_* */
    uint64 dxg_present_helper_source_live; /* registered source still tracked */
    uint64 dxg_present_helper_requires_completion; /* present_id/completed required */
    uint64 dxg_present_commit_no_source; /* commits without live registered source */
    uint64 dxg_present_commit_bad_flags; /* commits rejected by flags/sync contract */
    uint64 dxg_present_commit_adapter_mismatch; /* source/target adapter mismatch */
    uint64 dxg_present_commit_resource_fd_unverified; /* no verifiable dxgresource fd */
    uint64 dxg_present_commit_no_transport; /* no GPU-P/DDA display lane */
    uint64 dxg_present_commit_no_completion; /* no present completion source */
    uint64 dxg_present_release_sources; /* sources cleaned on render fd close */
    uint64 dxg_present_bind_contract_queries; /* future bind contract queries */
    uint64 dxg_present_bind_contract_rejects; /* bind queries fail-closed */
    uint64 dxg_present_bind_contract_successes; /* native bind query accepts */
    uint64 dxg_scanout_bind_attempts; /* selected scanout-bind attempts */
    uint64 dxg_scanout_bind_rejects; /* scanout-bind fail-closed rejects */
    uint64 dxg_scanout_bind_successes; /* real scanout-bind accepts */
    uint64 dxg_scanout_bind_completion_queries; /* completion polls */
    uint64 dxg_scanout_bind_completion_successes; /* completed native polls */
    uint64 dxg_scanout_bind_completion_pending; /* no native completion yet */
    uint64 dxg_scanout_bind_weak_evidence_rejects; /* no loose credit */
    uint64 dxg_scanout_bind_candidate_cmds_known; /* WSL enum candidates seen */
    uint64 dxg_scanout_bind_candidate_sender_contracts; /* usable senders */
    uint64 dxg_scanout_bind_candidate_completion_contracts; /* usable completions */
    uint64 dxg_scanout_bind_candidate_rejects; /* known cmds but no ABI */
    uint64 dxg_scanout_bind_candidate_presenthistory_cmd; /* VMBus enum ID */
    uint64 dxg_scanout_bind_candidate_redirected_flip_fence_cmd; /* VMBus enum ID */
    uint64 dxg_scanout_bind_candidate_blt_cmd; /* VMBus enum ID */
    uint64 dxg_scanout_bind_candidate_propagate_presenthistory_cmd; /* host-to-VM enum ID */
    uint64 dxg_scanout_bind_candidate_vmbus_enum_known; /* enum namespace known */
    uint64 dxg_scanout_bind_wsl_ioctl_namespace_checked; /* WSL ioctl audit */
    uint64 dxg_scanout_bind_wsl_display_bind_ioctl_absent; /* no ioctl ABI */
    uint64 dxg_scanout_bind_candidate_linux_ioctl_contracts; /* ioctl send ABI */
    uint64 dxg_scanout_bind_candidate_resource_bind_contracts; /* resource bind ABI */
    uint64 dxg_scanout_bind_candidate_display_completion_contracts; /* display done ABI */
    uint64 dxg_scanout_bind_standard_alloc_private_data; /* WSL stdalloc role */
    uint64 dxg_scanout_bind_standard_alloc_display_bind_absent; /* no bind ABI */
    uint64 dxg_scanout_bind_synthvid_gpa_dirty_present; /* synthvid dirty path */
    uint64 dxg_scanout_bind_synthvid_resource_bind_absent; /* no D3D12 bind */
    uint64 dxg_scanout_bind_dda_pci_display_present; /* DDA display split */
    uint64 dxg_scanout_bind_dda_resource_import_absent; /* no DXG import */
    uint64 dxg_scanout_bind_dda_scanout_bind_absent; /* no D3D12 scanout bind */
    uint64 dxg_scanout_bind_dda_hw_flip_completion_absent; /* no D3D12 complete */
    uint64 dxg_scanout_bind_candidate_reject_reasons; /* FB_GPU_DXG_SCANOUT_* */
    uint64 dxg_scanout_bind_weak_dxg_ready_only; /* DXG ready only */
    uint64 dxg_scanout_bind_weak_d3dkmt_handles_only; /* D3DKMT handles only */
    uint64 dxg_scanout_bind_weak_same_adapter_resource_only; /* same LUID only */
    uint64 dxg_scanout_bind_weak_syncfile_only; /* sync-file acquire only */
    uint64 dxg_scanout_bind_weak_synthvid_gpa_dirty_only; /* GPA dirty only */
    uint64 dxg_scanout_bind_weak_software_or_readback_path; /* software path */
    uint64 dxg_scanout_bind_last_transport; /* helper transport snapshot */
    uint64 dxg_scanout_bind_last_status; /* last errno/status */
    uint64 dxg_scanout_bind_last_present_id; /* last native present id */
    uint64 dxg_scanout_bind_last_completed; /* last native completion id */
    uint64 dxg_scanout_bind_last_source_generation; /* source generation */
    uint64 dxg_scanout_bind_last_resource_generation; /* resource generation */
    uint64 dxg_scanout_bind_last_dirty_sequence; /* display dirty seq */
    uint64 dxg_scanout_bind_last_dirty_rects; /* display dirty rect count */
    uint64 dxg_display_bind_contract_version; /* canonical bind ABI version */
    uint64 dxg_display_bind_backend; /* FB_GPU_DXG_PRESENT_LANE_* */
    uint64 dxg_display_bind_transport; /* FB_GPU_DXG_PRESENT_*_TRANSPORT_* */
    uint64 dxg_display_bind_transport_present; /* real transport exists */
    uint64 dxg_display_bind_operation; /* FB_GPU_DXG_PRESENT_*_OP_* */
    uint64 dxg_display_bind_required_metadata; /* FB_GPU_DXG_PRESENT_META_* */
    uint64 dxg_display_bind_lifetime; /* FB_GPU_DXG_PRESENT_LIFE_* */
    uint64 dxg_display_bind_block_reason; /* FB_GPU_DXG_PRESENT_BLOCK_* */
    uint64 dxg_display_bind_completion_source; /* FB_GPU_DXG_PRESENT_COMPLETION_* */
    uint64 dxg_display_bind_present_id; /* nonzero after native bind */
    uint64 dxg_display_bind_completed_id; /* display-correlated complete id */
    uint64 dxg_display_bind_source_generation; /* source generation */
    uint64 dxg_display_bind_resource_generation; /* resource generation */
    uint64 dxg_display_bind_status; /* last errno/status */
    uint64 dxg_display_bind_provider_submits; /* provider submit attempts */
    uint64 dxg_display_bind_provider_pin_revalidated; /* provider checked pin */
    uint64 dxg_display_bind_provider_no_host_abi; /* provider lacks ABI */
    uint64 dxg_display_bind_provider_no_sender; /* provider lacks sender */
    uint64 dxg_display_bind_provider_no_completion; /* provider lacks done */
    uint64 dxg_display_bind_provider_preflight_ready; /* ready before send */
    uint64 dxg_display_bind_provider_send_attempts; /* real sends attempted */
    uint64 dxg_display_bind_provider_send_blocked_no_host_abi; /* ABI block */
    uint64 dxg_display_bind_provider_completion_demux_attempts; /* done demux */
    uint64 dxg_display_bind_provider_completion_demux_blocked_no_contract; /* no done ABI */
    uint64 dxg_display_bind_provider_publication_attempts; /* publish decisions */
    uint64 dxg_display_bind_provider_publish_before_send; /* published before send */
    uint64 dxg_display_bind_provider_transport_pending_id; /* host pending id */
    uint64 dxg_display_bind_provider_command_id; /* host command id */
    uint64 dxg_display_bind_provider_transaction_id; /* host transaction id */
    uint64 dxg_display_bind_provider_channel; /* host channel enum */
    uint64 dxg_display_bind_provider_completion_demux_registered; /* demux armed */
    uint64 dxg_display_bind_transport_source; /* FB_GPU_DXG_DISPLAY_BIND_SOURCE_* */
    uint64 dxg_display_bind_host_saw_packet; /* host saw bind packet */
    uint64 dxg_display_bind_wsl_presenthistory_completion_credit; /* WSL telemetry credit */
    uint64 dxg_display_bind_provider_resolved_or_cancelled; /* pending done */
    uint64 dxg_display_bind_provider_refs_released; /* pending refs released */
    uint64 dxg_display_bind_request_metadata_complete; /* provider saw shape */
    uint64 dxg_display_bind_request_sync_metadata_complete; /* sync/fence seen */
    uint64 dxg_display_bind_request_missing_metadata; /* FB_GPU_DXG_PRESENT_META_* */
    uint64 dxg_display_bind_lock_dropped_submits; /* submits outside fb lock */
    uint64 dxg_display_bind_revalidate_attempts; /* post-submit rechecks */
    uint64 dxg_display_bind_revalidate_successes; /* source still current */
    uint64 dxg_display_bind_revalidate_failures; /* stale source/resource */
    uint64 dxg_display_bind_pin_attempts; /* WSL-style fd pin attempts */
    uint64 dxg_display_bind_pin_successes; /* dxg/resource fds retained */
    uint64 dxg_display_bind_pin_failures; /* pin/admission failed */
    uint64 dxg_display_bind_unpins; /* retained fds released after submit */
    uint64 dxg_display_bind_pinned_dxg_file; /* last dxg file retained */
    uint64 dxg_display_bind_pinned_resource_file; /* last resource fd retained */
    uint64 dxg_display_bind_pinned_resource_generation; /* sealed generation */
    uint64 dxg_display_bind_pinned_process_generation; /* dxgprocess generation */
    uint64 dxg_display_bind_pinned_process_refs; /* dxgprocess refs while pinned */
    uint64 dxg_display_bind_pinned_shared_parent; /* WSL-style parent id */
    uint64 dxg_display_bind_pinned_parent_refs; /* parent refs while pinned */
    uint64 dxg_display_bind_pinned_parent_children; /* opened children */
    uint64 dxg_display_bind_pending_sequence; /* last source-owned bind request */
    uint64 dxg_display_bind_pending_created; /* pending bind records created */
    uint64 dxg_display_bind_pending_active; /* currently unresolved binds */
    uint64 dxg_display_bind_pending_peak; /* max unresolved bind records */
    uint64 dxg_display_bind_pending_completed; /* display-bind completions */
    uint64 dxg_display_bind_pending_failclosed; /* resolved by fail-closed provider */
    uint64 dxg_display_bind_pending_cancelled; /* stale/release cancellations */
    uint64 dxg_display_bind_pending_last_status; /* last pending resolution errno */
    uint64 dxg_display_bind_pending_last_block_reason; /* last pending block reason */
    uint64 dxg_display_bind_pending_last_source_generation; /* source gen snapshot */
    uint64 dxg_display_bind_pending_last_resource_generation; /* resource gen snapshot */
    uint64 dxg_display_bind_release_clears; /* release cleared bind ids */
    uint64 dxg_display_bind_stale_source_rejects; /* stale source rejected */
    uint64 dxg_display_bind_stale_generation_rejects; /* stale generation */
    uint64 dxg_display_bind_stale_completion_rejects; /* stale completion */
    uint64 dxg_display_bind_late_completion_after_release; /* late done */
    uint64 dxg_display_bind_after_close_queries; /* stale queries after close */
    uint64 dxg_display_bind_after_close_nonzero_id_rejects; /* stale ids */
    uint64 display_presents;  /* display present/flush operations issued */
    uint64 display_completions; /* display present completions observed */
    uint64 display_last_present; /* latest issued display-present sequence */
    uint64 display_last_complete; /* latest completed display-present sequence */
    uint64 fence_objects_created; /* internal fb_gpu_fence objects created */
    uint64 fence_objects_live; /* currently live fb_gpu_fence objects */
    uint64 fence_objects_peak; /* high-water mark of live fence objects */
    uint64 fence_objects_signaled; /* fence objects signaled */
    uint64 fence_objects_errors; /* fence objects completed with an error */
    uint64 fence_objects_waits; /* waits issued against fence objects */
    uint64 fence_objects_wait_queued; /* fence object waits that slept */
    uint64 fence_objects_wakeups; /* waiters woken by fence signal */
    uint64 fence_objects_ref_puts; /* fence object reference drops */
    uint64 fence_objects_callbacks_added; /* callback records armed */
    uint64 fence_objects_callbacks_removed; /* armed callbacks cancelled */
    uint64 fence_objects_callbacks_fired; /* callbacks fired by signal */
    uint64 fence_objects_callbacks_late; /* add after signaled */
    uint64 fence_objects_callback_errors; /* inconsistent callback state */
    uint64 dxg_display_bind_provider_no_host_abi_cancelled; /* no-ABI cancels */
    uint64 dxg_display_bind_provider_no_host_abi_refs_released; /* no-ABI refs */
    uint64 dxg_display_bind_provider_pending_owner_generation; /* dxgprocess gen */
    uint64 dxg_display_bind_provider_pending_source_generation; /* source gen */
    uint64 dxg_display_bind_provider_pending_resource_generation; /* resource gen */
    uint64 dxg_display_bind_pending_last_owner_generation; /* dxgprocess gen */
    uint64 dxg_display_bind_stale_after_release_rejects; /* stale after release */
    uint64 dxg_display_bind_provider_pending_dxgprocess_generation; /* WSL proc */
    uint64 dxg_display_bind_provider_pending_process_adapter_generation; /* WSL adapter */
    uint64 dxg_display_bind_provider_pending_hmgr_index_unique_valid; /* hmgr index/unique valid */
    uint64 dxg_display_bind_provider_pending_process_namespace_valid; /* TGID namespace */
    uint64 dxg_display_bind_provider_pending_device_hmgr_index_unique_valid; /* device hmgr */
    uint64 dxg_display_bind_provider_pending_resource_hmgr_index_unique_valid; /* resource hmgr */
    uint64 dxg_display_bind_provider_pending_allocation_hmgr_index_unique_valid; /* allocation hmgr */
    uint64 dxg_display_bind_provider_pending_device_object_ref_active; /* device entry ref */
    uint64 dxg_display_bind_provider_pending_resource_object_ref_active; /* resource entry ref */
    uint64 dxg_display_bind_provider_pending_allocation_object_ref_active; /* allocation entry ref */
    uint64 dxg_display_bind_provider_pending_shared_parent_id; /* parent id */
    uint64 dxg_display_bind_provider_pending_shared_parent_refs; /* parent refs */
    uint64 dxg_display_bind_provider_pending_shared_parent_children; /* children */
    uint64 dxg_display_bind_provider_pending_shared_parent_fd_refs; /* fd refs */
    uint64 dxg_display_bind_provider_pending_shared_parent_host_nt_refs; /* NT refs */
    uint64 dxg_display_bind_provider_pending_shared_parent_child_refs; /* child refs */
    uint64 dxg_display_bind_provider_pending_shared_parent_global_share; /* host share */
    uint64 dxg_display_bind_provider_pending_shared_parent_host_nt_handle; /* NT handle */
    uint64 dxg_display_bind_provider_pending_opened_child_parent_id_match; /* parent id match */
    uint64 dxg_display_bind_provider_pending_opened_child_global_share_match; /* share match */
    uint64 dxg_display_bind_provider_pending_opened_child_sealed_generation_match; /* gen match */
    uint64 dxg_display_bind_provider_pending_shared_parent_snapshot_valid; /* parent snapshot */
    uint64 dxg_display_bind_provider_pending_opened_child_snapshot_valid; /* child snapshot */
    uint64 dxg_display_bind_provider_pending_shared_parent_global_share_match; /* child share */
    uint64 dxg_display_bind_provider_pending_syncobject_object_ref_active; /* sync entry ref */
    uint64 dxg_display_bind_provider_pending_syncobject_shared_owner_present; /* sync owner */
    uint64 dxg_display_bind_provider_pending_syncobject_monitored_fence; /* monitored sync */
    uint64 dxg_display_bind_provider_pending_syncobject_fence_value; /* wait fence */
    uint64 dxg_display_bind_provider_pending_syncobject_fence_cpu_va_present; /* user VA */
    uint64 dxg_display_bind_provider_pending_syncobject_fence_gpu_va_present; /* kernel/GPU VA */
    uint64 dxg_display_bind_provider_pending_syncobject_fence_kva_present; /* kernel VA */
    uint64 dxg_display_bind_provider_pending_syncobject_fence_gpu_va_alias_gap; /* GPU VA alias */
    uint64 dxg_display_bind_provider_pending_syncobject_real_fence_gpu_va_present; /* real GPU VA */
    uint64 dxg_display_bind_provider_pending_syncobject_fence_gpu_va_source; /* source */
    uint64 dxg_display_bind_provider_pending_syncobject_fence_map_size; /* map bytes */
    uint64 dxg_display_bind_provider_pending_owner_close_cancelled; /* owner close */
    uint64 kms_cursor_uploads; /* KMS cursor images handed to virtio */
    uint64 kms_cursor_upload_failures; /* KMS cursor image upload failures */
    uint64 kms_cursor_last_width; /* last KMS cursor image width */
    uint64 kms_cursor_last_height; /* last KMS cursor image height */
    uint64 kms_cursor_last_hot_x; /* last KMS cursor hotspot X */
    uint64 kms_cursor_last_hot_y; /* last KMS cursor hotspot Y */
    uint64 kms_cursor_last_checksum; /* rolling checksum of last cursor */
    uint64 kms_cursor_last_alpha_nonzero; /* pixels with alpha != 0 */
    uint64 kms_cursor_last_alpha_zero; /* pixels with alpha == 0 */
    uint64 kms_cursor_last_alpha_opaque; /* pixels with alpha == 255 */
    uint64 kms_cursor_last_rgb_nonzero; /* visible-color candidate pixels */
    uint64 kms_cursor_last_first_pixel; /* first ARGB8888/BGRA-memory pixel */
    uint64 kms_cursor_last_center_pixel; /* center ARGB8888/BGRA-memory pixel */
    uint64 virtio_hot_shape_owners; /* owner slots with hot-shape samples */
    uint64 virtio_hot_shape_owner_drops; /* hot-shape owner table drops */
    uint64 virtio_hot_shape_submit_calls; /* hot execbuffer submissions */
    uint64 virtio_hot_shape_make_room_calls; /* hot-shape admissions */
    uint64 virtio_hot_shape_make_room_stalls; /* hot admissions stalled */
    uint64 virtio_hot_shape_make_room_wait_us; /* total hot wait time */
    uint64 virtio_hot_shape_make_room_max_wait_us; /* max hot wait */
    uint64 virtio_hot_shape_make_room_depth_max; /* max pending depth */
    uint64 virtio_hot_shape_make_room_count_max; /* max pending count */
    uint64 virtio_hot_shape_make_room_wait_count_max; /* max wait count */
    uint64 virtio_hot_shape_posted; /* hot async commands posted */
    uint64 virtio_hot_shape_post_count_max; /* max hot posted count */
    uint64 virtio_hot_shape_retired; /* hot async commands retired */
    uint64 virtio_hot_shape_retire_us; /* total hot retire age */
    uint64 virtio_hot_shape_retire_max_us; /* max hot retire age */
    uint64 virtio_hot_shape_failures; /* hot-shape failures */
    uint64 virtio_hot_shape_mixed; /* hot-shape identity changes */
    uint64 virtio_present_copy_calls; /* present-copy drain decisions */
    uint64 virtio_present_copy_drain_calls; /* actual present drains */
    uint64 virtio_present_copy_src_fence_drains; /* source fence drains */
    uint64 virtio_present_copy_blanket_drains; /* legacy whole-ring drains */
    uint64 virtio_present_copy_src_fence_only_drains; /* only source fence */
    uint64 virtio_present_copy_blanket_only_drains; /* only legacy blanket */
    uint64 virtio_present_copy_src_fence_blanket_drains; /* both reasons */
    uint64 virtio_present_copy_no_drain_skips; /* no-drain fast-path skips */
    uint64 virtio_present_copy_minimal_skips; /* minimal-drain fast skips */
    uint64 virtio_present_copy_drain_failures; /* failed present drains */
    uint64 virtio_present_copy_drain_ticks; /* total present drain time */
    uint64 virtio_present_copy_drain_last_us; /* last present drain time */
    uint64 virtio_present_copy_drain_max_us; /* max present drain time */
    /* Append-only ABI: FB_GPU_GET_STATS copies out sizeof(stats) and guest
     * tools may be built against an older layout, so new fields must only be
     * added at the END of this struct. */
    uint64 kms_flips_paced_total; /* flips whose completion event was vblank-paced */
    uint64 kms_paced_delay_us_total; /* summed vblank-edge deferral of paced events (us) */
    uint64 kms_paced_dropped_total; /* paced flip-complete events dropped undelivered */
    /* SLICE 3 (virtio_gpu_async_cursor): async cursor-plane image uploads.
     * Append-only ABI — these MUST stay at the end of the struct. */
    uint64 cursor_async_submits_total; /* cursor image uploads pushed by the async worker */
    uint64 cursor_async_coalesced_total; /* pending cursor images superseded before submit (latest-wins) */
    uint64 cursor_async_errors_total; /* async cursor image uploads that failed in the worker */
    /* virtio_gpu_async_present (gate, DEFAULT OFF): non-blocking DRM page-flip
     * present. The flip ioctl returns in ~us and the blit runs on a worker;
     * DRM_EVENT_FLIP_COMPLETE is delivered at real completion. Append-only ABI
     * — these MUST stay at the end of the struct. */
    uint64 present_async_submits_total; /* page flips accepted into the async worker */
    /* Counts async presents whose blit COMPLETED successfully, not events
     * delivered: a failed present still delivers a flip-complete for liveness
     * (see present_async_errors_total) but is not counted here, and a
     * completion for an owner that closed mid-present is a benign drop yet is
     * still counted as a completed present. */
    uint64 present_async_complete_total; /* async presents whose blit completed OK */
    uint64 present_async_fallback_sync_total; /* flips that fell back to synchronous present */
    uint64 present_async_bo_hold_max_us; /* max BO ref-hold (host DMA) window, us */
    uint64 present_async_errors_total; /* async presents whose blit FAILED (event still delivered for liveness; scanout not advanced) */
    uint64 present_clock60_events_total; /* flip-complete events issued on the free-running 60Hz present clock (gate virtio_gpu_present_clock_60hz) */
    uint64 present_clock60_snap_total; /* present-clock frames snapped forward past a missed grid edge (async worker ran long / host stall) */
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
int  fb_gpu_register_render_node(void);
int  fb_init_virtio_gpu_scanout(uint32 width, uint32 height);
int  fb_init_virtio_gpu_scanout_backing(uint32 width, uint32 height,
                                        void *backing, uint32 backing_size,
                                        uint32 pitch);
int  fb_replace_virtio_gpu_scanout_backing(uint32 width, uint32 height,
                                           void *backing, uint32 backing_size,
                                           uint32 pitch);
int  fb_detected(void);
void fb_pci_init(uint8 bus, uint8 dev, uint8 func);
void fb_get_resolution(uint32 *xres, uint32 *yres);
void fb_gpu_destroy_owner(pid_t owner_tgid);
void fb_panic_screen(const char *text);

#endif /* __KERNEL_DEV_FB_H */
