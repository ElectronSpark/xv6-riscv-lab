#ifndef __UAPI_DRM_H
#define __UAPI_DRM_H

#include <types.h>

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
#define DRM_IOCTL_MODE_SETCRTC                 0xc06864a2UL
#define DRM_IOCTL_MODE_GETENCODER              0xc01464a6UL
#define DRM_IOCTL_MODE_GETCONNECTOR            0xc05064a7UL
#define DRM_IOCTL_MODE_GETPROPERTY             0xc04064aaUL
#define DRM_IOCTL_MODE_GETPROPBLOB             0xc01064acUL
#define DRM_IOCTL_MODE_RMFB                    0xc00464afUL
#define DRM_IOCTL_MODE_PAGE_FLIP               0xc01864b0UL
#define DRM_IOCTL_MODE_CREATE_DUMB             0xc02064b2UL
#define DRM_IOCTL_MODE_MAP_DUMB                0xc01064b3UL
#define DRM_IOCTL_MODE_DESTROY_DUMB            0xc00464b4UL
#define DRM_IOCTL_MODE_GETPLANERESOURCES       0xc01064b5UL
#define DRM_IOCTL_MODE_GETPLANE                0xc02064b6UL
#define DRM_IOCTL_MODE_ADDFB2                  0xc06864b8UL
#define DRM_IOCTL_MODE_ATOMIC                  0xc03864bcUL
#define DRM_IOCTL_SYNCOBJ_CREATE               0xc00864bfUL
#define DRM_IOCTL_SYNCOBJ_DESTROY              0xc00864c0UL
#define DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD         0xc01064c1UL
#define DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE         0xc01064c2UL
#define DRM_IOCTL_SYNCOBJ_WAIT                 0xc02864c3UL
#define DRM_IOCTL_SYNCOBJ_RESET                0xc01064c4UL
#define DRM_IOCTL_SYNCOBJ_SIGNAL               0xc01064c5UL
#define DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT        0xc03064caUL
#define DRM_IOCTL_SYNCOBJ_QUERY                0xc01864cbUL
#define DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL      0xc01864cdUL
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
#define DRM_IOCTL_NOUVEAU_GETPARAM             0xc0106440UL
#define DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC        0xc0586442UL
#define DRM_IOCTL_NOUVEAU_CHANNEL_FREE         0x40046443UL
#define DRM_IOCTL_NOUVEAU_VM_INIT              0xc0106450UL
#define DRM_IOCTL_NOUVEAU_VM_BIND              0xc0286451UL
#define DRM_IOCTL_NOUVEAU_EXEC                 0xc0286452UL
#define DRM_IOCTL_NOUVEAU_GEM_NEW              0xc0306480UL
#define DRM_IOCTL_NOUVEAU_GEM_PUSHBUF          0xc0406481UL
#define DRM_IOCTL_NOUVEAU_GEM_CPU_PREP         0x40086482UL
#define DRM_IOCTL_NOUVEAU_GEM_CPU_FINI         0x40046483UL
#define DRM_IOCTL_NOUVEAU_GEM_INFO             0xc0286484UL

#define DRM_CAP_DUMB_BUFFER             0x1
#define DRM_CAP_VBLANK_HIGH_CRTC        0x2
#define DRM_CAP_DUMB_PREFERRED_DEPTH    0x3
#define DRM_CAP_DUMB_PREFER_SHADOW      0x4
#define DRM_CAP_PRIME                   0x5
#define DRM_PRIME_CAP_IMPORT            0x1
#define DRM_PRIME_CAP_EXPORT            0x2
#define DRM_CAP_TIMESTAMP_MONOTONIC     0x6
#define DRM_CAP_ASYNC_PAGE_FLIP         0x7
#define DRM_CAP_CURSOR_WIDTH            0x8
#define DRM_CAP_CURSOR_HEIGHT           0x9
#define DRM_CAP_ADDFB2_MODIFIERS        0x10
#define DRM_CAP_PAGE_FLIP_TARGET        0x11
#define DRM_CAP_CRTC_IN_VBLANK_EVENT    0x12
#define DRM_CAP_SYNCOBJ                 0x13
#define DRM_CAP_SYNCOBJ_TIMELINE        0x14
#define DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP  0x15

#define DRM_CLIENT_CAP_STEREO_3D        1
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#define DRM_CLIENT_CAP_ATOMIC           3
#define DRM_CLIENT_CAP_ASPECT_RATIO     4
#define DRM_CLIENT_CAP_WRITEBACK_CONNECTORS 5

#define DRM_MODE_TYPE_PREFERRED         (1 << 3)
#define DRM_MODE_TYPE_DRIVER            (1 << 6)
#define DRM_MODE_FLAG_NHSYNC            (1 << 1)
#define DRM_MODE_FLAG_NVSYNC            (1 << 3)
#define DRM_MODE_ENCODER_VIRTUAL        5
#define DRM_MODE_CONNECTOR_VIRTUAL      15
#define DRM_MODE_CONNECTED              1
#define DRM_MODE_SUBPIXEL_UNKNOWN       1
#define DRM_MODE_FB_INTERLACED          (1 << 0)
#define DRM_MODE_FB_MODIFIERS           (1 << 1)
#define DRM_MODE_PAGE_FLIP_EVENT        0x01
#define DRM_MODE_PAGE_FLIP_ASYNC        0x02
#define DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE 0x4
#define DRM_MODE_PAGE_FLIP_TARGET_RELATIVE 0x8
#define DRM_MODE_PAGE_FLIP_FLAGS \
    (DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_ASYNC | \
     DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE | DRM_MODE_PAGE_FLIP_TARGET_RELATIVE)
#define DRM_MODE_ATOMIC_TEST_ONLY       0x0100
#define DRM_MODE_ATOMIC_NONBLOCK        0x0200
#define DRM_MODE_ATOMIC_ALLOW_MODESET   0x0400
#define DRM_MODE_ATOMIC_FLAGS \
    (DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_ASYNC | \
     DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_NONBLOCK | \
     DRM_MODE_ATOMIC_ALLOW_MODESET)
#define DRM_MODE_PROP_PENDING           (1 << 0)
#define DRM_MODE_PROP_RANGE             (1 << 1)
#define DRM_MODE_PROP_IMMUTABLE         (1 << 2)
#define DRM_MODE_PROP_ENUM              (1 << 3)
#define DRM_MODE_PROP_BLOB              (1 << 4)
#define DRM_MODE_PROP_BITMASK           (1 << 5)
#define DRM_MODE_PROP_TYPE(n)           ((n) << 6)
#define DRM_MODE_PROP_OBJECT            DRM_MODE_PROP_TYPE(1)
#define DRM_MODE_PROP_SIGNED_RANGE      DRM_MODE_PROP_TYPE(2)
#define DRM_MODE_PROP_ATOMIC            0x80000000U

#define DRM_FORMAT_ARGB8888             0x34325241U
#define DRM_FORMAT_XRGB8888             0x34325258U
#define DRM_FORMAT_MOD_LINEAR           0ULL

#define DRM_SYNCOBJ_CREATE_SIGNALED     (1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL (1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT (1 << 1)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE (1 << 2)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE (1 << 3)
#define DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED (1 << 0)

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

#define NOUVEAU_GETPARAM_PCI_VENDOR       3
#define NOUVEAU_GETPARAM_PCI_DEVICE       4
#define NOUVEAU_GETPARAM_BUS_TYPE         5
#define NOUVEAU_GETPARAM_FB_SIZE          8
#define NOUVEAU_GETPARAM_AGP_SIZE         9
#define NOUVEAU_GETPARAM_CHIPSET_ID       11
#define NOUVEAU_GETPARAM_VM_VRAM_BASE     12
#define NOUVEAU_GETPARAM_GRAPH_UNITS      13
#define NOUVEAU_GETPARAM_PTIMER_TIME      14
#define NOUVEAU_GETPARAM_HAS_BO_USAGE     15
#define NOUVEAU_GETPARAM_HAS_PAGEFLIP     16
#define NOUVEAU_GETPARAM_EXEC_PUSH_MAX    17
#define NOUVEAU_GETPARAM_VRAM_BAR_SIZE    18
#define NOUVEAU_GETPARAM_VRAM_USED        19
#define NOUVEAU_GETPARAM_HAS_VMA_TILEMODE 20

#define NOUVEAU_GEM_DOMAIN_CPU      (1U << 0)
#define NOUVEAU_GEM_DOMAIN_VRAM     (1U << 1)
#define NOUVEAU_GEM_DOMAIN_GART     (1U << 2)
#define NOUVEAU_GEM_DOMAIN_MAPPABLE (1U << 3)
#define NOUVEAU_GEM_DOMAIN_COHERENT (1U << 4)
#define NOUVEAU_GEM_DOMAIN_NO_SHARE (1U << 5)
#define NOUVEAU_GEM_VALID_DOMAINS \
    (NOUVEAU_GEM_DOMAIN_CPU | NOUVEAU_GEM_DOMAIN_VRAM | \
     NOUVEAU_GEM_DOMAIN_GART | NOUVEAU_GEM_DOMAIN_MAPPABLE | \
     NOUVEAU_GEM_DOMAIN_COHERENT | NOUVEAU_GEM_DOMAIN_NO_SHARE)
#define NOUVEAU_GEM_MAX_BUFFERS     1024
#define NOUVEAU_GEM_MAX_RELOCS      1024
#define NOUVEAU_GEM_MAX_PUSH        512
#define NOUVEAU_GEM_CPU_PREP_NOWAIT 0x00000001
#define NOUVEAU_GEM_CPU_PREP_WRITE  0x00000004

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

struct drm_mode_property_enum_compat {
    uint64 value;
    char name[32];
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

struct drm_mode_get_plane_compat {
    uint32 plane_id;
    uint32 crtc_id;
    uint32 fb_id;
    uint32 possible_crtcs;
    uint32 gamma_size;
    uint32 count_format_types;
    uint64 format_type_ptr;
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

struct drm_mode_fb_cmd2_compat {
    uint32 fb_id;
    uint32 width;
    uint32 height;
    uint32 pixel_format;
    uint32 flags;
    uint32 handles[4];
    uint32 pitches[4];
    uint32 offsets[4];
    uint64 modifier[4];
};

struct drm_mode_crtc_page_flip_compat {
    uint32 crtc_id;
    uint32 fb_id;
    uint32 flags;
    uint32 reserved;
    uint64 user_data;
};

struct drm_mode_atomic_compat {
    uint32 flags;
    uint32 count_objs;
    uint64 objs_ptr;
    uint64 count_props_ptr;
    uint64 props_ptr;
    uint64 prop_values_ptr;
    uint64 reserved;
    uint64 user_data;
};

struct drm_syncobj_create_compat {
    uint32 handle;
    uint32 flags;
};

struct drm_syncobj_destroy_compat {
    uint32 handle;
    uint32 pad;
};

struct drm_syncobj_handle_compat {
    uint32 handle;
    uint32 flags;
    int32 fd;
    uint32 pad;
};

struct drm_syncobj_wait_compat {
    uint64 handles;
    int64 timeout_nsec;
    uint32 count_handles;
    uint32 flags;
    uint32 first_signaled;
    uint32 pad;
    uint64 deadline_nsec;
};

struct drm_syncobj_timeline_wait_compat {
    uint64 handles;
    uint64 points;
    int64 timeout_nsec;
    uint32 count_handles;
    uint32 flags;
    uint32 first_signaled;
    uint32 pad;
    uint64 deadline_nsec;
};

struct drm_syncobj_array_compat {
    uint64 handles;
    uint32 count_handles;
    uint32 pad;
};

struct drm_syncobj_timeline_array_compat {
    uint64 handles;
    uint64 points;
    uint32 count_handles;
    uint32 flags;
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
    uint32 x;
    uint32 y;
    uint32 z;
    uint32 w;
    uint32 h;
    uint32 d;
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

struct drm_nouveau_getparam_compat {
    uint64 param;
    uint64 value;
};

struct drm_nouveau_channel_alloc_compat {
    uint32 fb_ctxdma_handle;
    uint32 tt_ctxdma_handle;
    int32 channel;
    uint32 pushbuf_domains;
    uint32 notifier_handle;
    struct {
        uint32 handle;
        uint32 grclass;
    } subchan[8];
    uint32 nr_subchan;
};

struct drm_nouveau_channel_free_compat {
    int32 channel;
};

struct drm_nouveau_gem_info_compat {
    uint32 handle;
    uint32 domain;
    uint64 size;
    uint64 offset;
    uint64 map_handle;
    uint32 tile_mode;
    uint32 tile_flags;
};

struct drm_nouveau_gem_new_compat {
    struct drm_nouveau_gem_info_compat info;
    uint32 channel_hint;
    uint32 align;
};

struct drm_nouveau_gem_pushbuf_compat {
    uint32 channel;
    uint32 nr_buffers;
    uint64 buffers;
    uint32 nr_relocs;
    uint32 nr_push;
    uint64 relocs;
    uint64 push;
    uint32 suffix0;
    uint32 suffix1;
    uint64 vram_available;
    uint64 gart_available;
};

struct drm_nouveau_gem_cpu_prep_compat {
    uint32 handle;
    uint32 flags;
};

struct drm_nouveau_gem_cpu_fini_compat {
    uint32 handle;
};

struct drm_nouveau_vm_init_compat {
    uint64 kernel_managed_addr;
    uint64 kernel_managed_size;
};

struct drm_nouveau_vm_bind_compat {
    uint32 op_count;
    uint32 flags;
    uint32 wait_count;
    uint32 sig_count;
    uint64 wait_ptr;
    uint64 sig_ptr;
    uint64 op_ptr;
};

struct drm_nouveau_exec_compat {
    uint32 channel;
    uint32 push_count;
    uint32 wait_count;
    uint32 sig_count;
    uint64 wait_ptr;
    uint64 sig_ptr;
    uint64 push_ptr;
};

#endif /* __UAPI_DRM_H */
