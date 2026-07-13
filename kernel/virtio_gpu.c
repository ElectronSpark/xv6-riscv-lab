//
// Minimal virtio-gpu PCI transport bring-up.
//
// Bochs BGA remains the fallback scanout path.  When QEMU boots with a
// virtio-gpu primary display, /dev/fb0 can point directly at the persistent
// virtio scanout resource backing so presents avoid a second shadow copy.
//

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "errno.h"
#include "printf.h"
#include "param.h"
#include "trap.h"
#include "cmdline.h"
#include "timer/timer.h"
#include "lock/completion.h"
#include <mm/memlayout.h>
#include "lock/mutex.h"
#include "lock/spinlock.h"
#include "dev/fb.h"
#include "dev/virtio.h"
#include "dev/pci.h"
#include <mm/pgtable.h>
#include <mm/page.h>
#include <mm/rmap.h>
#include <mm/vm.h>
#include <proc/thread.h>
#include <proc/workqueue.h>
#include "arch/vm.h"

#if defined(__x86_64__) || defined(__i386__)

#define VIRTIO_GPU_MAX_SCANOUTS 16
#define VIRTIO_GPU_POLL_LIMIT 10000000
#define VIRTIO_GPU_FAST_POLL_LIMIT 20000
#define VIRTIO_GPU_IRQ_WAIT_MS 5000
#define VIRTIO_GPU_IRQ_WAIT_MS_MAX 60000
#define VIRTIO_GPU_IRQ_WAIT_SLICE_MS 1
#define VIRTIO_GPU_SCANOUT_READ_LOCK_WAIT_MS 3000
#define VIRTIO_GPU_SCANOUT_READ_LOCK_WAIT_MS_MAX 30000

#define VIRTIO_GPU_F_VIRGL          0
#define VIRTIO_GPU_F_EDID           1
#define VIRTIO_GPU_F_RESOURCE_BLOB  3
#define VIRTIO_GPU_F_CONTEXT_INIT   4

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO 0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D 0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF     0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT        0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH     0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO    0x0108
#define VIRTIO_GPU_CMD_GET_CAPSET         0x0109
#define VIRTIO_GPU_CMD_GET_EDID           0x010a
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB 0x010c
#define VIRTIO_GPU_CMD_CTX_CREATE         0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY        0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE 0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE 0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D  0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D 0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D 0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D          0x0207
#define VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB  0x0208
#define VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB 0x0209
#define VIRTIO_GPU_CMD_UPDATE_CURSOR      0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR        0x0301

/* virtio-gpu cursor resources are a fixed 64x64 BGRA image. */
#define VIRTIO_GPU_CURSOR_DIM   64
#define VIRTIO_GPU_CURSOR_IMAGE_SLOTS 16
#define VIRTIO_GPU_CURSOR_RING  64
#define VIRTIO_GPU_INJECTED_CURSOR_W 16
#define VIRTIO_GPU_INJECTED_CURSOR_H 24
#define VIRTIO_GPU_RESP_OK_NODATA       0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO 0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO  0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET       0x1103
#define VIRTIO_GPU_RESP_OK_EDID         0x1104
#define VIRTIO_GPU_RESP_OK_MAP_INFO     0x1106
#define VIRTIO_GPU_RESP_ERR_UNSPEC      0x1200

#define VIRTIO_GPU_FLAG_FENCE  (1 << 0)
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1
#define VIRTIO_GPU_BLOB_MEM_GUEST             0x0001
#define VIRTIO_GPU_BLOB_MEM_HOST3D            0x0002
#define VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST      0x0003
#define VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE     0x0001
#define VIRTIO_GPU_BLOB_FLAG_USE_SHAREABLE    0x0002
#define VIRTIO_GPU_BLOB_FLAG_USE_CROSS_DEVICE 0x0004
#define VIRTIO_GPU_SHM_ID_HOST_VISIBLE         1
#define VIRTIO_GPU_MAP_CACHE_MASK              0x0f
#define VIRTIO_GPU_CAPSET_VIRGL          1
#define VIRTIO_GPU_CAPSET_VIRGL2         2
#define VIRTIO_GPU_CAPSET_DRM            6
#define VIRTIO_GPU_SMOKE_WIDTH 32
#define VIRTIO_GPU_SMOKE_HEIGHT 32
#define VIRTIO_GPU_MAX_RESOURCES 16384
#define VIRTIO_GPU_MAX_CONTEXTS 256
#define VIRTIO_GPU_SUBMIT_TRACE_OWNER_SLOTS VIRTIO_GPU_MAX_CONTEXTS
#define VIRTIO_GPU_SUBMIT_TRACE_PROC_LEN 16
#define VIRTIO_GPU_SUBMIT_TRACE_DEBUG_LEN 64
#define VIRTIO_GPU_RESOURCE_MAX_CONTEXT_ATTACHMENTS 8
#define VIRTIO_GPU_MAX_CAPSETS 8
#define VIRTIO_GPU_PAGE_FLIP_SCANOUT_SET_MAX 4
#define VIRGL_CCMD_NOP 0
#define VIRGL_CCMD_CREATE_OBJECT 1
#define VIRGL_CCMD_BIND_OBJECT 2
#define VIRGL_CCMD_DESTROY_OBJECT 3
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE 4
#define VIRGL_CCMD_SET_VERTEX_BUFFERS 5
#define VIRGL_CCMD_CLEAR 6
#define VIRGL_CCMD_DRAW_VBO 7
#define VIRGL_CCMD_RESOURCE_INLINE_WRITE 8
#define VIRGL_CCMD_SET_SAMPLER_VIEWS 9
#define VIRGL_CCMD_SET_INDEX_BUFFER 10
#define VIRGL_CCMD_SET_CONSTANT_BUFFER 11
#define VIRGL_CCMD_BLIT 16
#define VIRGL_CCMD_RESOURCE_COPY_REGION 17
#define VIRGL_CCMD_SET_SUB_CTX 27
#define VIRGL_CCMD_CREATE_SUB_CTX 28
#define VIRGL_CCMD_DESTROY_SUB_CTX 29
#define VIRGL_CCMD_TEXTURE_BARRIER 38
#define VIRGL_CCMD_COPY_TRANSFER3D 45
#define VIRGL_CCMD_CLEAR_TEXTURE 47
#define VIRGL_CMD_BLIT_SIZE 21
#define VIRGL_CMD_RESOURCE_COPY_REGION_SIZE 13
#define VIRGL_COPY_TRANSFER3D_SIZE 14
#define VIRGL_COPY_TRANSFER3D_FLAGS_SYNCHRONIZED (1u << 0)
#define VIRGL_COPY_TRANSFER3D_FLAGS_READ_FROM_HOST (1u << 1)
#define VIRGL_CLEAR_TEXTURE_SIZE 12
#define VIRGL_CMD0(cmd, obj, len) ((cmd) | ((obj) << 8) | ((len) << 16))
#define VIRGL_CMD_BLIT_S0_MASK(x) (((x) & 0xff) << 0)
#define VIRGL_CMD_BLIT_S0_FILTER(x) (((x) & 0x3) << 8)
#define VIRGL_PIPE_MASK_RGBA 0xf
#define VIRGL_PIPE_TEX_FILTER_NEAREST 0
#define VIRGL_FORMAT_B8G8R8A8_UNORM 1
#define VIRTIO_GPU_PIPE_TEXTURE_2D 2
#define VIRTIO_GPU_PIPE_BIND_RENDER_TARGET  (1u << 1)
#define VIRTIO_GPU_PIPE_BIND_SAMPLER_VIEW   (1u << 3)
#define VIRTIO_GPU_PIPE_BIND_DISPLAY_TARGET (1u << 7)
#define VIRTIO_GPU_PIPE_BIND_SCANOUT        (1u << 18)
#define VIRTIO_GPU_PIPE_BIND_SHARED         (1u << 20)
#define VIRTIO_GPU_PIPE_BIND_LINEAR         (1u << 22)
#define VIRTIO_GPU_PRESENT_SAMPLE_COUNT 5

extern pagetable_t kernel_pagetable;

struct virtio_gpu_config {
    uint32 events_read;
    uint32 events_clear;
    uint32 num_scanouts;
    uint32 num_capsets;
};

struct virtio_gpu_ctrl_hdr {
    uint32 type;
    uint32 flags;
    uint64 fence_id;
    uint32 ctx_id;
    uint8 ring_idx;
    uint8 padding[3];
};

struct virtio_gpu_rect {
    uint32 x;
    uint32 y;
    uint32 width;
    uint32 height;
};

struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32 enabled;
    uint32 flags;
};

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
};

struct virtio_gpu_cursor_pos {
    uint32 scanout_id;
    uint32 x;
    uint32 y;
    uint32 padding;
};

struct virtio_gpu_update_cursor {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_cursor_pos pos;
    uint32 resource_id;
    uint32 hot_x;
    uint32 hot_y;
    uint32 padding;
};

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 resource_id;
    uint32 format;
    uint32 width;
    uint32 height;
};

struct virtio_gpu_resource_create_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 resource_id;
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
    uint32 padding;
};

struct virtio_gpu_resource_create_blob {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 resource_id;
    uint32 blob_mem;
    uint32 blob_flags;
    uint32 nr_entries;
    uint64 blob_id;
    uint64 size;
};

struct virtio_gpu_resource_map_blob {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 resource_id;
    uint32 padding;
    uint64 offset;
};

struct virtio_gpu_resp_map_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 map_info;
    uint32 padding;
};

struct virtio_gpu_resource_unmap_blob {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 resource_id;
    uint32 padding;
};

struct virtio_gpu_box {
    uint32 x;
    uint32 y;
    uint32 z;
    uint32 w;
    uint32 h;
    uint32 d;
};

struct virtio_gpu_transfer_host_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_box box;
    uint64 offset;
    uint32 resource_id;
    uint32 level;
    uint32 stride;
    uint32 layer_stride;
};

struct virtio_gpu_resource_unref {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 resource_id;
    uint32 padding;
};

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32 scanout_id;
    uint32 resource_id;
};

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 resource_id;
    uint32 nr_entries;
};

struct virtio_gpu_mem_entry {
    uint64 addr;
    uint32 length;
    uint32 padding;
};

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64 offset;
    uint32 resource_id;
    uint32 padding;
};

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32 resource_id;
    uint32 padding;
};

struct virtio_gpu_get_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 capset_index;
    uint32 padding;
};

struct virtio_gpu_resp_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 capset_id;
    uint32 capset_max_version;
    uint32 capset_max_size;
    uint32 padding;
};

struct virtio_gpu_get_capset {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 capset_id;
    uint32 capset_version;
};

struct virtio_gpu_resp_capset {
    struct virtio_gpu_ctrl_hdr hdr;
    uint8 capset_data[];
};

struct virtio_gpu_cmd_get_edid {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 scanout;
    uint32 padding;
};

struct virtio_gpu_resp_edid {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 size;
    uint32 padding;
    uint8 edid[1024];
};

struct virtio_gpu_ctx_create {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 nlen;
    uint32 context_init;
    char debug_name[64];
};

struct virtio_gpu_ctx_destroy {
    struct virtio_gpu_ctrl_hdr hdr;
};

struct virtio_gpu_ctx_resource {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 resource_id;
    uint32 padding;
};

struct virtio_gpu_cmd_submit {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 size;
    uint32 padding;
};

struct virtio_gpu_queue {
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    uint16 size;
    uint16 used_idx;
    uint16 notify_off;
    spinlock_t lock;
    /*
     * N1 B2 fix: the in-flight SYNC command record (control queue only).
     * Synchronous commands own the dedicated descriptor block [0, 3)
     * (head id 0); op_lock guarantees at most one is in flight.  The
     * unified reaper maps used element id 0 to this record and signals
     * the waiter through it, so a reaper running concurrently with a
     * sync command (unlocked-wait make_room) can no longer steal the
     * sync completion.  All fields are q->lock-protected; sync_done is
     * additionally read with an ACQUIRE load by the waiter's poll loop.
     * sync_stale counts timed-out sync commands whose used elements have
     * not arrived yet: the device retires a queue's buffers in order, so
     * those elements precede the current in-flight command's element and
     * the reaper must swallow them instead of mis-attributing them to it.
     */
    int sync_inflight;
    int sync_done;
    uint32 sync_stale;
    completion_t *sync_completion;
};

struct virtio_gpu_stats {
    uint64 commands;
    uint64 failures;
    uint64 timeouts;
    uint64 resources;
    uint64 resource_bytes;
    uint64 transfers;
    uint64 flushes;
    uint64 scanouts;
    uint64 capsets;
    uint64 virgl;
    uint64 virgl_version;
    uint64 virgl_size;
    uint64 contexts;
    uint64 context_failed;
    uint64 context_failures;
    uint64 submits;
    uint64 fences;
    uint64 last_fence;
    uint64 irq_completions;
    uint64 poll_fallbacks;
    uint64 async_posted;
    uint64 async_posted_submit_3d;
    uint64 async_posted_flush;
    uint64 async_posted_transfer;
    uint64 async_retired;
    uint64 async_make_room_calls;
    uint64 async_make_room_submit_3d_calls;
    uint64 async_make_room_flush_calls;
    uint64 async_make_room_transfer_calls;
    uint64 async_make_room_stalls;
    uint64 async_make_room_submit_3d_stalls;
    uint64 async_make_room_flush_stalls;
    uint64 async_make_room_transfer_stalls;
    uint64 async_wait_progress_calls;
    uint64 async_make_room_wait_ticks;
    uint64 async_make_room_last_wait_us;
    uint64 async_make_room_max_wait_us;
    uint64 present_copy_calls;
    uint64 present_copy_drain_calls;
    uint64 present_copy_src_fence_drains;
    uint64 present_copy_blanket_drains;
    uint64 present_copy_src_fence_only_drains;
    uint64 present_copy_blanket_only_drains;
    uint64 present_copy_src_fence_blanket_drains;
    uint64 present_copy_no_drain_skips;
    uint64 present_copy_minimal_skips;
    uint64 present_copy_drain_failures;
    uint64 present_copy_drain_ticks;
    uint64 present_copy_drain_last_us;
    uint64 present_copy_drain_max_us;
    uint64 submit_trace_submit_calls;
    uint64 submit_trace_submit_ticks;
    uint64 submit_trace_lock_wait_ticks;
    uint64 submit_trace_resource_attach_count;
    uint64 submit_trace_resource_attach_ticks;
    uint64 submit_trace_async_prepare_ticks;
    uint64 submit_trace_post_ticks;
    uint64 submit_trace_first_submits;
    uint64 submit_trace_failures;
    uint64 submit_trace_fence_calls;
    uint64 submit_trace_fence_ticks;
    uint64 submit_trace_fence_drain_calls;
    uint64 submit_trace_fence_drain_ticks;
    uint64 submit_trace_fence_failures;
    uint64 submit_trace_wait_for_used_calls;
    uint64 submit_trace_wait_for_used_ticks;
    uint64 submit_trace_wait_for_used_max_us;
    uint64 submit_trace_async_wait_progress_calls;
    uint64 submit_trace_async_wait_progress_ticks;
    uint64 submit_trace_async_make_room_ticks;
    uint64 submit_trace_async_make_room_depth_max;
    uint64 submit_trace_async_make_room_count_max;
    uint64 submit_trace_async_make_room_wait_count_max;
    uint64 submit_trace_async_post_count_max;
    uint64 submit_trace_async_retire_ticks;
    uint64 submit_trace_async_retire_max_us;
    uint64 submit_trace_async_retire_submit_3d_ticks;
    uint64 submit_trace_async_retire_submit_3d_max_us;
    uint64 submit_trace_async_retire_flush_ticks;
    uint64 submit_trace_async_retire_flush_max_us;
    uint64 submit_trace_async_retire_transfer_ticks;
    uint64 submit_trace_async_retire_transfer_max_us;
    uint64 submit_trace_emitted_submit_calls;
    uint64 submit_trace_emitted_fence_calls;
    uint64 submit_trace_emitted_wait_for_used_calls;
    uint64 submit_trace_owner_drops;
    uint64 submit_trace_owner_emitted_drops;
};

enum virtio_gpu_async_reason {
    VIRTIO_GPU_ASYNC_REASON_OTHER = 0,
    VIRTIO_GPU_ASYNC_REASON_SUBMIT_3D,
    VIRTIO_GPU_ASYNC_REASON_FLUSH,
    VIRTIO_GPU_ASYNC_REASON_TRANSFER,
};

enum virtio_gpu_op_holder {
    VIRTIO_GPU_OP_NONE = 0,
    VIRTIO_GPU_OP_SUBMIT_3D,
    VIRTIO_GPU_OP_SCANOUT,
    VIRTIO_GPU_OP_CONTEXT,
    VIRTIO_GPU_OP_FENCE,
    VIRTIO_GPU_OP_CAPSET,
    VIRTIO_GPU_OP_RESOURCE,
    VIRTIO_GPU_OP_TRANSFER,
    VIRTIO_GPU_OP_FB_PRESENT,
    VIRTIO_GPU_OP_PRESENT_COPY,
    VIRTIO_GPU_OP_PROBE,
    VIRTIO_GPU_OP_RESIZE,
    VIRTIO_GPU_OP_PAGE_FLIP,
    VIRTIO_GPU_OP_OTHER,
};

struct virtio_gpu_async_drain_sample {
    uint64 submit_3d;
    uint64 flush;
    uint64 transfer;
    uint64 other;
};

struct virtio_gpu_resource {
    int in_use;
    uint32 id;
    uint64 owner_id;
    pid_t owner_tgid;
    uint32 width;
    uint32 height;
    uint32 depth;
    uint32 last_level;
    uint32 format;
    uint32 backing_len;
    uint32 alloc_len;
    uint32 backing_order;
    void *backing;
    page_t **pages;
    uint32 npages;
    int is_3d;
    int attached;
    uint32 ctx_id;
    uint32 ctx_attach_count;
    uint32 ctx_attached[VIRTIO_GPU_RESOURCE_MAX_CONTEXT_ATTACHMENTS];
    uint32 target;
    uint32 bind;
    int is_blob;
    uint32 blob_mem;
    uint32 blob_flags;
    uint64 blob_id;
    int host_visible_mapped;
    uint64 host_visible_offset;
    uint64 host_visible_size;
    uint32 host_visible_map_info;
    uint64 last_submit_fence;
    uint32 export_refs;
    int destroy_pending;
};

struct virtio_gpu_context {
    int in_use;
    int failed;
    int first_submit_seen;
    uint32 id;
    uint32 capset_id;
    uint32 context_init;
    uint64 owner_id;
    pid_t owner_tgid;
    char debug_name[64];
};

struct virtio_gpu_submit_trace_key {
    uint64 owner_id;
    pid_t owner_tgid;
    uint32 ctx_id;
    char proc[VIRTIO_GPU_SUBMIT_TRACE_PROC_LEN];
    char debug_name[VIRTIO_GPU_SUBMIT_TRACE_DEBUG_LEN];
};

struct virtio_gpu_submit_trace_owner_counts {
    uint64 submit_calls;
    uint64 submit_ticks;
    uint64 lock_wait_ticks;
    uint64 attach_count;
    uint64 attach_ticks;
    uint64 async_prepare_ticks;
    uint64 post_ticks;
    uint64 make_room_calls;
    uint64 make_room_stalls;
    uint64 make_room_ticks;
    uint64 make_room_wait_ticks;
    uint64 make_room_max_wait_ticks;
    uint64 make_room_depth_max;
    uint64 make_room_count_max;
    uint64 make_room_wait_count_max;
    uint64 wait_progress_calls;
    uint64 wait_progress_ticks;
    uint64 wait_progress_max_ticks;
    uint64 wait_used_calls;
    uint64 wait_used_ticks;
    uint64 wait_used_max_ticks;
    uint64 posted;
    uint64 post_count_max;
    uint64 retired;
    uint64 retire_ticks;
    uint64 retire_max_ticks;
    uint64 retired_submit_3d;
    uint64 retire_submit_3d_ticks;
    uint64 retire_submit_3d_max_ticks;
    uint64 retired_flush;
    uint64 retire_flush_ticks;
    uint64 retire_flush_max_ticks;
    uint64 retired_transfer;
    uint64 retire_transfer_ticks;
    uint64 retire_transfer_max_ticks;
    uint64 retired_other;
    uint64 retire_other_ticks;
    uint64 retire_other_max_ticks;
    uint64 shape_submit_calls;
    uint64 shape_make_room_calls;
    uint64 shape_make_room_stalls;
    uint64 shape_make_room_ticks;
    uint64 shape_make_room_wait_ticks;
    uint64 shape_make_room_max_wait_ticks;
    uint64 shape_make_room_depth_max;
    uint64 shape_make_room_count_max;
    uint64 shape_make_room_wait_count_max;
    uint64 shape_posted;
    uint64 shape_post_count_max;
    uint64 shape_retired;
    uint64 shape_retire_ticks;
    uint64 shape_retire_max_ticks;
    uint64 shape_failures;
    uint64 shape_mixed;
    struct virtio_gpu_submit_trace_shape shape;
    uint64 first_submit;
    uint64 failures;
    int last_ret;
    uint64 last_fence;
};

struct virtio_gpu_submit_trace_owner {
    int in_use;
    struct virtio_gpu_submit_trace_key key;
    struct virtio_gpu_submit_trace_owner_counts total;
    struct virtio_gpu_submit_trace_owner_counts emitted;
    uint64 pending_make_room_max_wait_ticks;
    uint64 pending_make_room_depth_max;
    uint64 pending_make_room_count_max;
    uint64 pending_make_room_wait_count_max;
    uint64 pending_post_count_max;
    uint64 pending_retire_max_ticks;
    uint64 pending_retire_submit_3d_max_ticks;
    uint64 pending_retire_flush_max_ticks;
    uint64 pending_retire_transfer_max_ticks;
    uint64 pending_retire_other_max_ticks;
    uint64 pending_shape_make_room_max_wait_ticks;
    uint64 pending_shape_make_room_depth_max;
    uint64 pending_shape_make_room_count_max;
    uint64 pending_shape_make_room_wait_count_max;
    uint64 pending_shape_post_count_max;
    uint64 pending_shape_retire_max_ticks;
    uint64 pending_wait_progress_max_ticks;
    uint64 pending_wait_used_max_ticks;
};

struct virtio_gpu_capset {
    int valid;
    int creatable;
    uint32 id;
    uint32 version;
    uint32 size;
};

/*
 * N1 B3 fix: async ring slot claim/fill/post/reap state machine.
 * All transitions happen under q->lock except the final release to FREE,
 * which is a RELEASE store after the slot body has been wiped (see
 * virtio_gpu_async_submit_free).
 *
 *   FREE -> CLAIMED     virtio_gpu_async_reserve_slot (q->lock); the
 *                       poster owns the slot exclusively and fills its
 *                       fields without further locking.
 *   CLAIMED -> POSTED   virtio_gpu_async_post_locked (q->lock), in the
 *                       same critical section that publishes the
 *                       descriptor chain to the avail ring.  From here
 *                       the DEVICE may DMA into cmd/data/resp.
 *   POSTED -> FREE      reaper only (async_reap_serialize held), after
 *                       the slot's used element arrived — the device is
 *                       provably done with the memory.
 *   POSTED -> ABANDONED abort only (async_reap_serialize held, q->lock).
 *                       The device may still DMA: the slot's buffers and
 *                       descriptor block MUST NOT be freed or recycled.
 *   ABANDONED -> FREE   reaper only, when the used element eventually
 *                       arrives; only then is the memory handed back.
 *
 * Abort never touches FREE or CLAIMED slots (a CLAIMED slot is owned by
 * an op_lock'd poster that may be mid-fill/mid-post; freeing it under
 * the poster published descriptors over freed memory — the B3 host-DMA
 * corruption).
 */
enum virtio_gpu_async_slot_state {
    VIRTIO_GPU_ASYNC_SLOT_FREE = 0,
    VIRTIO_GPU_ASYNC_SLOT_CLAIMED = 1,
    VIRTIO_GPU_ASYNC_SLOT_POSTED = 2,
    VIRTIO_GPU_ASYNC_SLOT_ABANDONED = 3,
};

struct virtio_gpu_async_submit {
    int state; /* enum virtio_gpu_async_slot_state; must stay first */
    uint32 ctx_id;
    uint64 fence_id;
    uint32 type;
    uint32 expected;
    uint32 data_order;
    uint32 desc_base;
    void *cmd;
    uint32 cmd_len;
    void *data;
    uint32 data_len;
    void *resp;
    uint32 resp_len;
    uint64 posted_ticks;
    struct virtio_gpu_submit_trace_key trace_key;
    struct virtio_gpu_submit_trace_shape trace_shape;
    completion_t done;
};

/*
 * Async submission ring.  The control queue keeps up to VIRTIO_GPU_ASYNC_DEPTH
 * commands in flight at once so the compositor's GL-compose submit can overlap
 * the previous frame's scanout RESOURCE_FLUSH (which blocks on host vsync).
 * Each ring slot owns a private block of descriptors so an in-flight command's
 * descriptors are never overwritten while a younger one is posted.  QEMU's
 * virtio-gpu control queue retires buffers in submission order, so the ring is
 * drained strictly FIFO.
 */
/*
 * N1/M7: 60 slots is the descriptor-table budget ceiling — the control
 * queue has NUM=256 descriptors and each slot owns
 * VIRTIO_GPU_ASYNC_DESC_PER_SLOT=4 starting at DESC_BASE=8
 * (8 + 60*4 = 248 <= 256).  The deeper ring absorbs host (WSL D3D12)
 * retire jitter that previously filled the 32-deep ring and stalled GL
 * submits mid-frame.
 *
 * Descriptor budget (unchanged by the B2 sync-record fix): descriptors
 * [0, 8) are reserved for the SYNC path — a synchronous command chain
 * uses at most 3 of them (cmd/data/resp at head id 0) and op_lock keeps
 * at most one sync command in flight, so sync commands do NOT consume
 * ring slots; descriptors [8, 248) belong to the 60 async slots; [248,
 * 256) are spare.  The reaper is total: used id 0 maps to the queue's
 * sync record, ids 8 + i*4 map to slot i, anything else is stale and is
 * consumed with a warning.
 */
#define VIRTIO_GPU_ASYNC_MAX_DEPTH 60
#define VIRTIO_GPU_ASYNC_DESC_PER_SLOT 4
#define VIRTIO_GPU_ASYNC_DESC_BASE 8

struct virtio_gpu {
    int initialized;
    struct virtio_pci_state pci;
    volatile struct virtio_gpu_config *config;
    struct virtio_gpu_queue ctrlq;
    spinlock_t lock;
    mutex_t op_lock;
    /*
     * N1: serializes async PROGRESS WAITERS (virtio_gpu_async_wait_progress)
     * only — not ops.  g->async_wait is a single shared completion and
     * re-initializing a completion that still has a sleeping waiter
     * corrupts its wait queue (PANIC tq_remove: queue is empty).  With
     * the unlocked-wait submit path, multiple threads can wait for
     * used-ring progress concurrently; they all want the same event, and
     * a serialized second waiter snapshots used-ring MOVEMENT first, so
     * it wakes as soon as anything retires even when another consumer
     * eats the used elements.  g->async_wait is completion_init'ed once
     * at driver init; wait_progress only completion_reinit()s it, under
     * q->lock, which serializes against every complete_all() site (all
     * of which also hold q->lock).
     */
    mutex_t async_wait_serialize;
    /*
     * N1: single-consumer reap discipline.  Slot retire (find/validate/
     * free) and abort teardown historically relied on op_lock
     * exclusivity; the unlocked-wait submit path reaps without op_lock,
     * so concurrent reapers/aborters must serialize here.  async_count
     * and slot state transitions are additionally q->lock-atomic so
     * posters (reserve) and reapers never lose counter updates.
     * Lock order: op_lock -> async_reap_serialize -> q->lock -> g->lock,
     * and op_lock -> async_wait_serialize -> q->lock.  q->lock ->
     * completion.lock (complete_all / completion_reinit under q->lock)
     * is the only nesting below the spinlocks; nothing sleeps under a
     * spinlock.
     */
    mutex_t async_reap_serialize;
    uint32 next_resource_id;
    struct virtio_gpu_resource resources[VIRTIO_GPU_MAX_RESOURCES];
    struct virtio_gpu_resource *scanout_resource;
    struct virtio_gpu_stats stats;
    uint32 scanout_width;
    uint32 scanout_height;
    uint32 num_capsets;
    uint32 features0;
    uint32 features1;
    uint32 driver_features0;
    uint32 driver_features1;
    uint32 edid_width;
    uint32 edid_height;
    uint32 edid_refresh_millihz;
    uint32 virgl_capset_id;
    uint32 virgl_capset_version;
    uint32 virgl_capset_size;
    struct virtio_gpu_capset capsets[VIRTIO_GPU_MAX_CAPSETS];
    uint32 present_ctx_id;
    uint32 present_scanout_ctx_id;
    uint32 present_scanout_resource_id;
    struct virtio_gpu_resource *present_flip_resource[2];
    uint8 present_flip_valid[2];
    uint32 present_flip_base_generation[2];
    uint32 present_flip_rect_x[2];
    uint32 present_flip_rect_y[2];
    uint32 present_flip_rect_w[2];
    uint32 present_flip_rect_h[2];
    uint32 present_flip_index;
    uint32 present_base_generation;
    uint32 bound_scanout_resource_id;
    uint32 page_flip_scanout_set[VIRTIO_GPU_PAGE_FLIP_SCANOUT_SET_MAX];
    uint32 page_flip_scanout_set_count;
    uint32 page_flip_scanout_width;
    uint32 page_flip_scanout_height;
    uint32 diagnostic_present_resource_id;
    uint32 diagnostic_present_ctx_id;
    uint32 diagnostic_present_src_x;
    uint32 diagnostic_present_src_y;
    uint32 diagnostic_present_dst_x;
    uint32 diagnostic_present_dst_y;
    uint32 diagnostic_present_w;
    uint32 diagnostic_present_h;
    uint32 next_context_id;
    struct virtio_gpu_context contexts[VIRTIO_GPU_MAX_CONTEXTS];
    struct virtio_gpu_submit_trace_owner
        submit_trace_owners[VIRTIO_GPU_SUBMIT_TRACE_OWNER_SLOTS];
    struct virtio_gpu_submit_trace_key submit_trace_current;
    struct virtio_gpu_submit_trace_shape submit_trace_current_shape;
    int submit_trace_current_valid;
    uint64 next_fence_id;
    struct virtio_gpu_async_submit async_ring[VIRTIO_GPU_ASYNC_MAX_DEPTH];
    /*
     * async_count counts CLAIMED + POSTED slots; async_abandoned counts
     * ABANDONED slots (device-owned after an abort, unusable until their
     * used elements arrive).  Both are written only under q->lock;
     * lock-free readers use __atomic loads and treat the values as
     * hints (exactness is enforced at reserve time under q->lock).
     * async_retire_seq is a monotonic progress counter bumped under
     * q->lock whenever a used element is consumed or a slot leaves the
     * POSTED/ABANDONED population — progress waiters snapshot-compare
     * it (plus the device-written used->idx) instead of inferring
     * progress from used-ring occupancy, which a concurrent consumer
     * erases.
     */
    uint32 async_count;
    uint32 async_abandoned;
    uint64 async_retire_seq;
    /*
     * Usable async ring slots: min(VIRTIO_GPU_ASYNC_MAX_DEPTH,
     * (negotiated queue size - DESC_BASE) / DESC_PER_SLOT).  Written
     * once at ctrl-queue init (single-threaded, before any async
     * traffic); 0 disables the async ring entirely so no async post can
     * ever reference a descriptor beyond the negotiated table.
     */
    int async_capacity;
    int async_depth;
    int op_lock_holder;
    completion_t async_wait;
    void *cmd_page;
    void *resp_page;
    void *data_page;
    /* Hardware cursor plane (dedicated virtio-gpu cursor queue). */
    struct virtio_gpu_queue cursorq;
    int cursor_ready;
    uint32 cursor_resource_id;
    struct virtio_gpu_resource *cursor_resource;
    struct virtio_gpu_resource *cursor_resources[VIRTIO_GPU_CURSOR_IMAGE_SLOTS];
    uint32 cursor_resource_next;
    void *cursor_cmd_page;
    uint8 cursor_cmd_inflight[VIRTIO_GPU_CURSOR_RING];
    uint16 cursor_cmd_idx;
    uint16 cursor_ring;
    int cursor_x;
    int cursor_y;
    int cursor_visible;
    uint32 cursor_hot_x;
    uint32 cursor_hot_y;
    /*
     * SLICE 3: async cursor-plane image upload (gate virtio_gpu_async_cursor,
     * default OFF).  virtio_gpu_user_set_cursor()'s image upload runs a
     * synchronous fenced TRANSFER_TO_HOST_2D on the control queue (17-36ms
     * observed inside DRM_IOCTL_MODE_CURSOR).  When the gate is on, the ioctl
     * copies the latest cursor image into cursor_async_pending_* and returns;
     * a single-threaded workqueue drains it off the caller's thread.
     *
     * cursor_async_lock is a LEAF lock: taken only in process/worker context
     * (never IRQ), always released before any op_lock/g->lock is acquired, so
     * it cannot invert the op_lock -> ... -> g->lock order used by presents.
     */
    spinlock_t cursor_async_lock;
    uint8 cursor_async_lock_ready;
    uint8 cursor_async_pending_valid; /* an unsubmitted image is waiting */
    uint8 cursor_async_inflight;      /* worker is mid-upload */
    uint32 cursor_async_pending_w;
    uint32 cursor_async_pending_h;
    uint32 cursor_async_pending_hot_x;
    uint32 cursor_async_pending_hot_y;
    /* Latest-wins double buffer: pending = newest from the ioctl, work =
     * private snapshot the worker uploads from (so the slow fenced transfer
     * never holds cursor_async_lock and the host never reads a buffer the
     * ioctl is concurrently overwriting). */
    uint32 cursor_async_pending_pixels[VIRTIO_GPU_CURSOR_DIM *
                                       VIRTIO_GPU_CURSOR_DIM];
    uint32 cursor_async_work_pixels[VIRTIO_GPU_CURSOR_DIM *
                                    VIRTIO_GPU_CURSOR_DIM];
    uint64 cursor_async_submits_total;
    uint64 cursor_async_coalesced_total;
    uint64 cursor_async_errors_total;
    int host_visible_cap_present;
    int host_visible_blob_ok;
    int host_visible_mapped_once;
    uint64 host_visible_pa;
    uint64 host_visible_va;
    uint64 host_visible_length;
    uint64 host_visible_next_offset;
};

static struct virtio_gpu gpu;

static int virtio_gpu_owner_matches(uint64 object_owner_id,
                                    pid_t object_owner_tgid,
                                    uint64 owner_id, pid_t owner_tgid)
{
    if (owner_id != 0)
        return object_owner_id == owner_id;
    if (owner_tgid > 0 && object_owner_id != 0)
        return object_owner_tgid == owner_tgid;
    if (owner_tgid > 0)
        return object_owner_tgid == owner_tgid;
    return 1;
}

static int virtio_gpu_resource_context_attached_locked(
    const struct virtio_gpu_resource *res, uint32 ctx_id)
{
    if (res == NULL || ctx_id == 0)
        return 0;

    for (uint32 i = 0; i < res->ctx_attach_count; i++) {
        if (res->ctx_attached[i] == ctx_id)
            return 1;
    }
    return 0;
}

static uint32 virtio_gpu_resource_primary_context_locked(
    const struct virtio_gpu_resource *res)
{
    if (res == NULL)
        return 0;
    if (virtio_gpu_resource_context_attached_locked(res, res->ctx_id))
        return res->ctx_id;
    return res->ctx_attach_count != 0 ? res->ctx_attached[0] : 0;
}

static int virtio_gpu_resource_record_context_locked(
    struct virtio_gpu_resource *res, uint32 ctx_id)
{
    if (res == NULL || ctx_id == 0)
        return -EINVAL;
    if (virtio_gpu_resource_context_attached_locked(res, ctx_id)) {
        res->ctx_id = ctx_id;
        return 0;
    }
    if (res->ctx_attach_count >= VIRTIO_GPU_RESOURCE_MAX_CONTEXT_ATTACHMENTS)
        return -ENOSPC;
    res->ctx_attached[res->ctx_attach_count++] = ctx_id;
    res->ctx_id = ctx_id;
    return 0;
}

static void virtio_gpu_resource_forget_context_locked(
    struct virtio_gpu_resource *res, uint32 ctx_id)
{
    if (res == NULL || ctx_id == 0)
        return;

    for (uint32 i = 0; i < res->ctx_attach_count; i++) {
        if (res->ctx_attached[i] != ctx_id)
            continue;
        res->ctx_attach_count--;
        if (i != res->ctx_attach_count)
            res->ctx_attached[i] = res->ctx_attached[res->ctx_attach_count];
        res->ctx_attached[res->ctx_attach_count] = 0;
        break;
    }
    if (res->ctx_id == ctx_id)
        res->ctx_id = virtio_gpu_resource_primary_context_locked(res);
}

static uint32 virtio_gpu_resource_copy_contexts_locked(
    const struct virtio_gpu_resource *res, uint32 *ctx_ids, uint32 max)
{
    uint32 count = 0;

    if (res == NULL || ctx_ids == NULL)
        return 0;
    while (count < res->ctx_attach_count && count < max) {
        ctx_ids[count] = res->ctx_attached[count];
        count++;
    }
    return count;
}

static uint64 virtio_gpu_bar_base(struct virtio_pci_discovery *vd, uint8 bar)
{
    uint64 base = (uint64)(vd->bar[bar] & ~0xFU);
    if ((vd->bar[bar] & 0x6) == 0x4 && bar < 5)
        base |= ((uint64)vd->bar[bar + 1]) << 32;
    return base;
}

static uint64 virtio_gpu_map_mmio_window(uint64 bar, uint32 offset,
                                         uint32 length)
{
    if (bar & 0x1)
        panic("virtio_gpu_pci: capability uses I/O BAR 0x%lx", bar);

    uint64 target = bar + offset;
    uint64 start = PGROUNDDOWN(target);
    uint64 end = PGROUNDUP(target + (length ? length : 1));
    uint64 size = end - start;
    uint64 map_base;

    vm_wlock(kernel_vm);
    map_base = vm_find_free_range(kernel_vm, size, 0);
    if (map_base == 0) {
        vm_wunlock(kernel_vm);
        panic("virtio_gpu_pci: failed to allocate MMIO VA window");
    }

    vma_t *vma = vma_alloc(kernel_vm, map_base, size,
                           PROT_READ | PROT_WRITE | VMA_FLAG_KERNEL);
    vm_wunlock(kernel_vm);
    if (vma == NULL)
        panic("virtio_gpu_pci: failed to reserve MMIO VA window");

    for (uint64 page_off = 0; page_off < size; page_off += PGSIZE) {
        uint64 va = map_base + page_off;
        uint64 pa = start + page_off;

        if (arch_vm_map(kernel_pagetable, va, PGSIZE, pa,
                        PTE_R | PTE_W) != 0)
            panic("virtio_gpu_pci: failed to map MMIO page pa=0x%lx", pa);
    }

    arch_tlb_flush();
    return map_base + (target - start);
}

static uint64 virtio_gpu_align_up(uint64 value, uint64 alignment)
{
    if (alignment == 0)
        return value;
    return (value + alignment - 1) & ~(alignment - 1);
}

static int virtio_gpu_host_visible_configured(struct virtio_gpu *g)
{
    return g != NULL && g->host_visible_cap_present &&
        g->host_visible_pa != 0 && g->host_visible_length != 0;
}

static int virtio_gpu_host_visible_operational(struct virtio_gpu *g)
{
    return virtio_gpu_host_visible_configured(g) &&
        g->virgl_capset_id != 0 &&
        (g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)) &&
        (g->driver_features0 & (1u << VIRTIO_GPU_F_RESOURCE_BLOB));
}

static int virtio_gpu_host_visible_ready(struct virtio_gpu *g)
{
    return virtio_gpu_host_visible_operational(g) &&
        g->host_visible_blob_ok;
}

static void virtio_gpu_notify(struct virtio_gpu *g, uint16 queue)
{
    volatile uint16 *notify_addr = (volatile uint16 *)
        ((uint8 *)g->pci.notify_base +
         g->ctrlq.notify_off * g->pci.notify_off_multiplier);
    *notify_addr = queue;
}

static void virtio_gpu_count_failure(struct virtio_gpu *g)
{
    spin_lock(&g->lock);
    g->stats.failures++;
    spin_unlock(&g->lock);
}

static void virtio_gpu_count_timeout(struct virtio_gpu *g)
{
    spin_lock(&g->lock);
    g->stats.timeouts++;
    spin_unlock(&g->lock);
}

static void virtio_gpu_count_poll_fallback(struct virtio_gpu *g)
{
    spin_lock(&g->lock);
    g->stats.poll_fallbacks++;
    spin_unlock(&g->lock);
}

static void virtio_gpu_count_irq_completion(struct virtio_gpu *g)
{
    spin_lock(&g->lock);
    g->stats.irq_completions++;
    spin_unlock(&g->lock);
}

static void virtio_gpu_count_command(struct virtio_gpu *g, uint32 type)
{
    spin_lock(&g->lock);
    g->stats.commands++;
    if (type == VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D)
        g->stats.transfers++;
    else if (type == VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D ||
             type == VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D)
        g->stats.transfers++;
    else if (type == VIRTIO_GPU_CMD_RESOURCE_FLUSH)
        g->stats.flushes++;
    else if (type == VIRTIO_GPU_CMD_SET_SCANOUT)
        g->stats.scanouts++;
    else if (type == VIRTIO_GPU_CMD_GET_CAPSET_INFO)
        g->stats.capsets++;
    else if (type == VIRTIO_GPU_CMD_CTX_CREATE ||
             type == VIRTIO_GPU_CMD_CTX_DESTROY)
        g->stats.contexts++;
    else if (type == VIRTIO_GPU_CMD_SUBMIT_3D)
        g->stats.submits++;
    spin_unlock(&g->lock);
}

static int virtio_gpu_cmdline_enabled(const char *key)
{
    char buf[16];

    if (cmdline_get_param(key, buf, sizeof(buf)) != 0)
        return 0;
    return strcmp(buf, "1") == 0 ||
           strcmp(buf, "yes") == 0 ||
           strcmp(buf, "true") == 0 ||
           strcmp(buf, "on") == 0;
}

static int virtio_gpu_cmdline_present(const char *key)
{
    char buf[16];

    return cmdline_get_param(key, buf, sizeof(buf)) == 0;
}

static uint64 virtio_gpu_ticks_to_us(uint64 ticks);
static struct virtio_gpu_context *virtio_gpu_lookup_context_locked(
    struct virtio_gpu *g, uint32 id);

static int virtio_gpu_submit_trace_enabled(void)
{
    static int cached = -1;

    if (cached < 0)
        cached = virtio_gpu_cmdline_enabled("virtio_gpu_submit_trace");
    return cached;
}

static int virtio_gpu_submit_hot_shape_trace_enabled(void)
{
    static int cached = -1;

    if (cached < 0)
        cached = virtio_gpu_cmdline_enabled(
            "virtio_gpu_submit_hot_shape_trace");
    return cached;
}

static int virtio_gpu_submit_hot_shape_stats_enabled(void)
{
    static int cached = -1;

    if (cached < 0)
        cached = virtio_gpu_cmdline_enabled(
            "virtio_gpu_submit_hot_shape_stats");
    return cached;
}

static int virtio_gpu_submit_trace_collect_enabled(void)
{
    return virtio_gpu_submit_trace_enabled() ||
        virtio_gpu_submit_hot_shape_trace_enabled() ||
        virtio_gpu_submit_hot_shape_stats_enabled();
}

static int virtio_gpu_submit_trace_shape_valid(
    const struct virtio_gpu_submit_trace_shape *shape)
{
    return shape != NULL && shape->valid != 0;
}

static int virtio_gpu_submit_trace_shape_matches(
    const struct virtio_gpu_submit_trace_shape *a,
    const struct virtio_gpu_submit_trace_shape *b)
{
    return a->drm_flags == b->drm_flags &&
        a->nr_dwords == b->nr_dwords &&
        a->resource_count == b->resource_count &&
        a->first_word == b->first_word;
}

static int virtio_gpu_submit_trace_shape_is_glx_hot(
    const struct virtio_gpu_submit_trace_shape *shape)
{
    if (!virtio_gpu_submit_trace_shape_valid(shape))
        return 0;
    /*
     * Userspace sees the exported out-fence fd after ioctl return; the kernel
     * trace shape is captured before export, so key on the OUT request bit.
     */
    return shape->drm_flags == 0x2 &&
        shape->nr_dwords == 1026 &&
        shape->resource_count == 0;
}

static int virtio_gpu_submit_trace_full_or_hot_shape(
    const struct virtio_gpu_submit_trace_shape *shape)
{
    return virtio_gpu_submit_trace_enabled() ||
        ((virtio_gpu_submit_hot_shape_trace_enabled() ||
          virtio_gpu_submit_hot_shape_stats_enabled()) &&
         virtio_gpu_submit_trace_shape_is_glx_hot(shape));
}

static int virtio_gpu_submit_trace_note_shape_locked(
    struct virtio_gpu_submit_trace_owner *owner,
    const struct virtio_gpu_submit_trace_shape *shape)
{
    if (owner == NULL || !virtio_gpu_submit_trace_shape_is_glx_hot(shape))
        return 0;
    if (!owner->total.shape.valid) {
        owner->total.shape = *shape;
    } else if (!virtio_gpu_submit_trace_shape_matches(&owner->total.shape,
                                                       shape)) {
        owner->total.shape_mixed++;
        owner->total.shape = *shape;
    }
    return 1;
}

static void virtio_gpu_submit_trace_copy_string(char *dst, uint32 len,
                                                const char *src)
{
    if (len == 0)
        return;
    if (src != NULL && src[0] != '\0') {
        uint32 copy = (uint32)strlen(src);

        if (copy >= len)
            copy = len - 1;
        memmove(dst, src, copy);
        dst[copy] = '\0';
    } else {
        dst[0] = '-';
        if (len > 1)
            dst[1] = '\0';
    }
}

static void virtio_gpu_submit_trace_copy_proc(
    char dst[VIRTIO_GPU_SUBMIT_TRACE_PROC_LEN])
{
    if (current != NULL && current->name[0] != '\0') {
        memmove(dst, current->name,
                VIRTIO_GPU_SUBMIT_TRACE_PROC_LEN);
        dst[VIRTIO_GPU_SUBMIT_TRACE_PROC_LEN - 1] = '\0';
    } else {
        virtio_gpu_submit_trace_copy_string(
            dst, VIRTIO_GPU_SUBMIT_TRACE_PROC_LEN, NULL);
    }
}

static void virtio_gpu_submit_trace_copy_debug(
    char dst[VIRTIO_GPU_SUBMIT_TRACE_DEBUG_LEN], const char *src)
{
    virtio_gpu_submit_trace_copy_string(
        dst, VIRTIO_GPU_SUBMIT_TRACE_DEBUG_LEN, src);
}

static void virtio_gpu_submit_trace_key_for_ctx_locked(
    struct virtio_gpu *g, uint64 owner_id, pid_t owner_tgid, uint32 ctx_id,
    const char proc[VIRTIO_GPU_SUBMIT_TRACE_PROC_LEN],
    struct virtio_gpu_submit_trace_key *key)
{
    struct virtio_gpu_context *ctx = NULL;

    memset(key, 0, sizeof(*key));
    key->owner_id = owner_id;
    key->owner_tgid = owner_tgid;
    key->ctx_id = ctx_id;
    virtio_gpu_submit_trace_copy_string(
        key->proc, VIRTIO_GPU_SUBMIT_TRACE_PROC_LEN, proc);
    if (ctx_id != 0)
        ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
    if (ctx != NULL) {
        key->owner_id = ctx->owner_id;
        key->owner_tgid = ctx->owner_tgid;
        virtio_gpu_submit_trace_copy_debug(key->debug_name,
                                           ctx->debug_name);
    } else {
        virtio_gpu_submit_trace_copy_debug(key->debug_name, NULL);
    }
}

static int virtio_gpu_submit_trace_key_matches(
    const struct virtio_gpu_submit_trace_key *a,
    const struct virtio_gpu_submit_trace_key *b)
{
    return a->owner_id == b->owner_id &&
        a->owner_tgid == b->owner_tgid &&
        a->ctx_id == b->ctx_id &&
        strncmp(a->proc, b->proc, VIRTIO_GPU_SUBMIT_TRACE_PROC_LEN) == 0;
}

static struct virtio_gpu_submit_trace_owner *
virtio_gpu_submit_trace_owner_locked(
    struct virtio_gpu *g, const struct virtio_gpu_submit_trace_key *key)
{
    struct virtio_gpu_submit_trace_owner *free_slot = NULL;

    for (int i = 0; i < VIRTIO_GPU_SUBMIT_TRACE_OWNER_SLOTS; i++) {
        struct virtio_gpu_submit_trace_owner *o = &g->submit_trace_owners[i];

        if (!o->in_use) {
            if (free_slot == NULL)
                free_slot = o;
            continue;
        }
        if (virtio_gpu_submit_trace_key_matches(&o->key, key)) {
            virtio_gpu_submit_trace_copy_debug(o->key.debug_name,
                                               key->debug_name);
            return o;
        }
    }

    if (free_slot == NULL) {
        g->stats.submit_trace_owner_drops++;
        return NULL;
    }

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->in_use = 1;
    free_slot->key = *key;
    return free_slot;
}

static struct virtio_gpu_submit_trace_owner *
virtio_gpu_submit_trace_owner_for_ctx_locked(
    struct virtio_gpu *g, uint64 owner_id, pid_t owner_tgid, uint32 ctx_id,
    const char proc[VIRTIO_GPU_SUBMIT_TRACE_PROC_LEN])
{
    struct virtio_gpu_submit_trace_key key;

    virtio_gpu_submit_trace_key_for_ctx_locked(g, owner_id, owner_tgid,
                                               ctx_id, proc, &key);
    return virtio_gpu_submit_trace_owner_locked(g, &key);
}

static void virtio_gpu_submit_trace_set_current(
    struct virtio_gpu *g, uint64 owner_id, pid_t owner_tgid, uint32 ctx_id,
    const struct virtio_gpu_submit_trace_shape *shape)
{
    char proc[VIRTIO_GPU_SUBMIT_TRACE_PROC_LEN];

    if (!virtio_gpu_submit_trace_collect_enabled())
        return;

    virtio_gpu_submit_trace_copy_proc(proc);
    spin_lock(&g->lock);
    virtio_gpu_submit_trace_key_for_ctx_locked(g, owner_id, owner_tgid,
                                               ctx_id, proc,
                                               &g->submit_trace_current);
    if (virtio_gpu_submit_trace_shape_valid(shape))
        g->submit_trace_current_shape = *shape;
    else
        memset(&g->submit_trace_current_shape, 0,
               sizeof(g->submit_trace_current_shape));
    g->submit_trace_current_valid = 1;
    spin_unlock(&g->lock);
}

static void virtio_gpu_submit_trace_clear_current(struct virtio_gpu *g)
{
    if (!virtio_gpu_submit_trace_collect_enabled())
        return;

    spin_lock(&g->lock);
    g->submit_trace_current_valid = 0;
    memset(&g->submit_trace_current_shape, 0,
           sizeof(g->submit_trace_current_shape));
    spin_unlock(&g->lock);
}

static void virtio_gpu_submit_trace_snapshot_slot(
    struct virtio_gpu *g, struct virtio_gpu_async_submit *a, uint32 ctx_id)
{
    char proc[VIRTIO_GPU_SUBMIT_TRACE_PROC_LEN];

    if (!virtio_gpu_submit_trace_collect_enabled() || a == NULL)
        return;

    virtio_gpu_submit_trace_copy_proc(proc);
    spin_lock(&g->lock);
    virtio_gpu_submit_trace_key_for_ctx_locked(g, 0, 0, ctx_id, proc,
                                               &a->trace_key);
    a->trace_shape = g->submit_trace_current_shape;
    spin_unlock(&g->lock);
}

static void virtio_gpu_submit_trace_record_submit(
    struct virtio_gpu *g, uint64 owner_id, pid_t owner_tgid, uint32 ctx_id,
    uint64 total_ticks, uint64 lock_wait_ticks,
    uint64 resource_attach_count, uint64 resource_attach_ticks,
    uint64 async_prepare_ticks, uint64 post_ticks, int first_submit,
    int failed, int ret, const struct virtio_gpu_submit_trace_shape *shape)
{
    char proc[VIRTIO_GPU_SUBMIT_TRACE_PROC_LEN];
    struct virtio_gpu_submit_trace_owner *owner;

    virtio_gpu_submit_trace_copy_proc(proc);
    spin_lock(&g->lock);
    g->stats.submit_trace_submit_calls++;
    g->stats.submit_trace_submit_ticks += total_ticks;
    g->stats.submit_trace_lock_wait_ticks += lock_wait_ticks;
    g->stats.submit_trace_resource_attach_count += resource_attach_count;
    g->stats.submit_trace_resource_attach_ticks += resource_attach_ticks;
    g->stats.submit_trace_async_prepare_ticks += async_prepare_ticks;
    g->stats.submit_trace_post_ticks += post_ticks;
    if (first_submit)
        g->stats.submit_trace_first_submits++;
    if (failed)
        g->stats.submit_trace_failures++;
    if (!virtio_gpu_submit_trace_full_or_hot_shape(shape)) {
        spin_unlock(&g->lock);
        return;
    }
    owner = virtio_gpu_submit_trace_owner_for_ctx_locked(
        g, owner_id, owner_tgid, ctx_id, proc);
    if (owner != NULL) {
        owner->total.submit_calls++;
        owner->total.submit_ticks += total_ticks;
        owner->total.lock_wait_ticks += lock_wait_ticks;
        owner->total.attach_count += resource_attach_count;
        owner->total.attach_ticks += resource_attach_ticks;
        owner->total.async_prepare_ticks += async_prepare_ticks;
        owner->total.post_ticks += post_ticks;
        if (virtio_gpu_submit_trace_note_shape_locked(owner, shape)) {
            owner->total.shape_submit_calls++;
            if (failed)
                owner->total.shape_failures++;
        }
        if (first_submit)
            owner->total.first_submit++;
        if (failed)
            owner->total.failures++;
        owner->total.last_ret = ret;
    }
    spin_unlock(&g->lock);
}

static void virtio_gpu_submit_trace_record_fence(
    struct virtio_gpu *g, uint64 total_ticks, uint64 drain_calls,
    uint64 drain_ticks, int failed)
{
    spin_lock(&g->lock);
    g->stats.submit_trace_fence_calls++;
    g->stats.submit_trace_fence_ticks += total_ticks;
    g->stats.submit_trace_fence_drain_calls += drain_calls;
    g->stats.submit_trace_fence_drain_ticks += drain_ticks;
    if (failed)
        g->stats.submit_trace_fence_failures++;
    spin_unlock(&g->lock);
}

static void virtio_gpu_submit_trace_record_wait_for_used(
    struct virtio_gpu *g, uint64 ticks)
{
    uint64 us = virtio_gpu_ticks_to_us(ticks);
    struct virtio_gpu_submit_trace_owner *owner = NULL;

    spin_lock(&g->lock);
    g->stats.submit_trace_wait_for_used_calls++;
    g->stats.submit_trace_wait_for_used_ticks += ticks;
    if (us > g->stats.submit_trace_wait_for_used_max_us)
        g->stats.submit_trace_wait_for_used_max_us = us;
    if (g->submit_trace_current_valid &&
        virtio_gpu_submit_trace_full_or_hot_shape(
            &g->submit_trace_current_shape))
        owner = virtio_gpu_submit_trace_owner_locked(
            g, &g->submit_trace_current);
    if (owner != NULL) {
        owner->total.wait_used_calls++;
        owner->total.wait_used_ticks += ticks;
        if (ticks > owner->total.wait_used_max_ticks)
            owner->total.wait_used_max_ticks = ticks;
        if (ticks > owner->pending_wait_used_max_ticks)
            owner->pending_wait_used_max_ticks = ticks;
    }
    spin_unlock(&g->lock);
}

static void virtio_gpu_submit_trace_record_async_wait_progress(
    struct virtio_gpu *g, uint64 ticks)
{
    struct virtio_gpu_submit_trace_owner *owner = NULL;

    spin_lock(&g->lock);
    g->stats.submit_trace_async_wait_progress_calls++;
    g->stats.submit_trace_async_wait_progress_ticks += ticks;
    if (g->submit_trace_current_valid &&
        virtio_gpu_submit_trace_full_or_hot_shape(
            &g->submit_trace_current_shape))
        owner = virtio_gpu_submit_trace_owner_locked(
            g, &g->submit_trace_current);
    if (owner != NULL) {
        owner->total.wait_progress_calls++;
        owner->total.wait_progress_ticks += ticks;
        if (ticks > owner->total.wait_progress_max_ticks)
            owner->total.wait_progress_max_ticks = ticks;
        if (ticks > owner->pending_wait_progress_max_ticks)
            owner->pending_wait_progress_max_ticks = ticks;
    }
    spin_unlock(&g->lock);
}

static void virtio_gpu_submit_trace_record_async_make_room_current(
    struct virtio_gpu *g, uint64 ticks, uint64 wait_ticks, int stalled,
    uint64 depth, uint64 count_max, uint64 wait_count_max)
{
    struct virtio_gpu_submit_trace_owner *owner = NULL;

    spin_lock(&g->lock);
    g->stats.submit_trace_async_make_room_ticks += ticks;
    if (depth > g->stats.submit_trace_async_make_room_depth_max)
        g->stats.submit_trace_async_make_room_depth_max = depth;
    if (count_max > g->stats.submit_trace_async_make_room_count_max)
        g->stats.submit_trace_async_make_room_count_max = count_max;
    if (wait_count_max >
        g->stats.submit_trace_async_make_room_wait_count_max)
        g->stats.submit_trace_async_make_room_wait_count_max =
            wait_count_max;
    if (g->submit_trace_current_valid &&
        virtio_gpu_submit_trace_full_or_hot_shape(
            &g->submit_trace_current_shape))
        owner = virtio_gpu_submit_trace_owner_locked(
            g, &g->submit_trace_current);
    if (owner != NULL) {
        owner->total.make_room_calls++;
        owner->total.make_room_ticks += ticks;
        owner->total.make_room_wait_ticks += wait_ticks;
        if (wait_ticks > owner->total.make_room_max_wait_ticks)
            owner->total.make_room_max_wait_ticks = wait_ticks;
        if (wait_ticks > owner->pending_make_room_max_wait_ticks)
            owner->pending_make_room_max_wait_ticks = wait_ticks;
        if (depth > owner->total.make_room_depth_max)
            owner->total.make_room_depth_max = depth;
        if (depth > owner->pending_make_room_depth_max)
            owner->pending_make_room_depth_max = depth;
        if (count_max > owner->total.make_room_count_max)
            owner->total.make_room_count_max = count_max;
        if (count_max > owner->pending_make_room_count_max)
            owner->pending_make_room_count_max = count_max;
        if (wait_count_max > owner->total.make_room_wait_count_max)
            owner->total.make_room_wait_count_max = wait_count_max;
        if (wait_count_max > owner->pending_make_room_wait_count_max)
            owner->pending_make_room_wait_count_max = wait_count_max;
        if (stalled)
            owner->total.make_room_stalls++;
        if (virtio_gpu_submit_trace_note_shape_locked(
                owner, &g->submit_trace_current_shape)) {
            owner->total.shape_make_room_calls++;
            owner->total.shape_make_room_ticks += ticks;
            owner->total.shape_make_room_wait_ticks += wait_ticks;
            if (wait_ticks > owner->total.shape_make_room_max_wait_ticks)
                owner->total.shape_make_room_max_wait_ticks = wait_ticks;
            if (wait_ticks > owner->pending_shape_make_room_max_wait_ticks)
                owner->pending_shape_make_room_max_wait_ticks = wait_ticks;
            if (depth > owner->total.shape_make_room_depth_max)
                owner->total.shape_make_room_depth_max = depth;
            if (depth > owner->pending_shape_make_room_depth_max)
                owner->pending_shape_make_room_depth_max = depth;
            if (count_max > owner->total.shape_make_room_count_max)
                owner->total.shape_make_room_count_max = count_max;
            if (count_max > owner->pending_shape_make_room_count_max)
                owner->pending_shape_make_room_count_max = count_max;
            if (wait_count_max > owner->total.shape_make_room_wait_count_max)
                owner->total.shape_make_room_wait_count_max = wait_count_max;
            if (wait_count_max > owner->pending_shape_make_room_wait_count_max)
                owner->pending_shape_make_room_wait_count_max = wait_count_max;
            if (stalled)
                owner->total.shape_make_room_stalls++;
        }
    }
    spin_unlock(&g->lock);
}

static void virtio_gpu_submit_trace_record_async_post(
    struct virtio_gpu *g, const struct virtio_gpu_async_submit *a,
    uint64 async_count)
{
    struct virtio_gpu_submit_trace_owner *owner;

    if (!virtio_gpu_submit_trace_collect_enabled() || a == NULL)
        return;

    spin_lock(&g->lock);
    if (async_count > g->stats.submit_trace_async_post_count_max)
        g->stats.submit_trace_async_post_count_max = async_count;
    if (!virtio_gpu_submit_trace_full_or_hot_shape(&a->trace_shape)) {
        spin_unlock(&g->lock);
        return;
    }
    owner = virtio_gpu_submit_trace_owner_locked(g, &a->trace_key);
    if (owner != NULL) {
        owner->total.posted++;
        if (async_count > owner->total.post_count_max)
            owner->total.post_count_max = async_count;
        if (async_count > owner->pending_post_count_max)
            owner->pending_post_count_max = async_count;
        if (a->fence_id != 0)
            owner->total.last_fence = a->fence_id;
        if (virtio_gpu_submit_trace_note_shape_locked(owner,
                                                      &a->trace_shape)) {
            owner->total.shape_posted++;
            if (async_count > owner->total.shape_post_count_max)
                owner->total.shape_post_count_max = async_count;
            if (async_count > owner->pending_shape_post_count_max)
                owner->pending_shape_post_count_max = async_count;
        }
    }
    spin_unlock(&g->lock);
}

static void virtio_gpu_submit_trace_record_async_retire(
    struct virtio_gpu *g, const struct virtio_gpu_async_submit *a, int ret)
{
    struct virtio_gpu_submit_trace_owner *owner;
    uint64 age_ticks;
    uint64 age_us;

    if (!virtio_gpu_submit_trace_collect_enabled() || a == NULL)
        return;

    age_ticks = a->posted_ticks == 0 ? 0 : r_time() - a->posted_ticks;
    age_us = virtio_gpu_ticks_to_us(age_ticks);

    spin_lock(&g->lock);
    g->stats.submit_trace_async_retire_ticks += age_ticks;
    if (age_us > g->stats.submit_trace_async_retire_max_us)
        g->stats.submit_trace_async_retire_max_us = age_us;
    switch (a->type) {
    case VIRTIO_GPU_CMD_SUBMIT_3D:
        g->stats.submit_trace_async_retire_submit_3d_ticks += age_ticks;
        if (age_us > g->stats.submit_trace_async_retire_submit_3d_max_us)
            g->stats.submit_trace_async_retire_submit_3d_max_us = age_us;
        break;
    case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
        g->stats.submit_trace_async_retire_flush_ticks += age_ticks;
        if (age_us > g->stats.submit_trace_async_retire_flush_max_us)
            g->stats.submit_trace_async_retire_flush_max_us = age_us;
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D:
        g->stats.submit_trace_async_retire_transfer_ticks += age_ticks;
        if (age_us > g->stats.submit_trace_async_retire_transfer_max_us)
            g->stats.submit_trace_async_retire_transfer_max_us = age_us;
        break;
    default:
        break;
    }
    if (!virtio_gpu_submit_trace_full_or_hot_shape(&a->trace_shape)) {
        spin_unlock(&g->lock);
        return;
    }
    owner = virtio_gpu_submit_trace_owner_locked(g, &a->trace_key);
    if (owner != NULL) {
        owner->total.retired++;
        owner->total.retire_ticks += age_ticks;
        if (age_ticks > owner->total.retire_max_ticks)
            owner->total.retire_max_ticks = age_ticks;
        if (age_ticks > owner->pending_retire_max_ticks)
            owner->pending_retire_max_ticks = age_ticks;
        switch (a->type) {
        case VIRTIO_GPU_CMD_SUBMIT_3D:
            owner->total.retired_submit_3d++;
            owner->total.retire_submit_3d_ticks += age_ticks;
            if (age_ticks > owner->total.retire_submit_3d_max_ticks)
                owner->total.retire_submit_3d_max_ticks = age_ticks;
            if (age_ticks > owner->pending_retire_submit_3d_max_ticks)
                owner->pending_retire_submit_3d_max_ticks = age_ticks;
            break;
        case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
            owner->total.retired_flush++;
            owner->total.retire_flush_ticks += age_ticks;
            if (age_ticks > owner->total.retire_flush_max_ticks)
                owner->total.retire_flush_max_ticks = age_ticks;
            if (age_ticks > owner->pending_retire_flush_max_ticks)
                owner->pending_retire_flush_max_ticks = age_ticks;
            break;
        case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D:
            owner->total.retired_transfer++;
            owner->total.retire_transfer_ticks += age_ticks;
            if (age_ticks > owner->total.retire_transfer_max_ticks)
                owner->total.retire_transfer_max_ticks = age_ticks;
            if (age_ticks > owner->pending_retire_transfer_max_ticks)
                owner->pending_retire_transfer_max_ticks = age_ticks;
            break;
        default:
            owner->total.retired_other++;
            owner->total.retire_other_ticks += age_ticks;
            if (age_ticks > owner->total.retire_other_max_ticks)
                owner->total.retire_other_max_ticks = age_ticks;
            if (age_ticks > owner->pending_retire_other_max_ticks)
                owner->pending_retire_other_max_ticks = age_ticks;
            break;
        }
        owner->total.last_ret = ret;
        if (a->fence_id != 0)
            owner->total.last_fence = a->fence_id;
        if (ret != 0)
            owner->total.failures++;
        if (virtio_gpu_submit_trace_note_shape_locked(owner,
                                                      &a->trace_shape)) {
            owner->total.shape_retired++;
            owner->total.shape_retire_ticks += age_ticks;
            if (age_ticks > owner->total.shape_retire_max_ticks)
                owner->total.shape_retire_max_ticks = age_ticks;
            if (age_ticks > owner->pending_shape_retire_max_ticks)
                owner->pending_shape_retire_max_ticks = age_ticks;
            if (ret != 0)
                owner->total.shape_failures++;
        }
    }
    spin_unlock(&g->lock);
}

static int virtio_gpu_submit_trace_owner_counts_changed(
    const struct virtio_gpu_submit_trace_owner_counts *total,
    const struct virtio_gpu_submit_trace_owner_counts *emitted)
{
    return total->submit_calls != emitted->submit_calls ||
        total->make_room_calls != emitted->make_room_calls ||
        total->wait_progress_calls != emitted->wait_progress_calls ||
        total->wait_used_calls != emitted->wait_used_calls ||
        total->posted != emitted->posted ||
        total->retired != emitted->retired ||
        total->shape_submit_calls != emitted->shape_submit_calls ||
        total->shape_make_room_calls != emitted->shape_make_room_calls ||
        total->shape_posted != emitted->shape_posted ||
        total->shape_retired != emitted->shape_retired ||
        total->shape_failures != emitted->shape_failures ||
        total->shape_mixed != emitted->shape_mixed ||
        total->failures != emitted->failures;
}

static int virtio_gpu_submit_trace_owner_changed(
    const struct virtio_gpu_submit_trace_owner *o)
{
    return virtio_gpu_submit_trace_owner_counts_changed(&o->total,
                                                        &o->emitted) ||
        o->pending_make_room_max_wait_ticks != 0 ||
        o->pending_make_room_depth_max != 0 ||
        o->pending_make_room_count_max != 0 ||
        o->pending_make_room_wait_count_max != 0 ||
        o->pending_post_count_max != 0 ||
        o->pending_retire_max_ticks != 0 ||
        o->pending_retire_submit_3d_max_ticks != 0 ||
        o->pending_retire_flush_max_ticks != 0 ||
        o->pending_retire_transfer_max_ticks != 0 ||
        o->pending_retire_other_max_ticks != 0 ||
        o->pending_shape_make_room_max_wait_ticks != 0 ||
        o->pending_shape_make_room_depth_max != 0 ||
        o->pending_shape_make_room_count_max != 0 ||
        o->pending_shape_make_room_wait_count_max != 0 ||
        o->pending_shape_post_count_max != 0 ||
        o->pending_shape_retire_max_ticks != 0 ||
        o->pending_wait_progress_max_ticks != 0 ||
        o->pending_wait_used_max_ticks != 0;
}

static int virtio_gpu_submit_trace_owner_changed_locked(struct virtio_gpu *g)
{
    if (g->stats.submit_trace_owner_drops !=
        g->stats.submit_trace_owner_emitted_drops)
        return 1;
    for (int i = 0; i < VIRTIO_GPU_SUBMIT_TRACE_OWNER_SLOTS; i++) {
        struct virtio_gpu_submit_trace_owner *o = &g->submit_trace_owners[i];

        if (o->in_use && virtio_gpu_submit_trace_owner_changed(o))
            return 1;
    }
    return 0;
}

static void virtio_gpu_submit_trace_emit(struct virtio_gpu *g)
{
    struct virtio_gpu_stats s;
    int print_global = 0;
    int full_trace = virtio_gpu_submit_trace_enabled();
    int hot_shape_trace = virtio_gpu_submit_hot_shape_trace_enabled();

    if (!full_trace && !hot_shape_trace)
        return;

    spin_lock(&g->lock);
    print_global = full_trace &&
        !(g->stats.submit_trace_submit_calls ==
              g->stats.submit_trace_emitted_submit_calls &&
          g->stats.submit_trace_fence_calls ==
              g->stats.submit_trace_emitted_fence_calls &&
          g->stats.submit_trace_wait_for_used_calls ==
              g->stats.submit_trace_emitted_wait_for_used_calls);
    if (!print_global && !virtio_gpu_submit_trace_owner_changed_locked(g)) {
        spin_unlock(&g->lock);
        return;
    }
    s = g->stats;
    if (print_global) {
        g->stats.submit_trace_emitted_submit_calls =
            g->stats.submit_trace_submit_calls;
        g->stats.submit_trace_emitted_fence_calls =
            g->stats.submit_trace_fence_calls;
        g->stats.submit_trace_emitted_wait_for_used_calls =
            g->stats.submit_trace_wait_for_used_calls;
    }
    spin_unlock(&g->lock);

    if (print_global)
        printf("virtio-gpu-submit-trace: submit_calls=%lu submit_us=%lu lock_wait_us=%lu attach_count=%lu attach_us=%lu async_prepare_us=%lu post_us=%lu first_submit=%lu failures=%lu fence_calls=%lu fence_us=%lu fence_drains=%lu fence_drain_us=%lu fence_failures=%lu wait_used_calls=%lu wait_used_us=%lu wait_used_max_us=%lu async_wait_progress_calls=%lu async_wait_progress_us=%lu make_room_calls=%lu make_room_us=%lu make_room_stalls=%lu make_room_wait_us=%lu make_room_max_wait_us=%lu make_room_depth_max=%lu make_room_count_max=%lu make_room_wait_count_max=%lu post_count_max=%lu retire_us=%lu retire_max_us=%lu retire_submit_3d_us=%lu retire_submit_3d_max_us=%lu retire_flush_us=%lu retire_flush_max_us=%lu retire_transfer_us=%lu retire_transfer_max_us=%lu\n",
               s.submit_trace_submit_calls,
               virtio_gpu_ticks_to_us(s.submit_trace_submit_ticks),
               virtio_gpu_ticks_to_us(s.submit_trace_lock_wait_ticks),
               s.submit_trace_resource_attach_count,
               virtio_gpu_ticks_to_us(s.submit_trace_resource_attach_ticks),
               virtio_gpu_ticks_to_us(s.submit_trace_async_prepare_ticks),
               virtio_gpu_ticks_to_us(s.submit_trace_post_ticks),
               s.submit_trace_first_submits, s.submit_trace_failures,
               s.submit_trace_fence_calls,
               virtio_gpu_ticks_to_us(s.submit_trace_fence_ticks),
               s.submit_trace_fence_drain_calls,
               virtio_gpu_ticks_to_us(s.submit_trace_fence_drain_ticks),
               s.submit_trace_fence_failures,
               s.submit_trace_wait_for_used_calls,
               virtio_gpu_ticks_to_us(s.submit_trace_wait_for_used_ticks),
               s.submit_trace_wait_for_used_max_us,
               s.submit_trace_async_wait_progress_calls,
               virtio_gpu_ticks_to_us(
                   s.submit_trace_async_wait_progress_ticks),
               s.async_make_room_calls,
               virtio_gpu_ticks_to_us(s.submit_trace_async_make_room_ticks),
               s.async_make_room_stalls,
               virtio_gpu_ticks_to_us(s.async_make_room_wait_ticks),
               s.async_make_room_max_wait_us,
               s.submit_trace_async_make_room_depth_max,
               s.submit_trace_async_make_room_count_max,
               s.submit_trace_async_make_room_wait_count_max,
               s.submit_trace_async_post_count_max,
               virtio_gpu_ticks_to_us(s.submit_trace_async_retire_ticks),
               s.submit_trace_async_retire_max_us,
               virtio_gpu_ticks_to_us(
                   s.submit_trace_async_retire_submit_3d_ticks),
               s.submit_trace_async_retire_submit_3d_max_us,
               virtio_gpu_ticks_to_us(
                   s.submit_trace_async_retire_flush_ticks),
               s.submit_trace_async_retire_flush_max_us,
               virtio_gpu_ticks_to_us(
                   s.submit_trace_async_retire_transfer_ticks),
               s.submit_trace_async_retire_transfer_max_us);

    for (;;) {
        struct virtio_gpu_submit_trace_key key;
        struct virtio_gpu_submit_trace_owner_counts delta;
        int have = 0;

        spin_lock(&g->lock);
        for (int i = 0; i < VIRTIO_GPU_SUBMIT_TRACE_OWNER_SLOTS; i++) {
            struct virtio_gpu_submit_trace_owner *o =
                &g->submit_trace_owners[i];

            if (!o->in_use || !virtio_gpu_submit_trace_owner_changed(o))
                continue;
            key = o->key;
            memset(&delta, 0, sizeof(delta));
            delta.submit_calls =
                o->total.submit_calls - o->emitted.submit_calls;
            delta.submit_ticks =
                o->total.submit_ticks - o->emitted.submit_ticks;
            delta.lock_wait_ticks =
                o->total.lock_wait_ticks - o->emitted.lock_wait_ticks;
            delta.attach_count =
                o->total.attach_count - o->emitted.attach_count;
            delta.attach_ticks =
                o->total.attach_ticks - o->emitted.attach_ticks;
            delta.async_prepare_ticks =
                o->total.async_prepare_ticks -
                o->emitted.async_prepare_ticks;
            delta.post_ticks = o->total.post_ticks - o->emitted.post_ticks;
            delta.make_room_calls =
                o->total.make_room_calls - o->emitted.make_room_calls;
            delta.make_room_stalls =
                o->total.make_room_stalls - o->emitted.make_room_stalls;
            delta.make_room_ticks =
                o->total.make_room_ticks - o->emitted.make_room_ticks;
            delta.make_room_wait_ticks =
                o->total.make_room_wait_ticks -
                o->emitted.make_room_wait_ticks;
            delta.make_room_max_wait_ticks =
                o->pending_make_room_max_wait_ticks;
            delta.make_room_depth_max =
                o->pending_make_room_depth_max;
            delta.make_room_count_max =
                o->pending_make_room_count_max;
            delta.make_room_wait_count_max =
                o->pending_make_room_wait_count_max;
            delta.wait_progress_calls =
                o->total.wait_progress_calls -
                o->emitted.wait_progress_calls;
            delta.wait_progress_ticks =
                o->total.wait_progress_ticks -
                o->emitted.wait_progress_ticks;
            delta.wait_progress_max_ticks =
                o->pending_wait_progress_max_ticks;
            delta.wait_used_calls =
                o->total.wait_used_calls - o->emitted.wait_used_calls;
            delta.wait_used_ticks =
                o->total.wait_used_ticks - o->emitted.wait_used_ticks;
            delta.wait_used_max_ticks = o->pending_wait_used_max_ticks;
            delta.posted = o->total.posted - o->emitted.posted;
            delta.post_count_max = o->pending_post_count_max;
            delta.retired = o->total.retired - o->emitted.retired;
            delta.retire_ticks =
                o->total.retire_ticks - o->emitted.retire_ticks;
            delta.retire_max_ticks = o->pending_retire_max_ticks;
            delta.retired_submit_3d =
                o->total.retired_submit_3d -
                o->emitted.retired_submit_3d;
            delta.retire_submit_3d_ticks =
                o->total.retire_submit_3d_ticks -
                o->emitted.retire_submit_3d_ticks;
            delta.retire_submit_3d_max_ticks =
                o->pending_retire_submit_3d_max_ticks;
            delta.retired_flush =
                o->total.retired_flush - o->emitted.retired_flush;
            delta.retire_flush_ticks =
                o->total.retire_flush_ticks -
                o->emitted.retire_flush_ticks;
            delta.retire_flush_max_ticks =
                o->pending_retire_flush_max_ticks;
            delta.retired_transfer =
                o->total.retired_transfer -
                o->emitted.retired_transfer;
            delta.retire_transfer_ticks =
                o->total.retire_transfer_ticks -
                o->emitted.retire_transfer_ticks;
            delta.retire_transfer_max_ticks =
                o->pending_retire_transfer_max_ticks;
            delta.retired_other =
                o->total.retired_other - o->emitted.retired_other;
            delta.retire_other_ticks =
                o->total.retire_other_ticks -
                o->emitted.retire_other_ticks;
            delta.retire_other_max_ticks =
                o->pending_retire_other_max_ticks;
            delta.shape_submit_calls =
                o->total.shape_submit_calls - o->emitted.shape_submit_calls;
            delta.shape_make_room_calls =
                o->total.shape_make_room_calls -
                o->emitted.shape_make_room_calls;
            delta.shape_make_room_stalls =
                o->total.shape_make_room_stalls -
                o->emitted.shape_make_room_stalls;
            delta.shape_make_room_ticks =
                o->total.shape_make_room_ticks -
                o->emitted.shape_make_room_ticks;
            delta.shape_make_room_wait_ticks =
                o->total.shape_make_room_wait_ticks -
                o->emitted.shape_make_room_wait_ticks;
            delta.shape_make_room_max_wait_ticks =
                o->pending_shape_make_room_max_wait_ticks;
            delta.shape_make_room_depth_max =
                o->pending_shape_make_room_depth_max;
            delta.shape_make_room_count_max =
                o->pending_shape_make_room_count_max;
            delta.shape_make_room_wait_count_max =
                o->pending_shape_make_room_wait_count_max;
            delta.shape_posted =
                o->total.shape_posted - o->emitted.shape_posted;
            delta.shape_post_count_max = o->pending_shape_post_count_max;
            delta.shape_retired =
                o->total.shape_retired - o->emitted.shape_retired;
            delta.shape_retire_ticks =
                o->total.shape_retire_ticks - o->emitted.shape_retire_ticks;
            delta.shape_retire_max_ticks = o->pending_shape_retire_max_ticks;
            delta.shape_failures =
                o->total.shape_failures - o->emitted.shape_failures;
            delta.shape_mixed =
                o->total.shape_mixed - o->emitted.shape_mixed;
            delta.shape = o->total.shape;
            delta.first_submit =
                o->total.first_submit - o->emitted.first_submit;
            delta.failures = o->total.failures - o->emitted.failures;
            delta.last_ret = o->total.last_ret;
            delta.last_fence = o->total.last_fence;
            o->emitted = o->total;
            o->pending_make_room_max_wait_ticks = 0;
            o->pending_make_room_depth_max = 0;
            o->pending_make_room_count_max = 0;
            o->pending_make_room_wait_count_max = 0;
            o->pending_post_count_max = 0;
            o->pending_retire_max_ticks = 0;
            o->pending_retire_submit_3d_max_ticks = 0;
            o->pending_retire_flush_max_ticks = 0;
            o->pending_retire_transfer_max_ticks = 0;
            o->pending_retire_other_max_ticks = 0;
            o->pending_shape_make_room_max_wait_ticks = 0;
            o->pending_shape_make_room_depth_max = 0;
            o->pending_shape_make_room_count_max = 0;
            o->pending_shape_make_room_wait_count_max = 0;
            o->pending_shape_post_count_max = 0;
            o->pending_shape_retire_max_ticks = 0;
            o->pending_wait_progress_max_ticks = 0;
            o->pending_wait_used_max_ticks = 0;
            have = 1;
            break;
        }
        spin_unlock(&g->lock);
        if (!have)
            break;

        if (full_trace)
            printf("virtio-gpu-submit-owner-trace: owner_id=%lu owner_tgid=%d ctx_id=%u proc=%s debug=%s submit_calls=%lu submit_us=%lu lock_wait_us=%lu attach_count=%lu attach_us=%lu async_prepare_us=%lu post_us=%lu make_room_calls=%lu make_room_stalls=%lu make_room_us=%lu make_room_wait_us=%lu make_room_max_wait_us=%lu make_room_depth_max=%lu make_room_count_max=%lu make_room_wait_count_max=%lu wait_progress_calls=%lu wait_progress_us=%lu wait_progress_max_us=%lu wait_used_calls=%lu wait_used_us=%lu wait_used_max_us=%lu posted=%lu post_count_max=%lu retired=%lu retire_us=%lu retire_max_us=%lu retired_submit_3d=%lu retire_submit_3d_us=%lu retire_submit_3d_max_us=%lu retired_flush=%lu retire_flush_us=%lu retire_flush_max_us=%lu retired_transfer=%lu retire_transfer_us=%lu retire_transfer_max_us=%lu retired_other=%lu retire_other_us=%lu retire_other_max_us=%lu first_submit=%lu failures=%lu last_ret=%d last_fence=%lu\n",
                   key.owner_id, key.owner_tgid, key.ctx_id, key.proc,
                   key.debug_name, delta.submit_calls,
                   virtio_gpu_ticks_to_us(delta.submit_ticks),
                   virtio_gpu_ticks_to_us(delta.lock_wait_ticks),
                   delta.attach_count,
                   virtio_gpu_ticks_to_us(delta.attach_ticks),
                   virtio_gpu_ticks_to_us(delta.async_prepare_ticks),
                   virtio_gpu_ticks_to_us(delta.post_ticks),
                   delta.make_room_calls, delta.make_room_stalls,
                   virtio_gpu_ticks_to_us(delta.make_room_ticks),
                   virtio_gpu_ticks_to_us(delta.make_room_wait_ticks),
                   virtio_gpu_ticks_to_us(delta.make_room_max_wait_ticks),
                   delta.make_room_depth_max, delta.make_room_count_max,
                   delta.make_room_wait_count_max,
                   delta.wait_progress_calls,
                   virtio_gpu_ticks_to_us(delta.wait_progress_ticks),
                   virtio_gpu_ticks_to_us(delta.wait_progress_max_ticks),
                   delta.wait_used_calls,
                   virtio_gpu_ticks_to_us(delta.wait_used_ticks),
                   virtio_gpu_ticks_to_us(delta.wait_used_max_ticks),
                   delta.posted, delta.post_count_max, delta.retired,
                   virtio_gpu_ticks_to_us(delta.retire_ticks),
                   virtio_gpu_ticks_to_us(delta.retire_max_ticks),
                   delta.retired_submit_3d,
                   virtio_gpu_ticks_to_us(delta.retire_submit_3d_ticks),
                   virtio_gpu_ticks_to_us(delta.retire_submit_3d_max_ticks),
                   delta.retired_flush,
                   virtio_gpu_ticks_to_us(delta.retire_flush_ticks),
                   virtio_gpu_ticks_to_us(delta.retire_flush_max_ticks),
                   delta.retired_transfer,
                   virtio_gpu_ticks_to_us(delta.retire_transfer_ticks),
                   virtio_gpu_ticks_to_us(delta.retire_transfer_max_ticks),
                   delta.retired_other,
                   virtio_gpu_ticks_to_us(delta.retire_other_ticks),
                   virtio_gpu_ticks_to_us(delta.retire_other_max_ticks),
                   delta.first_submit,
                   delta.failures, delta.last_ret, delta.last_fence);
        if (hot_shape_trace && delta.shape.valid &&
            (delta.shape_submit_calls || delta.shape_make_room_calls ||
             delta.shape_posted || delta.shape_retired ||
             delta.shape_failures || delta.shape_mixed)) {
            printf("virtio-gpu-submit-depth-probe: owner_id=%lu owner_tgid=%d ctx_id=%u proc=%s debug=%s drm_flags=0x%x nr_dwords=%u resource_count=%u first_word=0x%x fence_fd=%d shape_submit_calls=%lu shape_make_room_calls=%lu shape_make_room_stalls=%lu shape_make_room_us=%lu shape_make_room_wait_us=%lu shape_make_room_max_wait_us=%lu shape_make_room_depth_max=%lu shape_make_room_count_max=%lu shape_make_room_wait_count_max=%lu shape_posted=%lu shape_post_count_max=%lu shape_retired=%lu shape_retire_us=%lu shape_retire_max_us=%lu shape_failures=%lu shape_mixed=%lu\n",
                   key.owner_id, key.owner_tgid, key.ctx_id, key.proc,
                   key.debug_name, delta.shape.drm_flags,
                   delta.shape.nr_dwords, delta.shape.resource_count,
                   delta.shape.first_word, delta.shape.fence_fd,
                   delta.shape_submit_calls, delta.shape_make_room_calls,
                   delta.shape_make_room_stalls,
                   virtio_gpu_ticks_to_us(delta.shape_make_room_ticks),
                   virtio_gpu_ticks_to_us(delta.shape_make_room_wait_ticks),
                   virtio_gpu_ticks_to_us(
                       delta.shape_make_room_max_wait_ticks),
                   delta.shape_make_room_depth_max,
                   delta.shape_make_room_count_max,
                   delta.shape_make_room_wait_count_max,
                   delta.shape_posted, delta.shape_post_count_max,
                   delta.shape_retired,
                   virtio_gpu_ticks_to_us(delta.shape_retire_ticks),
                   virtio_gpu_ticks_to_us(delta.shape_retire_max_ticks),
                   delta.shape_failures, delta.shape_mixed);
        }
    }

    spin_lock(&g->lock);
    if (g->stats.submit_trace_owner_drops !=
        g->stats.submit_trace_owner_emitted_drops) {
        uint64 drops_delta = g->stats.submit_trace_owner_drops -
            g->stats.submit_trace_owner_emitted_drops;
        uint64 drops = g->stats.submit_trace_owner_drops;

        g->stats.submit_trace_owner_emitted_drops =
            g->stats.submit_trace_owner_drops;
        spin_unlock(&g->lock);
        printf("virtio-gpu-submit-owner-trace: drops_delta=%lu drops=%lu\n",
               drops_delta, drops);
    } else {
        spin_unlock(&g->lock);
    }
}

static uint32 virtio_gpu_cmdline_uint(const char *key, uint32 default_value,
                                      uint32 max_value)
{
    char buf[16];
    uint32 value = 0;

    if (cmdline_get_param(key, buf, sizeof(buf)) != 0 || buf[0] == '\0')
        return default_value;
    for (int i = 0; buf[i] != '\0'; i++) {
        if (buf[i] < '0' || buf[i] > '9')
            return default_value;
        value = value * 10 + (uint32)(buf[i] - '0');
        if (value > max_value)
            return max_value;
    }
    return value;
}

static int virtio_gpu_async_scanout_flush_enabled(void)
{
    return virtio_gpu_cmdline_enabled("virtio_gpu_async_scanout_flush") ||
           virtio_gpu_cmdline_enabled("vgpu_async_flush");
}

static int virtio_gpu_use_3d_scanout(struct virtio_gpu *g)
{
    if (g == NULL || g->virgl_capset_id == 0 ||
        virtio_gpu_cmdline_enabled("virtio_gpu_no_3d_scanout"))
        return 0;
    /*
     * Alpine/Linux uses virgl-capable scanout resources and then flips between
     * full-screen resources.  Keep the old 2D scanout as an explicit opt-out for
     * hosts that cannot scan out a virgl 3D resource.
     */
    return 1;
}

static int virtio_gpu_pageflip_copy_enabled(void)
{
    if (virtio_gpu_cmdline_enabled("virtio_gpu_force_pageflip_copy"))
        return 1;
    if (virtio_gpu_cmdline_enabled("virtio_gpu_disable_pageflip_copy"))
        return 0;
    return virtio_gpu_cmdline_enabled("virtio_gpu_pageflip_copy");
}

static int virtio_gpu_pageflip_validate_enabled(void)
{
    if (virtio_gpu_cmdline_enabled("virtio_gpu_pageflip_no_validate") ||
        virtio_gpu_cmdline_enabled("virtio_gpu_pageflip_skip_validate"))
        return 0;
    return virtio_gpu_cmdline_enabled("virtio_gpu_pageflip_validate_copy");
}

static void virtio_gpu_page_flip_scanout_set_reset(struct virtio_gpu *g)
{
    memset(g->page_flip_scanout_set, 0, sizeof(g->page_flip_scanout_set));
    g->page_flip_scanout_set_count = 0;
    g->page_flip_scanout_width = 0;
    g->page_flip_scanout_height = 0;
}

static int virtio_gpu_page_flip_scanout_set_contains(struct virtio_gpu *g,
                                                     uint32 resource_id)
{
    for (uint32 i = 0; i < g->page_flip_scanout_set_count; i++) {
        if (g->page_flip_scanout_set[i] == resource_id)
            return 1;
    }
    return 0;
}

static void virtio_gpu_page_flip_scanout_set_note(struct virtio_gpu *g,
                                                  uint32 resource_id,
                                                  uint32 width,
                                                  uint32 height)
{
    if (g->page_flip_scanout_width != width ||
        g->page_flip_scanout_height != height)
        virtio_gpu_page_flip_scanout_set_reset(g);
    g->page_flip_scanout_width = width;
    g->page_flip_scanout_height = height;
    if (virtio_gpu_page_flip_scanout_set_contains(g, resource_id))
        return;
    if (g->page_flip_scanout_set_count >=
        VIRTIO_GPU_PAGE_FLIP_SCANOUT_SET_MAX) {
        virtio_gpu_page_flip_scanout_set_reset(g);
        g->page_flip_scanout_width = width;
        g->page_flip_scanout_height = height;
    }
    g->page_flip_scanout_set[g->page_flip_scanout_set_count++] = resource_id;
}

static void virtio_gpu_page_flip_scanout_set_remove(struct virtio_gpu *g,
                                                    uint32 resource_id)
{
    for (uint32 i = 0; i < g->page_flip_scanout_set_count; i++) {
        if (g->page_flip_scanout_set[i] != resource_id)
            continue;
        for (uint32 j = i + 1; j < g->page_flip_scanout_set_count; j++)
            g->page_flip_scanout_set[j - 1] = g->page_flip_scanout_set[j];
        g->page_flip_scanout_set_count--;
        g->page_flip_scanout_set[g->page_flip_scanout_set_count] = 0;
        break;
    }
    if (g->page_flip_scanout_set_count == 0) {
        g->page_flip_scanout_width = 0;
        g->page_flip_scanout_height = 0;
    }
}

static struct virtio_gpu_context *virtio_gpu_lookup_context_locked(
    struct virtio_gpu *g, uint32 id);
static void virtio_gpu_mark_context_failed_locked(
    struct virtio_gpu *g, struct virtio_gpu_context *ctx);
static uint64 virtio_gpu_ticks_to_us(uint64 ticks);
static void virtio_gpu_drain_sample_count(
    struct virtio_gpu_async_drain_sample *sample, uint32 type);
static int virtio_gpu_async_validate_retire(
    struct virtio_gpu *g, struct virtio_gpu_async_submit *a);
static struct virtio_gpu_async_submit *
virtio_gpu_async_find_slot_by_desc(struct virtio_gpu *g, uint32 id);
static int virtio_gpu_async_reap_completed_sample(
    struct virtio_gpu *g, struct virtio_gpu_async_drain_sample *sample);
static int virtio_gpu_async_wait_progress(struct virtio_gpu *g,
                                          uint32 budget_ms);
static int virtio_gpu_drain_async_submit_sample(
    struct virtio_gpu *g, int wait,
    struct virtio_gpu_async_drain_sample *sample);
static int virtio_gpu_drain_async_submit(struct virtio_gpu *g, int wait);
static void virtio_gpu_drop_present_flip_resources(struct virtio_gpu *g);
static int virtio_gpu_present_flip_slot_for_id(struct virtio_gpu *g,
                                               uint32 resource_id,
                                               uint32 *slot_out);
static void virtio_gpu_invalidate_present_flip_slot(struct virtio_gpu *g,
                                                    uint32 slot);

/*
 * Interrupt-side wakeups.  Called with q->lock held (lock order: q->lock ->
 * completion.lock).  Signals BOTH registration slots: the in-flight sync
 * command's private completion (if any) and the shared async progress
 * completion — with concurrent consumers we cannot know which waiter the
 * newly arrived used elements belong to, and each waiter re-checks its own
 * predicate after waking.
 */
static int virtio_gpu_complete_pending_locked(struct virtio_gpu *g,
                                              struct virtio_gpu_queue *q)
{
    if (q->used->idx == q->used_idx)
        return 0;
    if (q->sync_completion != NULL)
        complete_all(q->sync_completion);
    complete_all(&g->async_wait);
    return 1;
}

/*
 * N1 progress-detection fix: wait conditions are snapshot-compared
 * MOVEMENT, never used-ring occupancy.  used->idx is device-written and
 * monotonic (mod 2^16) and async_retire_seq is a guest-side monotonic
 * consumption counter, so a concurrent consumer that eats used elements
 * (and thereby erases occupancy) can no longer make a healthy queue look
 * stalled.  For sync waiters the condition additionally includes their
 * own completion flag so a reap performed by ANOTHER thread that
 * consumed the sync head still terminates the wait.
 */
struct virtio_gpu_wait_cond {
    struct virtio_gpu *g;
    struct virtio_gpu_queue *q;
    uint16 snap_used_idx;   /* device used->idx at snapshot time */
    uint64 snap_retire_seq; /* g->async_retire_seq at snapshot time */
    int sync;               /* also wake on q->sync_done */
};

static int virtio_gpu_wait_cond_ready(const struct virtio_gpu_wait_cond *c)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (c->sync &&
        __atomic_load_n(&c->q->sync_done, __ATOMIC_ACQUIRE) != 0)
        return 1;
    if (c->q->used->idx != c->snap_used_idx)
        return 1;
    return __atomic_load_n(&c->g->async_retire_seq, __ATOMIC_ACQUIRE) !=
           c->snap_retire_seq;
}

/*
 * @budget_ms: cumulative wall-clock budget granted by the caller's retry
 * loop, or 0 for a full standalone window.  Movement-based renewal
 * (attempt-6 fix) may RETRY a wait but must never EXTEND the caller's
 * total deadline: under unlocked-wait, foreign async traffic provides
 * movement forever, and an op_lock-holding sync waiter/park renewing a
 * fresh full window per movement stalled the whole desktop (archived
 * n1ab-unlocked global stall).
 */
static int virtio_gpu_wait_for_used(struct virtio_gpu *g,
                                    struct virtio_gpu_queue *q,
                                    completion_t *done,
                                    const struct virtio_gpu_wait_cond *cond,
                                    uint32 budget_ms)
{
    uint64 waited_ms = 0;
    int trace = virtio_gpu_submit_trace_enabled();
    uint64 trace_start = trace ? r_time() : 0;
    uint32 wait_limit_ms = virtio_gpu_cmdline_uint(
        "virtio_gpu_irq_wait_ms", VIRTIO_GPU_IRQ_WAIT_MS,
        VIRTIO_GPU_IRQ_WAIT_MS_MAX);

    if (budget_ms != 0 && budget_ms < wait_limit_ms)
        wait_limit_ms = budget_ms;
    int poll_fallback_counted = 0;
    int ret = 0;

    /*
     * QEMU often completes simple scanout and virgl control commands before
     * the interrupt path has a chance to schedule the waiter.  Poll briefly
     * first so compositor presents and WebKit submits do not sleep for a full
     * timer tick on every frame.
     */
    for (int i = 0; i < VIRTIO_GPU_FAST_POLL_LIMIT; i++) {
        if (virtio_gpu_wait_cond_ready(cond)) {
            ret = 1;
            goto out;
        }
    }

    /*
     * Keep the historical five-second failure deadline unless the launch asks
     * for a longer host-warmup window.  Poll the wait condition between short
     * sleeps so a missed or coalesced interrupt (or a completion consumed by a
     * concurrent reaper) costs about a millisecond, not a multi-second UI
     * freeze.
     */
    while (waited_ms < wait_limit_ms) {
        if (wait_for_completion_timeout(done,
                                        VIRTIO_GPU_IRQ_WAIT_SLICE_MS) != 0) {
            ret = 1;
            goto out;
        }
        waited_ms += VIRTIO_GPU_IRQ_WAIT_SLICE_MS;
        if (virtio_gpu_wait_cond_ready(cond)) {
            if (!poll_fallback_counted) {
                virtio_gpu_count_poll_fallback(g);
                poll_fallback_counted = 1;
            }
            ret = 1;
            goto out;
        }
    }

    if (!poll_fallback_counted)
        virtio_gpu_count_poll_fallback(g);
    for (int i = 0; i < VIRTIO_GPU_POLL_LIMIT; i++) {
        if (virtio_gpu_wait_cond_ready(cond)) {
            ret = 1;
            goto out;
        }
    }
out:
    if (trace)
        virtio_gpu_submit_trace_record_wait_for_used(g,
                                                     r_time() - trace_start);
    return ret;
}

/*
 * IRQ-driven reap (interactive-latency root cause, 2026-07-05):
 * completions used to be consumed only by SUBMIT/DRAIN/MAKE-ROOM
 * callers, so on an idle desktop a finished frame's slot (and its
 * fence) sat POSTED until the NEXT GPU activity reaped it — the
 * owner-trace showed retire_max ≈ 60s (the DRM fence-wait cap) for
 * kwin_wayland, felt as multi-second hover/tooltip/menu stalls that
 * shrank with cursor activity.  The reaper needs async_reap_serialize
 * (a mutex — unusable in IRQ context), so the interrupt queues this
 * work item instead; queue_work dedups while it is already pending.
 */
static struct workqueue *virtio_gpu_reap_wq;
static struct work_struct virtio_gpu_reap_work;
/* queue_work() has NO double-queue guard (enqueueing an already-queued
 * work_struct corrupts the list — first attempt hung boot at capset
 * time).  Same discipline as timerfd's work_pending flag. */
static int virtio_gpu_reap_work_queued;

static void virtio_gpu_reap_worker(struct work_struct *work)
{
    struct virtio_gpu *g = (struct virtio_gpu *)work->data;

    /* Clear BEFORE reaping: an IRQ arriving mid-reap re-queues us (the
     * work_struct is already off the list while running), so no
     * completion can be missed between our final consume and return. */
    __atomic_store_n(&virtio_gpu_reap_work_queued, 0, __ATOMIC_RELEASE);
    (void)virtio_gpu_async_reap_completed_sample(g, NULL);
}

static void virtio_gpu_reap_wq_init(struct virtio_gpu *g)
{
    virtio_gpu_reap_wq = workqueue_create("vgpu-reap", 1);
    if (virtio_gpu_reap_wq == NULL) {
        printf("virtio_gpu: reap workqueue unavailable; retire stays "
               "activity-driven\n");
        return;
    }
    init_work_struct(&virtio_gpu_reap_work, virtio_gpu_reap_worker,
                     (uint64)g);
}

static void virtio_gpu_intr(int irq, void *data, device_t *dev)
{
    struct virtio_gpu *g = (struct virtio_gpu *)data;
    struct virtio_gpu_queue *q;
    int kick_reap = 0;

    (void)irq;
    (void)dev;
    if (g == NULL)
        return;

    if (g->pci.isr != NULL) {
        volatile uint8 isr_status = *g->pci.isr;
        (void)isr_status;
    }

    q = &g->ctrlq;
    spin_lock(&q->lock);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (virtio_gpu_complete_pending_locked(g, q))
        virtio_gpu_count_irq_completion(g);
    /* Unconsumed used elements: retire them promptly instead of
     * waiting for the next submit/drain caller. */
    kick_reap = q->used->idx != q->used_idx;
    spin_unlock(&q->lock);

    if (kick_reap && virtio_gpu_reap_wq != NULL &&
        !__atomic_exchange_n(&virtio_gpu_reap_work_queued, 1,
                             __ATOMIC_ACQ_REL)) {
        if (!queue_work(virtio_gpu_reap_wq, &virtio_gpu_reap_work))
            __atomic_store_n(&virtio_gpu_reap_work_queued, 0,
                             __ATOMIC_RELEASE);
    }
}

/*
 * Record a FENCED synchronous command's fence into the global fence
 * progress after its response validated OK.  Only the async reaper ever
 * advanced stats.last_fence (true before and after the total-reaper
 * redesign), so a fenced submit that took the SYNC path (e.g. the
 * ctx_submit ring-pressure fallback) left its fence unsignaled: a
 * DRM fence waiter (gpu_drm_wait_virtio_fence 1ms-poll loop, 60s cap)
 * was only released when a LATER async fence happened to retire past
 * it — i.e. by unrelated activity.  Interactive symptom: hover/tooltip
 * /menu updates stalled for seconds while idle, recovering faster the
 * more the user moved the cursor.  Monotonic max: a stale sync fence
 * must never regress fence progress.
 */
static void virtio_gpu_record_sync_fence(struct virtio_gpu *g,
                                         const void *cmd)
{
    const struct virtio_gpu_ctrl_hdr *hdr = cmd;

    if (hdr == NULL || !(hdr->flags & VIRTIO_GPU_FLAG_FENCE) ||
        hdr->fence_id == 0)
        return;
    spin_lock(&g->lock);
    g->stats.fences++;
    if (hdr->fence_id > g->stats.last_fence)
        g->stats.last_fence = hdr->fence_id;
    spin_unlock(&g->lock);
}

/* Cumulative budget for movement-renewal retry loops: total wall time
 * equals ONE full wait window regardless of how many renewals occur. */
static uint32 virtio_gpu_wait_window_ms(void)
{
    return virtio_gpu_cmdline_uint("virtio_gpu_irq_wait_ms",
                                   VIRTIO_GPU_IRQ_WAIT_MS,
                                   VIRTIO_GPU_IRQ_WAIT_MS_MAX);
}

static uint32 virtio_gpu_wait_budget_remaining(uint64 start_jiffs,
                                               uint32 total_ms)
{
    uint64 elapsed = get_jiffs() - start_jiffs;
    return elapsed >= (uint64)total_ms ? 0 : (uint32)(total_ms - elapsed);
}

/*
 * Park a new sync post until every timed-out sync command's used element
 * has been swallowed by the reaper (q->sync_stale == 0).  While the
 * device owes a stale element it may still read the sync descriptor
 * block [0, 3) and DMA into the previous command's buffers; rewriting
 * those descriptors or re-posting head id 0 in that window would let the
 * host double-execute the NEW command's buffers, and a live completion
 * could be mis-swallowed as stale.  Reuses the 5s movement-window
 * discipline: the wait fails only after a full window with no queue
 * movement AND the stale element still missing.  Only a sync waiter
 * (an op_lock holder — us) ever increments sync_stale, so a 0 observed
 * here cannot regress before we post.  Lock order: (op_lock) ->
 * async_wait_serialize / async_reap_serialize -> q->lock (via callees).
 */
static int virtio_gpu_sync_park_stale(struct virtio_gpu *g,
                                      struct virtio_gpu_queue *q)
{
    if (__atomic_load_n(&q->sync_stale, __ATOMIC_ACQUIRE) == 0)
        return 0;
    (void)virtio_gpu_async_reap_completed_sample(g, NULL);
    uint64 budget_start = get_jiffs();
    uint32 budget_total = virtio_gpu_wait_window_ms();
    while (__atomic_load_n(&q->sync_stale, __ATOMIC_ACQUIRE) > 0) {
        uint32 remaining =
            virtio_gpu_wait_budget_remaining(budget_start, budget_total);
        if (remaining == 0 ||
            !virtio_gpu_async_wait_progress(g, remaining)) {
            /* One final reap before declaring the element lost (a
             * concurrent reaper may have raced our snapshot). */
            (void)virtio_gpu_async_reap_completed_sample(g, NULL);
            if (__atomic_load_n(&q->sync_stale, __ATOMIC_ACQUIRE) == 0)
                break;
            return -1;
        }
        (void)virtio_gpu_async_reap_completed_sample(g, NULL);
    }
    return 0;
}

/*
 * Post a synchronous command chain on the dedicated sync descriptor block
 * [0, 3) and register it in the queue's sync record.  Caller must hold
 * op_lock (which guarantees at most one sync command in flight); returns
 * -1 if the sync record is unexpectedly busy or the device never returned
 * a timed-out predecessor's buffers (in which case the post is refused —
 * the descriptor block is still device-owned).  Lock order: q->lock only,
 * plus the park's callees.
 */
static int virtio_gpu_sync_post(struct virtio_gpu *g,
                                struct virtio_gpu_queue *q, void *cmd,
                                uint32 cmd_len, void *data, uint32 data_len,
                                bool data_write, void *resp, uint32 resp_len,
                                completion_t *done)
{
    if (virtio_gpu_sync_park_stale(g, q) != 0) {
        printf("virtio_gpu: sync post refused; device owes %u stale sync completion(s)\n",
               __atomic_load_n(&q->sync_stale, __ATOMIC_ACQUIRE));
        virtio_gpu_count_timeout(g);
        virtio_gpu_count_failure(g);
        return -1;
    }

    int intena = spin_lock_irqsave(&q->lock);

    if (q->sync_inflight || q->sync_stale > 0) {
        spin_unlock_irqrestore(&q->lock, intena);
        printf("virtio_gpu: sync submit while another sync in flight\n");
        virtio_gpu_count_failure(g);
        return -1;
    }

    memset(resp, 0, resp_len);
    memset(q->desc, 0, 4 * sizeof(q->desc[0]));

    int resp_desc = 1;
    q->desc[0].addr = (uint64)cmd;
    q->desc[0].len = cmd_len;
    q->desc[0].flags = VRING_DESC_F_NEXT;
    if (data && data_len) {
        resp_desc = 2;
        q->desc[0].next = 1;
        q->desc[1].addr = (uint64)data;
        q->desc[1].len = data_len;
        q->desc[1].flags = VRING_DESC_F_NEXT |
                            (data_write ? VRING_DESC_F_WRITE : 0);
        q->desc[1].next = 2;
    } else {
        q->desc[0].next = 1;
    }
    q->desc[resp_desc].addr = (uint64)resp;
    q->desc[resp_desc].len = resp_len;
    q->desc[resp_desc].flags = VRING_DESC_F_WRITE;
    q->desc[resp_desc].next = 0;

    q->avail->ring[q->avail->idx % q->size] = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->avail->idx++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->sync_inflight = 1;
    __atomic_store_n(&q->sync_done, 0, __ATOMIC_RELEASE);
    q->sync_completion = done;
    virtio_gpu_notify(g, 0);
    spin_unlock_irqrestore(&q->lock, intena);
    return 0;
}

/*
 * Wait for the in-flight sync command registered by virtio_gpu_sync_post.
 * The waiter is itself a reaper: it consumes used elements through the
 * unified total reaper, which retires any async slots that complete in
 * the meantime (counted into @sample) and signals q->sync_done when the
 * sync head (id 0) arrives.  A reap performed by a CONCURRENT thread
 * (unlocked-wait make_room) equally signals sync_done — the completion
 * can no longer be stolen (B2).
 *
 * Returns 0 on completion, -1 after a full no-progress window (the sync
 * registration is cleared in both cases).  Lock order here:
 * async_reap_serialize -> q->lock (reap); q->lock alone for
 * snapshot/registration updates.
 */
static int virtio_gpu_sync_wait_done(struct virtio_gpu *g,
                                     struct virtio_gpu_queue *q,
                                     completion_t *done,
                                     struct virtio_gpu_async_drain_sample
                                     *sample)
{
    int intena;
    uint64 budget_start = get_jiffs();
    uint32 budget_total = virtio_gpu_wait_window_ms();

    for (;;) {
        struct virtio_gpu_wait_cond cond = {
            .g = g,
            .q = q,
            .sync = 1,
        };

        /*
         * Snapshot BEFORE reaping: any element arriving after the
         * snapshot registers as movement, so nothing that our reap
         * misses can be lost.  completion_reinit under q->lock keeps
         * the re-arm serialized against every complete_all site.
         */
        intena = spin_lock_irqsave(&q->lock);
        cond.snap_used_idx = q->used->idx;
        cond.snap_retire_seq = g->async_retire_seq;
        completion_reinit(done);
        spin_unlock_irqrestore(&q->lock, intena);

        (void)virtio_gpu_async_reap_completed_sample(g, sample);
        if (__atomic_load_n(&q->sync_done, __ATOMIC_ACQUIRE))
            break;
        /* Movement renews the RETRY, never the total deadline: this
         * waiter holds op_lock, and unbounded renewal under foreign
         * async movement froze the desktop (attempt-5 stall). */
        uint32 remaining =
            virtio_gpu_wait_budget_remaining(budget_start, budget_total);
        if (remaining != 0 &&
            virtio_gpu_wait_for_used(g, q, done, &cond, remaining))
            continue;

        /* Full window without movement: reap once more before deciding
         * the queue is wedged (a concurrent reaper may have consumed
         * the sync head just before our snapshot). */
        (void)virtio_gpu_async_reap_completed_sample(g, sample);
        intena = spin_lock_irqsave(&q->lock);
        if (q->sync_done) {
            spin_unlock_irqrestore(&q->lock, intena);
            break;
        }
        q->sync_inflight = 0;
        q->sync_completion = NULL;
        /* The device still owes this command's used element; the
         * reaper must swallow it instead of attributing it to the
         * NEXT sync command (in-order retirement per queue makes the
         * counter sufficient). */
        q->sync_stale++;
        spin_unlock_irqrestore(&q->lock, intena);
        return -1;
    }

    intena = spin_lock_irqsave(&q->lock);
    q->sync_done = 0;
    q->sync_completion = NULL;
    spin_unlock_irqrestore(&q->lock, intena);
    return 0;
}

static int virtio_gpu_submit_internal(struct virtio_gpu *g, void *cmd,
                                      uint32 cmd_len, void *data,
                                      uint32 data_len, bool data_write,
                                      void *resp, uint32 resp_len,
                                      uint32 expected,
                                      uint32 accepted_error,
                                      uint64 *drain_ticks_out,
                                      uint64 *command_ticks_out,
                                      struct virtio_gpu_async_drain_sample
                                      *drain_sample)
{
    struct virtio_gpu_queue *q = &g->ctrlq;
    uint32 type = ((struct virtio_gpu_ctrl_hdr *)cmd)->type;
    struct virtio_gpu_ctrl_hdr *resp_hdr = resp;
    completion_t done;
    uint64 start;

    start = r_time();
    if (virtio_gpu_drain_async_submit_sample(g, 1, drain_sample) != 0) {
        struct virtio_gpu_context *ctx;
        uint32 ctx_id = ((struct virtio_gpu_ctrl_hdr *)cmd)->ctx_id;
        int current_ctx_failed = 0;

        /*
         * Retiring an older async slot can report an error for a different
         * context.  That context is marked failed by the retire path; do not
         * poison the synchronous command that merely happened to drain it.
         */
        spin_lock(&g->lock);
        ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
        current_ctx_failed = ctx != NULL && ctx->failed;
        spin_unlock(&g->lock);
        if (current_ctx_failed && type != VIRTIO_GPU_CMD_CTX_DESTROY &&
            type != VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE)
            return -1;
    }
    if (drain_ticks_out != NULL)
        *drain_ticks_out = r_time() - start;

    completion_init(&done);
    if (virtio_gpu_sync_post(g, q, cmd, cmd_len, data, data_len, data_write,
                             resp, resp_len, &done) != 0)
        return -1;

    start = r_time();
    if (virtio_gpu_sync_wait_done(g, q, &done, drain_sample) != 0) {
        uint32 ctx_id = ((struct virtio_gpu_ctrl_hdr *)cmd)->ctx_id;

        if (command_ticks_out != NULL)
            *command_ticks_out = r_time() - start;
        virtio_gpu_count_timeout(g);
        virtio_gpu_count_failure(g);
        spin_lock(&g->lock);
        virtio_gpu_mark_context_failed_locked(
            g, virtio_gpu_lookup_context_locked(g, ctx_id));
        spin_unlock(&g->lock);
        printf("virtio_gpu: command 0x%x timed out (ctx=%u)\n", type, ctx_id);
        return -1;
    }
    if (command_ticks_out != NULL)
        *command_ticks_out = r_time() - start;

    if (resp_hdr->type != expected) {
        if (accepted_error != 0 && resp_hdr->type == accepted_error) {
            virtio_gpu_count_command(g, type);
            return 1;
        }
        virtio_gpu_count_failure(g);
        printf("virtio_gpu: command 0x%x response=0x%x expected=0x%x\n",
               type, resp_hdr->type, expected);
        return -1;
    }

    virtio_gpu_record_sync_fence(g, cmd);
    virtio_gpu_count_command(g, type);
    return 0;
}

/*
 * Post a synchronous command while async slots are still in flight and wait
 * for it, reaping async completions in the meantime.  With the unified
 * total reaper this is now structurally identical to the tail of
 * virtio_gpu_submit_internal — the reaper (any reaper) retires async slots
 * and routes the sync head to the queue's sync record (former third-reaper
 * blocker B1, now subsumed).  Async retire errors mark their own contexts
 * failed and do not poison this command's result.
 */
static int virtio_gpu_submit_mixed_async(struct virtio_gpu *g, void *cmd,
                                         uint32 cmd_len, void *data,
                                         uint32 data_len, bool data_write,
                                         void *resp, uint32 resp_len,
                                         uint32 expected,
                                         struct virtio_gpu_async_drain_sample
                                         *drain_sample)
{
    struct virtio_gpu_queue *q = &g->ctrlq;
    uint32 type = ((struct virtio_gpu_ctrl_hdr *)cmd)->type;
    struct virtio_gpu_ctrl_hdr *resp_hdr = resp;
    completion_t done;

    completion_init(&done);
    if (virtio_gpu_sync_post(g, q, cmd, cmd_len, data, data_len, data_write,
                             resp, resp_len, &done) != 0)
        return -1;

    if (virtio_gpu_sync_wait_done(g, q, &done, drain_sample) != 0) {
        uint32 ctx_id = ((struct virtio_gpu_ctrl_hdr *)cmd)->ctx_id;

        virtio_gpu_count_timeout(g);
        virtio_gpu_count_failure(g);
        spin_lock(&g->lock);
        virtio_gpu_mark_context_failed_locked(
            g, virtio_gpu_lookup_context_locked(g, ctx_id));
        spin_unlock(&g->lock);
        printf("virtio_gpu: mixed command 0x%x timed out (ctx=%u)\n",
               type, ctx_id);
        return -1;
    }

    if (resp_hdr->type != expected) {
        virtio_gpu_count_failure(g);
        printf("virtio_gpu: mixed command 0x%x response=0x%x expected=0x%x\n",
               type, resp_hdr->type, expected);
        return -1;
    }
    virtio_gpu_record_sync_fence(g, cmd);
    virtio_gpu_count_command(g, type);
    return 0;
}

static int virtio_gpu_submit(struct virtio_gpu *g, void *cmd, uint32 cmd_len,
                             void *data, uint32 data_len, bool data_write,
                             void *resp, uint32 resp_len, uint32 expected)
{
    return virtio_gpu_submit_internal(g, cmd, cmd_len, data, data_len,
                                      data_write, resp, resp_len, expected,
                                      0, NULL, NULL, NULL);
}

static int virtio_gpu_submit_accepting_error(struct virtio_gpu *g, void *cmd,
                                             uint32 cmd_len, void *data,
                                             uint32 data_len,
                                             bool data_write, void *resp,
                                             uint32 resp_len,
                                             uint32 expected,
                                             uint32 accepted_error)
{
    return virtio_gpu_submit_internal(g, cmd, cmd_len, data, data_len,
                                      data_write, resp, resp_len, expected,
                                      accepted_error, NULL, NULL, NULL);
}

static struct virtio_gpu_resource *virtio_gpu_alloc_resource_slot(
    struct virtio_gpu *g)
{
    for (int i = 0; i < VIRTIO_GPU_MAX_RESOURCES; i++) {
        if (!g->resources[i].in_use)
            return &g->resources[i];
    }
    return NULL;
}

static struct virtio_gpu_resource *virtio_gpu_lookup_resource_locked(
    struct virtio_gpu *g, uint32 id)
{
    if (id == 0)
        return NULL;
    for (int i = 0; i < VIRTIO_GPU_MAX_RESOURCES; i++) {
        if (g->resources[i].in_use && g->resources[i].id == id)
            return &g->resources[i];
    }
    return NULL;
}

static struct virtio_gpu_context *virtio_gpu_alloc_context_slot_locked(
    struct virtio_gpu *g)
{
    for (int i = 0; i < VIRTIO_GPU_MAX_CONTEXTS; i++) {
        if (!g->contexts[i].in_use)
            return &g->contexts[i];
    }
    return NULL;
}

static struct virtio_gpu_context *virtio_gpu_lookup_context_locked(
    struct virtio_gpu *g, uint32 id)
{
    if (id == 0)
        return NULL;
    for (int i = 0; i < VIRTIO_GPU_MAX_CONTEXTS; i++) {
        if (g->contexts[i].in_use && g->contexts[i].id == id)
            return &g->contexts[i];
    }
    return NULL;
}

static struct virtio_gpu_capset *virtio_gpu_lookup_capset_locked(
    struct virtio_gpu *g, uint32 capset_id)
{
    if (capset_id == 0)
        return NULL;
    for (int i = 0; i < VIRTIO_GPU_MAX_CAPSETS; i++) {
        if (g->capsets[i].valid && g->capsets[i].id == capset_id)
            return &g->capsets[i];
    }
    return NULL;
}

static int virtio_gpu_capset_supported(uint32 capset_id)
{
    return capset_id == VIRTIO_GPU_CAPSET_VIRGL ||
           capset_id == VIRTIO_GPU_CAPSET_VIRGL2;
}

static int virtio_gpu_capset_preferred(uint32 current_id, uint32 current_version,
                                       uint32 candidate_id,
                                       uint32 candidate_version)
{
    if (candidate_id == 0)
        return 0;
    if (current_id == 0)
        return 1;
    if (candidate_id == VIRTIO_GPU_CAPSET_VIRGL2 &&
        current_id != VIRTIO_GPU_CAPSET_VIRGL2)
        return 1;
    if (candidate_id == current_id && candidate_version > current_version)
        return 1;
    return 0;
}

static void virtio_gpu_mark_context_failed_locked(struct virtio_gpu *g,
                                                  struct virtio_gpu_context *ctx)
{
    if (ctx == NULL || !ctx->in_use || ctx->failed)
        return;
    ctx->failed = 1;
    g->stats.context_failed++;
    g->stats.context_failures++;
}

static void virtio_gpu_async_submit_free(struct virtio_gpu_async_submit *a)
{
    if (a->cmd != NULL)
        kfree(a->cmd);
    if (a->resp != NULL)
        kfree(a->resp);
    if (a->data != NULL)
        page_free(a->data, a->data_order);
    /*
     * N1: scrub the body FIRST and release the state to FREE LAST.
     * virtio_gpu_async_reserve_slot claims on state==FREE under q->lock
     * (with an ACQUIRE load pairing with this RELEASE store); with
     * unlocked-wait reapers this free can run concurrently with a
     * reserve scan, and a mid-memset state==FREE would let the poster's
     * fresh field writes race the remainder of the wipe.  `state` is
     * the first struct field, so wipe everything after it, then
     * release-store FREE.
     */
    _Static_assert(offsetof(struct virtio_gpu_async_submit, state) == 0,
                   "state must stay the first async slot field");
    memset((char *)a + sizeof(a->state), 0,
           sizeof(*a) - sizeof(a->state));
    __atomic_store_n(&a->state, VIRTIO_GPU_ASYNC_SLOT_FREE,
                     __ATOMIC_RELEASE);
}

static const char *virtio_gpu_virgl_cmd_name(uint32 cmd)
{
    switch (cmd) {
    case VIRGL_CCMD_NOP:
        return "NOP";
    case VIRGL_CCMD_CREATE_OBJECT:
        return "CREATE_OBJECT";
    case VIRGL_CCMD_BIND_OBJECT:
        return "BIND_OBJECT";
    case VIRGL_CCMD_DESTROY_OBJECT:
        return "DESTROY_OBJECT";
    case VIRGL_CCMD_SET_FRAMEBUFFER_STATE:
        return "SET_FRAMEBUFFER_STATE";
    case VIRGL_CCMD_SET_VERTEX_BUFFERS:
        return "SET_VERTEX_BUFFERS";
    case VIRGL_CCMD_CLEAR:
        return "CLEAR";
    case VIRGL_CCMD_DRAW_VBO:
        return "DRAW_VBO";
    case VIRGL_CCMD_RESOURCE_INLINE_WRITE:
        return "RESOURCE_INLINE_WRITE";
    case VIRGL_CCMD_SET_SAMPLER_VIEWS:
        return "SET_SAMPLER_VIEWS";
    case VIRGL_CCMD_SET_INDEX_BUFFER:
        return "SET_INDEX_BUFFER";
    case VIRGL_CCMD_SET_CONSTANT_BUFFER:
        return "SET_CONSTANT_BUFFER";
    case VIRGL_CCMD_BLIT:
        return "BLIT";
    case VIRGL_CCMD_RESOURCE_COPY_REGION:
        return "RESOURCE_COPY_REGION";
    case VIRGL_CCMD_SET_SUB_CTX:
        return "SET_SUB_CTX";
    case VIRGL_CCMD_CREATE_SUB_CTX:
        return "CREATE_SUB_CTX";
    case VIRGL_CCMD_DESTROY_SUB_CTX:
        return "DESTROY_SUB_CTX";
    case VIRGL_CCMD_TEXTURE_BARRIER:
        return "TEXTURE_BARRIER";
    case VIRGL_CCMD_COPY_TRANSFER3D:
        return "COPY_TRANSFER3D";
    case VIRGL_CCMD_CLEAR_TEXTURE:
        return "CLEAR_TEXTURE";
    default:
        return "UNKNOWN";
    }
}

static void virtio_gpu_print_virgl_timeout_stream(const uint32 *dw,
                                                  uint32 ndwords)
{
    uint32 off = 0;
    uint32 printed = 0;

    if (dw == NULL || ndwords == 0)
        return;
    while (off < ndwords && printed < 16) {
        uint32 hdr = dw[off];
        uint32 cmd = hdr & 0xff;
        uint32 obj = (hdr >> 8) & 0xff;
        uint32 len = hdr >> 16;
        uint32 a0 = off + 1 < ndwords ? dw[off + 1] : 0;
        uint32 a1 = off + 2 < ndwords ? dw[off + 2] : 0;
        uint32 a2 = off + 3 < ndwords ? dw[off + 3] : 0;
        uint32 a3 = off + 4 < ndwords ? dw[off + 4] : 0;

        printf("virtio_gpu: async timeout virgl[%u] off=%u cmd=%u(%s) obj=%u len=%u args=%08x,%08x,%08x,%08x\n",
               printed, off, cmd, virtio_gpu_virgl_cmd_name(cmd), obj, len,
               a0, a1, a2, a3);
        if (len + 1 == 0 || off + 1 + len <= off) {
            printf("virtio_gpu: async timeout virgl malformed off=%u len=%u ndwords=%u\n",
                   off, len, ndwords);
            return;
        }
        if (off + 1 + len > ndwords) {
            printf("virtio_gpu: async timeout virgl truncated off=%u len=%u ndwords=%u\n",
                   off, len, ndwords);
            return;
        }
        off += 1 + len;
        printed++;
    }
    if (off < ndwords)
        printf("virtio_gpu: async timeout virgl remaining off=%u ndwords=%u\n",
               off, ndwords);
}

static void virtio_gpu_print_async_timeout_diag_locked(
    struct virtio_gpu *g, const struct virtio_gpu_async_submit *a,
    uint64 now_ticks)
{
    struct virtio_gpu_context *ctx;
    uint32 *dw = (uint32 *)a->data;
    uint32 dw0 = 0, dw1 = 0, dw2 = 0, dw3 = 0;
    uint32 ndwords = a->data_len / sizeof(uint32);
    pid_t owner_tgid = 0;
    uint64 owner_id = 0;
    uint64 age_us = 0;

    ctx = virtio_gpu_lookup_context_locked(g, a->ctx_id);
    if (ctx != NULL) {
        owner_tgid = ctx->owner_tgid;
        owner_id = ctx->owner_id;
    }
    if (a->posted_ticks != 0 && now_ticks >= a->posted_ticks)
        age_us = virtio_gpu_ticks_to_us(now_ticks - a->posted_ticks);
    if (dw != NULL && ndwords > 0)
        dw0 = dw[0];
    if (dw != NULL && ndwords > 1)
        dw1 = dw[1];
    if (dw != NULL && ndwords > 2)
        dw2 = dw[2];
    if (dw != NULL && ndwords > 3)
        dw3 = dw[3];

    printf("virtio_gpu: async timeout detail type=0x%x ctx=%u owner_tgid=%d owner_id=%lu fence=%lu desc=%u cmd_len=%u data_len=%u ndwords=%u age_us=%lu head=%08x,%08x,%08x,%08x\n",
           a->type, a->ctx_id, owner_tgid, owner_id, a->fence_id,
           a->desc_base, a->cmd_len, a->data_len, ndwords, age_us,
           dw0, dw1, dw2, dw3);
    virtio_gpu_print_virgl_timeout_stream(dw, ndwords);
}

static uint64 virtio_gpu_ticks_to_us(uint64 ticks)
{
    extern uint64 __timebase_frequency;
    uint64 freq = __timebase_frequency ? __timebase_frequency : 10000000UL;

    return ticks * 1000000ULL / freq;
}

static void virtio_gpu_op_lock(struct virtio_gpu *g,
                               enum virtio_gpu_op_holder holder)
{
    mutex_lock(&g->op_lock);
    __atomic_store_n(&g->op_lock_holder, holder, __ATOMIC_RELAXED);
}

static int virtio_gpu_op_lock_timed(struct virtio_gpu *g,
                                    enum virtio_gpu_op_holder holder,
                                    uint64 timeout_ms)
{
    int ret;

    ret = mutex_lock_timed(&g->op_lock, timeout_ms);
    if (ret == 0)
        __atomic_store_n(&g->op_lock_holder, holder, __ATOMIC_RELAXED);
    return ret;
}

static void virtio_gpu_op_unlock(struct virtio_gpu *g)
{
    __atomic_store_n(&g->op_lock_holder, VIRTIO_GPU_OP_NONE,
                     __ATOMIC_RELAXED);
    mutex_unlock(&g->op_lock);
}

static void virtio_gpu_scanout_perf_log(int ret, int path, int already_bound,
                                        int wait_holder,
                                        uint64 total_ticks,
                                        uint64 lock_wait_ticks,
                                        uint64 set_scanout_ticks,
                                        uint64 flush_ticks)
{
    static uint64 calls;
    static uint64 total_total;
    static uint64 lock_wait_total;
    static uint64 set_scanout_total;
    static uint64 flush_total;
    static uint64 failed;
    static uint64 wait_submit_3d;
    static uint64 wait_scanout;
    static uint64 wait_context;
    static uint64 wait_fence;
    static uint64 wait_capset;
    static uint64 wait_resource;
    static uint64 wait_transfer;
    static uint64 wait_fb_present;
    static uint64 wait_present_copy;
    static uint64 wait_probe;
    static uint64 wait_resize;
    static uint64 wait_page_flip;
    static uint64 wait_other;
    static uint64 wait_none;
    static uint64 wait_submit_3d_ticks;
    static uint64 wait_scanout_ticks;
    static uint64 wait_context_ticks;
    static uint64 wait_fence_ticks;
    static uint64 wait_capset_ticks;
    static uint64 wait_resource_ticks;
    static uint64 wait_transfer_ticks;
    static uint64 wait_fb_present_ticks;
    static uint64 wait_present_copy_ticks;
    static uint64 wait_probe_ticks;
    static uint64 wait_resize_ticks;
    static uint64 wait_page_flip_ticks;
    static uint64 wait_other_ticks;
    static uint64 wait_none_ticks;
    const char *path_name = "none";

    switch (path) {
    case 1:
        path_name = "noflush";
        break;
    case 2:
        path_name = "async-flush";
        break;
    case 3:
        path_name = "sync-flush";
        break;
    }

    calls++;
    total_total += total_ticks;
    lock_wait_total += lock_wait_ticks;
    set_scanout_total += set_scanout_ticks;
    flush_total += flush_ticks;
    if (ret != 0)
        failed++;
    if (lock_wait_ticks != 0) {
        switch (wait_holder) {
        case VIRTIO_GPU_OP_SUBMIT_3D:
            wait_submit_3d++;
            wait_submit_3d_ticks += lock_wait_ticks;
            break;
        case VIRTIO_GPU_OP_SCANOUT:
            wait_scanout++;
            wait_scanout_ticks += lock_wait_ticks;
            break;
        case VIRTIO_GPU_OP_CONTEXT:
            wait_context++;
            wait_context_ticks += lock_wait_ticks;
            break;
        case VIRTIO_GPU_OP_FENCE:
            wait_fence++;
            wait_fence_ticks += lock_wait_ticks;
            break;
        case VIRTIO_GPU_OP_CAPSET:
            wait_capset++;
            wait_capset_ticks += lock_wait_ticks;
            break;
        case VIRTIO_GPU_OP_RESOURCE:
            wait_resource++;
            wait_resource_ticks += lock_wait_ticks;
            break;
        case VIRTIO_GPU_OP_TRANSFER:
            wait_transfer++;
            wait_transfer_ticks += lock_wait_ticks;
            break;
        case VIRTIO_GPU_OP_FB_PRESENT:
            wait_fb_present++;
            wait_fb_present_ticks += lock_wait_ticks;
            break;
        case VIRTIO_GPU_OP_PRESENT_COPY:
            wait_present_copy++;
            wait_present_copy_ticks += lock_wait_ticks;
            break;
        case VIRTIO_GPU_OP_PROBE:
            wait_probe++;
            wait_probe_ticks += lock_wait_ticks;
            break;
        case VIRTIO_GPU_OP_RESIZE:
            wait_resize++;
            wait_resize_ticks += lock_wait_ticks;
            break;
        case VIRTIO_GPU_OP_PAGE_FLIP:
            wait_page_flip++;
            wait_page_flip_ticks += lock_wait_ticks;
            break;
        case VIRTIO_GPU_OP_OTHER:
            wait_other++;
            wait_other_ticks += lock_wait_ticks;
            break;
        default:
            wait_none++;
            wait_none_ticks += lock_wait_ticks;
            break;
        }
    }
    if (calls < 20)
        return;

    printf("virtio-gpu: scanout avg_us total=%lu lock_wait=%lu set_scanout=%lu flush=%lu calls=%lu failed=%lu wait_submit_3d=%lu wait_scanout=%lu wait_context=%lu wait_fence=%lu wait_capset=%lu wait_resource=%lu wait_transfer=%lu wait_fb_present=%lu wait_present_copy=%lu wait_probe=%lu wait_resize=%lu wait_page_flip=%lu wait_other=%lu wait_none=%lu wait_us submit_3d=%lu scanout=%lu context=%lu fence=%lu capset=%lu resource=%lu transfer=%lu fb_present=%lu present_copy=%lu probe=%lu resize=%lu page_flip=%lu other=%lu none=%lu path=%s already_bound=%d\n",
           virtio_gpu_ticks_to_us(total_total) / calls,
           virtio_gpu_ticks_to_us(lock_wait_total) / calls,
           virtio_gpu_ticks_to_us(set_scanout_total) / calls,
           virtio_gpu_ticks_to_us(flush_total) / calls,
           calls, failed, wait_submit_3d, wait_scanout, wait_context,
           wait_fence, wait_capset, wait_resource, wait_transfer,
           wait_fb_present, wait_present_copy, wait_probe, wait_resize,
           wait_page_flip, wait_other, wait_none,
           virtio_gpu_ticks_to_us(wait_submit_3d_ticks) / calls,
           virtio_gpu_ticks_to_us(wait_scanout_ticks) / calls,
           virtio_gpu_ticks_to_us(wait_context_ticks) / calls,
           virtio_gpu_ticks_to_us(wait_fence_ticks) / calls,
           virtio_gpu_ticks_to_us(wait_capset_ticks) / calls,
           virtio_gpu_ticks_to_us(wait_resource_ticks) / calls,
           virtio_gpu_ticks_to_us(wait_transfer_ticks) / calls,
           virtio_gpu_ticks_to_us(wait_fb_present_ticks) / calls,
           virtio_gpu_ticks_to_us(wait_present_copy_ticks) / calls,
           virtio_gpu_ticks_to_us(wait_probe_ticks) / calls,
           virtio_gpu_ticks_to_us(wait_resize_ticks) / calls,
           virtio_gpu_ticks_to_us(wait_page_flip_ticks) / calls,
           virtio_gpu_ticks_to_us(wait_other_ticks) / calls,
           virtio_gpu_ticks_to_us(wait_none_ticks) / calls,
           path_name, already_bound);
    calls = 0;
    total_total = 0;
    lock_wait_total = 0;
    set_scanout_total = 0;
    flush_total = 0;
    failed = 0;
    wait_submit_3d = 0;
    wait_scanout = 0;
    wait_context = 0;
    wait_fence = 0;
    wait_capset = 0;
    wait_resource = 0;
    wait_transfer = 0;
    wait_fb_present = 0;
    wait_present_copy = 0;
    wait_probe = 0;
    wait_resize = 0;
    wait_page_flip = 0;
    wait_other = 0;
    wait_none = 0;
    wait_submit_3d_ticks = 0;
    wait_scanout_ticks = 0;
    wait_context_ticks = 0;
    wait_fence_ticks = 0;
    wait_capset_ticks = 0;
    wait_resource_ticks = 0;
    wait_transfer_ticks = 0;
    wait_fb_present_ticks = 0;
    wait_present_copy_ticks = 0;
    wait_probe_ticks = 0;
    wait_resize_ticks = 0;
    wait_page_flip_ticks = 0;
    wait_other_ticks = 0;
    wait_none_ticks = 0;
}

static void virtio_gpu_transfer_perf_log(int ret, int from_host, int async_path,
                                         int mixed_path,
                                         int bound_scanout,
                                         int wait_holder,
                                         uint64 total_ticks,
                                         uint64 lock_wait_ticks,
                                         uint64 submit_ticks,
                                         uint64 drain_ticks,
                                         uint64 command_ticks,
                                         uint64 drain_submit_3d,
                                         uint64 drain_flush,
                                         uint64 drain_transfer,
                                         uint64 drain_other,
                                         uint32 x, uint32 y,
                                         uint32 width, uint32 height,
                                         uint32 resource_id, uint32 ctx_id,
                                         uint32 res_width, uint32 res_height,
                                         uint32 target, uint32 bind,
                                         uint64 offset, uint32 stride,
                                         pid_t owner_tgid)
{
    static uint64 calls;
    static uint64 failed;
    static uint64 from_host_calls;
    static uint64 to_host_calls;
    static uint64 async_calls;
    static uint64 mixed_calls;
    static uint64 bound_scanout_calls;
    static uint64 total_total;
    static uint64 lock_wait_total;
    static uint64 submit_total;
    static uint64 drain_total;
    static uint64 command_total;
    static uint64 drain_submit_3d_total;
    static uint64 drain_flush_total;
    static uint64 drain_transfer_total;
    static uint64 drain_other_total;
    static uint64 pixels_total;
    static uint64 wait_transfer;
    static uint64 wait_scanout;
    static uint64 wait_submit_3d;
    static uint64 wait_fence;
    static uint64 wait_other;
    static uint64 wait_none;

    calls++;
    total_total += total_ticks;
    lock_wait_total += lock_wait_ticks;
    submit_total += submit_ticks;
    drain_total += drain_ticks;
    command_total += command_ticks;
    drain_submit_3d_total += drain_submit_3d;
    drain_flush_total += drain_flush;
    drain_transfer_total += drain_transfer;
    drain_other_total += drain_other;
    pixels_total += (uint64)width * height;
    if (ret != 0)
        failed++;
    if (from_host)
        from_host_calls++;
    else
        to_host_calls++;
    if (async_path)
        async_calls++;
    if (mixed_path)
        mixed_calls++;
    if (bound_scanout)
        bound_scanout_calls++;
    if (lock_wait_ticks != 0) {
        switch (wait_holder) {
        case VIRTIO_GPU_OP_TRANSFER:
            wait_transfer++;
            break;
        case VIRTIO_GPU_OP_SCANOUT:
            wait_scanout++;
            break;
        case VIRTIO_GPU_OP_SUBMIT_3D:
            wait_submit_3d++;
            break;
        case VIRTIO_GPU_OP_FENCE:
            wait_fence++;
            break;
        case VIRTIO_GPU_OP_NONE:
            wait_none++;
            break;
        default:
            wait_other++;
            break;
        }
    }
    if (calls < 20)
        return;

    printf("virtio-gpu: transfer avg_us total=%lu lock_wait=%lu submit=%lu drain=%lu command=%lu drain_submit_3d=%lu drain_flush=%lu drain_transfer=%lu drain_other=%lu calls=%lu failed=%lu from_host=%lu to_host=%lu async=%lu mixed=%lu bound_scanout=%lu avg_pixels=%lu wait_transfer=%lu wait_scanout=%lu wait_submit_3d=%lu wait_fence=%lu wait_other=%lu wait_none=%lu last_from_host=%d last_res=%u ctx=%u owner_tgid=%d res=%ux%u target=0x%x bind=0x%x offset=%lu stride=%u last_rect=%u,%u %ux%u\n",
           virtio_gpu_ticks_to_us(total_total) / calls,
           virtio_gpu_ticks_to_us(lock_wait_total) / calls,
           virtio_gpu_ticks_to_us(submit_total) / calls,
           virtio_gpu_ticks_to_us(drain_total) / calls,
           virtio_gpu_ticks_to_us(command_total) / calls,
           drain_submit_3d_total, drain_flush_total, drain_transfer_total,
           drain_other_total,
           calls, failed, from_host_calls, to_host_calls, async_calls, mixed_calls,
           bound_scanout_calls, pixels_total / calls, wait_transfer,
           wait_scanout, wait_submit_3d, wait_fence, wait_other, wait_none,
           from_host, resource_id, ctx_id, owner_tgid, res_width, res_height,
           target, bind, offset, stride, x, y, width, height);
    calls = 0;
    failed = 0;
    from_host_calls = 0;
    to_host_calls = 0;
    async_calls = 0;
    mixed_calls = 0;
    bound_scanout_calls = 0;
    total_total = 0;
    lock_wait_total = 0;
    submit_total = 0;
    drain_total = 0;
    command_total = 0;
    drain_submit_3d_total = 0;
    drain_flush_total = 0;
    drain_transfer_total = 0;
    drain_other_total = 0;
    pixels_total = 0;
    wait_transfer = 0;
    wait_scanout = 0;
    wait_submit_3d = 0;
    wait_fence = 0;
    wait_other = 0;
    wait_none = 0;
}

static int virtio_gpu_async_depth(struct virtio_gpu *g)
{
    if (g->async_depth == 0) {
        int depth = (virtio_gpu_cmdline_enabled("vgpu_async_pf") ||
                     virtio_gpu_cmdline_enabled(
                         "virtio_gpu_async_page_flip_scanout")) ? 32 : 2;

        depth = (int)virtio_gpu_cmdline_uint("virtio_gpu_async_depth",
                                             (uint32)depth,
                                             VIRTIO_GPU_ASYNC_MAX_DEPTH);
        if (depth < 1)
            depth = 1;
        /* Never exceed the negotiated descriptor budget (finding #7);
         * capacity 0 disables the ring. */
        if (depth > g->async_capacity)
            depth = g->async_capacity;
        g->async_depth = depth;
    }
    return g->async_depth;
}

static int virtio_gpu_async_depth_for_reason(
    struct virtio_gpu *g, enum virtio_gpu_async_reason reason)
{
    int depth = virtio_gpu_async_depth(g);

    if (reason == VIRTIO_GPU_ASYNC_REASON_SUBMIT_3D &&
        !virtio_gpu_cmdline_present("virtio_gpu_async_depth")) {
        /*
         * Keep one older RESOURCE_FLUSH in flight while admitting the next
         * GL submit.  A default of one compares the total async population
         * against one, so the flush re-serializes compose(N) even though the
         * FIFO control queue already preserves producer-before-present order.
         * Two is the smallest overlap that removes that serialization; a
         * globally constrained one-slot ring must remain one.  The explicit
         * submit-depth knob below stays authoritative and retains its existing
         * range and negotiated-capacity clamps.
         */
        int submit_default_depth = depth < 2 ? depth : 2;

        depth = (int)virtio_gpu_cmdline_uint(
            "virtio_gpu_async_submit_depth", (uint32)submit_default_depth,
            VIRTIO_GPU_ASYNC_MAX_DEPTH);
        if (depth < 1)
            depth = 1;
        if (depth > g->async_capacity)
            depth = g->async_capacity;
    }
    return depth;
}

static void virtio_gpu_count_async_make_room_locked(
    struct virtio_gpu *g, enum virtio_gpu_async_reason reason, int count_call,
    int stalled)
{
    if (count_call)
        g->stats.async_make_room_calls++;
    switch (reason) {
    case VIRTIO_GPU_ASYNC_REASON_SUBMIT_3D:
        if (count_call)
            g->stats.async_make_room_submit_3d_calls++;
        if (stalled)
            g->stats.async_make_room_submit_3d_stalls++;
        break;
    case VIRTIO_GPU_ASYNC_REASON_FLUSH:
        if (count_call)
            g->stats.async_make_room_flush_calls++;
        if (stalled)
            g->stats.async_make_room_flush_stalls++;
        break;
    case VIRTIO_GPU_ASYNC_REASON_TRANSFER:
        if (count_call)
            g->stats.async_make_room_transfer_calls++;
        if (stalled)
            g->stats.async_make_room_transfer_stalls++;
        break;
    default:
        break;
    }
    if (stalled)
        g->stats.async_make_room_stalls++;
}

static void virtio_gpu_count_async_posted_locked(
    struct virtio_gpu *g, uint32 type)
{
    g->stats.async_posted++;
    switch (type) {
    case VIRTIO_GPU_CMD_SUBMIT_3D:
        g->stats.async_posted_submit_3d++;
        break;
    case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
        g->stats.async_posted_flush++;
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D:
        g->stats.async_posted_transfer++;
        break;
    default:
        break;
    }
}

static void virtio_gpu_drain_sample_count(
    struct virtio_gpu_async_drain_sample *sample, uint32 type)
{
    if (sample == NULL)
        return;
    switch (type) {
    case VIRTIO_GPU_CMD_SUBMIT_3D:
        sample->submit_3d++;
        break;
    case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
        sample->flush++;
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D:
        sample->transfer++;
        break;
    default:
        sample->other++;
        break;
    }
}

/*
 * Validate and free a single completed async slot.  The host has already
 * written this slot's response (its descriptor chain appeared in the used
 * ring).  Returns 0 on success, -1 if the response/fence was unexpected.
 */
static int virtio_gpu_async_validate_retire(struct virtio_gpu *g,
                                            struct virtio_gpu_async_submit *a)
{
    struct virtio_gpu_ctrl_hdr *resp_hdr = (struct virtio_gpu_ctrl_hdr *)a->resp;
    int ret = 0;

    if (resp_hdr->type != a->expected) {
        virtio_gpu_count_failure(g);
        spin_lock(&g->lock);
        virtio_gpu_mark_context_failed_locked(
            g, virtio_gpu_lookup_context_locked(g, a->ctx_id));
        spin_unlock(&g->lock);
        printf("virtio_gpu: async command 0x%x response=0x%x expected=0x%x\n",
               a->type, resp_hdr->type, a->expected);
        ret = -1;
    } else if (a->fence_id != 0) {
        if (resp_hdr->fence_id != a->fence_id) {
            virtio_gpu_count_failure(g);
            spin_lock(&g->lock);
            virtio_gpu_mark_context_failed_locked(
                g, virtio_gpu_lookup_context_locked(g, a->ctx_id));
            spin_unlock(&g->lock);
            printf("virtio_gpu: async fence mismatch got=%lu expected=%lu\n",
                   resp_hdr->fence_id, a->fence_id);
            ret = -1;
        } else {
            spin_lock(&g->lock);
            g->stats.fences++;
            g->stats.last_fence = a->fence_id;
            spin_unlock(&g->lock);
        }
    }

    if (ret == 0)
        virtio_gpu_count_command(g, a->type);
    virtio_gpu_submit_trace_record_async_retire(g, a, ret == 0 ? 0 : -EIO);
    virtio_gpu_async_submit_free(a);
    return ret;
}

/*
 * Map a completed descriptor-chain head id back to its owning ring slot.
 * Caller must hold q->lock; only POSTED and ABANDONED slots — the states
 * in which the device owns the chain — can match.
 */
static struct virtio_gpu_async_submit *
virtio_gpu_async_find_slot_by_desc(struct virtio_gpu *g, uint32 id)
{
    for (int i = 0; i < VIRTIO_GPU_ASYNC_MAX_DEPTH; i++) {
        struct virtio_gpu_async_submit *a = &g->async_ring[i];

        if ((a->state == VIRTIO_GPU_ASYNC_SLOT_POSTED ||
             a->state == VIRTIO_GPU_ASYNC_SLOT_ABANDONED) &&
            a->desc_base == id)
            return a;
    }
    return NULL;
}

/*
 * The unified TOTAL reaper: consume every used element the host has already
 * retired and route each one to its owner —
 *   id 0                  -> the queue's sync record (B2 fix: sync
 *                            completions are signaled, never discarded);
 *   id 8 + i*4 (POSTED)   -> validate + free slot i;
 *   id 8 + i*4 (ABANDONED)-> free slot i's memory now that the device is
 *                            provably done with it (B3 fix);
 *   anything else         -> stale; consumed with a warning so the used
 *                            ring never wedges.
 * Retirement follows the host's actual completion order (read from the used
 * ring) rather than submission order, because QEMU's virgl renderer may
 * finish a fenced SUBMIT_3D after a later unfenced RESOURCE_FLUSH.
 * Caller must hold async_reap_serialize (single consumer).  Lock order:
 * async_reap_serialize -> q->lock -> completion.lock, and g->lock taken
 * only with no spinlock held (inside validate_retire) or nested under
 * nothing here.  Returns -1 if any retired command reported an unexpected
 * response.
 */
static int virtio_gpu_async_reap_completed_reaplocked(
    struct virtio_gpu *g, struct virtio_gpu_async_drain_sample *sample)
{
    struct virtio_gpu_queue *q = &g->ctrlq;
    int ret = 0;

    for (;;) {
        struct virtio_gpu_async_submit *a = NULL;
        uint32 id = 0;
        int have = 0;
        int stale_sync = 0;
        int abandoned = 0;
        int intena;

        intena = spin_lock_irqsave(&q->lock);
        if (q->used->idx != q->used_idx) {
            id = q->used->ring[q->used_idx % q->size].id;
            q->used_idx = (uint16)(q->used_idx + 1);
            have = 1;
            if (id == 0) {
                /* Sync head.  ONLY id 0 is a sync completion — the
                 * chain head is what the device returns; ids 1..7 in
                 * the reserved block are never posted as heads and are
                 * warn-consumed below like any unknown id.  Timed-out
                 * sync elements arrive in order BEFORE the current
                 * in-flight command's element — swallow those first,
                 * then signal the waiter through the sync record (the
                 * waiter owns the cmd/resp buffers and validates the
                 * response itself). */
                if (q->sync_stale > 0) {
                    q->sync_stale--;
                    g->async_retire_seq++;
                    stale_sync = 1;
                } else if (q->sync_inflight) {
                    q->sync_inflight = 0;
                    g->async_retire_seq++;
                    __atomic_store_n(&q->sync_done, 1, __ATOMIC_RELEASE);
                    if (q->sync_completion != NULL)
                        complete_all(q->sync_completion);
                } else {
                    stale_sync = 1;
                }
            } else {
                a = virtio_gpu_async_find_slot_by_desc(g, id);
                if (a != NULL)
                    abandoned = a->state ==
                                VIRTIO_GPU_ASYNC_SLOT_ABANDONED;
            }
        }
        spin_unlock_irqrestore(&q->lock, intena);
        if (!have)
            break;
        if (id == 0) {
            if (stale_sync)
                printf("virtio_gpu: reap stale sync completion id=%u\n",
                       id);
            continue;
        }
        if (a == NULL) {
            printf("virtio_gpu: async reap unknown desc id=%u\n", id);
            continue;
        }

        if (abandoned) {
            struct virtio_gpu_ctrl_hdr *resp_hdr =
                (struct virtio_gpu_ctrl_hdr *)a->resp;

            /* Slot was abandoned by a timeout abort while the device
             * still owned it; the used element proves the device is
             * done, so its memory may finally be recycled.  If the
             * late response is OK, record the fence and command
             * accounting exactly like a POSTED retire minus the
             * context-failure side effects (the abort already failed
             * the context) — a client polling for this fence must not
             * spin forever.  last_fence only advances: an arbitrarily
             * late abandoned fence must never regress it below newer
             * signaled fences. */
            if (resp_hdr != NULL && resp_hdr->type == a->expected) {
                if (a->fence_id != 0 &&
                    resp_hdr->fence_id == a->fence_id) {
                    spin_lock(&g->lock);
                    g->stats.fences++;
                    if (a->fence_id > g->stats.last_fence)
                        g->stats.last_fence = a->fence_id;
                    spin_unlock(&g->lock);
                }
                virtio_gpu_count_command(g, a->type);
            }
            virtio_gpu_async_submit_free(a);
            intena = spin_lock_irqsave(&q->lock);
            g->async_abandoned--;
            g->async_retire_seq++;
            complete_all(&g->async_wait);
            spin_unlock_irqrestore(&q->lock, intena);
            continue;
        }

        virtio_gpu_drain_sample_count(sample, a->type);
        if (virtio_gpu_async_validate_retire(g, a) != 0)
            ret = -1;
        /* Count transition under q->lock so a concurrent poster's
         * reserve (++) never loses this decrement; complete_all wakes
         * progress waiters promptly (they poll retire_seq anyway). */
        intena = spin_lock_irqsave(&q->lock);
        g->async_count--;
        g->async_retire_seq++;
        complete_all(&g->async_wait);
        spin_unlock_irqrestore(&q->lock, intena);
        spin_lock(&g->lock);
        g->stats.async_retired++;
        spin_unlock(&g->lock);
    }
    return ret;
}

static int virtio_gpu_async_reap_completed_sample(
    struct virtio_gpu *g, struct virtio_gpu_async_drain_sample *sample)
{
    int ret;

    /* Single consumer: see async_reap_serialize's comment.  Lock order:
     * (op_lock) -> async_reap_serialize -> q->lock. */
    mutex_lock(&g->async_reap_serialize);
    ret = virtio_gpu_async_reap_completed_reaplocked(g, sample);
    mutex_unlock(&g->async_reap_serialize);
    return ret;
}

static int virtio_gpu_async_reap_completed(struct virtio_gpu *g)
{
    return virtio_gpu_async_reap_completed_sample(g, NULL);
}

/*
 * Block until the control queue makes PROGRESS: the device advances
 * used->idx past the entry snapshot, or a consumer bumps async_retire_seq
 * (both monotonic, snapshot-compared).  Occupancy is only an entry-time
 * short-circuit: an unconsumed element is progress the caller can reap.
 * A healthy queue whose completions are eaten by a concurrent consumer
 * can no longer look stalled (former progress-detection defect).
 * Callers must re-reap and re-check their own predicate after a 0 return
 * before treating the queue as wedged.  Lock order: (op_lock) ->
 * async_wait_serialize -> q->lock.
 */
static int virtio_gpu_async_wait_progress(struct virtio_gpu *g,
                                          uint32 budget_ms)
{
    struct virtio_gpu_queue *q = &g->ctrlq;
    int trace = virtio_gpu_submit_trace_enabled();
    uint64 trace_start = trace ? r_time() : 0;
    struct virtio_gpu_wait_cond cond = {
        .g = g,
        .q = q,
        .sync = 0,
    };
    int intena;
    int ret;

    /* See async_wait_serialize's comment: one progress waiter at a time
     * so the completion re-arm never runs under a sleeping waiter. */
    mutex_lock(&g->async_wait_serialize);
    intena = spin_lock_irqsave(&q->lock);
    if (q->used->idx != q->used_idx) {
        spin_unlock_irqrestore(&q->lock, intena);
        mutex_unlock(&g->async_wait_serialize);
        ret = 1;
        goto out;
    }
    cond.snap_used_idx = q->used->idx;
    cond.snap_retire_seq = g->async_retire_seq;
    /* Re-arm under q->lock: every complete_all on async_wait also holds
     * q->lock, so the reinit can never race a wakeup (and the tq/spinlock
     * inside are only ever initialized once, at driver init). */
    completion_reinit(&g->async_wait);
    spin_unlock_irqrestore(&q->lock, intena);

    ret = virtio_gpu_wait_for_used(g, q, &g->async_wait, &cond, budget_ms);
    mutex_unlock(&g->async_wait_serialize);
out:
    if (trace)
        virtio_gpu_submit_trace_record_async_wait_progress(
            g, r_time() - trace_start);
    return ret;
}

/*
 * Abandon all outstanding async slots after a host timeout.
 *
 * B3 fix: slots whose commands were POSTED to the device are NOT freed —
 * the device may still DMA into their cmd/data/resp buffers and their
 * descriptor blocks.  They transition POSTED -> ABANDONED and the reaper
 * frees them if/when their used elements arrive; until then their memory
 * and descriptors stay quarantined (an unrecovered device permanently
 * leaks them — that is the safe direction).  CLAIMED slots belong to an
 * op_lock'd poster that may be mid-fill/mid-post and are left strictly
 * alone; the sync record and q->used_idx are also untouched (the reaper
 * is total, skipping used elements would orphan slots forever).
 * Lock order: async_reap_serialize -> { g->lock | q->lock } (never
 * nested with each other here).
 */
static void virtio_gpu_async_abort_all(struct virtio_gpu *g)
{
    struct virtio_gpu_queue *q = &g->ctrlq;
    int intena;

    virtio_gpu_count_timeout(g);
    virtio_gpu_count_failure(g);

    /* Abort walks the POSTED population wholesale — it must never run
     * concurrently with a reaper mid-retire on one of the slots. */
    mutex_lock(&g->async_reap_serialize);

    /* First consume everything that actually completed, so only slots
     * the device still owns are abandoned. */
    (void)virtio_gpu_async_reap_completed_reaplocked(g, NULL);

    /*
     * A host stall wedges the shared control queue, but only the contexts that
     * actually had a command in flight have undefined GPU state.  Fail just
     * those contexts instead of every in-use context, so an unrelated client
     * (for example the desktop compositor) is not poisoned when one client's
     * SUBMIT_3D times out.  Async commands that are not context-scoped carry
     * ctx_id 0, which never matches a real context (ids start at 2), so the
     * lookup returns NULL and marks nothing.
     */
    for (int i = 0; i < VIRTIO_GPU_ASYNC_MAX_DEPTH; i++) {
        struct virtio_gpu_async_submit *a = &g->async_ring[i];

        /* POSTED slots are stable here: posters only touch FREE/CLAIMED
         * slots and reapers are excluded by async_reap_serialize. */
        if (__atomic_load_n(&a->state, __ATOMIC_ACQUIRE) !=
            VIRTIO_GPU_ASYNC_SLOT_POSTED)
            continue;
        printf("virtio_gpu: async command 0x%x timed out (ctx=%u)\n",
               a->type, a->ctx_id);
        spin_lock(&g->lock);
        virtio_gpu_print_async_timeout_diag_locked(g, a, r_time());
        virtio_gpu_mark_context_failed_locked(
            g, virtio_gpu_lookup_context_locked(g, a->ctx_id));
        spin_unlock(&g->lock);

        intena = spin_lock_irqsave(&q->lock);
        if (a->state == VIRTIO_GPU_ASYNC_SLOT_POSTED) {
            a->state = VIRTIO_GPU_ASYNC_SLOT_ABANDONED;
            g->async_count--;
            g->async_abandoned++;
            g->async_retire_seq++;
            complete_all(&g->async_wait);
        }
        spin_unlock_irqrestore(&q->lock, intena);
    }
    mutex_unlock(&g->async_reap_serialize);
}

static int virtio_gpu_drain_async_submit_sample(
    struct virtio_gpu *g, int wait,
    struct virtio_gpu_async_drain_sample *sample)
{
    int ret = virtio_gpu_async_reap_completed_sample(g, sample);

    if (!wait)
        return ret;

    uint64 budget_start = get_jiffs();
    uint32 budget_total = virtio_gpu_wait_window_ms();
    while (__atomic_load_n(&g->async_count, __ATOMIC_ACQUIRE) > 0) {
        uint32 remaining =
            virtio_gpu_wait_budget_remaining(budget_start, budget_total);
        if (remaining == 0 ||
            !virtio_gpu_async_wait_progress(g, remaining)) {
            /* Progress is snapshot-based; reap and re-check once before
             * declaring the queue wedged (a concurrent consumer may
             * have retired the last slot around our snapshot). */
            if (virtio_gpu_async_reap_completed_sample(g, sample) != 0)
                ret = -1;
            if (__atomic_load_n(&g->async_count, __ATOMIC_ACQUIRE) == 0)
                break;
            virtio_gpu_async_abort_all(g);
            return -1;
        }
        if (virtio_gpu_async_reap_completed_sample(g, sample) != 0)
            ret = -1;
    }
    return ret;
}

static int virtio_gpu_drain_async_submit(struct virtio_gpu *g, int wait)
{
    return virtio_gpu_drain_async_submit_sample(g, wait, NULL);
}

static int virtio_gpu_drain_async_until_fence(struct virtio_gpu *g,
                                              uint64 fence_id)
{
    int ret = virtio_gpu_async_reap_completed(g);
    uint64 done;

    uint64 budget_start = get_jiffs();
    uint32 budget_total = virtio_gpu_wait_window_ms();
    for (;;) {
        spin_lock(&g->lock);
        done = g->stats.last_fence;
        spin_unlock(&g->lock);
        if (done >= fence_id ||
            __atomic_load_n(&g->async_count, __ATOMIC_ACQUIRE) == 0)
            return ret;
        uint32 remaining =
            virtio_gpu_wait_budget_remaining(budget_start, budget_total);
        if (remaining == 0 ||
            !virtio_gpu_async_wait_progress(g, remaining)) {
            /* Reap and re-check once before declaring the queue wedged
             * (progress is snapshot-based; a concurrent consumer may
             * have signaled the fence around our snapshot). */
            if (virtio_gpu_async_reap_completed(g) != 0)
                ret = -1;
            spin_lock(&g->lock);
            done = g->stats.last_fence;
            spin_unlock(&g->lock);
            if (done >= fence_id ||
                __atomic_load_n(&g->async_count, __ATOMIC_ACQUIRE) == 0)
                return ret;
            virtio_gpu_async_abort_all(g);
            return -1;
        }
        if (virtio_gpu_async_reap_completed(g) != 0)
            ret = -1;
    }
}

/*
 * Room predicate: a new async command may be posted when the active
 * population is below the configured depth AND at least one slot is
 * actually FREE (ABANDONED slots still own their descriptor blocks and
 * shrink the usable ring until the device returns them).  Lock-free
 * hint reads — exactness is enforced by reserve_slot under q->lock.
 */
static int virtio_gpu_async_room_available(struct virtio_gpu *g, int depth)
{
    uint32 count = __atomic_load_n(&g->async_count, __ATOMIC_ACQUIRE);
    uint32 abandoned = __atomic_load_n(&g->async_abandoned,
                                       __ATOMIC_ACQUIRE);

    if (depth <= 0 || g->async_capacity <= 0)
        return 0;
    return (int)count < depth &&
           count + abandoned < (uint32)g->async_capacity;
}

/*
 * Ensure at least one ring slot is free before posting a new async command.
 * When the ring is full this blocks on the next in-flight completion, which is
 * exactly where the host-vsync wait of a previous frame's flush is absorbed
 * while a younger command (e.g. this frame's GL compose) stays in flight.
 */
static int virtio_gpu_async_make_room(struct virtio_gpu *g,
                                      enum virtio_gpu_async_reason reason)
{
    int depth = virtio_gpu_async_depth_for_reason(g, reason);
    int trace = virtio_gpu_submit_trace_collect_enabled();
    uint64 trace_start = trace ? r_time() : 0;
    uint64 trace_count_max = 0;
    uint64 trace_wait_count_max = 0;
    uint64 wait_start = 0;
    uint64 wait_ticks = 0;
    int waited = 0;
    int ret = 0;

    /* Async ring disabled (queue too small, finding #7): fail fast —
     * there is no room to wait for and nothing to abort. */
    if (g->async_capacity <= 0)
        return -1;

    spin_lock(&g->lock);
    virtio_gpu_count_async_make_room_locked(g, reason, 1, 0);
    spin_unlock(&g->lock);
    /* Opportunistically reap anything the host has already finished. */
    (void)virtio_gpu_async_reap_completed(g);
    if (trace)
        trace_count_max = __atomic_load_n(&g->async_count,
                                          __ATOMIC_ACQUIRE);
    uint64 budget_start = get_jiffs();
    uint32 budget_total = virtio_gpu_wait_window_ms();
    while (!virtio_gpu_async_room_available(g, depth)) {
        uint64 count = __atomic_load_n(&g->async_count, __ATOMIC_ACQUIRE);

        if (trace && count > trace_count_max)
            trace_count_max = count;
        if (trace && count > trace_wait_count_max)
            trace_wait_count_max = count;
        if (!waited) {
            wait_start = r_time();
            waited = 1;
            spin_lock(&g->lock);
            virtio_gpu_count_async_make_room_locked(g, reason, 0, 1);
            spin_unlock(&g->lock);
        }
        spin_lock(&g->lock);
        g->stats.async_wait_progress_calls++;
        spin_unlock(&g->lock);
        uint32 remaining =
            virtio_gpu_wait_budget_remaining(budget_start, budget_total);
        if (remaining == 0 ||
            !virtio_gpu_async_wait_progress(g, remaining)) {
            /* Progress is snapshot-based; reap and re-check once before
             * declaring the queue wedged (a concurrent consumer may
             * have freed room around our snapshot). */
            (void)virtio_gpu_async_reap_completed(g);
            if (virtio_gpu_async_room_available(g, depth))
                break;
            virtio_gpu_async_abort_all(g);
            ret = -1;
            goto out;
        }
        /* A retired command may report an error; its slot is still freed. */
        (void)virtio_gpu_async_reap_completed(g);
    }
    if (waited) {
        uint64 us;

        wait_ticks = r_time() - wait_start;
        us = virtio_gpu_ticks_to_us(wait_ticks);

        spin_lock(&g->lock);
        g->stats.async_make_room_wait_ticks += wait_ticks;
        g->stats.async_make_room_last_wait_us = us;
        if (us > g->stats.async_make_room_max_wait_us)
            g->stats.async_make_room_max_wait_us = us;
        spin_unlock(&g->lock);
    }
out:
    if (trace)
        virtio_gpu_submit_trace_record_async_make_room_current(
            g, r_time() - trace_start, wait_ticks, waited,
            (uint64)depth, trace_count_max, trace_wait_count_max);
    return ret;
}

static void virtio_gpu_note_present_copy_drain(struct virtio_gpu *g,
                                               int src_fence_drain,
                                               int blanket_drain,
                                               int no_drain_skip,
                                               int minimal_skip,
                                               int drain_failed,
                                               uint64 drain_ticks)
{
    uint64 us = drain_ticks ? virtio_gpu_ticks_to_us(drain_ticks) : 0;
    int drained = src_fence_drain || blanket_drain;

    spin_lock(&g->lock);
    g->stats.present_copy_calls++;
    if (drained)
        g->stats.present_copy_drain_calls++;
    if (src_fence_drain)
        g->stats.present_copy_src_fence_drains++;
    if (blanket_drain)
        g->stats.present_copy_blanket_drains++;
    if (src_fence_drain && !blanket_drain)
        g->stats.present_copy_src_fence_only_drains++;
    if (!src_fence_drain && blanket_drain)
        g->stats.present_copy_blanket_only_drains++;
    if (src_fence_drain && blanket_drain)
        g->stats.present_copy_src_fence_blanket_drains++;
    if (no_drain_skip)
        g->stats.present_copy_no_drain_skips++;
    if (minimal_skip)
        g->stats.present_copy_minimal_skips++;
    if (drain_failed)
        g->stats.present_copy_drain_failures++;
    g->stats.present_copy_drain_ticks += drain_ticks;
    g->stats.present_copy_drain_last_us = us;
    if (us > g->stats.present_copy_drain_max_us)
        g->stats.present_copy_drain_max_us = us;
    spin_unlock(&g->lock);
}

/*
 * Post a fully-prepared async slot onto the control queue.  The slot owns the
 * descriptor block [desc_base .. desc_base + used).  Must be called with
 * g->op_lock held and a slot reserved by virtio_gpu_async_reserve_slot().
 */
static void virtio_gpu_async_post_locked(struct virtio_gpu *g,
                                         struct virtio_gpu_async_submit *a)
{
    struct virtio_gpu_queue *q = &g->ctrlq;
    uint32 base = a->desc_base;
    uint64 trace_async_count = 0;
    int intena;

    if (virtio_gpu_submit_trace_collect_enabled())
        virtio_gpu_submit_trace_snapshot_slot(g, a, a->ctx_id);

    intena = spin_lock_irqsave(&q->lock);
    memset(&q->desc[base], 0,
           VIRTIO_GPU_ASYNC_DESC_PER_SLOT * sizeof(q->desc[0]));
    q->desc[base].addr = (uint64)a->cmd;
    q->desc[base].len = a->cmd_len;
    q->desc[base].flags = VRING_DESC_F_NEXT;
    if (a->data != NULL && a->data_len != 0) {
        q->desc[base].next = base + 1;
        q->desc[base + 1].addr = (uint64)a->data;
        q->desc[base + 1].len = a->data_len;
        q->desc[base + 1].flags = VRING_DESC_F_NEXT;
        q->desc[base + 1].next = base + 2;
        q->desc[base + 2].addr = (uint64)a->resp;
        q->desc[base + 2].len = a->resp_len;
        q->desc[base + 2].flags = VRING_DESC_F_WRITE;
        q->desc[base + 2].next = 0;
    } else {
        q->desc[base].next = base + 1;
        q->desc[base + 1].addr = (uint64)a->resp;
        q->desc[base + 1].len = a->resp_len;
        q->desc[base + 1].flags = VRING_DESC_F_WRITE;
        q->desc[base + 1].next = 0;
    }

    /*
     * B3: CLAIMED -> POSTED in the same q->lock critical section that
     * publishes the descriptors.  From the avail->idx increment on, the
     * device owns this slot's memory; only the reaper may free it.
     */
    if (a->state != VIRTIO_GPU_ASYNC_SLOT_CLAIMED)
        printf("virtio_gpu: async post on slot in state %d\n", a->state);
    a->state = VIRTIO_GPU_ASYNC_SLOT_POSTED;
    q->avail->ring[q->avail->idx % q->size] = (uint16)base;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->avail->idx++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    virtio_gpu_notify(g, 0);
    spin_unlock_irqrestore(&q->lock, intena);
    spin_lock(&g->lock);
    virtio_gpu_count_async_posted_locked(g, a->type);
    trace_async_count = __atomic_load_n(&g->async_count, __ATOMIC_ACQUIRE);
    spin_unlock(&g->lock);
    virtio_gpu_submit_trace_record_async_post(g, a, trace_async_count);
}

/*
 * Reserve a free ring slot, assign its private descriptor block and return it.
 * Caller must have ensured room via virtio_gpu_async_make_room().
 */
static struct virtio_gpu_async_submit *
virtio_gpu_async_reserve_slot(struct virtio_gpu *g)
{
    struct virtio_gpu_queue *q = &g->ctrlq;
    struct virtio_gpu_async_submit *found = NULL;
    int intena;

    /* Claim + count under q->lock so reapers' decrements and this
     * increment never race (see async_reap_serialize's comment).  The
     * ACQUIRE load pairs with submit_free's RELEASE store of FREE so
     * the body wipe is fully visible before the poster's fresh field
     * writes. */
    intena = spin_lock_irqsave(&q->lock);
    /* Bounded by async_capacity: slots beyond the negotiated descriptor
     * budget must never be claimed (their desc_base would be out of
     * range for the device's table). */
    for (int i = 0; i < g->async_capacity; i++) {
        struct virtio_gpu_async_submit *a = &g->async_ring[i];

        if (__atomic_load_n(&a->state, __ATOMIC_ACQUIRE) !=
            VIRTIO_GPU_ASYNC_SLOT_FREE)
            continue;
        memset(a, 0, sizeof(*a));
        a->state = VIRTIO_GPU_ASYNC_SLOT_CLAIMED;
        a->desc_base = VIRTIO_GPU_ASYNC_DESC_BASE +
                       i * VIRTIO_GPU_ASYNC_DESC_PER_SLOT;
        g->async_count++;
        found = a;
        break;
    }
    spin_unlock_irqrestore(&q->lock, intena);
    return found;
}

/*
 * Non-blocking room probe: reap completed submits, then report whether a
 * ring slot is free for @reason.  0 = room available, -EAGAIN = full.
 * Unlike virtio_gpu_async_make_room() this never waits on host
 * retirement, so callers holding the op lock can back off and wait with
 * the lock RELEASED (the N1/M7 fix: a stalled GL submit must not
 * serialize page-flip presents behind it).
 */
static int virtio_gpu_async_room_nowait(struct virtio_gpu *g,
                                        enum virtio_gpu_async_reason reason)
{
    int depth = virtio_gpu_async_depth_for_reason(g, reason);

    (void)virtio_gpu_async_reap_completed(g);
    if (!virtio_gpu_async_room_available(g, depth))
        return -EAGAIN;
    return 0;
}

static int virtio_gpu_async_post_prepared(
    struct virtio_gpu *g, struct virtio_gpu_async_submit *prep,
    enum virtio_gpu_async_reason reason, int nowait)
{
    struct virtio_gpu_async_submit *a;

    if (nowait) {
        int room = virtio_gpu_async_room_nowait(g, reason);

        if (room != 0)
            return room;
    } else if (virtio_gpu_async_make_room(g, reason) != 0)
        return -1;

    a = virtio_gpu_async_reserve_slot(g);
    if (a == NULL) {
        /*
         * The room probe is a lock-free hint; losing the slot by the
         * time we claim under q->lock is a retryable condition in
         * nowait mode (the unlocked-wait loop drops op_lock and waits
         * for room), not a device error — do not conflate it with -1.
         * In wait mode make_room guarantees a FREE slot (abandoned-
         * aware) and no other poster can hold op_lock, so NULL here is
         * a genuine anomaly.
         */
        if (nowait)
            return -EAGAIN;
        printf("virtio_gpu: async reserve failed after make_room\n");
        return -1;
    }
    a->ctx_id = prep->ctx_id;
    a->fence_id = prep->fence_id;
    a->type = prep->type;
    a->expected = prep->expected;
    a->cmd = prep->cmd;
    a->cmd_len = prep->cmd_len;
    a->data = prep->data;
    a->data_len = prep->data_len;
    a->data_order = prep->data_order;
    a->resp = prep->resp;
    a->resp_len = prep->resp_len;
    a->posted_ticks = r_time();
    a->trace_shape = prep->trace_shape;
    prep->cmd = NULL;
    prep->data = NULL;
    prep->resp = NULL;

    virtio_gpu_async_post_locked(g, a);
    return 0;
}

/* Any async commands still in flight on the control queue?  (ABANDONED
 * slots are excluded: they cannot be drained, only returned by the
 * device.) */
static int virtio_gpu_async_pending(struct virtio_gpu *g)
{
    return __atomic_load_n(&g->async_count, __ATOMIC_ACQUIRE) > 0;
}

/*
 * Fence id of the most recently submitted outstanding async command, or 0 if
 * none are in flight.  Used to decide whether an outstanding submit needs to be
 * drained before a dependent operation (e.g. scanout readback).  Scans under
 * q->lock so slot state/fence pairs are read consistently against concurrent
 * reapers.
 */
static uint64 virtio_gpu_async_newest_fence(struct virtio_gpu *g)
{
    struct virtio_gpu_queue *q = &g->ctrlq;
    uint64 newest = 0;
    int intena;

    intena = spin_lock_irqsave(&q->lock);
    for (int i = 0; i < VIRTIO_GPU_ASYNC_MAX_DEPTH; i++) {
        struct virtio_gpu_async_submit *a = &g->async_ring[i];

        if ((a->state == VIRTIO_GPU_ASYNC_SLOT_CLAIMED ||
             a->state == VIRTIO_GPU_ASYNC_SLOT_POSTED) &&
            a->fence_id > newest)
            newest = a->fence_id;
    }
    spin_unlock_irqrestore(&q->lock, intena);
    return newest;
}

#include "virtio_gpu_resource.c"
#include "virtio_gpu_3d.c"
#include "virtio_gpu_user.c"
#include "virtio_gpu_scanout.c"
#include "virtio_gpu_present.c"
#else

void virtio_gpu_init(void) {}
void virtio_gpu_get_fb_stats(struct fb_gpu_stats *stats) { (void)stats; }
int virtio_gpu_has_virgl(void) { return 0; }
int virtio_gpu_has_context_init(void) { return 0; }
int virtio_gpu_has_resource_blob(void) { return 0; }
int virtio_gpu_has_host_visible(void) { return 0; }
int virtio_gpu_probe_scanout(uint32 *width, uint32 *height)
{
    if (width != NULL)
        *width = 0;
    if (height != NULL)
        *height = 0;
    return -ENODEV;
}
int virtio_gpu_probe_edid_mode(uint32 *width, uint32 *height,
                               uint32 *refresh_millihz)
{
    if (width != NULL)
        *width = 0;
    if (height != NULL)
        *height = 0;
    if (refresh_millihz != NULL)
        *refresh_millihz = 0;
    return -ENODEV;
}
int virtio_gpu_resize_scanout(uint32 width, uint32 height)
{
    (void)width;
    (void)height;
    return -ENODEV;
}
int virtio_gpu_copy_resource_to_scanout(uint32 src_resource_id,
                                        uint32 src_x, uint32 src_y,
                                        uint32 dst_x, uint32 dst_y,
                                        uint32 w, uint32 h)
{
    (void)src_resource_id;
    (void)src_x;
    (void)src_y;
    (void)dst_x;
    (void)dst_y;
    (void)w;
    (void)h;
    return -ENODEV;
}
int virtio_gpu_bind_resource_scanout(uint32 resource_id, uint32 x, uint32 y,
                                     uint32 w, uint32 h)
{
    (void)resource_id;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    return -ENODEV;
}
int virtio_gpu_page_flip_resource(uint32 resource_id, uint32 w, uint32 h,
                                  uint32 *flags_out)
{
    if (flags_out != NULL)
        *flags_out = 0;
    (void)resource_id;
    (void)w;
    (void)h;
    return -ENODEV;
}
int virtio_gpu_user_move_injected_cursor(uint16 x_abs, uint16 y_abs,
                                         int visible)
{
    (void)x_abs;
    (void)y_abs;
    (void)visible;
    return -ENODEV;
}
int virtio_gpu_read_current_scanout(uint32 x, uint32 y, uint32 w, uint32 h,
                                    void *dst, uint32 dst_pitch,
                                    uint32 *screen_width,
                                    uint32 *screen_height,
                                    uint32 *screen_pitch)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)dst;
    (void)dst_pitch;
    if (screen_width != NULL)
        *screen_width = 0;
    if (screen_height != NULL)
        *screen_height = 0;
    if (screen_pitch != NULL)
        *screen_pitch = 0;
    return -ENODEV;
}
int virtio_gpu_user_context_create(uint64 owner_id, pid_t owner_tgid,
                                   uint32 capset_id, uint32 context_init,
                                   const char *name, uint32 *ctx_id,
                                   uint32 *actual_capset_id)
{
    (void)owner_id;
    (void)owner_tgid;
    (void)capset_id;
    (void)context_init;
    (void)name;
    (void)ctx_id;
    (void)actual_capset_id;
    return -ENODEV;
}
int virtio_gpu_user_context_destroy(uint64 owner_id, pid_t owner_tgid,
                                    uint32 ctx_id)
{
    (void)owner_id;
    (void)owner_tgid;
    (void)ctx_id;
    return -ENODEV;
}
int virtio_gpu_user_submit(uint64 owner_id, pid_t owner_tgid, uint32 ctx_id,
                           uint32 flags, const uint32 *cmds,
                           uint32 nr_dwords, const uint32 *resources,
                           uint32 resource_count, uint64 *fence,
                           uint64 *signaled, uint32 *first_submit,
                           const struct virtio_gpu_submit_trace_shape *shape)
{
    (void)owner_id;
    (void)owner_tgid;
    (void)ctx_id;
    (void)flags;
    (void)cmds;
    (void)nr_dwords;
    (void)resources;
    (void)resource_count;
    (void)fence;
    (void)signaled;
    (void)first_submit;
    (void)shape;
    return -ENODEV;
}
int virtio_gpu_user_fence(uint64 wait_for, int wait, uint64 *signaled)
{
    (void)wait_for;
    (void)wait;
    (void)signaled;
    return -ENODEV;
}
int virtio_gpu_user_capset_ids(uint64 *ids)
{
    (void)ids;
    return -ENODEV;
}
int virtio_gpu_user_creatable_capset_ids(uint64 *ids)
{
    (void)ids;
    return -ENODEV;
}
int virtio_gpu_user_capset_query_only(uint32 capset_id)
{
    (void)capset_id;
    return 0;
}
int virtio_gpu_user_resource_blob_supported(void) { return 0; }
int virtio_gpu_user_get_caps_for(uint32 requested_capset_id,
                                 uint32 requested_capset_version,
                                 void *buf, uint32 buf_size,
                                 uint32 *capset_id, uint32 *capset_version,
                                 uint32 *capset_size)
{
    (void)requested_capset_id;
    (void)requested_capset_version;
    (void)buf;
    (void)buf_size;
    (void)capset_id;
    (void)capset_version;
    (void)capset_size;
    return -ENODEV;
}
int virtio_gpu_user_get_caps(void *buf, uint32 buf_size, uint32 *capset_id,
                             uint32 *capset_version, uint32 *capset_size)
{
    (void)buf;
    (void)buf_size;
    (void)capset_id;
    (void)capset_version;
    (void)capset_size;
    return -ENODEV;
}
int virtio_gpu_user_resource_create(uint64 owner_id, pid_t owner_tgid,
                                    struct fb_gpu_virgl_resource_create *req)
{
    (void)owner_id;
    (void)owner_tgid;
    (void)req;
    return -ENODEV;
}
int virtio_gpu_user_resource_create_blob(uint64 owner_id, pid_t owner_tgid,
                                         struct fb_gpu_virgl_blob_create *req)
{
    (void)owner_id;
    (void)owner_tgid;
    (void)req;
    return -ENODEV;
}
int virtio_gpu_user_resource_destroy(uint64 owner_id, pid_t owner_tgid,
                                     uint32 resource_id)
{
    (void)owner_id;
    (void)owner_tgid;
    (void)resource_id;
    return -ENODEV;
}
int virtio_gpu_user_resource_export_pages(uint64 owner_id, pid_t owner_tgid,
                                          uint32 resource_id, uint32 *width,
                                          uint32 *height, uint32 *pitch,
                                          uint64 *size, page_t ***pages_out,
                                          uint32 *npages_out)
{
    (void)owner_id;
    (void)owner_tgid;
    (void)resource_id;
    (void)width;
    (void)height;
    (void)pitch;
    (void)size;
    (void)pages_out;
    (void)npages_out;
    return -ENODEV;
}
void virtio_gpu_user_resource_export_put(uint32 resource_id)
{
    (void)resource_id;
}
int virtio_gpu_user_resource_info(uint64 owner_id, pid_t owner_tgid,
                                  uint32 resource_id, uint32 *width,
                                  uint32 *height, uint32 *format,
                                  uint64 *size)
{
    (void)owner_id;
    (void)owner_tgid;
    (void)resource_id;
    (void)width;
    (void)height;
    (void)format;
    (void)size;
    return -ENODEV;
}
int virtio_gpu_user_resource_blob_mem(uint64 owner_id, pid_t owner_tgid,
                                      uint32 resource_id,
                                      uint32 *blob_mem)
{
    (void)owner_id;
    (void)owner_tgid;
    (void)resource_id;
    (void)blob_mem;
    return -ENODEV;
}
int virtio_gpu_user_resource_map_offset(uint64 owner_id, pid_t owner_tgid,
                                        uint32 resource_id, uint64 *offset)
{
    (void)owner_id;
    (void)owner_tgid;
    (void)resource_id;
    if (offset)
        *offset = 0;
    return -ENODEV;
}
int virtio_gpu_user_host_visible_mmap(uint64 owner_id, pid_t owner_tgid,
                                      uint64 offset, uint64 size)
{
    (void)owner_id;
    (void)owner_tgid;
    (void)offset;
    (void)size;
    return -ENODEV;
}
void *virtio_gpu_user_host_visible_page(uint64 owner_id, pid_t owner_tgid,
                                        uint64 offset)
{
    (void)owner_id;
    (void)owner_tgid;
    (void)offset;
    return NULL;
}
int virtio_gpu_user_resource_last_submit_fence(uint64 owner_id,
                                               pid_t owner_tgid,
                                               uint32 resource_id,
                                               uint64 *fence)
{
    (void)owner_id;
    (void)owner_tgid;
    (void)resource_id;
    if (fence)
        *fence = 0;
    return -ENODEV;
}
void *virtio_gpu_user_resource_page(uint64 owner_id, pid_t owner_tgid,
                                    uint32 resource_id, uint64 page_index)
{
    (void)owner_id;
    (void)owner_tgid;
    (void)resource_id;
    (void)page_index;
    return NULL;
}
void virtio_gpu_user_destroy_owner(pid_t owner_tgid)
{
    (void)owner_tgid;
}
void virtio_gpu_user_destroy_render_owner(uint64 owner_id)
{
    (void)owner_id;
}
int virtio_gpu_user_transfer(uint64 owner_id, pid_t owner_tgid,
                             struct fb_gpu_virgl_transfer *req,
                             int from_host)
{
    (void)owner_id;
    (void)owner_tgid;
    (void)req;
    (void)from_host;
    return -ENODEV;
}

#endif
