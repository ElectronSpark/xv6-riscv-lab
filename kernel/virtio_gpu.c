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
    completion_t *pending_completion;
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
    uint32 id;
    uint32 capset_id;
    uint64 owner_id;
    pid_t owner_tgid;
};

struct virtio_gpu_capset {
    int valid;
    int creatable;
    uint32 id;
    uint32 version;
    uint32 size;
};

struct virtio_gpu_async_submit {
    int pending;
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
#define VIRTIO_GPU_ASYNC_MAX_DEPTH 32
#define VIRTIO_GPU_ASYNC_DESC_PER_SLOT 4
#define VIRTIO_GPU_ASYNC_DESC_BASE 8

struct virtio_gpu {
    int initialized;
    struct virtio_pci_state pci;
    volatile struct virtio_gpu_config *config;
    struct virtio_gpu_queue ctrlq;
    spinlock_t lock;
    mutex_t op_lock;
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
    uint64 next_fence_id;
    struct virtio_gpu_async_submit async_ring[VIRTIO_GPU_ASYNC_MAX_DEPTH];
    uint32 async_count;
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

static int virtio_gpu_complete_pending_locked(struct virtio_gpu_queue *q)
{
    if (q->pending_completion != NULL && q->used->idx != q->used_idx) {
        complete_all(q->pending_completion);
        return 1;
    }
    return 0;
}

static int virtio_gpu_used_advanced(struct virtio_gpu_queue *q)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return q->used->idx != q->used_idx;
}

static int virtio_gpu_wait_for_used(struct virtio_gpu *g,
                                    struct virtio_gpu_queue *q,
                                    completion_t *done)
{
    uint64 waited_ms = 0;
    uint32 wait_limit_ms = virtio_gpu_cmdline_uint(
        "virtio_gpu_irq_wait_ms", VIRTIO_GPU_IRQ_WAIT_MS,
        VIRTIO_GPU_IRQ_WAIT_MS_MAX);
    int poll_fallback_counted = 0;

    /*
     * QEMU often completes simple scanout and virgl control commands before
     * the interrupt path has a chance to schedule the waiter.  Poll briefly
     * first so compositor presents and WebKit submits do not sleep for a full
     * timer tick on every frame.
     */
    for (int i = 0; i < VIRTIO_GPU_FAST_POLL_LIMIT; i++) {
        if (virtio_gpu_used_advanced(q))
            return 1;
    }

    /*
     * Keep the historical five-second failure deadline unless the launch asks
     * for a longer host-warmup window.  Poll the used ring between short sleeps
     * so a missed or coalesced interrupt costs about a millisecond, not a
     * multi-second UI freeze.
     */
    while (waited_ms < wait_limit_ms) {
        if (wait_for_completion_timeout(done,
                                        VIRTIO_GPU_IRQ_WAIT_SLICE_MS) != 0)
            return 1;
        waited_ms += VIRTIO_GPU_IRQ_WAIT_SLICE_MS;
        if (virtio_gpu_used_advanced(q)) {
            if (!poll_fallback_counted) {
                virtio_gpu_count_poll_fallback(g);
                poll_fallback_counted = 1;
            }
            return 1;
        }
    }

    if (!poll_fallback_counted)
        virtio_gpu_count_poll_fallback(g);
    for (int i = 0; i < VIRTIO_GPU_POLL_LIMIT; i++) {
        if (virtio_gpu_used_advanced(q))
            return 1;
    }
    return 0;
}

static void virtio_gpu_intr(int irq, void *data, device_t *dev)
{
    struct virtio_gpu *g = (struct virtio_gpu *)data;
    struct virtio_gpu_queue *q;

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
    if (virtio_gpu_complete_pending_locked(q))
        virtio_gpu_count_irq_completion(g);
    spin_unlock(&q->lock);
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
    int intena = spin_lock_irqsave(&q->lock);

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
    q->pending_completion = &done;
    virtio_gpu_notify(g, 0);
    spin_unlock_irqrestore(&q->lock, intena);

    start = r_time();
    (void)virtio_gpu_wait_for_used(g, q, &done);
    if (command_ticks_out != NULL)
        *command_ticks_out = r_time() - start;

    intena = spin_lock_irqsave(&q->lock);

    if (q->used->idx == q->used_idx) {
        uint32 ctx_id = ((struct virtio_gpu_ctrl_hdr *)cmd)->ctx_id;

        q->pending_completion = NULL;
        spin_unlock_irqrestore(&q->lock, intena);
        virtio_gpu_count_timeout(g);
        virtio_gpu_count_failure(g);
        spin_lock(&g->lock);
        virtio_gpu_mark_context_failed_locked(
            g, virtio_gpu_lookup_context_locked(g, ctx_id));
        spin_unlock(&g->lock);
        printf("virtio_gpu: command 0x%x timed out (ctx=%u)\n", type, ctx_id);
        return -1;
    }

    q->used_idx = q->used->idx;
    q->pending_completion = NULL;
    spin_unlock_irqrestore(&q->lock, intena);

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

    virtio_gpu_count_command(g, type);
    return 0;
}

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
    int ret = 0;

    completion_init(&done);
    int intena = spin_lock_irqsave(&q->lock);

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
    q->pending_completion = &done;
    virtio_gpu_notify(g, 0);
    spin_unlock_irqrestore(&q->lock, intena);

    for (;;) {
        uint32 id = 0;
        struct virtio_gpu_async_submit *a;

        if (!virtio_gpu_wait_for_used(g, q, &done)) {
            uint32 ctx_id = ((struct virtio_gpu_ctrl_hdr *)cmd)->ctx_id;

            intena = spin_lock_irqsave(&q->lock);
            q->pending_completion = NULL;
            spin_unlock_irqrestore(&q->lock, intena);
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

        for (;;) {
            int have = 0;

            intena = spin_lock_irqsave(&q->lock);
            if (q->used->idx != q->used_idx) {
                id = q->used->ring[q->used_idx % q->size].id;
                q->used_idx = (uint16)(q->used_idx + 1);
                have = 1;
            }
            spin_unlock_irqrestore(&q->lock, intena);
            if (!have)
                break;

            if (id == 0) {
                intena = spin_lock_irqsave(&q->lock);
                q->pending_completion = NULL;
                spin_unlock_irqrestore(&q->lock, intena);
                if (resp_hdr->type != expected) {
                    virtio_gpu_count_failure(g);
                    printf("virtio_gpu: mixed command 0x%x response=0x%x expected=0x%x\n",
                           type, resp_hdr->type, expected);
                    return -1;
                }
                virtio_gpu_count_command(g, type);
                return ret;
            }

            a = virtio_gpu_async_find_slot_by_desc(g, id);
            if (a == NULL) {
                printf("virtio_gpu: mixed reap unknown desc id=%u\n", id);
                continue;
            }
            virtio_gpu_drain_sample_count(drain_sample, a->type);
            (void)virtio_gpu_async_validate_retire(g, a);
            g->async_count--;
            spin_lock(&g->lock);
            g->stats.async_retired++;
            spin_unlock(&g->lock);
        }

        completion_init(&done);
        intena = spin_lock_irqsave(&q->lock);
        if (q->pending_completion != NULL)
            q->pending_completion = &done;
        spin_unlock_irqrestore(&q->lock, intena);
    }
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
    memset(a, 0, sizeof(*a));
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
        depth = (int)virtio_gpu_cmdline_uint(
            "virtio_gpu_async_submit_depth", 1, VIRTIO_GPU_ASYNC_MAX_DEPTH);
        if (depth < 1)
            depth = 1;
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
    virtio_gpu_async_submit_free(a);
    return ret;
}

/* Map a completed descriptor-chain head id back to its owning ring slot. */
static struct virtio_gpu_async_submit *
virtio_gpu_async_find_slot_by_desc(struct virtio_gpu *g, uint32 id)
{
    for (int i = 0; i < VIRTIO_GPU_ASYNC_MAX_DEPTH; i++) {
        if (g->async_ring[i].pending && g->async_ring[i].desc_base == id)
            return &g->async_ring[i];
    }
    return NULL;
}

/*
 * Reap every async command the host has already completed.  Retirement follows
 * the host's actual completion order (read from the used ring) rather than
 * submission order, because QEMU's virgl renderer may finish a fenced SUBMIT_3D
 * after a later unfenced RESOURCE_FLUSH.  Must be called with g->op_lock held.
 * Returns -1 if any retired command reported an unexpected response.
 */
static int virtio_gpu_async_reap_completed_sample(
    struct virtio_gpu *g, struct virtio_gpu_async_drain_sample *sample)
{
    struct virtio_gpu_queue *q = &g->ctrlq;
    int ret = 0;

    for (;;) {
        struct virtio_gpu_async_submit *a;
        uint32 id = 0;
        int have = 0;
        int intena;

        intena = spin_lock_irqsave(&q->lock);
        if (q->used->idx != q->used_idx) {
            id = q->used->ring[q->used_idx % q->size].id;
            q->used_idx = (uint16)(q->used_idx + 1);
            have = 1;
        }
        spin_unlock_irqrestore(&q->lock, intena);
        if (!have)
            break;

        a = virtio_gpu_async_find_slot_by_desc(g, id);
        if (a == NULL) {
            printf("virtio_gpu: async reap unknown desc id=%u\n", id);
            continue;
        }
        virtio_gpu_drain_sample_count(sample, a->type);
        if (virtio_gpu_async_validate_retire(g, a) != 0)
            ret = -1;
        g->async_count--;
        spin_lock(&g->lock);
        g->stats.async_retired++;
        spin_unlock(&g->lock);
    }
    return ret;
}

static int virtio_gpu_async_reap_completed(struct virtio_gpu *g)
{
    return virtio_gpu_async_reap_completed_sample(g, NULL);
}

/* Block until the control queue retires at least one more buffer. */
static int virtio_gpu_async_wait_progress(struct virtio_gpu *g)
{
    struct virtio_gpu_queue *q = &g->ctrlq;
    int intena;

    intena = spin_lock_irqsave(&q->lock);
    if (q->used->idx != q->used_idx) {
        spin_unlock_irqrestore(&q->lock, intena);
        return 1;
    }
    completion_init(&g->async_wait);
    q->pending_completion = &g->async_wait;
    spin_unlock_irqrestore(&q->lock, intena);

    return virtio_gpu_wait_for_used(g, q, &g->async_wait);
}

/* Tear down all outstanding async slots after a host timeout. */
static void virtio_gpu_async_abort_all(struct virtio_gpu *g)
{
    struct virtio_gpu_queue *q = &g->ctrlq;
    int intena;

    virtio_gpu_count_timeout(g);
    virtio_gpu_count_failure(g);

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
        if (g->async_ring[i].pending) {
            printf("virtio_gpu: async command 0x%x timed out (ctx=%u)\n",
                   g->async_ring[i].type, g->async_ring[i].ctx_id);
            spin_lock(&g->lock);
            virtio_gpu_print_async_timeout_diag_locked(
                g, &g->async_ring[i], r_time());
            virtio_gpu_mark_context_failed_locked(
                g, virtio_gpu_lookup_context_locked(
                       g, g->async_ring[i].ctx_id));
            spin_unlock(&g->lock);
            virtio_gpu_async_submit_free(&g->async_ring[i]);
        }
    }
    g->async_count = 0;

    intena = spin_lock_irqsave(&q->lock);
    q->used_idx = q->used->idx;
    q->pending_completion = NULL;
    spin_unlock_irqrestore(&q->lock, intena);
}

static int virtio_gpu_drain_async_submit_sample(
    struct virtio_gpu *g, int wait,
    struct virtio_gpu_async_drain_sample *sample)
{
    int ret = virtio_gpu_async_reap_completed_sample(g, sample);

    if (!wait)
        return ret;

    while (g->async_count > 0) {
        if (!virtio_gpu_async_wait_progress(g)) {
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

    for (;;) {
        spin_lock(&g->lock);
        done = g->stats.last_fence;
        spin_unlock(&g->lock);
        if (done >= fence_id || g->async_count == 0)
            return ret;
        if (!virtio_gpu_async_wait_progress(g)) {
            virtio_gpu_async_abort_all(g);
            return -1;
        }
        if (virtio_gpu_async_reap_completed(g) != 0)
            ret = -1;
    }
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
    uint64 wait_start = 0;
    int waited = 0;

    spin_lock(&g->lock);
    virtio_gpu_count_async_make_room_locked(g, reason, 1, 0);
    spin_unlock(&g->lock);
    /* Opportunistically reap anything the host has already finished. */
    (void)virtio_gpu_async_reap_completed(g);
    while ((int)g->async_count >= depth) {
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
        if (!virtio_gpu_async_wait_progress(g)) {
            virtio_gpu_async_abort_all(g);
            return -1;
        }
        /* A retired command may report an error; its slot is still freed. */
        (void)virtio_gpu_async_reap_completed(g);
    }
    if (waited) {
        uint64 ticks = r_time() - wait_start;
        uint64 us = virtio_gpu_ticks_to_us(ticks);

        spin_lock(&g->lock);
        g->stats.async_make_room_wait_ticks += ticks;
        g->stats.async_make_room_last_wait_us = us;
        if (us > g->stats.async_make_room_max_wait_us)
            g->stats.async_make_room_max_wait_us = us;
        spin_unlock(&g->lock);
    }
    return 0;
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
    int intena;

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

    q->avail->ring[q->avail->idx % q->size] = (uint16)base;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->avail->idx++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->pending_completion = &g->async_wait;
    virtio_gpu_notify(g, 0);
    spin_unlock_irqrestore(&q->lock, intena);
    spin_lock(&g->lock);
    virtio_gpu_count_async_posted_locked(g, a->type);
    spin_unlock(&g->lock);
}

/*
 * Reserve a free ring slot, assign its private descriptor block and return it.
 * Caller must have ensured room via virtio_gpu_async_make_room().
 */
static struct virtio_gpu_async_submit *
virtio_gpu_async_reserve_slot(struct virtio_gpu *g)
{
    for (int i = 0; i < VIRTIO_GPU_ASYNC_MAX_DEPTH; i++) {
        struct virtio_gpu_async_submit *a = &g->async_ring[i];

        if (a->pending)
            continue;
        memset(a, 0, sizeof(*a));
        a->pending = 1;
        a->desc_base = VIRTIO_GPU_ASYNC_DESC_BASE +
                       i * VIRTIO_GPU_ASYNC_DESC_PER_SLOT;
        g->async_count++;
        return a;
    }
    return NULL;
}

static int virtio_gpu_async_post_prepared(
    struct virtio_gpu *g, struct virtio_gpu_async_submit *prep,
    enum virtio_gpu_async_reason reason)
{
    struct virtio_gpu_async_submit *a;

    if (virtio_gpu_async_make_room(g, reason) != 0)
        return -1;

    a = virtio_gpu_async_reserve_slot(g);
    if (a == NULL)
        return -1;
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
    prep->cmd = NULL;
    prep->data = NULL;
    prep->resp = NULL;

    virtio_gpu_async_post_locked(g, a);
    return 0;
}

/* Any async commands still in flight on the control queue? */
static int virtio_gpu_async_pending(struct virtio_gpu *g)
{
    return g->async_count > 0;
}

/*
 * Fence id of the most recently submitted outstanding async command, or 0 if
 * none are in flight.  Used to decide whether an outstanding submit needs to be
 * drained before a dependent operation (e.g. scanout readback).
 */
static uint64 virtio_gpu_async_newest_fence(struct virtio_gpu *g)
{
    uint64 newest = 0;

    for (int i = 0; i < VIRTIO_GPU_ASYNC_MAX_DEPTH; i++) {
        if (g->async_ring[i].pending &&
            g->async_ring[i].fence_id > newest)
            newest = g->async_ring[i].fence_id;
    }
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
                                   const char *name, uint32 *ctx_id)
{
    (void)owner_id;
    (void)owner_tgid;
    (void)capset_id;
    (void)context_init;
    (void)name;
    (void)ctx_id;
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
                           uint64 *signaled)
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
