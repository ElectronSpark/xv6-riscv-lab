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
#define FB_GPU_TTM_VALIDATE  0x4632 /* test/validate TTM placement state */

#define FB_GPU_DXG_DISPLAY_TARGET_NONE          0
#define FB_GPU_DXG_PRESENT_MISSING_NONE         0
#define FB_GPU_DXG_PRESENT_MISSING_SCANOUT_BIND 1

#define FB_GPU_DXG_PRESENT_HOST_SYNTHVID        0x1
#define FB_GPU_DXG_PRESENT_HOST_DXG             0x2
#define FB_GPU_DXG_PRESENT_REJECT_SYNTHVID_GPA_ONLY 0x1
#define FB_GPU_DXG_PRESENT_REJECT_DXG_NO_DISPLAY_BIND 0x2

#define FB_GPU_DXG_PRESENT_LANE_NONE            0
#define FB_GPU_DXG_PRESENT_LANE_HELPER_SCANOUT_BIND 1
#define FB_GPU_DXG_PRESENT_BLOCK_NO_TRANSPORT   0x0001
#define FB_GPU_DXG_PRESENT_BLOCK_SYNTHVID_GPA_ONLY 0x0002
#define FB_GPU_DXG_PRESENT_BLOCK_DXG_NO_DISPLAY_BIND 0x0004
#define FB_GPU_DXG_PRESENT_BLOCK_LUID_UNVERIFIED 0x0008
#define FB_GPU_DXG_PRESENT_BLOCK_NO_REGISTERED_SOURCE 0x0010
#define FB_GPU_DXG_PRESENT_BLOCK_RESOURCE_FD_UNVERIFIED 0x0020
#define FB_GPU_DXG_PRESENT_BLOCK_ADAPTER_MISMATCH 0x0040
#define FB_GPU_DXG_PRESENT_BLOCK_NO_COMPLETION 0x0080
#define FB_GPU_DXG_PRESENT_BLOCK_ALL \
    (FB_GPU_DXG_PRESENT_BLOCK_NO_TRANSPORT | \
     FB_GPU_DXG_PRESENT_BLOCK_SYNTHVID_GPA_ONLY | \
     FB_GPU_DXG_PRESENT_BLOCK_DXG_NO_DISPLAY_BIND | \
     FB_GPU_DXG_PRESENT_BLOCK_LUID_UNVERIFIED | \
     FB_GPU_DXG_PRESENT_BLOCK_NO_REGISTERED_SOURCE | \
     FB_GPU_DXG_PRESENT_BLOCK_RESOURCE_FD_UNVERIFIED | \
     FB_GPU_DXG_PRESENT_BLOCK_ADAPTER_MISMATCH | \
     FB_GPU_DXG_PRESENT_BLOCK_NO_COMPLETION)

#define FB_GPU_DXG_PRESENT_PROV_DXG_FD          0x0001
#define FB_GPU_DXG_PRESENT_PROV_RESOURCE_FD     0x0002
#define FB_GPU_DXG_PRESENT_PROV_D3DKMT_HANDLES  0x0004
#define FB_GPU_DXG_PRESENT_PROV_DIMENSIONS      0x0008
#define FB_GPU_DXG_PRESENT_PROV_ADAPTER_LUID    0x0010

#define FB_GPU_DXG_PRESENT_ADAPTER_UNKNOWN      0
#define FB_GPU_DXG_PRESENT_ADAPTER_UNVERIFIED   1
#define FB_GPU_DXG_PRESENT_ADAPTER_MATCH        2
#define FB_GPU_DXG_PRESENT_ADAPTER_MISMATCH     3

#define FB_GPU_DXG_PRESENT_HELPER_TRANSPORT_NONE   0
#define FB_GPU_DXG_PRESENT_HELPER_TRANSPORT_VMBUS  1
#define FB_GPU_DXG_PRESENT_HELPER_TRANSPORT_HVSOCK 2
#define FB_GPU_DXG_PRESENT_HELPER_OP_SCANOUT_BIND  1

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

#define FB_GPU_BO_F_EXPORTABLE 0x1    /* return a stable kernel handle */
#define FB_GPU_BO_FENCE_WAIT 0x1      /* wait_for must be signaled */
#define FB_GPU_FENCE_WAIT 0x1         /* wait for fence fd to signal */
#define FB_GPU_VIRGL_FENCE_WAIT 0x1   /* wait_for must be signaled */
#define FB_GPU_VIRGL_SUBMIT_ASYNC 0x1 /* return once the command is queued */
#define FB_GPU_VIRGL_SUBMIT_FORCE_FAIL 0x80000000u /* test-only context fault */
#define FB_GPU_DISPLAY_WAIT_F_WAIT 0x1 /* wait_for must be complete */
#define FB_GPU_DXG_PRESENT_F_WAIT_SYNC 0x1 /* wait for sync_object/fence_value */

#define FB_GPU_BO_FORMAT_XRGB8888 0x34325258u /* DRM_FORMAT_XRGB8888 */
#define FB_GPU_BO_FORMAT_ARGB8888 0x34325241u /* DRM_FORMAT_ARGB8888 */
#define FB_GPU_BO_FORMAT_NV12     0x3231564eu /* DRM_FORMAT_NV12 */
#define FB_GPU_BO_MOD_LINEAR      0ULL        /* DRM_FORMAT_MOD_LINEAR */

#define FB_GPU_TTM_PL_SYSTEM 0x0001u
#define FB_GPU_TTM_PL_TT     0x0002u
#define FB_GPU_TTM_PL_VRAM   0x0004u
#define FB_GPU_TTM_PL_STOLEN 0x0008u

#define FB_GPU_TTM_F_SET_PLACEMENT 0x0001u
#define FB_GPU_TTM_F_PIN           0x0002u
#define FB_GPU_TTM_F_UNPIN         0x0004u
#define FB_GPU_TTM_F_FORCE_EVICT   0x0008u

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
};

struct fb_gpu_ttm_validate {
    uint32 handle;
    uint32 flags;
    uint32 placement;
    uint32 mem_type;
    uint32 pin_count;
    uint32 tt_populated;
    uint32 sg_nents;
    uint32 reserved;
    uint64 size;
    uint64 dma_addr_base;
    uint64 reservation_seq;
    uint64 lru_seq;
    uint64 move_count;
    uint64 manager_bytes[4];
    uint64 evictions;
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
    uint64   present_id;      /* always 0 until host helper exists */
    uint64   completed;       /* always 0 until host helper exists */
    uint64   host_handoff_missing; /* commits blocked before host helper */
    uint64   requires_host_protocol; /* nonzero while fail-closed */
    uint64   missing_host_abi; /* FB_GPU_DXG_PRESENT_MISSING_* */
    uint64   helper_contract_version;
    uint64   helper_required_metadata;
    uint64   helper_transport;
    uint64   helper_transport_present;
    uint64   helper_operation;
    uint64   helper_lifetime;
    uint64   helper_requires_completion;
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
    uint32   helper_block_reason;
    uint64   host_candidates;
    uint64   host_rejects;
};

/*
 * Contract for a future host-display-helper resource scanout bind.
 *
 * This is intentionally not wired to an ioctl yet.  The current kernel has no
 * discoverable Hyper-V socket service or VMBus offer for such a helper, and
 * synthvid only accepts a guest physical VRAM address plus dirty rectangles.
 * A real helper must consume the same-adapter D3DKMT handles below and return
 * host-correlated present/completion sequence numbers before
 * FB_GPU_DXG_PRESENT_SOURCE_COMMIT can report success.
 */
struct fb_gpu_dxg_present_host_bind_contract {
    uint32 version;       /* set to 1 */
    uint32 transport;     /* FB_GPU_DXG_PRESENT_HELPER_TRANSPORT_* */
    uint32 operation;     /* FB_GPU_DXG_PRESENT_HELPER_OP_* */
    uint32 flags;         /* FB_GPU_DXG_PRESENT_F_* */
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
    uint64 helper_block_reason;
    int32 dxg_fd;
    int32 resource_fd;
    uint32 adapter_identity;
    uint32 reserved2;
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
    uint64 drm_primary_opens;  /* /dev/dri/card0 opens */
    uint64 drm_render_opens;   /* /dev/dri/renderD128 opens */
    uint64 drm_primary_live;   /* currently open primary DRM files */
    uint64 drm_render_live;    /* currently open render DRM files */
    uint64 drm_ioctls;         /* DRM ioctl calls through card/render nodes */
    uint64 drm_unknown_ioctls; /* unknown DRM ioctl commands rejected */
    uint64 drm_auths;          /* primary-node auth ioctls accepted */
    uint64 drm_master_sets;    /* primary-node SET_MASTER successes */
    uint64 drm_master_drops;   /* primary-node DROP_MASTER successes */
    uint64 nouveau_ioctl_entries; /* Nouveau private ioctl calls entered */
    uint64 nouveau_fail_closed; /* Nouveau ioctls rejected because absent */
    uint64 nouveau_getparams;  /* Nouveau GETPARAM calls accepted */
    uint64 nouveau_channel_allocs; /* Nouveau channel alloc successes */
    uint64 nouveau_channel_frees; /* Nouveau channel free successes */
    uint64 nouveau_gem_news;   /* Nouveau GEM_NEW successes */
    uint64 nouveau_gem_infos;  /* Nouveau GEM_INFO successes */
    uint64 nouveau_cpu_preps;  /* Nouveau GEM_CPU_PREP successes */
    uint64 nouveau_cpu_finis;  /* Nouveau GEM_CPU_FINI successes */
    uint64 nouveau_vm_inits;   /* Nouveau VM_INIT successes */
    uint64 nouveau_vm_bind_noops; /* zero-op Nouveau VM_BIND successes */
    uint64 nouveau_pushbuf_noops; /* zero-push Nouveau GEM_PUSHBUF successes */
    uint64 nouveau_exec_noops; /* zero-push Nouveau EXEC successes */
    uint64 nouveau_unsupported; /* recognized but not implemented Nouveau ops */
    uint64 kms_framebuffers;   /* currently registered KMS framebuffer IDs */
    uint64 kms_page_flips;     /* accepted KMS page flips */
    uint64 kms_atomic_commits; /* accepted/tested atomic commits */
    uint64 ttm_system_bytes;   /* BO bytes in system placement */
    uint64 ttm_tt_bytes;       /* BO bytes in GART/TT placement */
    uint64 ttm_vram_bytes;     /* BO bytes in VRAM placement */
    uint64 ttm_stolen_bytes;   /* BO bytes in stolen/scanout placement */
    uint64 ttm_pinned_bytes;   /* BO bytes with nonzero pin count */
    uint64 ttm_validate_failures; /* rejected placement/validation requests */
    uint64 syncobj_created;    /* DRM syncobjs created */
    uint64 syncobj_live;       /* currently live DRM syncobjs */
    uint64 syncobj_signals;    /* binary/timeline signal operations */
    uint64 syncobj_waits;      /* binary/timeline wait operations */
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
    uint64 dxg_present_helper_block_reason; /* FB_GPU_DXG_PRESENT_BLOCK_* */
    uint64 dxg_present_display_target_kind; /* FB_GPU_DXG_DISPLAY_TARGET_* */
    uint64 dxg_present_requires_host_protocol; /* no kernel/host bind ABI */
    uint64 dxg_present_missing_host_abi; /* FB_GPU_DXG_PRESENT_MISSING_* */
    uint64 dxg_present_host_candidates; /* FB_GPU_DXG_PRESENT_HOST_* */
    uint64 dxg_present_host_rejects; /* FB_GPU_DXG_PRESENT_REJECT_* */
    uint64 dxg_present_synthvid_state; /* present/open/init/dirt bitfield */
    uint64 dxg_present_synthvid_vram_gpa; /* current synthvid VRAM GPA */
    uint64 dxg_present_dxg_state; /* global/vgpu/d3dkmt readiness bitfield */
    uint64 dxg_present_helper_contract_version; /* passive helper ABI version */
    uint64 dxg_present_helper_required_metadata; /* FB_GPU_DXG_PRESENT_META_* */
    uint64 dxg_present_helper_transport; /* FB_GPU_DXG_PRESENT_HELPER_TRANSPORT_* */
    uint64 dxg_present_helper_transport_present; /* nonzero once host service exists */
    uint64 dxg_present_helper_operation; /* FB_GPU_DXG_PRESENT_HELPER_OP_* */
    uint64 dxg_present_helper_lifetime; /* FB_GPU_DXG_PRESENT_LIFE_* */
    uint64 dxg_present_helper_source_live; /* registered source still tracked */
    uint64 dxg_present_helper_requires_completion; /* present_id/completed required */
    uint64 dxg_present_commit_no_source; /* commits without live registered source */
    uint64 dxg_present_commit_bad_flags; /* commits rejected by flags/sync contract */
    uint64 dxg_present_commit_adapter_mismatch; /* source/target adapter mismatch */
    uint64 dxg_present_commit_resource_fd_unverified; /* no verifiable dxgresource fd */
    uint64 dxg_present_commit_no_transport; /* no host helper transport available */
    uint64 dxg_present_commit_no_completion; /* no present completion source */
    uint64 display_presents;  /* display present/flush operations issued */
    uint64 display_completions; /* display present completions observed */
    uint64 display_last_present; /* latest issued display-present sequence */
    uint64 display_last_complete; /* latest completed display-present sequence */
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
